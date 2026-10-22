#include "test.h"
#include "test_plan.h"
#include "plan/int63.h"

/* Independent arithmetic oracle: calculate exactly, then reduce to the
 * signed 63-bit interval before encoding by magnitude. */
__extension__ typedef __int128 wide_int;

static pl_val oracle(wide_int value) {
  const wide_int modulus = (wide_int)1 << 63;
  value %= modulus;
  if (value < PL_INT63_MIN)
    value += modulus;
  if (value > PL_INT63_MAX)
    value -= modulus;
  return (pl_val)(value < 0 ? -2 * value - 1 : 2 * value);
}

static const int64_t edges[] = {
    PL_INT63_MIN, PL_INT63_MIN + 1, -100,        -7, -2, -1, 0, 1, 2, 7,
    100,          PL_INT63_MAX - 1, PL_INT63_MAX};

static void check_pair(int64_t a, int64_t b) {
  pl_val x = oracle(a), y = oracle(b);
  ASSERT_EQ(pl_int63_decode(x), a);
  ASSERT_EQ(pl_int63_encode(a), x);
  ASSERT_EQ(pl_int63_add(x, y), oracle((wide_int)a + b));
  ASSERT_EQ(pl_int63_sub(x, y), oracle((wide_int)a - b));
  ASSERT_EQ(pl_int63_mul(x, y), oracle((wide_int)a * b));
  ASSERT_EQ(pl_int63_neg(x), oracle(-(wide_int)a));
  ASSERT_EQ(pl_int63_abs(x), oracle(a < 0 ? -(wide_int)a : a));
  if (b != 0) {
    ASSERT_EQ(pl_int63_div(x, y), oracle((wide_int)a / b));
    ASSERT_EQ(pl_int63_mod(x, y), oracle((wide_int)a % b));
  }
}

TEST(int63, encoding_and_boundary_arithmetic) {
  ASSERT_EQ(pl_int63_encode(0), 0);
  ASSERT_EQ(pl_int63_encode(-1), 1);
  ASSERT_EQ(pl_int63_encode(1), 2);
  ASSERT_EQ(pl_int63_encode(PL_INT63_MIN), PL_NAT63_MAX);
  ASSERT_EQ(pl_int63_encode(PL_INT63_MAX), PL_NAT63_MAX - 1);
  ASSERT_EQ(pl_int63_encode(INT64_MIN), 0);
  ASSERT_EQ(pl_int63_encode(INT64_MAX), 1);
  for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
    for (size_t j = 0; j < sizeof(edges) / sizeof(edges[0]); j++)
      check_pair(edges[i], edges[j]);
}

static uint64_t next_random(uint64_t* state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

TEST(int63, randomized_arithmetic_matches_wide_oracle) {
  uint64_t state = UINT64_C(0x129abc753def);
  for (size_t i = 0; i < 10000; i++) {
    uint64_t x = next_random(&state), y = next_random(&state);
    int64_t a = (int64_t)(x >> 2), b = (int64_t)(y >> 2);
    if (x & 1)
      a = -a - 1;
    if (y & 1)
      b = -b - 1;
    check_pair(a, b);
  }
}

/* Exercise public dispatch and the resumable strict-argument evaluator. */
static pl_val invoke(pl_thread* t, const char* name, size_t n,
                     const pl_val* args, pl_run_status want) {
  size_t base = t->vsp;
  /* Root operands before allocating long names such as I63MulDiv. */
  for (size_t i = 0; i < n; i++)
    pl_vpush(t, args[i]);
  pl_val name_nat = pl_nat_from_bytes(t, (const uint8_t*)name, strlen(name));
  pl_val row = test_app(t, name_nat, n, &t->vstack[base]);
  t->vsp = base;
  pl_vpush(t, row);
  pl_vpush(t, test_app1(t, 0, t->vstack[base]));
  pl_vpush(t, test_app1(t, 0, pl_pin(t, 83)));
  pl_vpush(t, test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]));
  pl_gc_reserve(t, PL_ENV_CELLS(1) + PL_THUNK_CELLS);
  pl_val env = pl_mk_env(t, 1);
  pl_val thunk = pl_mk_thunk(t, env, t->vstack[base + 3]);
  pl_thread_start(t, thunk);
  pl_run_status status;
  do {
    status = pl_thread_run(t, 2);
  } while (status == PL_RUN_YIELDED);
  ASSERT_EQ(status, want, "%s: %s", name, t->exn_msg ? t->exn_msg : "no error");
  pl_val result = want == PL_RUN_DONE ? pl_thread_result(t) : 0;
  t->vsp = base;
  return result;
}

TEST(int63, all_primitives_dispatch_with_signed_semantics) {
  test_rt rt = test_rt_new();
  rt.t->rplan_f = true;
  for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
    int64_t a = edges[i];
    pl_val unary[] = {oracle(a)};
    ASSERT_EQ(invoke(rt.t, "I63Inc", 1, unary, PL_RUN_DONE),
              oracle((wide_int)a + 1));
    ASSERT_EQ(invoke(rt.t, "I63Dec", 1, unary, PL_RUN_DONE),
              oracle((wide_int)a - 1));
    ASSERT_EQ(invoke(rt.t, "I63Neg", 1, unary, PL_RUN_DONE),
              oracle(-(wide_int)a));
    ASSERT_EQ(invoke(rt.t, "I63Abs", 1, unary, PL_RUN_DONE),
              oracle(a < 0 ? -(wide_int)a : a));
    for (size_t j = 0; j < sizeof(edges) / sizeof(edges[0]); j++) {
      int64_t b = edges[j];
      pl_val args[] = {oracle(a), oracle(b)};
      ASSERT_EQ(invoke(rt.t, "I63Add", 2, args, PL_RUN_DONE),
                oracle((wide_int)a + b));
      ASSERT_EQ(invoke(rt.t, "I63Sub", 2, args, PL_RUN_DONE),
                oracle((wide_int)a - b));
      ASSERT_EQ(invoke(rt.t, "I63Mul", 2, args, PL_RUN_DONE),
                oracle((wide_int)a * b));
      ASSERT_EQ(invoke(rt.t, "I63Eq", 2, args, PL_RUN_DONE), a == b);
      ASSERT_EQ(invoke(rt.t, "I63Ne", 2, args, PL_RUN_DONE), a != b);
      ASSERT_EQ(invoke(rt.t, "I63Lt", 2, args, PL_RUN_DONE), a < b);
      ASSERT_EQ(invoke(rt.t, "I63Le", 2, args, PL_RUN_DONE), a <= b);
      ASSERT_EQ(invoke(rt.t, "I63Gt", 2, args, PL_RUN_DONE), a > b);
      ASSERT_EQ(invoke(rt.t, "I63Ge", 2, args, PL_RUN_DONE), a >= b);
      if (b != 0) {
        ASSERT_EQ(invoke(rt.t, "I63Div", 2, args, PL_RUN_DONE),
                  oracle((wide_int)a / b));
        ASSERT_EQ(invoke(rt.t, "I63Mod", 2, args, PL_RUN_DONE),
                  oracle((wide_int)a % b));
      }
    }
  }
  test_rt_free(&rt);
}

TEST(int63, invalid_operands_and_zero_divisors_raise) {
  test_rt rt = test_rt_new();
  rt.t->rplan_f = true;
  pl_val args[] = {oracle(-7), 0};
  invoke(rt.t, "I63Div", 2, args, PL_RUN_EXN);
  ASSERT_STR_EQ(rt.t->exn_msg, "I63: division by zero", "%s", rt.t->exn_msg);
  invoke(rt.t, "I63Mod", 2, args, PL_RUN_EXN);
  ASSERT_STR_EQ(rt.t->exn_msg, "I63: division by zero", "%s", rt.t->exn_msg);
  /* Reject heap nats and non-nats, rather than interpreting pointer bits. */
  pl_gc_reserve(rt.t, PL_NAT_CELLS(1));
  args[0] = pl_mk_nat_u64(rt.t, UINT64_C(1) << 63);
  invoke(rt.t, "I63Neg", 1, args, PL_RUN_EXN);
  ASSERT_STR_EQ(rt.t->exn_msg, "I63: expected a 63-bit zigzag integer");
  args[0] = 0;
  args[1] = test_app1(rt.t, 0, 42);
  invoke(rt.t, "I63Add", 2, args, PL_RUN_EXN);
  ASSERT_STR_EQ(rt.t->exn_msg, "I63: expected a 63-bit zigzag integer");
  test_rt_free(&rt);
}

static pl_val lazy_literal(pl_thread* t, pl_val value) {
  size_t base = t->vsp;
  pl_vpush(t, test_app1(t, 0, value));
  pl_gc_reserve(t, PL_ENV_CELLS(1) + PL_THUNK_CELLS);
  pl_val env = pl_mk_env(t, 1);
  pl_val result = pl_mk_thunk(t, env, t->vstack[base]);
  t->vsp = base;
  return result;
}

TEST(int63, operands_are_forced_before_decoding) {
  test_rt rt = test_rt_new();
  rt.t->rplan_f = true;
  pl_vpush(rt.t, lazy_literal(rt.t, oracle(-7)));
  pl_vpush(rt.t, lazy_literal(rt.t, oracle(3)));
  pl_val args[] = {rt.t->vstack[0], rt.t->vstack[1]};
  ASSERT_EQ(invoke(rt.t, "I63Add", 2, args, PL_RUN_DONE), oracle(-4));
  rt.t->vsp = 0;
  test_rt_free(&rt);
}

/* Oracle uses the two adjacent integers, independently of the implementation's
 * truncation-and-sign correction. Normalize the denominator to positive. */
static pl_val muldiv_oracle(int64_t a, int64_t b, int64_t d, unsigned mode) {
  wide_int n = (wide_int)a * b;
  wide_int denominator = d;
  if (d < 0) {
    n = -n;
    denominator = -denominator;
  }
  wide_int lower = n / denominator;
  if (n % denominator < 0)
    lower--;
  wide_int fraction = n - lower * denominator;
  switch (mode) {
  case 0:
    return oracle(n / denominator);
  case 1:
    return oracle(lower);
  case 2:
    return oracle(lower + (fraction != 0));
  default:
    return oracle(lower + (2 * fraction > denominator ||
                           (2 * fraction == denominator && lower % 2 != 0)));
  }
}

TEST(int63, muldiv_rounding_and_projection_examples) {
  static const struct {
    int64_t a, b, d;
    int64_t expected[4];
  } cases[] = {
      {5, 1, 2, {2, 2, 3, 2}},
      {7, 1, 2, {3, 3, 4, 4}},
      {-5, 1, 2, {-2, -3, -2, -2}},
      {-7, 1, 2, {-3, -4, -3, -4}},
      {7, 1, -2, {-3, -4, -3, -4}},
      {-7, 1, -2, {3, 3, 4, 4}},
      {-1, 1, 3, {0, -1, 0, 0}},
      {2, 1, 3, {0, 0, 1, 1}},
      {0, -7, -3, {0, 0, 0, 0}},
      {PL_INT63_MAX,
       PL_INT63_MAX,
       PL_INT63_MAX,
       {PL_INT63_MAX, PL_INT63_MAX, PL_INT63_MAX, PL_INT63_MAX}},
      {PL_INT63_MIN,
       PL_INT63_MIN,
       PL_INT63_MIN,
       {PL_INT63_MIN, PL_INT63_MIN, PL_INT63_MIN, PL_INT63_MIN}},
      /* 2^40 * 2^30 / 2^35 = 2^35, despite an overflowing narrow product. */
      {INT64_C(1) << 40,
       INT64_C(1) << 30,
       INT64_C(1) << 35,
       {INT64_C(1) << 35, INT64_C(1) << 35, INT64_C(1) << 35,
        INT64_C(1) << 35}},
      /* MAX + 1/2: round before wrapping to MIN in ceil/even modes. */
      {7,
       INT64_C(1317624576693539401),
       2,
       {PL_INT63_MAX, PL_INT63_MAX, PL_INT63_MIN, PL_INT63_MIN}},
      /* A quotient outside the signed interval also wraps only at the end. */
      {PL_INT63_MAX,
       3,
       2,
       {-2305843009213693954LL, -2305843009213693954LL, -2305843009213693953LL,
        -2305843009213693954LL}},
  };
  test_rt rt = test_rt_new();
  rt.t->rplan_f = true;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    for (unsigned mode = 0; mode < 4; mode++) {
      pl_val args[] = {oracle(cases[i].a), oracle(cases[i].b),
                       oracle(cases[i].d), mode};
      ASSERT_EQ(
          pl_int63_muldiv(args[0], args[1], args[2], (pl_int63_round)mode),
          oracle(cases[i].expected[mode]));
      ASSERT_EQ(invoke(rt.t, "I63MulDiv", 4, args, PL_RUN_DONE),
                oracle(cases[i].expected[mode]));
    }
  }
  test_rt_free(&rt);
}

TEST(int63, muldiv_matches_independent_rounding_oracle) {
  uint64_t random = UINT64_C(0xad721bf903e8456);
  size_t edge_n = sizeof(edges) / sizeof(edges[0]);
  for (size_t sample = 0; sample < edge_n * edge_n * edge_n + 10000; sample++) {
    int64_t a, b, d;
    if (sample < edge_n * edge_n * edge_n) {
      a = edges[sample / (edge_n * edge_n)];
      b = edges[(sample / edge_n) % edge_n];
      d = edges[sample % edge_n];
    } else {
      a = pl_int63_decode(next_random(&random) & PL_NAT63_MAX);
      b = pl_int63_decode(next_random(&random) & PL_NAT63_MAX);
      d = pl_int63_decode(next_random(&random) & PL_NAT63_MAX);
    }
    if (d == 0)
      continue;
    for (unsigned mode = 0; mode < 4; mode++)
      ASSERT_EQ(pl_int63_muldiv(oracle(a), oracle(b), oracle(d),
                                (pl_int63_round)mode),
                muldiv_oracle(a, b, d, mode));
  }
}

TEST(int63, muldiv_validates_and_forces_every_argument) {
  test_rt rt = test_rt_new();
  rt.t->rplan_f = true;
  pl_val args[] = {oracle(-7), oracle(1), oracle(2), 3};
  for (unsigned k = 0; k < 4; k++) {
    pl_val saved = args[k];
    args[k] = lazy_literal(rt.t, saved);
    ASSERT_EQ(invoke(rt.t, "I63MulDiv", 4, args, PL_RUN_DONE), oracle(-4));
    args[k] = test_app1(rt.t, 0, 1);
    invoke(rt.t, "I63MulDiv", 4, args, PL_RUN_EXN);
    ASSERT_STR_EQ(rt.t->exn_msg, k == 3
                                     ? "I63MulDiv: invalid rounding mode"
                                     : "I63: expected a 63-bit zigzag integer");
    pl_gc_reserve(rt.t, PL_NAT_CELLS(1));
    args[k] = pl_mk_nat_u64(rt.t, UINT64_C(1) << 63);
    invoke(rt.t, "I63MulDiv", 4, args, PL_RUN_EXN);
    ASSERT_STR_EQ(rt.t->exn_msg, k == 3
                                     ? "I63MulDiv: invalid rounding mode"
                                     : "I63: expected a 63-bit zigzag integer");
    args[k] = saved;
  }
  args[3] = 4;
  invoke(rt.t, "I63MulDiv", 4, args, PL_RUN_EXN);
  ASSERT_STR_EQ(rt.t->exn_msg, "I63MulDiv: invalid rounding mode");
  args[2] = 0;
  args[3] = 3;
  invoke(rt.t, "I63MulDiv", 4, args, PL_RUN_EXN);
  ASSERT_STR_EQ(rt.t->exn_msg, "I63: division by zero");
  test_rt_free(&rt);
}
