
#include <stddef.h>
#include <string.h>

#include "enki/app.h"
#include "enki/eval.h"
#include "enki/interp.h"
#include "enki/pin.h"
#include "enki/value.h"


enki_value enki_eval_whnf(enki_interpreter* i, enki_value x) {
    if(!IS_PTR(x)) return x;
    enki_value_header* h = ENKI_AS(enki_value_header, x);
    switch(h->kind_b) {
        case ENKI_NAT:
            h->state_b = NF;
            return x;
        case ENKI_LAW:
            h->state_b = WHNF;
            return x;
        case ENKI_PIN: 
            h->state_b = WHNF;
            return x;
        case ENKI_APP: {
            if(h->state_b == NF || h->state_b == WHNF) return x;
            size_t base_fp_s = i->fp;
            size_t res_base_s = i->sp;
            i->stack_v[i->sp] = x;
            i->sp++;
            enki_app_apply(i, 0);
            while(i->fp > base_fp_s && !i->halted) {
                enki_interp_step(i);
                enki_arena_reset(i->scratch_a);
            }
            enki_value res_v = i->stack_v[res_base_s];
            i->sp = res_base_s + 1; // keep scratch_s are rooted
            res_v = enki_eval_whnf(i, res_v);
            i->sp = res_base_s; // free scratch_s slot to be collected by gc 
            return res_v;
        }
        default:
            break;
    }
    return x;
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
