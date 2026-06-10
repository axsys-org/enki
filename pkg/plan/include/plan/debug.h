#ifndef PL_DEBUG_H
#define PL_DEBUG_H

/*
 * Value printer.  Pure read: never forces, never allocates on the plan
 * heap.  Unevaluated thunks print as <thk>, blackholes as <bh>.
 */

#include <stddef.h>

#include "axsys/allocator.h"
#include "axsys/sb.h"
#include "plan/value.h"

/* Append a rendering of v to sb. */
void pl_show_sb(ax_sb* sb, pl_val v);

/* Convenience: allocate and return a NUL-terminated rendering. */
char* pl_show(const ax_allocator* a, pl_val v, size_t* out_s);

#endif
