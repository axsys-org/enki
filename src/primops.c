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
    enki_value inner_v = i->stack_v[i->sp - 1];
    /* TODO: TEMPORARY */
    enki_value* subpins_v = NULL; 
    size_t n_subpins_s = 0;
    uint8_t hash_b[32];
    memset(hash_b, 0, 32); 
    ///
    enki_value pin = enki_alloc_pin(i->gc, hash_b, inner_v, n_subpins_s, subpins_v);
    i->stack_v[i->sp - 1] = pin;
}

void primop_mklaw(enki_interpreter* i) {
    enki_value arity_s = i->stack_v[i->sp - 3];
    enki_value name_v = i->stack_v[i->sp - 2];
    enki_value body_v = i->stack_v[i->sp - 1];
    enki_vector* bc_v = enki_vector_create_sized(i->sys_a, sizeof(uint8_t));
    enki_vector* const_v = enki_vector_create_sized(i->sys_a, sizeof(enki_value));
    enki_compile_law(body_v, (size_t)arity_s, bc_v, const_v);
    size_t bc_len_s = enki_vector_len(bc_v);
    uint8_t* bc_b = (uint8_t*)enki_vector_data(bc_v);
    size_t n_const_s = enki_vector_len(const_v);
    enki_value* const_table_v = (enki_value*)enki_vector_data(const_v);
    enki_value law = enki_alloc_law(i->gc, (size_t)arity_s, name_v, body_v, bc_len_s, n_const_s, 
        bc_b, const_table_v);
    enki_vector_destroy(bc_v);
    enki_vector_destroy(const_v);
    i->sp -= 2;
    i->stack_v[i->sp - 1] = law; 
}

void primop_match(enki_interpreter* i) {
    enki_value o = i->stack_v[i->sp - 6];
    enki_value p = i->stack_v[i->sp - 5];
    enki_value l = i->stack_v[i->sp - 4];
    enki_value a = i->stack_v[i->sp - 3];
    enki_value z = i->stack_v[i->sp - 2];
    enki_value n = i->stack_v[i->sp - 1];
    
    if(!IS_PTR(o)) {
        if(o == 0) {
            i->sp -= 5;
            i->stack_v[i->sp - 1] = z;
            return;
        }
        i->sp -= 4;
        i->stack_v[i->sp - 1] = o - 1;
        i->stack_v[i->sp - 2] = n;
        enki_apply(i, 1);
        return;
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(o); 
    switch(h->kind_b) {
        case ENKI_PIN: {
            enki_pin* pin = ENKI_TO_PTR(o);
            i->sp -= 3;
            i->stack_v[i->sp - 1] = pin->inner_v;
            i->stack_v[i->sp - 2] = p;
            enki_apply(i, 1);
            return;
        }
        case ENKI_APP: {
            enki_app* app = ENKI_TO_PTR(o);
            i->sp -= 5;
            i->stack_v[i->sp++] = a;
            i->stack_v[i->sp++] = app->fn_v;
            for(size_t k = 0; k < app->n_args_s; k++) {
                i->stack_v[i->sp] = app->args_v[k];
                i->sp++;
            }
            enki_apply(i, app->n_args_s + 1);
            return;
        }
        case ENKI_LAW: {
            enki_law* law = ENKI_TO_PTR(o);
            i->sp -= 5;
            i->stack_v[i->sp - 1] = law->body_v;
            i->stack_v[i->sp - 2] = law->name_v;
            i->stack_v[i->sp - 3] = (enki_value)law->arity_s;
            i->stack_v[i->sp - 4] = l;
            enki_apply(i, 3);
            return;
        }
        default: 
            return;
    }
}
