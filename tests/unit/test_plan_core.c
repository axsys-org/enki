#include <criterion/criterion.h>
#include <string.h>

#include "test_plan.h"

/* ── Value representation ──────────────────────────────────────────────── */

Test(value, direct_nat_boundary) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  pl_gc_reserve(t, PL_NAT_CELLS(1));
  pl_val small = pl_mk_nat_u64(t, PL_NAT63_MAX);
  cr_assert(pl_is_nat63(small));

  pl_gc_reserve(t, PL_NAT_CELLS(1));
  pl_val big = pl_mk_nat_u64(t, PL_NAT63_MAX + 1);
  cr_assert(!pl_is_nat63(big));
  cr_assert_eq(pl_tag(big), PL_TAG_NAT);
  cr_assert(pl_is_whnf(big));
  cr_assert_eq(pl_nat_u64_clamp(big), PL_NAT63_MAX + 1);

  test_rt_free(&rt);
}

Test(value, nat_trim_canonicalizes) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  pl_gc_reserve(t, PL_NAT_CELLS(3));
  uint64_t* limbs;
  pl_val v = pl_mk_nat_limbs(t, 3, &limbs);
  limbs[0] = 42;
  limbs[1] = 0;
  limbs[2] = 0;
  v = pl_nat_trim(v);
  cr_assert_eq(v, 42); /* trims to a direct nat */

  test_rt_free(&rt);
}

Test(value, app_need_cache) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  pl_val law = test_law(t, 3, 0, 1);
  cr_assert_eq(pl_arity(law), 3);
  pl_val a1 = test_app1(t, law, 7);
  cr_assert_eq(pl_arity(a1), 2);

  pl_vpush(t, a1);
  pl_gc_reserve(t, PL_APP_CELLS(2));
  pl_val a2 = pl_mk_app_snoc(t, pl_vpop(t), 8);
  cr_assert_eq(pl_arity(a2), 1);

  /* inert app: nat head has arity 0 */
  pl_val row = test_app2(t, 0, 1, 2);
  cr_assert_eq(pl_arity(row), 0);

  test_rt_free(&rt);
}

/* ── Nat arithmetic ────────────────────────────────────────────────────── */

Test(nat, add_carries_across_boundary) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  size_t a = t->vsp;
  pl_vpush(t, PL_NAT63_MAX);
  pl_vpush(t, 1);
  pl_val sum = pl_nat_add(t, &t->vstack[a], &t->vstack[a + 1]);
  cr_assert(!pl_is_nat63(sum));
  cr_assert_eq(pl_nat_u64_clamp(sum), UINT64_C(1) << 63);

  /* and back below the boundary via sub */
  t->vsp = a;
  pl_vpush(t, sum);
  pl_vpush(t, 1);
  pl_val back = pl_nat_sub(t, &t->vstack[a], &t->vstack[a + 1]);
  cr_assert(pl_is_nat63(back));
  cr_assert_eq(back, PL_NAT63_MAX);

  test_rt_free(&rt);
}

Test(nat, sub_floors_at_zero) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t a = t->vsp;
  pl_vpush(t, 3);
  pl_vpush(t, 5);
  cr_assert_eq(pl_nat_sub(t, &t->vstack[a], &t->vstack[a + 1]), 0);
  test_rt_free(&rt);
}

Test(nat, mul_two_limbs) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t a = t->vsp;
  pl_vpush(t, UINT64_C(0xffffffffffffffff) >> 1);
  pl_vpush(t, 16);
  pl_val p = pl_nat_mul(t, &t->vstack[a], &t->vstack[a + 1]);
  cr_assert(!pl_is_nat63(p));
  cr_assert_eq(pl_nat_limb_at(p, 0), UINT64_C(0xfffffffffffffff0));
  cr_assert_eq(pl_nat_limb_at(p, 1), 7);
  test_rt_free(&rt);
}

Test(nat, divmod_bignum) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t a = t->vsp;
  /* (2^64 + 5) / 3 and mod */
  pl_vpush(t, 0);
  pl_vpush(t, 3);
  pl_gc_reserve(t, PL_NAT_CELLS(2));
  uint64_t* limbs;
  pl_val big = pl_mk_nat_limbs(t, 2, &limbs);
  limbs[0] = 5;
  limbs[1] = 1;
  t->vstack[a] = pl_nat_trim(big);
  pl_val q = pl_nat_div(t, &t->vstack[a], &t->vstack[a + 1]);
  pl_vpush(t, q);
  pl_val m = pl_nat_mod(t, &t->vstack[a], &t->vstack[a + 1]);
  /* 2^64+5 = 18446744073709551621 = 3*6148914691236517207 + 0 */
  cr_assert_eq(pl_nat_u64_clamp(t->vstack[a + 2]),
               UINT64_C(6148914691236517207));
  cr_assert_eq(m, 0);
  test_rt_free(&rt);
}

Test(nat, from_decimal_roundtrip) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  const char* dec = "340282366920938463463374607431768211456"; /* 2^128 */
  bool ok = false;
  pl_val v = pl_nat_from_decimal(t, dec, strlen(dec), &ok);
  cr_assert(ok);
  cr_assert_eq(pl_nat_bit_len(v), 129);
  cr_assert_eq(pl_nat_limb_at(v, 2), 1);
  test_rt_free(&rt);
}

/* ── Heap / collector ──────────────────────────────────────────────────── */

Test(gc, values_survive_collection) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  pl_val row = test_app2(t, 0, 17, PL_NAT63_MAX);
  pl_vpush(t, row);
  size_t slot = t->vsp - 1;

  /* churn enough garbage to force several collections */
  for (int i = 0; i < 100000; i++) {
    pl_gc_reserve(t, PL_APP_CELLS(2));
    pl_val args[2] = {1, 2};
    (void)pl_mk_app_from(t, 0, 2, args);
  }
  pl_gc_collect_now(t);

  pl_val moved = t->vstack[slot];
  pl_cell* p = pl_as(PL_TAG_APP, moved);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_n(p), 2);
  cr_assert_eq(pl_app_args(p)[0], 17);
  cr_assert_eq(pl_app_args(p)[1], (pl_val)PL_NAT63_MAX);

  test_rt_free(&rt);
}

Test(gc, store_values_are_terminal) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 0, 1, 2));
  pl_val pin = pl_pin(t, &t->vstack[base]);
  pl_vpush(t, pin);
  cr_assert(pl_store_owns(rt.store, pin));

  pl_gc_collect_now(t);
  /* the pin val must be unchanged (non-moving store region) */
  cr_assert_eq(t->vstack[base + 1], pin);

  test_rt_free(&rt);
}

Test(gc, ind_short_circuit_on_evacuation) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  /* a thunk that evaluates to 42: expr = (0 42) over an empty env */
  size_t base = t->vsp;
  pl_vpush(t, test_app1(t, 0, 42));
  pl_gc_reserve(t, PL_ENV_CELLS(1) + PL_THUNK_CELLS);
  pl_val env = pl_mk_env(t, 1);
  pl_val thunk = pl_mk_thunk(t, env, t->vstack[base]);
  t->vsp = base;
  pl_vpush(t, thunk);

  cr_assert_eq(pl_whnf(t, t->vstack[base]), 42);
  /* slot now holds an IND; collection should snap it to the target */
  cr_assert_eq(pl_tag(t->vstack[base]), PL_TAG_DEFER);
  pl_gc_collect_now(t);
  cr_assert_eq(t->vstack[base], 42);

  test_rt_free(&rt);
}
