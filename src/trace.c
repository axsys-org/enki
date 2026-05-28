#include <string.h>
#include <stdlib.h>

#include "enki/trace.h"
#include "enki/value.h"
#include "enki/gc.h"

void enki_trace_interp(enki_interpreter* i) {
    for(size_t k = 0; k < i->sp; k++) {
        i->stack_v[k] = i->gc->copy(i->gc, i->stack_v[k]);
    } 
    for(size_t k = 0; k < i->cp; k++) {
        if(i->call_stack_v[k].law != 0) {
            i->call_stack_v[k].law = i->gc->copy(i->gc, i->call_stack_v[k].law);
        }
    }
    if(i->error_v != 0) i->error_v = i->gc->copy(i->gc, i->error_v);
    if(i->cp > 0) {
        enki_law* law = ENKI_AS(enki_law, i->call_stack_v[i->cp - 1].law);
        i->bc_b = ENKI_LAW_BC(law);
        i->const_table_v = ENKI_LAW_CONSTS(law);
    }
}

void enki_trace_value(enki_gc* gc, void* obj) {
    enki_value_header* h = obj;
    switch (h->kind_b) {
        case ENKI_PIN: {
          enki_pin* pin = obj;
          pin->inner_v = gc->copy(gc, pin->inner_v);
          for(size_t k = 0; k < pin->n_subpins_s; k++) {
            pin->subpins_v[k] = gc->copy(gc, pin->subpins_v[k]);
          }
          break;
        }
        case ENKI_LAW: {
          enki_law* law = obj;
          law->body_v = gc->copy(gc, law->body_v);
          law->name_v = gc->copy(gc, law->name_v);
          for(size_t k = 0; k < law->n_const_s; k++) {
            ENKI_LAW_CONSTS(law)[k] = gc->copy(gc, ENKI_LAW_CONSTS(law)[k]);
          }
          break;
        }
        case ENKI_APP: {
          enki_app* app = obj;
          app->fn_v = gc->copy(gc, app->fn_v);
          for (size_t k = 0; k < app->n_args_s; k++) {
            app->args_v[k] = gc->copy(gc, app->args_v[k]);
          }
          break;
        }
        case ENKI_CONT: {
          enki_cont* cont_v = obj;
          for (size_t k = 0; k < cont_v->n_args_s; k++) {
            cont_v->args_v[k] = gc->copy(gc, cont_v->args_v[k]);
          }
          break;
        }
        case ENKI_NAT:
          break;
        default:
          abort();
    }
}
