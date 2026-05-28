#include "enki/op0.h"
void op0_pin(enki_interpreter* i) {
    enki_value inner_v = i->stack_v[i->sp - 1];
    enki_vector* out_subpins_v = enki_vector_create_sized_or_throw(i,
        enki_arena_as_allocator(i->scratch_a), sizeof(enki_value));
    enki_pin_collect_subpins(i, inner_v, out_subpins_v);
    size_t n_subpins_s = enki_vector_len(out_subpins_v);
    enki_value* subpins_v = (enki_value*)enki_vector_data(out_subpins_v);
    uint8_t hash_b[32] = {0};
    enki_value pin = enki_pin_alloc(i->gc, hash_b, inner_v, n_subpins_s, subpins_v);
    i->stack_v[i->sp - 1] = pin;
}

void op0_law(enki_interpreter* i) {
    enki_value arity_s = i->stack_v[i->sp - 3];
    enki_value name_v = i->stack_v[i->sp - 2];
    enki_value body_v = i->stack_v[i->sp - 1];
    enki_vector* bc_v = enki_vector_create_sized_or_throw(i,
        enki_arena_as_allocator(i->scratch_a), sizeof(uint8_t));
    enki_vector* const_v = enki_vector_create_sized_or_throw(i,
        enki_arena_as_allocator(i->scratch_a), sizeof(enki_value));
    enki_law_compile(i, body_v, (size_t)arity_s, bc_v, const_v);
    size_t bc_len_s = enki_vector_len(bc_v);
    uint8_t* bc_b = (uint8_t*)enki_vector_data(bc_v);
    size_t n_const_s = enki_vector_len(const_v);
    enki_value* const_table_v = (enki_value*)enki_vector_data(const_v);
    enki_value law = enki_law_alloc(i->gc, (size_t)arity_s, name_v, body_v, bc_len_s, n_const_s, 
        bc_b, const_table_v);
    i->sp -= 2;
    i->stack_v[i->sp - 1] = law; 
}

void op0_elim(enki_interpreter* i) {
    enki_value o = i->stack_v[i->sp - 6];
    enki_value p = i->stack_v[i->sp - 5];
    enki_value l = i->stack_v[i->sp - 4];
    enki_value a = i->stack_v[i->sp - 3];
    enki_value z = i->stack_v[i->sp - 2];
    enki_value n = i->stack_v[i->sp - 1];
    o = enki_value_unind(o);
    
    if(!IS_PTR(o)) {
        if(o == 0) {
            i->sp -= 5;
            i->stack_v[i->sp - 1] = z;
            return;
        }
        i->sp -= 4;
        i->stack_v[i->sp - 1] = o - 1;
        i->stack_v[i->sp - 2] = n;
        enki_app_apply(i, 1);
        return;
    }
    enki_value_header* h = ENKI_AS(enki_value_header, o); 
    switch(h->kind_b) {
        case ENKI_PIN: {
            enki_pin* pin = ENKI_AS(enki_pin, o);
            i->sp -= 3;
            i->stack_v[i->sp - 1] = pin->inner_v;
            i->stack_v[i->sp - 2] = p;
            enki_app_apply(i, 1);
            return;
        }
        case ENKI_APP: {
            enki_app* app = ENKI_AS(enki_app, o);
            i->sp -= 5;
            i->stack_v[i->sp++] = a;
            i->stack_v[i->sp++] = app->fn_v;
            for(size_t k = 0; k < app->n_args_s; k++) {
                i->stack_v[i->sp] = app->args_v[k];
                i->sp++;
            }
            enki_app_apply(i, app->n_args_s + 1);
            return;
        }
        case ENKI_LAW: {
            enki_law* law = ENKI_AS(enki_law, o);
            i->sp -= 5;
            i->stack_v[i->sp - 1] = law->body_v;
            i->stack_v[i->sp - 2] = law->name_v;
            i->stack_v[i->sp - 3] = (enki_value)law->arity_s;
            i->stack_v[i->sp - 4] = l;
            enki_app_apply(i, 3);
            return;
        }
        default: 
            enki_interp_throw(i, ENKI_ERROR_BAD_TAG, o);
    }
}
