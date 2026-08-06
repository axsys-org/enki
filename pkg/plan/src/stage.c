/*
 * Staged compilation: running the optimizing compiler off the main thread.
 *
 * The compiler that runs at intern time sits directly in the latency path of
 * every pin — it must be cheap, which bounds how much optimization it can do.
 * The optimizer is the opposite trade: worth minutes of CPU on a law that
 * will run for hours.  Staging separates the two.  A law is compiled by the
 * fast tier the moment it is interned, so it never runs interpreted, and is
 * queued here; a worker recompiles it with the optimizing compiler and swaps
 * the result in.  Both tiers must compile a law to the same meaning, so the
 * upgrade is invisible except in speed.
 *
 * The worker is an ordinary PLAN machine (its own heap and thread) against
 * the shared store, exactly like an MT actor.  Three rules keep it out of
 * the main thread's way:
 *
 *   - it never holds save_mu while the compiler runs, only around the store
 *     steps (load, snapshot, cache write) that need it;
 *   - it never holds the stage lock while taking save_mu, so a quiesce from
 *     the interning thread cannot deadlock against a compile;
 *   - it runs the compiler in fuel-bounded quanta, so shutdown and compiler
 *     replacement interrupt it at the next safepoint instead of waiting out
 *     a compile.
 *
 * Publication is pl_pin_set_code: one release store to a word the evaluator
 * reads with acquire on every law entry.  A reader sees either generation,
 * both correct, and retired code is owned by the store until teardown.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#endif

#include "axsys/assume.h"
#include "axsys/ds.h"
#include "axsys/util.h"
#include "internal.h"
#include "plan/eval.h"
#include "plan/heap.h"
#include "plan/store.h"
#include "store_internal.h"

/* Cells per semispace of a worker's heap.  The optimizer allocates hard:
 * this matches the interning compiler's machine. */
#define PL_STAGE_HEAP_CELLS (((size_t)1) << 26)

/* Machine steps between stop checks.  Small enough that process exit is
 * prompt, large enough that the check is noise. */
#define PL_STAGE_QUANTUM (((uint64_t)1) << 20)

#define PL_STAGE_MAX_WORKERS 8

typedef struct pl_stage_seen {
  pl_hash key;
  bool value;
} pl_stage_seen;

typedef struct pl_stage_worker {
  struct pl_stage* stage;
  pthread_t thread;
  pl_heap* heap;
  pl_thread* t;
  bool live;
  bool busy;
} pl_stage_worker;

/* Set for the duration of a worker thread.  Quiescing the stage from a
 * worker would wait for that worker to go idle — for itself — so the ops
 * that quiesce refuse to run there. */
static _Thread_local pl_stage_worker* pl_stage_current;

struct pl_stage {
  pl_store* s;
  pthread_mutex_t mu;
  pthread_cond_t work_cv; /* queued work, or stop */
  pthread_cond_t idle_cv; /* queue drained and no worker busy */
  pl_hash* queue;         /* FIFO ring: entries [head, arrlen) are pending */
  size_t head;
  pl_stage_seen* seen; /* queued-or-done, per optimizing generation */
  pl_stage_worker workers[PL_STAGE_MAX_WORKERS];
  size_t worker_n;
  size_t busy_n;
  bool stop;    /* workers exit at the next safepoint */
  bool quiesce; /* workers abandon the current compile and idle */
  uint64_t queued, upgraded, served, failed;
};

/* ── Configuration ─────────────────────────────────────────────────────── */

static size_t stage_worker_count(void) {
  const char* env = getenv("PL_STAGE_WORKERS");
  if (env == NULL || env[0] == '\0')
    return 1;
  long n = strtol(env, NULL, 10);
  if (n < 0)
    n = 0;
  if (n > PL_STAGE_MAX_WORKERS)
    n = PL_STAGE_MAX_WORKERS;
  return (size_t)n;
}

static bool stage_stats_wanted(void) {
  const char* env = getenv("PL_STAGE_STATS");
  return env != NULL && strcmp(env, "1") == 0;
}

/* Upgrades are less urgent than the program being upgraded: ask the OS to
 * schedule them behind it rather than competing for a core with the thread
 * whose latency this whole design is protecting.  Not the lowest band,
 * though — an upgrade that lands after the program has moved on is wasted
 * work, and on Apple silicon QOS_CLASS_BACKGROUND means efficiency cores
 * only (measured ~2.3x slower to compile a corpus). */
static void stage_lower_priority(void) {
#ifdef __APPLE__
  (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
  /* Linux nice values are per-thread; `who = 0` means the calling thread. */
  (void)setpriority(PRIO_PROCESS, 0, 5);
#endif
}

/* ── Queue ─────────────────────────────────────────────────────────────── */

static bool stage_queue_empty(const pl_stage* g) {
  return g->head >= (size_t)ax_arrlen(g->queue);
}

/* Drop the drained prefix once it dominates the array. */
static void stage_queue_compact(pl_stage* g) {
  size_t n = (size_t)ax_arrlen(g->queue);
  if (g->head < 64 || g->head * 2 < n)
    return;
  size_t left = n - g->head;
  if (left > 0)
    memmove(g->queue, g->queue + g->head, left * sizeof(*g->queue));
  ax_arrsetlen(g->queue, (ptrdiff_t)left);
  g->head = 0;
}

static void stage_queue_clear(pl_stage* g) {
  ax_arrfree(g->queue);
  g->queue = NULL;
  g->head = 0;
  ax_hmfree(g->seen);
  g->seen = NULL;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

static void* stage_worker_main(void* arg);

/* Create the stage on first use.  Called with no lock held; the store lock
 * serializes creation. */
static pl_stage* stage_get(pl_store* s, bool create) {
  pl_store_lock(s);
  pl_stage* g = s->stage;
  if (g == NULL && create) {
    /* PL_STAGE_WORKERS=0 disables staging; so does PL_NO_BYTECODE, where the
     * only product of a compile is refused at the decoder anyway. */
    size_t workers = pl_bytecode_enabled() ? stage_worker_count() : 0;
    if (workers == 0) {
      pl_store_unlock(s);
      return NULL;
    }
    g = calloc(1, sizeof(*g));
    ax_assume(g != NULL, "oom");
    g->s = s;
    ax_assume(pthread_mutex_init(&g->mu, NULL) == 0,
              "stage pthread_mutex_init");
    ax_assume(pthread_cond_init(&g->work_cv, NULL) == 0,
              "stage pthread_cond_init");
    ax_assume(pthread_cond_init(&g->idle_cv, NULL) == 0,
              "stage pthread_cond_init");
    g->worker_n = workers;
    s->stage = g;
    for (size_t i = 0; i < workers; i++) {
      pl_stage_worker* w = &g->workers[i];
      w->stage = g;
      w->heap = pl_heap_new(PL_STAGE_HEAP_CELLS, s);
      w->t = pl_thread_new(w->heap);
      ax_assume(pthread_create(&w->thread, NULL, stage_worker_main, w) == 0,
                "stage pthread_create");
      w->live = true;
    }
  }
  pl_store_unlock(s);
  return g;
}

static void stage_print_stats(pl_stage* g) {
  fprintf(stderr,
          "plan: staged compiles queued=%" PRIu64 " upgraded=%" PRIu64
          " cached=%" PRIu64 " failed=%" PRIu64 "\n",
          g->queued, g->upgraded, g->served, g->failed);
}

void pl_stage_shutdown(pl_store* s) {
  pl_store_lock(s);
  pl_stage* g = s->stage;
  s->stage = NULL;
  pl_store_unlock(s);
  if (g == NULL)
    return;

  pthread_mutex_lock(&g->mu);
  g->stop = true;
  pthread_cond_broadcast(&g->work_cv);
  pthread_mutex_unlock(&g->mu);
  for (size_t i = 0; i < g->worker_n; i++) {
    pl_stage_worker* w = &g->workers[i];
    if (!w->live)
      continue;
    (void)pthread_join(w->thread, NULL);
    w->live = false;
    /* A worker that stopped mid-compile leaves its machine suspended: the
     * whole continuation is in these two objects, so dropping them drops
     * the compile.  Nothing outside the heap references it. */
    pl_thread_free(w->t);
    pl_heap_free(w->heap);
  }
  if (stage_stats_wanted())
    stage_print_stats(g);
  stage_queue_clear(g);
  pthread_cond_destroy(&g->idle_cv);
  pthread_cond_destroy(&g->work_cv);
  pthread_mutex_destroy(&g->mu);
  free(g);
}

bool pl_stage_in_worker(void) {
  return pl_stage_current != NULL;
}

void pl_stage_reset(pl_store* s) {
  pl_stage* g = stage_get(s, false);
  if (g == NULL)
    return;
  ax_assume(pl_stage_current == NULL,
            "stage reset from a staging worker would wait for itself");
  pthread_mutex_lock(&g->mu);
  g->quiesce = true;
  stage_queue_clear(g);
  pthread_cond_broadcast(&g->work_cv);
  while (g->busy_n > 0)
    pthread_cond_wait(&g->idle_cv, &g->mu);
  g->quiesce = false;
  pthread_mutex_unlock(&g->mu);
}

/* ── Submission ────────────────────────────────────────────────────────── */

void pl_stage_enqueue(pl_store* s, const uint8_t law_hash[32]) {
  pl_stage* g = stage_get(s, true);
  if (g == NULL)
    return;
  pl_hash key;
  memcpy(key.b, law_hash, sizeof(key.b));
  pthread_mutex_lock(&g->mu);
  if (ax_hmgeti(g->seen, key) < 0) {
    ax_hmput(g->seen, key, true);
    ax_arrpush(g->queue, key);
    g->queued++;
    pthread_cond_signal(&g->work_cv);
  }
  pthread_mutex_unlock(&g->mu);
}

void pl_stage_mark_done(pl_store* s, const uint8_t law_hash[32]) {
  pl_stage* g = stage_get(s, false);
  if (g == NULL)
    return;
  pl_hash key;
  memcpy(key.b, law_hash, sizeof(key.b));
  pthread_mutex_lock(&g->mu);
  if (ax_hmgeti(g->seen, key) < 0) {
    ax_hmput(g->seen, key, true);
    g->served++;
  }
  pthread_mutex_unlock(&g->mu);
}

/* ── The upgrade ───────────────────────────────────────────────────────── */

typedef enum {
  STAGE_UPGRADED = 0,
  STAGE_SERVED,   /* the persistent cache already had the row */
  STAGE_FAILED,   /* the compiler raised, blocked, or produced no code */
  STAGE_ABANDONED /* stop or quiesce interrupted the compile */
} stage_result;

/*
 * Run the optimizing compiler over one law.  save_mu covers the store steps
 * only: the compile itself is the long pole and must not block Save, Load,
 * or the interning compiler while it runs.
 */
static stage_result stage_compile_one(pl_stage* g, pl_stage_worker* w,
                                      pl_hash key) {
  pl_store* s = g->s;
  pl_thread* t = w->t;

  pl_store_lock(s);
  bool active = s->opt_f && s->compiler_f;
  uint8_t opt_hash[32];
  memcpy(opt_hash, s->opt_compiler, sizeof(opt_hash));
  pl_store_unlock(s);
  if (!active)
    return STAGE_ABANDONED;
  if (pl_store_code_have(s, key.b, true))
    return STAGE_SERVED;

  uint8_t cache_key[32];
  pl_store_code_key(opt_hash, key.b, cache_key);

  /* written inside the setjmp region and read after it */
  volatile pl_val compiler = 0;
  volatile pl_val law = 0;
  pl_store_save_lock(s);
  bool served = pl_store_code_serve(s, t, key.b, cache_key, true);
  if (!served) {
    /* Both pins are canonical store objects: terminal for this heap's
     * collector, so they need no rooting across the run below. */
    pl_catch c;
    pl_catch_init(t, &c);
    if (setjmp(c.jb) != 0) {
      pl_catch_unwind(t, &c);
      compiler = 0;
    } else {
      compiler = pl_store_load(t, opt_hash);
      law = pl_store_load(t, key.b);
      pl_catch_pop(t, &c);
    }
  }
  pl_store_save_unlock(s);
  if (served)
    return STAGE_SERVED;
  if (compiler == 0 || law == 0)
    return STAGE_FAILED;

  PL_STORE_PROFILE("store.compile.upgrade");
  pl_thread_start_call_nf(t, compiler, law);
  pl_run_status st;
  for (;;) {
    st = pl_thread_run(t, PL_STAGE_QUANTUM);
    if (st != PL_RUN_YIELDED)
      break;
    pthread_mutex_lock(&g->mu);
    bool interrupted = g->stop || g->quiesce;
    pthread_mutex_unlock(&g->mu);
    if (interrupted) {
      pl_thread_abandon(t);
      return STAGE_ABANDONED;
    }
  }
  if (st == PL_RUN_BLOCKED) {
    /* a compiler that initiates an effect has no executor here */
    pl_thread_abandon(t);
    return STAGE_FAILED;
  }
  if (st != PL_RUN_DONE)
    return STAGE_FAILED; /* EXN: pl_thread_run unwound the stacks */

  stage_result out = STAGE_FAILED;
  size_t row_at = t->vsp;
  pl_vpush(t, pl_thread_result(t));
  pl_store_save_lock(s);
  /* Decoded OP_PUSH_LIT operands borrow pl_val pointers, so the row has to
   * cross into the store before the decoder sees it (this heap moves). */
  pl_val stable = pl_store_snapshot_normal(t, t->vstack[row_at]);
  t->vstack[row_at] = stable;
  pl_code* code = pl_bytecode_from_val(stable);
  if (code != NULL) {
    pl_store_code_publish(s, key.b, code, true);
    out = STAGE_UPGRADED;
  }
  /* Cache the row even when it did not decode: the compile would only
   * reproduce it.  Optimizing compiles always clear the write threshold
   * the fast tier is gated on. */
  pl_store_code_persist(s, t, row_at, cache_key);
  pl_store_save_unlock(s);
  t->vsp = row_at;
  return out;
}

static void* stage_worker_main(void* arg) {
  pl_stage_worker* w = arg;
  pl_stage* g = w->stage;
  pl_stage_current = w;
  stage_lower_priority();

  pthread_mutex_lock(&g->mu);
  for (;;) {
    while (!g->stop && (g->quiesce || stage_queue_empty(g)))
      pthread_cond_wait(&g->work_cv, &g->mu);
    if (g->stop)
      break;
    pl_hash key = g->queue[g->head++];
    stage_queue_compact(g);
    w->busy = true;
    g->busy_n++;
    pthread_mutex_unlock(&g->mu);

    stage_result r = stage_compile_one(g, w, key);

    pthread_mutex_lock(&g->mu);
    switch (r) {
    case STAGE_UPGRADED:
      g->upgraded++;
      break;
    case STAGE_SERVED:
      g->served++;
      break;
    case STAGE_FAILED:
      g->failed++;
      break;
    case STAGE_ABANDONED:
      break;
    }
    w->busy = false;
    g->busy_n--;
    if (g->busy_n == 0 && (stage_queue_empty(g) || g->quiesce))
      pthread_cond_broadcast(&g->idle_cv);
  }
  pthread_mutex_unlock(&g->mu);
  return NULL;
}

/* ── Public surface ────────────────────────────────────────────────────── */

bool pl_store_stage_drain(pl_store* s) {
  pl_stage* g = stage_get(s, false);
  if (g == NULL)
    return false;
  pthread_mutex_lock(&g->mu);
  while (!g->stop && (!stage_queue_empty(g) || g->busy_n > 0))
    pthread_cond_wait(&g->idle_cv, &g->mu);
  pthread_mutex_unlock(&g->mu);
  return true;
}

void pl_store_stage_stats(pl_store* s, uint64_t* queued, uint64_t* upgraded,
                          uint64_t* served, uint64_t* failed) {
  pl_stage* g = stage_get(s, false);
  uint64_t q = 0, u = 0, c = 0, f = 0;
  if (g != NULL) {
    pthread_mutex_lock(&g->mu);
    q = g->queued;
    u = g->upgraded;
    c = g->served;
    f = g->failed;
    pthread_mutex_unlock(&g->mu);
  }
  if (queued != NULL)
    *queued = q;
  if (upgraded != NULL)
    *upgraded = u;
  if (served != NULL)
    *served = c;
  if (failed != NULL)
    *failed = f;
}
