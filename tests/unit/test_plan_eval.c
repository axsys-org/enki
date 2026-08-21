#include "test.h"
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

static pl_val test_byte_mask(pl_thread* t, const uint8_t* bytes, size_t n) {
  uint8_t mask[32] = {0};
  for (size_t i = 0; i < n; i++)
    mask[bytes[i] / 8u] |= (uint8_t)(1u << (bytes[i] % 8u));
  return pl_nat_from_bytes(t, mask, sizeof(mask));
}

static void test_assert_nat_row3(pl_val value, pl_val a, pl_val b, pl_val c) {
  pl_cell* p = pl_as(PL_TAG_APP, value);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_head(p), 0);
  ASSERT_EQ(pl_app_n(p), 3);
  ASSERT(pl_is_nat(pl_app_args(p)[0]));
  ASSERT(pl_is_nat(pl_app_args(p)[1]));
  ASSERT(pl_is_nat(pl_app_args(p)[2]));
  ASSERT(pl_nat_eq(pl_app_args(p)[0], a));
  ASSERT(pl_nat_eq(pl_app_args(p)[1], b));
  ASSERT(pl_nat_eq(pl_app_args(p)[2], c));
}

static pl_val test_cord_text(pl_thread* t, pl_val text) {
  pl_val fields[2] = {ax_s4('t', 'e', 'x', 't'), text};
  return test_app(t, 0, 2, fields);
}

static pl_val test_cord_slice(pl_thread* t, pl_val source, pl_val offset,
                              pl_val length) {
  pl_val fields[4] = {ax_s5('s', 'l', 'i', 'c', 'e'), source, offset, length};
  return test_app(t, 0, 4, fields);
}

static pl_val test_cord_repeat(pl_thread* t, pl_val byte, pl_val count) {
  pl_val fields[3] = {ax_s6('r', 'e', 'p', 'e', 'a', 't'), byte, count};
  return test_app(t, 0, 3, fields);
}

static pl_val test_cord_cat(pl_thread* t, pl_val left, pl_val right) {
  pl_val fields[3] = {ax_s3('c', 'a', 't'), left, right};
  return test_app(t, 0, 3, fields);
}

static bool growing_enter_hook(pl_thread* t, size_t hbase, uint32_t argc,
                               pl_val* out) {
  (void)hbase;
  (void)argc;
  (void)out;
  /* Larger than test_rt's initial semispace: this collects, grows, and frees
   * both old spaces while the entered unresolved PIN remains stack-rooted. */
  pl_gc_reserve(t, (size_t)1 << 17);
  return false;
}

static void test_expect_no_op66(pl_thread* t, pl_val name, size_t n,
                                const pl_val* args) {
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_op66(t, name, n, args);
    FAIL_TEST("expected no primop");
  }
  pl_catch_unwind(t, &c);
  ASSERT_NOT_NULL(t->exn_msg);
  char expected[64];
  (void)snprintf(expected, sizeof(expected), "no primop 66 (argc %zu)", n);
  ASSERT_STR_EQ(t->exn_msg, expected);
}

static bool test_install_self_replacement_raises(pl_thread* t,
                                                 pl_val compiler) {
  pl_val args[1] = {compiler};
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_op66(t, ax_s7('I', 'n', 's', 't', 'a', 'l', 'l'), 1, args);
    pl_catch_pop(t, &c);
    return false;
  }
  pl_catch_unwind(t, &c);
  return true;
}

/* ── Application shapes ────────────────────────────────────────────────── */

TEST(apply, under_application_builds_app) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_val k = test_law(t, 2, 0, 1); /* K x y = x */
  pl_val r = pl_apply(t, k, 7);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_arity(r), 1);
  test_rt_free(&rt);
}

TEST(apply, exact_and_over_application) {
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
  ASSERT_EQ(r, 9);
  test_rt_free(&rt);
}

TEST(apply, args_stay_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0)
    FAIL_TEST("unexpected exception");
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 2, 0, 1)); /* K x y = x */
  pl_vpush(t, test_throwing(t, 7));
  /* K 5 <throw>  ->  5 without forcing the throwing arg */
  pl_val r = pl_apply(t, t->vstack[base], 5);
  pl_vpush(t, r);
  r = pl_apply(t, t->vstack[base + 2], t->vstack[base + 1]);
  ASSERT_EQ(r, 5);
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

TEST(apply, slow_bytecode_thunk_applies_unknown_arity) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 2, 0, 1)); /* K x y = x */
  pl_vpush(t, 5);
  pl_vpush(t, test_throwing(t, 7));
  pl_gc_reserve(t, PL_THKE_CELLS(3));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 3, &t->vstack[base]);
  t->vsp = base;

  ASSERT_EQ(pl_whnf(t, thke), 5);
  test_rt_free(&rt);
}

TEST(apply, fast_bytecode_thunk_applies_deferred_head) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_thunk(t, test_app1(t, 0, test_law(t, 2, 0, 1))));
  pl_vpush(t, 5);
  pl_vpush(t, test_throwing(t, 7));
  pl_gc_reserve(t, PL_THKE_CELLS(3));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 3, &t->vstack[base]);
  t->vsp = base;

  ASSERT_EQ(pl_whnf(t, thke), 5);
  test_rt_free(&rt);
}

TEST(apply, slow_thke_under_applied_builds_flat_app) {
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
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_n(p), 2);
  ASSERT_EQ(pl_arity(r), 1);
  ASSERT_EQ(pl_whnf(t, pl_apply(t, r, 7)), 5);
  test_rt_free(&rt);
}

TEST(apply, slow_thke_under_applied_partial_head_stays_flat) {
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
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_n(p), 2);
  ASSERT_EQ(pl_app_head(p), t->vstack[base]);
  ASSERT_EQ(pl_whnf(t, pl_apply(t, r, 7)), 5);
  t->vsp = base;
  test_rt_free(&rt);
}

TEST(apply, slow_thke_data_head_extends_row) {
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
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_n(p), 3);
  ASSERT_EQ(pl_app_head(p), 0);
  ASSERT_EQ(pl_app_args(p)[2], 9);
  test_rt_free(&rt);
}

TEST(apply, slow_thke_splices_partial_app_head) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* head is already a partial application: (K 5) applied to 9 */
  pl_vpush(t, test_app1(t, test_law(t, 2, 0, 1), 5));
  pl_vpush(t, 9);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_SLOW, 2, &t->vstack[base]);
  t->vsp = base;

  ASSERT_EQ(pl_whnf(t, thke), 5);
  test_rt_free(&rt);
}

TEST(apply, enter_hook_growth_refreshes_unresolved_pin_body) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 1, ax_s4('M', 'o', 'v', 'e'), 1));
  t->vstack[base] = pl_pin(t, t->vstack[base]);

  pl_set_enter_hook(growing_enter_hook);
  pl_val result = pl_apply(t, t->vstack[base], 42);
  pl_set_enter_hook(NULL);

  ASSERT_EQ(result, 42);
  pl_cell* proxy = pl_as(PL_TAG_PIN, t->vstack[base]);
  ASSERT_NOT_NULL(proxy);
  ASSERT(pl_pin_is_proxy(proxy));
  ASSERT_EQ(pl_pin_proxy_target(proxy), 0);
  ASSERT_NOT_NULL(pl_as(PL_TAG_LAW, pl_pin_body(proxy)));
  test_rt_free(&rt);
}

TEST(apply, tailcall_loops_in_constant_frame_space) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* loop x = loop x, as fused tail-call bytecode: must run in constant
   * frame space AND stay preemptable (the tail path takes a fuel step) */
  pl_vpush(t, test_law(t, 1, 0, 0));
  t->vstack[base] = pl_pin(t, t->vstack[base]);
  char err[192] = {0};
  ASSERT(pl_store_save_root(rt.store, t->vstack[base], NULL, err, sizeof(err)),
         "%s", err);
  pl_cell* proxy = pl_as(PL_TAG_PIN, t->vstack[base]);
  ASSERT_NOT_NULL(proxy);
  pl_val pin = pl_pin_proxy_target(proxy);
  ASSERT_NEQ(pin, 0); /* static bytecode literals must be non-moving */
  t->vsp = base;
  pl_cell* pp = pl_as(PL_TAG_PIN, pin);
  ASSERT_NOT_NULL(pp);
  static pl_op_t loop_ops[7];
  loop_ops[0] = OP_PUSH_LIT;
  loop_ops[1] = pin;
  loop_ops[2] = OP_PUSH_VAR;
  loop_ops[3] = 1;
  loop_ops[4] = OP_TAILCALL;
  loop_ops[5] = 2;
  loop_ops[6] = PL_BAN_FAST;
  static pl_code loop_code = {loop_ops, 7, 0, 0, 0};
  pl_pin_set_code(pp, &loop_code);

  size_t fcap0 = t->fcap;
  pl_vpush(t, pin);
  pl_vpush(t, 5);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 2, &t->vstack[base]);
  t->vsp = base;
  pl_thread_start(t, thke);
  for (int i = 0; i < 200; i++)
    ASSERT_EQ(pl_thread_run(t, 10000), PL_RUN_YIELDED);
  /* ~2M tail calls: frames grow only at fuel checkpoints (one F_UPD
   * per quantum survives until the loop would return), never per call */
  ASSERT_EQ(t->fcap, fcap0);
  ASSERT(t->fsp <= t->base_fsp + 220);
  pl_pin_set_code(pp, NULL); /* the code is a stack-lifetime fake */
  test_rt_free(&rt);
}

TEST(apply, slow_thke_over_applied_order) {
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

  ASSERT_EQ(pl_whnf(t, thke), 6);
  test_rt_free(&rt);
}

/* ── Recursive-let knots ───────────────────────────────────────────────── */

TEST(judge, environment_populates_self_slot) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 1, 0, 0)); /* f x = f */

  pl_val result = pl_apply(t, t->vstack[base], 9);
  ASSERT_EQ(result, t->vstack[base]);

  t->vsp = base;
  test_rt_free(&rt);
}

/*
 * f x = let b1 = b2; b2 = x in b1
 * body: (1 3 (1 1 2)) with slots [self=0, x=1, b1=2, b2=3].
 * Exercises the backpatched knot: b1's expression references the later
 * bind b2.
 */
TEST(judge, knot_forward_reference) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 1, 1, 2));               /* (1 1 2)   */
  pl_vpush(t, test_app2(t, 1, 3, t->vstack[base])); /* (1 3 ...) */
  pl_val f = test_law(t, 1, 0, t->vstack[base + 1]);
  ASSERT_EQ(pl_apply(t, f, 9), 9);
  test_rt_free(&rt);
}

/* f x = let b = b in b  ->  <<loop>> */
TEST(judge, self_referential_bind_raises_loop) {
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
    FAIL_TEST("expected <<loop>>");
  }
  pl_catch_unwind(t, &c);
  ASSERT_NOT_NULL(t->exn_msg);
  ASSERT_STR_EQ(t->exn_msg, "<<loop>>");
  test_rt_free(&rt);
}

/* ── Primop strictness ─────────────────────────────────────────────────── */

TEST(ops, strict_args_force_left_to_right) {
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
    FAIL_TEST("expected PLAN_EXN");
  }
  pl_catch_unwind(t, &c);
  ASSERT_NULL(t->exn_msg);
  ASSERT_EQ(t->exn, 7); /* arg 0 forced first */
  test_rt_free(&rt);
}

TEST(ops, lookup_segregates_opcode_set_and_argc) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_val arg1[1] = {0};
  test_expect_no_op66(t, ax_s3('A', 'd', 'd'), 1, arg1);
  test_expect_no_op66(t, ax_s4('R', 'e', 'c', 'v'), 1, arg1);
  test_rt_free(&rt);
}

TEST(ops, install_rejects_compiler_self_replacement) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, pl_pin(t, 42));
  char err[192] = {0};
  ASSERT(pl_store_save_root(rt.store, t->vstack[base], NULL, err, sizeof(err)),
         "%s", err);
  pl_val compiler = t->vstack[base];
  const uint8_t* hash = pl_pin_hash(compiler);

  /* Make the old, unguarded path idempotent so this test fails safely
   * instead of freeing the active machine out from under op_install. */
  memcpy(rt.store->compiler, hash, sizeof(rt.store->compiler));
  rt.store->compiler_f = hash[0] == 0 || memcmp(hash, hash + 1, 31) != 0;
  rt.store->compiler_t = t;

  bool raised = test_install_self_replacement_raises(t, compiler);

  rt.store->compiler_t = NULL;
  rt.store->compiler_f = false;
  memset(rt.store->compiler, 0, sizeof(rt.store->compiler));
  ASSERT(raised, "expected a compiler self-install to raise");
  ASSERT_STR_EQ(t->exn_msg, "Install: compiler cannot replace its own machine");
  test_rt_free(&rt);
}

TEST(ops, untaken_branches_stay_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_throwing(t, 7));
  /* If 0 <throw> 42  ->  42 */
  pl_val args[3] = {0, t->vstack[base], 42};
  ASSERT_EQ(test_op66(t, ax_s2('I', 'f'), 3, args), 42);
  /* And 0 <throw> -> 0; Or 1 <throw> -> 1 */
  ASSERT_EQ(test_op66_2(t, ax_s3('A', 'n', 'd'), 0, t->vstack[base]), 0);
  ASSERT_EQ(test_op66_2(t, ax_s2('O', 'r'), 1, t->vstack[base]), 1);
  test_rt_free(&rt);
}

TEST(ops, elim_case_branches_stay_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_throwing(t, 7));
  /* match _ _ _ z _ 0  ->  z, with every other branch throwing */
  pl_val th = t->vstack[base];
  pl_val args[6] = {th, th, th, 42, th, 0};
  ASSERT_EQ(test_op66(t, ax_s4('E', 'l', 'i', 'm'), 6, args), 42);
  test_rt_free(&rt);
}

TEST(ops, elim_decomposition_hands_out_lazy_args) {
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
  ASSERT_EQ(test_op66(t, ax_s4('E', 'l', 'i', 'm'), 6, args), 99);
  test_rt_free(&rt);
}

TEST(ops, seq_and_force) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  /* Seq 1 2 -> 2 */
  ASSERT_EQ(test_op66_2(t, ax_s3('S', 'e', 'q'), 1, 2), 2);
  /* Seq <throw> 2 raises */
  size_t base = t->vsp;
  pl_vpush(t, test_throwing(t, 7));
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_op66_2(t, ax_s3('S', 'e', 'q'), t->vstack[base], 2);
    FAIL_TEST("expected PLAN_EXN");
  }
  pl_catch_unwind(t, &c);
  ASSERT_EQ(t->exn, 7);
  test_rt_free(&rt);
}

TEST(ops, force_deep_normalizes_arg) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_val args[1] = {t->vstack[base]};
  pl_val r = test_op66(t, ax_s5('F', 'o', 'r', 'c', 'e'), 1, args);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_args(p)[0], 42);
  ASSERT((pl_hdr_flags(p[0]) & PL_F_NORMAL) != 0);
  test_rt_free(&rt);
}

TEST(ops, pin_keeps_nested_fields_lazy) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_val args[1] = {t->vstack[base]};
  pl_vpush(t, test_op66(t, ax_s3('P', 'i', 'n'), 1, args));

  pl_cell* proxy = pl_as(PL_TAG_PIN, t->vstack[base + 1]);
  ASSERT_NOT_NULL(proxy);
  pl_cell* body = pl_as(PL_TAG_APP, pl_pin_body(proxy));
  ASSERT_NOT_NULL(body);
  ASSERT_EQ(pl_tag(pl_app_args(body)[0]), PL_TAG_DEFER);
  ASSERT_EQ(pl_hdr_flags(proxy[0]) & PL_F_NORMAL, 0);
  test_rt_free(&rt);
}

TEST(ops, deepseq_normalizes_first_and_returns_second_lazily) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_vpush(t, test_throwing(t, 7));
  pl_vpush(t, test_app1(t, 0, t->vstack[base + 1]));
  pl_val r = test_op66_2(t, ax_s7('D', 'e', 'e', 'p', 'S', 'e', 'q'),
                         t->vstack[base], t->vstack[base + 2]);
  ASSERT_EQ(r, t->vstack[base + 2]);
  pl_cell* xp = pl_as(PL_TAG_APP, t->vstack[base]);
  ASSERT_NOT_NULL(xp);
  ASSERT_EQ(pl_app_args(xp)[0], 42);
  ASSERT((pl_hdr_flags(xp[0]) & PL_F_NORMAL) != 0);
  pl_cell* yp = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(yp);
  ASSERT_EQ(pl_tag(pl_app_args(yp)[0]), PL_TAG_DEFER);
  test_rt_free(&rt);
}

TEST(ops, trace_deep_normalizes_before_showing) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_vpush(t, test_throwing(t, 7));
  pl_vpush(t, test_app1(t, 0, t->vstack[base + 1]));
  pl_val r = test_op66_2(t, ax_s5('T', 'r', 'a', 'c', 'e'), t->vstack[base],
                         t->vstack[base + 2]);
  ASSERT_EQ(r, t->vstack[base + 2]);
  pl_cell* xp = pl_as(PL_TAG_APP, t->vstack[base]);
  ASSERT_NOT_NULL(xp);
  ASSERT_EQ(pl_app_args(xp)[0], 42);
  pl_cell* yp = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(yp);
  ASSERT_EQ(pl_tag(pl_app_args(yp)[0]), PL_TAG_DEFER);
  test_rt_free(&rt);
}

TEST(ops, try_catches_plan_exn_only) {
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
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_head(p), 1); /* Left: (1 exn) */
  ASSERT_EQ(pl_app_args(p)[0], 5);

  /* and the Right case */
  pl_vpush(t, test_law(t, 1, 0, 1)); /* id */
  pl_val args2[2] = {t->vstack[t->vsp - 1], 9};
  r = test_op66(t, ax_s3('T', 'r', 'y'), 2, args2);
  p = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_head(p), 0);
  ASSERT_EQ(pl_app_args(p)[0], 9);
  test_rt_free(&rt);
}

TEST(ops, equal_deep_and_pin_identity) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 0, 1, 2));
  pl_vpush(t, test_app2(t, 0, 1, 2));
  ASSERT_EQ(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base],
                        t->vstack[base + 1]),
            1);
  /* Pin construction is lazy: equal values get distinct proxies while PLAN
   * equality remains structural until Save gives them canonical hashes. */
  pl_vpush(t, pl_pin(t, t->vstack[base]));
  pl_vpush(t, pl_pin(t, t->vstack[base + 1]));
  ASSERT_NEQ(t->vstack[base + 2], t->vstack[base + 3]);
  ASSERT_EQ(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base + 2],
                        t->vstack[base + 3]),
            1);

  /* Save keeps the two public proxies distinct but deduplicates their
   * canonical target.  Equal must chase that target before its identity
   * check, including when only one operand is still the public proxy. */
  char err[192] = {0};
  ASSERT(
      pl_store_save_root(rt.store, t->vstack[base + 2], NULL, err, sizeof(err)),
      "%s", err);
  ASSERT(
      pl_store_save_root(rt.store, t->vstack[base + 3], NULL, err, sizeof(err)),
      "%s", err);
  pl_val canonical = pl_pin_proxy_target(pl_ptr(t->vstack[base + 2]));
  ASSERT_NEQ(canonical, 0);
  ASSERT_EQ(canonical, pl_pin_proxy_target(pl_ptr(t->vstack[base + 3])));
  ASSERT_EQ(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base + 2],
                        t->vstack[base + 3]),
            1);
  ASSERT_EQ(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base + 2],
                        canonical),
            1);
  test_rt_free(&rt);
}

TEST(ops, equal_deep_normalizes_second_arg) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1_thunk_to(t, 42));
  pl_vpush(t, test_app1_thunk_to(t, 42));
  ASSERT_EQ(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base],
                        t->vstack[base + 1]),
            1);
  test_rt_free(&rt);
}

TEST(ops, row_elements_stay_lazy) {
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
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_n(p), 1);
  pl_vpush(t, r);
  /* forcing the element gives Ix0 of the stream = 5 */
  pl_cell* rp = pl_as(PL_TAG_APP, t->vstack[base + 2]);
  ASSERT_NOT_NULL(rp);
  pl_val e = pl_whnf(t, pl_app_args(rp)[0]);
  ASSERT_EQ(e, 5);
  test_rt_free(&rt);
}

TEST(ops, whole_row_slice_reuses_only_exact_result) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_val items[3] = {11, 22, 33};
  pl_vpush(t, test_app(t, 0, 3, items));

  pl_val exact_args[3] = {0, 3, t->vstack[base]};
  pl_val exact = test_op66(t, ax_s5('S', 'l', 'i', 'c', 'e'), 3, exact_args);
  ASSERT_EQ(exact, t->vstack[base]);

  pl_val oversized_args[3] = {0, 99, t->vstack[base]};
  pl_val oversized =
      test_op66(t, ax_s5('S', 'l', 'i', 'c', 'e'), 3, oversized_args);
  ASSERT_EQ(oversized, t->vstack[base]);

  pl_vpush(t, test_app(t, 7, 3, items));
  pl_val rehead_args[3] = {0, 3, t->vstack[base + 1]};
  pl_val reheaded =
      test_op66(t, ax_s5('S', 'l', 'i', 'c', 'e'), 3, rehead_args);
  ASSERT_NEQ(reheaded, t->vstack[base + 1]);
  pl_cell* p = pl_as(PL_TAG_APP, reheaded);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_head(p), 0);
  ASSERT_EQ(pl_app_n(p), 3);

  test_rt_free(&rt);
}

TEST(ops, scan8_handles_both_polarities_and_stops_at_rejected_byte) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static const uint8_t source[] = {'a', 'a', 'a', 'b'};
  static const uint8_t klass[] = {'a'};
  pl_vpush(t, pl_nat_from_bytes(t, source, sizeof(source)));
  pl_vpush(t, test_byte_mask(t, klass, sizeof(klass)));

  pl_val inside_args[4] = {
      t->vstack[base],
      0,
      t->vstack[base + 1],
      1,
  };
  pl_val inside = test_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 4, inside_args);
  test_assert_nat_row3(inside, 3, 0, 3);

  pl_val outside_args[4] = {
      t->vstack[base],
      3,
      t->vstack[base + 1],
      0,
  };
  pl_val outside =
      test_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 4, outside_args);
  test_assert_nat_row3(outside, 4, 0, 1);
  test_rt_free(&rt);
}

TEST(ops, scan8_counts_lf_and_tracks_column_from_nonzero_start) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static const uint8_t source[] = {'x', 'x',  'a', '\n', 'b',
                                   'c', '\n', 'z', '!'};
  uint8_t all_bytes[32];
  memset(all_bytes, 0xff, sizeof(all_bytes));
  pl_vpush(t, pl_nat_from_bytes(t, source, sizeof(source)));
  pl_vpush(t, pl_nat_from_bytes(t, all_bytes, sizeof(all_bytes)));

  pl_val args[4] = {
      t->vstack[base],
      2,
      t->vstack[base + 1],
      42,
  };
  pl_val out = test_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 4, args);
  test_assert_nat_row3(out, 9, 2, 2);
  test_rt_free(&rt);
}

TEST(ops, scan8_preserves_eof_and_past_end_start) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static const uint8_t source[] = {'a', 'b', 'c'};
  pl_vpush(t, pl_nat_from_bytes(t, source, sizeof(source)));

  pl_val eof_args[4] = {t->vstack[base], 3, 0, 0};
  pl_val eof = test_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 4, eof_args);
  test_assert_nat_row3(eof, 3, 0, 0);

  uint8_t large_start_bytes[9] = {0};
  large_start_bytes[8] = 1;
  pl_vpush(t,
           pl_nat_from_bytes(t, large_start_bytes, sizeof(large_start_bytes)));
  pl_val past_args[4] = {t->vstack[base], t->vstack[base + 1], 0, 0};
  pl_val past = test_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 4, past_args);
  test_assert_nat_row3(past, t->vstack[base + 1], 0, 0);

  pl_val empty_args[4] = {0, 0, 0, 1};
  pl_val empty = test_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 4, empty_args);
  test_assert_nat_row3(empty, 0, 0, 0);
  test_rt_free(&rt);
}

TEST(ops, scan8_uses_all_256_mask_bits) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static const uint8_t source[] = {0xff, 'x'};
  static const uint8_t klass[] = {0xff};
  pl_vpush(t, pl_nat_from_bytes(t, source, sizeof(source)));
  pl_vpush(t, test_byte_mask(t, klass, sizeof(klass)));
  pl_val args[4] = {
      t->vstack[base],
      0,
      t->vstack[base + 1],
      1,
  };
  pl_val out = test_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 4, args);
  test_assert_nat_row3(out, 1, 0, 1);
  test_rt_free(&rt);
}

TEST(ops, strtree_text_returns_its_string) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
  pl_vpush(t, pl_nat_from_bytes(t, hello, sizeof(hello)));
  pl_vpush(t, test_cord_text(t, t->vstack[base]));

  pl_val args[1] = {t->vstack[base + 1]};
  pl_val out = test_op66(t, ax_s7('S', 't', 'r', 'T', 'r', 'e', 'e'), 1, args);
  ASSERT(pl_nat_eq(out, t->vstack[base]));
  test_rt_free(&rt);
}

TEST(ops, strtree_materializes_all_constructors_and_shared_subtrees) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
  static const uint8_t source[] = {'s', 'o', 'u', 'r', 'c', 'e'};
  static const uint8_t expected[] = {
      'h', 'e', 'l', 'l', 'o', 'o', 'u', 'r', '!', '!', 'h', 'e', 'l', 'l', 'o',
  };
  pl_vpush(t, pl_nat_from_bytes(t, hello, sizeof(hello)));
  pl_vpush(t, pl_nat_from_bytes(t, source, sizeof(source)));
  pl_vpush(t, test_cord_text(t, t->vstack[base]));
  pl_vpush(t, test_cord_slice(t, t->vstack[base + 1], 1, 3));
  pl_vpush(t, test_cord_repeat(t, '!', 2));
  pl_vpush(t, test_cord_cat(t, t->vstack[base + 2], t->vstack[base + 3]));
  pl_vpush(t, test_cord_cat(t, t->vstack[base + 4], t->vstack[base + 2]));
  pl_vpush(t, test_cord_cat(t, t->vstack[base + 5], t->vstack[base + 6]));

  pl_val args[1] = {t->vstack[base + 7]};
  pl_val out = test_op66(t, ax_s7('S', 't', 'r', 'T', 'r', 'e', 'e'), 1, args);
  ASSERT_EQ(pl_nat_byte_len(out), sizeof(expected));
  for (size_t i = 0; i < sizeof(expected); i++)
    ASSERT_EQ(pl_nat_byte_at(out, i), expected[i]);
  test_rt_free(&rt);
}

TEST(ops, strtree_traversal_is_stack_safe) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_cord_repeat(t, 'x', 1));
  pl_vpush(t, t->vstack[base]);
  for (size_t i = 0; i < 4096; i++) {
    pl_val next = test_cord_cat(t, t->vstack[base + 1], t->vstack[base]);
    t->vstack[base + 1] = next;
  }

  pl_val args[1] = {t->vstack[base + 1]};
  pl_val out = test_op66(t, ax_s7('S', 't', 'r', 'T', 'r', 'e', 'e'), 1, args);
  ASSERT_EQ(pl_nat_byte_len(out), 4097);
  ASSERT_EQ(pl_nat_byte_at(out, 0), 'x');
  ASSERT_EQ(pl_nat_byte_at(out, 4096), 'x');
  test_rt_free(&rt);
}

TEST(ops, strtree_rejects_malformed_nodes_and_wrong_arity) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app1(t, 0, 0));
  pl_vpush(t, test_cord_text(t, t->vstack[base]));
  pl_val args[1] = {t->vstack[base + 1]};
  ASSERT_EQ(test_op66(t, ax_s7('S', 't', 'r', 'T', 'r', 'e', 'e'), 1, args), 0);

  pl_val bad_fields[2] = {ax_s3('b', 'a', 'd'), 0};
  pl_vpush(t, test_app(t, 0, 2, bad_fields));
  pl_val bad_args[1] = {t->vstack[base + 2]};
  ASSERT_EQ(test_op66(t, ax_s7('S', 't', 'r', 'T', 'r', 'e', 'e'), 1, bad_args),
            0);

  pl_val old_fields[1] = {0};
  pl_vpush(t, test_app(t, ax_s4('t', 'e', 'x', 't'), 1, old_fields));
  pl_val old_args[1] = {t->vstack[base + 3]};
  ASSERT_EQ(test_op66(t, ax_s7('S', 't', 'r', 'T', 'r', 'e', 'e'), 1, old_args),
            0);

  pl_val wrong_args[2] = {0, 0};
  test_expect_no_op66(t, ax_s7('S', 't', 'r', 'T', 'r', 'e', 'e'), 2,
                      wrong_args);
  pl_val scan_args[3] = {0, 0, 0};
  test_expect_no_op66(t, ax_s5('s', 'c', 'a', 'n', '8'), 3, scan_args);
  test_rt_free(&rt);
}

TEST(ops, law_op_adds_one_to_arity) {
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
  ASSERT_NOT_NULL(lp);
  ASSERT_EQ(pl_law_arity(lp), 2); /* nat a + 1 */
  test_rt_free(&rt);
}

/* ── nf ────────────────────────────────────────────────────────────────── */

TEST(nf, deep_normalization_snaps_thunks) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* row of one thunk that evaluates to 42 */
  pl_vpush(t, test_thunk(t, test_app1(t, 0, 42)));
  pl_vpush(t, test_app1(t, 0, t->vstack[base]));
  pl_val r = pl_nf(t, t->vstack[base + 1]);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_args(p)[0], 42); /* snapped, no IND left */
  ASSERT((pl_hdr_flags(p[0]) & PL_F_NORMAL) != 0);
  test_rt_free(&rt);
}

/* ── S2 opcodes: FORCE / PUSH_SLOT / BR / JMP / CALL / NOUPD ─────────── */

/* Make a saved law pin with arity and set its code; returns the pin
 * rooted at the current vsp (caller owns the slot). */
/* Distinct `name`s keep otherwise-identical laws from interning to the
 * same canonical pin (and silently sharing one code slot). */
static pl_val test_code_pin_named(test_rt* rt, uint64_t arity, uint64_t name,
                                  pl_code* code) {
  pl_thread* t = rt->t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, arity, name, 0));
  t->vstack[base] = pl_pin(t, t->vstack[base]);
  char err[192] = {0};
  ASSERT(pl_store_save_root(rt->store, t->vstack[base], NULL, err, sizeof(err)),
         "%s", err);
  pl_cell* proxy = pl_as(PL_TAG_PIN, t->vstack[base]);
  ASSERT_NOT_NULL(proxy);
  pl_val pin = pl_pin_proxy_target(proxy);
  ASSERT_NEQ(pin, 0);
  t->vstack[base] = pin;
  pl_pin_set_code(pl_as(PL_TAG_PIN, pin), code);
  return pin;
}

static pl_val test_code_pin(test_rt* rt, uint64_t arity, pl_code* code) {
  return test_code_pin_named(rt, arity, 0, code);
}

static pl_val test_run_call1(pl_thread* t, pl_val pin, pl_val arg) {
  size_t base = t->vsp;
  pl_vpush(t, pin);
  pl_vpush(t, arg);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 2, &t->vstack[base]);
  t->vsp = base;
  pl_thread_start(t, thke);
  pl_run_status s;
  while ((s = pl_thread_run(t, 100000)) == PL_RUN_YIELDED)
    ;
  ASSERT_EQ(s, PL_RUN_DONE);
  return t->result;
}

TEST(exec, force_delivers_whnf_result) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static pl_op_t ops[4] = {OP_PUSH_VAR, 1, OP_FORCE, OP_RET};
  static pl_code code = {ops, 4, 0, 0, 0};
  pl_val pin = test_code_pin(&rt, 1, &code);
  (void)pin;
  pl_vpush(t, test_thunk(t, 42)); /* lazy arg, forced by OP_FORCE */
  pl_val r = test_run_call1(t, t->vstack[base], t->vstack[base + 1]);
  ASSERT_EQ(r, 42);
  /* already-WHNF operand passes through unchanged */
  r = test_run_call1(t, t->vstack[base], 7);
  ASSERT_EQ(r, 7);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  test_rt_free(&rt);
}

TEST(exec, push_slot_duplicates_operand) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static pl_op_t ops[8] = {OP_PUSH_VAR, 1,         OP_FORCE, OP_PUSH_SLOT,
                           0,           OP_MK_APP, 1,        OP_RET};
  static pl_code code = {ops, 8, 0, 0, 0};
  test_code_pin(&rt, 1, &code);
  pl_val r = test_run_call1(t, t->vstack[base], 42);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_head(p), 42);
  ASSERT_EQ(pl_app_n(p), 1);
  ASSERT_EQ(pl_app_args(p)[0], 42);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  test_rt_free(&rt);
}

TEST(exec, br_selects_arm_and_bounds) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static pl_op_t ops[13] = {OP_PUSH_VAR, 1,  OP_FORCE,    OP_BR, 2,
                            7,           10, OP_PUSH_LIT, 10,    OP_RET,
                            OP_PUSH_LIT, 20, OP_RET};
  static pl_code code = {ops, 13, 0, 0, 0};
  test_code_pin(&rt, 1, &code);
  ASSERT_EQ(test_run_call1(t, t->vstack[base], 0), 10);
  ASSERT_EQ(test_run_call1(t, t->vstack[base], 1), 20);
  /* out-of-range scrutinee raises */
  size_t mark = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 2);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 2, &t->vstack[mark]);
  t->vsp = mark;
  pl_thread_start(t, thke);
  pl_run_status s;
  while ((s = pl_thread_run(t, 100000)) == PL_RUN_YIELDED)
    ;
  ASSERT_EQ(s, PL_RUN_EXN);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  test_rt_free(&rt);
}

TEST(exec, jmp_loop_stays_preemptable) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static pl_op_t ops[3] = {OP_JMP, 0, OP_RET};
  static pl_code code = {ops, 3, 0, 0, 0};
  test_code_pin(&rt, 1, &code);
  size_t fcap0 = t->fcap;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 5);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 2, &t->vstack[base + 1]);
  t->vsp = base + 1;
  pl_thread_start(t, thke);
  for (int i = 0; i < 50; i++)
    ASSERT_EQ(pl_thread_run(t, 10000), PL_RUN_YIELDED);
  ASSERT_EQ(t->fcap, fcap0); /* the loop runs in constant frame space */
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  test_rt_free(&rt);
}

TEST(exec, call_local_block_returns_forced) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* main: [0, arg] -> call sub(arg) -> [0, arg'] -> row [arg']
   * sub (at 10): force its stack argument and return it */
  static pl_op_t ops[14] = {OP_PUSH_LIT,  0, OP_PUSH_VAR, 1,     OP_CALL,
                            10,           1, OP_MK_APP,   1,     OP_RET,
                            OP_PUSH_SLOT, 0, OP_FORCE,    OP_RET};
  static pl_code code = {ops, 14, 0, 0, 0};
  test_code_pin(&rt, 1, &code);
  pl_vpush(t, test_thunk(t, 42));
  pl_val r = test_run_call1(t, t->vstack[base], t->vstack[base + 1]);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_head(p), 0);
  ASSERT_EQ(pl_app_n(p), 1);
  ASSERT_EQ(pl_app_args(p)[0], 42);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  test_rt_free(&rt);
}

TEST(exec, noupd_thke_reevaluates) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* the code allocates a fresh row [x] on every execution */
  static pl_op_t ops[7] = {OP_PUSH_LIT, 0, OP_PUSH_VAR, 1,
                           OP_MK_APP,   1, OP_RET};
  static pl_code code = {ops, 7, 0, 0, 0};
  test_code_pin(&rt, 1, &code);
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 7);
  pl_gc_reserve(t, 2 * PL_THKE_CELLS(2));
  PL_GC_FORBID(t);
  pl_val no_upd =
      pl_mk_thke(t, 0, PL_BAN_FAST | PL_BAN_NOUPD, 2, &t->vstack[base + 1]);
  pl_val upd = pl_mk_thke(t, 0, PL_BAN_FAST, 2, &t->vstack[base + 1]);
  PL_GC_ALLOW(t);
  t->vsp = base + 1;
  pl_vpush(t, no_upd);
  pl_vpush(t, upd);
  for (int round = 0; round < 2; round++) {
    /* a NOUPD thke survives forcing un-updated, and re-evaluates */
    pl_thread_start(t, t->vstack[base + 1]);
    pl_run_status s;
    while ((s = pl_thread_run(t, 100000)) == PL_RUN_YIELDED)
      ;
    ASSERT_EQ(s, PL_RUN_DONE);
    pl_cell* rp = pl_as(PL_TAG_APP, t->result);
    ASSERT_NOT_NULL(rp);
    ASSERT_EQ(pl_app_args(rp)[0], 7);
    ASSERT_EQ(pl_hdr_kind(*pl_ptr(t->vstack[base + 1])), PL_K_THKE);
  }
  /* the updateable control is overwritten by its first force */
  pl_thread_start(t, t->vstack[base + 2]);
  pl_run_status s;
  while ((s = pl_thread_run(t, 100000)) == PL_RUN_YIELDED)
    ;
  ASSERT_EQ(s, PL_RUN_DONE);
  ASSERT_NEQ(pl_hdr_kind(*pl_ptr(t->vstack[base + 2])), PL_K_THKE);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  test_rt_free(&rt);
}

TEST(exec, ingest_validates_targets_and_guards_fusion) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* BR arm 1 targets the RET behind a MK_THK: decode must NOT fuse */
  pl_val guarded[13] = {OP_PUSH_VAR, 1,           OP_FORCE,    OP_BR, 2,
                        7,           12,          OP_PUSH_VAR, 1,     OP_MK_THK,
                        1,           PL_BAN_SLOW, OP_RET};
  pl_vpush(t, test_app(t, 0, 13, guarded));
  pl_code* c = pl_bytecode_from_val(t->vstack[base]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->ops[9], OP_MK_THK);
  pl_bytecode_free(c);
  /* same program, arm 1 rejoining arm 0 instead: the MK_THK fuses */
  pl_val fused[13] = {OP_PUSH_VAR, 1, OP_FORCE,  OP_BR, 2,           7,     7,
                      OP_PUSH_VAR, 1, OP_MK_THK, 1,     PL_BAN_SLOW, OP_RET};
  pl_vpush(t, test_app(t, 0, 13, fused));
  c = pl_bytecode_from_val(t->vstack[base + 1]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->ops[9], OP_TAILCALL);
  pl_bytecode_free(c);
  /* a target into an operand slot fails the decode */
  pl_val bad[6] = {OP_JMP, 4, OP_RET, OP_PUSH_LIT, 9, OP_RET};
  pl_vpush(t, test_app(t, 0, 6, bad));
  ASSERT_EQ(pl_bytecode_from_val(t->vstack[base + 2]), NULL);
  /* a target out of range fails the decode */
  pl_val oob[3] = {OP_JMP, 99, OP_RET};
  pl_vpush(t, test_app(t, 0, 3, oob));
  ASSERT_EQ(pl_bytecode_from_val(t->vstack[base + 3]), NULL);
  test_rt_free(&rt);
}

TEST(exec, strict_entry_dual_entrypoints) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* checked prologue forces arg 1; OP_ENTRY 1 marks the fast entry;
   * the body never touches the argument */
  static pl_op_t ops[8] = {OP_PUSH_VAR, 1,           OP_FORCE, OP_ENTRY,
                           1,           OP_PUSH_LIT, 5,        OP_RET};
  static pl_code code = {ops, 8, 1, 5, 0};
  test_code_pin(&rt, 1, &code);
  /* decode of an equivalent row records mask + entry */
  {
    pl_val row[8] = {OP_PUSH_VAR, 1,           OP_FORCE, OP_ENTRY,
                     1,           OP_PUSH_LIT, 5,        OP_RET};
    pl_vpush(t, test_app(t, 0, 8, row));
    pl_code* c = pl_bytecode_from_val(t->vstack[base + 1]);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->strict_mask, 1);
    ASSERT_EQ(c->strict_entry, 5);
    pl_bytecode_free(c);
    t->vsp = base + 1;
  }
  pl_run_status s;
  /* checked entry (no hint): the prologue forces the raising thunk */
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, test_throwing(t, 77));
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val thke = pl_mk_thke(t, 0, PL_BAN_FAST, 2, &t->vstack[base + 1]);
  t->vsp = base + 1;
  pl_thread_start(t, thke);
  while ((s = pl_thread_run(t, 100000)) == PL_RUN_YIELDED)
    ;
  ASSERT_EQ(s, PL_RUN_EXN);
  /* matching hint: fast entry skips the force entirely */
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, test_throwing(t, 77));
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  thke = pl_mk_thke(t, 0, PL_BAN_FAST | (1u << 8), 2, &t->vstack[base + 1]);
  t->vsp = base + 1;
  pl_thread_start(t, thke);
  while ((s = pl_thread_run(t, 100000)) == PL_RUN_YIELDED)
    ;
  ASSERT_EQ(s, PL_RUN_DONE);
  ASSERT_EQ(t->result, 5);
  /* mismatched hint degrades to the checked entry */
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, test_throwing(t, 77));
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  thke = pl_mk_thke(t, 0, PL_BAN_FAST | (2u << 8), 2, &t->vstack[base + 1]);
  t->vsp = base + 1;
  pl_thread_start(t, thke);
  while ((s = pl_thread_run(t, 100000)) == PL_RUN_YIELDED)
    ;
  ASSERT_EQ(s, PL_RUN_EXN);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  test_rt_free(&rt);
}

/* ── Direct call opcodes: CALL_KNOWN / CALL_FAST / CALL_SLOW ─────────── */

TEST(exec, call_known_runs_resolved_primop_direct) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* decode resolves (opset 66, "Add", 2); law computes arg + 1 */
  pl_vpush(t, pl_pin(t, 66));
  {
    pl_val row[10] = {OP_PUSH_VAR,
                      1,
                      OP_FORCE,
                      OP_PUSH_LIT,
                      1,
                      OP_CALL_KNOWN,
                      2,
                      t->vstack[base],
                      UINT64_C(0x646441) /* "Add" */,
                      OP_RET};
    pl_vpush(t, test_app(t, 0, 10, row));
  }
  pl_code* c = pl_bytecode_from_val(t->vstack[base + 1]);
  ASSERT_NOT_NULL(c);
  t->vsp = base;
  pl_val pin = test_code_pin(&rt, 1, c);
  ASSERT_EQ(test_run_call1(t, pin, 41), 42);
  /* a lazy arg is forced by the op's own strict-arg driver */
  pl_vpush(t, test_thunk(t, 6));
  ASSERT_EQ(test_run_call1(t, t->vstack[base], t->vstack[base + 1]), 7);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  pl_bytecode_free(c);
  test_rt_free(&rt);
}

TEST(exec, call_fast_enters_law_direct) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* callee: force and return its argument */
  static pl_op_t id_ops[4] = {OP_PUSH_VAR, 1, OP_FORCE, OP_RET};
  static pl_code id_code = {id_ops, 4, 0, 0, 0};
  pl_val callee = test_code_pin_named(&rt, 1, 111, &id_code);
  /* caller: direct call of the callee on its (lazy) argument */
  pl_op_t caller_ops[8] = {OP_PUSH_LIT,  callee, OP_PUSH_VAR, 1,
                           OP_CALL_FAST, 1,      0,           OP_RET};
  pl_code caller_code = {caller_ops, 8, 0, 0, 0};
  test_code_pin(&rt, 1, &caller_code);
  ASSERT_EQ(test_run_call1(t, t->vstack[base + 1], 42), 42);
  pl_vpush(t, test_thunk(t, 9));
  ASSERT_EQ(test_run_call1(t, t->vstack[base + 1], t->vstack[base + 2]), 9);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base + 1]), NULL);
  test_rt_free(&rt);
}

TEST(exec, call_fast_arity_mismatch_degrades_to_apply) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* head: an arity-2 law pin (its code never runs — the direct call
   * supplies one arg, so verification rejects it and the generic
   * apply path yields the WHNF partial application) */
  static pl_op_t id_ops[4] = {OP_PUSH_VAR, 1, OP_FORCE, OP_RET};
  static pl_code id_code = {id_ops, 4, 0, 0, 0};
  pl_val head2 = test_code_pin(&rt, 2, &id_code);
  pl_op_t caller_ops[8] = {OP_PUSH_LIT,  head2, OP_PUSH_VAR, 1,
                           OP_CALL_FAST, 1,     0,           OP_RET};
  pl_code caller_code = {caller_ops, 8, 0, 0, 0};
  test_code_pin(&rt, 1, &caller_code);
  pl_val r = test_run_call1(t, t->vstack[base + 1], 42);
  pl_cell* p = pl_as(PL_TAG_APP, r);
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(pl_app_n(p), 1);
  ASSERT_EQ(pl_app_args(p)[0], 42);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base + 1]), NULL);
  test_rt_free(&rt);
}

TEST(exec, call_slow_forces_head_then_applies) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  static pl_op_t id_ops[4] = {OP_PUSH_VAR, 1, OP_FORCE, OP_RET};
  static pl_code id_code = {id_ops, 4, 0, 0, 0};
  pl_val callee = test_code_pin_named(&rt, 1, 222, &id_code);
  (void)callee;
  /* caller: head is a lazy thunk (forced by the slow path) */
  pl_op_t caller_ops[7] = {OP_PUSH_VAR,  1, OP_PUSH_LIT, 9,
                           OP_CALL_SLOW, 1, OP_RET};
  pl_code caller_code = {caller_ops, 7, 0, 0, 0};
  test_code_pin(&rt, 1, &caller_code);
  pl_vpush(t, test_thunk(t, t->vstack[base]));
  ASSERT_EQ(test_run_call1(t, t->vstack[base + 1], t->vstack[base + 2]), 9);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base]), NULL);
  pl_pin_set_code(pl_as(PL_TAG_PIN, t->vstack[base + 1]), NULL);
  test_rt_free(&rt);
}

/* ── op 66 Memo (doc/sigoflaw-memo-spec.md) ────────────────────────────── */

/* Pin v and persist it so the pin carries a content hash. */
static pl_val test_saved_pin(test_rt* rt, pl_val v) {
  pl_thread* t = rt->t;
  size_t base = t->vsp;
  pl_vpush(t, v);
  t->vstack[base] = pl_pin(t, t->vstack[base]);
  char err[192] = {0};
  ASSERT(pl_store_save_root(rt->store, t->vstack[base], NULL, err, sizeof(err)),
         "%s", err);
  pl_val pin = t->vstack[base];
  t->vsp = base;
  return pin;
}

static pl_val test_memo(pl_thread* t, pl_val f, pl_val x) {
  return test_op66_2(t, ax_s4('M', 'e', 'm', 'o'), f, x);
}

TEST(memo, unpinned_args_apply_without_caching) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  uint64_t rec0;
  uint64_t rec1;
  pl_memo_stats(NULL, NULL, &rec0);
  /* raw (unpinned) law: (Memo f 7) is exactly (f 7), never recorded */
  pl_vpush(t, test_law(t, 1, ax_s4('m', 'm', '_', 'a'), test_app1(t, 0, 42)));
  ASSERT_EQ(test_memo(t, t->vstack[base], 7), 42);
  pl_memo_stats(NULL, NULL, &rec1);
  ASSERT_EQ(rec0, rec1);
  test_rt_free(&rt);
}

TEST(memo, pinned_nat_result_is_recorded_and_served) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 1, ax_s4('m', 'm', '_', 'b'), test_app1(t, 0, 42)));
  t->vstack[base] = test_saved_pin(&rt, t->vstack[base]);
  pl_vpush(t, test_saved_pin(&rt, ax_s4('m', 'm', 'x', 'b')));
  ASSERT_EQ(test_memo(t, t->vstack[base], t->vstack[base + 1]), 42);
  uint64_t got = 0;
  ASSERT(pl_memo_probe(pl_pin_hash(t->vstack[base]),
                       pl_pin_hash(t->vstack[base + 1]), &got));
  ASSERT_EQ(got, 42);
  test_rt_free(&rt);
}

TEST(memo, cached_value_is_served_without_reevaluation) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* plant a sentinel for a never-evaluated pair: a probe hit must be
   * served verbatim, proving the serve path short-circuits the apply */
  pl_vpush(t, test_law(t, 1, ax_s4('m', 'm', '_', 'c'), test_app1(t, 0, 42)));
  t->vstack[base] = test_saved_pin(&rt, t->vstack[base]);
  pl_vpush(t, test_saved_pin(&rt, ax_s4('m', 'm', 'x', 'c')));
  pl_memo_record(pl_pin_hash(t->vstack[base]), pl_pin_hash(t->vstack[base + 1]),
                 99);
  ASSERT_EQ(test_memo(t, t->vstack[base], t->vstack[base + 1]), 99);
  test_rt_free(&rt);
}

TEST(memo, effectful_evaluation_is_never_recorded) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* f's body runs (Trace 1 5): the effect-epoch bump must block the
   * record even though the result (5) is a cacheable nat63 */
  pl_val trace_args[2] = {1, 5};
  pl_vpush(t, test_app(t, ax_s5('T', 'r', 'a', 'c', 'e'), 2, trace_args));
  pl_vpush(t, test_app1(t, 0, t->vstack[base])); /* (0 row) literal */
  pl_vpush(t, test_app1(t, 0, test_p66(t)));     /* (0 P66) literal */
  pl_vpush(t, test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]));
  pl_vpush(t, test_law(t, 1, ax_s4('m', 'm', '_', 'd'), t->vstack[base + 3]));
  t->vstack[base] = test_saved_pin(&rt, t->vstack[base + 4]);
  t->vsp = base + 1;
  pl_vpush(t, test_saved_pin(&rt, ax_s4('m', 'm', 'x', 'd')));
  ASSERT_EQ(test_memo(t, t->vstack[base], t->vstack[base + 1]), 5);
  uint64_t got = 0;
  ASSERT(!pl_memo_probe(pl_pin_hash(t->vstack[base]),
                        pl_pin_hash(t->vstack[base + 1]), &got));
  test_rt_free(&rt);
}

TEST(memo, non_nat_result_evaluates_but_never_caches) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* identity law: (f x) is the x pin itself — not a nat63, so the
   * application runs uncached and nothing is recorded */
  pl_vpush(t, test_law(t, 1, ax_s4('m', 'm', '_', 'e'), 1));
  t->vstack[base] = test_saved_pin(&rt, t->vstack[base]);
  pl_vpush(t,
           test_saved_pin(&rt, test_law(t, 2, ax_s4('m', 'm', 'x', 'e'), 1)));
  pl_val r = test_memo(t, t->vstack[base], t->vstack[base + 1]);
  ASSERT(!pl_is_nat63(r));
  ASSERT_NOT_NULL(pl_as(PL_TAG_PIN, r));
  ASSERT(memcmp(pl_pin_hash(r), pl_pin_hash(t->vstack[base + 1]), 32) == 0);
  uint64_t got = 0;
  ASSERT(!pl_memo_probe(pl_pin_hash(t->vstack[base]),
                        pl_pin_hash(t->vstack[base + 1]), &got));
  test_rt_free(&rt);
}

TEST(ops, equal_survives_very_deep_structures) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* two structurally-equal 200k-deep app chains (distinct cells): the
   * old recursive pl_eq_deep overflowed the C stack near ~80k */
  enum { DEPTH = 200000 };
  pl_vpush(t, 7);
  pl_vpush(t, 7);
  pl_vpush(t, 8);
  for (int i = 0; i < DEPTH; i++) {
    t->vstack[base] = test_app1(t, 0, t->vstack[base]);
    t->vstack[base + 1] = test_app1(t, 0, t->vstack[base + 1]);
    t->vstack[base + 2] = test_app1(t, 0, t->vstack[base + 2]);
  }
  ASSERT_EQ(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base],
                        t->vstack[base + 1]),
            1);
  /* same shape, different leaf */
  ASSERT_EQ(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base],
                        t->vstack[base + 2]),
            0);
  test_rt_free(&rt);
}
