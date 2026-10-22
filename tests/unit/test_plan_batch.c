#include "test.h"
#include "test_plan.h"
#include "../../pkg/plan/src/batch.h"
#include "../../pkg/plan/src/jit.h"

static bool batching_enabled(void) {
  const char* v = getenv("PL_BATCH");
  return v == NULL || strcmp(v, "0") != 0;
}

static pl_code* decode(pl_thread* t, const pl_val* ops, size_t n) {
  return pl_bytecode_from_val(test_app(t, 0, n, ops));
}

TEST(batch, sharing_gc_and_fuel_match_unbatched) {
  if (!batching_enabled())
    SKIP_TEST("PL_BATCH=0");
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  /* One delayed application, shared both directly and through a row. */
  pl_val ops[] = {OP_PUSH_LIT, 0,
                  OP_PUSH_VAR, 1,
                  OP_MK_THK,   2,
                  PL_BAN_SLOW, OP_PUSH_LIT,
                  0,           OP_PUSH_SLOT,
                  0,           OP_MK_APP,
                  1,           OP_PUSH_LIT,
                  0,           OP_PUSH_SLOT,
                  0,           OP_PUSH_SLOT,
                  1,           OP_MK_APP,
                  2,           OP_RET};
  pl_code* c = decode(t, ops, sizeof ops / sizeof ops[0]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->nbatches, 1);
  ASSERT_EQ(c->batches[0].cells,
            PL_THKE_CELLS(2) + PL_APP_CELLS(1) + PL_APP_CELLS(2));
  pl_vpush(t, pl_pin(t, test_law(t, 1, 0, 0)));
  char error[192];
  ASSERT(pl_store_save_root(rt.store, t->vstack[0], NULL, error, sizeof error));
  pl_pin_set_code(pl_ptr(t->vstack[0]), c);
  pl_op_t* batch = c->batch_ops;
#ifdef PL_SLJIT
  pl_op_t* native = c->jit_ops;
  c->jit_ops = NULL;
#endif
  for (unsigned env = 0; env < 2; env++) {
    c->max_var = env ? UINT32_MAX : 1;
    for (unsigned fuel = 2; fuel <= 9; fuel++) {
      unsigned expected = 0;
      for (unsigned mode = 0; mode < 2; mode++) {
        c->batch_ops = mode ? batch : NULL;
        t->vsp = 1;
        /* A moving input, including collections between every suspension. */
        pl_vpush(t, test_app1(t, 0, 777));
        pl_vpush(t, t->vstack[0]);
        pl_vpush(t, t->vstack[1]);
        pl_gc_reserve(t, PL_THKE_CELLS(2));
        pl_val call = pl_mk_thke(t, PL_BAN_FAST, 2, t->vstack + 2);
        t->vsp = 2;
        pl_thread_start(t, call);
        unsigned yields = 0;
        pl_run_status status;
        while ((status = pl_thread_run(t, fuel)) == PL_RUN_YIELDED) {
          ASSERT_LT(++yields, 100);
          pl_gc_collect_now(t);
        }
        ASSERT_EQ(status, PL_RUN_DONE);
        if (!mode)
          expected = yields;
        ASSERT_EQ(yields, expected);
        pl_gc_collect_now(t);
        pl_cell* result = pl_as(PL_TAG_APP, t->result);
        ASSERT_NOT_NULL(result);
        ASSERT_EQ(pl_app_n(result), 2);
        pl_val thunk = pl_app_args(result)[0];
        pl_cell* row = pl_as(PL_TAG_APP, pl_app_args(result)[1]);
        ASSERT_NOT_NULL(row);
        ASSERT_EQ(pl_app_args(row)[0], thunk);
        ASSERT_EQ(pl_hdr_kind(pl_ptr(thunk)[0]), PL_K_THKE);
        ASSERT_EQ(pl_thke_args(pl_ptr(thunk))[1], t->vstack[1]);
      }
    }
  }
  c->batch_ops = batch;
#ifdef PL_SLJIT
  c->jit_ops = native;
#endif
  pl_pin_set_code(pl_ptr(t->vstack[0]), NULL);
  pl_bytecode_free(c);
  test_rt_free(&rt);
}

TEST(batch, guard_failure_is_transactional_and_stack_can_grow) {
  if (!batching_enabled())
    SKIP_TEST("PL_BATCH=0");
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_val ops[] = {OP_PUSH_LIT,  0, OP_PUSH_VAR, 1, OP_MK_APP, 1, OP_PUSH_LIT, 0,
                  OP_PUSH_SLOT, 0, OP_MK_APP,   1, OP_RET};
  pl_code* c = decode(t, ops, sizeof ops / sizeof ops[0]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->nbatches, 1);
  pl_frame fr = {.code = c, .argc = 1};
  size_t room = pl_gc_headroom(t);
  ASSERT(!pl_batch_reserve(t, &fr, c->batches));
  ASSERT_EQ(t->vsp, 0);
  ASSERT_EQ(pl_gc_headroom(t), room);
  while (t->vsp < t->vcap)
    pl_vpush(t, 42);
  fr.argc = 2;
  fr.argbase = (uint32_t)t->vsp;
  size_t cap = t->vcap;
  ASSERT(pl_batch_reserve(t, &fr, c->batches));
  ASSERT_GT(t->vcap, cap);
  ASSERT(pl_gc_headroom(t) >= c->batches[0].cells);
  pl_bytecode_free(c);
  test_rt_free(&rt);
}

TEST(batch, respects_targets_and_eager_shortcuts) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  /* A target between the two constructions prevents grouping them. */
  pl_val ops[] = {OP_JMP,      2,         OP_PUSH_LIT, 0,           OP_PUSH_LIT,
                  7,           OP_MK_APP, 1,           OP_PUSH_LIT, 0,
                  OP_PUSH_LIT, 8,         OP_MK_APP,   1,           OP_PUSH_LIT,
                  0,           OP_BR,     1,           8,           OP_RET};
  pl_code* c = decode(t, ops, sizeof ops / sizeof ops[0]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->nbatches, 0);
  pl_bytecode_free(c);
  pl_vpush(t, test_p66(t));
  pl_val eager[] = {OP_PUSH_VAR,
                    1,
                    OP_PUSH_VAR,
                    2,
                    OP_MK_THK,
                    2,
                    PL_BAN_PRIM_KNOWN,
                    t->vstack[0],
                    ax_s3('A', 'd', 'd'),
                    OP_PUSH_LIT,
                    0,
                    OP_PUSH_SLOT,
                    0,
                    OP_MK_APP,
                    1,
                    OP_RET};
  c = decode(t, eager, sizeof eager / sizeof eager[0]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->nbatches, 0);
  pl_bytecode_free(c);
  pl_val private_op[] = {OP_BUILD, 0, OP_RET};
  ASSERT_EQ(decode(t, private_op, 3), NULL);
  test_rt_free(&rt);
}
