
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/value.h"

static void enki_complete_app(size_t arity_s, size_t n_args_s, 
    size_t fn_index_i, enki_app* app, enki_interpreter* i) {
    i->stack_v[fn_index_i] = app->fn_v;
    size_t off_o = fn_index_i + 1;
    for (size_t k = n_args_s; k > 0; k--) {
        size_t idx_i = k - 1;
        i->stack_v[off_o + idx_i + app->n_args_s] = i->stack_v[off_o + idx_i];
    }
    for(size_t k = 0; k < app->n_args_s; k++) {
        i->stack_v[off_o + k] = app->args_v[k];
    }
    i->sp = fn_index_i + app->n_args_s + n_args_s + 1;
    enki_enter_law(arity_s, app->fn_v, i);
}


void enki_enter_law(size_t arity_s, enki_value val_v, enki_interpreter* i) {
    enki_frame f;
    f.pc = 0;
    f.law = val_v;  
    size_t call_width_s = arity_s + 1; // head_v + all the args_v 
    f.res_base_s = i->sp - call_width_s; 
    f.arg_base_s = f.res_base_s + 1;
    f.cont_v = 0;
    i->fp += 1; // go to next frame and set it
    i->frame[i->fp] = f;
}

size_t enki_arity(enki_value val_v) {
    if(!IS_PTR(val_v)) return 0; 
    enki_value_header* h = ENKI_TO_PTR(val_v);
    switch(h->kind_b) {
        case LAW: {
            enki_law* law = ENKI_TO_PTR(val_v);
            return law->arity_s;
        }
        case APP: {
            enki_app* app = ENKI_TO_PTR(val_v);
            size_t fn_arity = enki_arity(app->fn_v);
            if(fn_arity <= app->n_args_s) return 0;
            return fn_arity - app->n_args_s;
        }
        case PIN:
            return 0; // pins are not transparent
        case NAT:
            return 0;
        default: 
            return 0;
    }
}

static void enki_make_cont(size_t fn_index_i, size_t needed,
    size_t n_args_s, enki_interpreter* i) {
    size_t xt_args_c_s = n_args_s - needed;
    enki_value* bas_v = &i->stack_v[fn_index_i + 1 + needed];
    enki_value cont_v = enki_alloc_cont(i->gc, xt_args_c_s, bas_v);
    enki_frame f;
    i->sp -= xt_args_c_s;
    f.res_base_s = fn_index_i;
    f.pc = 0;
    f.law = 0;
    f.arg_base_s = 0;
    f.cont_v = cont_v;
    i->fp++;
    i->frame[i->fp] = f;
}

static void enki_make_partial_apply(enki_interpreter* i, size_t fn_index_i, enki_value fn_v,
    const enki_value* old_args_v, size_t n_old_args_s, size_t n_new_args_s) {
    enki_value app = enki_alloc_app(i->gc, fn_v, n_old_args_s + n_new_args_s);
    enki_app* ptr = (enki_app*)ENKI_TO_PTR(app);
    if(n_old_args_s > 0 && old_args_v != NULL) {
        memcpy(ptr->args_v, old_args_v, sizeof(enki_value) * n_old_args_s);
    }
    for(size_t k = 0; k < n_new_args_s; k++) {
        ptr->args_v[k + n_old_args_s] = i->stack_v[fn_index_i + 1 + k];
    }
    i->stack_v[fn_index_i] = app; // pop stack_v and set result_v to app
    i->sp = fn_index_i + 1; 
}

void enki_apply(enki_interpreter* i, size_t n_args_s) {
    size_t fn_index_i = i->sp - ((size_t)n_args_s + 1);
    enki_value head_v = i->stack_v[fn_index_i];
    if(!IS_PTR(head_v)) return; // TODO error out 
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(head_v);
    switch(h->kind_b) {
        case LAW: {
            enki_law* law = (enki_law*)ENKI_TO_PTR(head_v);  
            if(law->arity_s == n_args_s) {
                enki_enter_law(n_args_s, head_v, i);
            }
            else if(law->arity_s > n_args_s) {
                enki_make_partial_apply(i, fn_index_i, head_v, NULL, 0, n_args_s);
            }
            else {
                enki_make_cont(fn_index_i, law->arity_s, n_args_s, i);
                enki_enter_law(law->arity_s, head_v, i);
            }
            return;
        }
        case APP: {
             enki_app* app = (enki_app*)ENKI_TO_PTR(head_v);
             size_t arity_s = enki_arity(app->fn_v);
             size_t new_arg_c_s = app->n_args_s + (size_t)n_args_s;
             enki_value_header* fn_h = ENKI_TO_PTR(app->fn_v);
             if(new_arg_c_s == arity_s) {
                switch(fn_h->kind_b) {
                    case LAW:
                        enki_complete_app(arity_s, n_args_s, fn_index_i, app, i);
                        break;
                    default:
                        break;
                }
             }
             else if(new_arg_c_s < arity_s){
                enki_make_partial_apply(i, fn_index_i, app->fn_v, app->args_v, app->n_args_s, n_args_s);
                return;
            }
             else {
                switch(fn_h->kind_b) {
                    case LAW: {
                        size_t needed = arity_s - app->n_args_s;
                        enki_make_cont(fn_index_i, needed, n_args_s, i);
                        enki_complete_app(arity_s, needed, fn_index_i, app, i);
                        break;
                    }
                    default:
                        break;
                }
             }
             return;
        }

        default: 
            return;
    }
}
