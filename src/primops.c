#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "enki/apply.h"
#include "enki/compiler.h"
#include "enki/interp.h"
#include "enki/primops.h"
#include "enki/value.h"
#include "enki/vector.h"

void primop_mkpin(enki_interpreter* i) {
    enki_value inner = i->stack[i->sp - 1];
    /* TODO: TEMPORARY */
    enki_value* subpins = NULL; 
    size_t n_subpins = 0;
    uint8_t hash[32];
    memset(hash, 0, 32); 
    ///
    enki_value pin = enki_alloc_pin(i->gc, hash, inner, n_subpins, subpins);
    i->stack[i->sp - 1] = pin;
}

void primop_mklaw(enki_interpreter* i) {
    enki_value arity = i->stack[i->sp - 3];
    enki_value name = i->stack[i->sp - 2];
    enki_value body = i->stack[i->sp - 1];
    enki_vector* bc_v = enki_vector_create_sized(i->sys, sizeof(uint8_t));
    enki_vector* const_v = enki_vector_create_sized(i->sys, sizeof(enki_value));
    enki_compile_law(body, (size_t)arity, bc_v, const_v);
    size_t bc_len = enki_vector_len(bc_v);
    uint8_t* bc = (uint8_t*)enki_vector_data(bc_v);
    size_t n_const = enki_vector_len(const_v);
    enki_value* const_table = (enki_value*)enki_vector_data(const_v);
    enki_value law = enki_alloc_law(i->gc, (size_t)arity, name, body, bc_len, n_const, 
        bc, const_table);
    enki_vector_destroy(bc_v);
    enki_vector_destroy(const_v);
    i->sp -= 2;
    i->stack[i->sp - 1] = law; 
}

void primop_match(enki_interpreter* i) {
    enki_value o = i->stack[i->sp - 6];
    enki_value p = i->stack[i->sp - 5];
    enki_value l = i->stack[i->sp - 4];
    enki_value a = i->stack[i->sp - 3];
    enki_value z = i->stack[i->sp - 2];
    enki_value n = i->stack[i->sp - 1];
    
    if(!IS_PTR(o)) {
        if(o == 0) {
            i->sp -= 5;
            i->stack[i->sp - 1] = z;
            return;
        }
        i->sp -= 4;
        i->stack[i->sp - 1] = o - 1;
        i->stack[i->sp - 2] = n;
        enki_apply(i, 1);
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(o); 
    switch(h->kind) {
        case ENKI_PIN: {
            enki_pin* pin = ENKI_TO_PTR(o);
            i->sp -= 3;
            i->stack[i->sp - 1] = pin->inner;
            i->stack[i->sp - 2] = p;
            enki_apply(i, 1);
            return;
        }
        case ENKI_APP: {
            enki_app* app = ENKI_TO_PTR(o);
            i->sp -= 5;
            i->stack[i->sp++] = a;
            i->stack[i->sp++] = app->fn;
            for(size_t k = 0; k < app->n_args; k++) {
                i->stack[i->sp] = app->args[k];
                i->sp++;
            }
            enki_apply(i, app->n_args + 1);
            return;
        }
        case ENKI_LAW: {
            enki_law* law = ENKI_TO_PTR(o);
            i->sp -= 5;
            i->stack[i->sp - 1] = law->body;
            i->stack[i->sp - 2] = law->name;
            i->stack[i->sp - 3] = (enki_value)law->arity;
            i->stack[i->sp - 4] = l;
            enki_apply(i, 3);
            return;
        }
        default: 
            return;
    }
}
