#include <string.h>

#include "enki/value.h"
#include "enki/gc.h"

void enki_trace_value(enki_gc* gc, void* obj) {
    enki_value_header* h = obj;
    switch (h->kind_b) {
        case ENKI_PIN: {
          enki_pin* pin = obj;
          pin->inner_v = gc->copy(gc, pin->inner_v);
          for(size_t k = 0; k < pin->n_subpins_s; k++) {
            pin->subpins_v[k] = gc->copy(gc, pin->subpins_v[k]);
          }
          break;
        }
        case ENKI_LAW: {
          enki_law* law = obj;
          law->body_v = gc->copy(gc, law->body_v);
          law->name_v = gc->copy(gc, law->name_v);
          for(size_t k = 0; k < law->n_const_s; k++) {
            ENKI_LAW_CONSTS(law)[k] = gc->copy(gc, ENKI_LAW_CONSTS(law)[k]);
          }
          break;
        }
        case ENKI_APP: {
          enki_app* app = obj;
          app->fn_v = gc->copy(gc, app->fn_v);
          for (size_t k = 0; k < app->n_args_s; k++) {
            app->args_v[k] = gc->copy(gc, app->args_v[k]);
          }
          break;
        }
        case ENKI_CONT: {
          enki_cont* cont_v = obj;
          for (size_t k = 0; k < cont_v->n_args_s; k++) {
            cont_v->args_v[k] = gc->copy(gc, cont_v->args_v[k]);
          }
          break;
        }
        default:
          return;
    }
}

enki_value enki_alloc_big_nat(enki_gc* gc, size_t n_limbs_s, mp_limb_t limbs[]) {
    size_t n = sizeof(enki_nat) + (n_limbs_s * sizeof(mp_limb_t));
    enki_nat* new = (enki_nat*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_BIG_NAT;
    new->h.state_b = NF;
    new->n_limbs_s = n_limbs_s;
    memcpy(new->limbs, limbs, (n_limbs_s * sizeof(mp_limb_t)));
    return PTR_TO_ENKI(new);
}
enki_value enki_alloc_law(enki_gc* gc, size_t arity_s, enki_value name_v, enki_value body_v, 
    size_t bc_len_s, size_t n_const_s, uint8_t* bc_b, enki_value* const_table_v) {
    size_t n = sizeof(enki_law) + bc_len_s + (n_const_s * sizeof(enki_value));
    enki_law* new = (enki_law*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_LAW;
    new->h.state_b = NF;
    new->body_v = body_v;
    new->name_v = name_v;
    new->arity_s = (uint32_t)arity_s;
    new->bc_len_s = bc_len_s;
    new->n_const_s = n_const_s;
    size_t const_off_o = n_const_s * sizeof(enki_value);
    if(n_const_s > 0) memcpy(new->data_b, const_table_v, const_off_o);
    if(bc_len_s > 0) memcpy(new->data_b + const_off_o, bc_b, bc_len_s);
    return PTR_TO_ENKI(new);
}

enki_value enki_alloc_nat(enki_gc* gc, mp_limb_t* out, size_t n_limbs_s) {
    size_t n = n_limbs_s;
    while (n > 0 && out[n - 1] == 0) {
        n--;
    }
    if(n == 0) {
      gc->sys_a.free(gc->sys_a.ctx, out);
      return (enki_value)0;
    }
    if(n == 1 && out[n - 1] < (1ULL << 63)) {
        enki_value res_v = (enki_value)out[0];
        gc->sys_a.free(gc->sys_a.ctx, out);
        return res_v;
    }
    enki_value res_v = enki_alloc_big_nat(gc, n, out);
    gc->sys_a.free(gc->sys_a.ctx, out);
    return res_v;
}

enki_value enki_alloc_pin(enki_gc* gc, const 
    uint8_t hash_b[32], enki_value inner_v, size_t n_subpins_s, enki_value subpins_v[]) {
    size_t n = sizeof(enki_pin) + (n_subpins_s * sizeof(enki_value));
    enki_pin* new = (enki_pin*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_PIN;
    new->h.state_b = WHNF;
    new->inner_v = inner_v;
    new->n_subpins_s = n_subpins_s;
    memcpy(new->hash_b, hash_b, 32);
    if(n_subpins_s > 0) memcpy(new->subpins_v, subpins_v, n_subpins_s * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}
enki_value enki_alloc_app(enki_gc* gc, enki_value fn_v, 
    size_t n_args_s) {
    size_t n = sizeof(enki_app) + (n_args_s * sizeof(enki_value));
    enki_app* new = (enki_app*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_APP;
    new->h.state_b = WHNF;
    new->fn_v = fn_v;
    new->n_args_s = n_args_s;
    //if(n_args_s > 0 && args_v != NULL) memcpy(new->args_v, args_v, n_args_s * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}

enki_value enki_alloc_cont(enki_gc* gc, size_t n_args_s, enki_value* bas_v) {
    size_t n = sizeof(enki_cont) + (n_args_s * sizeof(enki_value));
    enki_cont* new = (enki_cont*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_CONT;
    new->h.state_b = WHNF;
    new->n_args_s = n_args_s;
    memcpy(new->args_v, bas_v, n_args_s * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}
