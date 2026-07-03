#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <string.h>

#include "test_plan.h"

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

/* A one-element app whose field is a thunk that evaluates to value. */
static pl_val test_app1_thunk_to(pl_thread* t, pl_val value) {
  size_t base = t->vsp;
  pl_vpush(t, test_thunk(t, test_app1(t, 0, value)));
  pl_val out = test_app1(t, 0, t->vstack[base]);
  t->vsp = base;
  return out;
}

static void test_expect_no_op66(pl_thread* t, pl_val name, size_t n,
                                const pl_val* args) {
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_op66(t, name, n, args);
    cr_assert_fail("expected no primop");
  }
  pl_catch_unwind(t, &c);
  cr_assert_not_null(t->exn_msg);
  cr_assert_str_eq(t->exn_msg, "no primop 66 (argc 1)");
}

/* ── Application shapes ────────────────────────────────────────────────── */

Test(apply, under_application_builds_app) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_val k = test_law(t, 2, 0, 1); /* K x y = x */
  pl_val r = pl_apply(t, k, 7);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_arity(r), 1);
  test_rt_free(&rt);
}

Test(apply, exact_and_over_application) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 2, 0, 1)); /* K */
  pl_vpush(t, test_law(t, 1, 0, 1)); /* id */
  /* ((K id) 7) 9  ->  id 9  ->  9 */
  pl_val r = pl_apply(t, t->vstack[base], t->vstack[base + 1]);
  pl_vpush(t, r);
  r = pl_apply(t, t->vstack[base + 2], 7);
  pl_vpush(t, r);
  r = pl_apply(t, t->vstack[base + 3], 9);
  cr_assert_eq(r, 9);
  test_rt_free(&rt);
}

Test(apply, args_stay_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0)
    cr_assert_fail("unexpected exception");
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 2, 0, 1)); /* K x y = x */
  pl_vpush(t, test_throwing(t, 7));
  /* K 5 <throw>  ->  5 without forcing the throwing arg */
  pl_val r = pl_apply(t, t->vstack[base], 5);
  pl_vpush(t, r);
  r = pl_apply(t, t->vstack[base + 2], t->vstack[base + 1]);
  cr_assert_eq(r, 5);
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

Test(apply, slow_bytecode_thunk_applies_unknown_arity) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 2, 0, 1)); /* K x y = x */
  pl_vpush(t, 5);
  pl_vpush(t, test_throwing(t, 7));
  pl_gc_reserve(t, PL_THKE_CELLS(3));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 3, &t->vstack[base]);
  t->vsp = base;

  cr_assert_eq(pl_whnf(t, thke), 5);
  test_rt_free(&rt);
}

Test(apply, fast_bytecode_thunk_applies_deferred_head) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_thunk(t, test_app1(t, 0, test_law(t, 2, 0, 1))));
  pl_vpush(t, 5);
  pl_vpush(t, test_throwing(t, 7));
  pl_gc_reserve(t, PL_THKE_CELLS(3));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 3, &t->vstack[base]);
  t->vsp = base;

  cr_assert_eq(pl_whnf(t, thke), 5);
  test_rt_free(&rt);
}

Test(apply, slow_thke_under_applied_builds_flat_app) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 3, 0, 1)); /* K3 x y z = x */
  pl_vpush(t, 5);
  pl_vpush(t, 6);
  pl_gc_reserve(t, PL_THKE_CELLS(3));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 3, &t->vstack[base]);
  t->vsp = base;

  pl_val r = pl_whnf(t, thke);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_n(p), 2);
  cr_assert_eq(pl_arity(r), 1);
  cr_assert_eq(pl_whnf(t, pl_apply(t, r, 7)), 5);
  test_rt_free(&rt);
}

Test(apply, slow_thke_under_applied_partial_head_stays_flat) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_val k3 = test_law(t, 3, 0, 1); /* K3 x y z = x */
  pl_vpush(t, k3);
  pl_vpush(t, test_app1(t, t->vstack[base], 5)); /* (K3 5), need 2 */
  pl_vpush(t, 6);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 2, &t->vstack[base + 1]);
  t->vsp = base + 1;

  /* the result must extend the spine (flat), not nest an app head */
  pl_val r = pl_whnf(t, thke);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_n(p), 2);
  cr_assert_eq(pl_app_head(p), t->vstack[base]);
  cr_assert_eq(pl_whnf(t, pl_apply(t, r, 7)), 5);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(apply, slow_thke_data_head_extends_row) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 0, 7, 8)); /* the row [7 8] */
  pl_vpush(t, 9);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 2, &t->vstack[base]);
  t->vsp = base;

  /* snoc onto a row: [7 8 9], still flat and 0-headed */
  pl_val r = pl_whnf(t, thke);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_n(p), 3);
  cr_assert_eq(pl_app_head(p), 0);
  cr_assert_eq(pl_app_args(p)[2], 9);
  test_rt_free(&rt);
}

Test(apply, slow_thke_splices_partial_app_head) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* head is already a partial application: (K 5) applied to 9 */
  pl_vpush(t, test_app1(t, test_law(t, 2, 0, 1), 5));
  pl_vpush(t, 9);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 2, &t->vstack[base]);
  t->vsp = base;

  cr_assert_eq(pl_whnf(t, thke), 5);
  test_rt_free(&rt);
}

Test(apply, tailcall_loops_in_constant_frame_space) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* loop x = loop x, as fused tail-call bytecode: must run in constant
   * frame space AND stay preemptable (the tail path takes a fuel step) */
  pl_vpush(t, test_law(t, 1, 0, 0));
  pl_val pin = pl_pin(t, t->vstack[base]); /* store-resident: GC-stable */
  t->vsp = base;
  pl_cell* pp = pl_as(PL_TAG_PIN, pin);
  cr_assert_not_null(pp);
  static pl_op_t loop_ops[7];
  loop_ops[0] = OP_PUSH_LIT;
  loop_ops[1] = pin;
  loop_ops[2] = OP_PUSH_VAR;
  loop_ops[3] = 1;
  loop_ops[4] = OP_TAILCALL;
  loop_ops[5] = 2;
  loop_ops[6] = PL_BAN_FAST;
  static pl_code loop_code = {loop_ops, 7};
  pl_pin_set_code(pp, &loop_code);

  size_t fcap0 = t->fcap;
  pl_vpush(t, pin);
  pl_vpush(t, 5);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 2, &t->vstack[base]);
  t->vsp = base;
  pl_thread_start(t, thke);
  for (int i = 0; i < 200; i++)
    cr_assert_eq(pl_thread_run(t, 10000), PL_RUN_YIELDED);
  /* ~2M tail calls: frames grow only at fuel checkpoints (one F_UPD
   * per quantum survives until the loop would return), never per call */
  cr_assert_eq(t->fcap, fcap0);
  cr_assert(t->fsp <= t->base_fsp + 220);
  pl_pin_set_code(pp, NULL); /* the code is a stack-lifetime fake */
  test_rt_free(&rt);
}

Test(apply, slow_thke_over_applied_order) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* (K K' 5) 6 7 = K' 6 7 = 6: the first excess arg applies first */
  pl_vpush(t, test_law(t, 2, 0, 1)); /* K x y = x */
  pl_vpush(t, test_law(t, 2, 0, 1)); /* K' */
  pl_vpush(t, 5);
  pl_vpush(t, 6);
  pl_vpush(t, 7);
  pl_gc_reserve(t, PL_THKE_CELLS(5));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 5, &t->vstack[base]);
  t->vsp = base;

  cr_assert_eq(pl_whnf(t, thke), 6);
  test_rt_free(&rt);
}

/* ── Recursive-let knots ───────────────────────────────────────────────── */

/*
 * f x = let b1 = b2; b2 = x in b1
 * body: (1 3 (1 1 2)) with slots [self=0, x=1, b1=2, b2=3].
 * Exercises the backpatched knot: b1's expression references the later
 * bind b2.
 */
Test(judge, knot_forward_reference) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 1, 1, 2));               /* (1 1 2)   */
  pl_vpush(t, test_app2(t, 1, 3, t->vstack[base])); /* (1 3 ...) */
  pl_val f = test_law(t, 1, 0, t->vstack[base + 1]);
  cr_assert_eq(pl_apply(t, f, 9), 9);
  test_rt_free(&rt);
}

/* f x = let b = b in b  ->  <<loop>> */
Test(judge, self_referential_bind_raises_loop) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 1, 2, 2)); /* (1 2 2) */
  pl_val f = test_law(t, 1, 0, t->vstack[base]);
  pl_vpush(t, f);
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)pl_apply(t, t->vstack[base + 1], 9);
    cr_assert_fail("expected <<loop>>");
  }
  pl_catch_unwind(t, &c);
  cr_assert_not_null(t->exn_msg);
  cr_assert_str_eq(t->exn_msg, "<<loop>>");
  test_rt_free(&rt);
}

/* ── Primop strictness ─────────────────────────────────────────────────── */

Test(ops, strict_args_force_left_to_right) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_throwing(t, 7));
  pl_vpush(t, test_throwing(t, 8));
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_op66_2(t, ax_s3('A', 'd', 'd'), t->vstack[base],
                      t->vstack[base + 1]);
    cr_assert_fail("expected PLAN_EXN");
  }
  pl_catch_unwind(t, &c);
  cr_assert_null(t->exn_msg);
  cr_assert_eq(t->exn, 7); /* arg 0 forced first */
  test_rt_free(&rt);
}

Test(ops, lookup_segregates_opcode_set_and_argc) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_val arg1[1] = {0};
  test_expect_no_op66(t, ax_s3('A', 'd', 'd'), 1, arg1);
  test_expect_no_op66(t, ax_s4('R', 'e', 'c', 'v'), 1, arg1);
  test_rt_free(&rt);
}

Test(ops, untaken_branches_stay_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_throwing(t, 7));
  /* If 0 <throw> 42  ->  42 */
  pl_val args[3] = {0, t->vstack[base], 42};
  cr_assert_eq(test_op66(t, ax_s2('I', 'f'), 3, args), 42);
  /* And 0 <throw> -> 0; Or 1 <throw> -> 1 */
  cr_assert_eq(test_op66_2(t, ax_s3('A', 'n', 'd'), 0, t->vstack[base]), 0);
  cr_assert_eq(test_op66_2(t, ax_s2('O', 'r'), 1, t->vstack[base]), 1);
  test_rt_free(&rt);
}

Test(ops, elim_case_branches_stay_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_throwing(t, 7));
  /* match _ _ _ z _ 0  ->  z, with every other branch throwing */
  pl_val th = t->vstack[base];
  pl_val args[6] = {th, th, th, 42, th, 0};
  cr_assert_eq(test_op66(t, ax_s4('E', 'l', 'i', 'm'), 6, args), 42);
  test_rt_free(&rt);
}

Test(ops, elim_decomposition_hands_out_lazy_args) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* scrutinee row (0 1 <throw>): the a-branch gets ini and the (lazy)
   * last element; a const2 law ignores both. */
  pl_vpush(t, test_throwing(t, 7));
  pl_vpush(t, test_app2(t, 0, 1, t->vstack[base]));
  pl_vpush(t, test_app1(t, 0, 99));                    /* (0 99) quote */
  pl_vpush(t, test_law(t, 2, 0, t->vstack[base + 2])); /* const2 -> 99 */
  pl_val th = t->vstack[base];
  pl_val args[6] = {th, th, t->vstack[base + 3], th, th, t->vstack[base + 1]};
  cr_assert_eq(test_op66(t, ax_s4('E', 'l', 'i', 'm'), 6, args), 99);
  test_rt_free(&rt);
}

Test(ops, seq_and_force) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  /* Seq 1 2 -> 2 */
  cr_assert_eq(test_op66_2(t, ax_s3('S', 'e', 'q'), 1, 2), 2);
  /* Seq <throw> 2 raises */
  size_t base = t->vsp;
  pl_vpush(t, test_throwing(t, 7));
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_op66_2(t, ax_s3('S', 'e', 'q'), t->vstack[base], 2);
    cr_assert_fail("expected PLAN_EXN");
  }
  pl_catch_unwind(t, &c);
  cr_assert_eq(t->exn, 7);
  test_rt_free(&rt);
}

Test(ops, force_deep_normalizes_arg) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_val args[1] = {t->vstack[base]};
  pl_val r = test_op66(t, ax_s5('F', 'o', 'r', 'c', 'e'), 1, args);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_args(p)[0], 42);
  cr_assert((pl_hdr_flags(p[0]) & PL_F_NORMAL) != 0);
  test_rt_free(&rt);
}

Test(ops, deepseq_normalizes_first_and_returns_second_lazily) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_vpush(t, test_throwing(t, 7));
  pl_vpush(t, test_app1(t, 0, t->vstack[base + 1]));
  pl_val r = test_op66_2(t, ax_s7('D', 'e', 'e', 'p', 'S', 'e', 'q'),
                         t->vstack[base], t->vstack[base + 2]);
  cr_assert_eq(r, t->vstack[base + 2]);
  pl_cell* xp = pl_as(PL_TAG_APP, t->vstack[base]);
  cr_assert_not_null(xp);
  cr_assert_eq(pl_app_args(xp)[0], 42);
  cr_assert((pl_hdr_flags(xp[0]) & PL_F_NORMAL) != 0);
  pl_cell* yp = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(yp);
  cr_assert_eq(pl_tag(pl_app_args(yp)[0]), PL_TAG_DEFER);
  test_rt_free(&rt);
}

Test(ops, trace_deep_normalizes_before_showing, .init = cr_redirect_stderr) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_vpush(t, test_throwing(t, 7));
  pl_vpush(t, test_app1(t, 0, t->vstack[base + 1]));
  pl_val r = test_op66_2(t, ax_s5('T', 'r', 'a', 'c', 'e'), t->vstack[base],
                         t->vstack[base + 2]);
  cr_assert_eq(r, t->vstack[base + 2]);
  pl_cell* xp = pl_as(PL_TAG_APP, t->vstack[base]);
  cr_assert_not_null(xp);
  cr_assert_eq(pl_app_args(xp)[0], 42);
  pl_cell* yp = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(yp);
  cr_assert_eq(pl_tag(pl_app_args(yp)[0]), PL_TAG_DEFER);
  test_rt_free(&rt);
}

Test(ops, try_catches_plan_exn_only) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* thrower x = Throw 5 — body: (0 (0 P66) (0 ("Throw" 5))) */
  {
    pl_val row_args[1] = {5};
    pl_vpush(t, test_app(t, ax_s5('T', 'h', 'r', 'o', 'w'), 1, row_args));
    pl_vpush(t, test_app1(t, 0, t->vstack[base]));
    pl_vpush(t, test_app1(t, 0, test_p66(t)));
    pl_vpush(t, test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]));
    pl_vpush(t, test_law(t, 1, 0, t->vstack[base + 3]));
  }
  size_t thrower = t->vsp - 1;
  pl_val args[2] = {t->vstack[thrower], 1};
  pl_val r = test_op66(t, ax_s3('T', 'r', 'y'), 2, args);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), 1); /* Left: (1 exn) */
  cr_assert_eq(pl_app_args(p)[0], 5);

  /* and the Right case */
  pl_vpush(t, test_law(t, 1, 0, 1)); /* id */
  pl_val args2[2] = {t->vstack[t->vsp - 1], 9};
  r = test_op66(t, ax_s3('T', 'r', 'y'), 2, args2);
  p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), 0);
  cr_assert_eq(pl_app_args(p)[0], 9);
  test_rt_free(&rt);
}

Test(ops, equal_deep_and_pin_identity) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 0, 1, 2));
  pl_vpush(t, test_app2(t, 0, 1, 2));
  cr_assert_eq(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base],
                           t->vstack[base + 1]),
               1);
  /* pins dedup to pointer identity */
  pl_val p1 = pl_pin(t, t->vstack[base]);
  pl_val p2 = pl_pin(t, t->vstack[base + 1]);
  cr_assert_eq(p1, p2);
  test_rt_free(&rt);
}

Test(ops, equal_deep_normalizes_second_arg) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_vpush(t, test_app1_thunk_to(t, 42));
  cr_assert_eq(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base],
                           t->vstack[base + 1]),
               1);
  test_rt_free(&rt);
}

Test(ops, row_elements_stay_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* Row 0 1 <stream whose Ix1 tail throws but Ix0 head is 5>:
   * the single element must come out as a lazy Ix0 thunk. */
  pl_vpush(t, test_throwing(t, 7));
  pl_vpush(t, test_app2(t, 0, 5, t->vstack[base])); /* (0 5 <throw>) */
  pl_val args[3] = {0, 1, t->vstack[base + 1]};
  pl_val r = test_op66(t, ax_s3('R', 'o', 'w'), 3, args);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_n(p), 1);
  pl_vpush(t, r);
  /* forcing the element gives Ix0 of the stream = 5 */
  pl_cell* rp = pl_as(PL_TAG_APP, t->vstack[base + 2]);
  cr_assert_not_null(rp);
  pl_val e = pl_whnf(t, pl_app_args(rp)[0]);
  cr_assert_eq(e, 5);
  test_rt_free(&rt);
}

Test(ops, law_op_adds_one_to_arity) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  /* op0: (P0 % (1 a m b)) makes L (a+1) m b */
  size_t base = t->vsp;
  pl_vpush(t, 0);
  pl_val p0 = pl_pin(t, t->vstack[base]);
  pl_vpush(t, p0);
  pl_val row_args[3] = {1, 0, 1};
  pl_vpush(t, test_app(t, 1, 3, row_args)); /* (1 1 0 1) */
  pl_val r = pl_apply(t, t->vstack[base + 1], t->vstack[base + 2]);
  pl_cell* lp = pl_as(PL_TAG_LAW, r);
  cr_assert_not_null(lp);
  cr_assert_eq(pl_law_arity(lp), 2); /* nat a + 1 */
  test_rt_free(&rt);
}

/* ── nf ────────────────────────────────────────────────────────────────── */

Test(nf, deep_normalization_snaps_thunks) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* row of one thunk that evaluates to 42 */
  pl_vpush(t, test_thunk(t, test_app1(t, 0, 42)));
  pl_vpush(t, test_app1(t, 0, t->vstack[base]));
  pl_val r = pl_nf(t, t->vstack[base + 1]);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_args(p)[0], 42); /* snapped, no IND left */
  cr_assert((pl_hdr_flags(p[0]) & PL_F_NORMAL) != 0);
  test_rt_free(&rt);
}
