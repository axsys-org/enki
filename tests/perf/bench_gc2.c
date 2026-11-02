/* Shared-heap allocation and collection microbenchmark. For comparison with
 * the old arena, copy this source into that checkout and compile with
 * -DBENCH_GC2_BASELINE. Each invocation uses a fresh store; startup, teardown,
 * and checksum traversal are excluded from the reported timings. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "test_plan.h"
#include "../../pkg/plan/src/store_internal.h"

static uint64_t now_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
  size_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000000;
  unsigned live = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 10;
  unsigned rounds = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1;
  if (!n || n > 10000000 || live > 100 || !rounds || rounds > 100)
    return 2;
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, 0);
#ifndef BENCH_GC2_BASELINE
  pl_store_gc_set_threshold(rt.store, 0);
#endif
  for (unsigned round = 0; round < rounds; round++) {
    rt.t->vstack[0] = 0;
    size_t expected = 0;
    uint64_t start = now_ns();
    for (size_t i = 0; i < n; i++) {
      pl_cell* p = pl_store_alloc(rt.store, PL_APP_CELLS(1));
      p[0] = pl_hdr_make(PL_K_APP, PL_F_NORMAL, 0, PL_APP_CELLS(1));
      p[1] = 0;
      p[2] = rt.t->vstack[0];
      if (i % 100 < live) {
        rt.t->vstack[0] = pl_make(PL_TAG_APP, p);
        expected++;
      }
    }
    uint64_t alloc_ns = now_ns() - start;
    uint64_t mark_ns = 0, cycle_ns = 0;
    size_t retained, spans = 0;
#ifndef BENCH_GC2_BASELINE
    start = now_ns();
    pl_store_gc_collect(rt.store);
    mark_ns = now_ns() - start;
    pl_store_gc_wait(rt.store);
    cycle_ns = now_ns() - start;
    pl_gc2_stats stats = pl_store_gc_stats(rt.store);
    retained = stats.allocated_bytes;
    spans = stats.span_bytes;
#else
    retained = pl_store_mark(rt.store);
#endif
    size_t actual = 0;
    for (pl_val v = rt.t->vstack[0]; v; v = pl_ptr(v)[2])
      actual++;
    if (actual != expected)
      return 1;
    printf("{\"round\":%u,\"objects\":%zu,\"live_percent\":%u,"
           "\"alloc_ns\":%" PRIu64 ",\"mark_ns\":%" PRIu64
           ",\"cycle_ns\":%" PRIu64 ",\"retained_bytes\":%zu,"
           "\"span_bytes\":%zu}\n",
           round, n, live, alloc_ns, mark_ns, cycle_ns, retained, spans);
  }
  test_rt_free(&rt);
  return 0;
}
