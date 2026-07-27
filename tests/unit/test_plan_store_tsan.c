#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "axsys/profile.h"
#include "plan/build.h"
#include "plan/heap.h"
#include "plan/store.h"
#include "test_plan.h"

typedef struct store_pin_gate {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  size_t ready;
  bool start;
} store_pin_gate;

typedef struct store_pin_worker {
  pl_store* store;
  store_pin_gate* gate;
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

static const char* store_lock_profile_path;
static const char* store_silo_path;

static void* store_pin_thread(void* arg) {
  store_pin_worker* w = arg;
  pl_heap* heap = pl_heap_new(1 << 14, w->store);
  pl_thread* t = pl_thread_new(heap);
  pl_store_profile_locks_prepare_thread(w->store);
  pthread_mutex_lock(&w->gate->mu);
  w->gate->ready++;
  pthread_cond_broadcast(&w->gate->cv);
  while (!w->gate->start)
    pthread_cond_wait(&w->gate->cv, &w->gate->mu);
  pthread_mutex_unlock(&w->gate->mu);

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
  pl_store* store = store_silo_path == NULL
                        ? pl_store_new_mem()
                        : pl_store_new_silo(store_silo_path, (size_t)64 << 20);
  if (store == NULL) {
    fprintf(stderr, "failed to open stress-test store\n");
    return 1;
  }
  if (store_lock_profile_path != NULL &&
      !pl_store_profile_locks(store, store_lock_profile_path)) {
    perror("pl_store_profile_locks");
    pl_store_free(store);
    return 1;
  }
  enum { NTHREADS = 4 };
  store_pin_worker workers[NTHREADS];
  pthread_t threads[NTHREADS];
  bool started[NTHREADS] = {0};
  store_pin_gate gate = {
      .mu = PTHREAD_MUTEX_INITIALIZER,
      .cv = PTHREAD_COND_INITIALIZER,
  };
  size_t started_count = 0;
  int status = 0;
  pl_val canonical[2] = {0};

  for (size_t i = 0; i < NTHREADS; i++) {
    workers[i] = (store_pin_worker){
        .store = store,
        .gate = &gate,
        .group = (unsigned)(i % 2),
        .ok = true,
    };
    int err = pthread_create(&threads[i], NULL, store_pin_thread, &workers[i]);
    if (err != 0) {
      fprintf(stderr, "pthread_create(%zu) failed: %d\n", i, err);
      status = 1;
      break;
    }
    started[i] = true;
    started_count++;
  }
  pthread_mutex_lock(&gate.mu);
  while (gate.ready != started_count)
    pthread_cond_wait(&gate.cv, &gate.mu);
  gate.start = true;
  pthread_cond_broadcast(&gate.cv);
  pthread_mutex_unlock(&gate.mu);

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
  pthread_cond_destroy(&gate.cv);
  pthread_mutex_destroy(&gate.mu);
  if (!pl_store_profile_locks_finish(store)) {
    fprintf(stderr, "failed to finalize store lock profile\n");
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

int main(int argc, char** argv) {
  const char* profile_json_path = NULL;
  for (int argi = 1; argi < argc;) {
    const char* value = argi + 1 < argc ? argv[argi + 1] : NULL;
    if (value != NULL &&
        (strcmp(argv[argi], "--jobs") == 0 || strcmp(argv[argi], "-j") == 0)) {
      /* `make test-unit` passes Criterion's job option to every unit binary,
       * including these standalone TSAN harnesses. */
      argi += 2;
      continue;
    }
    if (strncmp(argv[argi], "--jobs=", sizeof("--jobs=") - 1) == 0 ||
        (strncmp(argv[argi], "-j", sizeof("-j") - 1) == 0 &&
         argv[argi][sizeof("-j") - 1] != '\0')) {
      argi++;
      continue;
    }
    if (value != NULL && strcmp(argv[argi], "--profile-locks") == 0) {
      store_lock_profile_path = value;
    } else if (value != NULL && strcmp(argv[argi], "--profile-json") == 0) {
      profile_json_path = value;
    } else if (value != NULL && strcmp(argv[argi], "--silo") == 0) {
      store_silo_path = value;
    } else {
      fprintf(stderr,
              "usage: %s [--profile-locks FILE] [--profile-json FILE] "
              "[--silo DIR]\n",
              argv[0]);
      return 2;
    }
    argi += 2;
  }
  if (profile_json_path != NULL && !ax_profile_json_start(profile_json_path)) {
    fprintf(stderr, "failed to start profile JSON\n");
    return 1;
  }
  int status = test_concurrent_saves_publish_equal_and_distinct_values();
  if (test_pin_code_publication_is_atomic() != 0)
    status = 1;
  if (!ax_profile_json_finish()) {
    fprintf(stderr, "failed to finish profile JSON\n");
    status = 1;
  }
  return status;
}
