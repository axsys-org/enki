
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/value.h"

static void enki_complete_app(size_t arity, size_t n_args, 
    size_t fn_index, enki_app* app, enki_interpreter* i) {
    i->stack[fn_index] = app->fn;
    size_t off = fn_index + 1;
    for (size_t k = n_args; k > 0; k--) {
        size_t idx = k - 1;
        i->stack[off + idx + app->n_args] = i->stack[off + idx];
    }
    for(size_t k = 0; k < app->n_args; k++) {
        i->stack[off + k] = app->args[k];
    }
    i->sp = fn_index + app->n_args + n_args + 1;
    enki_enter_law(arity, app->fn, i);
}


void enki_enter_law(size_t arity, enki_value val, enki_interpreter* i) {
    enki_frame f;
    f.pc = 0;
    f.law = val;  
    size_t call_width = arity + 1; // head + all the args 
    f.res_base = i->sp - call_width; 
    f.arg_base = f.res_base + 1;
    f.cont = 0;
    i->fp += 1; // go to next frame and set it
    i->frame[i->fp] = f;
}

size_t enki_arity(enki_value val) {
    if(!IS_PTR(val)) return 0; 
    enki_value_header* h = ENKI_TO_PTR(val);
    switch(h->kind) {
        case LAW: {
            enki_law* law = ENKI_TO_PTR(val);
            return law->arity;
        }
        case APP: {
            enki_app* app = ENKI_TO_PTR(val);
            size_t fn_arity = enki_arity(app->fn);
            if(fn_arity <= app->n_args) return 0;
            return fn_arity - app->n_args;
        }
        case PIN:
            return 0; // pins are not transparent
        case NAT:
            return 0;
        default: 
            return 0;
    }
}

static void enki_make_cont(size_t fn_index, size_t needed,
    size_t n_args, enki_interpreter* i) {
    size_t xt_args_c = n_args - needed;
    enki_value* bas = &i->stack[fn_index + 1 + needed];
    enki_value cont = enki_alloc_cont(i->gc, xt_args_c, bas);
    enki_frame f;
    i->sp -= xt_args_c;
    f.res_base = fn_index;
    f.pc = 0;
    f.law = 0;
    f.arg_base = 0;
    f.cont = cont;
    i->fp++;
    i->frame[i->fp] = f;
}

static void enki_make_partial_apply(enki_interpreter* i, size_t fn_index, enki_value fn,
    const enki_value* old_args, size_t n_old_args, size_t n_new_args) {
    enki_value app = enki_alloc_app(i->gc, fn, n_old_args + n_new_args);
    enki_app* ptr = (enki_app*)ENKI_TO_PTR(app);
    if(n_old_args > 0 && old_args != NULL) {
        memcpy(ptr->args, old_args, sizeof(enki_value) * n_old_args);
    }
    for(size_t k = 0; k < n_new_args; k++) {
        ptr->args[k + n_old_args] = i->stack[fn_index + 1 + k];
    }
    i->stack[fn_index] = app; // pop stack and set result to app
    i->sp = fn_index + 1; 
}

void enki_apply(enki_interpreter* i, size_t n_args) {
    size_t fn_index = i->sp - ((size_t)n_args + 1);
    enki_value head = i->stack[fn_index];
    if(!IS_PTR(head)) return; // TODO error out 
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(head);
    switch(h->kind) {
        case LAW: {
            enki_law* law = (enki_law*)ENKI_TO_PTR(head);  
            if(law->arity == n_args) {
                enki_enter_law(n_args, head, i);
            }
            else if(law->arity > n_args) {
                enki_make_partial_apply(i, fn_index, head, NULL, 0, n_args);
            }
            else {
                enki_make_cont(fn_index, law->arity, n_args, i);
                enki_enter_law(law->arity, head, i);
            }
            return;
        }
        case APP: {
             enki_app* app = (enki_app*)ENKI_TO_PTR(head);
             size_t arity = enki_arity(app->fn);
             size_t new_arg_c = app->n_args + (size_t)n_args;
             enki_value_header* fn_h = ENKI_TO_PTR(app->fn);
             if(new_arg_c == arity) {
                switch(fn_h->kind) {
                    case LAW:
                        enki_complete_app(arity, n_args, fn_index, app, i);
                        break;
                    default:
                        break;
                }
             }
             else if(new_arg_c < arity){
                enki_make_partial_apply(i, fn_index, app->fn, app->args, app->n_args, n_args);
                return;
            }
             else {
                switch(fn_h->kind) {
                    case LAW: {
                        size_t needed = arity - app->n_args;
                        enki_make_cont(fn_index, needed, n_args, i);
                        enki_complete_app(arity, needed, fn_index, app, i);
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
