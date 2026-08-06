#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "plan/build.h"
#include "plan/heap.h"
#include "plan/store.h"
#include "test.h"
#include "test_plan.h"

typedef struct store_pin_worker {
  pl_store* store;
  pl_val canonical;
  unsigned group;
  bool ok;
} store_pin_worker;

typedef struct pin_code_race {
  pl_cell* pin;
  pl_code* first;
  pl_code* second;
  bool start;
  bool ok;
} pin_code_race;

static void* store_pin_thread(void* arg) {
  store_pin_worker* w = arg;
  pl_heap* heap = pl_heap_new(1 << 14, w->store);
  pl_thread* t = pl_thread_new(heap);
  for (int i = 0; i < 100; i++) {
    size_t base = t->vsp;
    pl_vpush(t, test_law(t, 1, ax_s4('T', 's', 'a', 'n'), 42 + w->group));
    t->vstack[base] = pl_pin(t, t->vstack[base]);
    pl_val proxy = t->vstack[base];
    pl_cell* pp = pl_as(PL_TAG_PIN, proxy);
    if (pp == NULL || !pl_pin_is_proxy(pp) || pl_pin_proxy_target(pp) != 0 ||
        pl_store_owns(w->store, proxy)) {
      w->ok = false;
      break;
    }

    uint8_t hash[32];
    char err[192] = {0};
    if (!pl_store_save_root(w->store, proxy, hash, err, sizeof(err))) {
      fprintf(stderr, "concurrent Save failed: %s\n", err);
      w->ok = false;
      break;
    }
    pl_val canonical = pl_pin_proxy_target(pl_ptr(t->vstack[base]));
    if (canonical == 0 || !pl_store_owns(w->store, canonical) ||
        !pl_pin_is_hashed(canonical) || pl_store_load(t, hash) != canonical) {
      w->ok = false;
      break;
    }
    if (w->canonical != 0 && w->canonical != canonical) {
      w->ok = false;
      break;
    }
    w->canonical = canonical;
    t->vsp = base;
  }
  pl_thread_free(t);
  pl_heap_free(heap);
  return NULL;
}

static void* pin_code_writer(void* arg) {
  pin_code_race* race = arg;
  while (!__atomic_load_n(&race->start, __ATOMIC_ACQUIRE)) {
  }
  for (size_t i = 0; i < 100000; i++)
    pl_pin_set_code(race->pin, (i & 1u) != 0 ? race->first : race->second);
  return NULL;
}

static void* pin_code_reader(void* arg) {
  pin_code_race* race = arg;
  while (!__atomic_load_n(&race->start, __ATOMIC_ACQUIRE)) {
  }
  for (size_t i = 0; i < 100000; i++) {
    void* code = pl_pin_code(race->pin);
    if (code != NULL && code != race->first && code != race->second) {
      race->ok = false;
      break;
    }
  }
  return NULL;
}

static int test_concurrent_saves_publish_equal_and_distinct_values(void) {
  pl_store* store = pl_store_new_mem();
  enum { NTHREADS = 4 };
  store_pin_worker workers[NTHREADS];
  pthread_t threads[NTHREADS];
  bool started[NTHREADS] = {0};
  int status = 0;
  pl_val canonical[2] = {0};

  for (size_t i = 0; i < NTHREADS; i++) {
    workers[i] = (store_pin_worker){
        .store = store, .group = (unsigned)(i % 2), .ok = true};
    int err = pthread_create(&threads[i], NULL, store_pin_thread, &workers[i]);
    if (err != 0) {
      fprintf(stderr, "pthread_create(%zu) failed: %d\n", i, err);
      status = 1;
      break;
    }
    started[i] = true;
  }
  for (size_t i = 0; i < NTHREADS; i++) {
    if (!started[i])
      continue;
    int err = pthread_join(threads[i], NULL);
    if (err != 0) {
      fprintf(stderr, "pthread_join(%zu) failed: %d\n", i, err);
      status = 1;
      continue;
    }
    if (!workers[i].ok) {
      fprintf(stderr, "store pin worker %zu failed proxy publication\n", i);
      status = 1;
    } else if (canonical[workers[i].group] == 0) {
      canonical[workers[i].group] = workers[i].canonical;
    } else if (workers[i].canonical != canonical[workers[i].group]) {
      fprintf(stderr,
              "equal concurrent Saves published different canonicals\n");
      status = 1;
    }
  }
  if (canonical[0] == 0 || canonical[1] == 0 || canonical[0] == canonical[1]) {
    fprintf(stderr, "distinct concurrent Saves did not stay distinct\n");
    status = 1;
  }
  pl_store_free(store);
  return status;
}

static int test_pin_code_publication_is_atomic(void) {
  pl_cell pin[PL_PIN_CELLS(0)] = {0};
  pin[0] =
      pl_hdr_make(PL_K_PIN, PL_F_NORMAL | PL_F_PIN_HASHED, 0, PL_PIN_CELLS(0));
  pl_code first = {0};
  pl_code second = {0};
  pin_code_race race = {
      .pin = pin, .first = &first, .second = &second, .ok = true};
  pthread_t writer;
  pthread_t reader;

  int err = pthread_create(&writer, NULL, pin_code_writer, &race);
  if (err != 0) {
    fprintf(stderr, "pin code writer pthread_create failed: %d\n", err);
    return 1;
  }
  err = pthread_create(&reader, NULL, pin_code_reader, &race);
  if (err != 0) {
    fprintf(stderr, "pin code reader pthread_create failed: %d\n", err);
    __atomic_store_n(&race.start, true, __ATOMIC_RELEASE);
    (void)pthread_join(writer, NULL);
    return 1;
  }

  __atomic_store_n(&race.start, true, __ATOMIC_RELEASE);
  int status = 0;
  err = pthread_join(writer, NULL);
  if (err != 0) {
    fprintf(stderr, "pin code writer pthread_join failed: %d\n", err);
    status = 1;
  }
  err = pthread_join(reader, NULL);
  if (err != 0) {
    fprintf(stderr, "pin code reader pthread_join failed: %d\n", err);
    status = 1;
  }
  if (!race.ok) {
    fprintf(stderr, "pin code reader observed an invalid code pointer\n");
    status = 1;
  }
  return status;
}

/*
 * Staged compilation under the race it is built on: the worker publishes
 * optimized code for laws the main thread is calling at the same time.
 * Every call must return one generation's answer — the interpreted body
 * (42) or the optimized program's constant (9) — and never a torn read.
 */
enum { STAGE_LAWS = 8 };

static pl_val stage_saved_law(test_rt* rt, uint64_t name, pl_val body,
                              uint8_t out_hash[32]) {
  pl_thread* t = rt->t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 1, name, body));
  pl_val proxy = pl_pin(t, t->vstack[base]);
  t->vstack[base] = proxy;
  char err[192] = {0};
  ASSERT(pl_store_save_root(rt->store, t->vstack[base], out_hash, err,
                            sizeof(err)),
         "%s", err);
  pl_val canonical = pl_pin_proxy_target(pl_ptr(t->vstack[base]));
  ASSERT_NEQ(canonical, 0);
  t->vsp = base;
  return canonical;
}

TEST(store_tsan, staged_upgrades_race_with_law_entry) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_store* s = rt.store;
  size_t base = t->vsp;

  /* an arity-1 law whose literal body is the code row "return 9" */
  pl_val ops[3] = {OP_PUSH_LIT, 9, OP_RET};
  pl_vpush(t, test_app(t, 99, 3, ops));
  pl_val compiler_law = test_law(t, 1, ax_s3('o', 'p', 't'), t->vstack[base]);
  t->vstack[base] = compiler_law;
  pl_val proxy = pl_pin(t, t->vstack[base]);
  t->vstack[base] = proxy;
  uint8_t opt_hash[32] = {0};
  char err[192] = {0};
  ASSERT(pl_store_save_root(s, t->vstack[base], opt_hash, err, sizeof(err)),
         "%s", err);
  pl_val compiler = pl_pin_proxy_target(pl_ptr(t->vstack[base]));
  t->vsp = base;

  uint8_t fast_hash[32];
  for (size_t i = 0; i < sizeof(fast_hash); i++)
    fast_hash[i] = (uint8_t)i;
  ASSERT(pl_store_put_compiler(s, fast_hash));
  ASSERT(pl_store_put_opt_compiler(s, opt_hash));
  ASSERT(pl_store_stage_drain(s));
  pl_pin_set_code(pl_ptr(compiler), NULL); /* the constant compiler is not
                                              self-hosting */

  pl_val laws[STAGE_LAWS];
  for (size_t i = 0; i < STAGE_LAWS; i++) {
    uint8_t law_hash[32] = {0};
    laws[i] = stage_saved_law(&rt, ax_s2('l', (char)('a' + i)), 42, law_hash);
  }
  /* the worker is upgrading these while this loop enters them */
  for (size_t round = 0; round < 200; round++) {
    for (size_t i = 0; i < STAGE_LAWS; i++) {
      pl_val r = test_call1(t, laws[i], 0);
      ASSERT(r == 42 || r == 9, "law entry saw neither generation: %llu",
             (unsigned long long)r);
    }
  }
  ASSERT(pl_store_stage_drain(s));
  for (size_t i = 0; i < STAGE_LAWS; i++)
    ASSERT_EQ(test_call1(t, laws[i], 0), 9);
  test_rt_free(&rt);
}

TEST(store_tsan, concurrent_saves_publish_equal_and_distinct_values) {
  ASSERT_EQ(test_concurrent_saves_publish_equal_and_distinct_values(), 0);
}

TEST(store_tsan, pin_code_publication_is_atomic) {
  ASSERT_EQ(test_pin_code_publication_is_atomic(), 0);
}
