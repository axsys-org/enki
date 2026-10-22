#ifndef PL_INT63_H
#define PL_INT63_H

#include "plan/value.h"

/* Signed 63-bit integers stored as direct nats in zigzag order:
 * 0, -1, 1, -2, 2, ... . Range: -2^62 through 2^62-1.
 * Arithmetic wraps modulo 2^63. Operands must be direct nats.
 * Unsigned intermediates avoid signed overflow and signed shifts. */
#define PL_INT63_MIN (-INT64_C(4611686018427387904))
#define PL_INT63_MAX INT64_C(4611686018427387903)

/* Encode the low 63 two's-complement bits, including wrapped results. */
static inline pl_val pl_int63_from_bits(uint64_t bits) {
  bits &= PL_NAT63_MAX;
  return ((bits << 1) ^ (UINT64_C(0) - (bits >> 62))) & PL_NAT63_MAX;
}

static inline pl_val pl_int63_encode(int64_t value) {
  return pl_int63_from_bits((uint64_t)value);
}

static inline int64_t pl_int63_decode(pl_val value) {
  int64_t magnitude = (int64_t)(value >> 1);
  return (value & 1) ? -magnitude - 1 : magnitude;
}

static inline pl_val pl_int63_add(pl_val a, pl_val b) {
  return pl_int63_from_bits((uint64_t)pl_int63_decode(a) +
                            (uint64_t)pl_int63_decode(b));
}

static inline pl_val pl_int63_sub(pl_val a, pl_val b) {
  return pl_int63_from_bits((uint64_t)pl_int63_decode(a) -
                            (uint64_t)pl_int63_decode(b));
}

static inline pl_val pl_int63_mul(pl_val a, pl_val b) {
  return pl_int63_from_bits((uint64_t)pl_int63_decode(a) *
                            (uint64_t)pl_int63_decode(b));
}

static inline pl_val pl_int63_neg(pl_val a) {
  return pl_int63_encode(-pl_int63_decode(a));
}

static inline pl_val pl_int63_abs(pl_val a) {
  return (a & 1) ? pl_int63_neg(a) : a;
}

/* b must be nonzero. Division truncates toward zero; remainder has the
 * dividend's sign. MIN / -1 fits int64_t, then wraps back to MIN. */
static inline pl_val pl_int63_div(pl_val a, pl_val b) {
  return pl_int63_encode(pl_int63_decode(a) / pl_int63_decode(b));
}

static inline pl_val pl_int63_mod(pl_val a, pl_val b) {
  return pl_int63_encode(pl_int63_decode(a) % pl_int63_decode(b));
}

/* Rounding is a plain nat, not a zigzag integer. */
typedef enum {
  PL_INT63_ROUND_ZERO = 0,
  PL_INT63_ROUND_FLOOR = 1,
  PL_INT63_ROUND_CEIL = 2,
  PL_INT63_ROUND_EVEN = 3,
} pl_int63_round;

/* Exact signed 128-bit product, then divide, round, and finally wrap to
 * 63 bits. Operands must be direct nats, divisor nonzero, and rounding one
 * of the modes above. No allocation or floating-point operations. */
pl_val pl_int63_muldiv(pl_val a, pl_val b, pl_val divisor,
                       pl_int63_round rounding);

#endif
