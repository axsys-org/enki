#ifndef PL_JIT_H
#define PL_JIT_H
#include "plan/heap.h"
#include "batch.h"

#ifdef PL_SLJIT
void pl_jit_prepare(pl_code* code);
void pl_jit_free(pl_code* code);
bool pl_jit_compiled(const pl_code* code);
uint32_t pl_jit_run(pl_thread* t, pl_frame* fr, uint32_t pc);
static inline const pl_op_t* pl_jit_ops(const pl_code* c) {
  return c->jit_ops != NULL ? c->jit_ops : pl_batch_ops(c);
}
#else
static inline void pl_jit_prepare(pl_code* c) {
  (void)c;
}
static inline void pl_jit_free(pl_code* c) {
  (void)c;
}
static inline const pl_op_t* pl_jit_ops(const pl_code* c) {
  return pl_batch_ops(c);
}
#endif
#endif
