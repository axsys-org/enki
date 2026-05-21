
#include <stddef.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/value.h"


enki_value enki_eval_whnf(enki_interpreter* i, enki_value x) {
    if(!IS_PTR(x)) return x;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    switch(h->kind) {
        case ENKI_NAT:
            h->state = NF;
            return x;
        case ENKI_LAW:
            h->state = WHNF;
            return x;
        case ENKI_PIN: 
            h->state = WHNF;
            return x;
        case ENKI_APP: {
            if(h->state == NF || h->state == WHNF) return x;
            size_t base_fp = i->fp;
            size_t res_base = i->sp;
            i->stack[i->sp] = x;
            i->sp++;
            enki_apply(i, 0);
            while(i->fp > base_fp && !i->halted) {
                enki_step(i);
            }
            enki_value res = i->stack[res_base];
            i->sp = res_base + 1; // keep scratch are rooted
            res = enki_eval_whnf(i, res);
            i->sp = res_base; // free scratch slot to be collected by gc 
            return res;
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
    if (h->state == NF) return x;
    switch(h->kind) {
        case ENKI_NAT:
            h->state = NF;
            return x;
        case ENKI_PIN: {
            enki_pin* pin = (enki_pin*)ENKI_TO_PTR(x);
            pin->inner = enki_eval_nf(i, pin->inner);
            for(size_t k = 0; k < pin->n_subpins; k++) {
                pin->subpins[k] = enki_eval_nf(i, pin->subpins[k]);
            }
            /*  TODO later: 
                hash = sha256(canonize(pin))
                if pin has existing hash:
                    verify hash matches
                else:
                    store hash
            */
            h->state = NF;
            return x;
        }
        case ENKI_LAW: {
            enki_law* law = (enki_law*)ENKI_TO_PTR(x);
            law->body = enki_eval_nf(i, law->body);
            law->name = enki_eval_nf(i, law->name);
            for(size_t k = 0; k < law->n_const; k++) {
                ENKI_LAW_CONSTS(law)[k] = enki_eval_nf(i, ENKI_LAW_CONSTS(law)[k]);
            }
            h->state = NF;
            return x;
        }
        case ENKI_APP: {
          enki_app* app = (enki_app*)ENKI_TO_PTR(x);
          app->fn = enki_eval_nf(i, app->fn);
          for (size_t k = 0; k < app->n_args; k++) {
            app->args[k] = enki_eval_nf(i, app->args[k]);
          }
          h->state = NF;
          return x;
        }
        default:
            break;
    }   
    return x;
}
