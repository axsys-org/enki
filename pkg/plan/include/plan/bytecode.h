#ifndef PLAN_BYTECODE_H
#define PLAN_BYTECODE_H
#include "plan/value.h"

typedef uint64_t pl_op_t; /* one slot per opcode and per operand */

typedef struct pl_code {
  pl_op_t* ops;
  size_t nops;
} pl_code;

typedef enum pl_op {
  OP_PUSH_VAR = 0,  /* +slot: push env slot                              */
  OP_PUSH_LIT = 1,  /* +val: push literal                                */
  OP_MK_THK = 2,    /* +argc +bane[|NOUPD] (+op +name when KNOWN)        */
  OP_FORCE = 3,     /* pop, evaluate to WHNF, push the result            */
  OP_CALL = 4,      /* +target +argc: local block call; args become the  */
                    /* callee's operand-stack base; result arrives WHNF  */
  OP_TAILCALL = 5,  /* ingest-fused MK_THK+RET                           */
  OP_INTERP = 6,    /* +expr: interpret a kal expr under the env         */
  OP_RET = 7,       /* pop, reset the operand stack, return              */
  OP_MK_APP = 8,    /* +argc: build an app (WHNF) from head + args       */
  OP_PUSH_SLOT = 9, /* +n: re-push operand-stack slot argbase+n          */
  OP_BR = 10,       /* +m +t0..t(m-1): pop nat scrutinee, jump to arm    */
  OP_JMP = 11,      /* +target: unconditional jump                       */
  PL_OP_COUNT = 12  /* sentinel: sizes pl_run's exec dispatch table */
} pl_op;

pl_code* pl_bytecode_from_val(pl_val val);
void pl_bytecode_free(pl_code* code);

#endif
