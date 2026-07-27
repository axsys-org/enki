#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "axsys/util.h"
#include "plan/store.h"
#include "test_plan.h"

typedef struct start_gate {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  size_t ready;
  bool start;
} start_gate;

typedef struct snapshot_worker {
  pl_store* store;
  start_gate* gate;
  size_t iterations;
  size_t depth;
  bool ok;
} snapshot_worker;

static uint64_t now_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    perror("clock_gettime");
    exit(1);
  }
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static bool parse_size(const char* text, size_t* out) {
  if (text[0] == '\0' || text[0] == '-')
    return false;
  errno = 0;
  char* end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' ||
      value > (unsigned long long)SIZE_MAX)
    return false;
  *out = (size_t)value;
  return true;
}

static pl_val build_payload(pl_thread* t, size_t depth) {
  pl_val payload = 0;
  for (size_t i = 0; i < depth; i++)
    payload = test_app2(t, ax_s4('m', 's', 'g', 'x'), payload, (pl_val)i);
  return payload;
}

static void* snapshot_worker_main(void* arg) {
  snapshot_worker* worker = arg;
  pl_heap* heap = pl_heap_new(1 << 16, worker->store);
  pl_thread* t = pl_thread_new(heap);
  size_t root = t->vsp;
  pl_vpush(t, build_payload(t, worker->depth));
  t->vstack[root] = pl_nf(t, t->vstack[root]);
  pl_store_profile_locks_prepare_thread(worker->store);

  pthread_mutex_lock(&worker->gate->mu);
  worker->gate->ready++;
  pthread_cond_broadcast(&worker->gate->cv);
  while (!worker->gate->start)
    pthread_cond_wait(&worker->gate->cv, &worker->gate->mu);
  pthread_mutex_unlock(&worker->gate->mu);

  for (size_t i = 0; i < worker->iterations; i++) {
    pl_val copy = pl_store_snapshot_message(t, t->vstack[root]);
    if (!pl_store_owns(worker->store, copy)) {
      worker->ok = false;
      break;
    }
  }
  t->vsp = root;
  pl_thread_free(t);
  pl_heap_free(heap);
  return NULL;
}

static int run(size_t thread_count, size_t iterations, size_t depth,
               const char* profile_path) {
  pl_store* store = pl_store_new_mem();
  if (strcmp(profile_path, "-") != 0 &&
      !pl_store_profile_locks(store, profile_path)) {
    perror("pl_store_profile_locks");
    pl_store_free(store);
    return 1;
  }
  start_gate gate = {
      .mu = PTHREAD_MUTEX_INITIALIZER,
      .cv = PTHREAD_COND_INITIALIZER,
  };
  snapshot_worker* workers = calloc(thread_count, sizeof(*workers));
  pthread_t* threads = calloc(thread_count, sizeof(*threads));
  if (workers == NULL || threads == NULL) {
    fprintf(stderr, "out of memory\n");
    free(workers);
    free(threads);
    pl_store_free(store);
    return 1;
  }

  size_t started = 0;
  int status = 0;
  for (size_t i = 0; i < thread_count; i++) {
    workers[i] = (snapshot_worker){
        .store = store,
        .gate = &gate,
        .iterations = iterations,
        .depth = depth,
        .ok = true,
    };
    int rc =
        pthread_create(&threads[i], NULL, snapshot_worker_main, &workers[i]);
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
      fprintf(stderr, "snapshot worker %zu failed\n", i);
      status = 1;
    }
  }
  if (started != thread_count)
    status = 1;

  pthread_cond_destroy(&gate.cv);
  pthread_mutex_destroy(&gate.mu);
  free(threads);
  free(workers);
  uint64_t duration_ns = now_ns() - started_ns;
  if (!pl_store_profile_locks_finish(store)) {
    fprintf(stderr, "failed to finalize store lock profile\n");
    status = 1;
  }
  pl_store_free(store);
  printf("{\"threads\":%zu,\"iterations\":%zu,\"payload_depth\":%zu,"
         "\"profiled\":%s,\"duration_ns\":%" PRIu64 "}\n",
         thread_count, iterations, depth,
         strcmp(profile_path, "-") == 0 ? "false" : "true", duration_ns);
  return status;
}

int main(int argc, char** argv) {
  size_t threads;
  size_t iterations;
  size_t depth;
  if (argc != 5 || !parse_size(argv[1], &threads) ||
      !parse_size(argv[2], &iterations) || !parse_size(argv[3], &depth) ||
      threads == 0 || iterations == 0 || depth == 0) {
    fprintf(stderr,
            "usage: %s THREADS ITERATIONS PAYLOAD_DEPTH PROFILE_JSON\n"
            "       use - as PROFILE_JSON to disable profiling\n",
            argv[0]);
    return 2;
  }
  return run(threads, iterations, depth, argv[4]);
}
