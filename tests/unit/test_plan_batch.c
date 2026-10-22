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
  if (native) {
    pl_frame frame = {.code = c, .argc = 1, .argbase = 1};
    for (unsigned k = 0; k < 1000; k++)
      ASSERT_EQ(pl_jit_run(t, &frame, 0), 0); /* invalid var: no effects */
    ASSERT(pl_jit_compiled(c));
  }
  c->jit_ops = NULL;
#endif
  for (unsigned env = 0; env < 2; env++) {
    c->max_var = env ? UINT32_MAX : 1;
    for (unsigned fuel = 2; fuel <= 9; fuel++) {
      unsigned expected = 0;
      unsigned modes = 2;
#ifdef PL_SLJIT
      if (native)
        modes = 3;
#endif
      for (unsigned mode = 0; mode < modes; mode++) {
#ifdef PL_SLJIT
        c->jit_ops = mode == 2 ? native : NULL;
#endif
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

#if defined(PL_SLJIT) && !defined(PL_GC_STRESS) && !defined(PL_CACHE_STATS)
TEST(batch, native_guards_and_known_thunk_layout) {
  if (!batching_enabled() ||
      (getenv("PL_JIT") && strcmp(getenv("PL_JIT"), "1") != 0))
    SKIP_TEST("native batches disabled");
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_vpush(t, test_p66(t));
  pl_val ops[] = {OP_PUSH_VAR,
                  1,
                  OP_PUSH_LIT,
                  0,
                  OP_MK_THK,
                  2,
                  PL_BAN_PRIM_KNOWN | PL_BAN_NOUPD,
                  t->vstack[0],
                  ax_s3('D', 'i', 'v'),
                  OP_PUSH_LIT,
                  0,
                  OP_PUSH_SLOT,
                  0,
                  OP_MK_APP,
                  1,
                  OP_RET};
  pl_code* c = decode(t, ops, sizeof ops / sizeof ops[0]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->nbatches, 1);
  ASSERT_NOT_NULL(c->jit_ops);
  ASSERT_EQ(c->ops[0], OP_PUSH_VAR);
  ASSERT_EQ(c->jit_ops[0], OP_JIT);
  for (uint32_t k = 2; k < c->batches[0].end;
       k += (uint32_t)pl_bytecode_width(c->ops + k))
    ASSERT(c->jit_ops[k] != OP_JIT); /* no entry after partial construction */
  pl_vpush(t, test_app1(t, 0, 777));
  pl_frame frame = {.code = c, .argc = 2, .argbase = 2, .b = 0};
  pl_cell* limit = t->heap->limit;
  t->heap->limit =
      t->heap->free; /* compile with a deliberately failing guard */
  t->fuel = 123;
  for (unsigned k = 0; k < 1000; k++) {
    ASSERT_EQ(pl_jit_run(t, &frame, 0), 0);
    ASSERT_EQ(t->vsp, 2);
    ASSERT_EQ(t->heap->free, t->heap->limit);
    ASSERT_EQ(t->fuel, 123);
  }
  t->heap->limit = limit;
  ASSERT(pl_jit_compiled(c));
  pl_cell* before = t->heap->free;
  size_t capacity = t->vcap;
  t->vcap = t->vsp + c->batches[0].extra - 1;
  ASSERT_EQ(pl_jit_run(t, &frame, 0), 0);
  ASSERT_EQ(t->heap->free, before);
  ASSERT_EQ(t->vsp, 2);
  t->vcap = capacity;
  frame.argc = 1;
  ASSERT_EQ(pl_jit_run(t, &frame, 0), 0);
  ASSERT_EQ(t->heap->free, before);
  ASSERT_EQ(t->vsp, 2);
  frame.argc = 2;
  /* An exact fit must work; this must execute the native stores. */
  t->heap->limit = before + c->batches[0].cells;
  ASSERT_EQ(pl_jit_run(t, &frame, 0), c->batches[0].end);
  ASSERT_EQ(t->heap->free, t->heap->limit);
  ASSERT_EQ(t->vsp, 4);
  ASSERT_EQ(t->fuel, 123);
  t->heap->limit = limit;
  pl_gc_collect_now(t);
  pl_val thunk = t->vstack[2];
  pl_cell* th = pl_ptr(thunk);
  ASSERT_EQ(pl_tag(thunk), PL_TAG_DEFER);
  ASSERT_EQ(th[0], pl_hdr_make(PL_K_THKE, 0, 0, PL_THKE_CELLS(3)));
  ASSERT_EQ(th[1], PL_BAN_PRIM_KNOWN | PL_BAN_NOUPD);
  ASSERT_EQ(th[2], c->ops[7]);
  ASSERT_EQ(th[3], t->vstack[1]);
  ASSERT_EQ(th[4], 0); /* Div-by-zero remains lazy, and was never executed. */
  pl_cell* row = pl_as(PL_TAG_APP, t->vstack[3]);
  ASSERT_NOT_NULL(row);
  ASSERT_EQ(row[0], pl_hdr_make(PL_K_APP, 0, 0, PL_APP_CELLS(1)));
  ASSERT_EQ(pl_app_args(row)[0], thunk);
  pl_bytecode_free(c);
  test_rt_free(&rt);
}

typedef struct {
  pl_store* store;
  pl_code* code;
  bool ok;
} batch_worker;

static void* native_batch_worker(void* opaque) {
  batch_worker* w = opaque;
  pl_heap* heap = pl_heap_new(4096, w->store);
  pl_thread* t = pl_thread_new(heap);
  pl_frame frame = {.code = w->code};
  w->ok = true;
  unsigned ran = 0;
  for (unsigned k = 0; k < 1000; k++) {
    t->vsp = 0;
    t->fuel = 100;
    pl_gc_reserve(t, w->code->batches[0].cells);
    uint32_t pc = pl_jit_run(t, &frame, 0);
    if (pc == 0) {
      w->ok &= t->vsp == 0;
      continue;
    }
    ran++;
    w->ok &= pc == w->code->batches[0].end && t->vsp == 2 && t->fuel == 100;
    pl_gc_collect_now(t);
    pl_cell* row = pl_as(PL_TAG_APP, t->vstack[1]);
    pl_cell* partial = pl_as(PL_TAG_APP, t->vstack[0]);
    w->ok &= row && pl_app_args(row)[0] == t->vstack[0];
    w->ok &= partial && pl_app_need(partial) == 2 &&
             pl_pin_resolved(pl_ptr(pl_app_head(partial))) ==
                 pl_pin_resolved(pl_ptr(w->code->ops[1])) &&
             pl_app_args(partial)[0] == 7;
  }
  w->ok &= ran != 0;
  pl_thread_free(t);
  pl_heap_free(heap);
  return NULL;
}

TEST(batch, native_concurrent_heaps) {
  if (!batching_enabled() ||
      (getenv("PL_JIT") && strcmp(getenv("PL_JIT"), "1") != 0))
    SKIP_TEST("native batches disabled");
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, pl_pin(rt.t, test_law(rt.t, 3, 0, 0)));
  char error[192];
  ASSERT(
      pl_store_save_root(rt.store, rt.t->vstack[0], NULL, error, sizeof error));
  pl_val ops[] = {OP_PUSH_LIT,  0, OP_PUSH_LIT, 7, OP_MK_APP, 1, OP_PUSH_LIT, 0,
                  OP_PUSH_SLOT, 0, OP_MK_APP,   1, OP_RET};
  /* Shared code literals must be store-resident, not a proxy in rt's heap. */
  ops[1] = pl_make(PL_TAG_PIN, pl_pin_resolved(pl_ptr(rt.t->vstack[0])));
  pl_code* c = decode(rt.t, ops, sizeof ops / sizeof ops[0]);
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->nbatches, 1);
  ASSERT_NOT_NULL(c->jit_ops);
  batch_worker warm = {.store = rt.store, .code = c};
  (void)native_batch_worker(&warm);
  ASSERT(warm.ok);
  pthread_t threads[4];
  batch_worker workers[4];
  for (unsigned k = 0; k < 4; k++) {
    workers[k] = (batch_worker){.store = rt.store, .code = c};
    ASSERT_EQ(
        pthread_create(&threads[k], NULL, native_batch_worker, &workers[k]), 0);
  }
  for (unsigned k = 0; k < 4; k++) {
    ASSERT_EQ(pthread_join(threads[k], NULL), 0);
    ASSERT(workers[k].ok);
  }
  ASSERT(pl_jit_compiled(c));
  pl_bytecode_free(c);
  test_rt_free(&rt);
}
#endif
