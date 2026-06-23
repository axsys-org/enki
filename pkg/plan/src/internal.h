#ifndef PL_INTERNAL_H
#define PL_INTERNAL_H

/* Private plan-layer declarations shared between eval.c, op.c, pin.c. */

#include "plan/build.h"
#include "plan/eval.h"
#include "plan/heap.h"
#include "plan/value.h"

/*
 * Primop descriptor.  Strictness is data: strict_mask bit i forces
 * arg i to WHNF (low to high) before the body runs; deep_mask bit i
 * additionally deep-normalizes arg i — through the machine at depth 0,
 * so effects inside the normalization suspend and block correctly
 * (coordination ops use this to force payloads before the request
 * parks).  Bodies address their args as value-stack slots
 * t->vstack[ab + i]: slots survive reserves (the collector rewrites them
 * in place) and remain valid across nested machine runs, which may grow
 * the stack.  Bodies may push F_APPLY/F_SEQ/F_NF frames and return a
 * value that the machine continues to evaluate.
 *
 * coord marks a coordination effect: the body only validates the forced
 * args and returns the request spine [name, args…]; the machine parks
 * it in t->blocked_on and suspends with PL_RUN_BLOCKED instead of
 * evaluating the body's result.  The name slot sits at vstack[ab - 1].
 */
typedef struct pl_opdesc {
  uint64_t opset;     /* 0, 66 or 82 */
  uint64_t name;      /* op0: subop number; op66: name mote */
  const char* name_c; /* non-NULL: match the name nat against this string
                         (op-82 names can exceed the 7-byte mote width) */
  uint8_t argc;
  uint32_t strict_mask;
  uint32_t deep_mask;
  bool coord;
  pl_val (*body)(pl_thread* t, size_t ab);
} pl_opdesc;

extern const pl_opdesc pl_ops[];
extern const size_t pl_nops;

/* Returns descriptor index, or -1 if there is no matching primop. */
int pl_op_lookup(uint64_t opset, pl_val name, uint32_t argc);

/* op 82 (rplan) bodies, in rplan.c; arg conventions as pl_opdesc. */
pl_val pl_op82_input(pl_thread* t, size_t ab);
pl_val pl_op82_output(pl_thread* t, size_t ab);
pl_val pl_op82_warn(pl_thread* t, size_t ab);
pl_val pl_op82_read_file(pl_thread* t, size_t ab);
pl_val pl_op82_write_file(pl_thread* t, size_t ab);
pl_val pl_op82_print(pl_thread* t, size_t ab);
pl_val pl_op82_stamp(pl_thread* t, size_t ab);
pl_val pl_op82_now(pl_thread* t, size_t ab);
pl_val pl_op82_closefd(pl_thread* t, size_t ab);
pl_val pl_op82_listen(pl_thread* t, size_t ab);
pl_val pl_op82_accept(pl_thread* t, size_t ab);
pl_val pl_op82_read(pl_thread* t, size_t ab);
pl_val pl_op82_write(pl_thread* t, size_t ab);

/* op 82 coordination effects: validate + build the request spine. */
pl_val pl_op82_spawn(pl_thread* t, size_t ab);
pl_val pl_op82_send(pl_thread* t, size_t ab);
pl_val pl_op82_send_caps(pl_thread* t, size_t ab);
pl_val pl_op82_recv(pl_thread* t, size_t ab);
pl_val pl_op82_close_handle(pl_thread* t, size_t ab);

/* Frame-push helpers usable from op bodies. */
static inline void pl_push_apply(pl_thread* t, pl_val x) {
  pl_frame* f = pl_fpush(t);
  f->kind = PL_F_APPLY;
  f->b = x;
}

static inline void pl_push_seq(pl_thread* t, pl_val y) {
  pl_frame* f = pl_fpush(t);
  f->kind = PL_F_SEQ;
  f->b = y;
}

static inline void pl_push_nf(pl_thread* t) {
  pl_frame* f = pl_fpush(t);
  f->kind = PL_F_NF;
}

/* Dynamic raise: message formatted into a per-thread static buffer. */
[[noreturn]] void pl_raise_msgf(pl_thread* t, const char* fmt, ...);

/* Is v deeply normal already? (terminal kinds or NORMAL flag) */
static inline bool pl_is_normal(pl_val v) {
  if (pl_is_nat63(v))
    return true;
  switch (pl_tag(v)) {
  case PL_TAG_NAT:
  case PL_TAG_PIN:
    return true;
  case PL_TAG_APP:
  case PL_TAG_LAW:
    return (pl_hdr_flags(*pl_ptr(v)) & PL_F_NORMAL) != 0;
  default:
    return false;
  }
}

#endif
