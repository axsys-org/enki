#include "enki/compiler.h"

static const enki_prim_spec* enki_lookup_prim(enki_value id) {
    size_t n = sizeof(ENKI_PRIMS) / sizeof(ENKI_PRIMS[0]);
    for(size_t k = 0; k < n; k++) {
        if(ENKI_PRIMS[k].id == id) {
            return &ENKI_PRIMS[k];
        }
    }
    return NULL;
}

static void enki_emit_prim(const enki_prim_spec* prim, enki_vector* bc_b) {
    if(prim->group == ENKI_PRIM_GROUP_OP0) {
        enki_vector_push_u8(bc_b, OP_OP0);
    }
    else if(prim->group == ENKI_PRIM_GROUP_OP66) {
        enki_vector_push_u8(bc_b, OP_OP66);
    }
    else {
        return;
    }
    enki_vector_push_u8(bc_b, prim->subop_b);
}

void enki_compile_value(enki_value body_v, size_t depth_s, enki_vector* bc_b, enki_vector* const_table_v) {
  if(!IS_PTR(body_v) && body_v <= depth_s) {
    enki_vector_push_u8(bc_b, OP_PICK);
    enki_vector_push_u8(bc_b, (uint8_t)(depth_s - body_v)); // cant exceed 255 yet
    return;
  }
  void* ptr = ENKI_TO_PTR(body_v);
  if(!IS_PTR(body_v) || ((enki_value_header*)ptr)->kind_b != ENKI_APP) {
    size_t n_const_s = enki_vector_len(const_table_v);
    enki_vector_push_copy(const_table_v, &body_v);
    enki_vector_push_u8(bc_b, OP_PUSH_CONST);
    enki_vector_push_u8(bc_b, (uint8_t)n_const_s); // cant exceed 255 yet
    return;
  }
  else {
    enki_app* app = (enki_app*)ptr;
    const enki_prim_spec* prim = enki_lookup_prim(app->fn_v);
    if(prim != NULL && app->n_args_s == prim->arity_s) {
      enki_compile_args(app, depth_s, bc_b, const_table_v);
      enki_emit_prim(prim, bc_b);
      return;
    }
    enki_compile_value(app->fn_v, depth_s, bc_b, const_table_v);
    enki_compile_args(app, depth_s, bc_b, const_table_v);
    enki_vector_push_u8(bc_b, OP_APPLY);
    enki_vector_push_u8(bc_b, (uint8_t)app->n_args_s);
  }
}

void enki_compile_args(enki_app* app, size_t depth_s, enki_vector* bc_b, enki_vector* const_table_v) {
   for(uint8_t k = 0; k < (uint8_t)app->n_args_s; k++) { // cant exceed 255 yet
      enki_compile_value(app->args_v[k], depth_s, bc_b, const_table_v);
    }
}

void enki_compile_law(enki_value body_v, size_t arity_s, enki_vector* bc_b, enki_vector* const_table_v) {
    enki_compile_value(body_v, arity_s, bc_b, const_table_v);
    enki_vector_push_u8(bc_b, OP_RETURN);
}
