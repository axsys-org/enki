#include "compiler.h"
#define SYS_MKPIN ((OP_OP0 << 8) | 0x00)
#define SYS_MKLAW ((OP_OP0 << 8) | 0x01)
#define SYS_MATCH  ((OP_OP0 << 8) | 0x02)

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
    if(!IS_PTR(app->fn) && app->fn == SYS_MKLAW) {
      enki_compile_args(app, depth, bc, const_table);
      enki_vector_push_u8(bc, OP_OP0);
      enki_vector_push_u8(bc, 0x00);
    }
    else if(!IS_PTR(app->fn) && app->fn == SYS_MKPIN){
      enki_compile_args(app, depth, bc, const_table);
      enki_vector_push_u8(bc, OP_OP0);
      enki_vector_push_u8(bc, 0x01);
    }
    else if(!IS_PTR(app->fn) && app->fn == SYS_MATCH) {
      enki_compile_args(app, depth, bc, const_table);
      enki_vector_push_u8(bc, OP_OP0);
      enki_vector_push_u8(bc, 0x02);
    }
    else {
      enki_compile_value(app->fn, depth, bc, const_table);
      enki_compile_args(app, depth, bc, const_table);
      enki_vector_push_u8(bc, OP_APPLY);
      enki_vector_push_u8(bc, (uint8_t)app->n_args); // cant exceed 255 yet
    }
  }
}

void compile_args(enki_app* app, size_t depth, enki_vector* bc, enki_vector* const_table) {

   for(uint8_t k = 0; k < (uint8_t)app->n_args; k++) { // cant exceed 255 yet
      enki_compile_value(app->args[k], depth, bc, const_table);
    }
}

void enki_compile_law(enki_value body, size_t arity, enki_vector* bc, enki_vector* const_table) {
    enki_compile_value(body, arity, bc, const_table);
    enki_vector_push_u8(bc, OP_RETURN);
}