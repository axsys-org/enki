#include "enki/bytecode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bytecode_internal.h"

#include "enki/gc.h"
#include "enki/profile.h"
#include "enki/run_ops.h"
#include "enki/run.h"
#include "enki/motes.h"

typedef struct er_bc_code {
  const enki_allocator* loc_a;
  er_op* op_v;
  size_t op_s;
  size_t cap_s;
  bool ok_f;
} er_bc_code;

typedef struct er_bc_label {
  er_op* code_v;
  size_t code_s;
  bool set_f;
} er_bc_label;

typedef struct er_bc_compiler {
  const enki_allocator* loc_a;
  er_bc_label* label_v;
  size_t label_s;
  size_t cap_s;
  bool ok_f;
} er_bc_compiler;

#define ER_BC_L ER_BC_EVAL_NONE
#define ER_BC_W ER_BC_EVAL_WHNF
#define ER_BC_N ER_BC_EVAL_NF

#define ER_BC_PRIM_ROUTE(_tag, _arg_s)                                         \
  {                                                                            \
      .tag = (_tag),                                                           \
      .arg_s = (_arg_s),                                                       \
      .valid_f = true,                                                         \
      .arg_eval_v = {ER_BC_W, ER_BC_W, ER_BC_W, ER_BC_W, ER_BC_W, ER_BC_W,     \
                     ER_BC_W, ER_BC_W},                                        \
  }

#define ER_BC_PRIM_ROUTE_ARGS(_tag, _arg_s, ...)                               \
  {                                                                            \
      .tag = (_tag),                                                           \
      .arg_s = (_arg_s),                                                       \
      .valid_f = true,                                                         \
      .arg_eval_v = {__VA_ARGS__},                                             \
  }

static const er_bc_prim_route er_bc_prim0_route_v[] = {
    [OP0_PIN] = ER_BC_PRIM_ROUTE(OP_PIN, 1),
    [OP0_LAW] = ER_BC_PRIM_ROUTE_ARGS(OP_LAW, 3, ER_BC_N, ER_BC_N, ER_BC_N),
    [OP0_ELIM] = ER_BC_PRIM_ROUTE_ARGS(OP_ELIM, 6, ER_BC_L, ER_BC_L, ER_BC_L,
                                       ER_BC_L, ER_BC_L, ER_BC_W),
};

static const er_bc_prim_route er_bc_prim66_route_v[ER_OP66_COUNT] = {
    [OP66_INC] = ER_BC_PRIM_ROUTE(OP_INC, 1),
    [OP66_DEC] = ER_BC_PRIM_ROUTE(OP_DEC, 1),
    [OP66_ADD] = ER_BC_PRIM_ROUTE(OP_ADD, 2),
    [OP66_SUB] = ER_BC_PRIM_ROUTE(OP_SUB, 2),
    [OP66_MUL] = ER_BC_PRIM_ROUTE(OP_MUL, 2),
    [OP66_DIV] = ER_BC_PRIM_ROUTE(OP_DIV, 2),
    [OP66_MOD] = ER_BC_PRIM_ROUTE(OP_MOD, 2),
    [OP66_EQ] = ER_BC_PRIM_ROUTE(OP_EQ, 2),
    [OP66_LE] = ER_BC_PRIM_ROUTE(OP_LE, 2),
    [OP66_CMP] = ER_BC_PRIM_ROUTE(OP_CMP, 2),
    [OP66_RSH] = ER_BC_PRIM_ROUTE(OP_RSH, 2),
    [OP66_LSH] = ER_BC_PRIM_ROUTE(OP_LSH, 2),
    [OP66_TEST] = ER_BC_PRIM_ROUTE(OP_TEST, 2),
    [OP66_BEX] = ER_BC_PRIM_ROUTE(OP_BEX, 1),
    [OP66_BITS] = ER_BC_PRIM_ROUTE(OP_BITS, 1),
    [OP66_BYTES] = ER_BC_PRIM_ROUTE(OP_BYTES, 1),
    [OP66_LOAD8] = ER_BC_PRIM_ROUTE(OP_LOAD8, 2),
    [OP66_STORE8] = ER_BC_PRIM_ROUTE(OP_STORE8, 3),
    [OP66_TRUNC] = ER_BC_PRIM_ROUTE(OP_TRUNC, 2),
    [OP66_TRUNC8] = ER_BC_PRIM_ROUTE(OP_TRUNC8, 1),
    [OP66_TRUNC16] = ER_BC_PRIM_ROUTE(OP_TRUNC16, 1),
    [OP66_TRUNC32] = ER_BC_PRIM_ROUTE(OP_TRUNC32, 1),
    [OP66_TRUNC64] = ER_BC_PRIM_ROUTE(OP_TRUNC64, 1),
    [OP66_NAT] = ER_BC_PRIM_ROUTE(OP_NAT, 1),
    [OP66_UNPIN] = ER_BC_PRIM_ROUTE(OP_UNPIN, 1),
    [OP66_ARITY] = ER_BC_PRIM_ROUTE(OP_ARI, 1),
    [OP66_NAME] = ER_BC_PRIM_ROUTE(OP_NAM, 1),
    [OP66_BODY] = ER_BC_PRIM_ROUTE(OP_BODY, 1),
    [OP66_HD] = ER_BC_PRIM_ROUTE(OP_HD, 1),
    [OP66_LAST] = ER_BC_PRIM_ROUTE(OP_LAST, 1),
    [OP66_INIT] = ER_BC_PRIM_ROUTE(OP_INIT, 1),
    [OP66_REP] = ER_BC_PRIM_ROUTE_ARGS(OP_REP, 3, ER_BC_L, ER_BC_L, ER_BC_W),
    [OP66_SLICE] = ER_BC_PRIM_ROUTE(OP_SLICE, 3),
    [OP66_WELD] = ER_BC_PRIM_ROUTE(OP_WELD, 2),
    [OP66_UP] = ER_BC_PRIM_ROUTE_ARGS(OP_UP, 3, ER_BC_W, ER_BC_L, ER_BC_W),
    [OP66_UP_UNIQ] = ER_BC_PRIM_ROUTE_ARGS(OP_UP, 3, ER_BC_W, ER_BC_L, ER_BC_W),
    [OP66_COUP] = ER_BC_PRIM_ROUTE_ARGS(OP_COUP, 2, ER_BC_L, ER_BC_W),
    [OP66_SZ] = ER_BC_PRIM_ROUTE(OP_SZ, 1),
    [OP66_IX] = ER_BC_PRIM_ROUTE(OP_IX, 2),
    [OP66_NIL] = ER_BC_PRIM_ROUTE(OP_NOT, 1),
    [OP66_TRUTH] = ER_BC_PRIM_ROUTE(OP_TRU, 1),
    [OP66_OR] = ER_BC_PRIM_ROUTE_ARGS(OP_OR, 2, ER_BC_W, ER_BC_L),
    [OP66_AND] = ER_BC_PRIM_ROUTE_ARGS(OP_AND, 2, ER_BC_W, ER_BC_L),
    [OP66_LOAD] = ER_BC_PRIM_ROUTE(OP_LOAD, 3),
    [ER_OP66_STORE] = ER_BC_PRIM_ROUTE(OP_STORE, 4),
    [ER_OP66_MET] = ER_BC_PRIM_ROUTE(OP_MET_DYN, 2),
};

bool er_bc_prim_route_strict(er_optag tag, size_t arg_s,
                             er_bc_prim_route* out) {
  if (out == NULL || arg_s > ER_BC_MAX_PRIM_ARITY) {
    return false;
  }
  *out = (er_bc_prim_route)ER_BC_PRIM_ROUTE(tag, arg_s);
  return true;
}

bool er_bc_prim0_route(size_t op_s, er_bc_prim_route* out) {
  if (out == NULL ||
      op_s >= sizeof(er_bc_prim0_route_v) / sizeof(er_bc_prim0_route_v[0]) ||
      !er_bc_prim0_route_v[op_s].valid_f) {
    return false;
  }
  *out = er_bc_prim0_route_v[op_s];
  return true;
}

bool er_bc_prim66_route(int op_i, er_bc_prim_route* out) {
  if (out == NULL || op_i < 0 || (size_t)op_i >= ER_OP66_COUNT ||
      !er_bc_prim66_route_v[op_i].valid_f) {
    return false;
  }
  *out = er_bc_prim66_route_v[op_i];
  return true;
}

static bool er_bc_mul_size(size_t a_s, size_t b_s, size_t* out_s) {
  if (b_s != 0 && a_s > SIZE_MAX / b_s) {
    return false;
  }
  *out_s = a_s * b_s;
  return true;
}

static bool er_bc_realloc(const enki_allocator* loc_a, void** ptr,
                          size_t old_count_s, size_t new_count_s,
                          size_t elem_s) {
  size_t old_size_s = 0;
  size_t new_size_s = 0;
  if (!er_bc_mul_size(old_count_s, elem_s, &old_size_s) ||
      !er_bc_mul_size(new_count_s, elem_s, &new_size_s) || new_size_s == 0) {
    return false;
  }
  if (loc_a == NULL || loc_a->alloc == NULL || loc_a->free == NULL) {
    return false;
  }
  if (loc_a->realloc != NULL) {
    void* next = loc_a->realloc(loc_a->ctx, *ptr, new_size_s);
    if (next == NULL) {
      return false;
    }
    *ptr = next;
    return true;
  }
  void* next = loc_a->alloc(loc_a->ctx, new_size_s);
  if (next == NULL) {
    return false;
  }
  if (*ptr != NULL && old_size_s != 0) {
    memcpy(next, *ptr, old_size_s < new_size_s ? old_size_s : new_size_s);
    loc_a->free(loc_a->ctx, *ptr);
  }
  *ptr = next;
  return true;
}

static bool er_bc_code_reserve(er_bc_code* code, size_t need_s) {
  if (!code->ok_f) {
    return false;
  }
  if (need_s <= code->cap_s) {
    return true;
  }
  size_t next_s = code->cap_s == 0 ? 8 : code->cap_s;
  while (next_s < need_s) {
    if (next_s > SIZE_MAX / 2) {
      code->ok_f = false;
      return false;
    }
    next_s *= 2;
  }
  void* ptr = code->op_v;
  if (!er_bc_realloc(code->loc_a, &ptr, code->cap_s, next_s, sizeof(er_op))) {
    code->ok_f = false;
    return false;
  }
  code->op_v = ptr;
  code->cap_s = next_s;
  return true;
}

static bool er_bc_emit(er_bc_code* code, er_optag tag) {
  if (!er_bc_code_reserve(code, code->op_s + 1)) {
    return false;
  }
  code->op_v[code->op_s] = (er_op){.tag = tag};
  code->op_s++;
  return true;
}

static bool er_bc_emit_u32(er_bc_code* code, er_optag tag, uint32_t u32) {
  if (!er_bc_emit(code, tag)) {
    return false;
  }
  code->op_v[code->op_s - 1].as.u32 = u32;
  return true;
}

static bool er_bc_emit_lit(er_bc_code* code, er_val lit_v) {
  if (!er_bc_emit(code, OP_PUSH_LIT)) {
    return false;
  }
  code->op_v[code->op_s - 1].as.lit_v = lit_v;
  return true;
}

static bool er_bc_emit_var(er_bc_code* code, er_val var_v) {
  if (!er_bc_emit(code, OP_PUSH_VAR)) {
    return false;
  }
  code->op_v[code->op_s - 1].as.slot = (uintptr_t)var_v;
  return true;
}

static void er_bc_code_free(er_bc_code* code) {
  if (code->op_v != NULL && code->loc_a != NULL && code->loc_a->free != NULL) {
    code->loc_a->free(code->loc_a->ctx, code->op_v);
  }
  code->op_v = NULL;
  code->op_s = 0;
  code->cap_s = 0;
}

static bool er_bc_compiler_reserve(er_bc_compiler* c, size_t need_s) {
  if (!c->ok_f) {
    return false;
  }
  if (need_s <= c->cap_s) {
    return true;
  }
  size_t next_s = c->cap_s == 0 ? 8 : c->cap_s;
  while (next_s < need_s) {
    if (next_s > SIZE_MAX / 2) {
      c->ok_f = false;
      return false;
    }
    next_s *= 2;
  }
  void* ptr = c->label_v;
  if (!er_bc_realloc(c->loc_a, &ptr, c->cap_s, next_s, sizeof(er_bc_label))) {
    c->ok_f = false;
    return false;
  }
  c->label_v = ptr;
  for (size_t k = c->cap_s; k < next_s; k++) {
    c->label_v[k] = (er_bc_label){0};
  }
  c->cap_s = next_s;
  return true;
}

static bool er_bc_compiler_set_label(er_bc_compiler* c, uint32_t label_d,
                                     er_op* code_v, size_t code_s) {
  size_t label_s = (size_t)label_d;
  if (code_v == NULL || code_s == 0 ||
      !er_bc_compiler_reserve(c, label_s + 1)) {
    return false;
  }
  if (label_s >= c->label_s) {
    c->label_s = label_s + 1;
  }
  c->label_v[label_s].code_v = code_v;
  c->label_v[label_s].code_s = code_s;
  c->label_v[label_s].set_f = true;
  return true;
}

static void er_bc_compiler_free(er_bc_compiler* c, bool free_code_f) {
  if (c->label_v == NULL || c->loc_a == NULL || c->loc_a->free == NULL) {
    return;
  }
  if (free_code_f) {
    for (size_t k = 0; k < c->label_s; k++) {
      if (c->label_v[k].code_v != NULL) {
        c->loc_a->free(c->loc_a->ctx, c->label_v[k].code_v);
      }
    }
  }
  c->loc_a->free(c->loc_a->ctx, c->label_v);
  c->label_v = NULL;
  c->label_s = 0;
  c->cap_s = 0;
}

static bool er_bc_is_let(er_val val_v, er_val* v_v, er_val* k_v) {
  er_app* app = er_outt(er_tag_app, val_v);
  if (app == NULL || app->fn_v != 1 || app->arg_s != 2) {
    return false;
  }
  *v_v = app->arg_v[0];
  *k_v = app->arg_v[1];
  return true;
}

static bool er_bc_is_var(size_t depth_s, er_val val_v) {
  return er_is_cat(val_v) && val_v <= (er_val)depth_s;
}

static bool er_bc_emit_eval_stack_arg(er_bc_code* code, size_t arg_s,
                                      size_t arg_i, er_bc_eval_req eval) {
  if (eval == ER_BC_EVAL_NONE) {
    return true;
  }
  if (arg_i >= arg_s || arg_s > UINT32_MAX) {
    code->ok_f = false;
    return false;
  }

  size_t span_s = arg_s - arg_i;
  if (span_s > UINT32_MAX) {
    code->ok_f = false;
    return false;
  }

  /*
   * Bring the selected argument to top, evaluate it, then left-rotate the
   * same window enough times to restore the primitive's operand order.
   */
  uint32_t span_d = (uint32_t)span_s;
  if (span_s > 1 && !er_bc_emit_u32(code, OP_ROTATE, span_d)) {
    return false;
  }
  if (!er_bc_emit(code, eval == ER_BC_EVAL_NF ? OP_FORCE : OP_EVAL)) {
    return false;
  }
  for (size_t k = 1; k < span_s; k++) {
    if (!er_bc_emit_u32(code, OP_ROTATE, span_d)) {
      return false;
    }
  }
  return true;
}

bool er_bc_emit_prim_route_fragment(er_op out_v[], size_t cap_s, er_optag tag,
                                    size_t arg_s,
                                    const er_bc_eval_req arg_eval_v[],
                                    er_val lit_v, size_t* out_s) {
  er_bc_code code = {
      .op_v = out_v,
      .op_s = 0,
      .cap_s = cap_s,
      .ok_f = true,
  };
  if (out_v == NULL || arg_eval_v == NULL || out_s == NULL || tag >= OP_COUNT ||
      arg_s > ER_BC_MAX_PRIM_ARITY) {
    return false;
  }
  for (size_t k = 0; k < arg_s; k++) {
    if (!er_bc_emit_eval_stack_arg(&code, arg_s, k, arg_eval_v[k])) {
      return false;
    }
  }
  if (!er_bc_emit(&code, tag)) {
    return false;
  }
  code.op_v[code.op_s - 1u].as.lit_v = lit_v;
  *out_s = code.op_s;
  return true;
}

static bool er_bc_compile_expr(er_bc_compiler* c, size_t depth_s, er_val val_v,
                               er_bc_code* code);

static bool er_bc_compile_app(er_bc_compiler* c, size_t depth_s, er_app* app,
                              er_bc_code* code) {
  if (app->fn_v == 0 && app->arg_s == 1) {
    return er_bc_emit_lit(code, app->arg_v[0]);
  }
  if (app->fn_v == 0 && app->arg_s == 2) {
    return er_bc_compile_expr(c, depth_s, app->arg_v[0], code) &&
           er_bc_compile_expr(c, depth_s, app->arg_v[1], code) &&
           er_bc_emit_u32(code, OP_APPLY_UNK, 2);
  }
  return er_bc_emit_lit(code, er_into(er_tag_app, app));

  // if (app->arg_s == 0) {
  //   return er_bc_compile_expr(c, depth_s, app->fn_v, code);
  // }
  // if (!er_bc_compile_expr(c, depth_s, app->fn_v, code)) {
  //   return false;
  // }
  // for (size_t k = 0; k < app->arg_s; k++) {
  //   if (!er_bc_compile_expr(c, depth_s, app->arg_v[k], code) ||
  //       !er_bc_emit_u32(code, OP_APPLY_UNK, 2)) {
  //     return false;
  //   }
  // }
  // return er_bc_emit_lit(code, er_into(er_tag_app, app));
}

static bool er_bc_compile_expr(er_bc_compiler* c, size_t depth_s, er_val val_v,
                               er_bc_code* code) {
  ENKI_PROFILE_ZONE("er_bc_compile_expr");
  if (er_bc_is_var(depth_s, val_v)) {
    return er_bc_emit_var(code, val_v);
  }

  er_app* app = er_outt(er_tag_app, val_v);
  if (app != NULL) {
    return er_bc_compile_app(c, depth_s, app, code);
  }
  return er_bc_emit_lit(code, val_v);
}

static bool er_bc_compile_label(er_bc_compiler* c, size_t depth_s,
                                uint32_t label_d, er_val body_v) {
  ENKI_PROFILE_ZONE("er_bc_compile_label");
  er_bc_code code = {.loc_a = c->loc_a, .ok_f = true};
  if (!er_bc_compile_expr(c, depth_s, body_v, &code) ||
      !er_bc_emit(&code, OP_RET)) {
    er_bc_code_free(&code);
    c->ok_f = false;
    return false;
  }
  if (!er_bc_compiler_set_label(c, label_d, code.op_v, code.op_s)) {
    er_bc_code_free(&code);
    return false;
  }
  return true;
}

er_val er_law_compile(const enki_allocator* loc_a, er_val nam_v, er_val bod_v,
                      uint32_t ari_d) {
  ENKI_PROFILE_ZONE("er_law_compile");
  if (loc_a == NULL || loc_a->alloc == NULL || loc_a->free == NULL) {
    return 0;
  }
  enki_gc* gc = enki_gc_from_allocator(loc_a);
  const enki_allocator* work_a =
      gc == NULL ? loc_a : enki_gc_parent_allocator(gc);
  if (work_a == NULL || work_a->alloc == NULL || work_a->free == NULL) {
    return 0;
  }

  er_val scan_v = bod_v;
  er_val let_v = 0;
  er_val next_v = 0;
  size_t let_s = 0;
  while (er_bc_is_let(scan_v, &let_v, &next_v)) {
    if (let_s == UINT32_MAX || ari_d > UINT32_MAX - (uint32_t)let_s - 1) {
      return 0;
    }
    let_s++;
    scan_v = next_v;
  }

  er_val law_v = 0;
  er_val* lets_v = NULL;
  er_op** code_v = NULL;
  size_t* code_len_v = NULL;
  er_bc_compiler c = {
      .loc_a = work_a,
      .ok_f = true,
  };

  if (let_s > 0) {
    lets_v = work_a->alloc(work_a->ctx, let_s * sizeof(er_val));
    if (lets_v == NULL) {
      goto cleanup;
    }
  }

  scan_v = bod_v;
  for (size_t k = 0; k < let_s; k++) {
    if (!er_bc_is_let(scan_v, &lets_v[k], &scan_v)) {
      goto cleanup;
    }
  }

  size_t depth_s = (size_t)ari_d + let_s;
  for (size_t k = 0; k < let_s; k++) {
    if (!er_bc_compile_label(&c, depth_s, (uint32_t)k + 1, lets_v[k])) {
      goto cleanup;
    }
  }
  if (!er_bc_compile_label(&c, depth_s, 0, scan_v)) {
    goto cleanup;
  }

  size_t bc_s = c.label_s;
  if (!c.ok_f || bc_s == 0) {
    goto cleanup;
  }
  code_v = work_a->alloc(work_a->ctx, bc_s * sizeof(er_op*));
  if (code_v == NULL) {
    goto cleanup;
  }
  code_len_v = work_a->alloc(work_a->ctx, bc_s * sizeof(size_t));
  if (code_len_v == NULL) {
    goto cleanup;
  }
  for (size_t k = 0; k < bc_s; k++) {
    if (!c.label_v[k].set_f || c.label_v[k].code_v == NULL) {
      goto cleanup;
    }
    code_v[k] = c.label_v[k].code_v;
    code_len_v[k] = c.label_v[k].code_s;
  }

  law_v = er_law_make_code(loc_a, nam_v, bod_v, ari_d, (uint32_t)let_s, bc_s,
                           code_v, code_len_v);

cleanup:
  if (code_len_v != NULL) {
    work_a->free(work_a->ctx, code_len_v);
  }
  if (code_v != NULL) {
    work_a->free(work_a->ctx, code_v);
  }
  if (lets_v != NULL) {
    work_a->free(work_a->ctx, lets_v);
  }
  er_bc_compiler_free(&c, true);
  return law_v;
}

/// Wisp bytecode emission
///
///
static er_bc_asm_status er_bc_asm_op_decode(er_val op_v, er_op* out) {
  er_app* app = er_outt(er_tag_app, op_v);
  if (!app)
    return ER_BC_ASM_BAD_SHAPE;

  er_val cod_v = app->arg_v[0];
#define GUARD(len)                                                             \
  if (app->arg_s != (len))                                                     \
    return ER_BC_ASM_BAD_OPERAND;
#define OK return ER_BC_ASM_OK
  switch (cod_v) {
  case MOTE_UNK_APP:
    GUARD(2)
    out->tag = OP_APPLY_UNK;
    // XX: should guard?
    out->as.u32 = (uint32_t)app->arg_v[1];
    OK;
  case MOTE_PUSH_VAR:
    GUARD(2)
    out->tag = OP_PUSH_VAR;
    out->as.u32 = (uint32_t)app->arg_v[1];
    OK;
  case MOTE_PUSH_LIT:
    GUARD(2)
    out->tag = OP_PUSH_VAR;
    out->as.lit_v = app->arg_v[1];
    OK;
  default:
    return ER_BC_ASM_BAD_OPCODE;
  }
#undef GUARD
#undef OK
}

static er_bc_asm_status er_bc_asm_label_decode(const enki_allocator* work_a,
                                               er_val lab_v,
                                               er_bc_asm_label* out) {
  er_app* lab = er_outt(er_tag_app, lab_v);
  if (!lab)
    return ER_BC_ASM_BAD_SHAPE;

  out->op_v = ea_calloc(work_a, er_op, lab->arg_s);
  out->op_s = lab->arg_s;

  er_bc_asm_status stat;
  for (size_t i = 0; i < out->op_s; i++) {
    stat = er_bc_asm_op_decode(lab->arg_v[i], &out->op_v[i]);
    if (stat != ER_BC_ASM_OK)
      return stat;
  }

  return ER_BC_ASM_OK;
}

er_bc_asm_status er_bc_asm_decode(const enki_allocator* work_a, er_val asm_v,
                                  er_bc_asm* out) {
  er_bc_asm_status stat = ER_BC_ASM_OK;
  er_app* app = er_outt(er_tag_app, asm_v);
  if (!app || app->arg_s != 2 || !er_is_cat(app->arg_v[0]))
    return ER_BC_ASM_BAD_SHAPE;
  out->let_d = (uint32_t)app->arg_v[0];

  er_app* labs = er_outt(er_tag_app, app->arg_v[1]);
  if (!labs || labs->arg_s < out->let_d)
    return ER_BC_ASM_BAD_SHAPE;

  out->label_s = labs->arg_s;
  out->label = ea_calloc(work_a, er_bc_asm_label, labs->arg_s);

  for (size_t i = 0; i < labs->arg_s; i++) {
    stat = er_bc_asm_label_decode(work_a, labs->arg_v[i], &out->label[i]);
    if (stat != ER_BC_ASM_OK) {
      return stat;
    }
  }

  return stat;
}

#undef ER_BC_N
#undef ER_BC_W
#undef ER_BC_L
