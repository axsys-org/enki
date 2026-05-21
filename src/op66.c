#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/value.h"

void op66_inc(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_inc(i->gc, a);
}
void op66_dec(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_dec(i->gc, a);
}
void op66_add(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_add(i->gc, a, b);
}
void op66_sub(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_sub(i->gc, a, b);
}
void op66_mul(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_mul(i->gc, a, b);
}
void op66_div(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_div(i->gc, a, b);
}
void op66_mod(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_mod(i->gc, a, b);
}
enki_value op66_structural_eq(enki_value a, enki_value b) {
    if(!IS_PTR(a) && !IS_PTR(b)) {
        return (a == b ? (enki_value)1 : (enki_value)0);
    }
    enki_value_header* h_a = IS_PTR(a) ? (enki_value_header*)ENKI_TO_PTR(a) : NULL;
    enki_value_header* h_b = IS_PTR(b) ? (enki_value_header*)ENKI_TO_PTR(b) : NULL;

    if(IS_PTR(a) && !IS_PTR(b)) {
        if(h_a->kind == ENKI_NAT) return enki_nat_eq(a, b);
        return (enki_value)0;
    }
    else if(!IS_PTR(a) && IS_PTR(b)) {
        if(h_b->kind == ENKI_NAT) return enki_nat_eq(a, b);
        return (enki_value)0;
    }
    else if(IS_PTR(a) && IS_PTR(b)) {
        if(h_a->kind == ENKI_NAT && h_b->kind == ENKI_NAT) {
            return enki_nat_eq(a, b);
        }
    }
    if(h_b->kind != h_a->kind) return (enki_value)0;
    if(h_a->kind == ENKI_PIN) {
        enki_pin* pin_a = (enki_pin*)ENKI_TO_PTR(a);
        enki_pin* pin_b = (enki_pin*)ENKI_TO_PTR(b);
        if(pin_a->n_subpins != pin_b->n_subpins) return (enki_value)0;
        for(size_t k = 0; k < pin_a->n_subpins; k++) {
            if(!op66_structural_eq(pin_a->subpins[k], pin_b->subpins[k])) {
                return (enki_value)0;
            }
        }
        /*
            LATER:
            if both pins are frozen:
                compare hash bytes
            else:
                force/freeze or structural compare temporarily
        */
        return (op66_structural_eq(pin_a->inner, pin_b->inner)) ? (enki_value)1 : (enki_value)0;  
    }
    else if(h_a->kind == ENKI_LAW) {
        enki_law* law_a = (enki_law*)ENKI_TO_PTR(a);
        enki_law* law_b = (enki_law*)ENKI_TO_PTR(b);
        return (op66_structural_eq(law_a->name, law_b->name) &&
                op66_structural_eq(law_a->body, law_b->body) &&
                (law_b->arity == law_a->arity))
        ? (enki_value)1
        : (enki_value)0;        
    }
    else if(h_a->kind == ENKI_APP) {
        enki_app* app_a = (enki_app*)ENKI_TO_PTR(a);
        enki_app* app_b = (enki_app*)ENKI_TO_PTR(b);
        if(app_a->n_args != app_b->n_args) return (enki_value)0;
        for(size_t k = 0; k < app_a->n_args; k++) {
            if(!op66_structural_eq(app_a->args[k], app_b->args[k])) {
                return (enki_value)0;
            }
        }
        return (op66_structural_eq(app_a->fn, app_b->fn)) ? (enki_value)1 : (enki_value)0;  
    }
    else return (enki_value)0;
}
void op66_eq(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    a = enki_eval_nf(i, a);
    b = enki_eval_nf(i, b);
    i->sp--;
    i->stack[i->sp - 1] = op66_structural_eq(a, b);
}
void op66_ne(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_ne(a, b);
}
void op66_gt(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_gt(a, b);
}
void op66_ge(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_ge(a, b);
}
void op66_lt(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_lt(a, b);
}
void op66_le(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_le(a, b);
}
void op66_cmp(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value b = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = (enki_value)enki_nat_cmp(a, b);
}
void op66_lsh(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value bits = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_lsh(i->gc, a, bits);
}
void op66_rsh(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 2];
    enki_value bits = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_rsh(i->gc, a, bits);
}
void op66_test(enki_interpreter* i) {
    enki_value bit = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_test(i->gc, bit, a);
}
void op66_set(enki_interpreter* i) {
    enki_value bit = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_set(i->gc, bit, a);
}
void op66_clear(enki_interpreter* i) {
    enki_value bit = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_clear(i->gc, bit, a);
}
void op66_bex(enki_interpreter* i) {
    enki_value bit = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_bex(i->gc, bit);
}
void op66_bits(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_bits(i->gc, a);
}
void op66_bytes(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_bytes(i->gc, a);
}
void op66_nib(enki_interpreter* i) {
    enki_value index = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_nib(i->gc, index, a);
}
void op66_load8(enki_interpreter* i) {
    enki_value index = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_load8(i->gc, index, a);
}
void op66_store8(enki_interpreter* i) {
    enki_value index = i->stack[i->sp - 3];
    enki_value byte = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];
    i->sp-=2;
    i->stack[i->sp - 1] = enki_nat_store8(i->gc, index, byte, a);
}
void op66_trunc(enki_interpreter* i) {
    enki_value width = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];
    i->sp--;
    i->stack[i->sp - 1] = enki_nat_trunc(i->gc, width, a);
}
void op66_trunc8(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_trunc8(i->gc, a);
}
void op66_trunc16(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_trunc16(i->gc, a);
}
void op66_trunc32(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_trunc32(i->gc, a);
}
void op66_trunc64(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = enki_nat_trunc64(i->gc, a);
}

void op66_type(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    uint8_t res = 0;
    if(!IS_PTR(x)) {
        res = 0;
    }
    else {
        enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
        switch (h->kind) {
            case ENKI_NAT: 
                res = 0; break;
            case ENKI_PIN: 
                res = 1; break;
            case ENKI_LAW: 
                res = 2; break;
            case ENKI_APP:
                res = 3; break;
            default:
                exit(1); break;
        }
    }
    i->stack[i->sp - 1] = (enki_value)res;
}
void op66_is_pin(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res = 0;
    if(h->kind == ENKI_PIN) res = 1;
    i->stack[i->sp - 1] = (enki_value)res;
}
void op66_is_law(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res = 0;
    if(h->kind == ENKI_LAW) res = 1;
    i->stack[i->sp - 1] = (enki_value)res;
}
void op66_is_app(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res = 0;
    if(h->kind == ENKI_APP) res = 1;
    i->stack[i->sp - 1] = (enki_value)res;
}
void op66_is_nat(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)1;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    size_t res = 0;
    if(h->kind == ENKI_NAT) res = 1;
    i->stack[i->sp - 1] = (enki_value)res;
}
void op66_nat(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)x;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = 0;
    if(h->kind == ENKI_NAT) res = x;
    i->stack[i->sp - 1] = (enki_value)res;
}
void op66_unpin(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = 0;
    if(h->kind == ENKI_PIN) {
        enki_pin* pin = (enki_pin*)ENKI_TO_PTR(x);
        res = pin->inner;
    }
    i->stack[i->sp - 1] = (enki_value)res;
}
void op66_name(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = 0;
    if(h->kind == ENKI_LAW) {
        enki_law* law = (enki_law*)ENKI_TO_PTR(x);
        res = law->name;
    }
    i->stack[i->sp - 1] = res;
}
void op66_body(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = 0;
    if(h->kind == ENKI_LAW) {
        enki_law* law = (enki_law*)ENKI_TO_PTR(x);
        res = law->body;
    }
    i->stack[i->sp - 1] = res;
}
void op66_arity(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = 0;
    if(h->kind == ENKI_LAW) {
        enki_law* law = (enki_law*)ENKI_TO_PTR(x);
        res = (enki_value)law->arity;
    }
    i->stack[i->sp - 1] = res;
}
void op66_hd(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = x;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = x;
    if(h->kind == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        res = app->fn;
    }
    i->stack[i->sp - 1] = res;
}
void op66_last(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = (enki_value)0;
    if(h->kind == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(app->n_args == 0) res = 0;
        else res = app->args[app->n_args - 1];
    }
    i->stack[i->sp - 1] = res;
}
void op66_init(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = (enki_value)0;
    if(h->kind == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(app->n_args == 1) {
            res = app->fn;
        }
        else if(app->n_args > 1) {
            size_t n_args = app->n_args - 1;
            enki_value fn = app->fn;
            enki_value new = enki_alloc_app(i->gc, fn, n_args);
            x = i->stack[i->sp - 1];
            app = (enki_app*)ENKI_TO_PTR(x);
            enki_app* new_app = (enki_app*)ENKI_TO_PTR(new);
            memcpy(new_app->args, app->args, n_args * sizeof(enki_value));
            res = new;
        }
    }
    i->stack[i->sp - 1] = res;
}
void op66_sz(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = (enki_value)0;
    if(h->kind == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        res = app->n_args;
    }
    i->stack[i->sp - 1] = res;
}

enki_value ix_at(enki_value i, enki_value x) {
    if(!IS_PTR(x) || IS_PTR(i)) {
        return (enki_value)0;
    }
    enki_value res = (enki_value)0;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    if(h->kind == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(i >= app->n_args) res = 0;
        else res = app->args[i];
    }
    return res;
}
void op66_ix(enki_interpreter* interp) {
    enki_value idx = interp->stack[interp->sp - 2];
    enki_value x = interp->stack[interp->sp - 1];
    interp->sp--;
    interp->stack[interp->sp - 1] = ix_at(idx, x);
}
void op66_ix0(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)0, x);
}

void op66_ix1(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)1, x);
}

void op66_ix2(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)2, x);
}

void op66_ix3(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)3, x);
}

void op66_ix4(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)4, x);
}

void op66_ix5(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)5, x);
}

void op66_ix6(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)6, x);
}

void op66_ix7(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = ix_at((enki_value)7, x);
}
void op66_row(enki_interpreter* i) {
    enki_value h = i->stack[i->sp - 3];
    enki_value n = i->stack[i->sp - 2];
    enki_value xs = i->stack[i->sp - 1];
    if(IS_PTR(n)) exit(1);
    enki_value app = enki_alloc_app(i->gc, h, n);
    enki_app* ptr = ENKI_TO_PTR(app);
    enki_value curr = xs;
    for (size_t k = 0; k < n; k++) {
        ptr->args[k] = ix_at((enki_value)0, curr);
        curr = ix_at((enki_value)1, curr);
    }
    i->sp-=2;
    i->stack[i->sp - 1] = app;
}
void op66_rep(enki_interpreter* i) {
    enki_value h = i->stack[i->sp - 3];
    enki_value x = i->stack[i->sp - 2];
    enki_value n = i->stack[i->sp - 1];
    if(IS_PTR(n)) exit(1);
    enki_value app = enki_alloc_app(i->gc, h, n);
    enki_app* ptr = ENKI_TO_PTR(app);
    for (size_t k = 0; k < n; k++) { 
        ptr->args[k] = x;
    }
    i->sp -= 2;
    i->stack[i->sp - 1] = app;
}
void op66_slice(enki_interpreter* i) {
    enki_value o = i->stack[i->sp - 3];
    enki_value n = i->stack[i->sp - 2];
    enki_value x = i->stack[i->sp - 1];
    if(IS_PTR(o) || IS_PTR(n) || !IS_PTR(x)) {
        i->sp -= 2;
        i->stack[i->sp - 1] = (enki_value)0;
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
    enki_value res = 0;
    if(h->kind == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(x);
        if(o < app->n_args) {
            size_t rsz = app->n_args - o;
            if(n < rsz) rsz = n;
            if(rsz != 0) {
                enki_value src_args[rsz];
                for(size_t k = 0; k < rsz; k++) {
                    src_args[k] = app->args[o + k];
                }
                enki_value new = enki_alloc_app(i->gc, (enki_value)0, rsz);
                enki_app* new_app = (enki_app*)ENKI_TO_PTR(new);
                for(size_t k = 0; k < rsz; k++) {
                    new_app->args[k] = src_args[k];
                }
                res = new; 
            }
        }
    }
    i->sp -= 2;
    i->stack[i->sp - 1] = (enki_value)res;

}
static enki_value* args(enki_value a) {
    if(!IS_PTR(a)) return NULL;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(a);
    if(h->kind == ENKI_APP) {
        enki_app* app = (enki_app*)ENKI_TO_PTR(a);
        return app->args;
    }
    return NULL;
}

void op66_weld(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 2];
    enki_value y = i->stack[i->sp - 1]; 
    enki_value* x_args = args(x);
    enki_value* y_args = args(y);
    enki_value res;
    if(x_args == NULL && y_args == NULL) res = (enki_value)0;
    else {
        size_t x_c = 0;
        if(x_args != NULL) x_c = ((enki_app*)ENKI_TO_PTR(x))->n_args;
        size_t y_c = 0;
        if(y_args != NULL) y_c = ((enki_app*)ENKI_TO_PTR(y))->n_args;
        size_t n_args = (x_c + y_c);
        res = enki_alloc_app(i->gc, (enki_value)0, n_args);
        enki_app* ptr = (enki_app*)ENKI_TO_PTR(res);
        x = i->stack[i->sp - 2];
        y = i->stack[i->sp - 1]; 
        x_args = args(x);
        y_args = args(y);
        if(x_args != NULL) {
            for (size_t k = 0; k < x_c; k++) ptr->args[k] = x_args[k];
        }
        if(y_args != NULL) {
            for (size_t k = 0; k < y_c; k++) ptr->args[x_c + k] = y_args[k];
        }
    }
    i->sp--;
    i->stack[i->sp - 1] = res;
}
void op66_up(enki_interpreter* i) {
    enki_value idx = i->stack[i->sp - 3];
    enki_value v = i->stack[i->sp - 2];
    enki_value x = i->stack[i->sp - 1];
    enki_value res = x;
    if(IS_PTR(x) && !IS_PTR(idx)) {
        enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(x);
        if(h->kind == ENKI_APP) {
            enki_app* app = (enki_app*)ENKI_TO_PTR(x);
            if(idx < app->n_args) {
                size_t n_args = app->n_args;
                enki_value fn = app->fn;
                size_t scratch = i->sp;
                for(size_t k = 0; k < n_args; k++) {
                    i->stack[i->sp] = app->args[k];
                    i->sp++;
                }
                res = enki_alloc_app(i->gc, fn, n_args);
                enki_app* new = (enki_app*)ENKI_TO_PTR(res);
                for(size_t k = 0; k < n_args; k++) {
                    new->args[k] = i->stack[scratch + k];
                }
                new->args[idx] = v;
                i->sp = scratch;
            }
        } 
    }
    i->sp -= 2;
    i->stack[i->sp - 1] = res;
}
void op66_up_uniq(enki_interpreter* i) {
    op66_up(i); // same as UP for now 
}
void op66_coup(enki_interpreter* i) {
    enki_value h = i->stack[i->sp - 2];
    enki_value x = i->stack[i->sp - 1];
    if(!IS_PTR(x)) {
        i->sp--;
        i->stack[i->sp - 1] = h;
        return;
    }
    enki_value_header* xh = (enki_value_header*)ENKI_TO_PTR(x);
    if(xh->kind != ENKI_APP) {
        i->sp--;
        i->stack[i->sp - 1] = h;
        return;
    }
    enki_app* app = (enki_app*)ENKI_TO_PTR(x);
    i->sp--; // remove x leaving h at sp - 1
    for(size_t k = 0; k < app->n_args; k++) {
        i->stack[i->sp] = app->args[k];
        i->sp++;
    }
    enki_apply(i, app->n_args);
    return;
}

void op66_case(enki_interpreter* i) {
    enki_value ix = i->stack[i->sp - 3];
    enki_value cs = i->stack[i->sp - 2];
    enki_value f = i->stack[i->sp - 1];
    enki_value res = f;

    if(!IS_PTR(ix) && IS_PTR(cs)) {
        enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(cs);
        if(h->kind == ENKI_APP) {
            enki_app* app = (enki_app*)ENKI_TO_PTR(cs);
            if(ix < app->n_args) {
                res = app->args[ix];
            }
        }
    }

    i->sp -= 2;
    i->stack[i->sp - 1] = res;
}

void op66_case_n(enki_interpreter* i, size_t n) {
    enki_value ix = i->stack[i->sp - (n + 2)];
    enki_value fallback = i->stack[i->sp - 1];
    enki_value res = fallback;

    if(!IS_PTR(ix) && ix < n) {
        res = i->stack[i->sp - (n + 1) + ix];
    }

    i->sp -= (n + 1);
    i->stack[i->sp - 1] = res;
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
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = (x == 0) ? (enki_value)1 : (enki_value)0;
}

void op66_truth(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];
    i->stack[i->sp - 1] = (x == 0) ? (enki_value)0 : (enki_value)1;
}

void op66_or(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 2];
    enki_value y = i->stack[i->sp - 1];
    enki_value res = (x == 0) ? y : x;

    i->sp--;
    i->stack[i->sp - 1] = res;
}

void op66_nor(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 2];
    enki_value y = i->stack[i->sp - 1];
    enki_value res = (x != 0) ? (enki_value)0 : ((y == 0) ? (enki_value)1 : (enki_value)0);

    i->sp--;
    i->stack[i->sp - 1] = res;
}

void op66_and(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 2];
    enki_value y = i->stack[i->sp - 1];
    enki_value res = (x == 0) ? (enki_value)0 : y;

    i->sp--;
    i->stack[i->sp - 1] = res;
}

void op66_if(enki_interpreter* i) {
    enki_value c = i->stack[i->sp - 3];
    enki_value t = i->stack[i->sp - 2];
    enki_value e = i->stack[i->sp - 1];
    enki_value res = (c != 0) ? t : e;

    i->sp -= 2;
    i->stack[i->sp - 1] = res;
}

void op66_ifz(enki_interpreter* i) {
    enki_value c = i->stack[i->sp - 3];
    enki_value t = i->stack[i->sp - 2];
    enki_value e = i->stack[i->sp - 1];
    enki_value res = (c == 0) ? t : e;

    i->sp -= 2;
    i->stack[i->sp - 1] = res;
}

void op66_seq(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 2];
    enki_value y = i->stack[i->sp - 1];

    x = enki_eval_whnf(i, x);

    i->sp--;
    i->stack[i->sp - 1] = y;
}

void op66_seq2(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 3];
    enki_value y = i->stack[i->sp - 2];
    enki_value z = i->stack[i->sp - 1];

    x = enki_eval_whnf(i, x);
    y = enki_eval_whnf(i, y);

    i->sp -= 2;
    i->stack[i->sp - 1] = z;
}

void op66_seq3(enki_interpreter* i) {
    enki_value a = i->stack[i->sp - 4];
    enki_value b = i->stack[i->sp - 3];
    enki_value c = i->stack[i->sp - 2];
    enki_value d = i->stack[i->sp - 1];

    a = enki_eval_whnf(i, a);
    b = enki_eval_whnf(i, b);
    c = enki_eval_whnf(i, c);

    i->sp -= 3;
    i->stack[i->sp - 1] = d;
}

static enki_value op66_apply_whnf(enki_interpreter* i, size_t n_args) {
    size_t base_fp = i->fp;
    size_t res_base = i->sp - (n_args + 1);

    enki_apply(i, n_args);
    while(i->fp > base_fp && !i->halted) {
        enki_step(i);
    }

    enki_value result = i->stack[res_base];
    i->sp = res_base + 1;
    result = enki_eval_whnf(i, result);
    i->stack[res_base] = result;
    i->sp = res_base + 1;
    return result;
}

void op66_sap(enki_interpreter* i) {
    enki_value f = i->stack[i->sp - 2];
    enki_value x = i->stack[i->sp - 1];

    x = enki_eval_whnf(i, x);

    i->stack[i->sp - 2] = f;
    i->stack[i->sp - 1] = x;

    op66_apply_whnf(i, 1);
}

void op66_sap2(enki_interpreter* i) {
    enki_value f = i->stack[i->sp - 3];
    enki_value x = i->stack[i->sp - 2];
    enki_value y = i->stack[i->sp - 1];

    x = enki_eval_whnf(i, x);
    y = enki_eval_whnf(i, y);

    i->sp--;
    i->stack[i->sp - 2] = f;
    i->stack[i->sp - 1] = x;

    op66_apply_whnf(i, 1);
    i->stack[i->sp] = y;
    i->sp++;

    op66_apply_whnf(i, 1);
}

void op66_force(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 1];

    x = enki_eval_nf(i, x);

    i->stack[i->sp - 1] = x;
}

void op66_deepseq(enki_interpreter* i) {
    enki_value x = i->stack[i->sp - 2];
    enki_value y = i->stack[i->sp - 1];

    x = enki_eval_nf(i, x);

    i->sp--;
    i->stack[i->sp - 1] = y;
}
