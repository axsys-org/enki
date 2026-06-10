#ifndef PL_VALUE_H
#define PL_VALUE_H

/*
 * PLAN value representation.
 *
 * A pl_val is a 64-bit word:
 *   - bit 63 clear: a direct nat; the low 63 bits are the value.
 *   - bit 63 set:   a tagged pointer; bits 62..56 carry a 7-bit tag and
 *                   bits 55..0 the (8-aligned) address.
 *
 * The tag is a kind cache, valid only for kinds that never mutate in
 * place.  THUNK -> BLACKHOLE -> IND transitions happen under aliasing, so
 * all three share PL_TAG_DEFER and dispatch reads the object header for
 * the true kind.  Consequently WHNF is a register test:
 *   pl_is_nat63(v) || pl_tag(v) != PL_TAG_DEFER.
 */

#include <stdint.h>

#include "axsys/assume.h"

typedef uint64_t pl_val;
typedef uint64_t pl_cell;

#define PL_PTR_BIT   (UINT64_C(1) << 63)
#define PL_ADDR_MASK UINT64_C(0x00ffffffffffffff)
#define PL_NAT63_MAX UINT64_C(0x7fffffffffffffff)

/* Tags live in the top byte (bit 63 implied set). */
#define PL_TAG_NAT   UINT64_C(0x81) /* boxed nat (>= 2^63) */
#define PL_TAG_APP   UINT64_C(0x82)
#define PL_TAG_LAW   UINT64_C(0x83)
#define PL_TAG_PIN   UINT64_C(0x84)
#define PL_TAG_ENV   UINT64_C(0x85)
#define PL_TAG_DEFER UINT64_C(0x86) /* THUNK | BLACKHOLE | IND */

/* Object kinds (authoritative, stored in the header). */
typedef enum {
  PL_K_NAT = 1,
  PL_K_APP,
  PL_K_LAW,
  PL_K_PIN,
  PL_K_THUNK,
  PL_K_ENV,
  PL_K_IND,
  PL_K_BH,
  PL_K_FWD, /* forwarding pointer, exists only during collection */
} pl_kind;

/*
 * Header word, one per object:
 *   [ kind:4 | flags:8 | meta:20 | cells:32 ]
 * cells is the total size in 8-byte cells including the header.
 */
#define PL_F_NORMAL 0x1u /* deep normal form reached (§ nf) */

static inline pl_cell pl_hdr_make(pl_kind kind, uint32_t flags, uint32_t meta,
                                  uint32_t cells) {
  return (pl_cell)(kind & 0xFu) | ((pl_cell)(flags & 0xFFu) << 4) |
         ((pl_cell)(meta & 0xFFFFFu) << 12) | ((pl_cell)cells << 32);
}

static inline pl_kind pl_hdr_kind(pl_cell hdr) {
  return (pl_kind)(hdr & 0xFu);
}
static inline uint32_t pl_hdr_flags(pl_cell hdr) {
  return (uint32_t)(hdr >> 4) & 0xFFu;
}
static inline uint32_t pl_hdr_meta(pl_cell hdr) {
  return (uint32_t)(hdr >> 12) & 0xFFFFFu;
}
static inline uint32_t pl_hdr_cells(pl_cell hdr) {
  return (uint32_t)(hdr >> 32);
}

/* ── pl_val accessors ──────────────────────────────────────────────────── */

static inline bool pl_is_nat63(pl_val v) {
  return (v & PL_PTR_BIT) == 0;
}
static inline uint64_t pl_nat63(pl_val v) {
  return v;
}
static inline uint64_t pl_tag(pl_val v) {
  return v >> 56;
}
static inline pl_cell* pl_ptr(pl_val v) {
  /* Pointers MUST be masked before dereference on every target. */
  return (pl_cell*)(uintptr_t)(v & PL_ADDR_MASK);
}
static inline pl_val pl_make(uint64_t tag, void* p) {
  return (tag << 56) | ((uint64_t)(uintptr_t)p & PL_ADDR_MASK);
}

/* WHNF needs no memory access except for PL_TAG_DEFER. */
static inline bool pl_is_whnf(pl_val v) {
  return pl_is_nat63(v) || pl_tag(v) != PL_TAG_DEFER;
}

/* True kind: tag, or header load for PL_TAG_DEFER. */
static inline pl_kind pl_kind_of(pl_val v) {
  ax_assume(!pl_is_nat63(v), "pl_kind_of on a direct nat");
  switch (pl_tag(v)) {
  case PL_TAG_NAT:
    return PL_K_NAT;
  case PL_TAG_APP:
    return PL_K_APP;
  case PL_TAG_LAW:
    return PL_K_LAW;
  case PL_TAG_PIN:
    return PL_K_PIN;
  case PL_TAG_ENV:
    return PL_K_ENV;
  default:
    return pl_hdr_kind(*pl_ptr(v));
  }
}

static inline uint64_t pl_tag_for_kind(pl_kind k) {
  switch (k) {
  case PL_K_NAT:
    return PL_TAG_NAT;
  case PL_K_APP:
    return PL_TAG_APP;
  case PL_K_LAW:
    return PL_TAG_LAW;
  case PL_K_PIN:
    return PL_TAG_PIN;
  case PL_K_ENV:
    return PL_TAG_ENV;
  default:
    return PL_TAG_DEFER;
  }
}

/* ── Layouts (cells include the header word) ───────────────────────────── */

#define PL_NAT_CELLS(limbs) (1u + (uint32_t)(limbs))
#define PL_APP_CELLS(n)     (2u + (uint32_t)(n))
#define PL_LAW_CELLS        4u
#define PL_PIN_CELLS(np)    (6u + (uint32_t)(np))
#define PL_THUNK_CELLS      3u
#define PL_ENV_CELLS(n)     (1u + (uint32_t)(n))
#define PL_IND_CELLS        2u

/* K_NAT { hdr(meta=used limbs); limb[..] } — mpn limbs, little-endian. */
static inline uint32_t pl_nat_limbs(pl_cell* p) {
  return pl_hdr_meta(p[0]);
}
static inline uint64_t* pl_nat_limb_ptr(pl_cell* p) {
  return (uint64_t*)(p + 1);
}

/* K_APP { hdr(meta=need, n=cells-2); head; arg[n] } — immutable, n-ary. */
static inline uint32_t pl_app_n(pl_cell* p) {
  return pl_hdr_cells(p[0]) - 2u;
}
static inline uint32_t pl_app_need(pl_cell* p) {
  return pl_hdr_meta(p[0]);
}
static inline pl_val pl_app_head(pl_cell* p) {
  return (pl_val)p[1];
}
static inline pl_val* pl_app_args(pl_cell* p) {
  return (pl_val*)(p + 2);
}

/* K_LAW { hdr; u64 arity; name; body } */
static inline uint64_t pl_law_arity(pl_cell* p) {
  return p[1];
}
static inline pl_val pl_law_name(pl_cell* p) {
  return (pl_val)p[2];
}
static inline pl_val pl_law_body(pl_cell* p) {
  return (pl_val)p[3];
}

/* K_PIN { hdr(meta=npins); u8 hash[32]; body; pin[npins] } — store only. */
static inline uint8_t* pl_pin_hash_bytes(pl_cell* p) {
  return (uint8_t*)(p + 1);
}
static inline pl_val pl_pin_body(pl_cell* p) {
  return (pl_val)p[5];
}
static inline uint32_t pl_pin_npins(pl_cell* p) {
  return pl_hdr_meta(p[0]);
}
static inline pl_val* pl_pin_subpins(pl_cell* p) {
  return (pl_val*)(p + 6);
}

/* K_THUNK { hdr; env; expr } */
static inline pl_val pl_thunk_env(pl_cell* p) {
  return (pl_val)p[1];
}
static inline pl_val pl_thunk_expr(pl_cell* p) {
  return (pl_val)p[2];
}

/* K_ENV { hdr(n=cells-1); slot[n] } — law activation [self, args…, binds…]. */
static inline uint32_t pl_env_n(pl_cell* p) {
  return pl_hdr_cells(p[0]) - 1u;
}
static inline pl_val* pl_env_slots(pl_cell* p) {
  return (pl_val*)(p + 1);
}

/* K_IND / K_BH { hdr; target } */
static inline pl_val pl_ind_target(pl_cell* p) {
  return (pl_val)p[1];
}

/* ── Convenience predicates (WHNF inputs) ──────────────────────────────── */

static inline bool pl_is_nat(pl_val v) {
  return pl_is_nat63(v) || pl_tag(v) == PL_TAG_NAT;
}

static inline pl_cell* pl_as(uint64_t tag, pl_val v) {
  return (!pl_is_nat63(v) && pl_tag(v) == tag) ? pl_ptr(v) : (pl_cell*)0;
}

#endif
