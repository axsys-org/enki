#ifndef PL_EVAL_H
#define PL_EVAL_H

/*
 * The reduction machine: an explicit-frame EVAL/RETURN loop over the
 * thread's frame stack.  No unbounded C recursion, no C-stack scanning.
 * The naive JUDGE/KAL path implemented here is the executable reference
 * semantics; enki may install an enter hook for jets / compiled code.
 */

#include "plan/heap.h"
#include "plan/value.h"

/* Force to weak head normal form. */
pl_val pl_whnf(pl_thread* t, pl_val v);

/* The reference `%`: forces f (to decide arity); x stays lazy.  A
 * saturated application is executed to WHNF. */
pl_val pl_apply(pl_thread* t, pl_val f, pl_val x);

/* Deep normalization (the reference rnf). */
pl_val pl_nf(pl_thread* t, pl_val v);

/* ── Errors ────────────────────────────────────────────────────────────── */

/* Raise PLAN_EXN carrying a PLAN value (catchable by Try). */
[[noreturn]] void pl_raise(pl_thread* t, pl_val v);
/* Raise a runtime error with a static message (NOT catchable by Try). */
[[noreturn]] void pl_raise_msg(pl_thread* t, const char* msg);

/*
 * Handler protocol.  Usage:
 *
 *   pl_catch c;
 *   pl_catch_init(t, &c);
 *   if (setjmp(c.jb) == 0) {
 *     ... protected region ...
 *     pl_catch_pop(t, &c);
 *   } else {
 *     pl_catch_unwind(t, &c);   // resets stacks to the saved watermarks
 *     ... t->exn / t->exn_msg ...
 *   }
 */
typedef struct pl_catch {
  jmp_buf jb;
  jmp_buf* prev;
  size_t vsp, fsp;
} pl_catch;

void pl_catch_init(pl_thread* t, pl_catch* c);
void pl_catch_pop(pl_thread* t, pl_catch* c);
void pl_catch_unwind(pl_thread* t, pl_catch* c);

/* ── enki hook seam (§11.1): jets / compiled code ──────────────────────── */

/*
 * Consulted by ENTER before the naive JUDGE/KAL path runs a law.  The
 * saturated call is parked on the value stack: vstack[hbase] is the head
 * (law or pinned law) and vstack[hbase+1 .. hbase+argc] are the
 * arguments.  A hook that handles the call writes the (possibly lazy)
 * result to *out and returns true; the machine then continues by
 * evaluating *out.  NULL hook (the default) means the naive path.
 */
typedef bool (*pl_enter_hook)(pl_thread* t, size_t hbase, uint32_t argc,
                              pl_val* out);
void pl_set_enter_hook(pl_enter_hook hook);

#endif
