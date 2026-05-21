#include <string.h>

#include "enki/value.h"
#include "enki/gc.h"

void enki_trace_value(enki_gc* gc, void* obj) {
    enki_value_header* h = obj;
    switch (h->kind) {
        case ENKI_PIN: {
          enki_pin* pin = obj;
          pin->inner = gc->copy(gc, pin->inner);
          for(size_t k = 0; k < pin->n_subpins; k++) {
            pin->subpins[k] = gc->copy(gc, pin->subpins[k]);
          }
          break;
        }
        case ENKI_LAW: {
          enki_law* law = obj;
          law->body = gc->copy(gc, law->body);
          law->name = gc->copy(gc, law->name);
          for(size_t k = 0; k < law->n_const; k++) {
            ENKI_LAW_CONSTS(law)[k] = gc->copy(gc, ENKI_LAW_CONSTS(law)[k]);
          }
          break;
        }
        case ENKI_APP: {
          enki_app* app = obj;
          app->fn = gc->copy(gc, app->fn);
          for (size_t k = 0; k < app->n_args; k++) {
            app->args[k] = gc->copy(gc, app->args[k]);
          }
          break;
        }
        case ENKI_CONT: {
          enki_cont* cont = obj;
          for (size_t k = 0; k < cont->n_args; k++) {
            cont->args[k] = gc->copy(gc, cont->args[k]);
          }
          break;
        }
        default:
          return;
    }
}

enki_value enki_alloc_big_nat(enki_gc* gc, size_t n_limbs, mp_limb_t limbs[]) {
    size_t n = sizeof(enki_nat) + (n_limbs * sizeof(mp_limb_t));
    enki_nat* new = (enki_nat*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_BIG_NAT;
    new->h.state = NF;
    new->n_limbs = n_limbs;
    memcpy(new->limbs, limbs, (n_limbs * sizeof(mp_limb_t)));
    return PTR_TO_ENKI(new);
}
enki_value enki_alloc_law(enki_gc* gc, size_t arity, enki_value name, enki_value body, 
    size_t bc_len, size_t n_const, uint8_t* bc, enki_value* const_table) {
    size_t n = sizeof(enki_law) + bc_len + (n_const * sizeof(enki_value));
    enki_law* new = (enki_law*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_LAW;
    new->h.state = NF;
    new->body = body;
    new->name = name;
    new->arity = (uint32_t)arity;
    new->bc_len = bc_len;
    new->n_const = n_const;
    size_t const_off = n_const * sizeof(enki_value);
    if(n_const > 0) memcpy(new->data, const_table, const_off);
    if(bc_len > 0) memcpy(new->data + const_off, bc, bc_len);
    return PTR_TO_ENKI(new);
}

enki_value enki_alloc_nat(enki_gc* gc, mp_limb_t* out, size_t n_limbs) {
    size_t n = n_limbs;
    while (n > 0 && out[n - 1] == 0) {
        n--;
    }
    if(n == 0) {
      gc->sys.free(gc->sys.ctx, out);
      return (enki_value)0;
    }
    if(n == 1 && out[n - 1] < (1ULL << 63)) {
        enki_value res = (enki_value)out[0];
        gc->sys.free(gc->sys.ctx, out);
        return res;
    }
    enki_value res = enki_alloc_big_nat(gc, n, out);
    gc->sys.free(gc->sys.ctx, out);
    return res;
}

enki_value enki_alloc_pin(enki_gc* gc, const 
    uint8_t hash[32], enki_value inner, size_t n_subpins, enki_value subpins[]) {
    size_t n = sizeof(enki_pin) + (n_subpins * sizeof(enki_value));
    enki_pin* new = (enki_pin*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_PIN;
    new->h.state = WHNF;
    new->inner = inner;
    new->n_subpins = n_subpins;
    memcpy(new->hash, hash, 32);
    if(n_subpins > 0) memcpy(new->subpins, subpins, n_subpins * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}
enki_value enki_alloc_app(enki_gc* gc, enki_value fn, 
    size_t n_args) {
    size_t n = sizeof(enki_app) + (n_args * sizeof(enki_value));
    enki_app* new = (enki_app*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_APP;
    new->h.state = WHNF;
    new->fn = fn;
    new->n_args = n_args;
    //if(n_args > 0 && args != NULL) memcpy(new->args, args, n_args * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}

enki_value enki_alloc_cont(enki_gc* gc, size_t n_args, enki_value* bas) {
    size_t n = sizeof(enki_cont) + (n_args * sizeof(enki_value));
    enki_cont* new = (enki_cont*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_CONT;
    new->h.state = WHNF;
    new->n_args = n_args;
    memcpy(new->args, bas, n_args * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}
