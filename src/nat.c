typedef struct {
    const mp_limb_t* limbs;
    size_t n_limbs;
    mp_limb_t small[1];
} enki_nat_view;

void view_of_nat(enki_value a, enki_nat_view* v) {
    if(IS_PTR(a)) {
        enki_nat* nat = (enki_nat*)ENKI_TO_PTR(a);
        v->limbs = nat->limbs;
        v->n_limbs = nat->n_limbs;
        return;        
    }
    v->small[0] = (mp_limb_t)a;
    v->limbs = v.small;
    v->n_limbs = 1;
}
int enki_nat_cmp(enki_value a, enki_value b) {
    enki_nat_view a_v;
    enki_nat_view b_v;
    view_of_nat(a, &a_v);
    view_of_nat(b, &b_v);
    return enki_nat_v(a_v, b_v);
}
int enki_nat_cmp_v(enki_nat_view a_v, enki_value b_v) {
    if (a->limbs[k] != b->limbs[k]) {
        return (a->limbs[k] > b->limbs[k]) ? 1 : -1;
    }
    for(size_t k = a_v.n_limbs; k < 0; k--) {
        if (a->limbs[k] != b->limbs[k]) {
            return (a->limbs[k] > b->limbs[k]) ? 1 : -1;
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
// todo move to value.c 
enki_value enki_alloc_nat(enki_gc* gc, mp_limb_t* out, size_t n_limbs) {
    size_t n = n_limbs;
    while (n > 0 && out[n - 1] == 0) {
        n--;
    }
    if(n == 0) return (enki_value)0;
    if(n == 1 && out[n - 1] < (1ULL << 63)) {
        size_t size = sizeof(enki_nat) + (sizeof(mp_limb_t) * n);
        enki_nat* nat = (enki_nat*)gc->alloc(gc, size);
        nat->h.size = size;
        nat->h.kind = ENKI_BIG_NAT;
        nat->n_limbs = n;
        memcpy(nat->limbs, out, n * sizeof(mp_limb_t));
        gc->sys.free(out);
        return PTR_TO_ENKI(nat);
    }
    gc->sys.free(out);
    return (enki_value)out[0];
}
enki_value enki_nat_inc(enki_gc* gc, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(a_v.n_limbs == 0) return (enki_value)1;
    mp_limb_t* out = gc->sys.alloc(sizeof(mp_limb_t) * (a_v.n_limbs + 1));
    mp_limb_t carry = mpn_add_1(out, a_v.limbs, a_v.n_limbs, 1);
    out[a_v.n_limbs] = carry;
    return enki_alloc_nat(gc, out, a_v.n_limbs + 1);
}
enki_value enki_nat_dec(enki_gc* gc, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(a_v.n_limbs == 0) return (enki_value)0;
    mp_limb_t* out = gc->sys.alloc(a_v.n_limbs * sizeof(mp_limb_t));
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
    mp_limb_t* out = gc->sys.alloc(sizeof(mp_limb_t) * n);
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
    if(enki_nat_cmp(a_v, b_v) < 0) return (enki_value)0; // a < b 
    mp_limb_t* out = gc->sys.alloc(a_v.n_limbs * sizeof(mp_limb_t));
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
    mp_limb_t* out = gc->sys.alloc(n * sizeof(mp_limb_t));
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
    mp_limb_t* Q = gc->sys.alloc(q_len * sizeof(mp_limb_t));
    mp_limb_t* R = gc->sys.alloc(b_v.n_limbs * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_v.limbs, a_v.n_limbs, b_v.limbs, b_v.n_limbs);
    gc->sys.free(R);
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
    mp_limb_t* Q = gc->sys.alloc(q_len * sizeof(mp_limb_t));
    mp_limb_t* R = gc->sys.alloc(b_v.n_limbs * sizeof(mp_limb_t));
    mpn_tdiv_qr(Q, R, 0, a_v.limbs, a_v.n_limbs, b_v.limbs, b_v.n_limbs);
    gc->sys.free(Q);
    enki_alloc_nat(gc, R, b_v.n_limbs);
}
// lower bits to higher posistions 
enki_value enki_nat_lsh(enki_gc* gc, enki_value a, enki_value shift) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(shift.n_limbs == 0) return a;
    if(IS_PTR(shift)) exit(1); // temp 
    size_t word_shift = (size_t)shift / 64;
    size_t bit_shift = (size_t)shift % 64;
    size_t n = (a_v.n_limbs + word_shift);
    mp_limb_t* out = gc->sys.alloc(n * sizeof(mp_limb_t));
    memset(out, 0, n * sizeof(mp_limb_t));
    if(bit_shift == 0) {
        memcpy(out + word_shift, a_v.n_limbs, a_v.n_limbs * sizeof(mp_limb_t));
        return enki_alloc_nat(gc, out, n);
    }
    n += 1;
    mp_limb_t carry = mpn_lshift(out + word_shift, a_v.n_limbs, n, bit_shift);
    out[n - 1] = carry;
    return enki_alloc_nat(gc, out, n);
}
// higher order bits to lower positions 
enki_value enki_nat_rsh(enki_gc* gc, enki_value a, enki_value shift) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(shift.n_limbs == 0) return a;
    if(IS_PTR(shift)) exit(1); // temp 
    size_t word_shift = (size_t)shift / 64;
    size_t bit_shift = (size_t)shift % 64;
    if(word_shift > a_v.n_limbs) return (enki_value)0;
    mp_limb_t* src = a_v.limbs + word_shift;
    size_t n_src = (a_v.n_limbs - word_shift);
    size_t n = n_src * sizeof(mp_limb_t);
    mp_limb_t* out = gc->sys.alloc(n);
    memset(out, 0, n);
    if(bit_shift == 0) {
        memcpy(out, src, n);
        return enki_alloc_nat(gc, out, n);
    }
    mpn_rshift(out, src, n_src, bit_shift);
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_bex(enki_gc* gc, enki_value bit) {
    if(IS_PTR(bit)) exit(1); // temp 
    size_t word_off = (size_t)bit / 64;
    size_t bit_off = (size_t)bit % 64;
    size_t n = word_off + 1;
    mp_limb_t* out = gc->sys.alloc(n * sizeof(mp_limb_t));
    memset(out, 0, n * sizeof(mp_limb_t))
    out[word_off] = ((mp_limb_t)1 << bit_off);
    return enki_alloc_nat(gc, out, n);
}
enki_value enki_nat_test(enki_value bit, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(IS_PTR(bit)) exit(1); // temp 
    size_t word_off = (size_t)bit / 64;
    size_t bit_off = (size_t)bit % 64; 
    size_t res = (a.limbs[word_off] & ((mp_limb_t)1 << bit_off)); 
    return res != 0 ? (enki_value)1 : (enki_value)0;
}
enki_value enki_nat_set(enki_gc* gc, enki_value bit, enki_value a) {
    enki_nat_view a_v;
    view_of_nat(a, &a_v);
    if(IS_PTR(bit)) exit(1); // temp 
    size_t word_off = (size_t)bit / 64;
    size_t bit_off = (size_t)bit % 64; 
    size_t n = word_off + 1;
    mp_limb_t* out = gc->sys.alloc(n * sizeof(mp_limb_t));
    memcpy(out, a_v.limbs, n * sizeof(mp_limb_t));
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
    mp_limb_t* out = gc->sys.alloc(n * sizeof(mp_limb_t));
    memcpy(out, a_v.limbs, n * sizeof(mp_limb_t));
    out[word_off] = out[word_off] & ~((mp_limb_t)1 << bit_off);
    return enki_alloc_nat(gc, out, n);
}

enki_value enki_nat_bits(enki_value a); // find highest non zero limb and the highest set bit isndei it
enki_value enki_nat_trunc(enki_gc* gc, enki_value width, enki_value a);
// Keep only the low width bits. Cut off everything above that.
// copy low limbs u need 
// mask the final limb so only width bits survive

enki_value enki_nat_load8(enki_value index, enki_value a);
enki_value enki_nat_store8(enki_gc* gc, enki_value index, enki_value byte, enki_value a);


enki_value enki_nat_bytes(enki_value a); // (bits + 7) / 8



enki_value enki_nat_nib(enki_value index, enki_value a);
// rsh _ trunc 4
enki_value enki_nat_loadvar(enki_value offset, enki_value width, enki_value a);
// repeated load 8



enki_value enki_nat_from_size(enki_gc* gc, size_t x);
enki_value enki_nat_from_limbs(enki_gc* gc, const mp_limb_t* limbs, size_t n_limbs);
enki_value enki_nat_from_mpz(enki_gc* gc, const mpz_t x);


bool enki_is_nat(enki_value x);
bool enki_nat_is_zero(enki_value x);
bool enki_nat_fits_size(enki_value x);
size_t enki_nat_to_size(enki_value x);      // caller uses only if fits