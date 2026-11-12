#include "test.h"
#include <unistd.h>
#include "test_plan.h"
#include "plan/mesa.h"
#include "../../pkg/plan/src/internal.h"
#ifdef PL_MESA
static pl_val bar(pl_thread* t, const void* bytes, size_t n) {
  uint8_t* b = malloc(n + 1);
  ASSERT_NOT_NULL(b);
  memcpy(b, bytes, n);
  b[n] = 1;
  pl_val v = pl_nat_from_bytes(t, b, n + 1);
  free(b);
  return v;
}
static pl_val field(pl_val v, size_t i) {
  return pl_app_args(pl_ptr(v))[i];
}
/* Each result stays rooted; indices remain valid under gc1 stress. */
static size_t invoke(test_rt* rt, const char* op, pl_val handles, pl_val data) {
  pl_thread* t = rt->t;
  size_t base = t->vsp;
  pl_vpush(t, handles);
  pl_vpush(t, data);
  pl_vpush(t, pl_mesa_call(t, op, t->vstack[base], t->vstack[base + 1]));
  pl_val out = t->vstack[base + 2];
  t->vsp = base;
  pl_vpush(t, out);
  return base;
}
static pl_val handle(test_rt* rt, size_t result) {
  ASSERT_EQ(field(rt->t->vstack[result], 0), 0);
  return field(field(rt->t->vstack[result], 2), 0);
}
static pl_val one(pl_thread* t, pl_val x) {
  return test_app1(t, 0, x);
}
TEST(mesa, native_handles_move_close_and_collect) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  uint8_t seed[32] = {7};
  pl_vpush(t, one(t, bar(t, seed, sizeof(seed))));
  size_t key = invoke(&rt, "MesaKeyFromSeed", 0, t->vstack[0]);
  ASSERT(pl_is_mesa_handle(handle(&rt, key)));
  size_t pub = invoke(&rt, "MesaKeyPublic", one(t, handle(&rt, key)), 0);
  size_t bytes = invoke(&rt, "MesaPublicBytes", one(t, handle(&rt, pub)), 0);
  ASSERT_EQ(pl_nat_byte_len(field(t->vstack[bytes], 1)), 33);
  pl_val stable = handle(&rt, key);
  pl_gc_collect_now(t);
  pl_store_gc_collect(rt.store);
  pl_store_gc_wait(rt.store);
  ASSERT_EQ(handle(&rt, key), stable);
  size_t close = invoke(&rt, "MesaClose", one(t, handle(&rt, key)), 0);
  ASSERT_EQ(field(t->vstack[close], 0), 0);
  size_t invalid = invoke(&rt, "MesaKeyPublic", one(t, stable), 0);
  ASSERT_EQ(field(t->vstack[invalid], 0), 1);
  t->vsp = 0;
  pl_store_gc_collect(rt.store);
  pl_store_gc_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).external_bytes, 0);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  test_rt_free(&rt);
}
TEST(mesa, syscall_host_arguments_and_persistence_boundary) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  pl_vpush(t, pl_pin(t, 83));
  uint8_t b[] = {0, 12, 0};
  pl_vpush(t, one(t, bar(t, b, sizeof(b))));
  size_t buffer = invoke(&rt, "MesaBuffer", 0, t->vstack[1]);
  pl_vpush(t, one(t, handle(&rt, buffer))); /* 3 */
  pl_vpush(t,
           pl_nat_from_bytes(t, (const uint8_t*)"MesaBufferBytes", 15)); /* 4 */
  pl_val args[2] = {t->vstack[3], 0};
  pl_vpush(t, test_app(t, t->vstack[4], 2, args));
  pl_vpush(t, t->vstack[0]);
  pl_vpush(t, t->vstack[5]);
  pl_gc_reserve(t, PL_THKE_CELLS(2));
  pl_val call = pl_mk_thke(t, PL_BAN_SLOW, 2, &t->vstack[6]);
  t->vsp = 6;
  pl_vpush(t, call);
  pl_thread_start_host(t, t->vstack[6]);
  pl_run_status s;
  while ((s = pl_thread_run(t, 2)) == PL_RUN_YIELDED) {
    pl_gc_collect_now(t);
    pl_store_gc_collect(rt.store);
    pl_store_gc_wait(rt.store);
  }
  ASSERT_EQ(s, PL_RUN_DONE, "%s", t->exn_msg);
  ASSERT_EQ(field(pl_thread_result(t), 0), 0);
  pl_val result = field(pl_thread_result(t), 1);
  ASSERT_EQ(pl_nat_byte_len(result), 4);
  ASSERT_EQ(pl_nat_byte_at(result, 0), 0);
  ASSERT_EQ(pl_nat_byte_at(result, 1), 12);
  ASSERT_EQ(pl_nat_byte_at(result, 2), 0);
  pl_thread_start_nf(t, t->vstack[buffer]);
  while ((s = pl_thread_run(t, 2)) == PL_RUN_YIELDED) {
  }
  ASSERT_EQ(s, PL_RUN_EXN);
  ASSERT_STR_EQ(t->exn_msg, "<<wormhole>>");
  t->rplan_no_resources = true;
  size_t denied = invoke(&rt, "MesaBufferBytes", t->vstack[3], 0);
  ASSERT_EQ(field(t->vstack[denied], 0), 1);
  test_rt_free(&rt);
}
TEST(mesa, all_registered_operations_are_lookupable) {
  test_rt rt = test_rt_new();
#define MESA_OP(name)                                                          \
  {                                                                            \
    pl_val n =                                                                 \
        pl_nat_from_bytes(rt.t, (const uint8_t*)name, sizeof(name) - 1);       \
    int i = pl_op_lookup(83, n, 2);                                            \
    ASSERT_GTE(i, 0, "%s", name);                                              \
    ASSERT_EQ(pl_ops[i].caps_mask, 1);                                         \
    ASSERT_EQ(pl_ops[i].deep_mask, 2);                                         \
  }
#include "../../pkg/mesa/include/ops.def"
#undef MESA_OP
  test_rt_free(&rt);
}

/* Exercise the compiler's known-op path and collect at every yield. */
static size_t vm_call(test_rt* rt, const char* op, size_t n, const size_t* refs,
                      pl_val data) {
  pl_thread* t = rt->t;
  size_t base = t->vsp;
  pl_vpush(t, data);
  for (size_t i = 0; i < n; i++)
    pl_vpush(t, handle(rt, refs[i]));
  pl_val hs = n ? test_app(t, 0, n, t->vstack + base + 1) : 0;
  t->vsp = base + 1;
  pl_vpush(t, hs);
  pl_val name = pl_nat_from_bytes(t, (const uint8_t*)op, strlen(op));
  int idx = pl_op_lookup(83, name, 2);
  ASSERT_GTE(idx, 0);
  pl_gc_reserve(t, PL_THKE_CELLS(3));
  pl_val args[2] = {t->vstack[base + 1], t->vstack[base]};
  pl_val call = pl_mk_thke_known(t, (uint32_t)idx, 2, args);
  pl_vpush(t, call);
  t->rplan_f = true;
  pl_thread_start_host(t, call);
  pl_run_status status;
  while ((status = pl_thread_run(t, 2)) == PL_RUN_YIELDED) {
    pl_gc_collect_now(t);
    pl_store_gc_collect(rt->store);
    pl_store_gc_wait(rt->store);
  }
  ASSERT_EQ(status, PL_RUN_DONE, "%s", t->exn_msg);
  pl_val result = pl_thread_result(t);
  t->vsp = base;
  pl_vpush(t, result);
  ASSERT_EQ(field(result, 0), 0, "%s", op);
  return base;
}
static size_t vm_peer(test_rt* rt, size_t id) {
  pl_val domain = bar(rt->t, "gw-btc", 6);
  pl_val fields[] = {id, domain, 1, 0, id, 1};
  return vm_call(rt, "MesaPeer", 0, NULL, test_app(rt->t, 0, 6, fields));
}
static bool tag_is(pl_val v, const char* s) {
  size_t n = strlen(s);
  if (!pl_is_nat(v) || pl_nat_byte_len(v) != n + 1 || pl_nat_byte_at(v, n) != 1)
    return false;
  for (size_t i = 0; i < n; i++)
    if (pl_nat_byte_at(v, i) != (uint8_t)s[i])
      return false;
  return true;
}
typedef struct {
  size_t side, event;
} mesa_message;
static bool pump_actions(test_rt* rt, size_t side, size_t batch,
                         mesa_message* queue, size_t* queued,
                         const uint8_t* expected, size_t length) {
  pl_thread* t = rt->t;
  for (;;) {
    size_t out = vm_call(rt, "MesaActionsNext", 1, &batch, 0);
    pl_val a = field(t->vstack[out], 1);
    if (!a)
      return false;
    if (tag_is(field(a, 0), "Send")) {
      ASSERT_LT(*queued, 1000);
      pl_val args[] = {side + 1, 7, field(a, 3)};
      size_t event = t->vsp;
      pl_vpush(t, test_app(t, 0, 3, args));
      queue[(*queued)++] = (mesa_message){1 - side, event};
    } else if (tag_is(field(a, 0), "ReadFinished")) {
      ASSERT_EQ(field(a, 2), 0);
      pl_val bytes = field(a, 3);
      ASSERT_EQ(pl_nat_byte_len(bytes), length + 1);
      for (size_t i = 0; i < length; i++)
        ASSERT_EQ(pl_nat_byte_at(bytes, i), expected[i]);
      return true;
    }
  }
}
TEST(mesa, signed_fragmented_exchange_through_plan_syscalls) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t keys[2], publics[2], peers[2], engines[2];
  for (size_t i = 0; i < 2; i++) {
    uint8_t seed[32];
    memset(seed, (int)(11 * (i + 1)), sizeof(seed));
    keys[i] = vm_call(&rt, "MesaKeyFromSeed", 0, NULL,
                      one(t, bar(t, seed, sizeof(seed))));
    publics[i] = vm_call(&rt, "MesaKeyPublic", 1, &keys[i], 0);
    peers[i] = vm_peer(&rt, i + 1);
  }
  size_t cfg = vm_call(&rt, "MesaConfig", 0, NULL, 0);
  for (size_t i = 0; i < 2; i++) {
    size_t refs[] = {keys[i], publics[1 - i]};
    pl_val d = test_app2(t, 0, i + 1, one(t, 2 - i));
    size_t protection = vm_call(&rt, "MesaProtection", 2, refs, d);
    size_t er[] = {cfg, peers[i], protection};
    engines[i] = vm_call(&rt, "MesaEngine", 3, er, 0);
  }
  uint8_t payload[5000];
  for (size_t i = 0; i < sizeof(payload); i++)
    payload[i] = (uint8_t)(i % 251);
  payload[sizeof(payload) - 1] = 0;
  mesa_message queue[1000];
  size_t queued = 0;
  for (size_t i = 0; i < 2; i++) {
    size_t refs[] = {engines[i], peers[1 - i]};
    size_t a = vm_call(&rt, "MesaEngineInstallPeer", 2, refs, 0);
    ASSERT_FALSE(
        pump_actions(&rt, i, a, queue, &queued, payload, sizeof(payload)));
    pl_val d = test_app2(t, 0, 2 - i, one(t, 7));
    a = vm_call(&rt, "MesaEngineSetAttachment", 1, &engines[i], d);
    ASSERT_FALSE(
        pump_actions(&rt, i, a, queue, &queued, payload, sizeof(payload)));
  }
  size_t path = t->vsp;
  pl_vpush(t, bar(t, "publ/example", 12));
  size_t bytes = t->vsp;
  pl_vpush(t, bar(t, payload, sizeof(payload)));
  pl_val bind[] = {t->vstack[path], t->vstack[bytes], 0};
  size_t a =
      vm_call(&rt, "MesaEngineBind", 1, &engines[1], test_app(t, 0, 3, bind));
  ASSERT_FALSE(
      pump_actions(&rt, 1, a, queue, &queued, payload, sizeof(payload)));
  pl_val read[] = {99, 2, t->vstack[path]};
  a = vm_call(&rt, "MesaEngineRead", 1, &engines[0], test_app(t, 0, 3, read));
  ASSERT_FALSE(
      pump_actions(&rt, 0, a, queue, &queued, payload, sizeof(payload)));
  bool done = false;
  for (size_t next = 0; next < queued && !done; next++) {
    mesa_message m = queue[next];
    a = vm_call(&rt, "MesaEngineReceive", 1, &engines[m.side],
                t->vstack[m.event]);
    done =
        pump_actions(&rt, m.side, a, queue, &queued, payload, sizeof(payload));
  }
  ASSERT(done);
  test_rt_free(&rt);
}
TEST(mesa, store_paths_obey_file_root) {
  test_rt rt = test_rt_new();
  char root[] = "/tmp/enki-mesa-jail-XXXXXX";
  ASSERT_NOT_NULL(mkdtemp(root));
  rt.t->rplan_file_root_c = root;
  pl_val d = test_app2(rt.t, 0, bar(rt.t, "../escape.sqlite", 16), 0);
  size_t out = invoke(&rt, "MesaStoreCreate", 0, d);
  ASSERT_EQ(field(rt.t->vstack[out], 0), 1);
  ASSERT(tag_is(field(rt.t->vstack[out], 1),
                "invalid Mesa data, path, or size limit"));
  d = test_app2(rt.t, 0, bar(rt.t, "inside.sqlite", 13), 0);
  out = invoke(&rt, "MesaStoreCreate", 0, d);
  ASSERT_EQ(field(rt.t->vstack[out], 0), 1);
  /* Path accepted; the native bridge then rejects the absent Checkpoint. */
  ASSERT(tag_is(field(rt.t->vstack[out], 1), "closed or moved handle"));
  rt.t->rplan_file_root_c = NULL;
  ASSERT_EQ(rmdir(root), 0);
  test_rt_free(&rt);
}
#endif
