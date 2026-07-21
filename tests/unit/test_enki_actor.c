#include <criterion/criterion.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "axsys/fd.h"
#include "enki/actor.h"
#include "test_plan.h"

/* Configuration assertions intentionally inspect the scheduler's resolved
 * values; execution behavior remains covered through the public API below. */
#include "../../pkg/enki/src/actor_internal.h"

/*
 * Actor runtime: er_scheduler drives er_actors — each one
 * deep normalization of (fn 0) — servicing the op-82 coordination
 * effects parked by PL_RUN_BLOCKED.
 *
 * Test programs are arity-1 laws whose body code applies the op-82 pin
 * to effect rows.  In KAL body code under the 2-slot env [self, arg]:
 * (0 f x) is application (operands deferred as thunks), (0 v) is a
 * literal, and nats >= 2 are self-literals.  Effect rows are hand-built
 * APP spines (data by the under-application invariant); rows built *by*
 * the program arise from applying name nats, which have arity 0 and so
 * also accumulate as data.
 *
 * GC discipline: a heap pl_val in a C local must not be a sibling
 * argument of another allocating call — every intermediate is parked on
 * the value stack and re-read from its slot.
 */

static pl_val code_lit(pl_thread* t, pl_val v) {
  return test_app1(t, 0, v);
}

static pl_val code_app(pl_thread* t, pl_val f, pl_val x) {
  return test_app2(t, 0, f, x);
}

/* The pin of nat 82 (rplan I/O). */
static pl_val test_p82(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, 82);
  pl_val pin = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return pin;
}

/* The pin of nat 83 (structured request/response drivers). */
static pl_val test_p83(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, 83);
  pl_val pin = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return pin;
}

/* Code performing (P82 % row) for a hand-built effect row. */
static pl_val code_effect(pl_thread* t, pl_val name, size_t n,
                          const pl_val* args) {
  size_t base = t->vsp;
  pl_vpush(t, code_lit(t, test_app(t, name, n, args)));
  pl_vpush(t, code_lit(t, test_p82(t)));
  pl_val out = code_app(t, t->vstack[base + 1], t->vstack[base]);
  t->vsp = base;
  return out;
}

/* Code performing (P83 % row) for a hand-built effect row. */
static pl_val code_effect83(pl_thread* t, pl_val name, size_t n,
                            const pl_val* args) {
  size_t base = t->vsp;
  pl_vpush(t, code_lit(t, test_app(t, name, n, args)));
  pl_vpush(t, code_lit(t, test_p83(t)));
  pl_val out = code_app(t, t->vstack[base + 1], t->vstack[base]);
  t->vsp = base;
  return out;
}

/* Code sequencing a then b: (P66 % (Seq a b)), Seq strict only in a. */
static pl_val code_seq(pl_thread* t, pl_val a, pl_val b) {
  size_t base = t->vsp;
  pl_vpush(t, a);
  pl_vpush(t, b);
  pl_vpush(t, code_lit(t, ax_s3('S', 'e', 'q')));
  pl_vpush(t, code_app(t, t->vstack[base + 2], t->vstack[base]));
  pl_vpush(t, code_app(t, t->vstack[base + 3], t->vstack[base + 1]));
  pl_vpush(t, code_lit(t, test_p66(t)));
  pl_val out = code_app(t, t->vstack[base + 5], t->vstack[base + 4]);
  t->vsp = base;
  return out;
}

/* An arity-1 law (the actor boot fn) with the given body code. */
static pl_val actor_fn(pl_thread* t, pl_val body) {
  return test_law(t, 1, 0, body);
}

static pl_val recv_code(pl_thread* t) {
  pl_val args[1] = {0};
  return code_effect(t, ax_s4('R', 'e', 'c', 'v'), 1, args);
}

static pl_val sleep_code(pl_thread* t, uint64_t seconds) {
  pl_val args[1] = {seconds};
  return code_effect83(t, ax_s5('S', 'l', 'e', 'e', 'p'), 1, args);
}

static double monotonic_seconds(void) {
  struct timespec ts;
  cr_assert_eq(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

typedef struct mt_run_task {
  er_mt_executor* ex;
  er_run_reason reason;
} mt_run_task;

static void* mt_run_task_main(void* arg) {
  mt_run_task* task = arg;
  task->reason = er_mt_executor_run(task->ex);
  return NULL;
}

static bool nat_text_eq(pl_val value, const char* text) {
  size_t n = strlen(text);
  if (!pl_is_nat(value) || pl_nat_byte_len(value) != n)
    return false;
  for (size_t i = 0; i < n; i++)
    if (pl_nat_byte_at(value, i) != (uint8_t)text[i])
      return false;
  return true;
}

static pl_cell* folder_entry(pl_val row, const char* name) {
  pl_cell* row_p = pl_as(PL_TAG_APP, row);
  if (row_p == NULL || pl_app_head(row_p) != 0)
    return NULL;
  for (uint32_t i = 0; i < pl_app_n(row_p); i++) {
    pl_cell* entry = pl_as(PL_TAG_APP, pl_app_args(row_p)[i]);
    if (entry != NULL && pl_app_head(entry) == 0 && pl_app_n(entry) == 2 &&
        nat_text_eq(pl_app_args(entry)[1], name))
      return entry;
  }
  return NULL;
}

Test(actor, readfolder_lists_entries_and_honors_file_root) {
  char dir[] = "/tmp/enki-read-folder-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  char root[512], listing[512], child[512], file[512];
  int n = snprintf(root, sizeof(root), "%s/files", dir);
  cr_assert(n >= 0 && (size_t)n < sizeof(root));
  n = snprintf(listing, sizeof(listing), "%s/listing", root);
  cr_assert(n >= 0 && (size_t)n < sizeof(listing));
  n = snprintf(child, sizeof(child), "%s/subdir", listing);
  cr_assert(n >= 0 && (size_t)n < sizeof(child));
  n = snprintf(file, sizeof(file), "%s/plain.txt", listing);
  cr_assert(n >= 0 && (size_t)n < sizeof(file));
  cr_assert_eq(mkdir(root, 0700), 0);
  cr_assert_eq(mkdir(listing, 0700), 0);
  cr_assert_eq(mkdir(child, 0700), 0);
  FILE* f = fopen(file, "wb");
  cr_assert_not_null(f);
  fputs("contents", f);
  fclose(f);

  test_rt rt = test_rt_new();
  er_scheduler* sys =
      er_scheduler_new(rt.store, (er_config){.file_root_c = root});
  er_actor* inside = er_scheduler_actor(sys);
  pl_thread* inside_t = er_actor_thread(inside);
  size_t base = inside_t->vsp;
  pl_vpush(inside_t,
           pl_nat_from_bytes(inside_t, (const uint8_t*)"ReadFolder", 10));
  pl_vpush(inside_t, pl_nat_from_bytes(inside_t, (const uint8_t*)"listing", 7));
  pl_val args[1] = {inside_t->vstack[base + 1]};
  pl_val body = code_effect83(inside_t, inside_t->vstack[base], 1, args);
  inside_t->vsp = base;
  er_actor_start(inside, actor_fn(inside_t, body));

  er_actor* outside = er_scheduler_actor(sys);
  pl_thread* outside_t = er_actor_thread(outside);
  base = outside_t->vsp;
  pl_vpush(outside_t,
           pl_nat_from_bytes(outside_t, (const uint8_t*)"ReadFolder", 10));
  pl_vpush(outside_t, pl_nat_from_bytes(outside_t, (const uint8_t*)"../", 3));
  args[0] = outside_t->vstack[base + 1];
  body = code_effect83(outside_t, outside_t->vstack[base], 1, args);
  outside_t->vsp = base;
  er_actor_start(outside, actor_fn(outside_t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(inside), ER_ACTOR_HALTED);
  pl_val row = er_actor_result(inside);
  pl_cell* row_p = pl_as(PL_TAG_APP, row);
  cr_assert_not_null(row_p);
  cr_assert_eq(pl_app_head(row_p), 0);
  cr_assert_eq(pl_app_n(row_p), 2);
  pl_cell* subdir = folder_entry(row, "subdir");
  cr_assert_not_null(subdir);
  cr_assert_eq(pl_app_args(subdir)[0], 1);
  pl_cell* plain = folder_entry(row, "plain.txt");
  cr_assert_not_null(plain);
  cr_assert_eq(pl_app_args(plain)[0], 0);

  cr_assert_eq(er_actor_state(outside), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(outside), 0);
  er_scheduler_free(sys);
  test_rt_free(&rt);

  cr_assert_eq(unlink(file), 0);
  cr_assert_eq(rmdir(child), 0);
  cr_assert_eq(rmdir(listing), 0);
  cr_assert_eq(rmdir(root), 0);
  cr_assert_eq(rmdir(dir), 0);
}

Test(actor, single_actor_halts_with_result) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  er_actor_start(a, actor_fn(t, 7)); /* body: self-literal 7 */
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(a), 7);
  cr_assert_eq(er_actor_id(a), 0);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, crash_is_isolated) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  er_actor* b = er_scheduler_actor(sys);
  {
    /* a: (P66 % (Throw 9)) */
    pl_thread* t = er_actor_thread(a);
    size_t base = t->vsp;
    pl_vpush(t, code_lit(t, ax_s5('T', 'h', 'r', 'o', 'w')));
    pl_vpush(t, code_lit(t, 9));
    pl_vpush(t, code_app(t, t->vstack[base], t->vstack[base + 1]));
    pl_vpush(t, code_lit(t, test_p66(t)));
    pl_val body = code_app(t, t->vstack[base + 3], t->vstack[base + 2]);
    t->vsp = base;
    er_actor_start(a, actor_fn(t, body));
  }
  {
    pl_thread* t = er_actor_thread(b);
    er_actor_start(b, actor_fn(t, 7));
  }
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_CRASHED);
  cr_assert_eq(er_actor_state(b), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(b), 7);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

/* Send 42 to self (handle 0), then Recv it: result [42, 0]. */
static void run_self_ping(pl_store* store, uint64_t quantum) {
  er_scheduler* sys = er_scheduler_new(store, (er_config){.quantum = quantum});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  size_t base = t->vsp;
  pl_val sargs[2] = {0, 42};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_vpush(t, recv_code(t));
  pl_val body = code_seq(t, t->vstack[base], t->vstack[base + 1]);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_head(r), 0);
  cr_assert_eq(pl_app_n(r), 2);
  cr_assert_eq(pl_app_args(r)[0], 42);
  cr_assert_eq(pl_app_args(r)[1], 0); /* no caps: the empty row is 0 */
  er_scheduler_free(sys);
}

Test(actor, send_to_self_then_recv) {
  test_rt rt = test_rt_new();
  run_self_ping(rt.store, 0);
  test_rt_free(&rt);
}

Test(actor, mt_multiple_actors_halt) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){.quantum = 2});
  er_actor* a = er_scheduler_actor(sys);
  er_actor* b = er_scheduler_actor(sys);
  er_actor_start(a, actor_fn(er_actor_thread(a), 7));
  er_actor_start(b, actor_fn(er_actor_thread(b), 9));

  er_mt_executor* ex = er_mt_executor_new(sys, (er_mt_config){.workers = 2});
  cr_assert_eq(er_mt_executor_run(ex), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(a), 7);
  cr_assert_eq(er_actor_state(b), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(b), 9);
  er_mt_executor_free(ex);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, mt_bound_spawn_wakes_general_worker_without_broadcast,
     .timeout = 4) {
  int pipe_fd[2];
  cr_assert_eq(pipe(pipe_fd), 0);
  size_t read_handle = ax_fd_add(pipe_fd[0]);
  size_t write_handle = ax_fd_add(pipe_fd[1]);
  cr_assert_neq(read_handle, AX_FD_INVALID);
  cr_assert_neq(write_handle, AX_FD_INVALID);

  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* parent = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(parent);
  size_t base = t->vsp;
  pl_vpush(t, recv_code(t));
  pl_val read_args[2] = {read_handle, 1};
  pl_vpush(t, code_effect(t, ax_s4('R', 'e', 'a', 'd'), 2, read_args));
  pl_vpush(t, actor_fn(t, 7));
  pl_val spawn_args[1] = {t->vstack[base + 2]};
  pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spawn_args));
  pl_vpush(t, code_seq(t, t->vstack[base + 1], t->vstack[base + 3]));
  pl_val body = code_seq(t, t->vstack[base], t->vstack[base + 4]);
  t->vsp = base;
  er_actor_start(parent, actor_fn(t, body));

  /* Recv binds the parent to the only original worker and leaves the
   * replacement general worker parked between executor generations. */
  er_mt_executor* ex = er_mt_executor_new(sys, (er_mt_config){.workers = 1});
  cr_assert_eq(er_mt_executor_run(ex), ER_RUN_QUIESCENT);
  cr_assert_eq(er_actor_state(parent), ER_ACTOR_BLOCKED);

  /* On the second generation the replacement runs sentinel while the bound
   * parent waits in Read.  The general worker holds sys->mu continuously from
   * marking sentinel HALTED until it waits on sys->cv, so observing HALTED
   * under that mutex proves the worker has reached the shared-cv wait. */
  er_actor* sentinel = er_scheduler_actor(sys);
  er_actor_start(sentinel, actor_fn(er_actor_thread(sentinel), 9));
  er_scheduler_inject(sys, parent, 123);
  mt_run_task task = {.ex = ex};
  pthread_t runner;
  cr_assert_eq(pthread_create(&runner, NULL, mt_run_task_main, &task), 0);

  for (;;) {
    pthread_mutex_lock(&sys->mu);
    if (sentinel->status == ER_ACTOR_HALTED)
      break;
    pthread_mutex_unlock(&sys->mu);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    (void)nanosleep(&pause, NULL);
  }
  cr_assert(ex->running);
  cr_assert_null(sys->qhead);
  uint8_t byte = 42;
  cr_assert_eq(write(pipe_fd[1], &byte, 1), 1);
  pthread_mutex_unlock(&sys->mu);

  cr_assert_eq(pthread_join(runner, NULL), 0);
  cr_assert_eq(task.reason, ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(parent), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(parent), 1);
  cr_assert_eq(er_actor_state(sentinel), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(sentinel), 9);
  er_actor* child = er_scheduler_actor_by_id(sys, 2);
  cr_assert_not_null(child);
  cr_assert_eq(er_actor_state(child), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(child), 7);

  er_mt_executor_free(ex);
  er_scheduler_free(sys);
  test_rt_free(&rt);
  cr_assert_eq(ax_fd_close(read_handle), 0);
  cr_assert_eq(ax_fd_close(write_handle), 0);
}

Test(actor, root_quantum_config_environment_and_precedence) {
  const char* old_c = getenv("ENKI_ROOT_QUANTUM");
  char* old = old_c == NULL ? NULL : strdup(old_c);
  cr_assert(old_c == NULL || old != NULL);
  cr_assert_eq(unsetenv("ENKI_ROOT_QUANTUM"), 0);

  test_rt rt = test_rt_new();
  er_scheduler* inherited =
      er_scheduler_new(rt.store, (er_config){.quantum = 17});
  cr_assert_eq(inherited->cfg.quantum, 17);
  cr_assert_eq(inherited->cfg.root_quantum, 17);
  er_scheduler_free(inherited);

  cr_assert_eq(setenv("ENKI_ROOT_QUANTUM", "257", 1), 0);
  er_scheduler* from_env =
      er_scheduler_new(rt.store, (er_config){.quantum = 17});
  cr_assert_eq(from_env->cfg.quantum, 17);
  cr_assert_eq(from_env->cfg.root_quantum, 257);
  er_scheduler_free(from_env);

  er_scheduler* explicit = er_scheduler_new(
      rt.store, (er_config){.quantum = 17, .root_quantum = 33});
  cr_assert_eq(explicit->cfg.quantum, 17);
  cr_assert_eq(explicit->cfg.root_quantum, 33);
  er_scheduler_free(explicit);
  test_rt_free(&rt);

  if (old != NULL) {
    cr_assert_eq(setenv("ENKI_ROOT_QUANTUM", old, 1), 0);
    free(old);
  } else {
    cr_assert_eq(unsetenv("ENKI_ROOT_QUANTUM"), 0);
  }
}

Test(actor, mt_blocking_effect_replenishes_shared_pool, .timeout = 4) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  er_actor* b = er_scheduler_actor(sys);
  pl_thread* at = er_actor_thread(a);
  pl_thread* bt = er_actor_thread(b);
  er_actor_start(a, actor_fn(at, sleep_code(at, 1)));
  er_actor_start(b, actor_fn(bt, sleep_code(bt, 1)));

  /* With one shared worker and no executor affinity/compensation these
   * sleeps serialize and take at least two seconds.  Each actor's first
   * RPLAN effect instead claims its worker and immediately replenishes the
   * shared pool, allowing both blocking syscalls to overlap. */
  er_mt_executor* ex = er_mt_executor_new(sys, (er_mt_config){.workers = 1});
  double before = monotonic_seconds();
  cr_assert_eq(er_mt_executor_run(ex), ER_RUN_IDLE);
  double elapsed = monotonic_seconds() - before;
  cr_assert_lt(elapsed, 1.95,
               "blocking effects serialized across the shared pool: %.3fs",
               elapsed);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_state(b), ER_ACTOR_HALTED);

  er_mt_executor_free(ex);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, mt_direct_read_replenishes_before_blocking, .timeout = 4) {
  int pipe_fd[2];
  cr_assert_eq(pipe(pipe_fd), 0);
  size_t read_handle = ax_fd_add(pipe_fd[0]);
  size_t write_handle = ax_fd_add(pipe_fd[1]);
  cr_assert_neq(read_handle, AX_FD_INVALID);
  cr_assert_neq(write_handle, AX_FD_INVALID);

  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* reader = er_scheduler_actor(sys);
  er_actor* writer = er_scheduler_actor(sys);
  pl_thread* reader_t = er_actor_thread(reader);
  pl_thread* writer_t = er_actor_thread(writer);
  pl_val read_args[2] = {read_handle, 1};
  pl_val write_args[2] = {write_handle, ((pl_val)1 << 8) | 42};
  er_actor_start(
      reader,
      actor_fn(reader_t,
               code_effect(reader_t, ax_s4('R', 'e', 'a', 'd'), 2, read_args)));
  er_actor_start(
      writer,
      actor_fn(writer_t, code_effect(writer_t, ax_s5('W', 'r', 'i', 't', 'e'),
                                     2, write_args)));

  /* The reader is first in the queue and blocks in read(2).  With one
   * shared worker, the writer can run only if the effect hook binds the
   * reader and replenishes the pool before entering the direct-op body. */
  er_mt_executor* ex = er_mt_executor_new(sys, (er_mt_config){.workers = 1});
  cr_assert_eq(er_mt_executor_run(ex), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(reader), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(reader), ((pl_val)1 << 8) | 42);
  cr_assert_eq(er_actor_state(writer), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(writer), 0);

  er_mt_executor_free(ex);
  er_scheduler_free(sys);
  test_rt_free(&rt);
  cr_assert_eq(ax_fd_close(read_handle), 0);
  cr_assert_eq(ax_fd_close(write_handle), 0);
}

Test(actor, mt_recv_blocks_until_injection) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  er_actor_start(a, actor_fn(t, recv_code(t)));

  er_mt_executor* ex = er_mt_executor_new(sys, (er_mt_config){.workers = 2});
  cr_assert_eq(er_mt_executor_run(ex), ER_RUN_QUIESCENT);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_BLOCKED);

  er_scheduler_inject(sys, a, 123);
  cr_assert_eq(er_mt_executor_run(ex), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 123);
  er_mt_executor_free(ex);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, mt_free_detaches_bound_actor) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  er_actor_start(a, actor_fn(t, recv_code(t)));

  er_mt_executor* ex = er_mt_executor_new(sys, (er_mt_config){.workers = 1});
  cr_assert_eq(er_mt_executor_run(ex), ER_RUN_QUIESCENT);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_BLOCKED);
  er_mt_executor_free(ex); /* releases the actor's private worker affinity */

  er_scheduler_inject(sys, a, 77);
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 77);

  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, results_independent_of_quantum) {
  /* results must not depend on the quantum: one step per quantum and a huge
   * quantum agree */
  test_rt rt = test_rt_new();
  run_self_ping(rt.store, 2);
  run_self_ping(rt.store, 1 << 20);
  test_rt_free(&rt);
}

Test(actor, tiny_heaps_collect_through_service) {
  /* 256-cell semispaces force collection (and growth) inside pinning
   * and response building — a GC-pressure pass over the service paths
   * without the PL_GC_STRESS build flag. */
  test_rt rt = test_rt_new();
  er_scheduler* sys =
      er_scheduler_new(rt.store, (er_config){.quantum = 2, .heap_cells = 256});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  size_t base = t->vsp;
  pl_val sargs[2] = {0, 42};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_vpush(t, recv_code(t));
  pl_val body = code_seq(t, t->vstack[base], t->vstack[base + 1]);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 42);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, recv_blocks_until_injection) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  er_actor_start(a, actor_fn(t, recv_code(t)));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_QUIESCENT);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_BLOCKED);

  er_scheduler_inject(sys, a, 99);
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 99);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, spawn_runs_child_and_returns_handle) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* parent = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(parent);
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, 7)); /* child halts with 7 */
  pl_val spargs[1] = {t->vstack[base]};
  pl_val body = code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs);
  t->vsp = base;
  er_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(parent), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(parent), 1); /* first minted handle */
  er_actor* child = er_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(er_actor_state(child), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(child), 7);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, sendcaps_reminted_in_receiver) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* parent = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(parent);
  /* parent: Seq(Spawn <recv-child>, SendCaps 1 5 [0]); the child handle
   * is deterministically 1 (first minted), so the row embeds it. */
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, recv_code(t))); /* child: Recv, halt */
  pl_val spargs[1] = {t->vstack[base]};
  pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  pl_vpush(t, test_app1(t, 0, 0)); /* caps row [0]: self */
  pl_val scargs[3] = {1, 5, t->vstack[base + 2]};
  pl_vpush(t, code_effect(t, ax_s8('S', 'e', 'n', 'd', 'C', 'a', 'p', 's'), 3,
                          scargs));
  pl_val body = code_seq(t, t->vstack[base + 1], t->vstack[base + 3]);
  t->vsp = base;
  er_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(parent), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(parent), 0); /* SendCaps response */
  er_actor* child = er_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(er_actor_state(child), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(child));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 5);
  pl_cell* caps = pl_as(PL_TAG_APP, pl_app_args(r)[1]);
  cr_assert_not_null(caps);
  cr_assert_eq(pl_app_n(caps), 1);
  cr_assert_eq(pl_app_args(caps)[0], 1); /* fresh receiver-local handle */
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, send_to_closed_handle_crashes_sender) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  /* Seq(CloseHandle 0, Send 0 9): the self handle is revocable; the
   * send through the closed handle is the reference's host error. */
  size_t base = t->vsp;
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)"CloseHandle", 11));
  pl_val cargs[1] = {0};
  pl_vpush(t, code_effect(t, t->vstack[base], 1, cargs));
  pl_val sargs[2] = {0, 9};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_val body = code_seq(t, t->vstack[base + 1], t->vstack[base + 2]);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_CRASHED);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, cross_actor_payload_is_store_resident) {
  /* Send a structured payload (a law) to a recv-blocked child: it must
   * arrive intact through the store, forced at send. */
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* parent = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(parent);
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, recv_code(t)));     /* child: Recv, halt */
  pl_vpush(t, test_law(t, 2, ax_s1('K'), 1)); /* payload: the K law */
  pl_val spargs[1] = {t->vstack[base]};
  pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  pl_val sargs[2] = {1, t->vstack[base + 1]};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_val body = code_seq(t, t->vstack[base + 2], t->vstack[base + 3]);
  t->vsp = base;
  er_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  er_actor* child = er_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(er_actor_state(child), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(child));
  cr_assert_not_null(r);
  pl_cell* law = pl_as(PL_TAG_LAW, pl_app_args(r)[0]);
  cr_assert_not_null(law);
  cr_assert_eq(pl_law_arity(law), 2);
  cr_assert_eq(pl_law_name(law), ax_s1('K'));
  cr_assert(pl_store_owns(rt.store, pl_app_args(r)[0]),
            "payload must live in the shared store");
  er_scheduler_free(sys);
  test_rt_free(&rt);
}
