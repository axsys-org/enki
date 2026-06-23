#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "plan/build.h"
#include "plan/heap.h"
#include "plan/store.h"
#include "test_plan.h"

typedef struct store_pin_worker {
  pl_store* store;
  bool ok;
} store_pin_worker;

static void* store_pin_thread(void* arg) {
  store_pin_worker* w = arg;
  pl_heap* heap = pl_heap_new(1 << 14, w->store);
  pl_thread* t = pl_thread_new(heap);
  for (int i = 0; i < 100; i++) {
    size_t base = t->vsp;
    pl_vpush(t, test_law(t, 1, ax_s4('T', 's', 'a', 'n'), 42));
    pl_val pin = pl_pin(t, t->vstack[base]);
    if (!pl_store_owns(w->store, pin)) {
      w->ok = false;
      break;
    }
    t->vsp = base;
  }
  pl_thread_free(t);
  pl_heap_free(heap);
  return NULL;
}

static int test_concurrent_pin_interns_store_values(void) {
  pl_store* store = pl_store_new_mem();
  enum { NTHREADS = 4 };
  store_pin_worker workers[NTHREADS];
  pthread_t threads[NTHREADS];
  bool started[NTHREADS] = {0};
  int status = 0;

  for (size_t i = 0; i < NTHREADS; i++) {
    workers[i] = (store_pin_worker){.store = store, .ok = true};
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
      fprintf(stderr, "store pin worker %zu produced a non-store pin\n", i);
      status = 1;
    }
  }
  pl_store_free(store);
  return status;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  return test_concurrent_pin_interns_store_values();
}
