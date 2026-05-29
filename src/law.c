#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "enki/app.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/profile.h"
#include "enki/value.h"

#define ENKI_BC_WIDE_MAX   UINT16_MAX

enki_value enki_law_alloc(enki_gc* gc, size_t arity_s, enki_value name_v, enki_value body_v, 
    size_t bc_len_s, size_t n_const_s, uint8_t* bc_b, enki_value* const_table_v) {
    size_t n = sizeof(enki_law) + bc_len_s + (n_const_s * sizeof(enki_value));
    enki_law* new = (enki_law*)enki_gc_alloc_locked(gc, n, _Alignof(enki_law));
    new->h.size_s = n;
    new->h.kind_b = ENKI_LAW;
    new->h.state_b = NF;
    new->body_v = body_v;
    new->name_v = name_v;
    new->arity_s = (uint32_t)arity_s;
    new->bc_len_s = bc_len_s;
    new->n_const_s = n_const_s;
    size_t const_off_o = n_const_s * sizeof(enki_value);
    if(n_const_s > 0) memcpy(new->data_b, const_table_v, const_off_o);
    if(bc_len_s > 0) memcpy(new->data_b + const_off_o, bc_b, bc_len_s);
    return PTR_TO_ENKI(new);
}

void enki_law_enter(size_t arity_s, enki_value val_v, enki_interpreter* i) {
    ENKI_PROFILE_ZONE("enki_law_enter");
    i->stats.law_enter_s++;
    
    if(i->cp > 0) {
        i->call_stack_v[i->cp - 1].pc = i->pc;
        i->call_stack_v[i->cp - 1].arg_base_s = i->arg_base_s;
    }

    enki_call call;
    size_t call_width_s = arity_s + 1; // head_v + all the args_v
    call.pc = 0;
    call.res_base_s = i->sp - call_width_s;
    call.arg_base_s = call.res_base_s + 1;
    call.law = val_v;

    enki_law* law = ENKI_AS(enki_law, val_v); 
    call.bc_b = ENKI_LAW_BC(law);
    call.const_table_v = ENKI_LAW_CONSTS(law);
    i->call_stack_v[i->cp++] = call;

    i->bc_b = call.bc_b;
    i->const_table_v = call.const_table_v;
    i->pc = 0;
    i->arg_base_s = call.arg_base_s;
}

static void enki_law_compile_value(enki_interpreter* i, enki_value body_v, size_t depth_s, enki_vector* bc_b, enki_vector* const_table_v);

static void enki_law_compile_args(enki_interpreter* i, enki_app* app, size_t depth_s, enki_vector* bc_b, enki_vector* const_table_v) {
   for(size_t k = 0; k < app->n_args_s; k++) { 
      enki_law_compile_value(i, app->args_v[k], depth_s, bc_b, const_table_v);
    }
}
static void enki_law_push_const(enki_interpreter* i, enki_vector* bc_b, enki_vector* const_table_v, enki_value val) {
    size_t n_const_s = enki_vector_len(const_table_v);  
    enki_vector_push_copy_or_throw(i, const_table_v, &val);
    if(n_const_s > 255) {
        if(n_const_s > ENKI_BC_WIDE_MAX) abort();
        enki_vector_push_u8_or_throw(i, bc_b, OP_PUSH_CONST_WIDE);
        enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)(n_const_s));
        enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)(n_const_s >> 8));
        return;
    }
    enki_vector_push_u8_or_throw(i, bc_b, OP_PUSH_CONST);
    enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)n_const_s);
}

static void enki_law_compile_value(enki_interpreter* i, enki_value body_v, size_t depth_s, enki_vector* bc_b, enki_vector* const_table_v) {
  body_v = enki_value_unind(body_v);
  if(body_v != 0 && !IS_PTR(body_v) && body_v <= depth_s) {
    size_t index = (depth_s - body_v);
    if(index > 255) {
        if(index > ENKI_BC_WIDE_MAX) abort();
        enki_vector_push_u8_or_throw(i, bc_b, OP_PICK_WIDE);
        enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)(index));
        enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)(index >> 8));
        return;
    }
    enki_vector_push_u8_or_throw(i, bc_b, OP_PICK);
    enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)index);
    return;
  }
  void* ptr = ENKI_AS(void, body_v);
  if(!IS_PTR(body_v) || ((enki_value_header*)ptr)->kind_b != ENKI_APP) {
    enki_law_push_const(i, bc_b, const_table_v, body_v);
    return;
  }
  else {
    enki_app* app = (enki_app*)ptr;
    // [0 x] case 
    if(app->fn_v == 0 && app->n_args_s == 1) {
        enki_law_push_const(i, bc_b, const_table_v, app->args_v[0]);
        return;
    }
    // [0 f x] case 
    else if(app->fn_v == 0 && app->n_args_s == 2) {  
        enki_law_compile_value(i, app->args_v[0], depth_s, bc_b, const_table_v);
        enki_law_compile_value(i, app->args_v[1], depth_s, bc_b, const_table_v);
        enki_vector_push_u8_or_throw(i, bc_b, OP_APPLY);
        enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)1);
        return;
    }
    // is it a primitive with proper arity? if not do a generic apply 
    else if(!IS_PTR(app->fn_v) && app->fn_v <= MAX_PRIM_ID) {
        enki_prim_spec spec = ENKI_PRIMS[app->fn_v];
        if(app->n_args_s == spec.arity_s) {
            enki_law_compile_args(i, app, depth_s, bc_b, const_table_v);
            enki_vector_push_u8_or_throw(i, bc_b, spec.group);
            enki_vector_push_u8_or_throw(i, bc_b, spec.subop_b);
            return;
        }
        else {
            goto generic_apply;
        }
    }
    generic_apply:
        enki_law_compile_value(i, app->fn_v, depth_s, bc_b, const_table_v);
        enki_law_compile_args(i, app, depth_s, bc_b, const_table_v);
        size_t n_args = app->n_args_s;
        if(n_args > 255) {
            if(n_args > ENKI_BC_WIDE_MAX) abort();
            enki_vector_push_u8_or_throw(i, bc_b, OP_APPLY_WIDE);
            enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)(n_args));
            enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)(n_args >> 8));
            return;
        }

        enki_vector_push_u8_or_throw(i, bc_b, OP_APPLY);
        enki_vector_push_u8_or_throw(i, bc_b, (uint8_t)app->n_args_s);
  }
}
static bool enki_law_is_letrec(enki_value body_v, enki_value* v, enki_value* k) {
    body_v = enki_value_unind(body_v);
    if(!IS_PTR(body_v)) return false;
    enki_value_header* h = ENKI_AS(enki_value_header, body_v);
    if(h->kind_b != ENKI_APP) return false;
    enki_app* app = ENKI_AS(enki_app, body_v);
    // [1 v k] case
    if (app->fn_v == 1 && app->n_args_s == 2) {
        *v = app->args_v[0];
        *k = app->args_v[1];
        return true;
    }
    return false;
}
void enki_law_compile(enki_interpreter* i, enki_value body_v, size_t arity_s, enki_vector* bc_b, enki_vector* const_table_v) {
    size_t depth = arity_s;
    enki_value k;
    enki_value v;
    while(enki_law_is_letrec(body_v, &v, &k)) {
        enki_law_compile_value(i, v, depth, bc_b, const_table_v);
        depth += 1;
        body_v = k;
    }
    enki_law_compile_value(i, body_v, depth, bc_b, const_table_v);
    enki_vector_push_u8_or_throw(i, bc_b, OP_RETURN);
}
