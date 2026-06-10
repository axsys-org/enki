#include <criterion/criterion.h>

#include "test_plan.h"

/*
 * Suspension core (spec §3–§4): pl_thread_start / pl_thread_run /
 * pl_thread_deposit.  A suspended thread is a complete continuation in
 * the thread's stacks plus the resume slots; these tests exercise the
 * yield/resume protocol, exception unwinding to the entry watermarks,
 * GC of a suspended thread, and yield-point determinism.
 *
 * APP cells are WHNF by construction, so redexes are driven through
 * lazy thunks whose body code runs under an empty (1-slot) env, as in
 * test_plan_eval.c.
 */

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

/* A thunk computing ((K (K 7 8)) 9) -> 7, K x y = x.  Body code:
 * (0 (0 (0 K) (0 (0 (0 K) 7) 8)) 9); nats are literals (maxArg 0). */
static pl_val test_k_thunk(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, test_app1(t, 0, test_law(t, 2, 0, 1))); /* (0 K)       */
  pl_vpush(t, test_app2(t, 0, t->vstack[base], 7));   /* (0 K' 7)    */
  pl_vpush(t, test_app2(t, 0, t->vstack[base + 1], 8));
  pl_vpush(t, test_app2(t, 0, t->vstack[base], t->vstack[base + 2]));
  pl_val expr = test_app2(t, 0, t->vstack[base + 3], 9);
  t->vsp = base;
  return test_thunk(t, expr);
}

/* A thunk that raises PLAN_EXN(code) when forced: P66 % ("Throw" code). */
static pl_val test_throwing(pl_thread* t, uint64_t code) {
  size_t base = t->vsp;
  pl_val args[1] = {code};
  pl_vpush(t, test_app(t, ax_s5('T', 'h', 'r', 'o', 'w'), 1, args));
  pl_vpush(t, test_app1(t, 0, t->vstack[base])); /* (0 row) literal */
  pl_vpush(t, test_app1(t, 0, test_p66(t)));     /* (0 P66)         */
  pl_val expr = test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]);
  t->vsp = base;
  return test_thunk(t, expr);
}

Test(thread, run_to_done_with_ample_fuel) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_thread_start(t, test_k_thunk(t));
  cr_assert_eq(pl_thread_run(t, 1u << 20), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 7);
  test_rt_free(&rt);
}

Test(thread, yields_on_fuel_and_resumes_to_same_result) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t vsp0 = t->vsp, fsp0 = t->fsp;
  pl_thread_start(t, test_k_thunk(t));
  int quanta = 0;
  pl_run_status s;
  do {
    s = pl_thread_run(t, 2); /* exactly one machine step per quantum */
    quanta++;
    cr_assert_lt(quanta, 1 << 20, "runaway resume loop");
  } while (s == PL_RUN_YIELDED);
  cr_assert_eq(s, PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 7);
  cr_assert_gt(quanta, 1, "expected at least one yield");
  /* completion restores the entry watermarks */
  cr_assert_eq(t->vsp, vsp0);
  cr_assert_eq(t->fsp, fsp0);
  test_rt_free(&rt);
}

Test(thread, yield_points_are_deterministic) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  int counts[2];
  for (int i = 0; i < 2; i++) {
    pl_thread_start(t, test_k_thunk(t));
    int quanta = 0;
    while (pl_thread_run(t, 2) == PL_RUN_YIELDED)
      quanta++;
    cr_assert_eq(t->status, PL_RUN_DONE);
    cr_assert_eq(pl_thread_result(t), 7);
    counts[i] = quanta;
  }
  cr_assert_eq(counts[0], counts[1]);
  test_rt_free(&rt);
}

Test(thread, exception_unwinds_to_watermarks) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t vsp0 = t->vsp, fsp0 = t->fsp;
  pl_thread_start(t, test_throwing(t, 7));
  pl_run_status s;
  do
    s = pl_thread_run(t, 2);
  while (s == PL_RUN_YIELDED);
  cr_assert_eq(s, PL_RUN_EXN);
  cr_assert_null(t->exn_msg);
  cr_assert_eq(t->exn, 7);
  cr_assert_eq(t->vsp, vsp0);
  cr_assert_eq(t->fsp, fsp0);
  /* the thread object remains usable for a fresh run */
  pl_thread_start(t, test_k_thunk(t));
  cr_assert_eq(pl_thread_run(t, 1u << 20), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 7);
  test_rt_free(&rt);
}

Test(thread, suspended_continuation_survives_gc) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_thread_start(t, test_k_thunk(t));
  pl_run_status s;
  do {
    pl_gc_collect_now(t); /* moves everything the continuation roots */
    s = pl_thread_run(t, 2);
  } while (s == PL_RUN_YIELDED);
  cr_assert_eq(s, PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 7);
  test_rt_free(&rt);
}

Test(thread, start_nf_normalizes_deeply) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* (K <thunk -> 7>): under-applied, so already WHNF; nf forces the
   * lazy field and the result row carries the nat */
  pl_vpush(t, test_law(t, 2, 0, 1));
  pl_vpush(t, test_k_thunk(t));
  pl_val v = test_app1(t, t->vstack[base], t->vstack[base + 1]);
  t->vsp = base;
  pl_thread_start_nf(t, v);
  pl_run_status s;
  do
    s = pl_thread_run(t, 2);
  while (s == PL_RUN_YIELDED);
  cr_assert_eq(s, PL_RUN_DONE);
  pl_val r = pl_thread_result(t);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_whnf(t, pl_app_args(p)[0]), 7);
  test_rt_free(&rt);
}

Test(thread, deposit_resumes_with_response) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  /* Simulate a blocked effect at thread top level: the deposited
   * response is RETURNed to the (empty) frame stack and becomes the
   * result.  Real BLOCKED producers arrive with op 82. */
  pl_thread_start(t, 0);
  t->status = PL_RUN_BLOCKED;
  t->blocked_on = 42;
  pl_thread_deposit(t, 9);
  cr_assert_eq(t->blocked_on, 0);
  cr_assert_eq(pl_thread_run(t, 1u << 10), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 9);
  test_rt_free(&rt);
}
