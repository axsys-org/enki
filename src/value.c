#include <string.h>

#include "enki/value.h"

enki_value enki_alloc_nat(enki_allocator a_alloc, mp_limb_t* p_limbs, size_t n_limbs) {
    if(!a_alloc.alloc || !a_alloc.free) return 0; // port back
    if(n_limbs > 0 && !p_limbs) return 0; // port back
    size_t n_used = n_limbs;
    while (n_used > 0 && p_limbs[n_used - 1] == 0) {
        n_used--;
    }
    if(n_used == 0) {
        a_alloc.free(a_alloc.ctx, p_limbs); // port back
        return (enki_value)0;
    }
    if(n_used == 1 && p_limbs[n_used - 1] < (1ULL << 63)) {
        enki_value v = (enki_value)p_limbs[0];
        a_alloc.free(a_alloc.ctx, p_limbs); // port back
        return v;
    }
    size_t cb = sizeof(enki_nat) + (sizeof(mp_limb_t) * n_used);
    enki_nat* p_nat = (enki_nat*)a_alloc.alloc(a_alloc.ctx, cb);
    if(!p_nat) {
        a_alloc.free(a_alloc.ctx, p_limbs);
        return 0;
    }
    p_nat->h.size = cb;
    p_nat->h.kind = ENKI_NAT;
    p_nat->n_limbs = n_used;
    memcpy(p_nat->limbs, p_limbs, n_used * sizeof(mp_limb_t));
    a_alloc.free(a_alloc.ctx, p_limbs);
    return PTR_TO_ENKI(p_nat);
}
enki_value enki_alloc_law(enki_allocator a_alloc, size_t n_arity, enki_value v_name, enki_value v_body, 
    size_t cb_bc, size_t n_const, uint8_t* p_bc, enki_value* p_const_table) {
    if(!a_alloc.alloc) return 0; // port back
    if(n_arity > UINT32_MAX) return 0; // port back
    if(cb_bc > 0 && !p_bc) return 0; // port back
    if(n_const > 0 && !p_const_table) return 0; // port back
    size_t cb = sizeof(enki_law) + cb_bc + (n_const * sizeof(enki_value));
    enki_law* p_law = (enki_law*)a_alloc.alloc(a_alloc.ctx, cb);
    if(!p_law) return 0;
    p_law->h.size = cb;
    p_law->h.kind = ENKI_LAW;
    p_law->body = v_body;
    p_law->name = v_name;
    // port back
    p_law->arity = (uint32_t)n_arity;
    p_law->bc_len = cb_bc;
    p_law->n_const = n_const;
    size_t off_const = n_const * sizeof(enki_value);
    if(n_const > 0 && p_const_table != NULL) memcpy(p_law->data, p_const_table, off_const);
    if(cb_bc > 0 && p_bc != NULL) memcpy(p_law->data + off_const, p_bc, cb_bc);
    return PTR_TO_ENKI(p_law);
}
enki_value enki_alloc_pin(enki_allocator a_alloc, const 
    uint8_t p_hash[32], enki_value v_inner, size_t n_subpins, enki_value p_subpins[]) {
    if(!a_alloc.alloc) return 0; // port back
    if(!p_hash) return 0; // port back
    if(n_subpins > 0 && !p_subpins) return 0; // port back
    size_t cb = sizeof(enki_pin) + (n_subpins * sizeof(enki_value));
    enki_pin* p_pin = (enki_pin*)a_alloc.alloc(a_alloc.ctx, cb);
    if(!p_pin) return 0;
    p_pin->h.size = cb;
    p_pin->h.kind = ENKI_PIN;
    p_pin->inner = v_inner;
    p_pin->n_subpins = n_subpins;
    memcpy(p_pin->hash, p_hash, 32);
    if(n_subpins > 0 && p_subpins != NULL) memcpy(p_pin->subpins, p_subpins, n_subpins * sizeof(enki_value));
    return PTR_TO_ENKI(p_pin);
}
enki_value enki_alloc_app(enki_allocator a_alloc, enki_value v_fn, 
    size_t n_args, enki_value* p_args) {
    if(!a_alloc.alloc) return 0; // port back
    size_t cb = sizeof(enki_app) + (n_args * sizeof(enki_value));
    enki_app* p_app = (enki_app*)a_alloc.alloc(a_alloc.ctx, cb);
    if(!p_app) return 0;
    p_app->h.size = cb;
    p_app->h.kind = ENKI_APP;
    p_app->fn = v_fn;
    p_app->n_args = n_args;
    if(n_args > 0 && p_args != NULL) memcpy(p_app->args, p_args, n_args * sizeof(enki_value));
    return PTR_TO_ENKI(p_app);
}
