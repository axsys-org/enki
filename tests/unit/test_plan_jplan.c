#include <criterion/criterion.h>

#include <stdbool.h>
#include <stdint.h>

#include "axsys/util.h"
#include "plan/host.h"
#include "plan/wormhole.h"
#include "test_plan.h"

typedef struct jplan_state {
  size_t effects;
  size_t interceptor_calls;
  size_t releases;
  uint64_t environments[4];
  uint64_t objects[4];
  size_t object_counts[4];
  pl_val sources[4];
  pl_val rows[4];
} jplan_state;

static jplan_state state;

static pl_val jplan_effect(void* ctx, void* scope, pl_thread* t, pl_host_op op,
                           size_t ab) {
  (void)scope;
  jplan_state* s = ctx;
  cr_assert_eq(op, PL_HOST_OP_JPLAN_EVAL);
  cr_assert_lt(s->effects, 4);
  size_t call = s->effects++;

  cr_assert(pl_is_wormhole(t->vstack[ab]));
  cr_assert(!pl_wormhole_is_closed(t->vstack[ab]));
  s->environments[call] = pl_wormhole_token(t->vstack[ab]);
  s->sources[call] = t->vstack[ab + 2];
  s->rows[call] = t->vstack[ab + 1];

  if (t->vstack[ab + 1] != 0) {
    pl_cell* row = pl_as(PL_TAG_APP, t->vstack[ab + 1]);
    cr_assert_not_null(row);
    cr_assert_eq(pl_app_head(row), 0);
    s->object_counts[call] = pl_app_n(row);
    if (pl_app_n(row) != 0) {
      cr_assert(pl_is_wormhole(pl_app_args(row)[0]));
      s->objects[call] = pl_wormhole_token(pl_app_args(row)[0]);
    }
  }

  return pl_wormhole_adopt(t, 100 + call);
}

static void jplan_retain(void* ctx, uint64_t token) {
  (void)ctx;
  (void)token;
}

static void jplan_release(void* ctx, uint64_t token) {
  (void)token;
  ((jplan_state*)ctx)->releases++;
}

static const pl_host jplan_host = {
    .ctx = &state,
    .effect = jplan_effect,
    .retain = jplan_retain,
    .release = jplan_release,
};

static void jplan_init(void) {
  state = (jplan_state){0};
  pl_host_install(&jplan_host);
}

TestSuite(jplan, .init = jplan_init);

static pl_val jplan_thunk(pl_thread* t, pl_val expr) {
  size_t base = t->vsp;
  pl_vpush(t, expr);
  pl_gc_reserve(t, PL_ENV_CELLS(1) + PL_THUNK_CELLS);
  pl_val env = pl_mk_env(t, 1);
  pl_val out = pl_mk_thunk(t, env, t->vstack[base]);
  t->vsp = base;
  return out;
}

static pl_val jplan_pin(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, 74);
  pl_val out = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return out;
}

static pl_val jplan_eval_thunk(pl_thread* t, pl_val environment, pl_val objects,
                               pl_val source) {
  size_t base = t->vsp;
  pl_val args[3] = {environment, objects, source};
  pl_vpush(t, test_app(t, ax_s4('E', 'v', 'a', 'l'), 3, args));
  pl_vpush(t, test_app1(t, 0, t->vstack[base]));
  pl_vpush(t, test_app1(t, 0, jplan_pin(t)));
  pl_val expr = test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]);
  t->vsp = base;
  return jplan_thunk(t, expr);
}

static pl_run_status jplan_run(pl_thread* t) {
  pl_run_status status;
  size_t steps = 0;
  do {
    status = pl_thread_run(t, 2);
    cr_assert_lt(++steps, 1u << 20);
  } while (status == PL_RUN_YIELDED);
  return status;
}

static bool reject_interceptor(void* ctx, pl_thread* t, uint32_t op,
                               size_t argbase, pl_val* out) {
  (void)t;
  (void)op;
  (void)argbase;
  (void)out;
  ((jplan_state*)ctx)->interceptor_calls++;
  return false;
}

Test(jplan, lazy_handles_and_rows_are_opaque_and_interceptor_is_bypassed) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, pl_wormhole_adopt(t, 11));
  pl_vpush(t, test_app1(t, 0, t->vstack[base]));
  pl_vpush(t, jplan_thunk(t, t->vstack[base + 1]));
  pl_vpush(t, pl_wormhole_adopt(t, 21));
  pl_vpush(t, test_app1(t, 0, t->vstack[base + 3]));
  pl_vpush(t, jplan_thunk(t, t->vstack[base + 4]));
  pl_val object_args[1] = {t->vstack[base + 5]};
  pl_vpush(t, test_app(t, 0, 1, object_args));
  pl_vpush(t, jplan_thunk(t, 7));
  pl_val original_row = t->vstack[base + 6];

  pl_thread_set_effect_interceptor(t, reject_interceptor, &state);
  pl_thread_start(t, jplan_eval_thunk(t, t->vstack[base + 2], original_row,
                                      t->vstack[base + 7]));
  pl_run_status status = jplan_run(t);
  cr_assert_eq(status, PL_RUN_DONE, "status=%d error=%s", (int)status,
               t->exn_msg == NULL ? "<none>" : t->exn_msg);
  cr_assert(pl_is_wormhole(pl_thread_result(t)));
  cr_assert_eq(pl_wormhole_token(pl_thread_result(t)), 100);
  cr_assert_eq(state.effects, 1);
  cr_assert_eq(state.interceptor_calls, 0);
  cr_assert_eq(state.environments[0], 11);
  cr_assert_eq(state.object_counts[0], 1);
  cr_assert_eq(state.objects[0], 21);
  cr_assert_eq(state.sources[0], 7);
  cr_assert_neq(state.rows[0], original_row);
  cr_assert(!pl_is_wormhole(pl_app_args(pl_ptr(original_row))[0]));
  test_rt_free(&rt);
}

Test(jplan, empty_rows_and_nested_eval_results_work) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, pl_wormhole_adopt(t, 31));
  pl_vpush(t, jplan_eval_thunk(t, t->vstack[base], 0, 1));
  pl_val object_args[1] = {t->vstack[base + 1]};
  pl_vpush(t, test_app(t, 0, 1, object_args));

  pl_thread_start(t,
                  jplan_eval_thunk(t, t->vstack[base], t->vstack[base + 2], 2));
  cr_assert_eq(jplan_run(t), PL_RUN_DONE);
  cr_assert_eq(state.effects, 2);
  cr_assert_eq(state.object_counts[0], 0);
  cr_assert_eq(state.sources[0], 1);
  cr_assert_eq(state.object_counts[1], 1);
  cr_assert_eq(state.objects[1], 100);
  cr_assert_eq(state.sources[1], 2);
  cr_assert_eq(pl_wormhole_token(pl_thread_result(t)), 101);
  test_rt_free(&rt);
}

Test(jplan, ordinary_strict_continuations_still_force_results) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, pl_wormhole_adopt(t, 41));
  pl_vpush(t, jplan_eval_thunk(t, t->vstack[base], 0, 1));
  pl_val add_args[2] = {t->vstack[base + 1], 1};

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_op66(t, ax_s3('A', 'd', 'd'), 2, add_args);
    cr_assert_fail("expected the JPLAN result to retain blackhole semantics");
  }
  pl_catch_unwind(t, &c);
  cr_assert_str_eq(t->exn_msg, "<<loop>>");
  cr_assert_eq(state.effects, 1);
  t->vsp = base + 1;
  test_rt_free(&rt);
}
