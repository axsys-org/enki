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
  uint8_t* starts = NULL; /* pass-1 scratch, freed on every exit */
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
  pl_op_t last_op = PL_OP_COUNT;
  /* pass 1: validate opcodes/operands, record instruction starts, and
   * mark jump targets; pass 2 checks every target lands on a start;
   * pass 3 fuses MK_THK+RET (skipped when the RET is a jump target). */
  starts = calloc(n, 2); /* [0..n): is-start, [n..2n): is-target */
  uint8_t* is_target = starts == NULL ? NULL : starts + n;
  if (starts == NULL)
    FAIL("oom")
#define MARK_TARGET(tgt)                                                       \
  {                                                                            \
    if ((pl_val)(tgt) >= n || !pl_is_nat63((pl_val)(tgt)))                     \
      FAIL("jump target out of range")                                         \
    is_target[(size_t)(tgt)] = 1;                                              \
  }
  while (i < n) {
    starts[i] = 1;
    pl_op_t op = ops[i++];
    last_op = op;
    switch (op) {
    case OP_PUSH_VAR:
    case OP_PUSH_LIT:
    case OP_MK_APP:
    case OP_INTERP:
    case OP_PUSH_SLOT:
      if (i + 1 > n)
        FAIL("truncated operand")
      i += 1;
      break;
    case OP_RET:
    case OP_FORCE:
      break;
    case OP_JMP:
      if (i + 1 > n)
        FAIL("truncated operand")
      MARK_TARGET(ops[i])
      i += 1;
      break;
    case OP_CALL:
      if (i + 2 > n)
        FAIL("truncated operand")
      MARK_TARGET(ops[i])
      i += 2;
      break;
    case OP_BR: {
      if (i + 1 > n)
        FAIL("truncated operand")
      pl_op_t arms = ops[i];
      if (arms == 0 || !pl_is_nat63((pl_val)arms) || i + 1 + arms > n)
        FAIL("bad branch arm count")
      for (size_t arm = 0; arm < arms; arm++)
        MARK_TARGET(ops[i + 1 + arm])
      i += 1 + arms;
      break;
    }
    case OP_MK_THK: {
      if (i + 2 > n)
        FAIL("truncated operand")
      pl_op_t argc = ops[i];
      pl_op_t bane = ops[i + 1] & PL_BAN_MASK;
      if (ops[i + 1] & ~(pl_op_t)(PL_BAN_MASK | PL_BAN_NOUPD))
        FAIL("bad bane")
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
#undef MARK_TARGET
  /* exec must never run off the end: the last instruction is a RET
   * (possibly the unreachable one behind a fused TAILCALL) */
  if (last_op != OP_RET)
    FAIL("bad program end")
  for (size_t j = 0; j < n; j++)
    if (is_target[j] && !starts[j])
      FAIL("jump target inside an instruction")
  /* pass 3: a thunk RETurned immediately is forced immediately — fuse
   * into a direct tail entry (the trailing RET becomes unreachable).
   * Never fuse when something jumps to that RET: it must stay live. */
  i = 0;
  while (i < n) {
    size_t opslot = i;
    pl_op_t op = ops[i++];
    switch (op) {
    case OP_PUSH_VAR:
    case OP_PUSH_LIT:
    case OP_MK_APP:
    case OP_INTERP:
    case OP_PUSH_SLOT:
    case OP_JMP:
      i += 1;
      break;
    case OP_CALL:
      i += 2;
      break;
    case OP_BR:
      i += 1 + (size_t)ops[i];
      break;
    case OP_MK_THK:
      i += (ops[i + 1] & PL_BAN_MASK) == PL_BAN_PRIM_KNOWN ? 4 : 2;
      if (i < n && ops[i] == OP_RET && !is_target[i])
        ops[opslot] = OP_TAILCALL;
      break;
    default:
      break;
    }
  }
  free(starts);
  return out;

failed:
  free(starts);
  if (out->ops != NULL)
    free(out->ops);
  free(out);
  if (msg != NULL)
    fprintf(stderr, "Failed to decode bytecode: %s\r\n", msg);
  return NULL;
}

void pl_bytecode_free(pl_code* code) {
  if (code == NULL)
    return;
  free(code->ops);
  free(code);
}
