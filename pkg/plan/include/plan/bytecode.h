#ifndef PLAN_BYTECODE_H
#define PLAN_BYTECODE_H
#include "plan/value.h"

typedef uint64_t pl_op_t;      /* one slot per opcode and per operand */

typedef struct pl_code {
  pl_op_t* ops;
  size_t nops;
} pl_code;

typedef enum pl_op {
  OP_PUSH_VAR = 0,
  OP_PUSH_LIT = 1,
  OP_MK_THK = 2,
  OP_EVAL = 3,
  OP_CALL = 4,
  OP_TAILCALL = 5,
  OP_INTERP = 6,
  OP_RET = 7
} pl_op;

pl_code* pl_bytecode_from_val(pl_val val);


#endif
