#include "test.h"
#include "test_plan.h"
#include "axsys/ds.h"
#include "enki/actor.h"
#include "../../pkg/plan/src/store_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

static pl_val app(pl_store* s, pl_val child) {
  pl_cell* p = pl_store_alloc(s, PL_APP_CELLS(1));
  p[0] = pl_hdr_make(PL_K_APP, PL_F_NORMAL, 0, PL_APP_CELLS(1));
  p[1] = 0;
  p[2] = child;
  return pl_make(PL_TAG_APP, p);
}
static void root(pl_root_visit visit, void* ctx, void* src) {
  visit(src, ctx);
}
static void collect_wait(pl_store* s) {
  pl_store_gc_collect(s);
  pl_store_gc_wait(s);
}

#ifdef __linux__
TEST(gc2, address_window_does_not_reserve_commit) {
  FILE* policy = fopen("/proc/sys/vm/overcommit_memory", "r");
  ASSERT_NOT_NULL(policy);
  int overcommit = -1;
  int scanned = fscanf(policy, "%d", &overcommit);
  ASSERT_EQ(fclose(policy), 0);
  ASSERT_EQ(scanned, 1);
  if (overcommit == 2)
    SKIP_TEST("strict overcommit ignores MAP_NORESERVE");

  test_rt rt = test_rt_new();
  uintptr_t address = (uintptr_t)rt.store->lo;
  FILE* smaps = fopen("/proc/self/smaps", "r");
  ASSERT_NOT_NULL(smaps);
  char line[1024];
  bool found = false, flags = false, noreserve = false, accounted = false;
  while (fgets(line, sizeof(line), smaps) != NULL) {
    unsigned long long start, end;
    if (sscanf(line, "%llx-%llx", &start, &end) == 2) {
      if (found)
        break;
      found = address >= start && address < end;
      if (found)
        ASSERT_GTE(end, (uintptr_t)rt.store->hi);
    } else if (found && strncmp(line, "VmFlags:", 8) == 0) {
      flags = true;
      for (char* flag = strtok(line + 8, " \t\n"); flag != NULL;
           flag = strtok(NULL, " \t\n")) {
        noreserve |= strcmp(flag, "nr") == 0;
        accounted |= strcmp(flag, "ac") == 0;
      }
    }
  }
  ASSERT_EQ(fclose(smaps), 0);
  test_rt_free(&rt);
  ASSERT(found && flags);
  ASSERT(noreserve);
  ASSERT_FALSE(accounted);
}
#endif

TEST(gc2, reachable_actor_graph_and_dead_gc1_edges) {
  test_rt rt = test_rt_new();
  pl_val shared = app(rt.store, 42);
  pl_vpush(rt.t, test_app1(rt.t, 0, shared));
  pl_val addr = shared;
  for (unsigned i = 0; i < 100; i++)
    (void)app(rt.store, i);
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 32);
  ASSERT_EQ(pl_app_args(pl_ptr(rt.t->vstack[0]))[0], addr);
  pl_gc_collect_now(rt.t);
  collect_wait(rt.store);
  ASSERT_EQ(pl_app_args(pl_ptr(addr))[0], 42);
  rt.t->vsp = 0; /* old gc1 object still exists, but is unreachable */
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  ASSERT_EQ(pl_store_gc_stats(rt.store).span_bytes, 0);
  test_rt_free(&rt);
}

TEST(gc2, shared_cycles_and_multiple_heaps) {
  test_rt rt = test_rt_new();
  pl_heap* h = pl_heap_new(4096, rt.store);
  pl_thread* t = pl_thread_new(h);
  pl_val a = app(rt.store, 0), b = app(rt.store, a);
  pl_ptr(a)[2] = b; /* construct the cycle before publication */
  pl_vpush(t, a);
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 64);
  t->vsp = 0;
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  pl_thread_free(t);
  pl_heap_free(h);
  test_rt_free(&rt);
}

TEST(gc2, resolved_proxy_discards_old_body) {
  test_rt rt = test_rt_new();
  pl_val body = app(rt.store, 23);
  pl_val proxy = pl_store_mk_proxy(rt.store, body);
  uint8_t hash[32] = {1};
  pl_val canonical = pl_store_mk_pin(rt.store, hash, 99, 0, NULL);
  pl_vpush(rt.t, proxy);
  collect_wait(rt.store);
  /* The otherwise unrooted canonical was reclaimed; construct it anew. */
  canonical = pl_store_mk_pin(rt.store, hash, 99, 0, NULL);
  pl_pin_set_target(pl_ptr(proxy), canonical);
  collect_wait(rt.store);
  ASSERT_EQ(pl_pin_body(pl_ptr(proxy)), 99);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 128);
  rt.t->vsp = 0;
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  ASSERT_EQ(ax_hmlen(rt.store->intern), 0);
  test_rt_free(&rt);
}

TEST(gc2, weak_intern_eviction_and_reload) {
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, pl_pin(rt.t, 678));
  uint8_t hash[32];
  char err[192];
  ASSERT(pl_store_save_pin(rt.store, rt.t->vstack[0], hash, err, sizeof(err)));
  collect_wait(rt.store);
  ASSERT(ax_hmlen(rt.store->intern) > 0);
  rt.t->vsp = 0;
  collect_wait(rt.store);
  ASSERT_EQ(ax_hmlen(rt.store->intern), 0);
  ASSERT_EQ(ax_arrlen(rt.store->pins), 0);
  pl_vpush(rt.t, pl_store_load(rt.t, hash));
  collect_wait(rt.store);
  ASSERT_EQ(pl_pin_body(pl_ptr(rt.t->vstack[0])), 678);
  test_rt_free(&rt);
}

TEST(gc2, code_literal_survives_without_its_pin) {
  test_rt rt = test_rt_new();
  pl_val literal = app(rt.store, 91);
  pl_code* c = calloc(1, sizeof(*c));
  c->nops = 3;
  c->ops = malloc(3 * sizeof(*c->ops));
  c->ops[0] = OP_PUSH_LIT;
  c->ops[1] = literal;
  c->ops[2] = OP_RET;
  ax_arrpush(rt.store->codes, c); /* retired code retains its literal roots */
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 32);
  ASSERT_EQ(pl_app_args(pl_ptr(literal))[0], 91);
  test_rt_free(&rt);
}

TEST(gc2, active_frame_keeps_external_code_literals) {
  test_rt rt = test_rt_new();
  pl_op_t ops[] = {OP_PUSH_LIT, app(rt.store, 73), OP_RET};
  pl_code c = {.ops = ops, .nops = 3};
  rt.t->fstack[rt.t->fsp++] = (pl_frame){.kind = PL_F_EXEC, .code = &c};
  collect_wait(rt.store);
  ASSERT_EQ(pl_app_args(pl_ptr(ops[1]))[0], 73);
  rt.t->fstack[0].kind = PL_F_EXECV;
  collect_wait(rt.store);
  ASSERT_EQ(pl_app_args(pl_ptr(ops[1]))[0], 73);
  rt.t->fsp = 0;
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  test_rt_free(&rt);
}

TEST(gc2, host_roots_and_mailboxes) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  size_t handles = pl_store_gc_stats(rt.store).allocated_bytes;
  pl_val host = app(rt.store, 101);
  pl_store_gc_add_roots(rt.store, root, &host);
  er_scheduler_inject(sys, a, host);
  pl_store_gc_del_roots(rt.store, root, &host);
  collect_wait(rt.store); /* only the unstarted actor's mailbox owns it */
  ASSERT_EQ(pl_app_args(pl_ptr(host))[0], 101);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, handles + 32);
  er_scheduler_free(sys);
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  test_rt_free(&rt);
}

TEST(gc2, rollback_and_large_extent_reuse) {
  test_rt rt = test_rt_new();
  pl_store_lock(rt.store);
  size_t checkpoint = pl_store_mark(rt.store);
  pl_cell* large = pl_store_alloc(rt.store, 12000);
  (void)app(rt.store, 5);
  pl_store_release(rt.store, checkpoint);
  ASSERT_EQ(pl_store_mark(rt.store), checkpoint);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  pl_cell* again = pl_store_alloc(rt.store, 12000);
  ASSERT_EQ((uintptr_t)again, (uintptr_t)large);
  pl_store_release(rt.store, checkpoint);
  pl_store_unlock(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).span_bytes, 0);
  test_rt_free(&rt);
}

TEST(gc2, allocations_after_mark_survive_concurrent_sweep) {
  test_rt rt = test_rt_new();
  for (unsigned cycle = 0; cycle < 4; cycle++) {
    for (unsigned i = 0; i < 30000; i++)
      (void)app(rt.store, i);
    pl_store_gc_collect(rt.store);
    pl_val chain = 0;
    for (unsigned i = 0; i < 3000; i++)
      chain = app(rt.store, chain);
    pl_vpush(rt.t, chain);
    pl_store_gc_wait(rt.store);
    ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 3000 * 32);
    collect_wait(rt.store);
    ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 3000 * 32);
    for (unsigned i = 0; i < 3000; i++)
      chain = pl_app_args(pl_ptr(chain))[0];
    ASSERT_EQ(chain, 0);
    rt.t->vsp = 0;
  }
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  test_rt_free(&rt);
}

typedef struct handshake {
  pl_store* s;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  bool ready, release;
  atomic_bool collecting, done;
  pl_val value;
} handshake;
static void* mutator(void* arg) {
  handshake* h = arg;
  pl_store_mutator_enter(h->s);
  pl_store_mutator_enter(h->s); /* nested scope must not release the world */
  pl_store_mutator_leave(h->s);
  pl_val a = app(h->s, 8);
  pthread_mutex_lock(&h->mu);
  h->ready = true;
  pthread_cond_broadcast(&h->cv);
  while (!h->release)
    pthread_cond_wait(&h->cv, &h->mu);
  h->value = a; /* publish the root before leaving the active scope */
  pthread_mutex_unlock(&h->mu);
  pl_store_mutator_leave(h->s);
  return NULL;
}
static void* collector(void* arg) {
  handshake* h = arg;
  atomic_store(&h->collecting, true);
  collect_wait(h->s);
  atomic_store(&h->done, true);
  return NULL;
}
TEST(gc2, handshake_waits_for_publication) {
  test_rt rt = test_rt_new();
  handshake h = {.s = rt.store};
  pthread_mutex_init(&h.mu, NULL);
  pthread_cond_init(&h.cv, NULL);
  pl_store_gc_add_roots(rt.store, root, &h.value);
  pthread_t mut, col;
  ASSERT_EQ(pthread_create(&mut, NULL, mutator, &h), 0);
  pthread_mutex_lock(&h.mu);
  while (!h.ready)
    pthread_cond_wait(&h.cv, &h.mu);
  pthread_mutex_unlock(&h.mu);
  ASSERT_EQ(pthread_create(&col, NULL, collector, &h), 0);
  while (!atomic_load(&h.collecting))
    sched_yield();
  struct timespec delay = {.tv_nsec = 10000000};
  nanosleep(&delay, NULL);
  ASSERT_FALSE(atomic_load(&h.done));
  pthread_mutex_lock(&h.mu);
  h.release = true;
  pthread_cond_broadcast(&h.cv);
  pthread_mutex_unlock(&h.mu);
  ASSERT_EQ(pthread_join(mut, NULL), 0);
  ASSERT_EQ(pthread_join(col, NULL), 0);
  ASSERT(atomic_load(&h.done));
  ASSERT_EQ(pl_app_args(pl_ptr(h.value))[0], 8);
  pl_store_gc_del_roots(rt.store, root, &h.value);
  collect_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).allocated_bytes, 0);
  pthread_cond_destroy(&h.cv);
  pthread_mutex_destroy(&h.mu);
  test_rt_free(&rt);
}

TEST(gc2, threshold_requests_only_at_safe_poll) {
  test_rt rt = test_rt_new();
  pl_store_gc_set_threshold(rt.store, 32);
  pl_vpush(rt.t, app(rt.store, 44));
  ASSERT_EQ(pl_store_gc_stats(rt.store).collections, 0);
  ASSERT(pl_store_gc_poll(rt.store));
  pl_store_gc_wait(rt.store);
  ASSERT_EQ(pl_store_gc_stats(rt.store).collections, 1);
  ASSERT_FALSE(pl_store_gc_poll(rt.store));
  ASSERT_EQ(pl_app_args(pl_ptr(rt.t->vstack[0]))[0], 44);
  test_rt_free(&rt);
}

static void* blocking_mutator(void* arg) {
  handshake* h = arg;
  pl_store_mutator_enter(h->s);
  pl_store_mutator_blocking(h->s);
  pthread_mutex_lock(&h->mu);
  h->ready = true;
  pthread_cond_broadcast(&h->cv);
  while (!h->release)
    pthread_cond_wait(&h->cv, &h->mu);
  pthread_mutex_unlock(&h->mu);
  pl_store_mutator_leave(h->s);
  return NULL;
}
TEST(gc2, blocking_effect_defers_gc_without_stopping_other_mutators) {
  test_rt rt = test_rt_new();
  handshake h = {.s = rt.store};
  pthread_mutex_init(&h.mu, NULL);
  pthread_cond_init(&h.cv, NULL);
  pthread_t mut;
  ASSERT_EQ(pthread_create(&mut, NULL, blocking_mutator, &h), 0);
  pthread_mutex_lock(&h.mu);
  while (!h.ready)
    pthread_cond_wait(&h.cv, &h.mu);
  pthread_mutex_unlock(&h.mu);
  pl_store_gc_request(rt.store);
  ASSERT_FALSE(pl_store_gc_poll(rt.store));
  pl_store_mutator_enter(rt.store); /* the actor completing the blocked I/O */
  pthread_mutex_lock(&h.mu);
  h.release = true;
  pthread_cond_broadcast(&h.cv);
  pthread_mutex_unlock(&h.mu);
  pl_store_mutator_leave(rt.store);
  ASSERT_EQ(pthread_join(mut, NULL), 0);
  ASSERT(pl_store_gc_poll(rt.store));
  pthread_cond_destroy(&h.cv);
  pthread_mutex_destroy(&h.mu);
  test_rt_free(&rt);
}
