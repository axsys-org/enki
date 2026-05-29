#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gmp.h>

#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/nat.h"
#include "enki/profile.h"
#include "enki/util.h"
#include "enki/value.h"
#define ENKI_SMALL_MAX ((enki_value)((UINT64_C(1) << 63) - 1))

typedef struct {
    const mp_limb_t* limbs;
    size_t n_limbs_s;
    mp_limb_t small[1];
} enki_nat_view;

static mp_limb_t* enki_nat_tmp_alloc(enki_gc* gc, size_t n_bytes_s) {
    if(gc != NULL && gc->root != NULL) {
        gc->root->stats.nat_tmp_alloc_s++;
        gc->root->stats.nat_tmp_bytes_s += n_bytes_s;
    }
    mp_limb_t* out = enki_arena_alloc(gc->root->scratch_a, n_bytes_s);
    if(!out) enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
    return out;
}
uint8_t* enki_nat_to_bytes(enki_interpreter* i, enki_value a, size_t* len) {
    *len = (size_t)enki_nat_bytes(i->gc, a);
    uint8_t* res = enki_arena_alloc(i->scratch_a, *len);
    if(!res) enki_interp_throw(i, ENKI_ERROR_OOM, a);
    for(size_t k = 0; k < *len; k++) {
        res[k] = (uint8_t)enki_nat_load8(i->gc, k, a);
    }
    return res;
}
static void view_of_nat(enki_value a_v, enki_nat_view* v);
static int enki_nat_cmp_v(const enki_nat_view* a_nv, const enki_nat_view* b_nv);
static size_t min_s(size_t a_v, size_t b_v) {
    return a_v < b_v ? a_v : b_v;
}
enki_value enki_nat_alloc_big(enki_gc* gc, size_t n_limbs_s, mp_limb_t limbs[]) {
    size_t n = sizeof(enki_nat) + (n_limbs_s * sizeof(mp_limb_t));
    if(gc != NULL && gc->root != NULL) {
        gc->root->stats.nat_heap_alloc_s++;
        gc->root->stats.nat_heap_bytes_s += n;
        gc->root->stats.nat_big_result_s++;
    }
    enki_nat* new = (enki_nat*)gc->alloc(gc, n, _Alignof(enki_nat));
    new->h.size_s = n;
    new->h.kind_b = ENKI_BIG_NAT;
    new->h.state_b = NF;
    new->n_limbs_s = n_limbs_s;
    memcpy(new->limbs, limbs, (n_limbs_s * sizeof(mp_limb_t)));
    return PTR_TO_ENKI(new);
}
enki_value enki_nat_alloc(enki_gc* gc, mp_limb_t* out, size_t n_limbs_s) {
    size_t n = n_limbs_s;
    while (n > 0 && out[n - 1] == 0) {
        n--;
    }
    if(gc != NULL && gc->root != NULL) {
        gc->root->stats.nat_normalize_s++;
        gc->root->stats.nat_requested_limbs_s += n_limbs_s;
        gc->root->stats.nat_final_limbs_s += n;
        gc->root->stats.nat_trimmed_limbs_s += n_limbs_s - n;
    }
    if(n == 0) {
      if(gc != NULL && gc->root != NULL) gc->root->stats.nat_immediate_result_s++;
      return (enki_value)0;
    }
    if(n == 1 && out[n - 1] < (1ULL << 63)) {
        enki_value res_v = (enki_value)out[0];
        if(gc != NULL && gc->root != NULL) gc->root->stats.nat_immediate_result_s++;
        return res_v;
    }
    enki_value res_v = enki_nat_alloc_big(gc, n, out);
    return res_v;
}
bool enki_nat_is_zero(enki_value x_v) {
    enki_nat_view a_nv;
    view_of_nat(x_v, &a_nv);
    return (a_nv.n_limbs_s == 0);
}
static void view_of_nat(enki_value a_v, enki_nat_view* v) {
    if(IS_PTR(a_v)) {
        enki_nat* nat = ENKI_AS(enki_nat, a_v);
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
    ENKI_PROFILE_ZONE("enki_nat_cmp");
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v < b_v ? -1 : (a_v > b_v ? 1 : 0);
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    return enki_nat_cmp_v(&a_nv, &b_nv);
}
static int enki_nat_cmp_v(const enki_nat_view* a_nv, const enki_nat_view* b_nv) {
    if(a_nv->n_limbs_s != b_nv->n_limbs_s) {
        return (a_nv->n_limbs_s > b_nv->n_limbs_s) ? 1 : -1;
    }
    for(size_t k_i = a_nv->n_limbs_s; k_i > 0; k_i--) {
        size_t idx_i = k_i - 1;
        if (a_nv->limbs[idx_i] != b_nv->limbs[idx_i]) {
            return (a_nv->limbs[idx_i] > b_nv->limbs[idx_i]) ? 1 : -1;
        }
    }
    return 0;
}
enki_value enki_nat_eq(enki_value a_v, enki_value b_v) {
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v == b_v ? 1 : 0;
    return (enki_nat_cmp(a_v, b_v) == 0) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_ne(enki_value a_v, enki_value b_v) {
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v != b_v ? 1 : 0;
    return (enki_nat_cmp(a_v, b_v) != 0) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_lt(enki_value a_v, enki_value b_v) {
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v < b_v ? 1 : 0;
    return (enki_nat_cmp(a_v, b_v) == -1) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_le(enki_value a_v, enki_value b_v) {
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v <= b_v ? 1 : 0;
    return (enki_nat_lt(a_v, b_v) || (enki_nat_eq(a_v, b_v))) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_gt(enki_value a_v, enki_value b_v) {
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v > b_v ? 1 : 0;
    return (enki_nat_cmp(a_v, b_v) == 1) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_ge(enki_value a_v, enki_value b_v) {
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v >= b_v ? 1 : 0;
    return (enki_nat_gt(a_v, b_v) || (enki_nat_eq(a_v, b_v))) ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_inc(enki_gc* gc, enki_value a_v) {
    ENKI_PROFILE_ZONE("enki_nat_inc");
    if(!IS_PTR(a_v) && a_v < ENKI_SMALL_MAX) return a_v + 1;
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)1;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, sizeof(mp_limb_t) * (a_nv.n_limbs_s + 1));
    mp_limb_t carry_q = mpn_add_1(out, a_nv.limbs, a_nv.n_limbs_s, 1);
    out[a_nv.n_limbs_s] = carry_q;
    return enki_nat_alloc(gc, out, a_nv.n_limbs_s + 1);
}
enki_value enki_nat_dec(enki_gc* gc, enki_value a_v) {
    ENKI_PROFILE_ZONE("enki_nat_dec");
    if(!IS_PTR(a_v)) return a_v == 0 ? 0 : a_v - 1;
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, a_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_sub_1(out, a_nv.limbs, a_nv.n_limbs_s, 1);
    return enki_nat_alloc(gc, out, a_nv.n_limbs_s);   
}
enki_value enki_nat_add(enki_gc* gc, enki_value a_v, enki_value b_v) {
    ENKI_PROFILE_ZONE("enki_nat_add");
    if(!IS_PTR(a_v) && !IS_PTR(b_v) && a_v <= ENKI_SMALL_MAX - b_v) return a_v + b_v;
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(a_nv.n_limbs_s == 0) return b_v;
    if(b_nv.n_limbs_s == 0) return a_v;
    const enki_nat_view* big_nv = &a_nv;
    const enki_nat_view* small_nv = &b_nv;
    if(big_nv->n_limbs_s < small_nv->n_limbs_s) {
        const enki_nat_view* tmp_nv = big_nv;
        big_nv = small_nv;
        small_nv = tmp_nv;
    }
    size_t n_s = big_nv->n_limbs_s + 1;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, sizeof(mp_limb_t) * n_s);
    mp_limb_t carry_q = mpn_add(out, big_nv->limbs, big_nv->n_limbs_s, small_nv->limbs, small_nv->n_limbs_s);
    out[n_s - 1] = carry_q;
    return enki_nat_alloc(gc, out, n_s);
}
enki_value enki_nat_sub(enki_gc* gc, enki_value a_v, enki_value b_v) {
    ENKI_PROFILE_ZONE("enki_nat_sub");
    if(!IS_PTR(a_v) && !IS_PTR(b_v)) return a_v > b_v ? a_v - b_v : 0;
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0; // no negatives 
    if(b_nv.n_limbs_s == 0) return a_v;
    if(enki_nat_cmp_v(&a_nv, &b_nv) < 0) return (enki_value)0; // a_v < b_v 
    mp_limb_t* out = enki_nat_tmp_alloc(gc, a_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_sub(out, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    return enki_nat_alloc(gc, out, a_nv.n_limbs_s);
}
enki_value enki_nat_mul(enki_gc* gc, enki_value a_v, enki_value b_v) {
    ENKI_PROFILE_ZONE("enki_nat_mul");
    if(!IS_PTR(a_v) && !IS_PTR(b_v) && (a_v == 0 || b_v <= ENKI_SMALL_MAX / a_v)) return a_v * b_v;
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(a_nv.n_limbs_s == 0 || b_nv.n_limbs_s == 0) return (enki_value)0;
    size_t n_s = (a_nv.n_limbs_s + b_nv.n_limbs_s);
    mp_limb_t* out = enki_nat_tmp_alloc(gc, n_s * sizeof(mp_limb_t));
    if(a_nv.n_limbs_s > b_nv.n_limbs_s) {
        mpn_mul(out, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    }
    else {
        mpn_mul(out, b_nv.limbs, b_nv.n_limbs_s, a_nv.limbs, a_nv.n_limbs_s);
    }
    return enki_nat_alloc(gc, out, n_s);
}
enki_value enki_nat_div(enki_gc* gc, enki_value a_v, enki_value b_v) {
    ENKI_PROFILE_ZONE("enki_nat_div");
    if(!IS_PTR(a_v) && !IS_PTR(b_v) && b_v != 0) return a_v / b_v;
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(b_nv.n_limbs_s == 0) {
        enki_interp_throw(gc->root, ENKI_ERROR_DIV_ZERO, b_v);
    }
    if(enki_nat_cmp_v(&a_nv, &b_nv) == -1) return (enki_value)0;
    size_t q_len_s = a_nv.n_limbs_s - b_nv.n_limbs_s + 1;
    mp_limb_t* Q = enki_nat_tmp_alloc(gc, q_len_s * sizeof(mp_limb_t));
    mp_limb_t* R = enki_nat_tmp_alloc(gc, b_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    return enki_nat_alloc(gc, Q, q_len_s);
}
enki_value enki_nat_mod(enki_gc* gc, enki_value a_v, enki_value b_v) {
    ENKI_PROFILE_ZONE("enki_nat_mod");
    if(!IS_PTR(a_v) && !IS_PTR(b_v) && b_v != 0) return a_v % b_v;
    enki_nat_view a_nv;
    enki_nat_view b_nv;
    view_of_nat(a_v, &a_nv);
    view_of_nat(b_v, &b_nv);
    if(b_nv.n_limbs_s == 0) {
        enki_interp_throw(gc->root, ENKI_ERROR_DIV_ZERO, b_v);
    }
    if(enki_nat_cmp_v(&a_nv, &b_nv) == -1) return a_v;
    size_t q_len_s = a_nv.n_limbs_s - b_nv.n_limbs_s + 1;
    mp_limb_t* Q = enki_nat_tmp_alloc(gc, q_len_s * sizeof(mp_limb_t));
    mp_limb_t* R = enki_nat_tmp_alloc(gc, b_nv.n_limbs_s * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_nv.limbs, a_nv.n_limbs_s, b_nv.limbs, b_nv.n_limbs_s);
    return enki_nat_alloc(gc, R, b_nv.n_limbs_s);
}
// lower bits to higher posistions 
enki_value enki_nat_lsh(enki_gc* gc, enki_value a_v, enki_value shift_v) {
    ENKI_PROFILE_ZONE("enki_nat_lsh");
    if(!IS_PTR(a_v) && !IS_PTR(shift_v) && shift_v < 63 && a_v <= (ENKI_SMALL_MAX >> shift_v)) return a_v << shift_v;
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(shift_v)) enki_interp_throw(gc->root, ENKI_ERROR_TYPE, shift_v);
    if(shift_v == 0) return a_v;
    size_t word_shift_s = (size_t)shift_v / 64;
    size_t bit_shift_s = (size_t)shift_v % 64;
    size_t n_s = (a_nv.n_limbs_s + word_shift_s + 1);
    mp_limb_t* out = enki_nat_tmp_alloc(gc, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    if(bit_shift_s == 0) {
        memcpy(out + word_shift_s, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
        return enki_nat_alloc(gc, out, n_s - 1);
    }
    mp_limb_t carry_q = mpn_lshift(out + word_shift_s, a_nv.limbs, a_nv.n_limbs_s, (unsigned int)bit_shift_s);
    out[n_s - 1] = carry_q;
    return enki_nat_alloc(gc, out, n_s);
}
// higher order bits to lower positions 
enki_value enki_nat_rsh(enki_gc* gc, enki_value a_v, enki_value shift_v) {
    ENKI_PROFILE_ZONE("enki_nat_rsh");
    if(!IS_PTR(a_v) && !IS_PTR(shift_v)) return shift_v >= 63 ? 0 : a_v >> shift_v;
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(shift_v))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, shift_v);
    if(shift_v == 0) return a_v;
    size_t word_shift_s = (size_t)shift_v / 64;
    size_t bit_shift_s = (size_t)shift_v % 64;
    if(word_shift_s >= a_nv.n_limbs_s) return (enki_value)0;
    const mp_limb_t* src = a_nv.limbs + word_shift_s;
    size_t n_src_s = (a_nv.n_limbs_s - word_shift_s);
    size_t n_s = n_src_s * sizeof(mp_limb_t);
    mp_limb_t* out = enki_nat_tmp_alloc(gc, n_s);
    memset(out, 0, n_s);
    if(bit_shift_s == 0) {
        memcpy(out, src, n_s);
        return enki_nat_alloc(gc, out, n_src_s);
    }
    mpn_rshift(out, src, n_src_s, (unsigned int)bit_shift_s);
    return enki_nat_alloc(gc, out, n_src_s);
}
enki_value enki_nat_bex(enki_gc* gc, enki_value bit_v) {
    if(!IS_PTR(bit_v) && bit_v < 63) return ((enki_value)1 << bit_v);
    if(IS_PTR(bit_v))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, bit_v);
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64;
    size_t n_s = word_off_o + 1;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    out[word_off_o] = ((mp_limb_t)1 << bit_off_o);
    return enki_nat_alloc(gc, out, n_s);
}
enki_value enki_nat_test(enki_gc* gc, enki_value bit_v, enki_value a_v) {
    if(!IS_PTR(bit_v) && !IS_PTR(a_v)) return bit_v < 63 ? ((a_v >> bit_v) & 1) : 0;
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(bit_v)) enki_interp_throw(gc->root, ENKI_ERROR_TYPE, bit_v);
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64; 
    if(word_off_o >= a_nv.n_limbs_s) return (enki_value)0;
    size_t res_v = (a_nv.limbs[word_off_o] & ((mp_limb_t)1 << bit_off_o)); 
    return res_v != 0 ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_set(enki_gc* gc, enki_value bit_v, enki_value a_v) {
    if(!IS_PTR(bit_v) && !IS_PTR(a_v) && bit_v < 63) return a_v | ((enki_value)1 << bit_v);
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(bit_v))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, bit_v);
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64; 
    size_t n_s = word_off_o + 1;
    if(n_s < a_nv.n_limbs_s) n_s = a_nv.n_limbs_s;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
    out[word_off_o] = out[word_off_o] |= ((mp_limb_t)1 << bit_off_o);
    return enki_nat_alloc(gc, out, n_s);
}
enki_value enki_nat_clear(enki_gc* gc, enki_value bit_v, enki_value a_v) {
    if(!IS_PTR(bit_v) && !IS_PTR(a_v) && bit_v < 63) return a_v & ~((enki_value)1 << bit_v);
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(IS_PTR(bit_v))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, bit_v);
    size_t word_off_o = (size_t)bit_v / 64;
    size_t bit_off_o = (size_t)bit_v % 64; 
    size_t n_s = word_off_o + 1;
    if(n_s < a_nv.n_limbs_s) n_s = a_nv.n_limbs_s;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
    out[word_off_o] = out[word_off_o] & ~((mp_limb_t)1 << bit_off_o);
    return enki_nat_alloc(gc, out, n_s);
}
enki_value enki_nat_bits(enki_gc* gc, enki_value a_v) {
    (void)gc;
    if(!IS_PTR(a_v)) return a_v == 0 ? 0 : (enki_value)(64 - __builtin_clzll(a_v));
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0;
    mp_limb_t top_limb_q = a_nv.limbs[a_nv.n_limbs_s - 1];
    size_t top_s = top_limb_q == 0 ? 0 : (64u - (size_t)__builtin_clzl(top_limb_q));
    return (enki_value)(top_s + ((a_nv.n_limbs_s - 1) * 64));
}
enki_value enki_nat_bytes(enki_gc* gc, enki_value a_v) {
    if(!IS_PTR(a_v)) return a_v == 0 ? 0 : (enki_value)((64 - __builtin_clzll(a_v) + 7) / 8);
    return (enki_value)((enki_nat_bits(gc, a_v) + 7)/8);
}
enki_value enki_nat_trunc(enki_gc* gc, enki_value width_v, enki_value a_v) {
    if(!IS_PTR(width_v) && !IS_PTR(a_v)) return width_v >= 63 ? a_v : (a_v & (((enki_value)1 << width_v) - 1));
    if(IS_PTR(width_v))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, width_v); 
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    if(a_nv.n_limbs_s == 0) return (enki_value)0;
    size_t word_off_o = (size_t)width_v / 64;
    size_t bit_off_o = (size_t)width_v % 64; 
    size_t n_s = bit_off_o != 0 ? word_off_o + 1 : word_off_o;
    size_t keep_s = min_s(a_nv.n_limbs_s, n_s);
    if(keep_s == 0) return (enki_value)0;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, keep_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, keep_s * sizeof(mp_limb_t));
    if(bit_off_o == 0) {
        return enki_nat_alloc(gc, out, keep_s);
    }
    if(keep_s == n_s) {
        out[keep_s - 1] = (out[keep_s - 1] & (((mp_limb_t)1ULL << bit_off_o) - 1));
    }
    return enki_nat_alloc(gc, out, keep_s);
}
enki_value enki_nat_trunc8(enki_gc* gc, enki_value a_v) {
    (void)gc;
    if(!IS_PTR(a_v)) return a_v & 0xFF;
    return enki_nat_trunc(gc, 8, a_v);
}
enki_value enki_nat_trunc16(enki_gc* gc, enki_value a_v) {
    (void)gc;
    if(!IS_PTR(a_v)) return a_v & 0xFFFF;
    return enki_nat_trunc(gc, 16, a_v);
}
enki_value enki_nat_trunc32(enki_gc* gc, enki_value a_v) {
    (void)gc;
    if(!IS_PTR(a_v)) return a_v & 0xFFFFFFFF;
    return enki_nat_trunc(gc, 32, a_v);
}
enki_value enki_nat_trunc64(enki_gc* gc, enki_value a_v) {
    (void)gc;
    if(!IS_PTR(a_v)) return a_v;
    return enki_nat_trunc(gc, 64, a_v);
}
enki_value enki_nat_load8(enki_gc* gc, enki_value index_i, enki_value a_v) {
    if(!IS_PTR(index_i) && !IS_PTR(a_v)) return index_i < 8 ? ((a_v >> (index_i * 8)) & 0xFF) : 0;
    if(IS_PTR(index_i)) enki_interp_throw(gc->root, ENKI_ERROR_TYPE, index_i);
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
    if(!IS_PTR(index_i) && !IS_PTR(byte_b) && !IS_PTR(a_v) && index_i < 8) {
        enki_value shift_v = index_i * 8;
        enki_value res_v = (a_v & ~((enki_value)0xFF << shift_v)) | ((byte_b & 0xFF) << shift_v);
        if(res_v <= ENKI_SMALL_MAX) return res_v;
    }
    if(IS_PTR(index_i))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, index_i);
    if(IS_PTR(byte_b))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, byte_b);
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    size_t limb_index_i = (size_t)index_i / sizeof(mp_limb_t);
    size_t byte_offset_o = (size_t)index_i % sizeof(mp_limb_t);
    size_t n_s = a_nv.n_limbs_s <= limb_index_i ? limb_index_i + 1 : a_nv.n_limbs_s;
    mp_limb_t* out = enki_nat_tmp_alloc(gc, n_s * sizeof(mp_limb_t));
    memset(out, 0, n_s * sizeof(mp_limb_t));
    memcpy(out, a_nv.limbs, a_nv.n_limbs_s * sizeof(mp_limb_t));
    mp_limb_t mask_q = (mp_limb_t)~(0xFFULL << (byte_offset_o * 8)); 
    mp_limb_t byte_bits_q = (mp_limb_t)(byte_b & 0xFF) << (byte_offset_o * 8);
    out[limb_index_i] = (out[limb_index_i] & mask_q) | byte_bits_q;
    return enki_nat_alloc(gc, out, n_s);
}
enki_value enki_nat_nib(enki_gc* gc, enki_value index_i, enki_value a_v) {
    if(!IS_PTR(index_i) && !IS_PTR(a_v)) return index_i < 16 ? ((a_v >> (index_i * 4)) & 0xF) : 0;
    if(IS_PTR(index_i))  enki_interp_throw(gc->root, ENKI_ERROR_TYPE, index_i);
    enki_nat_view a_nv;
    view_of_nat(a_v, &a_nv);
    size_t limb_index_i = index_i/16;
    size_t shift_v = (index_i % 16) * 4;
    if (limb_index_i >= a_nv.n_limbs_s) return (enki_value)0;
    return (enki_value)((a_nv.limbs[limb_index_i] >> shift_v) & 0x0F);
}

int enki_nat_le_bool(enki_value a_v, enki_value b_v) {
    return enki_nat_cmp(a_v, b_v) < 0;
}

size_t enki_bat_met_bytes(enki_value a_v) {
    return (size_t)enki_nat_bytes(NULL, a_v);
}

static uint64_t _enki_strnat_direct(char* str_c, size_t str_s)
{
    uint64_t ret_q = 0;
    ea_assertf(str_s <= 8, "strnat bloat %lu", str_s);
    for(size_t k = 0; k < str_s; k++) {
        ret_q |= ((uint64_t)str_c[k] << (8 * k));
    }
    return ret_q;
}

enki_value enki_alloc_strnat(enki_gc* gc, char* str_c, size_t str_s)
{
    if(str_s < 8) return _enki_strnat_direct(str_c, str_s);
    return enki_alloc_big_nat_bytes(gc, str_s, str_c);
}

enki_value enki_alloc_cstrnat(enki_gc* gc, char* str_c)
{
    return enki_alloc_strnat(gc, str_c, strlen(str_c));
}

static enki_nat* _enki_alloc_big_nat_empty(enki_gc* gc, size_t n_limbs_s)
{
    size_t n = sizeof(enki_nat) + (n_limbs_s * sizeof(mp_limb_t));
    if(gc != NULL && gc->root != NULL) {
        gc->root->stats.nat_heap_alloc_s++;
        gc->root->stats.nat_heap_bytes_s += n;
        gc->root->stats.nat_big_result_s++;
    }
    enki_nat* new = (enki_nat*)gc->alloc(gc, n, _Alignof(enki_nat));
    if(!new) return NULL;
    new->h.size_s = n;
    new->h.kind_b = ENKI_BIG_NAT;
    new->h.state_b = NF;
    new->n_limbs_s = n_limbs_s;
    return new;
}

enki_value enki_alloc_big_nat_bytes(enki_gc* gc, size_t byt_s, char* byt_b)
{
    size_t n_limbs_s = (byt_s + sizeof(mp_limb_t) - 1) / sizeof(mp_limb_t);
    enki_nat* nat = _enki_alloc_big_nat_empty(gc, n_limbs_s);
    if(!nat) return 0;
    memset(nat->limbs, 0, n_limbs_s * sizeof(mp_limb_t));
    memcpy((char*)nat->limbs, byt_b, byt_s);
    return PTR_TO_ENKI(nat);
}
