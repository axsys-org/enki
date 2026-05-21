#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gmp.h>

#include "enki/gc.h"
#include "enki/value.h"

typedef struct {
    const mp_limb_t* limbs;
    size_t n_limbs;
    mp_limb_t small[1];
} enki_nat_view;
static void view_of_nat(enki_value a, enki_nat_view* v);
static int enki_nat_cmp_v(enki_nat_view a_v, enki_nat_view b_v);
static size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}
bool enki_nat_is_zero(enki_value x) {
    enki_nat_view a_v;
    view_of_nat(x, &a_v);
    return (a_v.n_limbs == 0);
}
static void view_of_nat(enki_value a, enki_nat_view* v) {
    if(IS_PTR(a)) {
        enki_nat* nat = (enki_nat*)ENKI_TO_PTR(a);
        v->limbs = nat->limbs;
        v->n_limbs = nat->n_limbs;
        return;        
    }
    if(a == 0) {
        v->limbs = v->small;
        v->n_limbs = 0;
        return;
    }
    v->small[0] = (mp_limb_t)a;
    v->limbs = v->small;
    v->n_limbs = 1;
}
int enki_nat_cmp(enki_value a, enki_value b) {
    enki_nat_view a_v;
    enki_nat_view b_v;
    view_of_nat(a, &a_v);
    view_of_nat(b, &b_v);
    return enki_nat_cmp_v(a_v, b_v);
}
static int enki_nat_cmp_v(enki_nat_view a_v, enki_nat_view b_v) {
    if(a_v.n_limbs != b_v.n_limbs) {
        return (a_v.n_limbs > b_v.n_limbs) ? 1 : -1;
    }
    for(size_t k = a_v.n_limbs; k > 0; k--) {
        size_t idx = k - 1;
        if (a_v.limbs[idx] != b_v.limbs[idx]) {
            return (a_v.limbs[idx] > b_v.limbs[idx]) ? 1 : -1;
        }
    }
    return 0;
}
enki_value enki_nat_eq(enki_value a, enki_value b) {
    return (enki_nat_cmp(a, b) == 0) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_ne(enki_value a, enki_value b) {
    return (enki_nat_cmp(a, b) != 0) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_lt(enki_value a, enki_value b) {
    return (enki_nat_cmp(a, b) == -1) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_le(enki_value a, enki_value b) {
    return (enki_nat_lt(a, b) || (enki_nat_eq(a, b))) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_gt(enki_value a, enki_value b) {
    return (enki_nat_cmp(a, b) == 1) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_ge(enki_value a, enki_value b) {
    return (enki_nat_gt(a, b) || (enki_nat_eq(a, b))) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_inc(enki_gc* gc, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(a_v.n_limbs == 0) return (enki_value)1;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, sizeof(mp_limb_t) * (a_v.n_limbs + 1));
    mp_limb_t carry = mpn_add_1(out, a_v.limbs, a_v.n_limbs, 1);
    out[a_v.n_limbs] = carry;
    return enki_alloc_nat(gc, out, a_v.n_limbs + 1);
}
enki_value enki_nat_dec(enki_gc* gc, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(a_v.n_limbs == 0) return (enki_value)0;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, a_v.n_limbs * sizeof(mp_limb_t));
    mpn_sub_1(out, a_v.limbs, a_v.n_limbs, 1);
    return enki_alloc_nat(gc, out, a_v.n_limbs);   
}
enki_value enki_nat_add(enki_gc* gc, enki_value a, enki_value b) {
    enki_nat_view a_v;
    enki_nat_view b_v;
    view_of_nat(a, &a_v);
    view_of_nat(b, &b_v);
    if(a_v.n_limbs == 0) return b;
    if(b_v.n_limbs == 0) return a;
    if(a_v.n_limbs < b_v.n_limbs) {  
        enki_nat_view temp = a_v;
        a_v = b_v;
        b_v = temp;
    }
    size_t n = a_v.n_limbs > b_v.n_limbs ? a_v.n_limbs + 1 : b_v.n_limbs + 1;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, sizeof(mp_limb_t) * n);
    mp_limb_t carry = mpn_add(out, a_v.limbs, a_v.n_limbs, b_v.limbs, b_v.n_limbs);
    out[n - 1] = carry;
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_sub(enki_gc* gc, enki_value a, enki_value b) {
    enki_nat_view a_v;
    enki_nat_view b_v;
    view_of_nat(a, &a_v);
    view_of_nat(b, &b_v);
    if(a_v.n_limbs == 0) return (enki_value)0; // no negatives 
    if(b_v.n_limbs == 0) return a;
    if(enki_nat_cmp_v(a_v, b_v) < 0) return (enki_value)0; // a < b 
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, a_v.n_limbs * sizeof(mp_limb_t));
    mpn_sub(out, a_v.limbs, a_v.n_limbs, b_v.limbs, b_v.n_limbs);
    return enki_alloc_nat(gc, out, a_v.n_limbs);
}
enki_value enki_nat_mul(enki_gc* gc, enki_value a, enki_value b) {
    enki_nat_view a_v;
    enki_nat_view b_v;
    view_of_nat(a, &a_v);
    view_of_nat(b, &b_v);
    if(a_v.n_limbs == 0 || b_v.n_limbs == 0) return (enki_value)0;
    size_t n = (a_v.n_limbs + b_v.n_limbs);
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, n * sizeof(mp_limb_t));
    if(a_v.n_limbs > b_v.n_limbs) {
        mpn_mul(out, a_v.limbs, a_v.n_limbs, b_v.limbs, b_v.n_limbs);
    }
    else {
        mpn_mul(out, b_v.limbs, b_v.n_limbs, a_v.limbs, a_v.n_limbs);
    }
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_div(enki_gc* gc, enki_value a, enki_value b) {
    enki_nat_view a_v;
    enki_nat_view b_v;
    view_of_nat(a, &a_v);
    view_of_nat(b, &b_v);
    if(b_v.n_limbs == 0) exit(1); // TODO: temporoary until we have throw 
    if(enki_nat_cmp_v(a_v, b_v) == -1) return (enki_value)0;
    size_t q_len = a_v.n_limbs - b_v.n_limbs + 1;
    mp_limb_t* Q = gc->sys.alloc(gc->sys.ctx, q_len * sizeof(mp_limb_t));
    mp_limb_t* R = gc->sys.alloc(gc->sys.ctx, b_v.n_limbs * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_v.limbs, a_v.n_limbs, b_v.limbs, b_v.n_limbs);
    gc->sys.free(gc->sys.ctx, R);
    return enki_alloc_nat(gc, Q, q_len);
}
enki_value enki_nat_mod(enki_gc* gc, enki_value a, enki_value b) {
    enki_nat_view a_v;
    enki_nat_view b_v;
    view_of_nat(a, &a_v);
    view_of_nat(b, &b_v);
    if(b_v.n_limbs == 0) exit(1); // TODO: temporoary until we have throw 
    if(enki_nat_cmp_v(a_v, b_v) == -1) return a;
    size_t q_len = a_v.n_limbs - b_v.n_limbs + 1;
    mp_limb_t* Q = gc->sys.alloc(gc->sys.ctx, q_len * sizeof(mp_limb_t));
    mp_limb_t* R = gc->sys.alloc(gc->sys.ctx, b_v.n_limbs * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_v.limbs, a_v.n_limbs, b_v.limbs, b_v.n_limbs);
    gc->sys.free(gc->sys.ctx, Q);
    return enki_alloc_nat(gc, R, b_v.n_limbs);
}
// lower bits to higher posistions 
enki_value enki_nat_lsh(enki_gc* gc, enki_value a, enki_value shift) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(IS_PTR(shift)) exit(1); // temp 
    if(shift == 0) return a;
    size_t word_shift = (size_t)shift / 64;
    size_t bit_shift = (size_t)shift % 64;
    size_t n = (a_v.n_limbs + word_shift + 1);
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, n * sizeof(mp_limb_t));
    memset(out, 0, n * sizeof(mp_limb_t));
    if(bit_shift == 0) {
        memcpy(out + word_shift, a_v.limbs, a_v.n_limbs * sizeof(mp_limb_t));
        return enki_alloc_nat(gc, out, n - 1);
    }
    mp_limb_t carry = mpn_lshift(out + word_shift, a_v.limbs, a_v.n_limbs, (unsigned int)bit_shift);
    out[n - 1] = carry;
    return enki_alloc_nat(gc, out, n);
}
// higher order bits to lower positions 
enki_value enki_nat_rsh(enki_gc* gc, enki_value a, enki_value shift) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(IS_PTR(shift)) exit(1); // temp 
    if(shift == 0) return a;
    size_t word_shift = (size_t)shift / 64;
    size_t bit_shift = (size_t)shift % 64;
    if(word_shift >= a_v.n_limbs) return (enki_value)0;
    const mp_limb_t* src = a_v.limbs + word_shift;
    size_t n_src = (a_v.n_limbs - word_shift);
    size_t n = n_src * sizeof(mp_limb_t);
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, n);
    memset(out, 0, n);
    if(bit_shift == 0) {
        memcpy(out, src, n);
        return enki_alloc_nat(gc, out, n_src);
    }
    mpn_rshift(out, src, n_src, (unsigned int)bit_shift);
    return enki_alloc_nat(gc, out, n_src);
}
enki_value enki_nat_bex(enki_gc* gc, enki_value bit) {
    if(IS_PTR(bit)) exit(1); // temp 
    size_t word_off = (size_t)bit / 64;
    size_t bit_off = (size_t)bit % 64;
    size_t n = word_off + 1;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, n * sizeof(mp_limb_t));
    memset(out, 0, n * sizeof(mp_limb_t));
    out[word_off] = ((mp_limb_t)1 << bit_off);
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_test(enki_gc* gc, enki_value bit, enki_value a) {
    (void)gc;
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(IS_PTR(bit)) exit(1); // temp 
    size_t word_off = (size_t)bit / 64;
    size_t bit_off = (size_t)bit % 64; 
    if(word_off >= a_v.n_limbs) return (enki_value)0;
    size_t res = (a_v.limbs[word_off] & ((mp_limb_t)1 << bit_off)); 
    return res != 0 ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_set(enki_gc* gc, enki_value bit, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(IS_PTR(bit)) exit(1); // temp 
    size_t word_off = (size_t)bit / 64;
    size_t bit_off = (size_t)bit % 64; 
    size_t n = word_off + 1;
    if(n < a_v.n_limbs) n = a_v.n_limbs;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, n * sizeof(mp_limb_t));
    memset(out, 0, n * sizeof(mp_limb_t));
    memcpy(out, a_v.limbs, a_v.n_limbs * sizeof(mp_limb_t));
    out[word_off] = out[word_off] |= ((mp_limb_t)1 << bit_off);
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_clear(enki_gc* gc, enki_value bit, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(IS_PTR(bit)) exit(1); // temp 
    size_t word_off = (size_t)bit / 64;
    size_t bit_off = (size_t)bit % 64; 
    size_t n = word_off + 1;
    if(n < a_v.n_limbs) n = a_v.n_limbs;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, n * sizeof(mp_limb_t));
    memset(out, 0, n * sizeof(mp_limb_t));
    memcpy(out, a_v.limbs, a_v.n_limbs * sizeof(mp_limb_t));
    out[word_off] = out[word_off] & ~((mp_limb_t)1 << bit_off);
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_bits(enki_gc* gc, enki_value a) {
    (void)gc;
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(a_v.n_limbs == 0) return (enki_value)0;
    mp_limb_t top_limb = a_v.limbs[a_v.n_limbs - 1];
    size_t top = top_limb == 0 ? 0 : (64u - (size_t)__builtin_clzl(top_limb));
    return (enki_value)(top + ((a_v.n_limbs - 1) * 64));
}
enki_value enki_nat_bytes(enki_gc* gc, enki_value a) {
    return (enki_value)((enki_nat_bits(gc, a) + 7)/8);
}
enki_value enki_nat_trunc(enki_gc* gc, enki_value width, enki_value a) {
    if(IS_PTR(width)) exit(1); // temp 
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(a_v.n_limbs == 0) return (enki_value)0;
    size_t word_off = (size_t)width / 64;
    size_t bit_off = (size_t)width % 64; 
    size_t n = bit_off != 0 ? word_off + 1 : word_off;
    size_t keep = min(a_v.n_limbs, n);
    if(keep == 0) return (enki_value)0;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, keep * sizeof(mp_limb_t));
    memcpy(out, a_v.limbs, keep * sizeof(mp_limb_t));
    if(bit_off == 0) {
        return enki_alloc_nat(gc, out, keep);
    }
    if(keep == n) {
        out[keep - 1] = (out[keep - 1] & (((mp_limb_t)1ULL << bit_off) - 1));
    }
    return enki_alloc_nat(gc, out, keep);
}
enki_value enki_nat_trunc8(enki_gc* gc, enki_value a) {
    return enki_nat_trunc(gc, 8, a);
}
enki_value enki_nat_trunc16(enki_gc* gc, enki_value a) {
    return enki_nat_trunc(gc, 16, a);
}
enki_value enki_nat_trunc32(enki_gc* gc, enki_value a) {
    return enki_nat_trunc(gc, 32, a);
}
enki_value enki_nat_trunc64(enki_gc* gc, enki_value a) {
    return enki_nat_trunc(gc, 64, a);
}
enki_value enki_nat_load8(enki_gc* gc, enki_value index, enki_value a) {
    (void)gc;
    if(IS_PTR(index)) exit(1); // temp 
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    // sizeof(mp_limb_t) = 8 for my machine 
    size_t limb_index = (size_t)index / sizeof(mp_limb_t);
    size_t byte_offset = (size_t)index % sizeof(mp_limb_t);
    if(a_v.n_limbs <= limb_index) return (enki_value)0;
    mp_limb_t limb = a_v.limbs[limb_index];
    return (enki_value)((limb >> (byte_offset * 8)) & ((1 << 8) - 1));
}
enki_value enki_nat_store8(enki_gc* gc, enki_value index, enki_value byte, enki_value a) {
    if(IS_PTR(index)) exit(1); // temp 
    if(IS_PTR(byte)) exit(1);
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    size_t limb_index = (size_t)index / sizeof(mp_limb_t);
    size_t byte_offset = (size_t)index % sizeof(mp_limb_t);
    size_t n = a_v.n_limbs <= limb_index ? limb_index + 1 : a_v.n_limbs;
    mp_limb_t* out = gc->sys.alloc(gc->sys.ctx, n * sizeof(mp_limb_t));
    memset(out, 0, n * sizeof(mp_limb_t));
    memcpy(out, a_v.limbs, a_v.n_limbs * sizeof(mp_limb_t));
    mp_limb_t mask = (mp_limb_t)~(0xFFULL << (byte_offset * 8)); 
    mp_limb_t byte_bits = (mp_limb_t)(byte & 0xFF) << (byte_offset * 8);
    out[limb_index] = (out[limb_index] & mask) | byte_bits;
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_nib(enki_gc* gc, enki_value index, enki_value a) {
    (void)gc;
    if(IS_PTR(index)) exit(1); // temp 
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    size_t limb_index = index/16;
    size_t shift = (index % 16) * 4;
    if (limb_index >= a_v.n_limbs) return (enki_value)0;
    return (enki_value)((a_v.limbs[limb_index] >> shift) & 0x0F);
}
