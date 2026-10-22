#ifndef PL_BATCH_H
#define PL_BATCH_H
#include "plan/heap.h"

/* No references to moving values: literals are read from canonical ops. */
typedef struct {
  uint32_t op, arg, aux;
  uint64_t flags;
} pl_batch_step;
typedef struct pl_batch {
  uint32_t start, end, cells, extra, depth, vars, nsteps;
  pl_batch_step* steps;
} pl_batch;

void pl_batch_prepare(pl_code* c, const uint8_t* targets);
void pl_batch_free(pl_code* c);
const pl_batch* pl_batch_at(const pl_code* c, uint32_t pc);
/* False leaves the machine untouched; execute the original instruction. */
bool pl_batch_reserve(pl_thread* t, pl_frame* fr, const pl_batch* b);
size_t pl_bytecode_width(const pl_op_t* o);
static inline const pl_op_t* pl_batch_ops(const pl_code* c) {
  return c->batch_ops != NULL ? c->batch_ops : c->ops;
}
#endif
