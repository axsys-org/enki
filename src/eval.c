
#include <stddef.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/value.h"


enki_value enki_eval_whnf(enki_interpreter* i, enki_value x) {
    if(!IS_PTR(x)) return x;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
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
            enki_apply(i, 0);
            while(i->fp > base_fp_s && !i->halted) {
                enki_step(i);
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
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    if (h->state_b == NF) return x;
    switch(h->kind_b) {
        case ENKI_NAT:
            h->state_b = NF;
            return x;
        case ENKI_PIN: {
            enki_pin* pin = (enki_pin*)ENKI_TO_PTR(x);
            pin->inner_v = enki_eval_nf(i, pin->inner_v);
            for(size_t k = 0; k < pin->n_subpins_s; k++) {
                pin->subpins_v[k] = enki_eval_nf(i, pin->subpins_v[k]);
            }
            /*  TODO later: 
                hash_b = sha256(canonize(pin))
                if pin has existing hash_b:
                    verify hash_b matches
                else:
                    store hash_b
            */
            h->state_b = NF;
            return x;
        }
        case ENKI_LAW: {
            enki_law* law = (enki_law*)ENKI_TO_PTR(x);
            law->body_v = enki_eval_nf(i, law->body_v);
            law->name_v = enki_eval_nf(i, law->name_v);
            for(size_t k = 0; k < law->n_const_s; k++) {
                ENKI_LAW_CONSTS(law)[k] = enki_eval_nf(i, ENKI_LAW_CONSTS(law)[k]);
            }
            h->state_b = NF;
            return x;
        }
        case ENKI_APP: {
          enki_app* app = (enki_app*)ENKI_TO_PTR(x);
          app->fn_v = enki_eval_nf(i, app->fn_v);
          for (size_t k = 0; k < app->n_args_s; k++) {
            app->args_v[k] = enki_eval_nf(i, app->args_v[k]);
          }
          h->state_b = NF;
          return x;
        }
        default:
            break;
    }   
    return x;
}
