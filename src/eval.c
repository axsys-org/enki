#include <stddef.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/value.h"

static void run_until_restored(enki_interpreter* i, size_t fp_s, enki_law* law)
{
    while (!i->halted && (i->fp > fp_s || i->law != law)) {
        enki_step(i);
    }
}

enki_value enki_eval_whnf(enki_interpreter* i, enki_value x)
{
    if (!IS_PTR(x)) {
        return x;
    }

    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    switch (h->kind_b) {
        case ENKI_NAT:
            h->state_b = NF;
            return x;
        case ENKI_LAW:
        case ENKI_PIN:
            h->state_b = WHNF;
            return x;
        case ENKI_APP: {
            if (h->state_b == NF || h->state_b == WHNF) {
                return x;
            }

            enki_app* app = (enki_app*)h;
            if (enki_arity(app->fn_v) > app->n_args_s) {
                return x;
            }

            size_t base_fp_s = i->fp;
            enki_law* base_law = i->law;
            bool was_halted = i->halted;
            size_t res_base_s = i->sp;

            i->stack_v[i->sp++] = app->fn_v;
            for (size_t k = 0; k < app->n_args_s; k++) {
                i->stack_v[i->sp++] = app->args_v[k];
            }

            enki_apply(i, app->n_args_s);
            run_until_restored(i, base_fp_s, base_law);

            enki_value res_v = i->stack_v[res_base_s];
            i->sp = res_base_s + 1;
            res_v = enki_eval_whnf(i, res_v);
            i->sp = res_base_s;
            if (base_law == NULL) {
                i->halted = was_halted;
            }
            return res_v;
        }
        default:
            return x;
    }
}

enki_value enki_eval_nf(enki_interpreter* i, enki_value x)
{
    x = enki_eval_whnf(i, x);
    if (!IS_PTR(x)) {
        return x;
    }

    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    if (h->state_b == NF) {
        return x;
    }

    switch (h->kind_b) {
        case ENKI_NAT:
            h->state_b = NF;
            return x;
        case ENKI_PIN: {
            enki_pin* pin = (enki_pin*)ENKI_TO_PTR(x);
            pin->inner_v = enki_eval_nf(i, pin->inner_v);
            for (size_t k = 0; k < pin->n_subpins_s; k++) {
                pin->subpins_v[k] = enki_eval_nf(i, pin->subpins_v[k]);
            }
            h->state_b = NF;
            return x;
        }
        case ENKI_LAW: {
            enki_law* law = (enki_law*)ENKI_TO_PTR(x);
            law->name_v = enki_eval_nf(i, law->name_v);
            law->body_v = enki_eval_nf(i, law->body_v);
            for (size_t k = 0; k < law->n_const_s; k++) {
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
            return x;
    }
}

void enki_upd_lets(enki_interpreter* i, enki_value* let_v, size_t let_s)
{
    for (size_t j = 0; j < let_s; j++) {
        let_v[j] = enki_eval_whnf(i, let_v[j]);
    }
}
