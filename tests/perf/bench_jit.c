/* Non-allocating local tail loop: isolates native-region execution from
 * compiler installation and C/native crossings. See DEVELOP.md. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "test_plan.h"

static uint64_t bench_now_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
  uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000000;
  if (n == 0 || n > 100000000) {
    fprintf(stderr, "usage: bench_jit [iterations: 1..100000000]\n");
    return 2;
  }
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_vpush(t, test_p66(t));
  pl_val p66 = t->vstack[0];
  pl_val ops[] = {OP_PUSH_VAR,
                  1,
                  OP_FORCE,
                  OP_PUSH_LIT,
                  0,
                  OP_TAILBLK,
                  8,
                  2,
                  OP_PUSH_SLOT,
                  0,
                  OP_PUSH_LIT,
                  0,
                  OP_CALL_KNOWN,
                  2,
                  p66,
                  ax_s3('C', 'm', 'p'),
                  OP_BR,
                  3,
                  21,
                  21,
                  24,
                  OP_PUSH_SLOT,
                  1,
                  OP_RET,
                  OP_PUSH_SLOT,
                  0,
                  OP_PUSH_LIT,
                  1,
                  OP_CALL_KNOWN,
                  2,
                  p66,
                  ax_s3('S', 'u', 'b'),
                  OP_PUSH_SLOT,
                  1,
                  OP_PUSH_SLOT,
                  0,
                  OP_CALL_KNOWN,
                  2,
                  p66,
                  ax_s3('A', 'd', 'd'),
                  OP_TAILBLK,
                  8,
                  2,
                  OP_RET};
  pl_val row = test_app(t, 0, sizeof ops / sizeof ops[0], ops);
  uint64_t start = bench_now_ns();
  pl_code* code = pl_bytecode_from_val(row);
  if (code == NULL)
    return 1;
  uint64_t decode_ns = bench_now_ns() - start;
  pl_vpush(t, test_law(t, 1, 0, 0));
  t->vstack[1] = pl_pin(t, t->vstack[1]);
  char error[192];
  if (!pl_store_save_root(rt.store, t->vstack[1], NULL, error, sizeof(error))) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  pl_pin_set_code(pl_ptr(t->vstack[1]), code);
  printf("decode_ns=%" PRIu64 "\n", decode_ns);
  for (unsigned sample = 0; sample < 6; sample++) {
    pl_vpush(t, t->vstack[1]);
    pl_vpush(t, n);
    pl_gc_reserve(t, PL_THKE_CELLS(2));
    pl_val call = pl_mk_thke(t, PL_BAN_FAST, 2, &t->vstack[2]);
    t->vsp = 2;
    start = bench_now_ns();
    pl_thread_start(t, call);
    pl_run_status status;
    while ((status = pl_thread_run(t, 1000000)) == PL_RUN_YIELDED)
      ;
    uint64_t elapsed = bench_now_ns() - start;
    if (status != PL_RUN_DONE || t->result != n * (n + 1) / 2) {
      fprintf(stderr, "checksum failed: status=%d result=%" PRIu64 "\n",
              (int)status, t->result);
      return 1;
    }
    printf("sample=%u ns=%" PRIu64 " result=%" PRIu64 "\n", sample, elapsed,
           t->result);
  }
  pl_pin_set_code(pl_ptr(t->vstack[1]), NULL);
  pl_bytecode_free(code);
  test_rt_free(&rt);
  return 0;
}
