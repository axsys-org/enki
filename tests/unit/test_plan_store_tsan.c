#include <criterion/criterion.h>

#include <pthread.h>

#include "plan/build.h"
#include "plan/heap.h"
#include "plan/store.h"
#include "test_plan.h"

typedef struct store_pin_worker {
  pl_store* store;
} store_pin_worker;

static void* store_pin_thread(void* arg) {
  store_pin_worker* w = arg;
  pl_heap* heap = pl_heap_new(1 << 14, w->store);
  pl_thread* t = pl_thread_new(heap);
  for (int i = 0; i < 100; i++) {
    size_t base = t->vsp;
    pl_vpush(t, test_law(t, 1, ax_s4('T', 's', 'a', 'n'), 42));
    pl_val pin = pl_pin(t, t->vstack[base]);
    cr_assert(pl_store_owns(w->store, pin));
    t->vsp = base;
  }
  pl_thread_free(t);
  pl_heap_free(heap);
  return NULL;
}

Test(store_tsan, concurrent_pin_interns_store_values) {
  pl_store* store = pl_store_new_mem();
  store_pin_worker w = {.store = store};
  enum { NTHREADS = 4 };
  pthread_t threads[NTHREADS];
  for (size_t i = 0; i < NTHREADS; i++)
    cr_assert_eq(pthread_create(&threads[i], NULL, store_pin_thread, &w), 0);
  for (size_t i = 0; i < NTHREADS; i++)
    cr_assert_eq(pthread_join(threads[i], NULL), 0);
  pl_store_free(store);
}
