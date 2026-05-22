#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/value.h"

void op66_inc(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_inc(i->gc, a);
}
void op66_dec(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_dec(i->gc, a);
}
void op66_add(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_add(i->gc, a, b);
}
void op66_sub(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_sub(i->gc, a, b);
}
void op66_mul(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_mul(i->gc, a, b);
}
void op66_div(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_div(i->gc, a, b);
}
void op66_mod(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_mod(i->gc, a, b);
}
enki_value op66_structural_eq(enki_value a, enki_value b) {
    if(!IS_PTR(a) && !IS_PTR(b)) {
        return (a == b ? (enki_value)1 : (enki_value)0);
    }
    enki_value_header* h_a = IS_PTR(a) ? (enki_value_header*)ENKI_TO_PTR(a) : NULL;
    enki_value_header* h_b = IS_PTR(b) ? (enki_value_header*)ENKI_TO_PTR(b) : NULL;

    if(IS_PTR(a) && !IS_PTR(b)) {
        if(h_a->kind_b == ENKI_NAT) return enki_nat_eq(a, b);
        return (enki_value)0;
    }
    else if(!IS_PTR(a) && IS_PTR(b)) {
        if(h_b->kind_b == ENKI_NAT) return enki_nat_eq(a, b);
        return (enki_value)0;
    }
    else if(IS_PTR(a) && IS_PTR(b)) {
        if(h_a->kind_b == ENKI_NAT && h_b->kind_b == ENKI_NAT) {
            return enki_nat_eq(a, b);
        }
    }
    if(h_b->kind_b != h_a->kind_b) return (enki_value)0;
    if(h_a->kind_b == ENKI_PIN) {
        enki_pin* pin_a = (enki_pin*)ENKI_TO_PTR(a);
        enki_pin* pin_b = (enki_pin*)ENKI_TO_PTR(b);
        if(pin_a->n_subpins_s != pin_b->n_subpins_s) return (enki_value)0;
        for(size_t k = 0; k < pin_a->n_subpins_s; k++) {
            if(!op66_structural_eq(pin_a->subpins_v[k], pin_b->subpins_v[k])) {
                return (enki_value)0;
            }
        }
        /*
            LATER:
            if both pins are frozen:
                compare hash_b bytes_s
            else:
                force/freeze or structural compare temporarily
        */
        return (op66_structural_eq(pin_a->inner_v, pin_b->inner_v)) ? (enki_value)1 : (enki_value)0;
    }
    else if(h_a->kind_b == ENKI_LAW) {
        enki_law* law_a = (enki_law*)ENKI_TO_PTR(a);
        enki_law* law_b = (enki_law*)ENKI_TO_PTR(b);
        return (op66_structural_eq(law_a->name_v, law_b->name_v) &&
                op66_structural_eq(law_a->body_v, law_b->body_v) &&
                (law_b->arity_s == law_a->arity_s))
        ? (enki_value)1
        : (enki_value)0;
    }
    else if(h_a->kind_b == ENKI_APP) {
        enki_app* app_a = (enki_app*)ENKI_TO_PTR(a);
        enki_app* app_b = (enki_app*)ENKI_TO_PTR(b);
        if(app_a->n_args_s != app_b->n_args_s) return (enki_value)0;
        for(size_t k = 0; k < app_a->n_args_s; k++) {
            if(!op66_structural_eq(app_a->args_v[k], app_b->args_v[k])) {
                return (enki_value)0;
            }
        }
        return (op66_structural_eq(app_a->fn_v, app_b->fn_v)) ? (enki_value)1 : (enki_value)0;
    }
    else return (enki_value)0;
}
void op66_eq(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    a = enki_eval_nf(i, a);
    b = enki_eval_nf(i, b);
    i->sp--;
    i->stack_v[i->sp - 1] = op66_structural_eq(a, b);
}
void op66_ne(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_ne(a, b);
}
void op66_gt(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_gt(a, b);
}
void op66_ge(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_ge(a, b);
}
void op66_lt(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_lt(a, b);
}
void op66_le(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_le(a, b);
}
void op66_cmp(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value b = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = (enki_value)enki_nat_cmp(a, b);
}
void op66_lsh(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value bits = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_lsh(i->gc, a, bits);
}
void op66_rsh(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 2];
    enki_value bits = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_rsh(i->gc, a, bits);
}
void op66_test(enki_interpreter* i) {
    enki_value bit = i->stack_v[i->sp - 2];
    enki_value a = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_test(i->gc, bit, a);
}
void op66_set(enki_interpreter* i) {
    enki_value bit = i->stack_v[i->sp - 2];
    enki_value a = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_set(i->gc, bit, a);
}
void op66_clear(enki_interpreter* i) {
    enki_value bit = i->stack_v[i->sp - 2];
    enki_value a = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_clear(i->gc, bit, a);
}
void op66_bex(enki_interpreter* i) {
    enki_value bit = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_bex(i->gc, bit);
}
void op66_bits(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_bits(i->gc, a);
}
void op66_bytes(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_bytes(i->gc, a);
}
void op66_nib(enki_interpreter* i) {
    enki_value index_i = i->stack_v[i->sp - 2];
    enki_value a = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_nib(i->gc, index_i, a);
}
void op66_load8(enki_interpreter* i) {
    enki_value index_i = i->stack_v[i->sp - 2];
    enki_value a = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_load8(i->gc, index_i, a);
}
void op66_store8(enki_interpreter* i) {
    enki_value index_i = i->stack_v[i->sp - 3];
    enki_value byte = i->stack_v[i->sp - 2];
    enki_value a = i->stack_v[i->sp - 1];
    i->sp-=2;
    i->stack_v[i->sp - 1] = enki_nat_store8(i->gc, index_i, byte, a);
}
void op66_trunc(enki_interpreter* i) {
    enki_value width = i->stack_v[i->sp - 2];
    enki_value a = i->stack_v[i->sp - 1];
    i->sp--;
    i->stack_v[i->sp - 1] = enki_nat_trunc(i->gc, width, a);
}
void op66_trunc8(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_trunc8(i->gc, a);
}
void op66_trunc16(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_trunc16(i->gc, a);
}
void op66_trunc32(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_trunc32(i->gc, a);
}
void op66_trunc64(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = enki_nat_trunc64(i->gc, a);
}

void op66_type(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    uint8_t res_v = 0;
    if(!IS_PTR(x)) {
        res_v = 0;
    }
    else {
        enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
        switch (h->kind_b) {
            case ENKI_NAT:
                res_v = 0; break;
            case ENKI_PIN:
                res_v = 1; break;
            case ENKI_LAW:
                res_v = 2; break;
            case ENKI_APP:
                res_v = 3; break;
            default:
                exit(1); break;
        }
    }
    i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_pin(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res_v = 0;
    if(h->kind_b == ENKI_PIN) res_v = 1;
    i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_law(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res_v = 0;
    if(h->kind_b == ENKI_LAW) res_v = 1;
    i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_app(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res_v = 0;
    if(h->kind_b == ENKI_APP) res_v = 1;
    i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_nat(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)1;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res_v = 0;
    if(h->kind_b == ENKI_NAT) res_v = 1;
    i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_nat(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)x;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = 0;
    if(h->kind_b == ENKI_NAT) res_v = x;
    i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_unpin(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = 0;
    if(h->kind_b == ENKI_PIN) {
        enki_pin* pin = (enki_pin*)ENKI_TO_PTR(x);
        res_v = pin->inner_v;
    }
    i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_name(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = 0;
    if(h->kind_b == ENKI_LAW) {
        enki_law* law = (enki_law*)ENKI_TO_PTR(x);
        res_v = law->name_v;
    }
    i->stack_v[i->sp - 1] = res_v;
}
void op66_body(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = 0;
    if(h->kind_b == ENKI_LAW) {
        enki_law* law = (enki_law*)ENKI_TO_PTR(x);
        res_v = law->body_v;
    }
    i->stack_v[i->sp - 1] = res_v;
}
void op66_arity(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = 0;
    if(h->kind_b == ENKI_LAW) {
        enki_law* law = (enki_law*)ENKI_TO_PTR(x);
        res_v = (enki_value)law->arity_s;
    }
    i->stack_v[i->sp - 1] = res_v;
}
void op66_hd(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = x;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = x;
    if(h->kind_b == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        res_v = app->fn_v;
    }
    i->stack_v[i->sp - 1] = res_v;
}
void op66_last(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = (enki_value)0;
    if(h->kind_b == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(app->n_args_s == 0) res_v = 0;
        else res_v = app->args_v[app->n_args_s - 1];
    }
    i->stack_v[i->sp - 1] = res_v;
}
void op66_init(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = (enki_value)0;
    if(h->kind_b == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(app->n_args_s == 1) {
            res_v = app->fn_v;
        }
        else if(app->n_args_s > 1) {
            size_t n_args_s = app->n_args_s - 1;
            enki_value fn_v = app->fn_v;
            enki_value new = enki_alloc_app(i->gc, fn_v, n_args_s);
            x = i->stack_v[i->sp - 1];
            app = (enki_app*)ENKI_TO_PTR(x);
            enki_app* new_app = (enki_app*)ENKI_TO_PTR(new);
            memcpy(new_app->args_v, app->args_v, n_args_s * sizeof(enki_value));
            res_v = new;
        }
    }
    i->stack_v[i->sp - 1] = res_v;
}
void op66_sz(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = (enki_value)0;
    if(h->kind_b == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        res_v = app->n_args_s;
    }
    i->stack_v[i->sp - 1] = res_v;
}

enki_value ix_at(enki_value i, enki_value x) {
    if(!IS_PTR(x) || IS_PTR(i)) {
        return (enki_value)0;
    }
    enki_value res_v = (enki_value)0;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    if(h->kind_b == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(i >= app->n_args_s) res_v = 0;
        else res_v = app->args_v[i];
    }
    return res_v;
}
void op66_ix(enki_interpreter* interp) {
    enki_value idx_i = interp->stack_v[interp->sp - 2];
    enki_value x = interp->stack_v[interp->sp - 1];
    interp->sp--;
    interp->stack_v[interp->sp - 1] = ix_at(idx_i, x);
}
void op66_ix0(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)0, x);
}

void op66_ix1(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)1, x);
}

void op66_ix2(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)2, x);
}

void op66_ix3(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)3, x);
}

void op66_ix4(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)4, x);
}

void op66_ix5(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)5, x);
}

void op66_ix6(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)6, x);
}

void op66_ix7(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = ix_at((enki_value)7, x);
}
void op66_row(enki_interpreter* i) {
    enki_value h = i->stack_v[i->sp - 3];
    enki_value n = i->stack_v[i->sp - 2];
    enki_value xs = i->stack_v[i->sp - 1];
    if(IS_PTR(n)) exit(1);
    enki_value app = enki_alloc_app(i->gc, h, n);
    enki_app* ptr = ENKI_TO_PTR(app);
    enki_value curr = xs;
    for (size_t k = 0; k < n; k++) {
        ptr->args_v[k] = ix_at((enki_value)0, curr);
        curr = ix_at((enki_value)1, curr);
    }
    i->sp-=2;
    i->stack_v[i->sp - 1] = app;
}
void op66_rep(enki_interpreter* i) {
    enki_value h = i->stack_v[i->sp - 3];
    enki_value x = i->stack_v[i->sp - 2];
    enki_value n = i->stack_v[i->sp - 1];
    if(IS_PTR(n)) exit(1);
    enki_value app = enki_alloc_app(i->gc, h, n);
    enki_app* ptr = ENKI_TO_PTR(app);
    for (size_t k = 0; k < n; k++) {
        ptr->args_v[k] = x;
    }
    i->sp -= 2;
    i->stack_v[i->sp - 1] = app;
}
void op66_slice(enki_interpreter* i) {
    enki_value o = i->stack_v[i->sp - 3];
    enki_value n = i->stack_v[i->sp - 2];
    enki_value x = i->stack_v[i->sp - 1];
    if(IS_PTR(o) || IS_PTR(n) || !IS_PTR(x)) {
        i->sp -= 2;
        i->stack_v[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res_v = 0;
    if(h->kind_b == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(o < app->n_args_s) {
            size_t rsz = app->n_args_s - o;
            if(n < rsz) rsz = n;
            if(rsz != 0) {
                enki_value src_args[rsz];
                for(size_t k = 0; k < rsz; k++) {
                    src_args[k] = app->args_v[o + k];
                }
                enki_value new = enki_alloc_app(i->gc, (enki_value)0, rsz);
                enki_app* new_app = (enki_app*)ENKI_TO_PTR(new);
                for(size_t k = 0; k < rsz; k++) {
                    new_app->args_v[k] = src_args[k];
                }
                res_v = new;
            }
        }
    }
    i->sp -= 2;
    i->stack_v[i->sp - 1] = (enki_value)res_v;

}
static enki_value* args_v(enki_value a) {
    if(!IS_PTR(a)) return NULL;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(a);
    if(h->kind_b == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(a);
        return app->args_v;
    }
    return NULL;
}

void op66_weld(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 2];
    enki_value y = i->stack_v[i->sp - 1];
    enki_value* x_args = args_v(x);
    enki_value* y_args = args_v(y);
    enki_value res_v;
    if(x_args == NULL && y_args == NULL) res_v = (enki_value)0;
    else {
        size_t x_c = 0;
        if(x_args != NULL) x_c = ((enki_app*)ENKI_TO_PTR(x))->n_args_s;
        size_t y_c = 0;
        if(y_args != NULL) y_c = ((enki_app*)ENKI_TO_PTR(y))->n_args_s;
        size_t n_args_s = (x_c + y_c);
        res_v = enki_alloc_app(i->gc, (enki_value)0, n_args_s);
        enki_app* ptr = (enki_app*)ENKI_TO_PTR(res_v);
        x = i->stack_v[i->sp - 2];
        y = i->stack_v[i->sp - 1];
        x_args = args_v(x);
        y_args = args_v(y);
        if(x_args != NULL) {
            for (size_t k = 0; k < x_c; k++) ptr->args_v[k] = x_args[k];
        }
        if(y_args != NULL) {
            for (size_t k = 0; k < y_c; k++) ptr->args_v[x_c + k] = y_args[k];
        }
    }
    i->sp--;
    i->stack_v[i->sp - 1] = res_v;
}
void op66_up(enki_interpreter* i) {
    enki_value idx_i = i->stack_v[i->sp - 3];
    enki_value v = i->stack_v[i->sp - 2];
    enki_value x = i->stack_v[i->sp - 1];
    enki_value res_v = x;
    if(IS_PTR(x) && !IS_PTR(idx_i)) {
        enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
        if(h->kind_b == ENKI_APP) {
            enki_app* app = (enki_app*)ENKI_TO_PTR(x);
            if(idx_i < app->n_args_s) {
                size_t n_args_s = app->n_args_s;
                enki_value fn_v = app->fn_v;
                size_t scratch_s = i->sp;
                for(size_t k = 0; k < n_args_s; k++) {
                    i->stack_v[i->sp] = app->args_v[k];
                    i->sp++;
                }
                res_v = enki_alloc_app(i->gc, fn_v, n_args_s);
                enki_app* new = (enki_app*)ENKI_TO_PTR(res_v);
                for(size_t k = 0; k < n_args_s; k++) {
                    new->args_v[k] = i->stack_v[scratch_s + k];
                }
                new->args_v[idx_i] = v;
                i->sp = scratch_s;
            }
        }
    }
    i->sp -= 2;
    i->stack_v[i->sp - 1] = res_v;
}
void op66_up_uniq(enki_interpreter* i) {
    op66_up(i); // same as UP for now
}
void op66_coup(enki_interpreter* i) {
    enki_value h = i->stack_v[i->sp - 2];
    enki_value x = i->stack_v[i->sp - 1];
    if(!IS_PTR(x)) {
        i->sp--;
        i->stack_v[i->sp - 1] = h;
        return;
    }
    enki_value_header* xh = (enki_value_header*)ENKI_TO_PTR(x);
    if(xh->kind_b != ENKI_APP) {
        i->sp--;
        i->stack_v[i->sp - 1] = h;
        return;
    }
    enki_app* app = (enki_app*)ENKI_TO_PTR(x);
    i->sp--; // remove x leaving h at sp - 1
    for(size_t k = 0; k < app->n_args_s; k++) {
        i->stack_v[i->sp] = app->args_v[k];
        i->sp++;
    }
    enki_apply(i, app->n_args_s);
    return;
}

void op66_case(enki_interpreter* i) {
    enki_value ix = i->stack_v[i->sp - 3];
    enki_value cs = i->stack_v[i->sp - 2];
    enki_value f = i->stack_v[i->sp - 1];
    enki_value res_v = f;

    if(!IS_PTR(ix) && IS_PTR(cs)) {
        enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(cs);
        if(h->kind_b == ENKI_APP) {
            enki_app* app = (enki_app*)ENKI_TO_PTR(cs);
            if(ix < app->n_args_s) {
                res_v = app->args_v[ix];
            }
        }
    }

    i->sp -= 2;
    i->stack_v[i->sp - 1] = res_v;
}

void op66_case_n(enki_interpreter* i, size_t n) {
    enki_value ix = i->stack_v[i->sp - (n + 2)];
    enki_value fallback_v = i->stack_v[i->sp - 1];
    enki_value res_v = fallback_v;

    if(!IS_PTR(ix) && ix < n) {
        res_v = i->stack_v[i->sp - (n + 1) + ix];
    }

    i->sp -= (n + 1);
    i->stack_v[i->sp - 1] = res_v;
}

void op66_case2(enki_interpreter* i) {
    op66_case_n(i, 2);
}

void op66_case3(enki_interpreter* i) {
    op66_case_n(i, 3);
}

void op66_case4(enki_interpreter* i) {
    op66_case_n(i, 4);
}

void op66_case5(enki_interpreter* i) {
    op66_case_n(i, 5);
}

void op66_case6(enki_interpreter* i) {
    op66_case_n(i, 6);
}

void op66_case7(enki_interpreter* i) {
    op66_case_n(i, 7);
}

void op66_case8(enki_interpreter* i) {
    op66_case_n(i, 8);
}

void op66_case9(enki_interpreter* i) {
    op66_case_n(i, 9);
}

void op66_case10(enki_interpreter* i) {
    op66_case_n(i, 10);
}

void op66_case11(enki_interpreter* i) {
    op66_case_n(i, 11);
}

void op66_case12(enki_interpreter* i) {
    op66_case_n(i, 12);
}

void op66_case13(enki_interpreter* i) {
    op66_case_n(i, 13);
}

void op66_case14(enki_interpreter* i) {
    op66_case_n(i, 14);
}

void op66_case15(enki_interpreter* i) {
    op66_case_n(i, 15);
}

void op66_case16(enki_interpreter* i) {
    op66_case_n(i, 16);
}

void op66_nil(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = (x == 0) ? (enki_value)1 : (enki_value)0;
}

void op66_truth(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    i->stack_v[i->sp - 1] = (x == 0) ? (enki_value)0 : (enki_value)1;
}

void op66_or(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 2];
    enki_value y = i->stack_v[i->sp - 1];
    enki_value res_v = (x == 0) ? y : x;

    i->sp--;
    i->stack_v[i->sp - 1] = res_v;
}

void op66_nor(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 2];
    enki_value y = i->stack_v[i->sp - 1];
    enki_value res_v = (x != 0) ? (enki_value)0 : ((y == 0) ? (enki_value)1 : (enki_value)0);

    i->sp--;
    i->stack_v[i->sp - 1] = res_v;
}

void op66_and(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 2];
    enki_value y = i->stack_v[i->sp - 1];
    enki_value res_v = (x == 0) ? (enki_value)0 : y;

    i->sp--;
    i->stack_v[i->sp - 1] = res_v;
}

void op66_if(enki_interpreter* i) {
    enki_value c = i->stack_v[i->sp - 3];
    enki_value t = i->stack_v[i->sp - 2];
    enki_value e = i->stack_v[i->sp - 1];
    enki_value res_v = (c != 0) ? t : e;

    i->sp -= 2;
    i->stack_v[i->sp - 1] = res_v;
}

void op66_ifz(enki_interpreter* i) {
    enki_value c = i->stack_v[i->sp - 3];
    enki_value t = i->stack_v[i->sp - 2];
    enki_value e = i->stack_v[i->sp - 1];
    enki_value res_v = (c == 0) ? t : e;

    i->sp -= 2;
    i->stack_v[i->sp - 1] = res_v;
}

void op66_seq(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 2];
    enki_value y = i->stack_v[i->sp - 1];

    x = enki_eval_whnf(i, x);

    i->sp--;
    i->stack_v[i->sp - 1] = y;
}

void op66_seq2(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 3];
    enki_value y = i->stack_v[i->sp - 2];
    enki_value z = i->stack_v[i->sp - 1];

    x = enki_eval_whnf(i, x);
    y = enki_eval_whnf(i, y);

    i->sp -= 2;
    i->stack_v[i->sp - 1] = z;
}

void op66_seq3(enki_interpreter* i) {
    enki_value a = i->stack_v[i->sp - 4];
    enki_value b = i->stack_v[i->sp - 3];
    enki_value c = i->stack_v[i->sp - 2];
    enki_value d = i->stack_v[i->sp - 1];

    a = enki_eval_whnf(i, a);
    b = enki_eval_whnf(i, b);
    c = enki_eval_whnf(i, c);

    i->sp -= 3;
    i->stack_v[i->sp - 1] = d;
}

static enki_value op66_apply_whnf(enki_interpreter* i, size_t n_args_s) {
    size_t base_fp_s = i->fp;
    enki_law* base_law = i->law;
    size_t res_base_s = i->sp - (n_args_s + 1);

    enki_apply(i, n_args_s);
    while((i->fp > base_fp_s || i->law != base_law) && !i->halted) {
        enki_step(i);
    }

    enki_value result_v = i->stack_v[res_base_s];
    i->sp = res_base_s + 1;
    result_v = enki_eval_whnf(i, result_v);
    i->stack_v[res_base_s] = result_v;
    i->sp = res_base_s + 1;
    return result_v;
}

void op66_sap(enki_interpreter* i) {
    enki_value f = i->stack_v[i->sp - 2];
    enki_value x = i->stack_v[i->sp - 1];

    x = enki_eval_whnf(i, x);

    i->stack_v[i->sp - 2] = f;
    i->stack_v[i->sp - 1] = x;

    op66_apply_whnf(i, 1);
}

void op66_sap2(enki_interpreter* i) {
    enki_value f = i->stack_v[i->sp - 3];
    enki_value x = i->stack_v[i->sp - 2];
    enki_value y = i->stack_v[i->sp - 1];

    x = enki_eval_whnf(i, x);
    y = enki_eval_whnf(i, y);

    i->sp--;
    i->stack_v[i->sp - 2] = f;
    i->stack_v[i->sp - 1] = x;

    op66_apply_whnf(i, 1);
    i->stack_v[i->sp] = y;
    i->sp++;

    op66_apply_whnf(i, 1);
}

void op66_force(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];

    x = enki_eval_nf(i, x);

    i->stack_v[i->sp - 1] = x;
}

void op66_deepseq(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 2];
    enki_value y = i->stack_v[i->sp - 1];

    x = enki_eval_nf(i, x);

    i->sp--;
    i->stack_v[i->sp - 1] = y;
}
