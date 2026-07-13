#include "enki/actor.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actor_internal.h"
#include "axsys/assume.h"
#include "axsys/ds.h"
#include "axsys/sha256.h"
#include "plan/build.h"
#include "plan/debug.h"
#include "plan/eval.h"
#include "axsys/allocator.h"
#include "plan/nat.h"
#include "plan/rplan.h"

/* Defined with the event-log helpers below; ReadFolder service appears in
 * the scheduler section above that implementation. */
static void er_vals_hash(const pl_val* args, uint32_t argc, uint8_t out[32]);
static uint8_t* er_folder_result_encode(pl_val row, uint64_t* out_n);
static pl_val er_folder_result_build(pl_thread* t, const uint8_t* data,
                                     uint64_t data_n);

/*
 * Deterministic single-OS-thread executor.  Everything
 * is FIFO and fuel-driven: scheduling decisions are a pure function of
 * the initial actor set, injections, and the quantum.  Service
 * order on a coordination effect is fixed: a woken receiver is enqueued
 * before the serviced sender resumes.
 *
 * Semantics follow the reaver reference (the semantic oracle):
 *   - Payloads (message bodies, spawn fns) are forced and pinned at
 *     initiation, in the sender/parent; reaver shares its heap and
 *     forces lazily at the receiver — an accepted divergence.
 *   - Send/SendCaps to an invalid handle, or a PLAN exception while
 *     pinning a payload, crashes the *sender* (reaver: host `error`).
 *   - CloseHandle of an unknown handle is a silent no-op (IntMap.delete).
 *   - Recv responds [msg, capsRow]; the empty row is nat 0 (valRow []).
 *   - Cap rows drop non-nat63 elements silently (reaver loadCap), but a
 *     nat63 cap that is not a live handle is an error.
 */

#define ER_DEFAULT_QUANTUM    4096
#define ER_DEFAULT_HEAP_CELLS 8192 /* 64 KiB per semispace */

/* Struct definitions live in actor_internal.h, shared with http.c. */

/* ── Construction ──────────────────────────────────────────────────────── */

er_scheduler* er_scheduler_new(pl_store* store, er_config cfg) {
  ax_assume(store != NULL, "er_scheduler_new: store required");
  er_scheduler* sys = calloc(1, sizeof(*sys));
  ax_assume(sys != NULL, "oom");
  ax_assume(pthread_mutex_init(&sys->mu, NULL) == 0, "pthread_mutex_init");
  ax_assume(pthread_cond_init(&sys->cv, NULL) == 0, "pthread_cond_init");
  sys->store = store;
  sys->cfg = cfg;
  if (sys->cfg.quantum == 0) {
    const char* q_c = getenv("ENKI_QUANTUM");
    sys->cfg.quantum =
        q_c != NULL && q_c[0] != '\0' ? strtoull(q_c, NULL, 10) : 0;
    if (sys->cfg.quantum == 0)
      sys->cfg.quantum = ER_DEFAULT_QUANTUM;
  }
  if (sys->cfg.heap_cells == 0) {
    const char* h_c = getenv("ENKI_ACTOR_HEAP_CELLS");
    sys->cfg.heap_cells =
        h_c != NULL && h_c[0] != '\0' ? strtoull(h_c, NULL, 10) : 0;
    if (sys->cfg.heap_cells == 0)
      sys->cfg.heap_cells = ER_DEFAULT_HEAP_CELLS;
  }
  ax_assume(sys->cfg.quantum >= 2, "er_scheduler_new: quantum must be >= 2");
  return sys;
}

static er_actor* er_register(er_scheduler* sys, pl_heap* heap, pl_thread* t,
                             bool adopted) {
  er_actor* a = calloc(1, sizeof(*a));
  ax_assume(a != NULL, "oom");
  a->sys = sys;
  a->id = sys->next_id++;
  a->heap = heap;
  a->t = t;
  a->t->host = a; /* effect attribution for the pl_io_hook */
  a->adopted = adopted;
  a->handle_cap = 8;
  a->handle_v = calloc(a->handle_cap, sizeof(er_actor*));
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

er_actor* er_scheduler_actor(er_scheduler* sys) {
  pl_heap* heap = pl_heap_new(sys->cfg.heap_cells, sys->store);
  pl_thread* t = pl_thread_new(heap);
  t->rplan_f = true; /* actors exist to perform effects */
  t->rplan_file_root_c = sys->cfg.file_root_c;
  return er_register(sys, heap, t, false);
}

er_actor* er_scheduler_adopt(er_scheduler* sys, pl_thread* t) {
  ax_assume(pl_heap_store(t->heap) == sys->store,
            "er_scheduler_adopt: thread heap is not on the system store");
  return er_register(sys, t->heap, t, true);
}

void er_scheduler_free(er_scheduler* sys) {
  if (sys == NULL)
    return;
  er_http_teardown(sys); /* before the actors: transfers reference them */
  for (er_actor* a = sys->all_head; a != NULL;) {
    er_actor* next = a->all_next;
    for (er_msg* m = a->mbox_head; m != NULL;) {
      er_msg* mn = m->next;
      free(m);
      m = mn;
    }
    free(a->handle_v);
    a->t->host = NULL; /* the backpointer dies with the actor */
    if (!a->adopted) { /* adopted threads/heaps stay with the embedder */
      pl_thread_free(a->t);
      pl_heap_free(a->heap);
    }
    free(a);
    a = next;
  }
  pthread_cond_destroy(&sys->cv);
  pthread_mutex_destroy(&sys->mu);
  free(sys);
}

/* ── Small accessors ───────────────────────────────────────────────────── */

pl_thread* er_actor_thread(er_actor* a) {
  return a->t;
}

er_actor_status er_actor_state(const er_actor* a) {
  return a->status;
}

uint64_t er_actor_id(const er_actor* a) {
  return a->id;
}

er_actor* er_scheduler_actor_by_id(er_scheduler* sys, uint64_t id) {
  for (er_actor* a = sys->all_head; a != NULL; a = a->all_next)
    if (a->id == id)
      return a;
  return NULL;
}

pl_val er_actor_result(er_actor* a) {
  ax_assume(a->status == ER_ACTOR_HALTED, "er_actor_result: not halted");
  return pl_thread_result(a->t);
}

/* ── Run queue / handle table ──────────────────────────────────────────── */

static bool er_trace(void) {
  static int on = -1;
  if (on < 0)
    on = getenv("ENKI_TRACE") != NULL;
  return on;
}

void er_enqueue(er_actor* a) {
  if (er_trace())
    fprintf(stderr, "[trace] enqueue actor=%llu\n", (unsigned long long)a->id);
  a->qnext = NULL;
  if (a->sys->qtail != NULL)
    a->sys->qtail->qnext = a;
  else
    a->sys->qhead = a;
  a->sys->qtail = a;
  pthread_cond_signal(&a->sys->cv);
}

static er_actor* er_dequeue(er_scheduler* sys) {
  er_actor* a = sys->qhead;
  if (a != NULL) {
    sys->qhead = a->qnext;
    if (sys->qhead == NULL)
      sys->qtail = NULL;
    a->qnext = NULL;
  }
  return a;
}

static bool er_remove_from_queue(er_scheduler* sys, er_actor* target) {
  er_actor* prev = NULL;
  for (er_actor* a = sys->qhead; a != NULL; a = a->qnext) {
    if (a != target) {
      prev = a;
      continue;
    }
    if (prev != NULL)
      prev->qnext = a->qnext;
    else
      sys->qhead = a->qnext;
    if (sys->qtail == a)
      sys->qtail = prev;
    a->qnext = NULL;
    return true;
  }
  return false;
}

static er_actor* er_dequeue_spawned(er_scheduler* sys) {
  for (er_actor* a = sys->qhead; a != NULL; a = a->qnext) {
    if (!a->adopted) {
      (void)er_remove_from_queue(sys, a);
      if (er_trace())
        fprintf(stderr, "[trace] dequeue-spawned actor=%llu\n",
                (unsigned long long)a->id);
      return a;
    }
  }
  return NULL;
}

static uint64_t er_handle_alloc(er_actor* a, er_actor* target) {
  if (a->handle_n == a->handle_cap) {
    a->handle_cap *= 2;
    a->handle_v = realloc(a->handle_v, a->handle_cap * sizeof(er_actor*));
    ax_assume(a->handle_v != NULL, "oom");
  }
  a->handle_v[a->handle_n] = target;
  return a->handle_n++;
}

/* The actor behind handle value h, or NULL (unforgeable: only minted
 * nats index the table; closed entries are NULL). */
static er_actor* er_handle_get(er_actor* a, pl_val h) {
  if (!pl_is_nat63(h) || h >= a->handle_n)
    return NULL;
  return a->handle_v[h];
}

/* ── Effect-name decoding ──────────────────────────────────────────────── */

static bool er_name_is(pl_val name, const char* s) {
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

/* The evaluator parks coordination requests with the op-table INDEX at
 * the head of the spine (rp_request rebuilds from the dispatch stack),
 * while hand-built requests (and the unit tests) use the op-name atom.
 * Accept both: translate an index head to its table name.
 *
 * THIS IS A DUMB HACK: TODO: FIX
 * */
static bool er_op_is(pl_val name, const char* s) {
  extern const size_t pl_nops;
  if (pl_is_nat63(name) && (size_t)name < pl_nops)
    return strcmp(pl_io_name((uint32_t)name), s) == 0;
  return er_name_is(name, s);
}

/* ── Messaging ─────────────────────────────────────────────────────────── */

void er_actor_start(er_actor* a, pl_val fn) {
  if (er_trace())
    fprintf(stderr, "[trace] start actor=%llu\n", (unsigned long long)a->id);
  ax_assume(!a->started && a->status == ER_ACTOR_RUNNABLE,
            "er_actor_start: actor already started");
  a->started = true;
  pl_thread_start_call_nf(a->t, fn, 0);
  er_enqueue(a);
}

/*
 * Snapshot a payload out of the sender's moving heap: nat63s and
 * pins are already shareable; anything else is pinned via a [v] row so
 * the store copy is unambiguous even when v is itself a pin.  Payloads
 * arrive deeply normalized — the coordination ops carry deep masks, so
 * forcing (and any effects or exceptions inside it)
 * happened at initiation as the sender's own execution — which makes
 * the pin here a pure store copy.  false means the sender crashed.
 */
static bool er_pin_payload(er_actor* a, pl_val v, pl_val* out) {
  if (pl_is_nat63(v)) {
    *out = v;
    return true;
  }
  if (pl_tag(v) == PL_TAG_PIN) {
    *out = v; /* pins are store-resident by construction (S8) */
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

static void er_recv_ready(er_actor* a);

/* Append to the mailbox; wake the receiver if it is parked on Recv. */
static void er_deliver(er_actor* to, pl_val payload, uint32_t ncaps,
                       er_actor* const* caps) {
  er_msg* m = malloc(sizeof(er_msg) + (size_t)ncaps * sizeof(er_actor*));
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
  /* an http-parked actor is not waiting on Recv; its mail just queues */
  if (to->status == ER_ACTOR_BLOCKED && to->http == NULL)
    er_recv_ready(to);
}

/*
 * Deliver the mailbox head to a Recv: response is valRow [msg, capsRow]
 * built in the receiver's heap, caps re-minted as fresh receiver-local
 * handles.  The receiver becomes runnable.
 */
static void er_recv_ready(er_actor* a) {
  er_msg* m = a->mbox_head;
  ax_assume(m != NULL, "er_recv_ready: empty mailbox");
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
      p[2 + i] = er_handle_alloc(a, m->caps[i]);
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
  a->status = ER_ACTOR_RUNNABLE;
  er_enqueue(a);
}

/* ── Coordination-effect service ───────────────────────────────────────── */

static void er_crash(er_actor* a) {
  a->status = ER_ACTOR_CRASHED; /* never resumed */
}

/* Service-detected crash (no PLAN raise happened): leave a message in
 * the thread's exn slot so embedders report something useful. */
void er_crash_msg(er_actor* a, const char* msg) {
  a->t->exn = 0;
  a->t->exn_msg = msg;
  er_crash(a);
}

/*
 * Translate a caps row in the sender's heap into actor refs via the
 * sender's handle table (reaver opSendCaps/loadCapsRow): the row and
 * each element are forced; non-nat63 elements are dropped; a nat63 that
 * is not a live handle is an error (sender crash).  Returns the count,
 * or -1 on crash.  *out is a malloc'd array (may be NULL when empty).
 */
static int64_t er_load_caps(er_actor* a, size_t capslot, er_actor*** out) {
  pl_thread* t = a->t;
  *out = NULL;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    return -1;
  }
  pl_val row = pl_whnf(t, t->vstack[capslot]);
  t->vstack[capslot] = row; /* slot address computed after the eval */
  pl_cell* rp = pl_as(PL_TAG_APP, row);
  if (rp == NULL) { /* rowVals of a non-row is [] */
    pl_catch_pop(t, &c);
    return 0;
  }
  uint32_t n = pl_app_n(rp);
  er_actor** caps = malloc((size_t)n * sizeof(er_actor*));
  ax_assume(caps != NULL, "oom");
  int64_t ncaps = 0;
  for (uint32_t i = 0; i < n; i++) {
    /* re-fetch through the rooted slot: forcing may move the row */
    pl_val e = pl_app_args(pl_ptr(t->vstack[capslot]))[i];
    e = pl_whnf(t, e);
    if (!pl_is_nat63(e))
      continue; /* loadCap: dropped silently */
    er_actor* target = er_handle_get(a, e);
    if (target == NULL) {
      free(caps);
      pl_catch_pop(t, &c);
      return -1; /* getActor: invalid handle is an error */
    }
    caps[ncaps++] = target;
  }
  pl_catch_pop(t, &c);
  *out = caps;
  return ncaps;
}

/* Service the request parked by PL_RUN_BLOCKED.  The request spine and
 * its args live in the actor's heap: every arg used across a reserve or
 * an evaluation is copied to the actor's vstack (rooted) first. */
static void er_service(er_scheduler* sys, er_actor* a) {
  pl_thread* t = a->t;
  pl_val req = pl_thread_request(t);
  pl_cell* p = pl_as(PL_TAG_APP, req);
  ax_assume(p != NULL, "er_service: malformed request");
  pl_val name = pl_app_head(p);
  uint32_t argc = pl_app_n(p);
  pl_val* args = pl_app_args(p);

  if (er_op_is(name, "Recv")) {
    ax_assume(argc == 1, "Recv arity");
    if (a->mbox_head == NULL)
      a->status = ER_ACTOR_BLOCKED; /* park; a deliver will wake us */
    else
      er_recv_ready(a);
    return;
  }

  if (er_op_is(name, "Send")) {
    ax_assume(argc == 2, "Send arity");
    er_actor* to = er_handle_get(a, args[0]);
    if (to == NULL) {
      er_crash_msg(a, "invalid actor handle");
      return;
    }
    pl_val payload;
    if (!er_pin_payload(a, args[1], &payload)) {
      er_crash(a);
      return;
    }
    er_deliver(to, payload, 0, NULL); /* receiver wakes (and queues) first */
    pl_thread_deposit(t, 0);
    a->status = ER_ACTOR_RUNNABLE;
    er_enqueue(a);
    return;
  }

  if (er_op_is(name, "SendCaps")) {
    ax_assume(argc == 3, "SendCaps arity");
    er_actor* to = er_handle_get(a, args[0]);
    if (to == NULL) {
      er_crash_msg(a, "invalid actor handle");
      return;
    }
    /* root msg and caps before any forcing/reserve moves the request */
    size_t base = t->vsp;
    pl_vpush(t, args[1]);
    pl_vpush(t, args[2]);
    er_actor** caps;
    int64_t ncaps = er_load_caps(a, base + 1, &caps);
    if (ncaps < 0) {
      t->vsp = base;
      er_crash(a);
      return;
    }
    pl_val payload;
    bool ok = er_pin_payload(a, t->vstack[base], &payload);
    t->vsp = base;
    if (!ok) {
      free(caps);
      er_crash(a);
      return;
    }
    er_deliver(to, payload, (uint32_t)ncaps, caps);
    free(caps);
    pl_thread_deposit(t, 0);
    a->status = ER_ACTOR_RUNNABLE;
    er_enqueue(a);
    return;
  }

  if (er_op_is(name, "Spawn")) {
    ax_assume(argc == 1, "Spawn arity");
    pl_val fn;
    if (!er_pin_payload(a, args[0], &fn)) {
      er_crash(a);
      return;
    }
    er_actor* child = er_scheduler_actor(sys);
    er_actor_start(child, fn); /* child queued before the parent resumes */
    uint64_t h = er_handle_alloc(a, child);
    pl_thread_deposit(t, h);
    a->status = ER_ACTOR_RUNNABLE;
    er_enqueue(a);
    return;
  }

  if (er_op_is(name, "Fetch")) {
    er_http_service(sys, a, argc, args);
    return;
  }

  if (er_op_is(name, "ReadFolder")) {
    ax_assume(argc == 1, "ReadFolder arity");
    uint8_t hash[32];
    er_vals_hash(args, argc, hash);
    pl_val result;
    if (sys->mode == ER_MODE_REPLAY) {
      const er_event* e = er_replay_next(sys);
      ax_assume(e->kind == ER_EV_FOLDER && e->actor == a->id &&
                    memcmp(e->args_hash, hash, 32) == 0,
                "er_log: replay divergence at a ReadFolder");
      result = er_folder_result_build(t, e->data, e->data_n);
    } else {
      /* Root the path while the directory result is assembled: its request
       * spine may move during collection. */
      size_t base = t->vsp;
      pl_vpush(t, args[0]);
      result = pl_rplan_read_folder(t, t->vstack[base]);
      t->vsp = base;
      if (sys->mode == ER_MODE_RECORD) {
        er_event e = {.kind = ER_EV_FOLDER, .actor = a->id};
        memcpy(e.args_hash, hash, 32);
        e.data = er_folder_result_encode(result, &e.data_n);
        ax_arrpush(sys->rec->ev, e);
      }
    }
    pl_thread_deposit(t, result);
    a->status = ER_ACTOR_RUNNABLE;
    er_enqueue(a);
    return;
  }

  if (er_op_is(name, "CloseHandle")) {
    ax_assume(argc == 1, "CloseHandle arity");
    pl_val h = args[0];
    /* unknown handles are a silent no-op; even handle 0 may be closed
     * (Recv reads the inbox directly, not the table — reaver rtsInbox) */
    if (pl_is_nat63(h) && h < a->handle_n)
      a->handle_v[h] = NULL;
    pl_thread_deposit(t, 0);
    a->status = ER_ACTOR_RUNNABLE;
    er_enqueue(a);
    return;
  }

  {
    size_t dbg_n;
    char* dbg_s = pl_show(ax_allocator_system(), req, &dbg_n);
    fprintf(stderr, "er_service: unmatched request: %.*s\n", (int)dbg_n, dbg_s);
  }
  ax_abort("er_service: unknown coordination op");
}

/* ── The executor loop ─────────────────────────────────────────────────── */

/* One scheduling step of a spawned (scheduler-owned) actor. */
static void er_step(er_scheduler* sys, er_actor* a, pl_run_status s) {
  switch (s) {
  case PL_RUN_YIELDED:
    er_enqueue(a); /* round-robin fairness */
    break;
  case PL_RUN_DONE:
    a->status = ER_ACTOR_HALTED; /* result discarded (kept for tests) */
    break;
  case PL_RUN_EXN:
    er_crash(a);
    break;
  case PL_RUN_BLOCKED:
    er_service(sys, a);
    break;
  }
}

er_run_reason er_scheduler_run(er_scheduler* sys) {
  for (;;) {
    er_http_pump(sys);
    er_actor* a = er_dequeue(sys);
    if (a == NULL) {
      if (er_http_idle(sys))
        continue; /* an http completion made someone runnable */
      for (er_actor* it = sys->all_head; it != NULL; it = it->all_next)
        if (it->status == ER_ACTOR_BLOCKED)
          return ER_RUN_QUIESCENT;
      return ER_RUN_IDLE;
    }
    ax_assume(!a->adopted,
              "er_scheduler_run: adopted actors run under er_scheduler_drive");
    er_step(sys, a, pl_thread_run(a->t, sys->cfg.quantum));
  }
}

static er_run_reason er_run_reason_locked(er_scheduler* sys) {
  for (er_actor* it = sys->all_head; it != NULL; it = it->all_next)
    if (it->status == ER_ACTOR_BLOCKED)
      return ER_RUN_QUIESCENT;
  return ER_RUN_IDLE;
}

static void* er_mt_worker(void* arg) {
  er_mt_executor* ex = arg;
  er_scheduler* sys = ex->sys;
  for (;;) {
    pthread_mutex_lock(&sys->mu);
    er_actor* a = NULL;
    for (;;) {
      if (ex->stopping) {
        pthread_mutex_unlock(&sys->mu);
        return NULL;
      }
      a = ex->root == NULL ? er_dequeue(sys) : er_dequeue_spawned(sys);
      if (a != NULL)
        break;
      if (er_http_mt_pump(sys))
        continue; /* pumped the CURLM: completions may have enqueued */
      if (ex->root == NULL && ex->active == 0 && !er_http_outstanding(sys)) {
        ex->reason = er_run_reason_locked(sys);
        ex->stopping = true;
        pthread_cond_broadcast(&sys->cv);
        pthread_mutex_unlock(&sys->mu);
        return NULL;
      }
      pthread_cond_wait(&sys->cv, &sys->mu);
    }
    ax_assume(!a->adopted, "er_mt_executor_run: adopted actors run under "
                           "er_scheduler_drive");
    ex->active++;
    pthread_mutex_unlock(&sys->mu);

    pl_run_status s = pl_thread_run(a->t, sys->cfg.quantum);

    pthread_mutex_lock(&sys->mu);
    er_step(sys, a, s);
    ex->active--;
    if (ex->root == NULL && sys->qhead == NULL && ex->active == 0 &&
        !er_http_outstanding(sys)) {
      ex->reason = er_run_reason_locked(sys);
      ex->stopping = true;
      pthread_cond_broadcast(&sys->cv);
    } else {
      pthread_cond_broadcast(&sys->cv);
    }
    pthread_mutex_unlock(&sys->mu);
  }
}

static uint32_t er_default_worker_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 2)
    return 2;
  if (n > UINT32_MAX)
    return UINT32_MAX;
  return (uint32_t)n;
}

er_mt_executor* er_mt_executor_new(er_scheduler* sys, er_mt_config cfg) {
  ax_assume(sys != NULL, "er_mt_executor_new: scheduler required");
  er_mt_executor* ex = calloc(1, sizeof(*ex));
  ax_assume(ex != NULL, "oom");
  ex->sys = sys;
  ex->workers = cfg.workers == 0 ? er_default_worker_count() : cfg.workers;
  ax_assume(ex->workers > 0, "er_mt_executor_new: workers required");
  ex->thread_v = calloc(ex->workers, sizeof(*ex->thread_v));
  ax_assume(ex->thread_v != NULL, "oom");
  return ex;
}

void er_mt_executor_free(er_mt_executor* ex) {
  if (ex == NULL)
    return;
  free(ex->thread_v);
  free(ex);
}

er_run_reason er_mt_executor_run(er_mt_executor* ex) {
  ax_assume(ex != NULL, "er_mt_executor_run: executor required");
  er_scheduler* sys = ex->sys;
  ax_assume(sys->mode == ER_MODE_LIVE,
            "er_mt_executor_run: record/replay requires the deterministic "
            "executor");
  ex->root = NULL;
  ex->active = 0;
  ex->stopping = false;
  ex->reason = ER_RUN_IDLE;
  for (uint32_t i = 0; i < ex->workers; i++)
    ax_assume(pthread_create(&ex->thread_v[i], NULL, er_mt_worker, ex) == 0,
              "pthread_create");
  for (uint32_t i = 0; i < ex->workers; i++)
    ax_assume(pthread_join(ex->thread_v[i], NULL) == 0, "pthread_join");
  return ex->reason;
}

/* Abandon the root's parked continuation: unwind to the watermarks the
 * arming recorded, so the embedder can re-arm the thread cleanly. */
static void er_root_unwind(er_actor* root) {
  root->t->vsp = root->t->base_vsp;
  root->t->fsp = root->t->base_fsp;
  root->status = ER_ACTOR_RUNNABLE;
}

er_drive_status er_scheduler_drive(er_scheduler* sys, er_actor* root) {
  ax_assume(root->adopted, "er_scheduler_drive: actor is not adopted");
  root->status = ER_ACTOR_RUNNABLE;
  er_enqueue(root);
  for (;;) {
    er_http_pump(sys);
    er_actor* a = er_dequeue(sys);
    if (a == NULL) {
      if (er_http_idle(sys))
        continue; /* an http completion made someone runnable */
      /* root is parked on Recv and nothing runnable can ever wake it */
      er_root_unwind(root);
      return ER_DRIVE_DEADLOCK;
    }
    if (a != root) {
      er_step(sys, a, pl_thread_run(a->t, sys->cfg.quantum));
      continue;
    }
    switch (pl_thread_run(a->t, sys->cfg.quantum)) {
    case PL_RUN_YIELDED:
      er_enqueue(a);
      break;
    case PL_RUN_DONE:
      return ER_DRIVE_DONE; /* leftover actors stay parked/queued */
    case PL_RUN_EXN:
      return ER_DRIVE_EXN; /* pl_thread_run unwound to the watermarks */
    case PL_RUN_BLOCKED:
      er_service(sys, a);
      if (a->status == ER_ACTOR_CRASHED) {
        er_root_unwind(root); /* the embedder owns its fate */
        return ER_DRIVE_EXN;  /* exn slots set by the service */
      }
      break;
    }
  }
}

er_drive_status er_mt_executor_drive(er_mt_executor* ex, er_actor* root) {
  ax_assume(ex != NULL, "er_mt_executor_drive: executor required");
  ax_assume(root->adopted, "er_mt_executor_drive: actor is not adopted");
  er_scheduler* sys = ex->sys;
  ax_assume(root->sys == sys, "er_mt_executor_drive: root is from another "
                              "scheduler");
  ax_assume(sys->mode == ER_MODE_LIVE,
            "er_mt_executor_drive: record/replay requires the deterministic "
            "executor");

  ex->root = root;
  ex->active = 0;
  ex->stopping = false;
  ex->reason = ER_RUN_IDLE;
  for (uint32_t i = 0; i < ex->workers; i++)
    ax_assume(pthread_create(&ex->thread_v[i], NULL, er_mt_worker, ex) == 0,
              "pthread_create");

  er_drive_status out = ER_DRIVE_DEADLOCK;
  pthread_mutex_lock(&sys->mu);
  root->status = ER_ACTOR_RUNNABLE;
  for (;;) {
    if (root->status == ER_ACTOR_RUNNABLE) {
      (void)er_remove_from_queue(sys, root);
      pthread_mutex_unlock(&sys->mu);
      pl_run_status s = pl_thread_run(root->t, sys->cfg.quantum);
      pthread_mutex_lock(&sys->mu);
      switch (s) {
      case PL_RUN_YIELDED:
        root->status = ER_ACTOR_RUNNABLE;
        break;
      case PL_RUN_DONE:
        out = ER_DRIVE_DONE;
        ex->stopping = true;
        pthread_cond_broadcast(&sys->cv);
        goto done;
      case PL_RUN_EXN:
        out = ER_DRIVE_EXN;
        ex->stopping = true;
        pthread_cond_broadcast(&sys->cv);
        goto done;
      case PL_RUN_BLOCKED:
        er_service(sys, root);
        if (root->status == ER_ACTOR_CRASHED) {
          er_root_unwind(root);
          out = ER_DRIVE_EXN;
          ex->stopping = true;
          pthread_cond_broadcast(&sys->cv);
          goto done;
        }
        break;
      }
      continue;
    }

    if (root->status == ER_ACTOR_BLOCKED && sys->qhead == NULL &&
        ex->active == 0 && !er_http_outstanding(sys)) {
      er_root_unwind(root);
      out = ER_DRIVE_DEADLOCK;
      ex->stopping = true;
      pthread_cond_broadcast(&sys->cv);
      goto done;
    }

    pthread_cond_wait(&sys->cv, &sys->mu);
  }

done:
  pthread_mutex_unlock(&sys->mu);
  for (uint32_t i = 0; i < ex->workers; i++)
    ax_assume(pthread_join(ex->thread_v[i], NULL) == 0, "pthread_join");
  ex->root = NULL;
  return out;
}

/* ── Event log & replay ────────────────────────────────────────────────── */

er_log* er_log_new(void) {
  er_log* log = calloc(1, sizeof(*log));
  ax_assume(log != NULL, "oom");
  return log;
}

void er_log_free(er_log* log) {
  if (log == NULL)
    return;
  for (ptrdiff_t i = 0; i < ax_arrlen(log->ev); i++)
    free(log->ev[i].data);
  ax_arrfree(log->ev);
  free(log);
}

size_t er_log_events(const er_log* log) {
  return (size_t)ax_arrlen(log->ev);
}

/* Effect names are <= 8 bytes; pack them as a mote like ax_s*. */
uint64_t er_mote(const char* s) {
  uint64_t v = 0;
  for (int i = 0; s[i] != '\0'; i++) {
    ax_assume(i < 8, "er_log: effect name too long");
    v |= (uint64_t)(uint8_t)s[i] << (8 * i);
  }
  return v;
}

/* SHA-256 over the forced args of a direct effect, as length-prefixed
 * nat bytes (non-nat args — e.g. Now's ignored slot — are a marker). */
static void er_vals_hash(const pl_val* args, uint32_t argc, uint8_t out[32]) {
  uint8_t* buf = NULL;
  for (uint32_t i = 0; i < argc; i++) {
    pl_val v = args[i];
    if (pl_is_nat(v)) {
      ax_arrpush(buf, 'n');
      uint64_t n = pl_nat_byte_len(v);
      for (int b = 0; b < 8; b++)
        ax_arrpush(buf, (uint8_t)(n >> (8 * b)));
      for (uint64_t j = 0; j < n; j++)
        ax_arrpush(buf, pl_nat_byte_at(v, j));
    } else {
      ax_arrpush(buf, 'x');
    }
  }
  ax_sha256(buf, (size_t)ax_arrlen(buf), out);
  ax_arrfree(buf);
}

static void er_args_hash(pl_thread* t, uint32_t op, size_t ab,
                         uint8_t out[32]) {
  er_vals_hash(&t->vstack[ab], pl_io_argc(op), out);
}

static uint8_t* er_nat_bytes(pl_val v, uint64_t* out_n) {
  uint64_t n = pl_nat_byte_len(v);
  uint8_t* b = malloc(n ? (size_t)n : 1);
  ax_assume(b != NULL, "oom");
  for (uint64_t i = 0; i < n; i++)
    b[i] = pl_nat_byte_at(v, i);
  *out_n = n;
  return b;
}

/* ReadFolder log payload: entry count followed by
 *   is-folder:u8, name-length:u64-le, name-bytes
 * for every entry.  Zero is both an empty row and the rplan error result. */
static uint8_t* er_folder_result_encode(pl_val row, uint64_t* out_n) {
  pl_cell* row_p = NULL;
  uint32_t count = 0;
  size_t size = 8;
  if (row != 0) {
    row_p = pl_as(PL_TAG_APP, row);
    ax_assume(row_p != NULL && pl_app_head(row_p) == 0,
              "er_log: malformed ReadFolder result");
    count = pl_app_n(row_p);
    for (uint32_t i = 0; i < count; i++) {
      pl_cell* entry = pl_as(PL_TAG_APP, pl_app_args(row_p)[i]);
      ax_assume(entry != NULL && pl_app_head(entry) == 0 &&
                    pl_app_n(entry) == 2 && pl_is_nat(pl_app_args(entry)[0]) &&
                    pl_app_args(entry)[0] <= 1 &&
                    pl_is_nat(pl_app_args(entry)[1]),
                "er_log: malformed ReadFolder entry");
      uint64_t name_n = pl_nat_byte_len(pl_app_args(entry)[1]);
      ax_assume(name_n <= SIZE_MAX - size - 9,
                "er_log: ReadFolder result too large");
      size += 9 + (size_t)name_n;
    }
  }

  uint8_t* data = malloc(size);
  ax_assume(data != NULL, "oom");
  size_t off = 0;
  for (int b = 0; b < 8; b++)
    data[off++] = (uint8_t)((uint64_t)count >> (8 * b));
  for (uint32_t i = 0; i < count; i++) {
    pl_cell* entry = pl_as(PL_TAG_APP, pl_app_args(row_p)[i]);
    pl_val name = pl_app_args(entry)[1];
    uint64_t name_n = pl_nat_byte_len(name);
    data[off++] = (uint8_t)pl_app_args(entry)[0];
    for (int b = 0; b < 8; b++)
      data[off++] = (uint8_t)(name_n >> (8 * b));
    for (uint64_t j = 0; j < name_n; j++)
      data[off++] = pl_nat_byte_at(name, j);
  }
  ax_assume(off == size, "er_log: ReadFolder encoder size mismatch");
  *out_n = (uint64_t)size;
  return data;
}

static uint64_t er_folder_u64_read(const uint8_t** p, const uint8_t* end) {
  ax_assume((size_t)(end - *p) >= 8, "er_log: malformed ReadFolder event");
  uint64_t out = 0;
  for (int b = 0; b < 8; b++)
    out |= (uint64_t)(*p)[b] << (8 * b);
  *p += 8;
  return out;
}

/* The decoder follows the same stack-rooting discipline as the filesystem
 * producer: every name and completed entry stays rooted across a collection.
 */
static pl_val er_folder_result_build(pl_thread* t, const uint8_t* data,
                                     uint64_t data_n) {
  ax_assume(data_n <= SIZE_MAX && data_n >= 8,
            "er_log: malformed ReadFolder event");
  const uint8_t* p = data;
  const uint8_t* end = data + (size_t)data_n;
  uint64_t count64 = er_folder_u64_read(&p, end);
  ax_assume(count64 <= UINT32_MAX - 2, "er_log: ReadFolder result too large");
  uint32_t count = (uint32_t)count64;
  if (count == 0) {
    ax_assume(p == end, "er_log: malformed empty ReadFolder event");
    return 0;
  }

  size_t base = t->vsp;
  for (uint32_t i = 0; i < count; i++) {
    ax_assume(p < end, "er_log: malformed ReadFolder entry");
    uint8_t is_folder = *p++;
    ax_assume(is_folder <= 1, "er_log: malformed ReadFolder flag");
    uint64_t name_n = er_folder_u64_read(&p, end);
    ax_assume(name_n <= (uint64_t)(end - p),
              "er_log: malformed ReadFolder name");
    pl_vpush(t, is_folder);
    pl_vpush(t, pl_nat_from_bytes(t, p, (size_t)name_n));
    p += (size_t)name_n;
    pl_gc_reserve(t, PL_APP_CELLS(2));
    PL_GC_FORBID(t);
    pl_val entry = pl_mk_app_from(t, 0, 2, &t->vstack[t->vsp - 2]);
    PL_GC_ALLOW(t);
    t->vsp -= 2;
    pl_vpush(t, entry);
  }
  ax_assume(p == end, "er_log: trailing ReadFolder event bytes");
  pl_gc_reserve(t, PL_APP_CELLS(count));
  PL_GC_FORBID(t);
  pl_val row = pl_mk_app_from(t, 0, count, &t->vstack[base]);
  PL_GC_ALLOW(t);
  t->vsp = base;
  return row;
}

const er_event* er_replay_next(er_scheduler* sys) {
  ax_assume(sys->cursor < er_log_events(sys->play),
            "er_log: replay ran past the end of the recording");
  return &sys->play->ev[sys->cursor++];
}

/*
 * The pl_io_hook: every direct op-82 effect of every actor on
 * a recording or replaying system funnels through here.  Record mode
 * performs the effect and appends (actor, op, args-hash, result);
 * replay mode verifies the site against the next record and substitutes
 * the logged result without any syscall.
 */
static bool er_io_hook(pl_thread* t, uint32_t op, size_t ab, pl_val* out) {
  er_actor* a = t->host;
  if (a == NULL || a->sys->mode == ER_MODE_LIVE)
    return false;
  er_scheduler* sys = a->sys;
  uint8_t hash[32];
  er_args_hash(t, op, ab, hash);
  uint64_t name = er_mote(pl_io_name(op));

  if (sys->mode == ER_MODE_RECORD) {
    pl_val r = pl_io_run(t, op, ab);
    ax_assume(pl_is_nat(r), "er_log: direct-effect results are nats");
    er_event e = {.kind = ER_EV_IO, .actor = a->id, .op = name};
    memcpy(e.args_hash, hash, 32);
    e.data = er_nat_bytes(r, &e.data_n);
    ax_arrpush(sys->rec->ev, e);
    *out = r;
    return true;
  }

  const er_event* e = er_replay_next(sys);
  ax_assume(e->kind == ER_EV_IO && e->actor == a->id && e->op == name &&
                memcmp(e->args_hash, hash, 32) == 0,
            "er_log: replay divergence at a direct effect");
  *out = e->data_n == 0 ? 0 : pl_nat_from_bytes(t, e->data, (size_t)e->data_n);
  return true;
}

void er_scheduler_record(er_scheduler* sys, er_log* log) {
  ax_assume(sys->mode == ER_MODE_LIVE, "er_scheduler_record: mode already set");
  ax_assume(er_log_events(log) == 0, "er_scheduler_record: log not empty");
  log->quantum = sys->cfg.quantum;
  sys->mode = ER_MODE_RECORD;
  sys->rec = log;
  pl_set_io_hook(er_io_hook);
}

void er_scheduler_replay(er_scheduler* sys, const er_log* log) {
  ax_assume(sys->mode == ER_MODE_LIVE, "er_scheduler_replay: mode already set");
  ax_assume(log->quantum == sys->cfg.quantum,
            "er_scheduler_replay: quantum differs from the recording");
  ax_assume(sys->curlm == NULL && sys->http_inflight_n == 0,
            "er_scheduler_replay: live http transfers exist");
  sys->mode = ER_MODE_REPLAY;
  sys->play = log;
  sys->cursor = 0;
  pl_set_io_hook(er_io_hook);
}

size_t er_scheduler_log_cursor(const er_scheduler* sys) {
  return sys->cursor;
}

/* Injection payload encoding: a nat63 by value, a pin by hash. */
static uint64_t er_inject_encode(pl_val payload, uint8_t buf[33]) {
  if (pl_is_nat63(payload)) {
    buf[0] = 0;
    for (int i = 0; i < 8; i++)
      buf[1 + i] = (uint8_t)(payload >> (8 * i));
    return 9;
  }
  ax_assume(pl_tag(payload) == PL_TAG_PIN,
            "er_log: logged injection payloads must be nat63s or pins");
  buf[0] = 1;
  memcpy(buf + 1, pl_pin_hash(payload), 32);
  return 33;
}

void er_scheduler_inject(er_scheduler* sys, er_actor* to, pl_val payload) {
  ax_assume(pl_is_nat63(payload) || pl_store_owns(sys->store, payload),
            "er_scheduler_inject: payload must be a nat63 or store-resident");
  if (to->status == ER_ACTOR_HALTED || to->status == ER_ACTOR_CRASHED)
    return; /* a Chan nobody reads (reaver: send succeeds silently) */
  if (sys->mode == ER_MODE_RECORD) {
    uint8_t buf[33];
    er_event e = {.kind = ER_EV_INJECT, .actor = to->id};
    e.data_n = er_inject_encode(payload, buf);
    e.data = malloc((size_t)e.data_n);
    ax_assume(e.data != NULL, "oom");
    memcpy(e.data, buf, (size_t)e.data_n);
    ax_arrpush(sys->rec->ev, e);
  } else if (sys->mode == ER_MODE_REPLAY) {
    uint8_t buf[33];
    uint64_t n = er_inject_encode(payload, buf);
    const er_event* e = er_replay_next(sys);
    ax_assume(e->kind == ER_EV_INJECT && e->actor == to->id && e->data_n == n &&
                  memcmp(e->data, buf, (size_t)n) == 0,
              "er_log: replay divergence at a host injection");
  }
  er_deliver(to, payload, 0, NULL);
}

/* ── Log file round trip ───────────────────────────────────────────────── */

static const uint8_t ER_LOG_MAGIC[8] = {'e', 'n', 'k', 'i',
                                        'l', 'o', 'g', '\1'};

static bool er_wr_u64(FILE* f, uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; i++)
    b[i] = (uint8_t)(v >> (8 * i));
  return fwrite(b, 1, 8, f) == 8;
}

static bool er_rd_u64(FILE* f, uint64_t* v) {
  uint8_t b[8];
  if (fread(b, 1, 8, f) != 8)
    return false;
  *v = 0;
  for (int i = 0; i < 8; i++)
    *v |= (uint64_t)b[i] << (8 * i);
  return true;
}

bool er_log_write_file(const er_log* log, const char* path) {
  FILE* f = fopen(path, "wb");
  if (f == NULL)
    return false;
  bool ok = fwrite(ER_LOG_MAGIC, 1, 8, f) == 8 && er_wr_u64(f, log->quantum) &&
            er_wr_u64(f, er_log_events(log));
  for (ptrdiff_t i = 0; ok && i < ax_arrlen(log->ev); i++) {
    const er_event* e = &log->ev[i];
    ok = fputc(e->kind, f) != EOF && er_wr_u64(f, e->actor) &&
         er_wr_u64(f, e->op) && fwrite(e->args_hash, 1, 32, f) == 32 &&
         er_wr_u64(f, e->data_n) &&
         (e->data_n == 0 ||
          fwrite(e->data, 1, (size_t)e->data_n, f) == (size_t)e->data_n);
  }
  return fclose(f) == 0 && ok;
}

er_log* er_log_read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f == NULL)
    return NULL;
  er_log* log = er_log_new();
  uint8_t magic[8];
  uint64_t count = 0;
  bool ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, ER_LOG_MAGIC, 8) == 0 &&
            er_rd_u64(f, &log->quantum) && er_rd_u64(f, &count);
  for (uint64_t i = 0; ok && i < count; i++) {
    er_event e = {0};
    int kind = fgetc(f);
    ok = kind != EOF && er_rd_u64(f, &e.actor) && er_rd_u64(f, &e.op) &&
         fread(e.args_hash, 1, 32, f) == 32 && er_rd_u64(f, &e.data_n);
    if (ok && e.data_n > 0) {
      e.data = malloc((size_t)e.data_n);
      ax_assume(e.data != NULL, "oom");
      ok = fread(e.data, 1, (size_t)e.data_n, f) == (size_t)e.data_n;
    }
    if (!ok) {
      free(e.data);
      break;
    }
    e.kind = (uint8_t)kind;
    ax_arrpush(log->ev, e);
  }
  fclose(f);
  if (!ok) {
    er_log_free(log);
    return NULL;
  }
  return log;
}
