#include "enki/compiler.h"
#include "enki/util.h"

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
    enki_vector_push_u8(bc_b, (uint8_t)body_v); // cant exceed 255 yet
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




static void enki_emit_bc(enki_value bod_v, uint64_t dep_q, enki_value* const_v, uint8_t* out_b)
{
  bool eval_lit = false;

  if(!IS_PTR(bod_v)) {
    if (bod_v <= dep_q) {
      assert(bod_v < 256);
      arrput(out_b, OP_PICK);
      arrput(out_b, (uint8_t)bod_v);
    } else {
      goto emit_lit;
    }
    return;
  }
  obj_header* h = (obj_header*)ENKI_TO_PTR(bod_v);
  if (h->kind_b != ENKI_APP ) {
    goto emit_lit;
  }
  enki_app* app = (enki_app*)h;
  if (app->fn_v != 0 || app->n_args_s != 2) {
    eval_lit = app->n_args_s == 1;
    goto emit_lit;
  }

  enki_emit_bc(app->args_v[0], dep_q, const_v, out_b);
  arrput(out_b, OP_EVAL);
  arrput(out_b, 0);

  enki_emit_bc(app->args_v[1], dep_q, const_v, out_b);
  arrput(out_b, OP_EVAL);
  arrput(out_b, 0);

  arrput(out_b, OP_APPLY);
  arrput(out_b, 2);

  // -- []


  return;

emit_lit:
  arrput(out_b, OP_PUSH_CONST);
  arrput(out_b, (uint8_t)arrlen(const_v));
  arrput(const_v, bod_v);
  if(eval_lit) {
    arrput(out_b, OP_EVAL);
    arrput(out_b, 0);
  }
  return;
}



