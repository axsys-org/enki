#include <criterion/criterion.h>

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "axsys/allocator.h"
#include "axsys/util.h"
#include "plan/debug.h"
#include "plan/host.h"
#include "plan/wormhole.h"
#include "test_plan.h"

typedef struct test_host_state {
  size_t retains;
  size_t releases;
  uint64_t last_token;
} test_host_state;

static test_host_state host_state;

static pl_val test_host_effect(void* ctx, void* scope, pl_thread* t,
                               pl_host_op op, size_t argbase) {
  (void)ctx;
  (void)scope;
  (void)op;
  (void)argbase;
  pl_raise_msg(t, "unexpected test host effect");
}

static void test_host_retain(void* ctx, uint64_t token) {
  test_host_state* state = ctx;
  state->retains++;
  state->last_token = token;
}

static void test_host_release(void* ctx, uint64_t token) {
  test_host_state* state = ctx;
  state->releases++;
  state->last_token = token;
}

static const pl_host test_host = {
    .ctx = &host_state,
    .effect = test_host_effect,
    .retain = test_host_retain,
    .release = test_host_release,
};

static void wormhole_init(void) {
  pl_host_install(&test_host);
  host_state = (test_host_state){0};
}

TestSuite(wormhole, .init = wormhole_init);

Test(wormhole, live_and_dead_collection) {
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, pl_wormhole_adopt(rt.t, UINT64_C(0xfeedface)));

  pl_gc_collect_now(rt.t);
  cr_assert(pl_is_wormhole(rt.t->vstack[0]));
  cr_assert_eq(pl_wormhole_token(rt.t->vstack[0]), UINT64_C(0xfeedface));
  cr_assert_eq(host_state.releases, 0);

  rt.t->vsp = 0;
  pl_gc_collect_now(rt.t);
  cr_assert_eq(host_state.releases, 1);
  cr_assert_eq(host_state.last_token, UINT64_C(0xfeedface));
  test_rt_free(&rt);
  cr_assert_eq(host_state.releases, 1);
}

Test(wormhole, close_is_idempotent) {
  test_rt rt = test_rt_new();
  pl_val wormhole = pl_wormhole_adopt(rt.t, 41);
  pl_vpush(rt.t, wormhole);

  pl_wormhole_close(wormhole);
  cr_assert(pl_wormhole_is_closed(wormhole));
  pl_wormhole_close(wormhole);
  cr_assert_eq(host_state.releases, 1);

  pl_gc_collect_now(rt.t);
  rt.t->vsp = 0;
  pl_gc_collect_now(rt.t);
  test_rt_free(&rt);
  cr_assert_eq(host_state.releases, 1);
}

Test(wormhole, clone_retains_per_wrapper) {
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, pl_wormhole_adopt(rt.t, 73));
  pl_val clone = pl_wormhole_clone(rt.t, rt.t->vstack[0]);
  pl_vpush(rt.t, clone);
  cr_assert_eq(host_state.retains, 1);
  cr_assert_eq(pl_wormhole_token(rt.t->vstack[1]), 73);

  rt.t->vstack[0] = 0;
  pl_gc_collect_now(rt.t);
  cr_assert_eq(host_state.releases, 1);
  cr_assert_eq(pl_wormhole_token(rt.t->vstack[1]), 73);

  rt.t->vsp = 0;
  pl_gc_collect_now(rt.t);
  cr_assert_eq(host_state.releases, 2);
  test_rt_free(&rt);
}

Test(wormhole, heap_growth_and_teardown_release_once) {
  pl_heap* heap = pl_heap_new(4096, NULL);
  pl_thread* t = pl_thread_new(heap);
  pl_vpush(t, pl_wormhole_adopt(t, 99));

  pl_gc_reserve(t, 8192);
  cr_assert_eq(host_state.releases, 0);
  cr_assert_eq(pl_wormhole_token(t->vstack[0]), 99);

  pl_thread_free(t);
  pl_heap_free(heap);
  cr_assert_eq(host_state.releases, 1);
}

typedef enum force_attempt {
  ATTEMPT_WHNF,
  ATTEMPT_NF,
  ATTEMPT_PIN,
  ATTEMPT_EQUAL,
  ATTEMPT_SAVE,
} force_attempt;

static void expect_loop(pl_thread* t, pl_val wormhole, force_attempt attempt) {
  size_t base = t->vsp;
  pl_vpush(t, wormhole);
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    switch (attempt) {
    case ATTEMPT_WHNF:
      (void)pl_whnf(t, t->vstack[base]);
      break;
    case ATTEMPT_NF:
      (void)pl_nf(t, t->vstack[base]);
      break;
    case ATTEMPT_PIN:
      (void)pl_pin(t, t->vstack[base]);
      break;
    case ATTEMPT_EQUAL:
      (void)test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), t->vstack[base],
                        t->vstack[base]);
      break;
    case ATTEMPT_SAVE: {
      pl_val arg[1] = {t->vstack[base]};
      (void)test_op66(t, ax_s4('S', 'a', 'v', 'e'), 1, arg);
      break;
    }
    }
    cr_assert_fail("expected <<loop>>");
  }
  pl_catch_unwind(t, &c);
  cr_assert_str_eq(t->exn_msg, "<<loop>>");
  t->vsp = base;
}

Test(wormhole, blackhole_operations_fail_and_debug_is_opaque) {
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, pl_wormhole_adopt(rt.t, UINT64_C(0x123456789abcdef0)));

  expect_loop(rt.t, rt.t->vstack[0], ATTEMPT_WHNF);
  expect_loop(rt.t, rt.t->vstack[0], ATTEMPT_NF);
  expect_loop(rt.t, rt.t->vstack[0], ATTEMPT_PIN);
  expect_loop(rt.t, rt.t->vstack[0], ATTEMPT_EQUAL);
  expect_loop(rt.t, rt.t->vstack[0], ATTEMPT_SAVE);

  char* shown = pl_show(ax_allocator_system(), rt.t->vstack[0], NULL);
  cr_assert_str_eq(shown, "<wormhole>");
  cr_assert_null(strstr(shown, "123456789abcdef0"));
  ax_free(ax_allocator_system(), shown);
  test_rt_free(&rt);
  cr_assert_eq(host_state.releases, 1);
}
