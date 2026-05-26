#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <gmp.h>

#include "enki/gc.h"
#include "enki/value.h"

typedef struct {
    const mp_limb_t* limbs;
    size_t n_limbs_s;
    mp_limb_t small[1];
} enki_nat_view;
static void view_of_nat(enki_value a_v, enki_nat_view* v);
static int enki_nat_cmp_v(enki_nat_view a_nv, enki_nat_view b_nv);
static size_t min_s(size_t a_v, size_t b_v) {
    return a_v < b_v ? a_v : b_v;
}
bool enki_nat_is_zero(enki_value x_v) {
    enki_nat_view a_nv;
    view_of_nat(x_v, &a_nv);
    return (a_nv.n_limbs_s == 0);
}
static void view_of_nat(enki_value a_v, enki_nat_view* v) {
    if(IS_PTR(a_v)) {
        enki_nat* nat = (enki_nat*)ENKI_TO_PTR(a_v);
        v->limbs = nat->limbs;
        v->n_limbs_s = nat->n_limbs_s;
        return;
    }
    if(a_v == 0) {
        v->limbs = v->small;
        v->n_limbs_s = 0;
        return;
    }
    v->small[0] = (mp_limb_t)a_v;
    v->limbs = v->small;
    v->n_limbs_s = 1;
}
int enki_nat_cmp(enki_value a_v, enki_value b_v) {
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    return enki_nat_cmp_v(a_nv, b_nv);
}
static int enki_nat_cmp_v(enki_nat_view a_nv, enki_nat_view b_nv) {
    if(a_nv.n_limbs_s != b_nv.n_limbs_s) {
        return (a_nv.n_limbs_s > b_nv.n_limbs_s) ? 1 : -1;
    }
    for(size_t k_i = a_nv.n_limbs_s; k_i > 0; k_i--) {
        size_t idx_i = k_i - 1;
        if (a_nv.limbs[idx_i] != b_nv.limbs[idx_i]) {
            return (a_nv.limbs[idx_i] > b_nv.limbs[idx_i]) ? 1 : -1;
        }
    }
    return 0;
}
enki_value enki_nat_eq(enki_value a_v, enki_value b_v) {
    return (enki_nat_cmp(a_v, b_v) == 0) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_ne(enki_value a_v, enki_value b_v) {
    return (enki_nat_cmp(a_v, b_v) != 0) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_lt(enki_value a_v, enki_value b_v) {
    return (enki_nat_cmp(a_v, b_v) == -1) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_le(enki_value a_v, enki_value b_v) {
    return (enki_nat_lt(a_v, b_v) || (enki_nat_eq(a_v, b_v))) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_gt(enki_value a_v, enki_value b_v) {
    return (enki_nat_cmp(a_v, b_v) == 1) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_ge(enki_value a_v, enki_value b_v) {
    return (enki_nat_gt(a_v, b_v) || (enki_nat_eq(a_v, b_v))) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_inc(enki_gc* gc, enki_value a_v) {
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)1;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, sizeof(mp_limb_t) * (a_nv.n_limbs_s + 1));
    mp_limb_t carry_q = mpn_add_1(out, a_nv.limbs, a_nv.n_limbs_s, 1);
    out[a_nv.n_limbs_s] = carry_q;
    return enki_alloc_nat(gc, out, a_nv.n_limbs_s + 1);
}
enki_value enki_nat_dec(enki_gc* gc, enki_value a_v) {
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, a_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_sub_1(out, a_nv.limbs, a_nv.n_limbs_s, 1);
    return enki_alloc_nat(gc, out, a_nv.n_limbs_s);
}
enki_value enki_nat_add(enki_gc* gc, enki_value a_v, enki_value b_v) {
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(a_nv.n_limbs_s == 0) return b_v;
    if(b_nv.n_limbs_s == 0) return a_v;
    if(a_nv.n_limbs_s < b_nv.n_limbs_s) {
        enki_nat_view temp = a_nv;
        a_nv = b_nv;
        b_nv = temp;
    }
    size_t n_s = a_nv.n_limbs_s > b_nv.n_limbs_s ? a_nv.n_limbs_s + 1 : b_nv.n_limbs_s + 1;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, sizeof(mp_limb_t) * n_s);
    mp_limb_t carry_q = mpn_add(out, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    out[n_s - 1] = carry_q;
    return enki_alloc_nat(gc, out, n_s);
}
enki_value enki_nat_sub(enki_gc* gc, enki_value a_v, enki_value b_v) {
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0; // no negatives
    if(b_nv.n_limbs_s == 0) return a_v;
    if(enki_nat_cmp_v(a_nv, b_nv) < 0) return (enki_value)0; // a_v < b_v
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, a_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_sub(out, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    return enki_alloc_nat(gc, out, a_nv.n_limbs_s);
}
enki_value enki_nat_mul(enki_gc* gc, enki_value a_v, enki_value b_v) {
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(a_nv.n_limbs_s == 0 || b_nv.n_limbs_s == 0) return (enki_value)0;
    size_t n_s = (a_nv.n_limbs_s + b_nv.n_limbs_s);
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, n_s * sizeof(mp_limb_t));
    if(a_nv.n_limbs_s > b_nv.n_limbs_s) {
        mpn_mul(out, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    }
    else {
        mpn_mul(out, b_nv.limbs, b_nv.n_limbs_s, a_nv.limbs, a_nv.n_limbs_s);
    }
    return enki_alloc_nat(gc, out, n_s);
}
enki_value enki_nat_div(enki_gc* gc, enki_value a_v, enki_value b_v) {
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(b_nv.n_limbs_s == 0) exit(1); // TODO: temporoary until we have throw
    if(enki_nat_cmp_v(a_nv, b_nv) == -1) return (enki_value)0;
    size_t q_len_s = a_nv.n_limbs_s - b_nv.n_limbs_s + 1;
    mp_limb_t* Q = gc->sys_a.alloc(gc->sys_a.ctx, q_len_s * sizeof(mp_limb_t));
    mp_limb_t* R = gc->sys_a.alloc(gc->sys_a.ctx, b_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    gc->sys_a.free(gc->sys_a.ctx, R);
    return enki_alloc_nat(gc, Q, q_len_s);
}
enki_value enki_nat_mod(enki_gc* gc, enki_value a_v, enki_value b_v) {
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(b_nv.n_limbs_s == 0) exit(1); // TODO: temporoary until we have throw
    if(enki_nat_cmp_v(a_nv, b_nv) == -1) return a_v;
    size_t q_len_s = a_nv.n_limbs_s - b_nv.n_limbs_s + 1;
    mp_limb_t* Q = gc->sys_a.alloc(gc->sys_a.ctx, q_len_s * sizeof(mp_limb_t));
    mp_limb_t* R = gc->sys_a.alloc(gc->sys_a.ctx, b_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    gc->sys_a.free(gc->sys_a.ctx, Q);
    return enki_alloc_nat(gc, R, b_nv.n_limbs_s);
}
// lower bits to higher posistions
enki_value enki_nat_lsh(enki_gc* gc, enki_value a_v, enki_value shift_v) {
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(shift_v)) exit(1); // temp
    if(shift_v == 0) return a_v;
    size_t word_shift_s = (size_t)shift_v / 64;
    size_t bit_shift_s = (size_t)shift_v % 64;
    size_t n_s = (a_nv.n_limbs_s + word_shift_s + 1);
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    if(bit_shift_s == 0) {
        memcpy(out + word_shift_s, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
        return enki_alloc_nat(gc, out, n_s - 1);
    }
    mp_limb_t carry_q = mpn_lshift(out + word_shift_s, a_nv.limbs, a_nv.n_limbs_s, (unsigned int)bit_shift_s);
    out[n_s - 1] = carry_q;
    return enki_alloc_nat(gc, out, n_s);
}
// higher order bits to lower positions
enki_value enki_nat_rsh(enki_gc* gc, enki_value a_v, enki_value shift_v) {
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(shift_v)) exit(1); // temp
    if(shift_v == 0) return a_v;
    size_t word_shift_s = (size_t)shift_v / 64;
    size_t bit_shift_s = (size_t)shift_v % 64;
    if(word_shift_s >= a_nv.n_limbs_s) return (enki_value)0;
    const mp_limb_t* src = a_nv.limbs + word_shift_s;
    size_t n_src_s = (a_nv.n_limbs_s - word_shift_s);
    size_t n_s = n_src_s * sizeof(mp_limb_t);
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, n_s);
    memset(out, 0, n_s);
    if(bit_shift_s == 0) {
        memcpy(out, src, n_s);
        return enki_alloc_nat(gc, out, n_src_s);
    }
    mpn_rshift(out, src, n_src_s, (unsigned int)bit_shift_s);
    return enki_alloc_nat(gc, out, n_src_s);
}
enki_value enki_nat_bex(enki_gc* gc, enki_value bit_v) {
    if(IS_PTR(bit_v)) exit(1); // temp
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64;
    size_t n_s = word_off_o + 1;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    out[word_off_o] = ((mp_limb_t)1 << bit_off_o);
    return enki_alloc_nat(gc, out, n_s);
}
enki_value enki_nat_test(enki_gc* gc, enki_value bit_v, enki_value a_v) {
    (void)gc;
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(bit_v)) exit(1); // temp
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64;
    if(word_off_o >= a_nv.n_limbs_s) return (enki_value)0;
    size_t res_v = (a_nv.limbs[word_off_o] & ((mp_limb_t)1 << bit_off_o));
    return res_v != 0 ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_set(enki_gc* gc, enki_value bit_v, enki_value a_v) {
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(bit_v)) exit(1); // temp
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64;
    size_t n_s = word_off_o + 1;
    if(n_s < a_nv.n_limbs_s) n_s = a_nv.n_limbs_s;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
    out[word_off_o] |= ((mp_limb_t)1 << bit_off_o);
    return enki_alloc_nat(gc, out, n_s);
}
enki_value enki_nat_clear(enki_gc* gc, enki_value bit_v, enki_value a_v) {
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(bit_v)) exit(1); // temp
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64;
    size_t n_s = word_off_o + 1;
    if(n_s < a_nv.n_limbs_s) n_s = a_nv.n_limbs_s;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
    out[word_off_o] = out[word_off_o] & ~((mp_limb_t)1 << bit_off_o);
    return enki_alloc_nat(gc, out, n_s);
}
enki_value enki_nat_bits(enki_gc* gc, enki_value a_v) {
    (void)gc;
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0;
    mp_limb_t top_limb_q = a_nv.limbs[a_nv.n_limbs_s - 1];
    size_t top_s = top_limb_q == 0 ? 0 : (64u - (size_t)__builtin_clzl(top_limb_q));
    return (enki_value)(top_s + ((a_nv.n_limbs_s - 1) * 64));
}
enki_value enki_nat_bytes(enki_gc* gc, enki_value a_v) {
    return (enki_value)((enki_nat_bits(gc, a_v) + 7)/8);
}

static size_t enki_cat_met_bytes(enki_value a_v) {
  size_t wid_s =  __builtin_clzll(a_v);
  return (8 - (wid_s /8));
}

size_t enki_bat_met_bytes(enki_value a_v) {
  if (a_v == 0) {
    return 0; }
  if (!IS_PTR(a_v)) { return enki_cat_met_bytes(a_v); }
  if ( ((obj_header*)a_v)->kind_b != ENKI_BIG_NAT ) { return 0; }
  enki_nat* nat = ((enki_nat*)a_v);
  size_t ret_s = (nat->n_limbs_s - 1) * 8;
  ret_s += enki_cat_met_bytes(nat->limbs[nat->n_limbs_s - 1]);
  return ret_s;
}
enki_value enki_nat_trunc(enki_gc* gc, enki_value width_v, enki_value a_v) {
    if(IS_PTR(width_v)) exit(1); // temp
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0;
    size_t word_off_o = (size_t)width_v / 64;
    size_t bit_off_o = (size_t)width_v % 64;
    size_t n_s = bit_off_o != 0 ? word_off_o + 1 : word_off_o;
    size_t keep_s = min_s(a_nv.n_limbs_s, n_s);
    if(keep_s == 0) return (enki_value)0;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, keep_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, keep_s * sizeof(mp_limb_t));
    if(bit_off_o == 0) {
        return enki_alloc_nat(gc, out, keep_s);
    }
    if(keep_s == n_s) {
        out[keep_s - 1] = (out[keep_s - 1] & (((mp_limb_t)1ULL << bit_off_o) - 1));
    }
    return enki_alloc_nat(gc, out, keep_s);
}
enki_value enki_nat_trunc8(enki_gc* gc, enki_value a_v) {
    return enki_nat_trunc(gc, 8, a_v);
}
enki_value enki_nat_trunc16(enki_gc* gc, enki_value a_v) {
    return enki_nat_trunc(gc, 16, a_v);
}
enki_value enki_nat_trunc32(enki_gc* gc, enki_value a_v) {
    return enki_nat_trunc(gc, 32, a_v);
}
enki_value enki_nat_trunc64(enki_gc* gc, enki_value a_v) {
    return enki_nat_trunc(gc, 64, a_v);
}
enki_value enki_nat_load8(enki_gc* gc, enki_value index_i, enki_value a_v) {
    (void)gc;
    if(IS_PTR(index_i)) exit(1); // temp
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    // sizeof(mp_limb_t) = 8 for my machine
    size_t limb_index_i = (size_t)index_i / sizeof(mp_limb_t);
    size_t byte_offset_o = (size_t)index_i % sizeof(mp_limb_t);
    if(a_nv.n_limbs_s <= limb_index_i) return (enki_value)0;
    mp_limb_t limb_q = a_nv.limbs[limb_index_i];
    return (enki_value)((limb_q >> (byte_offset_o * 8)) & ((1 << 8) - 1));
}
enki_value enki_nat_store8(enki_gc* gc, enki_value index_i, enki_value byte_b, enki_value a_v) {
    if(IS_PTR(index_i)) exit(1); // temp
    if(IS_PTR(byte_b)) exit(1);
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    size_t limb_index_i = (size_t)index_i / sizeof(mp_limb_t);
    size_t byte_offset_o = (size_t)index_i % sizeof(mp_limb_t);
    size_t n_s = a_nv.n_limbs_s <= limb_index_i ? limb_index_i + 1 : a_nv.n_limbs_s;
    mp_limb_t* out = gc->sys_a.alloc(gc->sys_a.ctx, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
    mp_limb_t mask_q = (mp_limb_t)~(0xFFULL << (byte_offset_o * 8));
    mp_limb_t byte_bits_q = (mp_limb_t)(byte_b & 0xFF) << (byte_offset_o * 8);
    out[limb_index_i] = (out[limb_index_i] & mask_q) | byte_bits_q;
    return enki_alloc_nat(gc, out, n_s);
}
enki_value enki_nat_nib(enki_gc* gc, enki_value index_i, enki_value a_v) {
    (void)gc;
    if(IS_PTR(index_i)) exit(1); // temp
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    size_t limb_index_i = index_i/16;
    size_t shift_v = (index_i % 16) * 4;
    if (limb_index_i >= a_nv.n_limbs_s) return (enki_value)0;
    return (enki_value)((a_nv.limbs[limb_index_i] >> shift_v) & 0x0F);
}
