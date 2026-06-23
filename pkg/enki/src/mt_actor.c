#include "enki/mt_actor.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "axsys/assume.h"
#include "plan/build.h"
#include "plan/nat.h"

/*
 * Multithreaded actor executor.  A pool of worker OS threads share one
 * run queue; the expensive part of an actor's life — pl_thread_run, the
 * reduction work on its private heap — runs lock-free on whichever worker
 * dequeued it.  Everything else (run queue, per-actor mailbox / status /
 * handle table, spawn, the all-actors list) is serialized under a single
 * scheduler mutex `smtx`; the shared store carries its own pin/load lock
 * (pl_store_set_concurrent), so payload pinning during service is safe
 * against parallel pins from op_pin running inside other workers.
 *
 * Invariants
 *   - An actor is RUNNING on at most one worker at a time, and while
 *     RUNNING only that worker touches its heap / thread; other workers
 *     touch only its mailbox and status, and only under smtx.
 *   - er_recv_ready builds the response in the *receiver's* heap.  Its two
 *     callers both have exclusive access: the receiver servicing its own
 *     Recv (it is RUNNING on this worker), or a sender delivering to a
 *     BLOCKED receiver (parked on no worker) — both under smtx.
 *   - `active` = #actors that are RUNNABLE (queued) or RUNNING.  It falls
 *     to 0 exactly when no actor can make progress or wake another; the
 *     worker that drives it to 0 declares quiescence and wakes the pool
 *     to exit.  RUNNING<->RUNNABLE transitions never change it.
 *
 * Semantics mirror actor.c / the reaver reference; see mt_actor.h.  The
 * cost is determinism: interleavings are OS-dependent, so there is no
 * record/replay or adopt/drive here.
 */

#define EM_DEFAULT_QUANTUM    4096
#define EM_DEFAULT_HEAP_CELLS 8192

typedef enum {
  EM_UNSTARTED = 0,
  EM_RUNNABLE, /* in the run queue */
  EM_RUNNING,  /* dequeued, executing on a worker */
  EM_BLOCKED,  /* parked on Recv against an empty mailbox */
  EM_HALTED,   /* (fn 0) reached normal form */
  EM_CRASHED,  /* uncaught exception or invalid effect */
} em_status;

typedef struct em_msg {
  pl_val payload; /* nat63 or store-resident — terminal for every GC */
  struct em_msg* next;
  uint32_t ncaps;
  em_actor* caps[]; /* actor refs, translated at send */
} em_msg;

struct em_actor {
  em_scheduler* sys;
  uint64_t id;
  pl_heap* heap;
  pl_thread* t;
  em_status status;  /* smtx */
  em_msg* mbox_head; /* smtx; arrival-order FIFO */
  em_msg* mbox_tail;
  em_actor** handle_v; /* smtx; dense handle table; NULL = closed; [0]=self */
  size_t handle_n;
  size_t handle_cap;
  em_actor* qnext;    /* smtx; run-queue link */
  em_actor* all_next; /* smtx; creation-order list */
};

struct em_scheduler {
  pl_store* store;
  er_config cfg;
  int nworkers;
  pthread_t* workers;

  pthread_mutex_t smtx;
  pthread_cond_t scnd;
  em_actor* qhead; /* run queue */
  em_actor* qtail;
  int64_t active; /* RUNNABLE + RUNNING actors */
  bool quiescing; /* set when active hits 0; tells workers to exit */

  em_actor* all_head; /* every actor, creation order */
  em_actor* all_tail;
  uint64_t next_id;
};

/* ── Construction ──────────────────────────────────────────────────────── */

em_scheduler* em_scheduler_new(pl_store* store, er_config cfg, int nworkers) {
  ax_assume(store != NULL, "em_scheduler_new: store required");
  em_scheduler* sys = calloc(1, sizeof(*sys));
  ax_assume(sys != NULL, "oom");
  sys->store = store;
  sys->cfg = cfg;
  if (sys->cfg.quantum == 0)
    sys->cfg.quantum = EM_DEFAULT_QUANTUM;
  if (sys->cfg.heap_cells == 0)
    sys->cfg.heap_cells = EM_DEFAULT_HEAP_CELLS;
  ax_assume(sys->cfg.quantum >= 2, "em_scheduler_new: quantum must be >= 2");
  if (nworkers <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    nworkers = (n > 0) ? (int)n : 1;
  }
  sys->nworkers = nworkers;
  sys->workers = calloc((size_t)nworkers, sizeof(pthread_t));
  ax_assume(sys->workers != NULL, "oom");
  pthread_mutex_init(&sys->smtx, NULL);
  pthread_cond_init(&sys->scnd, NULL);
  pl_store_set_concurrent(store, true);
  return sys;
}

static em_actor* em_register_locked(em_scheduler* sys) {
  em_actor* a = calloc(1, sizeof(*a));
  ax_assume(a != NULL, "oom");
  a->sys = sys;
  a->id = sys->next_id++;
  a->heap = pl_heap_new(sys->cfg.heap_cells, sys->store);
  a->t = pl_thread_new(a->heap);
  a->t->rplan_f = true; /* actors exist to perform effects */
  a->t->rplan_file_root_c = sys->cfg.file_root_c;
  a->t->host = a;
  a->status = EM_UNSTARTED;
  a->handle_cap = 8;
  a->handle_v = calloc(a->handle_cap, sizeof(em_actor*));
  ax_assume(a->handle_v != NULL, "oom");
  a->handle_v[0] = a; /* handle 0 = self */
  a->handle_n = 1;
  if (sys->all_tail != NULL)
    sys->all_tail->all_next = a;
  else
    sys->all_head = a;
  sys->all_tail = a;
  return a;
}

em_actor* em_scheduler_actor(em_scheduler* sys) {
  pthread_mutex_lock(&sys->smtx);
  em_actor* a = em_register_locked(sys);
  pthread_mutex_unlock(&sys->smtx);
  return a;
}

void em_scheduler_free(em_scheduler* sys) {
  if (sys == NULL)
    return;
  for (em_actor* a = sys->all_head; a != NULL;) {
    em_actor* next = a->all_next;
    for (em_msg* m = a->mbox_head; m != NULL;) {
      em_msg* mn = m->next;
      free(m);
      m = mn;
    }
    free(a->handle_v);
    a->t->host = NULL;
    pl_thread_free(a->t);
    pl_heap_free(a->heap);
    free(a);
    a = next;
  }
  pl_store_set_concurrent(sys->store, false);
  pthread_mutex_destroy(&sys->smtx);
  pthread_cond_destroy(&sys->scnd);
  free(sys->workers);
  free(sys);
}

/* ── Small accessors ───────────────────────────────────────────────────── */

pl_thread* em_actor_thread(em_actor* a) {
  return a->t;
}

er_actor_status em_actor_state(const em_actor* a) {
  switch (a->status) {
  case EM_BLOCKED:
    return ER_ACTOR_BLOCKED;
  case EM_HALTED:
    return ER_ACTOR_HALTED;
  case EM_CRASHED:
    return ER_ACTOR_CRASHED;
  default:
    return ER_ACTOR_RUNNABLE; /* UNSTARTED / RUNNABLE / RUNNING */
  }
}

uint64_t em_actor_id(const em_actor* a) {
  return a->id;
}

em_actor* em_scheduler_actor_by_id(em_scheduler* sys, uint64_t id) {
  for (em_actor* a = sys->all_head; a != NULL; a = a->all_next)
    if (a->id == id)
      return a;
  return NULL;
}

pl_val em_actor_result(em_actor* a) {
  ax_assume(a->status == EM_HALTED, "em_actor_result: not halted");
  return pl_thread_result(a->t);
}

/* ── Run queue (smtx held) ─────────────────────────────────────────────── */

/* Link a (already accounted in `active`) at the queue tail and wake one
 * idle worker.  Sets it RUNNABLE. */
static void em_queue_link(em_actor* a) {
  a->qnext = NULL;
  if (a->sys->qtail != NULL)
    a->sys->qtail->qnext = a;
  else
    a->sys->qhead = a;
  a->sys->qtail = a;
  a->status = EM_RUNNABLE;
  pthread_cond_signal(&a->sys->scnd);
}

/* Admit a not-yet-active actor (start / spawn / wake): enters `active`. */
static void em_admit(em_actor* a) {
  a->sys->active++;
  em_queue_link(a);
}

static em_actor* em_dequeue(em_scheduler* sys) {
  em_actor* a = sys->qhead;
  if (a != NULL) {
    sys->qhead = a->qnext;
    if (sys->qhead == NULL)
      sys->qtail = NULL;
    a->qnext = NULL;
  }
  return a;
}

/* An actor left the active set (HALTED / CRASHED / BLOCKED): if the
 * system has gone quiet, tell the pool to drain and exit. */
static void em_retire(em_actor* a, em_status terminal) {
  a->status = terminal;
  if (--a->sys->active == 0) {
    a->sys->quiescing = true;
    pthread_cond_broadcast(&a->sys->scnd);
  }
}

/* ── Handle table (smtx held; logically owner-only) ────────────────────── */

static uint64_t em_handle_alloc(em_actor* a, em_actor* target) {
  if (a->handle_n == a->handle_cap) {
    a->handle_cap *= 2;
    a->handle_v = realloc(a->handle_v, a->handle_cap * sizeof(em_actor*));
    ax_assume(a->handle_v != NULL, "oom");
  }
  a->handle_v[a->handle_n] = target;
  return a->handle_n++;
}

static em_actor* em_handle_get(em_actor* a, pl_val h) {
  if (!pl_is_nat63(h) || h >= a->handle_n)
    return NULL;
  return a->handle_v[h];
}

/* ── Effect-name decoding ──────────────────────────────────────────────── */

static bool em_name_is(pl_val name, const char* s) {
  if (!pl_is_nat(name))
    return false;
  size_t n = strlen(s);
  if (pl_nat_byte_len(name) != n)
    return false;
  for (size_t i = 0; i < n; i++)
    if (pl_nat_byte_at(name, i) != (uint8_t)s[i])
      return false;
  return true;
}

/* ── Payload snapshotting (matches er_pin_payload) ─────────────────────── */

/*
 * Snapshot a payload out of the sender's moving heap into the store.
 * nat63s and pins are already shareable; anything else is pinned via a
 * [v] row.  Payloads arrive deeply normalized (the coordination ops carry
 * deep masks), so the pin here is a pure store copy — pl_pin takes the
 * store's own lock, so this is safe under smtx and against parallel pins.
 * false means the sender crashed while pinning.  Touches only the
 * sender's own heap (this worker owns it while RUNNING).
 */
static bool em_pin_payload(em_actor* a, pl_val v, pl_val* out) {
  if (pl_is_nat63(v)) {
    *out = v;
    return true;
  }
  if (pl_tag(v) == PL_TAG_PIN) {
    *out = v;
    return true;
  }
  pl_thread* t = a->t;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    return false;
  }
  size_t base = t->vsp;
  pl_vpush(t, v);
  pl_gc_reserve(t, PL_APP_CELLS(1));
  PL_GC_FORBID(t);
  t->vstack[base] = pl_mk_app_from(t, 0, 1, &t->vstack[base]);
  PL_GC_ALLOW(t);
  pl_val pin = pl_pin(t, t->vstack[base]);
  *out = pl_app_args(pl_ptr(pl_pin_body(pl_ptr(pin))))[0];
  t->vsp = base;
  pl_catch_pop(t, &c);
  return true;
}

/* ── Messaging (smtx held) ─────────────────────────────────────────────── */

static void em_recv_ready(em_actor* a);

/* Append to the mailbox; wake the receiver if it is parked on Recv. */
static void em_deliver(em_actor* to, pl_val payload, uint32_t ncaps,
                       em_actor* const* caps) {
  em_msg* m = malloc(sizeof(em_msg) + (size_t)ncaps * sizeof(em_actor*));
  ax_assume(m != NULL, "oom");
  m->payload = payload;
  m->next = NULL;
  m->ncaps = ncaps;
  for (uint32_t i = 0; i < ncaps; i++)
    m->caps[i] = caps[i];
  if (to->mbox_tail != NULL)
    to->mbox_tail->next = m;
  else
    to->mbox_head = m;
  to->mbox_tail = m;
  if (to->status == EM_BLOCKED) { /* parked on Recv: wake and deliver */
    to->sys->active++;            /* BLOCKED -> active */
    em_recv_ready(to);
  }
}

/*
 * Deliver the mailbox head to a Recv: response valRow [msg, capsRow]
 * built in the receiver's heap, caps re-minted as fresh receiver-local
 * handles.  Deposits into the receiver's parked thread and enqueues it.
 * Does not touch `active`; the caller accounts for any BLOCKED->active
 * transition.
 */
static void em_recv_ready(em_actor* a) {
  em_msg* m = a->mbox_head;
  ax_assume(m != NULL, "em_recv_ready: empty mailbox");
  a->mbox_head = m->next;
  if (a->mbox_head == NULL)
    a->mbox_tail = NULL;

  pl_thread* t = a->t;
  size_t base = t->vsp;
  pl_val capsrow = 0; /* valRow [] is the nat 0 */
  if (m->ncaps > 0) {
    pl_gc_reserve(t, PL_APP_CELLS(m->ncaps));
    PL_GC_FORBID(t);
    pl_cell* p = pl_bump(t, PL_APP_CELLS(m->ncaps));
    p[0] = pl_hdr_make(PL_K_APP, 0, 0, PL_APP_CELLS(m->ncaps));
    p[1] = 0;
    for (uint32_t i = 0; i < m->ncaps; i++)
      p[2 + i] = em_handle_alloc(a, m->caps[i]);
    PL_GC_ALLOW(t);
    capsrow = pl_make(PL_TAG_APP, p);
  }
  pl_vpush(t, capsrow);
  pl_gc_reserve(t, PL_APP_CELLS(2));
  PL_GC_FORBID(t);
  pl_val fields[2] = {m->payload, t->vstack[base]};
  pl_val resp = pl_mk_app_from(t, 0, 2, fields);
  PL_GC_ALLOW(t);
  t->vsp = base;
  free(m);

  pl_thread_deposit(t, resp);
  em_queue_link(a);
}

/* ── Coordination-effect service (smtx held) ───────────────────────────── */

static void em_crash_msg(em_actor* a, const char* msg) {
  a->t->exn = 0;
  a->t->exn_msg = msg;
  em_retire(a, EM_CRASHED);
}

/*
 * Translate a caps row in the sender's heap into actor refs via the
 * sender's handle table (matches er_load_caps): the row and each element
 * are forced; non-nat63 elements are dropped; a nat63 that is not a live
 * handle is an error (sender crash).  Returns the count, or -1 on crash;
 * *out is a malloc'd array (may be NULL when empty).
 */
static int64_t em_load_caps(em_actor* a, size_t capslot, em_actor*** out) {
  pl_thread* t = a->t;
  *out = NULL;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    return -1;
  }
  pl_val row = pl_whnf(t, t->vstack[capslot]);
  t->vstack[capslot] = row;
  pl_cell* rp = pl_as(PL_TAG_APP, row);
  if (rp == NULL) {
    pl_catch_pop(t, &c);
    return 0;
  }
  uint32_t n = pl_app_n(rp);
  em_actor** caps = malloc((size_t)n * sizeof(em_actor*));
  ax_assume(caps != NULL, "oom");
  int64_t ncaps = 0;
  for (uint32_t i = 0; i < n; i++) {
    pl_val e = pl_app_args(pl_ptr(t->vstack[capslot]))[i];
    e = pl_whnf(t, e);
    if (!pl_is_nat63(e))
      continue;
    em_actor* target = em_handle_get(a, e);
    if (target == NULL) {
      free(caps);
      pl_catch_pop(t, &c);
      return -1;
    }
    caps[ncaps++] = target;
  }
  pl_catch_pop(t, &c);
  *out = caps;
  return ncaps;
}

/* Service the request parked by PL_RUN_BLOCKED.  smtx is held. */
static void em_service(em_scheduler* sys, em_actor* a) {
  pl_thread* t = a->t;
  pl_val req = pl_thread_request(t);
  pl_cell* p = pl_as(PL_TAG_APP, req);
  ax_assume(p != NULL, "em_service: malformed request");
  pl_val name = pl_app_head(p);
  uint32_t argc = pl_app_n(p);
  pl_val* args = pl_app_args(p);

  if (em_name_is(name, "Recv")) {
    ax_assume(argc == 1, "Recv arity");
    if (a->mbox_head == NULL)
      em_retire(a, EM_BLOCKED); /* park; a deliver will wake us */
    else
      em_recv_ready(a); /* RUNNING -> RUNNABLE, active unchanged */
    return;
  }

  if (em_name_is(name, "Send")) {
    ax_assume(argc == 2, "Send arity");
    em_actor* to = em_handle_get(a, args[0]);
    if (to == NULL) {
      em_crash_msg(a, "invalid actor handle");
      return;
    }
    pl_val payload;
    if (!em_pin_payload(a, args[1], &payload)) {
      em_retire(a, EM_CRASHED);
      return;
    }
    em_deliver(to, payload, 0, NULL);
    pl_thread_deposit(t, 0);
    em_queue_link(a);
    return;
  }

  if (em_name_is(name, "SendCaps")) {
    ax_assume(argc == 3, "SendCaps arity");
    em_actor* to = em_handle_get(a, args[0]);
    if (to == NULL) {
      em_crash_msg(a, "invalid actor handle");
      return;
    }
    size_t base = t->vsp;
    pl_vpush(t, args[1]);
    pl_vpush(t, args[2]);
    em_actor** caps;
    int64_t ncaps = em_load_caps(a, base + 1, &caps);
    if (ncaps < 0) {
      t->vsp = base;
      em_retire(a, EM_CRASHED);
      return;
    }
    pl_val payload;
    bool ok = em_pin_payload(a, t->vstack[base], &payload);
    t->vsp = base;
    if (!ok) {
      free(caps);
      em_retire(a, EM_CRASHED);
      return;
    }
    em_deliver(to, payload, (uint32_t)ncaps, caps);
    free(caps);
    pl_thread_deposit(t, 0);
    em_queue_link(a);
    return;
  }

  if (em_name_is(name, "Spawn")) {
    ax_assume(argc == 1, "Spawn arity");
    pl_val fn;
    if (!em_pin_payload(a, args[0], &fn)) {
      em_retire(a, EM_CRASHED);
      return;
    }
    em_actor* child = em_register_locked(sys);
    /* the child's boot fn is store-resident (just pinned); arm it on the
     * child's own heap and admit it to the run queue */
    child->status = EM_RUNNABLE;
    pl_thread_start_call_nf(child->t, fn, 0);
    em_admit(child);
    uint64_t h = em_handle_alloc(a, child);
    pl_thread_deposit(t, h);
    em_queue_link(a);
    return;
  }

  if (em_name_is(name, "CloseHandle")) {
    ax_assume(argc == 1, "CloseHandle arity");
    pl_val h = args[0];
    if (pl_is_nat63(h) && h < a->handle_n)
      a->handle_v[h] = NULL;
    pl_thread_deposit(t, 0);
    em_queue_link(a);
    return;
  }

  ax_abort("em_service: unknown coordination op");
}

/* ── Workers & the run loop ────────────────────────────────────────────── */

void em_actor_start(em_actor* a, pl_val fn) {
  pthread_mutex_lock(&a->sys->smtx);
  ax_assume(a->status == EM_UNSTARTED, "em_actor_start: already started");
  pl_thread_start_call_nf(a->t, fn, 0);
  em_admit(a);
  pthread_mutex_unlock(&a->sys->smtx);
}

/* Resolve one finished slice (smtx held). */
static void em_step(em_scheduler* sys, em_actor* a, pl_run_status s) {
  switch (s) {
  case PL_RUN_YIELDED:
    em_queue_link(a); /* RUNNING -> RUNNABLE, still active */
    break;
  case PL_RUN_DONE:
    em_retire(a, EM_HALTED);
    break;
  case PL_RUN_EXN:
    em_retire(a, EM_CRASHED);
    break;
  case PL_RUN_BLOCKED:
    em_service(sys, a);
    break;
  }
}

static void* em_worker_main(void* arg) {
  em_scheduler* sys = arg;
  for (;;) {
    pthread_mutex_lock(&sys->smtx);
    while (sys->qhead == NULL && !sys->quiescing)
      pthread_cond_wait(&sys->scnd, &sys->smtx);
    if (sys->qhead == NULL) { /* quiescing */
      pthread_mutex_unlock(&sys->smtx);
      return NULL;
    }
    em_actor* a = em_dequeue(sys);
    a->status = EM_RUNNING; /* RUNNABLE -> RUNNING, active unchanged */
    pthread_mutex_unlock(&sys->smtx);

    /* The reduction work: lock-free, exclusive on this actor's heap. */
    pl_run_status s = pl_thread_run(a->t, sys->cfg.quantum);

    pthread_mutex_lock(&sys->smtx);
    em_step(sys, a, s);
    pthread_mutex_unlock(&sys->smtx);
  }
}

er_run_reason em_scheduler_run(em_scheduler* sys) {
  pthread_mutex_lock(&sys->smtx);
  /* If nothing is runnable, quiesce immediately so workers exit at once;
   * otherwise let the natural active->0 transition declare it. */
  sys->quiescing = (sys->active == 0);
  pthread_mutex_unlock(&sys->smtx);

  for (int i = 0; i < sys->nworkers; i++)
    ax_assume(pthread_create(&sys->workers[i], NULL, em_worker_main, sys) == 0,
              "em_scheduler_run: pthread_create failed");
  for (int i = 0; i < sys->nworkers; i++)
    pthread_join(sys->workers[i], NULL);

  /* Quiet now: workers are joined, no concurrency.  Reset for a possible
   * subsequent run and report whether any actor is parked on Recv. */
  sys->quiescing = false;
  er_run_reason reason = ER_RUN_IDLE;
  for (em_actor* a = sys->all_head; a != NULL; a = a->all_next)
    if (a->status == EM_BLOCKED) {
      reason = ER_RUN_QUIESCENT;
      break;
    }
  return reason;
}

void em_scheduler_inject(em_scheduler* sys, em_actor* to, pl_val payload) {
  ax_assume(pl_is_nat63(payload) || pl_store_owns(sys->store, payload),
            "em_scheduler_inject: payload must be a nat63 or store-resident");
  pthread_mutex_lock(&sys->smtx);
  if (to->status != EM_HALTED && to->status != EM_CRASHED)
    em_deliver(to, payload, 0, NULL);
  pthread_mutex_unlock(&sys->smtx);
}
