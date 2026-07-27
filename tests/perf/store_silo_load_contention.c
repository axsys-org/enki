#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "axsys/profile.h"
#include "plan/heap.h"
#include "plan/store.h"

typedef struct load_gate {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  size_t ready;
  bool start;
} load_gate;

typedef struct load_worker {
  pl_store* store;
  load_gate* gate;
  const uint8_t* hash;
  bool ok;
} load_worker;

static uint64_t now_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    perror("clock_gettime");
    exit(1);
  }
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static bool parse_threads(const char* text, size_t* out) {
  if (text[0] == '\0' || text[0] == '-')
    return false;
  errno = 0;
  char* end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || value == 0 ||
      value > (unsigned long long)SIZE_MAX)
    return false;
  *out = (size_t)value;
  return true;
}

static void* load_worker_main(void* arg) {
  load_worker* worker = arg;
  pl_heap* heap = pl_heap_new(1 << 16, worker->store);
  pl_thread* thread = pl_thread_new(heap);
  pl_store_profile_locks_prepare_thread(worker->store);

  pthread_mutex_lock(&worker->gate->mu);
  worker->gate->ready++;
  pthread_cond_broadcast(&worker->gate->cv);
  while (!worker->gate->start)
    pthread_cond_wait(&worker->gate->cv, &worker->gate->mu);
  pthread_mutex_unlock(&worker->gate->mu);

  pl_val value = pl_store_load(thread, worker->hash);
  worker->ok = pl_store_owns(worker->store, value) && pl_pin_is_hashed(value);
  pl_thread_free(thread);
  pl_heap_free(heap);
  return NULL;
}

static int run(const char* store_dir, size_t thread_count,
               const char* lock_profile_path, const char* trace_path) {
  bool trace = strcmp(trace_path, "-") != 0;
  if (trace && !ax_profile_json_start(trace_path)) {
    fprintf(stderr, "failed to start profile JSON\n");
    return 1;
  }

  pl_store* store = pl_store_new_silo(store_dir, (size_t)64 << 20);
  if (store == NULL) {
    fprintf(stderr, "failed to open Silo store\n");
    (void)ax_profile_json_finish();
    return 1;
  }
  uint8_t root_hash[32];
  if (!pl_store_get_root(store, root_hash)) {
    fprintf(stderr, "Silo store has no root\n");
    pl_store_free(store);
    (void)ax_profile_json_finish();
    return 1;
  }
  bool profile = strcmp(lock_profile_path, "-") != 0;
  if (profile && !pl_store_profile_locks(store, lock_profile_path)) {
    perror("pl_store_profile_locks");
    pl_store_free(store);
    (void)ax_profile_json_finish();
    return 1;
  }

  load_gate gate = {
      .mu = PTHREAD_MUTEX_INITIALIZER,
      .cv = PTHREAD_COND_INITIALIZER,
  };
  load_worker* workers = calloc(thread_count, sizeof(*workers));
  pthread_t* threads = calloc(thread_count, sizeof(*threads));
  if (workers == NULL || threads == NULL) {
    fprintf(stderr, "out of memory\n");
    free(workers);
    free(threads);
    pl_store_free(store);
    (void)ax_profile_json_finish();
    return 1;
  }

  size_t started = 0;
  int status = 0;
  for (size_t i = 0; i < thread_count; i++) {
    workers[i] = (load_worker){
        .store = store,
        .gate = &gate,
        .hash = root_hash,
        .ok = true,
    };
    int rc = pthread_create(&threads[i], NULL, load_worker_main, &workers[i]);
    if (rc != 0) {
      fprintf(stderr, "pthread_create(%zu): %s\n", i, strerror(rc));
      status = 1;
      break;
    }
    started++;
  }

  pthread_mutex_lock(&gate.mu);
  while (gate.ready != started)
    pthread_cond_wait(&gate.cv, &gate.mu);
  uint64_t started_ns = now_ns();
  gate.start = true;
  pthread_cond_broadcast(&gate.cv);
  pthread_mutex_unlock(&gate.mu);

  for (size_t i = 0; i < started; i++) {
    int rc = pthread_join(threads[i], NULL);
    if (rc != 0) {
      fprintf(stderr, "pthread_join(%zu): %s\n", i, strerror(rc));
      status = 1;
    } else if (!workers[i].ok) {
      fprintf(stderr, "load worker %zu failed\n", i);
      status = 1;
    }
  }
  uint64_t duration_ns = now_ns() - started_ns;
  if (started != thread_count)
    status = 1;

  pthread_cond_destroy(&gate.cv);
  pthread_mutex_destroy(&gate.mu);
  free(threads);
  free(workers);
  if (!pl_store_profile_locks_finish(store)) {
    fprintf(stderr, "failed to finalize store lock profile\n");
    status = 1;
  }
  pl_store_free(store);
  if (!ax_profile_json_finish()) {
    fprintf(stderr, "failed to finish profile JSON\n");
    status = 1;
  }
  printf("{\"threads\":%zu,\"profiled\":%s,\"duration_ns\":%" PRIu64 "}\n",
         thread_count, profile ? "true" : "false", duration_ns);
  return status;
}

int main(int argc, char** argv) {
  size_t threads;
  if (argc != 5 || !parse_threads(argv[2], &threads)) {
    fprintf(stderr,
            "usage: %s SILO_DIR THREADS LOCK_PROFILE_JSON TRACE_JSON\n"
            "       use - to disable either profiler\n",
            argv[0]);
    return 2;
  }
  return run(argv[1], threads, argv[3], argv[4]);
}
