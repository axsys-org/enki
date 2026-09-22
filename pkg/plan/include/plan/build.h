#ifndef PL_BUILD_H
#define PL_BUILD_H

/*
 * Value construction.  Every constructor here is bump-only (I2): the
 * caller MUST have reserved headroom with pl_gc_reserve, using the
 * PL_*_CELLS size macros from plan/value.h, before calling.
 */

#include <string.h>

#include "plan/heap.h"
#include "plan/value.h"

/* Direct nat if < 2^63, else a boxed single-limb nat (2 cells). */
pl_val pl_mk_nat_u64(pl_thread* t, uint64_t n);

/* Allocate a boxed nat with `limbs` limbs; caller fills *out, then
 * canonicalizes with pl_nat_trim (which may return a direct nat). */
pl_val pl_mk_nat_limbs(pl_thread* t, size_t limbs, uint64_t** out);
pl_val pl_nat_trim(pl_val v);

/* APP construction. Counts exclude the head and must be positive; callers
 * collapse head-only results to the head. The head must be WHNF (arity is
 * consulted, never forced). */
static inline pl_val pl_mk_app_snoc(pl_thread* t, pl_val f, pl_val x);
/* Flat append of m args to f (as m chained snocs, one allocation).
 * Caller reserves PL_APP_CELLS((f is APP ? pl_app_n(f) : 0) + m). */
pl_val pl_mk_app_cat(pl_thread* t, pl_val f, uint32_t m, const pl_val* args);
pl_val pl_mk_app_take(pl_thread* t, pl_val app, uint32_t n);
static inline pl_val pl_mk_app_from(pl_thread* t, pl_val head, uint32_t n,
                                    const pl_val* args);

pl_val pl_mk_law(pl_thread* t, uint64_t arity, pl_val name, pl_val body);
pl_val pl_mk_env(pl_thread* t, uint32_t nslots); /* slots zeroed to nat 0 */
/* Allocation-only ENV constructor.  The caller must overwrite every slot in
 * a no-collect window before publishing the value or allowing a collection. */
pl_val pl_mk_env_uninit(pl_thread* t, uint32_t nslots);
static inline pl_val pl_mk_thunk(pl_thread* t, pl_val env, pl_val expr);
static inline pl_val pl_mk_thke(pl_thread* t, uint64_t bane, uint32_t nargs,
                                pl_val* args);
/* PL_BAN_PRIM_KNOWN thke: args[0] = the ingest-resolved pl_ops index. */
static inline pl_val pl_mk_thke_known(pl_thread* t, uint32_t idx,
                                      uint32_t nargs, pl_val* args);

/* Arity of a WHNF value (never forces). */
static inline uint64_t pl_arity(pl_val v);

/* ── The only three mutation sites ──────── */

static inline void pl_thke_update(pl_thread* t, pl_val thke, pl_val result);
/* Overwrite a THUNK or BLACKHOLE in place as K_IND -> result. */
static inline void pl_thunk_update(pl_thread* t, pl_val thunk_or_bh,
                                   pl_val result);

/* ── Inline bodies (hot in the evaluator) ──────────────────────────────── */

/* Argument vectors are short: copy the common sizes inline instead of
 * paying a libc memmove call per thunk. */
static inline void pl_copy_vals(pl_cell* dst, const pl_val* src, uint32_t n) {
  switch (n) {
  case 4:
    dst[3] = src[3];
    [[fallthrough]];
  case 3:
    dst[2] = src[2];
    [[fallthrough]];
  case 2:
    dst[1] = src[1];
    [[fallthrough]];
  case 1:
    dst[0] = src[0];
    [[fallthrough]];
  case 0:
    return;
  default:
    memcpy(dst, src, (size_t)n * sizeof(pl_val));
    return;
  }
}

static inline uint64_t pl_arity(pl_val v) {
  if (pl_is_nat63(v))
    return 0;
  switch (pl_tag(v)) {
  case PL_TAG_NAT:
    return 0;
  case PL_TAG_LAW:
    return pl_law_arity(pl_ptr(v));
  case PL_TAG_PIN: {
    pl_val body = pl_pin_body(pl_ptr(v));
    if (!pl_is_nat63(body) && pl_tag(body) == PL_TAG_LAW)
      return pl_law_arity(pl_ptr(body));
    return 1; /* pinned nat: 1; pinned app/pin: 1, error at exec */
  }
  case PL_TAG_APP:
    return pl_app_need(pl_ptr(v));
  default:
    ax_abort("pl_arity on a non-WHNF value (tag 0x%llx)",
             (unsigned long long)pl_tag(v));
  }
}

static inline uint32_t pl_need_after(pl_val head, uint64_t n_args) {
  uint64_t a = pl_arity(head);
  if (a == 0 || a <= n_args)
    return 0;
  uint64_t need = a - n_args;
  ax_assume(need < (1u << 20), "app need exceeds meta width");
  return (uint32_t)need;
}

static inline pl_val pl_mk_app_from(pl_thread* t, pl_val head, uint32_t n,
                                    const pl_val* args) {
  ax_assume(n >= 1, "empty app");
  pl_cell* p = pl_bump(t, PL_APP_CELLS(n));
  pl_cache_stat_alloc(t, PL_K_APP, PL_APP_CELLS(n));
  p[0] = pl_hdr_make(PL_K_APP, 0, pl_need_after(head, n), PL_APP_CELLS(n));
  p[1] = head;
  pl_copy_vals(p + 2, args, n);
  return pl_make(PL_TAG_APP, p);
}

static inline pl_val pl_mk_app_snoc(pl_thread* t, pl_val f, pl_val x) {
  pl_cell* fp = pl_as(PL_TAG_APP, f);
  if (fp != NULL) {
    uint32_t n = pl_app_n(fp);
    uint32_t need = pl_app_need(fp);
    pl_cell* p = pl_bump(t, PL_APP_CELLS(n + 1));
    pl_cache_stat_alloc(t, PL_K_APP, PL_APP_CELLS(n + 1));
    p[0] =
        pl_hdr_make(PL_K_APP, 0, need == 0 ? 0 : need - 1, PL_APP_CELLS(n + 1));
    p[1] = fp[1];
    pl_copy_vals(p + 2, (const pl_val*)(fp + 2), n);
    p[2 + n] = x;
    return pl_make(PL_TAG_APP, p);
  }
  pl_cell* p = pl_bump(t, PL_APP_CELLS(1));
  pl_cache_stat_alloc(t, PL_K_APP, PL_APP_CELLS(1));
  p[0] = pl_hdr_make(PL_K_APP, 0, pl_need_after(f, 1), PL_APP_CELLS(1));
  p[1] = f;
  p[2] = x;
  return pl_make(PL_TAG_APP, p);
}

static inline pl_val pl_mk_thunk(pl_thread* t, pl_val env, pl_val expr) {
  pl_cell* p = pl_bump(t, PL_THUNK_CELLS);
  pl_cache_stat_alloc(t, PL_K_THUNK, PL_THUNK_CELLS);
  p[0] = pl_hdr_make(PL_K_THUNK, 0, 0, PL_THUNK_CELLS);
  p[1] = env;
  p[2] = expr;
  return pl_make(PL_TAG_DEFER, p);
}

static inline pl_val pl_mk_thke(pl_thread* t, uint64_t bane, uint32_t nargs,
                                pl_val* args) {
  uint32_t size = PL_THKE_CELLS(nargs);
  pl_cell* p = pl_bump(t, size);
  pl_cache_stat_alloc(t, PL_K_THKE, size);
  p[0] = pl_hdr_make(PL_K_THKE, 0, 0, size);
  p[1] = bane;
  pl_copy_vals(p + 2, args, nargs);
  return pl_make(PL_TAG_DEFER, p);
}

static inline pl_val pl_mk_thke_known(pl_thread* t, uint32_t idx,
                                      uint32_t nargs, pl_val* args) {
  uint32_t size = PL_THKE_CELLS(nargs + 1);
  pl_cell* p = pl_bump(t, size);
  pl_cache_stat_alloc(t, PL_K_THKE, size);
  p[0] = pl_hdr_make(PL_K_THKE, 0, 0, size);
  p[1] = PL_BAN_PRIM_KNOWN;
  p[2] = idx; /* the ingest-resolved pl_ops index, a nat63 */
  pl_copy_vals(p + 3, args, nargs);
  return pl_make(PL_TAG_DEFER, p);
}

static inline void pl_thunk_update(pl_thread* t, pl_val thunk_or_bh,
                                   pl_val result) {
  (void)t;
  pl_cell* p = pl_ptr(thunk_or_bh);
  pl_kind k = pl_hdr_kind(p[0]);
  ax_assume(k == PL_K_THUNK || k == PL_K_BH, "thunk_update on kind %d", (int)k);
  /* keep the original cell count so the collector copies correctly */
  p[0] = pl_hdr_make(PL_K_IND, 0, 0, pl_hdr_cells(p[0]));
  p[1] = result;
}

static inline void pl_thke_update(pl_thread* t, pl_val thke, pl_val result) {
  (void)t;
  pl_cell* p = pl_ptr(thke);
  pl_kind k = pl_hdr_kind(p[0]);
  ax_assume(k == PL_K_THKE, "thunk_update on kind %d", (int)k);
  /* keep the original cell count so the collector copies correctly */
  p[0] = pl_hdr_make(PL_K_IND, 0, 0, pl_hdr_cells(p[0]));
  p[1] = result;
}

/* Snap a normalized child into pointer field `field` of `parent`. */
void pl_nf_writeback(pl_val parent, uint32_t field, pl_val child);

/* Pointer-field map used by nf and the collector: number of pointer
 * fields of a WHNF object and access to field i (APP: head+args;
 * LAW: name+body; unresolved PIN: body; others: none). */
uint32_t pl_nf_nfields(pl_val v);
pl_val pl_nf_field(pl_val v, uint32_t i);

#endif
