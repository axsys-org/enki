#include "batch.h"
#include "plan/build.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>

enum { BATCH_STEPS = 64, BATCH_CELLS = 4096 };
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static bool enabled;
static void configure(void) {
  const char* v = getenv("PL_BATCH");
  enabled = v == NULL || strcmp(v, "0") != 0;
}

size_t pl_bytecode_width(const pl_op_t* o) {
  switch (o[0]) {
  case OP_PUSH_VAR:
  case OP_PUSH_LIT:
  case OP_MK_APP:
  case OP_INTERP:
  case OP_PUSH_SLOT:
  case OP_JMP:
  case OP_ENTRY:
  case OP_CALL_SLOW:
    return 2;
  case OP_CALL:
  case OP_CALL_FAST:
  case OP_TAILBLK:
    return 3;
  case OP_CALL_KNOWN:
  case OP_CALL_READY:
  case OP_ADD:
  case OP_SUB:
  case OP_CMP:
    return 4;
  case OP_BR:
    return 2 + (size_t)o[1];
  case OP_MK_THK:
  case OP_TAILCALL:
  case OP_TAIL_READY:
  case OP_TAIL_ADD:
  case OP_TAIL_SUB:
  case OP_TAIL_CMP:
    return (o[2] & PL_BAN_MASK) == PL_BAN_PRIM_KNOWN ? 5 : 3;
  default:
    return 1;
  }
}

void pl_batch_prepare(pl_code* c, const uint8_t* targets) {
  (void)pthread_once(&config_once, configure);
  if (!enabled || c->nops > 8192)
    return;
  for (size_t pc = 0; pc < c->nops;) {
    pl_batch b = {.start = (uint32_t)pc};
    pl_batch_step steps[BATCH_STEPS];
    pl_val lits[BATCH_STEPS];
    bool literal[BATCH_STEPS] = {0};
    uint32_t depth = 0, allocations = 0;
    size_t end = pc;
    while (end < c->nops && b.nsteps < BATCH_STEPS &&
           (end == pc || (!targets[end] && end != c->strict_entry))) {
      const pl_op_t* o = c->ops + end;
      pl_batch_step s = {.op = (uint32_t)o[0]};
      uint32_t cells = 0;
      if (o[0] == OP_PUSH_LIT) {
        s.arg = (uint32_t)end + 1;
        literal[depth] = true;
        lits[depth++] = o[1];
      } else if (o[0] == OP_PUSH_VAR || o[0] == OP_PUSH_SLOT) {
        if (o[1] >= UINT32_MAX)
          break;
        s.arg = (uint32_t)o[1];
        if (o[0] == OP_PUSH_VAR) {
          if (b.vars <= s.arg)
            b.vars = s.arg + 1;
        } else if (s.arg >= depth && b.depth < s.arg + 1 - depth) {
          b.depth = s.arg + 1 - depth;
        }
        literal[depth++] = false;
      } else if (o[0] == OP_MK_APP) {
        if (o[1] >= depth || !literal[depth - (uint32_t)o[1] - 1])
          break;
        pl_val head = lits[depth - (uint32_t)o[1] - 1];
        if (!pl_is_whnf(head))
          break;
        uint64_t arity = pl_arity(head);
        uint64_t need = arity > o[1] ? arity - o[1] : 0;
        if (need >= (1u << 20))
          break;
        s.arg = (uint32_t)o[1];
        s.aux = (uint32_t)need;
        cells = PL_APP_CELLS(s.arg);
        depth -= s.arg;
        literal[depth - 1] = false;
      } else if (o[0] == OP_MK_THK) {
        if (o[1] > depth)
          break;
        s.arg = (uint32_t)o[1];
        s.flags = o[2];
        bool known = (s.flags & PL_BAN_MASK) == PL_BAN_PRIM_KNOWN;
        if (known)
          s.flags &= PL_BAN_MASK | PL_BAN_NOUPD;
        /* Preserve all eager field/numeric shortcuts in x_mk_thk. */
        if (known && o[4] != PL_SPEC_NONE)
          break;
        s.aux = known ? (uint32_t)o[3] : 0;
        cells = PL_THKE_CELLS(s.arg + (known ? 1u : 0u));
        depth = depth - s.arg + 1;
        literal[depth - 1] = false;
      } else {
        break;
      }
      if (cells > BATCH_CELLS - b.cells)
        break;
      allocations += cells != 0;
      b.cells += cells;
      if (depth > b.extra)
        b.extra = depth;
      steps[b.nsteps++] = s;
      end += pl_bytecode_width(o);
    }
    if (allocations >= 2) {
      b.end = (uint32_t)end;
      b.steps = malloc(b.nsteps * sizeof(*b.steps));
      if (!b.steps)
        return;
      pl_batch* batches = realloc(c->batches, (c->nbatches + 1) * sizeof(b));
      if (!batches) {
        free(b.steps);
        return;
      }
      c->batches = batches;
      if (!c->batch_ops) {
        c->batch_ops = malloc(c->nops * sizeof(*c->batch_ops));
        if (!c->batch_ops) {
          free(b.steps);
          return;
        }
        memcpy(c->batch_ops, c->ops, c->nops * sizeof(*c->ops));
      }
      memcpy(b.steps, steps, b.nsteps * sizeof(*steps));
      c->batch_ops[pc] = OP_BUILD;
      c->batches[c->nbatches++] = b;
    }
    pc = end > pc ? end : pc + pl_bytecode_width(c->ops + pc);
  }
}

const pl_batch* pl_batch_at(const pl_code* c, uint32_t pc) {
  uint32_t lo = 0, hi = c->nbatches;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (c->batches[mid].start < pc)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < c->nbatches && c->batches[lo].start == pc ? c->batches + lo
                                                        : NULL;
}

bool pl_batch_reserve(pl_thread* t, pl_frame* fr, const pl_batch* b) {
  if (t->vsp < fr->argbase || t->vsp - fr->argbase < b->depth)
    return false;
  if (b->vars && b->vars > (fr->argc ? fr->argc : pl_env_n(pl_ptr(fr->a))))
    return false;
  while (t->vcap - t->vsp < b->extra)
    pl_vstack_grow(t);
  pl_gc_reserve(t, b->cells);
  return true;
}

void pl_batch_free(pl_code* c) {
  for (uint32_t i = 0; i < c->nbatches; i++)
    free(c->batches[i].steps);
  free(c->batches);
  free(c->batch_ops);
}
