#ifndef PL_WORMHOLE_H
#define PL_WORMHOLE_H

/* Process-scoped opaque handles represented with blackhole semantics. */

#include <stdbool.h>
#include <stdint.h>

#include "plan/heap.h"
#include "plan/value.h"

/* Adopt transfers one already-owned host reference into the wrapper. */
pl_val pl_wormhole_adopt(pl_thread* t, uint64_t token);
/* Clone retains the token and creates a wrapper in t's heap. */
pl_val pl_wormhole_clone(pl_thread* t, pl_val wormhole);

bool pl_is_wormhole(pl_val v);
bool pl_wormhole_is_closed(pl_val wormhole);
uint64_t pl_wormhole_token(pl_val wormhole);
void pl_wormhole_close(pl_val wormhole);

#endif
