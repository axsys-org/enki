#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "enki/run.h"

typedef struct er_bc_code_span {
  er_op* op_v;
  size_t op_s;
} er_bc_code_span;

typedef struct er_bc_asm {
  uint32_t let_d;
  size_t label_s;
  er_bc_code_span* label_v;
} er_bc_asm;

typedef enum er_bc_asm_status {
  ER_BC_ASM_OK = 0,
  ER_BC_ASM_BAD_SHAPE,
  ER_BC_ASM_BAD_OPCODE,
  ER_BC_ASM_BAD_OPERAND,
  ER_BC_ASM_BAD_TARGET,
  ER_BC_ASM_MISSING_RET,
  ER_BC_ASM_OOM,
} er_bc_asm_status;

typedef struct er_bc_asm_error {
  er_bc_asm_status status;
  er_val at_v;
  size_t label_s;
  size_t op_s;
  const char* msg_c;
} er_bc_asm_error;

typedef er_val (*er_law_emit_asm_fn)(const enki_allocator* loc_a, void* ctx,
                                     er_val nam_v, er_val bod_v,
                                     uint32_t ari_d);

typedef struct er_law_compiler {
  er_law_emit_asm_fn emit_asm;
  void* ctx;
  bool enabled_f;
} er_law_compiler;

er_bc_asm_status er_bc_asm_decode(const enki_allocator* work_a, er_val asm_v,
                                  er_bc_asm* out);
void er_bc_asm_free(const enki_allocator* work_a, er_bc_asm* asm_b);
er_val er_law_make_asm(const enki_allocator* loc_a, er_val nam_v, er_val bod_v,
                       uint32_t ari_d, er_val asm_v, er_bc_asm_error* err);
er_val er_law_make_with_compiler(const enki_allocator* loc_a,
                                 const er_law_compiler* compiler, er_val nam_v,
                                 er_val bod_v, uint32_t ari_d);
er_val er_law_compile(const enki_allocator* loc_a, er_val nam_v, er_val bod_v,
                      uint32_t ari_d);
