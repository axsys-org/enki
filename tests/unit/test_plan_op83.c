#include <criterion/criterion.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_plan.h"

/*
 * op 83 (HTTP driver) at the plan layer: Fetch is a coordination
 * effect like Send/Recv — the body rebuilds the request spine
 * [Fetch, req, cfg] and the machine parks it with PL_RUN_BLOCKED.
 * Both args deep-normalize at initiation.  All shape/URL validation
 * happens at service time in pkg/enki; none of it is visible here.
 *
 * Redexes are driven through lazy thunks with KAL body code, as in
 * test_plan_op82.c.
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

/* The pin of nat 83, the HTTP driver op set. */
static pl_val test_p83(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, 83);
  pl_val pin = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return pin;
}

/* A thunk forcing (P83 % (name args…)): body (0 (0 P83) (0 row)). */
static pl_val test_op83_thunk(pl_thread* t, pl_val name, size_t n,
                              const pl_val* args) {
  size_t base = t->vsp;
  pl_vpush(t, test_app(t, name, n, args));
  pl_vpush(t, test_app1(t, 0, t->vstack[base])); /* (0 row) */
  pl_vpush(t, test_app1(t, 0, test_p83(t)));     /* (0 P83) */
  pl_val expr = test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]);
  t->vsp = base;
  return test_thunk(t, expr);
}

/* Run until the thread leaves the runnable set (2-fuel quanta). */
static pl_run_status test_run(pl_thread* t) {
  pl_run_status s;
  int quanta = 0;
  do {
    s = pl_thread_run(t, 2);
    cr_assert_lt(++quanta, 1 << 20, "runaway resume loop");
  } while (s == PL_RUN_YIELDED);
  return s;
}

#define FETCH ax_s5('F', 'e', 't', 'c', 'h')

Test(op83, fetch_parks_then_deposit_resumes) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t vsp0 = t->vsp, fsp0 = t->fsp;
  pl_val args[2] = {11, 22}; /* opaque to the plan layer */
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);

  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_n(p), 2);
  cr_assert_eq(pl_app_args(p)[0], 11);
  cr_assert_eq(pl_app_args(p)[1], 22);

  /* a synthetic (0 resp) result row, as the executor would deposit */
  size_t rb = t->vsp;
  pl_vpush(t, test_app1(t, 0, 200));
  pl_val resp = t->vstack[rb];
  t->vsp = rb;
  pl_thread_deposit(t, resp);
  cr_assert_eq(t->blocked_on, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  pl_cell* r = pl_as(PL_TAG_APP, pl_thread_result(t));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_head(r), 0);
  cr_assert_eq(pl_app_args(r)[0], 200);
  cr_assert_eq(t->vsp, vsp0);
  cr_assert_eq(t->fsp, fsp0);
  test_rt_free(&rt);
}

Test(op83, fetch_normalizes_both_args_at_initiation) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  pl_vpush(t, test_thunk(t, 7)); /* req: forced by the deep mask */
  pl_vpush(t, test_thunk(t, 9)); /* cfg: forced by the deep mask */
  pl_val args[2] = {t->vstack[base], t->vstack[base + 1]};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);

  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_n(p), 2);
  /* both payloads were deep-normalized before the request parked */
  cr_assert_eq(pl_app_args(p)[0], 7);
  cr_assert_eq(pl_app_args(p)[1], 9);

  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 0);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, payload_effects_block_before_the_request) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  /* A Fetch whose cfg normalization performs a Recv: the inner effect
   * blocks FIRST, as the caller's own execution, and only then does
   * the Fetch request park with the settled payload. */
  size_t base = t->vsp;
  pl_val rargs[1] = {0};
  pl_vpush(t, test_app(t, ax_s4('R', 'e', 'c', 'v'), 1, rargs));
  pl_vpush(t, test_app1(t, 0, t->vstack[base])); /* (0 recvrow) */
  size_t pb = t->vsp;
  pl_vpush(t, 82);
  pl_vpush(t, pl_pin(t, t->vstack[pb]));
  pl_vpush(t, test_app1(t, 0, t->vstack[pb + 1])); /* (0 P82) */
  pl_vpush(t, test_app2(t, 0, t->vstack[pb + 2], t->vstack[base + 1]));
  pl_val recv = test_thunk(t, t->vstack[pb + 3]);
  t->vsp = base;
  pl_vpush(t, recv);

  pl_val fargs[2] = {5, t->vstack[base]};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, fargs));

  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);
  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), ax_s4('R', 'e', 'c', 'v')); /* inner first */

  pl_thread_deposit(t, 6);
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);
  p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_args(p)[0], 5);
  cr_assert_eq(pl_app_args(p)[1], 6); /* the deposited response */

  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 0);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, requires_rplan_mode) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t; /* rplan_f defaults to false */
  pl_val args[2] = {11, 22};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);
  test_rt_free(&rt);
}

Test(op83, unknown_op_is_runtime_error) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  pl_val args[2] = {11, 22};
  pl_thread_start(t, test_op83_thunk(t, ax_s4('F', 'r', 'o', 'b'), 2, args));
  cr_assert_eq(test_run(t), PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);
  test_rt_free(&rt);
}

Test(op83, wrong_arity_is_runtime_error) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  /* Fetch is argc 2; a 1-arg row has no matching bucket entry. */
  pl_val args[1] = {11};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 1, args));
  cr_assert_eq(test_run(t), PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);
  test_rt_free(&rt);
}

Test(op83, blocked_request_survives_gc) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  pl_vpush(t, test_thunk(t, 7));
  pl_val args[2] = {t->vstack[base], 3};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);

  pl_gc_collect_now(t); /* moves the request and the parked continuation */
  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_args(p)[0], 7);
  cr_assert_eq(pl_app_args(p)[1], 3);

  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 0);
  t->vsp = base;
  test_rt_free(&rt);
}
