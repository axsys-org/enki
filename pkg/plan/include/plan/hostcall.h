#ifndef PL_HOSTCALL_H
#define PL_HOSTCALL_H

/*
 * op 83: admitted host-call bindings.
 *
 * Host packages contribute inert thunks into one process-global binding
 * table; PLAN programs invoke them with the op-82 spine, `(pin 83)`
 * applied to `(name arg…)`.  The table is the single extensible host
 * boundary: registration refuses duplicates, dispatch is gated on
 * pl_thread.hostcall_f, and bodies return witness/refusal rows as
 * ordinary values — host-level failure is data the program can route,
 * never a raise.  (Raises remain for malformed invocations only.)
 *
 * Result convention (rows are inert head-0 APPs):
 *
 *   witness: (0 "witness" binding payload…)
 *   refusal: (0 "refusal" binding kind detail)
 *
 * where binding/detail are string nats and kind is a refusal-kind nat.
 */

#include "plan/heap.h"
#include "plan/value.h"

/*
 * Body convention matches pl_opdesc: args live at t->vstack[ab + i],
 * pre-forced per the registered strict mask.  name_c must outlive the
 * registration (use a string literal).
 */
typedef pl_val (*pl_hostcall_body)(pl_thread* t, size_t ab);

/* False: table full or duplicate (name, argc) — duplicate-binding
 * refusal; the first registration stays authoritative. */
bool pl_hostcall_register(const char* name_c, uint8_t argc,
                          uint32_t strict_mask, pl_hostcall_body body);

size_t pl_hostcall_count(void);

/*
 * Result builders for thunk bodies.  For a witness, the caller pushes
 * the payload values on the value stack starting at payload_base; both
 * builders restore vsp and return the finished row.
 */
pl_val pl_hostcall_witness(pl_thread* t, const char* binding_c,
                           size_t payload_base);
pl_val pl_hostcall_refusal(pl_thread* t, const char* binding_c, uint64_t kind,
                           const char* detail_c);

#endif
