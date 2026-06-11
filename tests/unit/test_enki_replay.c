#include <criterion/criterion.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "enki/actor.h"
#include "test_plan.h"

/*
 * Event log & replay (spec §9 R1–R2): direct-effect results and host
 * injections are recorded; a replay run substitutes the recorded
 * results without performing syscalls, verifying (actor, op, args)
 * at every site.  Coordination effects are internal and never logged.
 *
 * Law-body code combinators as in test_enki_actor.c.
 */

static pl_val code_lit(pl_thread* t, pl_val v) {
  return test_app1(t, 0, v);
}

static pl_val code_app(pl_thread* t, pl_val f, pl_val x) {
  return test_app2(t, 0, f, x);
}

static pl_val test_p82(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, 82);
  pl_val pin = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return pin;
}

static pl_val code_effect(pl_thread* t, pl_val name, size_t n,
                          const pl_val* args) {
  size_t base = t->vsp;
  pl_vpush(t, code_lit(t, test_app(t, name, n, args)));
  pl_vpush(t, code_lit(t, test_p82(t)));
  pl_val out = code_app(t, t->vstack[base + 1], t->vstack[base]);
  t->vsp = base;
  return out;
}

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

static pl_val actor_fn(pl_thread* t, pl_val body) {
  return test_law(t, 1, 0, body);
}

/* A lazy thunk whose body expr runs under an empty (1-slot) env. */
static pl_val test_thunk(pl_thread* t, pl_val expr) {
  size_t base = t->vsp;
  pl_vpush(t, expr);
  pl_gc_reserve(t, PL_ENV_CELLS(1) + PL_THUNK_CELLS);
  pl_val env = pl_mk_env(t, 1);
  pl_val out = pl_mk_thunk(t, env, t->vstack[base]);
  t->vsp = base;
  return out;
}

static pl_val recv_code(pl_thread* t) {
  pl_val args[1] = {0};
  return code_effect(t, ax_s4('R', 'e', 'c', 'v'), 1, args);
}

/* An actor whose result is the ReadFile bar of `path`. */
static void start_readfile_actor(er_scheduler* sys, const char* path) {
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  size_t base = t->vsp;
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)path, strlen(path)));
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)"ReadFile", 8));
  /* read the rooted slots only after every allocating call above */
  pl_val rargs[1] = {t->vstack[base]};
  pl_val body = code_effect(t, t->vstack[base + 1], 1, rargs);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));
}

static void assert_bar(pl_val v, const char* s) {
  size_t n = strlen(s);
  cr_assert(pl_is_nat(v), "expected a bar nat");
  cr_assert_eq(pl_nat_byte_len(v), n + 1);
  for (size_t i = 0; i < n; i++)
    cr_assert_eq(pl_nat_byte_at(v, i), (uint8_t)s[i]);
  cr_assert_eq(pl_nat_byte_at(v, n), 1); /* bar terminator */
}

Test(replay, readfile_substitutes_without_syscall) {
  char dir[] = "/tmp/enki-replay-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  char path[256], logpath[256];
  (void)snprintf(path, sizeof(path), "%s/data.txt", dir);
  (void)snprintf(logpath, sizeof(logpath), "%s/run.enkilog", dir);
  FILE* f = fopen(path, "w");
  cr_assert_not_null(f);
  fputs("alpha", f);
  fclose(f);

  test_rt rt = test_rt_new();

  /* record: the effect really reads the file */
  er_log* log = er_log_new();
  {
    er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
    er_scheduler_record(sys, log);
    start_readfile_actor(sys, path);
    cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
    er_actor* a = er_scheduler_actor_by_id(sys, 0);
    cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
    assert_bar(er_actor_result(a), "alpha");
    er_scheduler_free(sys);
  }
  cr_assert_eq(er_log_events(log), 1);

  /* the log round-trips through a file; the data file is deleted, so
   * only substitution can reproduce the contents */
  cr_assert(er_log_write_file(log, logpath));
  er_log_free(log);
  cr_assert_eq(unlink(path), 0);
  er_log* loaded = er_log_read_file(logpath);
  cr_assert_not_null(loaded);
  cr_assert_eq(er_log_events(loaded), 1);

  {
    er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
    er_scheduler_replay(sys, loaded);
    start_readfile_actor(sys, path);
    cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
    er_actor* a = er_scheduler_actor_by_id(sys, 0);
    cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
    assert_bar(er_actor_result(a), "alpha"); /* file no longer exists */
    cr_assert_eq(er_scheduler_log_cursor(sys), 1);
    er_scheduler_free(sys);
  }
  er_log_free(loaded);
  test_rt_free(&rt);
}

/* Self-ping with a lazy Now payload: the clock read happens while the
 * payload is pinned at send, so the hook fires through the pinning path
 * and the recorded instant is reproduced exactly on replay. */
static pl_val run_now_ping(pl_store* store, const er_log* play, er_log* rec) {
  er_scheduler* sys = er_scheduler_new(store, (er_config){0});
  if (rec != NULL)
    er_scheduler_record(sys, rec);
  if (play != NULL)
    er_scheduler_replay(sys, play);
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  size_t base = t->vsp;
  pl_val nargs[1] = {0};
  /* the payload must be a real thunk: a code APP embedded in the row
   * would be data by the under-application invariant and never run */
  pl_vpush(t, test_thunk(t, code_effect(t, ax_s3('N', 'o', 'w'), 1, nargs)));
  pl_val sargs[2] = {0, t->vstack[base]};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_vpush(t, recv_code(t));
  pl_val body = code_seq(t, t->vstack[base + 1], t->vstack[base + 2]);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  pl_val instant = pl_app_args(r)[0];
  cr_assert(pl_is_nat63(instant));
  cr_assert_gt(instant, 0);
  if (play != NULL)
    cr_assert_eq(er_scheduler_log_cursor(sys), er_log_events(play));
  er_scheduler_free(sys);
  return instant;
}

Test(replay, now_through_pinning_is_stable) {
  test_rt rt = test_rt_new();
  er_log* log = er_log_new();
  pl_val recorded = run_now_ping(rt.store, NULL, log);
  cr_assert_eq(er_log_events(log), 1); /* messaging is internal: only Now */
  pl_val replayed = run_now_ping(rt.store, log, NULL);
  cr_assert_eq(replayed, recorded);
  er_log_free(log);
  test_rt_free(&rt);
}

Test(replay, injections_are_logged_and_verified) {
  test_rt rt = test_rt_new();
  er_log* log = er_log_new();
  {
    er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
    er_scheduler_record(sys, log);
    er_actor* a = er_scheduler_actor(sys);
    er_actor_start(a,
                   actor_fn(er_actor_thread(a), recv_code(er_actor_thread(a))));
    cr_assert_eq(er_scheduler_run(sys), ER_RUN_QUIESCENT);
    er_scheduler_inject(sys, a, 99);
    cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
    pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
    cr_assert_not_null(r);
    cr_assert_eq(pl_app_args(r)[0], 99);
    er_scheduler_free(sys);
  }
  cr_assert_eq(er_log_events(log), 1);
  {
    er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
    er_scheduler_replay(sys, log);
    er_actor* a = er_scheduler_actor(sys);
    er_actor_start(a,
                   actor_fn(er_actor_thread(a), recv_code(er_actor_thread(a))));
    cr_assert_eq(er_scheduler_run(sys), ER_RUN_QUIESCENT);
    er_scheduler_inject(sys, a, 99); /* verified against the log */
    cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
    pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
    cr_assert_not_null(r);
    cr_assert_eq(pl_app_args(r)[0], 99);
    cr_assert_eq(er_scheduler_log_cursor(sys), 1);
    er_scheduler_free(sys);
  }
  er_log_free(log);
  test_rt_free(&rt);
}

Test(replay, live_systems_are_unaffected_by_the_hook) {
  /* the io hook stays registered globally after a recording system is
   * freed; a plain live system must keep executing effects directly */
  test_rt rt = test_rt_new();
  er_log* log = er_log_new();
  {
    er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
    er_scheduler_record(sys, log);
    er_scheduler_free(sys);
  }
  er_log_free(log);
  {
    er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
    er_actor* a = er_scheduler_actor(sys);
    pl_thread* t = er_actor_thread(a);
    pl_val nargs[1] = {0};
    er_actor_start(a,
                   actor_fn(t, code_effect(t, ax_s3('N', 'o', 'w'), 1, nargs)));
    cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
    cr_assert_gt(er_actor_result(a), 0);
    er_scheduler_free(sys);
  }
  test_rt_free(&rt);
}
