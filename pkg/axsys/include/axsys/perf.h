#ifndef AX_PERF_H
#define AX_PERF_H

#define ax_likely(x)   __builtin_expect(!!(x), 1)
#define ax_unlikely(x) __builtin_expect(!!(x), 0)

/* For small hot helpers that must not be left as calls out of a huge
 * caller (the evaluator): the inliner's budget, not the helper's size,
 * otherwise decides. */
#define ax_always_inline __attribute__((always_inline))
/* Out-of-line, never-returning failure paths keep the inlined fast path
 * small enough to stay inlined. */
#define ax_cold          __attribute__((cold, noinline))

#endif
