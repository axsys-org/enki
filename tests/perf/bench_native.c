/* Run create and read as separate processes (the read is a process-cold,
 * filesystem-cache-warm restart). Use a fresh directory for each backend.
 * bench_native native|silo create|read DIRECTORY [PIN_COUNT]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "test_plan.h"
#include "axsys/ds.h"
#include "../../pkg/plan/src/store_internal.h"

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1000000;
}
/* Resident anonymous memory, separate from mapped object pages. Zero means
 * the platform did not expose a measurement. */
static uint64_t anonymous_rss(void) {
#ifdef __APPLE__
  task_vm_info_data_t info = {0};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) ==
      KERN_SUCCESS)
    return (uint64_t)info.internal;
#else
  FILE* f = fopen("/proc/self/status", "r");
  if (f != NULL) {
    char line[256];
    unsigned long long kb;
    while (fgets(line, sizeof(line), f)) {
      if (sscanf(line, "RssAnon: %llu kB", &kb) == 1) {
        fclose(f);
        return (uint64_t)kb * 1024;
      }
    }
    fclose(f);
  }
#endif
  return 0;
}
int main(int argc, char** argv) {
  if (argc < 4 || (strcmp(argv[1], "native") && strcmp(argv[1], "silo")) ||
      (strcmp(argv[2], "create") && strcmp(argv[2], "read"))) {
    fprintf(stderr, "usage: %s native|silo create|read DIRECTORY [PIN_COUNT]\n",
            argv[0]);
    return 2;
  }
  bool native = strcmp(argv[1], "native") == 0;
  bool create = strcmp(argv[2], "create") == 0;
  size_t count = argc > 4 ? (size_t)strtoull(argv[4], NULL, 10) : 10000;
  if (count < 2 || count > 1000000)
    return 2;
  char err[256] = {0};
  double start = now_ms();
  pl_store* s = native ? pl_store_new_native(argv[3], err, sizeof(err))
                       : pl_store_new_silo(argv[3], (size_t)1 << 30);
  if (s == NULL) {
    fprintf(stderr, "%s\n", err);
    return 1;
  }
  double opened = now_ms();
  if (native && (ax_arrlen(s->pins) != 0 || ax_hmlen(s->intern) != 0))
    abort();
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);
  uint8_t hash[32];
  if (create) {
    pl_vpush(t, test_law(t, 1, 7, 1));
    t->vstack[0] = pl_pin(t, t->vstack[0]);
    for (size_t i = 1; i < count; i++)
      pl_vpush(t, pl_pin(t, (pl_val)i));
    pl_gc_reserve(t, PL_APP_CELLS(count));
    pl_vpush(t, pl_mk_app_from(t, 0, (uint32_t)count, t->vstack));
    t->vstack[count] = pl_nf(t, t->vstack[count]);
    pl_vpush(t, pl_pin(t, t->vstack[count]));
    double before = now_ms();
    if (!pl_store_save_root(s, t->vstack[count + 1], hash, err, sizeof(err))) {
      fprintf(stderr, "%s\n", err);
      return 1;
    }
    double saved = now_ms();
    printf("%s pins=%zu open_ms=%.3f save_ms=%.3f\n", argv[1], count,
           opened - start, saved - before);
  } else {
    if (!pl_store_get_root(s, hash))
      return 1;
    double before = now_ms();
    pl_val pin = pl_store_load(t, hash);
    double loaded = now_ms();
    pl_cell* app = pl_ptr(pl_pin_body(pl_ptr(pin)));
    pl_val law = pl_app_args(app)[0];
    if (pl_apply(t, law, 42) != 42)
      abort();
    double first = now_ms();
    uint32_t n = pl_hdr_cells(app[0]) - 2;
    uint64_t sum = 0;
    for (uint32_t i = 1; i < n; i += n / 64 + 1)
      sum += pl_pin_body(pl_ptr(pl_app_args(app)[i]));
    double sparse = now_ms();
    double eval_start = now_ms();
    for (uint64_t i = 0; i < 100000; i++)
      if (pl_apply(t, law, i) != i)
        abort();
    double eval_end = now_ms();
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    uint64_t rss = (uint64_t)usage.ru_maxrss;
#ifndef __APPLE__
    rss *= 1024;
#endif
    printf(
        "%s pins=%u root=0x%llx open_ms=%.3f load_ms=%.3f first_eval_ms=%.3f "
        "sparse_ms=%.3f eval_per_s=%.0f anonymous_object_bytes=%zu "
        "registered_pins=%td anonymous_rss_bytes=%llu max_rss_bytes=%llu "
        "check=%llu\n",
        argv[1], n, (unsigned long long)pl_addr(pin), opened - start,
        loaded - before, first - start, sparse - first,
        100000000.0 / (eval_end - eval_start), pl_store_mark(s),
        ax_arrlen(s->pins), (unsigned long long)anonymous_rss(),
        (unsigned long long)rss, (unsigned long long)sum);
    if (native && ax_arrlen(s->pins) != 1)
      abort();
  }
  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  return 0;
}
