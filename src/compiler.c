#include "compiler.h"

static const enki_prim_spec* enki_lookup_prim(enki_value id) {
    size_t n = sizeof(ENKI_PRIMS) / sizeof(ENKI_PRIMS[0]);
    for(size_t k = 0; k < n; k++) {
        if(ENKI_PRIMS[k].id == id) {
            return &ENKI_PRIMS[k];
        }
    }
    return NULL;
}

static void enki_emit_prim(const enki_prim_spec* prim, enki_vector* bc) {
    if(prim->group == ENKI_PRIM_GROUP_OP0) {
        enki_vector_push_u8(bc, OP_OP0);
    }
    else if(prim->group == ENKI_PRIM_GROUP_OP66) {
        enki_vector_push_u8(bc, OP_OP66);
    }
    else {
        return;
    }
    enki_vector_push_u8(bc, prim->subop);
}

void enki_compile_value(enki_value body, uint32_t depth, enki_vector* bc, enki_vector* const_table) {
  if(!IS_PTR(body) && body <= depth) {
    enki_vector_push_u8(bc, OP_PICK);
    enki_vector_push_u8(bc, (uint8_t)(depth - body)); // cant exceed 255 yet
    return;
  }
  void* ptr = ENKI_TO_PTR(body);
  if(!IS_PTR(body) || ((enki_value_header*)ptr)->kind != ENKI_APP) {
    size_t n_const = enki_vector_len(const_table);
    enki_vector_push_copy(const_table, &body);
    enki_vector_push_u8(bc, OP_PUSH_CONST);
    enki_vector_push_u8(bc, (uint8_t)n_const); // cant exceed 255 yet
    return;
  }
  else {
    enki_app* app = (enki_app*)ptr;
    const enki_prim_spec* prim = enki_lookup_prim(app->fn);
    if(prim != NULL && app->n_args == prim->arity) {
      enki_compile_args(app, depth, bc, const_table);
      enki_emit_prim(prim, bc);
      return;
    }
    enki_compile_value(app->fn, depth, bc, const_table);
    enki_compile_args(app, depth, bc, const_table);
    enki_vector_push_u8(bc, OP_APPLY);
    enki_vector_push_u8(bc, (uint8_t)app->n_args);
  }
}

void enki_compile_args(enki_app* app, size_t depth, enki_vector* bc, enki_vector* const_table) {
   for(uint8_t k = 0; k < (uint8_t)app->n_args; k++) { // cant exceed 255 yet
      enki_compile_value(app->args[k], depth, bc, const_table);
    }
}

void enki_compile_law(enki_value body, size_t arity, enki_vector* bc, enki_vector* const_table) {
    enki_compile_value(body, arity, bc, const_table);
    enki_vector_push_u8(bc, OP_RETURN);
}