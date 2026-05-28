
#include <stddef.h>
#include <string.h>

#include "enki/app.h"
#include "enki/eval.h"
#include "enki/interp.h"
#include "enki/pin.h"
#include "enki/value.h"

enki_value enki_eval_whnf(enki_interpreter* i, enki_value val_v) {
    while(1) {
        i->stats.whnf_s++;
        if(!IS_PTR(val_v)) {
            i->stats.whnf_immediate_s++;
            return val_v;
        }
        enki_value_header* h = ENKI_AS(enki_value_header, val_v);
        if(h->kind_b == NAT) {
            i->stats.whnf_nat_s++;
            h->state_b = NF;
            return val_v;
        }
        else if(h->kind_b == PIN) {
            i->stats.whnf_pin_s++;
            h->state_b = WHNF;
            return val_v;
        }
        else if(h->kind_b == LAW) {
            i->stats.whnf_law_s++;
            h->state_b = WHNF;
            return val_v;
        }
        if(h->kind_b == IND) {
            enki_app* ind = ENKI_AS(enki_app, val_v);
            val_v = ind->fn_v;
            continue;
        }
        else if(h->kind_b == APP) {
            if(h->state_b != THUNK) {
                i->stats.whnf_app_whnf_s++;
                return val_v;
            }
            i->stats.whnf_app_thunk_s++;
            
            // root app 
            size_t root_sp = i->sp;
            i->stack_v[root_sp] = val_v;
            i->sp++;

            size_t base_sp = i->sp;
            size_t base_cp = i->cp;
            enki_app* app = ENKI_AS(enki_app, val_v);
            size_t arity_s = enki_app_arity(app->fn_v);
            size_t total_args_s = app->n_args_s;
            i->stack_v[base_sp] = app->fn_v;
            i->sp = base_sp + 1;
            for(size_t k = 0; k < arity_s; k++) {
                i->stack_v[i->sp++] = app->args_v[k];
            }
            enki_app_apply(i, arity_s);
            while(i->cp > base_cp) {
                enki_interp_step(i);
                enki_arena_reset(i->scratch_a);
            }
            size_t leftover_count_s = total_args_s - arity_s;
            if(leftover_count_s > 0) {
                app = ENKI_AS(enki_app, i->stack_v[root_sp]);
                i->sp = base_sp + 1;
                for(size_t k = 0; k < leftover_count_s; k++) {
                    i->stack_v[i->sp++] = app->args_v[k + arity_s];
                }
                enki_app_apply(i, leftover_count_s);
                while(i->cp > base_cp) {
                    enki_interp_step(i);
                    enki_arena_reset(i->scratch_a);
                }
            }
            app = ENKI_AS(enki_app, i->stack_v[root_sp]);
            app->h.state_b = WHNF;
            app->h.kind_b = IND;
            app->n_args_s = 0;
            app->fn_v = i->stack_v[base_sp];
            val_v = i->stack_v[base_sp];
            i->sp = root_sp;
            continue;
        }
        return val_v;
    }
}

enki_value enki_eval_nf(enki_interpreter* i, enki_value x) {
    x = enki_eval_whnf(i, x);
    if(!IS_PTR(x)) return x;
    enki_value_header* h = ENKI_AS(enki_value_header, x);
    if (h->state_b == NF) return x;
    size_t root_s = i->sp;
    i->stack_v[i->sp] = x;
    i->sp++;
    switch(h->kind_b) {
        case ENKI_NAT: {
            x = i->stack_v[root_s];
            h = ENKI_AS(enki_value_header, x);
            h->state_b = NF;
            i->sp = root_s;
            return x;
        }
        case ENKI_PIN: {
            x = i->stack_v[root_s];
            enki_pin* pin = ENKI_AS(enki_pin, x);
            pin->inner_v = enki_eval_nf(i, pin->inner_v);
            x = i->stack_v[root_s];
            pin = ENKI_AS(enki_pin, x);
            for(size_t k = 0; k < pin->n_subpins_s; k++) {
                pin->subpins_v[k] = enki_eval_nf(i, pin->subpins_v[k]);
                x = i->stack_v[root_s];
                pin = ENKI_AS(enki_pin, x);
            }
            uint8_t b_hash_b[32];
            enki_pin_hash(i, x, b_hash_b);
            x = i->stack_v[root_s];
            h = ENKI_AS(enki_value_header, x);
            pin = ENKI_AS(enki_pin, x);
            uint8_t zero_hash_b[32] = {0};
            if(memcmp(pin->hash_b, zero_hash_b, 32) != 0) {
                if(memcmp(pin->hash_b, b_hash_b, 32) != 0) {
                    enki_interp_throw(i, ENKI_ERROR_BAD_PIN, x); // hashes dont match 
                }
            }
            else {
                memcpy(pin->hash_b, b_hash_b, 32);
            }
            h->state_b = NF;
            i->sp = root_s;
            return x;
        }
        case ENKI_LAW: {
            x = i->stack_v[root_s];
            enki_law* law = ENKI_AS(enki_law, x);
            law->body_v = enki_eval_nf(i, law->body_v);
            x = i->stack_v[root_s];
            law = ENKI_AS(enki_law, x);
            law->name_v = enki_eval_nf(i, law->name_v);
            x = i->stack_v[root_s];
            law = ENKI_AS(enki_law, x);
            for(size_t k = 0; k < law->n_const_s; k++) {
                ENKI_LAW_CONSTS(law)[k] = enki_eval_nf(i, ENKI_LAW_CONSTS(law)[k]);
                x = i->stack_v[root_s];
                law = ENKI_AS(enki_law, x);
            }
            h = ENKI_AS(enki_value_header, x);
            h->state_b = NF;
            i->sp = root_s;
            return x;
        }
        case ENKI_APP: {
          x = i->stack_v[root_s];
          enki_app* app = ENKI_AS(enki_app, x);
          app->fn_v = enki_eval_nf(i, app->fn_v);
          x = i->stack_v[root_s];
          app = ENKI_AS(enki_app, x);
          for (size_t k = 0; k < app->n_args_s; k++) {
            app->args_v[k] = enki_eval_nf(i, app->args_v[k]);
            x = i->stack_v[root_s];
            app = ENKI_AS(enki_app, x);
          }
          h = ENKI_AS(enki_value_header, x);
          h->state_b = NF;
          i->sp = root_s;
          return x;
        }
        default:
            break;
    }   
    i->sp = root_s;
    return x;
}
