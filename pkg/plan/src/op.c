#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axsys/allocator.h"
#include "axsys/assume.h"
#include "axsys/base58.h"
#include "axsys/util.h"
#include "internal.h"
#include "plan/build.h"
#include "plan/canon.h"
#include "plan/debug.h"
#include "plan/nat.h"
#include "plan/store.h"

/*
 * Primops, normative semantics from the Haskell reference (Plan.hs).
 * op 0:  core PLAN ops (pin / law / case).
 * op 66: named extended ops.
 * op 82: rplan I/O (rplan.c) — RPLAN-mode gated, fd/console/file ops;
 *        actor ops raise until the io-work actor port lands.
 */

#define ARG(i) (t->vstack[ab + (i)])

static pl_val pl_resolve(pl_val v) {
  while (!pl_is_nat63(v) && pl_tag(v) == PL_TAG_DEFER &&
         pl_hdr_kind(*pl_ptr(v)) == PL_K_IND)
    v = pl_ind_target(pl_ptr(v));
  return v;
}

/* ── Small helpers ─────────────────────────────────────────────────────── */

static pl_val mk_app1_rooted(pl_thread* t, pl_val head, pl_val* slot) {
  pl_gc_reserve(t, PL_APP_CELLS(1));
  PL_GC_FORBID(t);
  pl_val r = pl_mk_app_from(t, head, 1, slot);
  PL_GC_ALLOW(t);
  return r;
}

/* ── op 0 / shared bodies ──────────────────────────────────────────────── */

static pl_val op_pin(pl_thread* t, size_t ab) {
  return pl_pin(t, &ARG(0));
}

static pl_val op_law(pl_thread* t, size_t ab) {
  uint64_t arity = pl_nat_u64_clamp(pl_nat_coerce(ARG(0)));
  ax_assume(arity < PL_NAT63_MAX, "law arity out of range");
  arity += 1; /* reference: L (nat a + 1) m b */
  pl_gc_reserve(t, PL_LAW_CELLS);
  PL_GC_FORBID(t);
  pl_val r = pl_mk_law(t, arity, ARG(1), ARG(2));
  PL_GC_ALLOW(t);
  return r;
}

/* match p l a z m o — strict only in the scrutinee o. */
static pl_val op_elim(pl_thread* t, size_t ab) {
  pl_val o = ARG(5);
  if (pl_is_nat(o)) {
    if (o == 0)
      return ARG(3);
    pl_val om1 = pl_nat_dec(t, &ARG(5));
    pl_push_apply(t, om1);
    return ARG(4);
  }
  switch (pl_tag(o)) {
  case PL_TAG_PIN:
    pl_push_apply(t, pl_pin_body(pl_ptr(o)));
    return ARG(0);
  case PL_TAG_LAW: {
    pl_cell* lp = pl_ptr(o);
    uint64_t a = pl_law_arity(lp);
    ax_assume(a <= PL_NAT63_MAX, "law arity out of range");
    pl_push_apply(t, pl_law_body(lp));
    pl_push_apply(t, pl_law_name(lp));
    pl_push_apply(t, a);
    return ARG(1);
  }
  case PL_TAG_APP: {
    pl_cell* p = pl_ptr(o);
    uint32_t n = pl_app_n(p);
    pl_val ini, last;
    if (n == 1) {
      ini = pl_app_head(p);
      last = pl_app_args(p)[0];
    } else {
      pl_gc_reserve(t, PL_APP_CELLS(n - 1));
      PL_GC_FORBID(t);
      ini = pl_mk_app_take(t, ARG(5), n - 1);
      PL_GC_ALLOW(t);
      last = pl_app_args(pl_ptr(ARG(5)))[n - 1];
    }
    pl_push_apply(t, last);
    pl_push_apply(t, ini);
    return ARG(2);
  }
  default:
    ax_abort("match: bad scrutinee tag");
  }
}

/* ── Arithmetic / bit ops ──────────────────────────────────────────────── */

#define COERCE(i) (ARG(i) = pl_nat_coerce(ARG(i)))

static pl_val op_inc(pl_thread* t, size_t ab) {
  COERCE(0);
  return pl_nat_inc(t, &ARG(0));
}
static pl_val op_dec(pl_thread* t, size_t ab) {
  COERCE(0);
  return pl_nat_dec(t, &ARG(0));
}
static pl_val op_add(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_add(t, &ARG(0), &ARG(1));
}
static pl_val op_sub(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_sub(t, &ARG(0), &ARG(1));
}
static pl_val op_mul(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_mul(t, &ARG(0), &ARG(1));
}
static pl_val op_div(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_div(t, &ARG(0), &ARG(1));
}
static pl_val op_mod(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_mod(t, &ARG(0), &ARG(1));
}
static pl_val op_rsh(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_rsh(t, &ARG(0), &ARG(1));
}
static pl_val op_lsh(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_lsh(t, &ARG(0), &ARG(1));
}
static pl_val op_bex(pl_thread* t, size_t ab) {
  COERCE(0);
  return pl_nat_bex(t, &ARG(0));
}
static pl_val op_test(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_test_bit(ARG(0), ARG(1)) ? 1 : 0;
}
static pl_val op_set(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_set_bit(t, &ARG(0), &ARG(1));
}
static pl_val op_clear(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_clear_bit(t, &ARG(0), &ARG(1));
}
static pl_val op_nib(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  uint64_t i = pl_nat_u64_clamp(ARG(0));
  uint8_t byte = pl_nat_byte_at(ARG(1), i / 2);
  return (byte >> (4 * (i % 2))) & 0xF;
}
static pl_val op_load8(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_byte_at(ARG(1), pl_nat_u64_clamp(ARG(0)));
}

static pl_val op_loadvar(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  COERCE(2);
  return pl_nat_load_var(t, &ARG(0), &ARG(1), &ARG(2));
}

static pl_val op_store8(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  COERCE(2);
  return pl_nat_store_byte(t, &ARG(0), &ARG(1), &ARG(2));
}
static pl_val op_trunc(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_trunc(t, &ARG(0), &ARG(1));
}
static pl_val op_trunc_k(pl_thread* t, size_t ab, uint64_t w) {
  COERCE(0);
  pl_val width = w;
  return pl_nat_trunc(t, &width, &ARG(0));
}
static pl_val op_trunc8(pl_thread* t, size_t ab) {
  return op_trunc_k(t, ab, 8);
}
static pl_val op_trunc16(pl_thread* t, size_t ab) {
  return op_trunc_k(t, ab, 16);
}
static pl_val op_trunc32(pl_thread* t, size_t ab) {
  return op_trunc_k(t, ab, 32);
}
static pl_val op_trunc64(pl_thread* t, size_t ab) {
  return op_trunc_k(t, ab, 64);
}
static pl_val op_bits(pl_thread* t, size_t ab) {
  COERCE(0);
  AX_UNUSED(t);
  return pl_nat_bit_len(ARG(0));
}
static pl_val op_bytes(pl_thread* t, size_t ab) {
  COERCE(0);
  AX_UNUSED(t);
  return pl_nat_byte_len(ARG(0));
}

/* ── Comparisons ───────────────────────────────────────────────────────── */

static int nat_cmp_args(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  return pl_nat_cmp(ARG(0), ARG(1));
}
static pl_val op_eq(pl_thread* t, size_t ab) {
  return nat_cmp_args(t, ab) == 0 ? 1 : 0;
}
static pl_val op_ne(pl_thread* t, size_t ab) {
  return nat_cmp_args(t, ab) != 0 ? 1 : 0;
}
static pl_val op_lt(pl_thread* t, size_t ab) {
  return nat_cmp_args(t, ab) < 0 ? 1 : 0;
}
static pl_val op_le(pl_thread* t, size_t ab) {
  return nat_cmp_args(t, ab) <= 0 ? 1 : 0;
}
static pl_val op_gt(pl_thread* t, size_t ab) {
  return nat_cmp_args(t, ab) > 0 ? 1 : 0;
}
static pl_val op_ge(pl_thread* t, size_t ab) {
  return nat_cmp_args(t, ab) >= 0 ? 1 : 0;
}
static pl_val op_cmp(pl_thread* t, size_t ab) {
  int c = nat_cmp_args(t, ab);
  return c < 0 ? 0 : (c == 0 ? 1 : 2);
}

/* ── Booleans / branches (laziness boundaries) ─────────────────────────── */

static pl_val op_nil(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(0) == 0 ? 1 : 0;
}
static pl_val op_truth(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(0) == 0 ? 0 : 1;
}
static pl_val op_or(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(0) == 0 ? ARG(1) : ARG(0);
}
static pl_val op_and(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(0) == 0 ? 0 : ARG(1);
}
static pl_val op_nor(pl_thread* t, size_t ab) {
  if (ARG(0) != 0)
    return 0;
  pl_val y = pl_whnf(t, ARG(1)); /* conditional strictness (planNil y) */
  return y == 0 ? 1 : 0;
}
static pl_val op_if(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(0) != 0 ? ARG(1) : ARG(2);
}
static pl_val op_ifz(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(0) == 0 ? ARG(1) : ARG(2);
}

/* CaseK x b0 .. b(K-2) fb — strict ONLY in the scrutinee. */
static pl_val op_case_k(pl_thread* t, size_t ab, uint32_t argc) {
  AX_UNUSED(t);
  pl_val x = ARG(0);
  if (pl_is_nat63(x) && x < argc - 2)
    return ARG(1 + x);
  return ARG(argc - 1);
}
#define DEF_CASE(K, ARGC)                                                      \
  static pl_val op_case##K(pl_thread* t, size_t ab) {                          \
    return op_case_k(t, ab, ARGC);                                             \
  }
DEF_CASE(2, 3)
DEF_CASE(3, 4)
DEF_CASE(4, 5)
DEF_CASE(5, 6)
DEF_CASE(6, 7)
DEF_CASE(7, 8)
DEF_CASE(8, 9)
DEF_CASE(9, 10)
DEF_CASE(10, 11)
DEF_CASE(11, 12)
DEF_CASE(12, 13)
DEF_CASE(13, 14)
DEF_CASE(14, 15)
DEF_CASE(15, 16)
DEF_CASE(16, 17)

/* Case ix cs f */
static pl_val op_case(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* cp = pl_as(PL_TAG_APP, ARG(1));
  if (cp != NULL && pl_is_nat63(ARG(0)) && ARG(0) < pl_app_n(cp))
    return pl_app_args(cp)[ARG(0)];
  return ARG(2);
}

/* ── Inspection ────────────────────────────────────────────────────────── */

static pl_val op_type(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_val x = ARG(0);
  if (pl_is_nat(x))
    return 0;
  switch (pl_tag(x)) {
  case PL_TAG_PIN:
    return 1;
  case PL_TAG_LAW:
    return 2;
  case PL_TAG_APP:
    return 3;
  default:
    return 0;
  }
}
static pl_val op_is_pin(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return pl_as(PL_TAG_PIN, ARG(0)) != NULL ? 1 : 0;
}
static pl_val op_is_law(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return pl_as(PL_TAG_LAW, ARG(0)) != NULL ? 1 : 0;
}
static pl_val op_is_app(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return pl_as(PL_TAG_APP, ARG(0)) != NULL ? 1 : 0;
}
static pl_val op_is_nat(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return pl_is_nat(ARG(0)) ? 1 : 0;
}
static pl_val op_nat(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return pl_nat_coerce(ARG(0));
}
static pl_val op_arity(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* lp = pl_as(PL_TAG_LAW, ARG(0));
  return lp != NULL ? pl_law_arity(lp) : 0;
}
static pl_val op_name(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* lp = pl_as(PL_TAG_LAW, ARG(0));
  return lp != NULL ? pl_law_name(lp) : 0;
}
static pl_val op_body(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* lp = pl_as(PL_TAG_LAW, ARG(0));
  return lp != NULL ? pl_law_body(lp) : 0;
}
static pl_val op_unpin(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* pp = pl_as(PL_TAG_PIN, ARG(0));
  return pp != NULL ? pl_pin_body(pp) : 0;
}

/* ── Rows ──────────────────────────────────────────────────────────────── */

static pl_val op_sz(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* p = pl_as(PL_TAG_APP, ARG(0));
  return p != NULL ? pl_app_n(p) : 0;
}
static pl_val op_hd(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* p = pl_as(PL_TAG_APP, ARG(0));
  return p != NULL ? pl_app_head(p) : ARG(0);
}
static pl_val op_last(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  pl_cell* p = pl_as(PL_TAG_APP, ARG(0));
  return p != NULL ? pl_app_args(p)[pl_app_n(p) - 1] : 0;
}
static pl_val op_init(pl_thread* t, size_t ab) {
  pl_cell* p = pl_as(PL_TAG_APP, ARG(0));
  if (p == NULL)
    return 0;
  uint32_t n = pl_app_n(p);
  if (n == 1)
    return pl_app_head(p);
  pl_gc_reserve(t, PL_APP_CELLS(n - 1));
  PL_GC_FORBID(t);
  pl_val r = pl_mk_app_take(t, ARG(0), n - 1);
  PL_GC_ALLOW(t);
  return r;
}
static pl_val op_ix(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  COERCE(0);
  pl_cell* p = pl_as(PL_TAG_APP, ARG(1));
  uint64_t i = pl_nat_u64_clamp(ARG(0));
  if (p != NULL && i < pl_app_n(p))
    return pl_app_args(p)[i];
  return 0;
}
static pl_val op_ix_k(pl_thread* t, size_t ab, uint64_t i) {
  AX_UNUSED(t);
  pl_cell* p = pl_as(PL_TAG_APP, ARG(0));
  if (p == NULL)
    return 0;
  if (i == 0)
    return pl_app_args(p)[0];
  return i < pl_app_n(p) ? pl_app_args(p)[i] : 0;
}
#define DEF_IX(K)                                                              \
  static pl_val op_ix##K(pl_thread* t, size_t ab) {                            \
    return op_ix_k(t, ab, K);                                                  \
  }
DEF_IX(0)
DEF_IX(1)
DEF_IX(2)
DEF_IX(3)
DEF_IX(4)
DEF_IX(5)
DEF_IX(6)
DEF_IX(7)

/* planUp: functional update of slot i; v stays lazy. */
static pl_val op_up(pl_thread* t, size_t ab) {
  COERCE(0);
  pl_cell* p = pl_as(PL_TAG_APP, ARG(2));
  uint64_t i = pl_nat_u64_clamp(ARG(0));
  if (p == NULL || i >= pl_app_n(p))
    return ARG(2);
  uint32_t n = pl_app_n(p);
  pl_gc_reserve(t, PL_APP_CELLS(n));
  PL_GC_FORBID(t);
  pl_cell* sp = pl_ptr(ARG(2));
  pl_val r = pl_mk_app_from(t, pl_app_head(sp), n, pl_app_args(sp));
  pl_app_args(pl_ptr(r))[i] = ARG(1);
  PL_GC_ALLOW(t);
  return r;
}

/* planSlice o n v — note the result head is N 0. */
static pl_val op_slice(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  pl_cell* p = pl_as(PL_TAG_APP, ARG(2));
  if (p == NULL)
    return 0;
  uint64_t o = pl_nat_u64_clamp(ARG(0));
  uint64_t n = pl_nat_u64_clamp(ARG(1));
  uint64_t sz = pl_app_n(p);
  if (o > sz)
    return 0;
  uint64_t rsz = sz - o < n ? sz - o : n;
  if (rsz == 0)
    return 0;
  pl_gc_reserve(t, PL_APP_CELLS(rsz));
  PL_GC_FORBID(t);
  pl_val r =
      pl_mk_app_from(t, 0, (uint32_t)rsz, pl_app_args(pl_ptr(ARG(2))) + o);
  PL_GC_ALLOW(t);
  return r;
}

/* planWeld x y — A (N 0) (toRow x <> toRow y); may produce a 0-ary app. */
static pl_val op_weld(pl_thread* t, size_t ab) {
  pl_cell* xp = pl_as(PL_TAG_APP, ARG(0));
  pl_cell* yp = pl_as(PL_TAG_APP, ARG(1));
  uint32_t nx = xp ? pl_app_n(xp) : 0;
  uint32_t ny = yp ? pl_app_n(yp) : 0;
  pl_gc_reserve(t, PL_APP_CELLS(nx + ny));
  PL_GC_FORBID(t);
  pl_cell* p = pl_bump(t, PL_APP_CELLS(nx + ny));
  p[0] = pl_hdr_make(PL_K_APP, 0, 0, PL_APP_CELLS(nx + ny));
  p[1] = 0;
  xp = pl_as(PL_TAG_APP, ARG(0));
  yp = pl_as(PL_TAG_APP, ARG(1));
  if (nx)
    memcpy(p + 2, pl_app_args(xp), nx * sizeof(pl_val));
  if (ny)
    memcpy(p + 2 + nx, pl_app_args(yp), ny * sizeof(pl_val));
  PL_GC_ALLOW(t);
  return pl_make(PL_TAG_APP, p);
}

/* planRep hd item sz — item replicated unforced. */
static pl_val op_rep(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(2);
  uint64_t n = pl_nat_u64_clamp(ARG(2));
  if (n == 0)
    return ARG(0);
  ax_assume(n < (1u << 24), "Rep size too large");
  pl_gc_reserve(t, PL_APP_CELLS(n));
  PL_GC_FORBID(t);
  pl_cell* p = pl_bump(t, PL_APP_CELLS(n));
  p[0] = pl_hdr_make(PL_K_APP, 0, 0, PL_APP_CELLS(n));
  p[1] = ARG(0);
  for (uint64_t i = 0; i < n; i++)
    p[2 + i] = ARG(1);
  PL_GC_ALLOW(t);
  return pl_make(PL_TAG_APP, p);
}

/*
 * planRow hd sz xs — elements stay LAZY: element k is the unforced
 * `Ix0 (Ix1^k xs)`.  Encoded with the store-resident ix0/ix1 law-body
 * expressions over tiny shared envs (see pl_store_ix?_expr).
 */
static pl_val op_row(pl_thread* t, size_t ab) {
  COERCE(0);
  COERCE(1);
  uint64_t n = pl_nat_u64_clamp(ARG(1));
  if (n == 0)
    return ARG(0);
  ax_assume(n < (1u << 22), "Row size too large");
  pl_store* s = pl_heap_store(t->heap);
  ax_assume(s != NULL, "Row requires a store");
  size_t per = PL_ENV_CELLS(2) + 2 * PL_THUNK_CELLS;
  pl_gc_reserve(t, n * per + PL_APP_CELLS(n));
  pl_val ix0e = pl_store_ix0_expr(s);
  pl_val ix1e = pl_store_ix1_expr(s);
  PL_GC_FORBID(t);
  pl_cell* p = pl_bump(t, PL_APP_CELLS(n));
  p[0] = pl_hdr_make(PL_K_APP, 0, 0, PL_APP_CELLS(n));
  p[1] = ARG(0);
  pl_val prefix = ARG(2);
  for (uint64_t k = 0; k < n; k++) {
    pl_val env = pl_mk_env(t, 2);
    pl_env_slots(pl_ptr(env))[1] = prefix;
    p[2 + k] = pl_mk_thunk(t, env, ix0e);
    if (k + 1 < n)
      prefix = pl_mk_thunk(t, env, ix1e);
  }
  PL_GC_ALLOW(t);
  return pl_make(PL_TAG_APP, p);
}

/* planCoup hd x */
static pl_val op_coup(pl_thread* t, size_t ab) {
  pl_cell* xp = pl_as(PL_TAG_APP, ARG(1));
  if (xp == NULL)
    return ARG(0);
  uint32_t n = pl_app_n(xp);
  if (pl_arity(ARG(0)) > n) {
    pl_gc_reserve(t, PL_APP_CELLS(n));
    PL_GC_FORBID(t);
    xp = pl_ptr(ARG(1));
    pl_val r = pl_mk_app_from(t, ARG(0), n, pl_app_args(xp));
    PL_GC_ALLOW(t);
    return r;
  }
  /* apple (hd : args): fold the applications through the machine */
  for (uint32_t i = n; i > 0; i--)
    pl_push_apply(t, pl_app_args(xp)[i - 1]);
  return ARG(0);
}

/* ── Forcing / sequencing ──────────────────────────────────────────────── */

static pl_val op_seq(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(1);
}
static pl_val op_seq2(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(2);
}
static pl_val op_seq3(pl_thread* t, size_t ab) {
  AX_UNUSED(t);
  return ARG(3);
}
static pl_val op_sap(pl_thread* t, size_t ab) {
  pl_push_apply(t, ARG(1));
  return ARG(0);
}
static pl_val op_sap2(pl_thread* t, size_t ab) {
  pl_push_apply(t, ARG(2));
  pl_push_apply(t, ARG(1));
  return ARG(0);
}
static pl_val op_force(pl_thread* t, size_t ab) {
  /* pl_push_nf(t); handled in flag */
  return ARG(0);
}
static pl_val op_deepseq(pl_thread* t, size_t ab) {
  pl_push_seq(t, ARG(1));
  return ARG(1);
}

/* ── Exceptions ────────────────────────────────────────────────────────── */

static pl_val op_throw(pl_thread* t, size_t ab) {
  pl_raise(t, ARG(0)); /* arg 0 already deeply forced (deep flag) */
}

static pl_val op_try(pl_thread* t, size_t ab) {
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    pl_val r = pl_apply(t, ARG(0), ARG(1));
    r = pl_nf(t, r);
    pl_catch_pop(t, &c);
    size_t ri = t->vsp;
    pl_vpush(t, r);
    pl_val out = mk_app1_rooted(t, 0, &t->vstack[ri]);
    t->vsp = ri;
    return out;
  }
  pl_catch_unwind(t, &c);
  if (t->exn_msg != NULL)
    pl_raise_msg(t, t->exn_msg); /* only PLAN_EXN is catchable */
  pl_val out = mk_app1_rooted(t, 1, &t->exn);
  t->exn = 0;
  return out;
}

/* ── Misc ──────────────────────────────────────────────────────────────── */

static pl_val op_trace(pl_thread* t, size_t ab) {
  // ARG(0) = pl_nf(t, ARG(0)); /* deep flag */
  char* s = pl_show_val(ax_allocator_system(), ARG(0), NULL);
  fprintf(stderr, "%s\n", s);
  ax_free(ax_allocator_system(), s);
  return ARG(1);
}

static bool pl_eq_deep(pl_val a, pl_val b) {
  a = pl_resolve(a);
  b = pl_resolve(b);
  if (a == b)
    return true;
  if (pl_is_nat(a) && pl_is_nat(b))
    return pl_nat_eq(a, b);
  if (pl_is_nat63(a) || pl_is_nat63(b))
    return false;
  if (pl_tag(a) != pl_tag(b))
    return false;
  switch (pl_tag(a)) {
  case PL_TAG_PIN:
    return false; /* interned: equal pins are pointer-equal */
  case PL_TAG_LAW: {
    pl_cell *pa = pl_ptr(a), *pb = pl_ptr(b);
    return pl_law_arity(pa) == pl_law_arity(pb) &&
           pl_eq_deep(pl_law_name(pa), pl_law_name(pb)) &&
           pl_eq_deep(pl_law_body(pa), pl_law_body(pb));
  }
  case PL_TAG_APP: {
    pl_cell *pa = pl_ptr(a), *pb = pl_ptr(b);
    uint32_t n = pl_app_n(pa);
    if (n != pl_app_n(pb))
      return false;
    if (!pl_eq_deep(pl_app_head(pa), pl_app_head(pb)))
      return false;
    for (uint32_t i = 0; i < n; i++)
      if (!pl_eq_deep(pl_app_args(pa)[i], pl_app_args(pb)[i]))
        return false;
    return true;
  }
  default:
    return false;
  }
}

static pl_val op_equal(pl_thread* t, size_t ab) {
  // ARG(1) = pl_nf(t, ARG(1)); /* arg 0 deep via flag */
  return pl_eq_deep(ARG(0), ARG(1)) ? 1 : 0;
}

/*
 * savePinOnly: write snap/<base58>.plan for the pin and (depth-first)
 * its sub-pins, skipping files that already exist; the file content is
 * the canonical text whose SHA-256 is the pin hash, so any PLAN
 * assembler can resume from the snapshot directory.
 */
static void save_pin_only(pl_thread* t, pl_val pin) {
  pl_cell* p = pl_ptr(pin);
  char b58[AX_BASE58_CAP(32)];
  ax_base58(pl_pin_hash(pin), 32, b58);
  char path[AX_BASE58_CAP(32) + 16];
  (void)snprintf(path, sizeof(path), "snap/%s.plan", b58);
  if (access(path, F_OK) == 0)
    return;

  uint32_t np = pl_pin_npins(p);
  for (uint32_t i = 0; i < np; i++)
    save_pin_only(t, pl_pin_subpins(p)[i]);

  size_t n;
  char* text = pl_canonize(ax_allocator_system(), pin, &n);
  FILE* f = fopen(path, "wb");
  if (f == NULL) {
    ax_free(ax_allocator_system(), text);
    pl_raise_msg(t, "Save: cannot write snapshot file");
  }
  size_t wrote = fwrite(text, 1, n, f);
  ax_free(ax_allocator_system(), text);
  if (fclose(f) != 0 || wrote != n)
    pl_raise_msg(t, "Save: short write");
}

static pl_val op_save(pl_thread* t, size_t ab) {
  pl_cell* pp = pl_as(PL_TAG_PIN, ARG(0));
  if (pp == NULL)
    pl_raise_msg(t, "Save: expected a pin");
  (void)mkdir("./snap", 0777); /* EEXIST is fine */
  save_pin_only(t, ARG(0));

  char b58[AX_BASE58_CAP(32)];
  ax_base58(pl_pin_hash(ARG(0)), 32, b58);
  FILE* f = fopen("snap/root.plan", "a");
  if (f == NULL)
    pl_raise_msg(t, "Save: cannot append snap/root.plan");
  fprintf(f, "@%s\n", b58);
  if (fclose(f) != 0)
    pl_raise_msg(t, "Save: short write");
  return 0;
}

static pl_val op_load(pl_thread* t, size_t ab) {
  AX_UNUSED(ab);
  pl_raise_msg(t, "load ./snap/root.plan"); /* loadSnapshot, verbatim */
}

/* ── The table ─────────────────────────────────────────────────────────── */

#define M2(a, b) ax_s2(a, b)
#define OP66(name, argc, mask, deep, body)                                     \
  {66, name, NULL, argc, mask, deep, body}
#define OP82(name, argc, mask, body) {82, 0, name, argc, mask, false, body}

const pl_opdesc pl_ops[] = {
    /* op 0: core PLAN */
    {0, 0, NULL, 1, 0b1, true, op_pin},
    {0, 1, NULL, 3, 0b111, false, op_law},
    {0, 2, NULL, 6, 0b100000, false, op_elim},

    OP66(ax_s3('P', 'i', 'n'), 1, 0b1, true, op_pin),
    OP66(ax_s3('L', 'a', 'w'), 3, 0b111, false, op_law),
    OP66(ax_s4('E', 'l', 'i', 'm'), 6, 0b100000, false, op_elim),

    OP66(ax_s3('I', 'n', 'c'), 1, 0b1, false, op_inc),
    OP66(ax_s3('D', 'e', 'c'), 1, 0b1, false, op_dec),
    OP66(ax_s3('A', 'd', 'd'), 2, 0b11, false, op_add),
    OP66(ax_s3('S', 'u', 'b'), 2, 0b11, false, op_sub),
    OP66(ax_s3('M', 'u', 'l'), 2, 0b11, false, op_mul),
    OP66(ax_s3('D', 'i', 'v'), 2, 0b11, false, op_div),
    OP66(ax_s3('M', 'o', 'd'), 2, 0b11, false, op_mod),
    OP66(ax_s3('R', 's', 'h'), 2, 0b11, false, op_rsh),
    OP66(ax_s3('L', 's', 'h'), 2, 0b11, false, op_lsh),

    OP66(ax_s5('C', 'a', 's', 'e', '2'), 3, 0b1, false, op_case2),
    OP66(ax_s5('C', 'a', 's', 'e', '3'), 4, 0b1, false, op_case3),
    OP66(ax_s5('C', 'a', 's', 'e', '4'), 5, 0b1, false, op_case4),
    OP66(ax_s5('C', 'a', 's', 'e', '5'), 6, 0b1, false, op_case5),
    OP66(ax_s5('C', 'a', 's', 'e', '6'), 7, 0b1, false, op_case6),
    OP66(ax_s5('C', 'a', 's', 'e', '7'), 8, 0b1, false, op_case7),
    OP66(ax_s5('C', 'a', 's', 'e', '8'), 9, 0b1, false, op_case8),
    OP66(ax_s5('C', 'a', 's', 'e', '9'), 10, 0b1, false, op_case9),
    OP66(ax_s6('C', 'a', 's', 'e', '1', '0'), 11, 0b1, false, op_case10),
    OP66(ax_s6('C', 'a', 's', 'e', '1', '1'), 12, 0b1, false, op_case11),
    OP66(ax_s6('C', 'a', 's', 'e', '1', '2'), 13, 0b1, false, op_case12),
    OP66(ax_s6('C', 'a', 's', 'e', '1', '3'), 14, 0b1, false, op_case13),
    OP66(ax_s6('C', 'a', 's', 'e', '1', '4'), 15, 0b1, false, op_case14),
    OP66(ax_s6('C', 'a', 's', 'e', '1', '5'), 16, 0b1, false, op_case15),
    OP66(ax_s6('C', 'a', 's', 'e', '1', '6'), 17, 0b1, false, op_case16),
    OP66(ax_s4('C', 'a', 's', 'e'), 3, 0b11, false, op_case),

    OP66(ax_s4('T', 'e', 's', 't'), 2, 0b11, false, op_test),
    OP66(ax_s3('N', 'i', 'b'), 2, 0b11, false, op_nib),
    OP66(ax_s5('L', 'o', 'a', 'd', '8'), 2, 0b11, false, op_load8),
    OP66(ax_s7('L', 'o', 'a', 'd', 'V', 'a', 'r'), 3, 0b111, false, op_loadvar),
    OP66(ax_s6('S', 't', 'o', 'r', 'e', '8'), 3, 0b111, false, op_store8),
    OP66(ax_s3('S', 'e', 't'), 2, 0b11, false, op_set),
    OP66(ax_s5('C', 'l', 'e', 'a', 'r'), 2, 0b11, false, op_clear),
    OP66(ax_s3('B', 'e', 'x'), 1, 0b1, false, op_bex),
    OP66(ax_s6('T', 'r', 'u', 'n', 'c', '8'), 1, 0b1, false, op_trunc8),
    OP66(ax_s7('T', 'r', 'u', 'n', 'c', '1', '6'), 1, 0b1, false, op_trunc16),
    OP66(ax_s7('T', 'r', 'u', 'n', 'c', '3', '2'), 1, 0b1, false, op_trunc32),
    OP66(ax_s7('T', 'r', 'u', 'n', 'c', '6', '4'), 1, 0b1, false, op_trunc64),
    OP66(ax_s5('T', 'r', 'u', 'n', 'c'), 2, 0b11, false, op_trunc),
    OP66(ax_s4('B', 'i', 't', 's'), 1, 0b1, false, op_bits),
    OP66(ax_s5('B', 'y', 't', 'e', 's'), 1, 0b1, false, op_bytes),

    OP66(ax_s5('U', 'n', 'p', 'i', 'n'), 1, 0b1, false, op_unpin),
    OP66(ax_s3('S', 'e', 'q'), 2, 0b01, false, op_seq),
    OP66(ax_s4('S', 'e', 'q', '2'), 3, 0b011, false, op_seq2),
    OP66(ax_s4('S', 'e', 'q', '3'), 4, 0b0111, false, op_seq3),
    OP66(ax_s3('S', 'a', 'p'), 2, 0b10, false, op_sap),
    OP66(ax_s4('S', 'a', 'p', '2'), 3, 0b110, false, op_sap2),
    OP66(ax_s4('T', 'y', 'p', 'e'), 1, 0b1, false, op_type),
    OP66(ax_s5('I', 's', 'P', 'i', 'n'), 1, 0b1, false, op_is_pin),
    OP66(ax_s5('I', 's', 'L', 'a', 'w'), 1, 0b1, false, op_is_law),
    OP66(ax_s5('I', 's', 'A', 'p', 'p'), 1, 0b1, false, op_is_app),
    OP66(ax_s5('I', 's', 'N', 'a', 't'), 1, 0b1, false, op_is_nat),
    OP66(ax_s3('N', 'a', 't'), 1, 0b1, false, op_nat),
    OP66(ax_s5('A', 'r', 'i', 't', 'y'), 1, 0b1, false, op_arity),
    OP66(ax_s4('N', 'a', 'm', 'e'), 1, 0b1, false, op_name),
    OP66(ax_s4('B', 'o', 'd', 'y'), 1, 0b1, false, op_body),

    OP66(ax_s3('R', 'o', 'w'), 3, 0b011, false, op_row),
    OP66(ax_s3('R', 'e', 'p'), 3, 0b101, false, op_rep),
    OP66(ax_s5('S', 'l', 'i', 'c', 'e'), 3, 0b111, false, op_slice),
    OP66(ax_s4('W', 'e', 'l', 'd'), 2, 0b11, false, op_weld),
    OP66(ax_s5('F', 'o', 'r', 'c', 'e'), 1, 0b1, true, op_force),
    OP66(ax_s7('D', 'e', 'e', 'p', 'S', 'e', 'q'), 2, 0b01, true, op_deepseq),
    OP66(ax_s2('U', 'p'), 3, 0b101, false, op_up),
    OP66(ax_s6('U', 'p', 'U', 'n', 'i', 'q'), 3, 0b101, false, op_up),
    OP66(ax_s4('C', 'o', 'u', 'p'), 2, 0b11, false, op_coup),
    OP66(ax_s3('T', 'r', 'y'), 2, 0, false, op_try),
    OP66(ax_s5('T', 'h', 'r', 'o', 'w'), 1, 0b1, true, op_throw),
    OP66(ax_s2('H', 'd'), 1, 0b1, false, op_hd),
    OP66(ax_s2('I', 'x'), 2, 0b11, false, op_ix),
    OP66(ax_s3('I', 'x', '0'), 1, 0b1, false, op_ix0),
    OP66(ax_s3('I', 'x', '1'), 1, 0b1, false, op_ix1),
    OP66(ax_s3('I', 'x', '2'), 1, 0b1, false, op_ix2),
    OP66(ax_s3('I', 'x', '3'), 1, 0b1, false, op_ix3),
    OP66(ax_s3('I', 'x', '4'), 1, 0b1, false, op_ix4),
    OP66(ax_s3('I', 'x', '5'), 1, 0b1, false, op_ix5),
    OP66(ax_s3('I', 'x', '6'), 1, 0b1, false, op_ix6),
    OP66(ax_s3('I', 'x', '7'), 1, 0b1, false, op_ix7),
    OP66(ax_s4('S', 'a', 'v', 'e'), 1, 0b1, false, op_save),
    OP66(ax_s4('L', 'o', 'a', 'd'), 1, 0b1, false, op_load),
    OP66(ax_s5('T', 'r', 'a', 'c', 'e'), 2, 0b01, true, op_trace),
    OP66(ax_s3('N', 'i', 'l'), 1, 0b1, false, op_nil),
    OP66(ax_s5('T', 'r', 'u', 't', 'h'), 1, 0b1, false, op_truth),
    OP66(ax_s2('O', 'r'), 2, 0b01, false, op_or),
    OP66(ax_s3('N', 'o', 'r'), 2, 0b01, false, op_nor),
    OP66(ax_s3('A', 'n', 'd'), 2, 0b01, false, op_and),
    OP66(ax_s2('I', 'f'), 3, 0b001, false, op_if),
    OP66(ax_s3('I', 'f', 'z'), 3, 0b001, false, op_ifz),
    OP66(ax_s2('E', 'q'), 2, 0b11, false, op_eq),
    OP66(ax_s2('N', 'e'), 2, 0b11, false, op_ne),
    OP66(ax_s2('L', 't'), 2, 0b11, false, op_lt),
    OP66(ax_s2('L', 'e'), 2, 0b11, false, op_le),
    OP66(ax_s2('G', 't'), 2, 0b11, false, op_gt),
    OP66(ax_s2('G', 'e'), 2, 0b11, false, op_ge),
    OP66(ax_s3('C', 'm', 'p'), 2, 0b11, false, op_cmp),
    OP66(ax_s2('S', 'z'), 1, 0b1, false, op_sz),
    OP66(ax_s4('L', 'a', 's', 't'), 1, 0b1, false, op_last),
    OP66(ax_s4('I', 'n', 'i', 't'), 1, 0b1, false, op_init),
    OP66(ax_s5('E', 'q', 'u', 'a', 'l'), 2, 0b11, true, op_equal),

    /* op 82: rplan I/O (mode-gated in eval.c) */
    OP82("Input", 1, 0b1, pl_op82_input),
    OP82("Output", 1, 0b1, pl_op82_output),
    OP82("Warn", 1, 0b1, pl_op82_warn),
    OP82("ReadFile", 1, 0b1, pl_op82_read_file),
    OP82("Print", 1, 0b1, pl_op82_print),
    OP82("Stamp", 1, 0b1, pl_op82_stamp),
    OP82("Now", 1, 0, pl_op82_now),
    OP82("CloseFd", 1, 0b1, pl_op82_closefd),
    OP82("Listen", 1, 0b1, pl_op82_listen),
    OP82("Accept", 1, 0b1, pl_op82_accept),
    OP82("Read", 2, 0b11, pl_op82_read),
    OP82("Write", 2, 0b11, pl_op82_write),
    OP82("Spawn", 1, 0, pl_op82_actor),
    OP82("Send", 2, 0b1, pl_op82_actor),
    OP82("SendCaps", 3, 0b1, pl_op82_actor),
    OP82("Recv", 1, 0b1, pl_op82_actor),
    OP82("CloseHandle", 1, 0b1, pl_op82_actor),
};

const size_t pl_nops = sizeof(pl_ops) / sizeof(pl_ops[0]);

static bool nat_name_eq(pl_val v, const char* s) {
  if (!pl_is_nat(v))
    return false;
  size_t n = strlen(s);
  if (pl_nat_byte_len(v) != n)
    return false;
  for (size_t i = 0; i < n; i++) {
    if (pl_nat_byte_at(v, i) != (uint8_t)s[i])
      return false;
  }
  return true;
}

int pl_op_lookup(uint64_t opset, pl_val name, uint32_t argc) {
  for (size_t i = 0; i < pl_nops; i++) {
    const pl_opdesc* d = &pl_ops[i];
    if (d->opset != opset || d->argc != argc)
      continue;
    if (d->name_c != NULL ? nat_name_eq(name, d->name_c)
                          : (pl_is_nat63(name) && d->name == name))
      return (int)i;
  }
  return -1;
}
