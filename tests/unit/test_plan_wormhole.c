#include "test.h"
#include "test_plan.h"
#include "../../pkg/plan/src/gc2_internal.h"

static const char worm_type;
static const char other_type;

static pl_val worm(pl_thread* t) {
  uint64_t bytes = UINT64_C(0xffffffffffffffff);
  return pl_mk_wormhole(t, &worm_type, &bytes, sizeof(bytes));
}

static pl_val lazy_call(pl_thread* t, pl_val f, pl_val x) {
  size_t base = t->vsp;
  pl_vpush(t, f);
  pl_vpush(t, x);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val out = pl_mk_thke(t, PL_BAN_SLOW, 2, &t->vstack[base]);
  t->vsp = base;
  return out;
}

static void collect_shared(pl_store* s) {
  pl_store_gc_collect(s);
  pl_store_gc_wait(s);
}

static void host_root(pl_root_visit visit, void* ctx, void* src) {
  visit(src, ctx);
}

static pl_run_status drive(pl_thread* t) {
  pl_run_status s;
  unsigned steps = 0;
  while ((s = pl_thread_run(t, 2)) == PL_RUN_YIELDED) {
    ASSERT_LT(++steps, 1000);
    pl_gc_collect_now(t);
    collect_shared(pl_heap_store(t->heap));
  }
  return s;
}

TEST(wormhole, variable_bytes_are_opaque_and_stable_in_gc2) {
  test_rt rt = test_rt_new();
  const size_t lengths[] = {0, 1, 7, 8, 9, 65537};
  unsigned char bytes[65537];
  memset(bytes, 0xff, sizeof(bytes)); /* would be invalid GC edges */
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t n = lengths[i];
    size_t headroom = pl_gc_headroom(rt.t);
    pl_vpush(rt.t, pl_mk_wormhole(rt.t, &worm_type, bytes, n));
    ASSERT_EQ(pl_gc_headroom(rt.t), headroom);
    pl_val before = rt.t->vstack[i];
    void* data = pl_wormhole_data(before, &worm_type);
    ASSERT(pl_store_owns(rt.store, before));
    pl_gc_collect_now(rt.t);
    collect_shared(rt.store);
    pl_val v = rt.t->vstack[i];
    ASSERT_EQ(v, before);
    ASSERT_EQ((uintptr_t)pl_wormhole_data(v, &worm_type), (uintptr_t)data);
    ASSERT(pl_is_wormhole(v));
    ASSERT_FALSE(pl_is_whnf(v));
    ASSERT_EQ(pl_wormhole_size(v), n);
    ASSERT_MEM_EQ(pl_wormhole_data(v, &worm_type), bytes, n);
    ASSERT_NULL(pl_wormhole_data(v, &other_type));
  }
  ASSERT_GT(pl_store_gc_stats(rt.store).allocated_bytes, 65537);
  rt.t->vsp = 0;
  collect_shared(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  ASSERT_EQ(pl_store_gc_stats(rt.store).span_bytes, 0);
  ASSERT_EQ(pl_wormhole_cells(SIZE_MAX), 0);
  ASSERT_NULL(pl_wormhole_data(42, &worm_type));
  test_rt_free(&rt);
}

TEST(wormhole, reachable_gc1_edges_keep_buffer_alive) {
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, test_app1(rt.t, 0, worm(rt.t)));
  collect_shared(rt.store);
  pl_gc_collect_now(rt.t);
  collect_shared(rt.store);
  pl_val v = pl_app_args(pl_ptr(rt.t->vstack[0]))[0];
  uint64_t bytes = UINT64_MAX;
  ASSERT_MEM_EQ(pl_wormhole_data(v, &worm_type), &bytes, sizeof(bytes));
  rt.t->vsp = 0; /* dead gc1 objects must not retain their gc2 edges */
  collect_shared(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  test_rt_free(&rt);
}

TEST(wormhole, host_root_outlives_allocating_heap) {
  test_rt rt = test_rt_new();
  pl_heap* heap = pl_heap_new(4096, rt.store);
  pl_thread* t = pl_thread_new(heap);
  pl_val v = worm(t);
  void* data = pl_wormhole_data(v, &worm_type);
  pl_store_gc_add_roots(rt.store, host_root, &v);
  pl_thread_free(t);
  pl_heap_free(heap);
  collect_shared(rt.store);
  ASSERT_EQ((uintptr_t)pl_wormhole_data(v, &worm_type), (uintptr_t)data);
  uint64_t bytes = UINT64_MAX;
  ASSERT_MEM_EQ(data, &bytes, sizeof(bytes));
  pl_store_gc_del_roots(rt.store, host_root, &v);
  collect_shared(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  test_rt_free(&rt);
}

TEST(wormhole, host_observes_thunks_but_ordinary_forcing_rejects) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_vpush(t, worm(t));
  pl_vpush(t, test_law(t, 1, 0, 1)); /* identity */
  pl_vpush(t, lazy_call(t, t->vstack[1], t->vstack[0]));
  pl_thread_start_host(t, t->vstack[2]);
  ASSERT_EQ(drive(t), PL_RUN_DONE);
  ASSERT_EQ(pl_thread_result(t), t->vstack[0]);
  ASSERT_EQ(pl_host_value(t, t->vstack[2]), t->vstack[0]);
  pl_thread_start(t, t->vstack[2]);
  ASSERT_EQ(drive(t), PL_RUN_EXN);
  ASSERT_STR_EQ(t->exn_msg, "<<wormhole>>");
  pl_thread_start_nf(t, test_app1(t, 0, t->vstack[0]));
  ASSERT_EQ(drive(t), PL_RUN_EXN);
  ASSERT_STR_EQ(t->exn_msg, "<<wormhole>>");
  /* A constant law can ignore the handle and produce a normal value. */
  pl_vpush(t, test_law(t, 1, 0, 42));
  pl_thread_start_nf(t, lazy_call(t, t->vstack[3], t->vstack[0]));
  ASSERT_EQ(drive(t), PL_RUN_DONE);
  ASSERT_EQ(pl_thread_result(t), 42);
  test_rt_free(&rt);
}

TEST(wormhole, host_observation_does_not_bypass_strict_operands) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_vpush(t, worm(t));
  pl_vpush(t, test_p66(t));
  pl_vpush(t, test_app2(t, ax_s3('S', 'e', 'q'), t->vstack[0], 42));
  pl_thread_start_host(t, lazy_call(t, t->vstack[1], t->vstack[2]));
  ASSERT_EQ(drive(t), PL_RUN_EXN);
  ASSERT_STR_EQ(t->exn_msg, "<<wormhole>>");
  test_rt_free(&rt);
}

TEST(wormhole, pin_and_persistence_reject_reachable_host_bytes) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_vpush(t, worm(t));
  pl_vpush(t, test_app1(t, 0, t->vstack[0]));
  pl_vpush(t, pl_pin(t, t->vstack[1])); /* pin proxy is still lazy */
  char err[256];
  uint8_t hash[32];
  ASSERT_FALSE(
      pl_store_save_pin(rt.store, t->vstack[2], hash, err, sizeof(err)));
  ASSERT_FALSE(
      pl_store_save_root(rt.store, t->vstack[2], hash, err, sizeof(err)));
  ASSERT_FALSE(rt.store->be.get_root(rt.store->be.ctx, hash));
  pl_thread_start_nf(t, t->vstack[2]);
  ASSERT_EQ(drive(t), PL_RUN_EXN);
  ASSERT_STR_EQ(t->exn_msg, "<<wormhole>>");
  ASSERT_FALSE(pl_pin_is_hashed(t->vstack[2]));
  test_rt_free(&rt);
}

static void count_drop(void* context) {
  ++*(unsigned*)context;
}

TEST(wormhole, resource_finalization_and_foreign_pressure) {
  test_rt rt = test_rt_new();
  unsigned drops = 0;
  void* pointer = &drops;
  pl_vpush(rt.t, pl_mk_resource(rt.t, &worm_type, &pointer, sizeof(pointer),
                                count_drop, &drops));
  pl_store_gc_set_threshold(rt.store, 1024);
  pl_store_gc_charge(rt.store, rt.t->vstack[0], 4096);
  ASSERT_EQ(pl_store_gc_stats(rt.store).external_bytes, 4096);
  ASSERT(pl_store_gc_poll(rt.store));
  pl_store_gc_wait(rt.store);
  ASSERT_EQ(drops, 0);
  pl_gc_collect_now(rt.t);
  ASSERT_EQ(pl_wormhole_size(rt.t->vstack[0]), sizeof(pointer));
  rt.t->vsp = 0;
  collect_shared(rt.store);
  ASSERT_EQ(drops, 1);
  ASSERT_EQ(pl_store_gc_stats(rt.store).external_bytes, 0);
  collect_shared(rt.store);
  ASSERT_EQ(drops, 1);
  (void)pl_mk_resource(rt.t, &worm_type, NULL, 0, count_drop, &drops);
  test_rt_free(&rt);
  ASSERT_EQ(drops, 2); /* live resources also close at store teardown */
}

TEST(wormhole, resource_rollback_queues_finalizer_once) {
  test_rt rt = test_rt_new();
  unsigned drops = 0;
  pl_store_gc_wait(rt.store);
  size_t checkpoint = pl_gc2_checkpoint(rt.store);
  pl_val v = pl_mk_resource(rt.t, &worm_type, NULL, 0, count_drop, &drops);
  pl_store_gc_charge(rt.store, v, 8192);
  pl_gc2_rollback(rt.store, checkpoint);
  ASSERT_EQ(drops, 0); /* rollback only queues native destruction */
  pl_store_gc_run_finalizers(rt.store);
  ASSERT_EQ(drops, 1);
  ASSERT_EQ(pl_store_gc_stats(rt.store).external_bytes, 0);
  test_rt_free(&rt);
  ASSERT_EQ(drops, 1);
}
