#include "plan/bytecode.h"
#include "stdio.h"
#include "inttypes.h"
#include "stdlib.h"

#include "internal.h"
#include "plan/nat.h"

/*
 * Decode a compiled code row into an executable pl_code.
 *
 * The walk below is the single validation point for compiled programs:
 * exec trusts the decoded stream (its checks are debug-only), so every
 * opcode, operand count, and bane is verified here, and a malformed
 * program fails the decode — the law simply stays interpreted.
 *
 * It is also where PL_BAN_PRIM_KNOWN thunks get their primop resolved:
 * the compiler emits the opset pin and the op-name nat as two inline
 * operands after the bane (a pure shape check on its side); we resolve
 * (opset, name, argc) through pl_op_lookup once and overwrite the pin
 * operand with the pl_ops index in this malloc'd copy.  The pinned
 * canonical row keeps the symbolic form, so content addressing and
 * snapshots are unaffected.
 */

/** mallocs (and leaks) */
pl_code* pl_bytecode_from_val(pl_val val) {
  /* PL_NO_BYTECODE=1: refuse every decode, so the whole system runs
   * interpreted — the differential-testing ground truth */
  static int no_bytecode = -1;
  if (no_bytecode < 0)
    no_bytecode = getenv("PL_NO_BYTECODE") != NULL;
  if (no_bytecode)
    return NULL;

  char* msg = NULL;
  pl_code* out = calloc(1, sizeof(pl_code));
  pl_cell* a = pl_as(PL_TAG_APP, val);
#define FAIL(m)                                                                \
  {                                                                            \
    msg = m;                                                                   \
    goto failed;                                                               \
  }
  if (a == NULL)
    FAIL("no app inside pin")
  out->nops = pl_app_n(a);
  out->ops = calloc(out->nops, sizeof(pl_op_t));
  pl_val* args = pl_app_args(a);
  for (size_t i = 0; i < out->nops; i++)
    out->ops[i] = args[i];

  pl_op_t* ops = out->ops;
  size_t n = out->nops;
  size_t i = 0;
  while (i < n) {
    pl_op_t op = ops[i++];
    switch (op) {
    case OP_PUSH_VAR:
    case OP_PUSH_LIT:
    case OP_MK_APP:
    case OP_INTERP:
      if (i + 1 > n)
        FAIL("truncated operand")
      i += 1;
      break;
    case OP_RET:
      break;
    case OP_MK_THK: {
      if (i + 2 > n)
        FAIL("truncated operand")
      pl_op_t argc = ops[i];
      pl_op_t bane = ops[i + 1];
      i += 2;
      if (bane == PL_BAN_PRIM_KNOWN) {
        if (i + 2 > n)
          FAIL("truncated operand")
        pl_cell* pin = pl_as(PL_TAG_PIN, (pl_val)ops[i]);
        pl_val name = (pl_val)ops[i + 1];
        if (pin == NULL || !pl_is_nat(pl_pin_body(pin)))
          FAIL("bad known-primop opset")
        uint64_t opset = pl_nat_u64_clamp(pl_pin_body(pin));
        int idx = pl_op_lookup(opset, name, (uint32_t)argc);
        if (idx < 0) {
          /* an op this runtime doesn't implement (wrappers are declared
           * speculatively): stay interpreted, silently — the interp
           * raises "no primop" if the wrapper is ever actually called */
          goto failed;
        }
        ops[i] = (pl_op_t)idx;
        ops[i + 1] = 0;
        i += 2;
      } else if (bane != PL_BAN_FAST && bane != PL_BAN_SLOW &&
                 bane != PL_BAN_PRIM) {
        FAIL("bad bane")
      }
      break;
    }
    default:
      FAIL("bad opcode")
    }
  }
  return out;

failed:
  if (out->ops != NULL)
    free(out->ops);
  free(out);
  if (msg != NULL)
    fprintf(stderr, "Failed to decode bytecode: %s\r\n", msg);
  return NULL;
}
