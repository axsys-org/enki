#ifndef PL_MESA_H
#define PL_MESA_H
#include "plan/heap.h"
/* Host entry: handles is a row of resolved Mesa wormholes; data is a normal
 * row/nat tree. Returns [status data handles], status 0 success or 1 error.
 * Callers sharing a store must cover the whole call/root publication with a
 * mutator scope. PLAN normally enters through the mode-gated op-83 bindings. */
pl_val pl_mesa_call(pl_thread* t, const char* name, pl_val handles,
                    pl_val data);
bool pl_is_mesa_handle(pl_val v);
#endif
