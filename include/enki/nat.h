#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <gmp.h>

#include "enki/gc.h"
#include "enki/value.h"

enki_value enki_nat_alloc(enki_gc* gc, mp_limb_t* out, size_t n_limbs_s);
enki_value enki_nat_alloc_big(enki_gc* gc, size_t n_limbs_s, mp_limb_t limbs[]);
bool enki_nat_is_zero(enki_value x_v);
int enki_nat_cmp(enki_value a_v, enki_value b_v);
enki_value enki_nat_eq(enki_value a_v, enki_value b_v);
enki_value enki_nat_ne(enki_value a_v, enki_value b_v);
enki_value enki_nat_lt(enki_value a_v, enki_value b_v);
enki_value enki_nat_le(enki_value a_v, enki_value b_v);
enki_value enki_nat_gt(enki_value a_v, enki_value b_v);
enki_value enki_nat_ge(enki_value a_v, enki_value b_v);
enki_value enki_nat_inc(enki_gc* gc, enki_value a_v);
enki_value enki_nat_dec(enki_gc* gc, enki_value a_v);
enki_value enki_nat_add(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_sub(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_mul(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_div(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_mod(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_lsh(enki_gc* gc, enki_value a_v, enki_value shift_v);
enki_value enki_nat_rsh(enki_gc* gc, enki_value a_v, enki_value shift_v);
enki_value enki_nat_bex(enki_gc* gc, enki_value bit_v);
enki_value enki_nat_test(enki_gc* gc, enki_value bit_v, enki_value a_v);
enki_value enki_nat_set(enki_gc* gc, enki_value bit_v, enki_value a_v);
enki_value enki_nat_clear(enki_gc* gc, enki_value bit_v, enki_value a_v);
enki_value enki_nat_bits(enki_gc* gc, enki_value a_v);
enki_value enki_nat_bytes(enki_gc* gc, enki_value a_v);
enki_value enki_nat_trunc(enki_gc* gc, enki_value width_v, enki_value a_v);
enki_value enki_nat_trunc8(enki_gc* gc, enki_value a_v);
enki_value enki_nat_trunc16(enki_gc* gc, enki_value a_v);
enki_value enki_nat_trunc32(enki_gc* gc, enki_value a_v);
enki_value enki_nat_trunc64(enki_gc* gc, enki_value a_v);
enki_value enki_nat_load8(enki_gc* gc, enki_value index_i, enki_value a_v);
enki_value enki_nat_store8(enki_gc* gc, enki_value index_i, enki_value byte_b, enki_value a_v);
enki_value enki_nat_nib(enki_gc* gc, enki_value index_i, enki_value a_v);
