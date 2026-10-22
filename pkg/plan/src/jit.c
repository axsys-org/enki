#include "jit.h"

#ifdef PL_SLJIT
#include <sljitLir.h>
#include "internal.h"
#include "plan/int63.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Native regions with guarded, bounded heap construction. There are no native
 * calls, GC safepoints, or surviving native continuations. Every exit spills
 * the operand stack and fuel, before executing the pending bytecode once in C.
 * Code and its dispatch overlay are private to one decoded pl_code version.
 * SLJIT memory is released together with that version, never on replacement.
 */
typedef sljit_uw(SLJIT_FUNC* jit_fn)(pl_thread*, pl_frame*, sljit_uw);
struct pl_jit {
  _Atomic(jit_fn) fn;
  _Atomic unsigned hits;
  _Atomic bool attempted;
  void* memory;
  sljit_uw* entries;
};

static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t compile_mu = PTHREAD_MUTEX_INITIALIZER;
static bool enabled, statistics;
static unsigned threshold = 32;
static _Atomic uint64_t compiled, failed, native_entries, compile_ns,
    code_bytes;

static uint64_t now_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void report(void) {
  fprintf(stderr,
          "SLJIT compiled=%llu failed=%llu entries=%llu compile_ns=%llu "
          "code_bytes=%llu\n",
          (unsigned long long)atomic_load(&compiled),
          (unsigned long long)atomic_load(&failed),
          (unsigned long long)atomic_load(&native_entries),
          (unsigned long long)atomic_load(&compile_ns),
          (unsigned long long)atomic_load(&code_bytes));
}

static void configure(void) {
  const char* v = getenv("PL_JIT");
  enabled = v == NULL || strcmp(v, "1") == 0;
  statistics = getenv("PL_JIT_STATS") != NULL;
  v = getenv("PL_JIT_THRESHOLD");
  if (v != NULL) {
    char* end;
    unsigned long n = strtoul(v, &end, 10);
    if (*v != 0 && *end == 0 && n > 0 && n <= UINT32_MAX)
      threshold = (unsigned)n;
  }
  if (statistics)
    (void)atexit(report);
}

/* Explicit allowlist: sets 66 and 83 also contain effects and evaluator
 * control. Dispatch is resolved from the descriptor, never from an untrusted
 * hint. */
typedef enum {
  JP_NONE,
  JP_Inc,
  JP_Dec,
  JP_Add,
  JP_Sub,
  JP_Mul,
  JP_Div,
  JP_Mod,
  JP_Rsh,
  JP_Lsh,
  JP_Test,
  JP_Nib,
  JP_Load8,
  JP_LoadVar,
  JP_Store8,
  JP_Set,
  JP_Clear,
  JP_Bex,
  JP_Trunc8,
  JP_Trunc16,
  JP_Trunc32,
  JP_Trunc64,
  JP_Trunc,
  JP_Bits,
  JP_Bytes,
  JP_Nil,
  JP_Truth,
  JP_Eq,
  JP_Ne,
  JP_Lt,
  JP_Le,
  JP_Gt,
  JP_Ge,
  JP_Cmp,
  JP_Nat,
  JP_Type,
  JP_IsPin,
  JP_IsLaw,
  JP_IsApp,
  JP_IsNat,
  JP_Arity,
  JP_Name,
  JP_Body,
  JP_Unpin,
  JP_Hd,
  JP_Sz,
  JP_Last,
  JP_Ix,
  JP_Ix0,
  JP_Ix1,
  JP_Ix2,
  JP_Ix3,
  JP_Ix4,
  JP_Ix5,
  JP_Ix6,
  JP_Ix7,
  JP_Or,
  JP_Nor,
  JP_And,
  JP_If,
  JP_Ifz,
  JP_Seq,
  JP_Seq2,
  JP_Seq3,
  JP_Case,
  JP_Case2,
  JP_Case3,
  JP_Case4,
  JP_Case5,
  JP_Case6,
  JP_Case7,
  JP_Case8,
  JP_Case9,
  JP_Case10,
  JP_Case11,
  JP_Case12,
  JP_Case13,
  JP_Case14,
  JP_Case15,
  JP_Case16,
  JP_Force,
  JP_DeepSeq,
  JP_Equal,
  JP_I63Inc,
  JP_I63Dec,
  JP_I63Neg,
  JP_I63Abs,
  JP_I63Add,
  JP_I63Sub,
  JP_I63Mul,
  JP_I63Div,
  JP_I63Mod,
  JP_I63Eq,
  JP_I63Ne,
  JP_I63Lt,
  JP_I63Le,
  JP_I63Gt,
  JP_I63Ge,
  JP_I63MulDiv,
} jit_prim;

static jit_prim primitive(const pl_op_t* o, const pl_opdesc** desc) {
  uint32_t idx;
  switch (o[0]) {
  case OP_CALL_KNOWN:
  case OP_CALL_READY:
  case OP_ADD:
  case OP_SUB:
  case OP_CMP:
    idx = (uint32_t)o[2];
    break;
  case OP_TAILCALL:
  case OP_TAIL_READY:
  case OP_TAIL_ADD:
  case OP_TAIL_SUB:
  case OP_TAIL_CMP:
  case OP_MK_THK:
    if ((o[2] & PL_BAN_MASK) != PL_BAN_PRIM_KNOWN)
      return JP_NONE;
    /* Only speculate where the interpreter already permits eagerness. */
    if (o[0] == OP_MK_THK && o[4] == PL_SPEC_NONE)
      return JP_NONE;
    idx = (uint32_t)o[3];
    break;
  default:
    return JP_NONE;
  }
  const pl_opdesc* d = &pl_ops[idx];
  if (d->opset == 83 && d->name_c != NULL) {
    static const struct {
      const char* name;
      jit_prim prim;
    } signed_ops[] = {
        {"I63Inc", JP_I63Inc}, {"I63Dec", JP_I63Dec},
        {"I63Neg", JP_I63Neg}, {"I63Abs", JP_I63Abs},
        {"I63Add", JP_I63Add}, {"I63Sub", JP_I63Sub},
        {"I63Mul", JP_I63Mul}, {"I63Div", JP_I63Div},
        {"I63Mod", JP_I63Mod}, {"I63Eq", JP_I63Eq},
        {"I63Ne", JP_I63Ne},   {"I63Lt", JP_I63Lt},
        {"I63Le", JP_I63Le},   {"I63Gt", JP_I63Gt},
        {"I63Ge", JP_I63Ge},   {"I63MulDiv", JP_I63MulDiv},
    };
    for (size_t k = 0; k < sizeof(signed_ops) / sizeof(signed_ops[0]); k++) {
      if (strcmp(d->name_c, signed_ops[k].name) == 0) {
        *desc = d;
        return signed_ops[k].prim;
      }
    }
  }
  if (d->opset != 66)
    return JP_NONE;
  *desc = d;
  switch (d->name) {
  case ax_s3('I', 'n', 'c'):
    return JP_Inc;
  case ax_s3('D', 'e', 'c'):
    return JP_Dec;
  case ax_s3('A', 'd', 'd'):
    return JP_Add;
  case ax_s3('S', 'u', 'b'):
    return JP_Sub;
  case ax_s3('M', 'u', 'l'):
    return JP_Mul;
  case ax_s3('D', 'i', 'v'):
    return JP_Div;
  case ax_s3('M', 'o', 'd'):
    return JP_Mod;
  case ax_s3('R', 's', 'h'):
    return JP_Rsh;
  case ax_s3('L', 's', 'h'):
    return JP_Lsh;
  case ax_s4('T', 'e', 's', 't'):
    return JP_Test;
  case ax_s3('N', 'i', 'b'):
    return JP_Nib;
  case ax_s5('L', 'o', 'a', 'd', '8'):
    return JP_Load8;
  case ax_s7('L', 'o', 'a', 'd', 'V', 'a', 'r'):
    return JP_LoadVar;
  case ax_s6('S', 't', 'o', 'r', 'e', '8'):
    return JP_Store8;
  case ax_s3('S', 'e', 't'):
    return JP_Set;
  case ax_s5('C', 'l', 'e', 'a', 'r'):
    return JP_Clear;
  case ax_s3('B', 'e', 'x'):
    return JP_Bex;
  case ax_s6('T', 'r', 'u', 'n', 'c', '8'):
    return JP_Trunc8;
  case ax_s7('T', 'r', 'u', 'n', 'c', '1', '6'):
    return JP_Trunc16;
  case ax_s7('T', 'r', 'u', 'n', 'c', '3', '2'):
    return JP_Trunc32;
  case ax_s7('T', 'r', 'u', 'n', 'c', '6', '4'):
    return JP_Trunc64;
  case ax_s5('T', 'r', 'u', 'n', 'c'):
    return JP_Trunc;
  case ax_s4('B', 'i', 't', 's'):
    return JP_Bits;
  case ax_s5('B', 'y', 't', 'e', 's'):
    return JP_Bytes;
  case ax_s3('N', 'i', 'l'):
    return JP_Nil;
  case ax_s5('T', 'r', 'u', 't', 'h'):
    return JP_Truth;
  case ax_s2('E', 'q'):
    return JP_Eq;
  case ax_s2('N', 'e'):
    return JP_Ne;
  case ax_s2('L', 't'):
    return JP_Lt;
  case ax_s2('L', 'e'):
    return JP_Le;
  case ax_s2('G', 't'):
    return JP_Gt;
  case ax_s2('G', 'e'):
    return JP_Ge;
  case ax_s3('C', 'm', 'p'):
    return JP_Cmp;
  case ax_s3('N', 'a', 't'):
    return JP_Nat;
  case ax_s4('T', 'y', 'p', 'e'):
    return JP_Type;
  case ax_s5('I', 's', 'P', 'i', 'n'):
    return JP_IsPin;
  case ax_s5('I', 's', 'L', 'a', 'w'):
    return JP_IsLaw;
  case ax_s5('I', 's', 'A', 'p', 'p'):
    return JP_IsApp;
  case ax_s5('I', 's', 'N', 'a', 't'):
    return JP_IsNat;
  case ax_s5('A', 'r', 'i', 't', 'y'):
    return JP_Arity;
  case ax_s4('N', 'a', 'm', 'e'):
    return JP_Name;
  case ax_s4('B', 'o', 'd', 'y'):
    return JP_Body;
  case ax_s5('U', 'n', 'p', 'i', 'n'):
    return JP_Unpin;
  case ax_s2('H', 'd'):
    return JP_Hd;
  case ax_s2('S', 'z'):
    return JP_Sz;
  case ax_s4('L', 'a', 's', 't'):
    return JP_Last;
  case ax_s2('I', 'x'):
    return JP_Ix;
  case ax_s3('I', 'x', '0'):
    return JP_Ix0;
  case ax_s3('I', 'x', '1'):
    return JP_Ix1;
  case ax_s3('I', 'x', '2'):
    return JP_Ix2;
  case ax_s3('I', 'x', '3'):
    return JP_Ix3;
  case ax_s3('I', 'x', '4'):
    return JP_Ix4;
  case ax_s3('I', 'x', '5'):
    return JP_Ix5;
  case ax_s3('I', 'x', '6'):
    return JP_Ix6;
  case ax_s3('I', 'x', '7'):
    return JP_Ix7;
  case ax_s2('O', 'r'):
    return JP_Or;
  case ax_s3('N', 'o', 'r'):
    return JP_Nor;
  case ax_s3('A', 'n', 'd'):
    return JP_And;
  case ax_s2('I', 'f'):
    return JP_If;
  case ax_s3('I', 'f', 'z'):
    return JP_Ifz;
  case ax_s3('S', 'e', 'q'):
    return JP_Seq;
  case ax_s4('S', 'e', 'q', '2'):
    return JP_Seq2;
  case ax_s4('S', 'e', 'q', '3'):
    return JP_Seq3;
  case ax_s4('C', 'a', 's', 'e'):
    return JP_Case;
  case ax_s5('C', 'a', 's', 'e', '2'):
    return JP_Case2;
  case ax_s5('C', 'a', 's', 'e', '3'):
    return JP_Case3;
  case ax_s5('C', 'a', 's', 'e', '4'):
    return JP_Case4;
  case ax_s5('C', 'a', 's', 'e', '5'):
    return JP_Case5;
  case ax_s5('C', 'a', 's', 'e', '6'):
    return JP_Case6;
  case ax_s5('C', 'a', 's', 'e', '7'):
    return JP_Case7;
  case ax_s5('C', 'a', 's', 'e', '8'):
    return JP_Case8;
  case ax_s5('C', 'a', 's', 'e', '9'):
    return JP_Case9;
  case ax_s6('C', 'a', 's', 'e', '1', '0'):
    return JP_Case10;
  case ax_s6('C', 'a', 's', 'e', '1', '1'):
    return JP_Case11;
  case ax_s6('C', 'a', 's', 'e', '1', '2'):
    return JP_Case12;
  case ax_s6('C', 'a', 's', 'e', '1', '3'):
    return JP_Case13;
  case ax_s6('C', 'a', 's', 'e', '1', '4'):
    return JP_Case14;
  case ax_s6('C', 'a', 's', 'e', '1', '5'):
    return JP_Case15;
  case ax_s6('C', 'a', 's', 'e', '1', '6'):
    return JP_Case16;
  case ax_s5('F', 'o', 'r', 'c', 'e'):
    return JP_Force;
  case ax_s7('D', 'e', 'e', 'p', 'S', 'e', 'q'):
    return JP_DeepSeq;
  case ax_s5('E', 'q', 'u', 'a', 'l'):
    return JP_Equal;
  default:
    return JP_NONE;
  }
}

static bool supported(const pl_op_t* o) {
  const pl_opdesc* d;
  if (primitive(o, &d) != JP_NONE)
    return true;
  switch (o[0]) {
  case OP_PUSH_VAR:
  case OP_PUSH_SLOT:
    return o[1] <= UINT32_MAX;
  case OP_PUSH_LIT:
  case OP_ADD:
  case OP_SUB:
  case OP_CMP:
  case OP_FORCE:
  case OP_FORCE_READY:
  case OP_ENTRY:
  case OP_NOP:
  case OP_JMP:
    return true;
  case OP_BR:
    return o[1] <= 32;
  case OP_TAILBLK:
    return o[2] <= 32;
  default:
    return false;
  }
}

void pl_jit_prepare(pl_code* c) {
  (void)pthread_once(&config_once, configure);
  if (!enabled || c->jit != NULL || c->nops > 8192 || c->nops == 0)
    return;
  /* Instrument runs of at least three supported instructions, or a complete
   * construction batch. Compilation also traverses shorter internal runs. */
  pl_op_t* overlay = malloc(c->nops * sizeof(*overlay));
  struct pl_jit* j = calloc(1, sizeof(*j));
  if (overlay == NULL || j == NULL) {
    free(overlay);
    free(j);
    return;
  }
  memcpy(overlay, pl_batch_ops(c), c->nops * sizeof(*overlay));
  size_t count = 0, start = 0;
  bool any = false;
  for (size_t pc = 0; pc <= c->nops;) {
    bool batch = pc < c->nops && pl_batch_ops(c)[pc] == OP_BUILD;
    if (pc < c->nops && (batch || supported(c->ops + pc))) {
      if (count == 0)
        start = pc;
      count += batch ? 3 : 1;
    } else {
      if (count >= 3) {
        any = true;
        for (size_t k = start; k < pc;) {
          overlay[k] = OP_JIT;
          const pl_batch* b = pl_batch_at(c, (uint32_t)k);
          k = b ? b->end : k + pl_bytecode_width(c->ops + k);
        }
      }
      count = 0;
    }
    if (pc == c->nops)
      break;
    pc = batch ? pl_batch_at(c, (uint32_t)pc)->end
               : pc + pl_bytecode_width(c->ops + pc);
  }
  if (!any) {
    free(overlay);
    free(j);
    return;
  }
  atomic_init(&j->fn, NULL);
  atomic_init(&j->hits, 0);
  atomic_init(&j->attempted, false);
  c->jit = j;
  c->jit_ops = overlay;
}

/* Register convention: S0 thread, S1 frame, S2 vstack base, S3 stack end,
 * S4 fuel, S5 operand base; R3 caches the top operand within a basic block.
 * R0-R2 and R4-R6 are scratch. No heap pointers survive return to the VM.
 */
#define T                SLJIT_S0
#define F                SLJIT_S1
#define VS               SLJIT_S2
#define SP               SLJIT_S3
#define FUEL             SLJIT_S4
#define BASE             SLJIT_S5
#define TOP              SLJIT_R3
#define M                SLJIT_MEM1
#define I                SLJIT_IMM
#define OFF(type, field) ((sljit_sw)offsetof(type, field))

typedef struct {
  struct sljit_jump* jump;
  uint32_t pc;
  bool cached;
} exit_patch;
typedef struct {
  struct sljit_jump* jump;
  uint32_t target;
} edge_patch;
typedef struct {
  struct sljit_compiler* c;
  exit_patch* exits;
  edge_patch* edges;
  size_t nexits, nedges;
  uint32_t pc;
  bool cached;
} emitter;

static void mov(emitter* e, int dst, sljit_sw dw, int src, sljit_sw sw) {
  (void)sljit_emit_op1(e->c, SLJIT_MOV, dst, dw, src, sw);
}
static void binary(emitter* e, int op, int dst, int a, sljit_sw aw, int b,
                   sljit_sw bw) {
  (void)sljit_emit_op2(e->c, op, dst, 0, a, aw, b, bw);
}
static void flush(emitter* e) {
  if (e->cached)
    mov(e, M(SP), -8, TOP, 0);
}
static void leave(emitter* e, uint32_t pc, bool cached) {
  if (cached)
    mov(e, M(SP), -8, TOP, 0);
  binary(e, SLJIT_SUB, SLJIT_R0, SP, 0, VS, 0);
  binary(e, SLJIT_LSHR, SLJIT_R0, SLJIT_R0, 0, I, 3);
  mov(e, M(T), OFF(pl_thread, vsp), SLJIT_R0, 0);
  mov(e, M(T), OFF(pl_thread, fuel), FUEL, 0);
  (void)sljit_emit_return(e->c, SLJIT_MOV, I, (sljit_sw)pc);
}
static void guard(emitter* e, int cond, int a, sljit_sw aw, int b,
                  sljit_sw bw) {
  e->exits[e->nexits++] =
      (exit_patch){sljit_emit_cmp(e->c, cond, a, aw, b, bw), e->pc, e->cached};
}
static void edge(emitter* e, uint32_t target) {
  e->edges[e->nedges++] =
      (edge_patch){sljit_emit_jump(e->c, SLJIT_JUMP), target};
}
static void need(emitter* e, unsigned n) {
  binary(e, SLJIT_ADD, SLJIT_R0, BASE, 0, I, (sljit_sw)n * 8);
  guard(e, SLJIT_LESS, SP, 0, SLJIT_R0, 0);
}
static void top(emitter* e) {
  if (!e->cached) {
    mov(e, TOP, 0, M(SP), -8);
    e->cached = true;
  }
}
static void push(emitter* e) {
  flush(e);
  mov(e, TOP, 0, SLJIT_R0, 0);
  binary(e, SLJIT_ADD, SP, SP, 0, I, 8);
  e->cached = true;
}
static void room(emitter* e) {
  mov(e, SLJIT_R0, 0, M(T), OFF(pl_thread, vcap));
  binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 3);
  binary(e, SLJIT_ADD, SLJIT_R0, VS, 0, SLJIT_R0, 0);
  guard(e, SLJIT_GREATER_EQUAL, SP, 0, SLJIT_R0, 0);
}

/* Primitive emitters keep TOP and the stack intact until all guards pass.
 * R0/R1/R4 hold arguments, R2 the result, R5/R6 temporary values. A guard
 * resumes the original call, including forcing, exceptions and big nats. */
static void whnf(emitter* e, int r) {
  binary(e, SLJIT_LSHR, SLJIT_R5, r, 0, I, 56);
  guard(e, SLJIT_EQUAL, SLJIT_R5, 0, I, PL_TAG_DEFER);
}

static void small_nat(emitter* e, int r) {
  /* Direct nats need one check. On the cold path, WHNF non-nats coerce to
   * zero; boxed nats and deferred values resume the original primitive. */
  struct sljit_jump* nat =
      sljit_emit_cmp(e->c, SLJIT_SIG_GREATER_EQUAL, r, 0, I, 0);
  binary(e, SLJIT_LSHR, SLJIT_R5, r, 0, I, 56);
  guard(e, SLJIT_EQUAL, SLJIT_R5, 0, I, PL_TAG_NAT);
  guard(e, SLJIT_EQUAL, SLJIT_R5, 0, I, PL_TAG_DEFER);
  mov(e, r, 0, I, 0);
  sljit_set_label(nat, sljit_emit_label(e->c));
}

static void boolean(emitter* e, int cond, int a, sljit_sw aw, int b,
                    sljit_sw bw) {
  mov(e, SLJIT_R2, 0, I, 0);
  struct sljit_jump* no = sljit_emit_cmp(e->c, cond ^ 1, a, aw, b, bw);
  mov(e, SLJIT_R2, 0, I, 1);
  sljit_set_label(no, sljit_emit_label(e->c));
}

/* Decode zigzag to a sign-extended machine word. R5 is scratch. */
static void int63_decode(emitter* e, int r) {
  binary(e, SLJIT_AND, SLJIT_R5, r, 0, I, 1);
  binary(e, SLJIT_SUB, SLJIT_R5, I, 0, SLJIT_R5, 0);
  binary(e, SLJIT_LSHR, r, r, 0, I, 1);
  binary(e, SLJIT_XOR, r, r, 0, SLJIT_R5, 0);
}

/* Encode the low 63 bits of R2, using bit 62 as the wrapped sign. */
static void int63_encode(emitter* e) {
  binary(e, SLJIT_SHL, SLJIT_R2, SLJIT_R2, 0, I, 1);
  binary(e, SLJIT_ASHR, SLJIT_R5, SLJIT_R2, 0, I, 63);
  binary(e, SLJIT_XOR, SLJIT_R2, SLJIT_R2, 0, SLJIT_R5, 0);
  binary(e, SLJIT_AND, SLJIT_R2, SLJIT_R2, 0, I, PL_NAT63_MAX);
}

/* Magnitude of a decoded int63, including MIN. R5 is scratch. */
static void int63_magnitude(emitter* e, int r) {
  binary(e, SLJIT_ASHR, SLJIT_R5, r, 0, I, 63);
  binary(e, SLJIT_XOR, r, r, 0, SLJIT_R5, 0);
  binary(e, SLJIT_SUB, r, r, 0, SLJIT_R5, 0);
}

/* Exact unsigned 128-bit product / 63-bit magnitude. Keep the low quotient
 * word and exact remainder: higher quotient bits cannot affect rounding or
 * the final modulo-2^63 result. No calls, allocation, or unbounded loops. */
static void int63_muldiv(emitter* e) {
  int63_decode(e, SLJIT_R4);
  guard(e, SLJIT_EQUAL, SLJIT_R4, 0, I, 0);
  guard(e, SLJIT_GREATER, SLJIT_R6, 0, I, PL_INT63_ROUND_EVEN);
  int63_magnitude(e, SLJIT_R0);
  int63_magnitude(e, SLJIT_R1);
  int63_magnitude(e, SLJIT_R4);
  (void)sljit_emit_op0(e->c, SLJIT_LMUL_UW); /* R1:R0 = abs(a*b) */
  struct sljit_jump* wide =
      sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, I, 0);
  mov(e, SLJIT_R1, 0, SLJIT_R4, 0);
  (void)sljit_emit_op0(e->c, SLJIT_DIVMOD_UW);
  mov(e, SLJIT_R2, 0, SLJIT_R0, 0);
  struct sljit_jump* divided = sljit_emit_jump(e->c, SLJIT_JUMP);

  sljit_set_label(wide, sljit_emit_label(e->c));
  mov(e, SLJIT_R2, 0, SLJIT_R0, 0); /* retain low product word */
  mov(e, SLJIT_R0, 0, SLJIT_R1, 0);
  mov(e, SLJIT_R1, 0, SLJIT_R4, 0);
  (void)sljit_emit_op0(e->c, SLJIT_DIVMOD_UW); /* high word % divisor */
  mov(e, SLJIT_R0, 0, SLJIT_R2, 0);
  mov(e, SLJIT_R2, 0, I, 0);
  mov(e, SLJIT_R6, 0, I, 64);
  struct sljit_label* loop = sljit_emit_label(e->c);
  binary(e, SLJIT_LSHR, SLJIT_R5, SLJIT_R0, 0, I, 63);
  binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 1);
  /* remainder < divisor <= 2^62, so this shift cannot overflow. */
  binary(e, SLJIT_SHL, SLJIT_R1, SLJIT_R1, 0, I, 1);
  binary(e, SLJIT_OR, SLJIT_R1, SLJIT_R1, 0, SLJIT_R5, 0);
  binary(e, SLJIT_SHL, SLJIT_R2, SLJIT_R2, 0, I, 1);
  struct sljit_jump* below =
      sljit_emit_cmp(e->c, SLJIT_LESS, SLJIT_R1, 0, SLJIT_R4, 0);
  binary(e, SLJIT_SUB, SLJIT_R1, SLJIT_R1, 0, SLJIT_R4, 0);
  binary(e, SLJIT_OR, SLJIT_R2, SLJIT_R2, 0, I, 1);
  sljit_set_label(below, sljit_emit_label(e->c));
  binary(e, SLJIT_SUB, SLJIT_R6, SLJIT_R6, 0, I, 1);
  sljit_set_label(sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R6, 0, I, 0),
                  loop);
  sljit_set_label(divided, sljit_emit_label(e->c));

  /* The original encoded operands still hold their sign bits. */
  binary(e, SLJIT_XOR, SLJIT_R6, M(SP), -32, M(SP), -24);
  binary(e, SLJIT_XOR, SLJIT_R6, SLJIT_R6, 0, M(SP), -16);
  binary(e, SLJIT_AND, SLJIT_R6, SLJIT_R6, 0, I, 1);
  mov(e, SLJIT_R0, 0, M(SP), -8); /* plain rounding mode */
  struct sljit_jump* done[6];
  struct sljit_jump* increment[3];
  done[0] = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R1, 0, I, 0);
  done[1] =
      sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R0, 0, I, PL_INT63_ROUND_ZERO);
  struct sljit_jump* not_floor = sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R0,
                                                0, I, PL_INT63_ROUND_FLOOR);
  done[2] = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R6, 0, I, 0);
  increment[0] = sljit_emit_jump(e->c, SLJIT_JUMP);
  sljit_set_label(not_floor, sljit_emit_label(e->c));
  struct sljit_jump* not_ceil = sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R0,
                                               0, I, PL_INT63_ROUND_CEIL);
  done[3] = sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R6, 0, I, 0);
  increment[1] = sljit_emit_jump(e->c, SLJIT_JUMP);
  sljit_set_label(not_ceil, sljit_emit_label(e->c));
  binary(e, SLJIT_SHL, SLJIT_R1, SLJIT_R1, 0, I, 1);
  done[4] = sljit_emit_cmp(e->c, SLJIT_LESS, SLJIT_R1, 0, SLJIT_R4, 0);
  increment[2] = sljit_emit_cmp(e->c, SLJIT_GREATER, SLJIT_R1, 0, SLJIT_R4, 0);
  binary(e, SLJIT_AND, SLJIT_R5, SLJIT_R2, 0, I, 1);
  done[5] = sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R5, 0, I, 0);
  struct sljit_label* up = sljit_emit_label(e->c);
  for (size_t k = 0; k < 3; k++)
    sljit_set_label(increment[k], up);
  binary(e, SLJIT_ADD, SLJIT_R2, SLJIT_R2, 0, I, 1);
  struct sljit_label* rounded = sljit_emit_label(e->c);
  for (size_t k = 0; k < 6; k++)
    sljit_set_label(done[k], rounded);
  binary(e, SLJIT_SUB, SLJIT_R6, I, 0, SLJIT_R6, 0);
  binary(e, SLJIT_XOR, SLJIT_R2, SLJIT_R2, 0, SLJIT_R6, 0);
  binary(e, SLJIT_SUB, SLJIT_R2, SLJIT_R2, 0, SLJIT_R6, 0);
}

static void emit_primitive(emitter* e, const pl_op_t* o, jit_prim prim,
                           const pl_opdesc* d) {
  unsigned argc = d->argc;
  bool thunk = o[0] == OP_MK_THK;
  bool tail = o[0] == OP_TAILCALL || o[0] == OP_TAIL_READY ||
              o[0] == OP_TAIL_ADD || o[0] == OP_TAIL_SUB || o[0] == OP_TAIL_CMP;
  unsigned steps =
      thunk ? 0
            : 1 + (unsigned)__builtin_popcount(d->strict_mask) + (unsigned)tail;
  if (prim == JP_Equal)
    steps += 2; /* deep-normalize both already-terminal nats */
  if (prim == JP_DeepSeq)
    steps++; /* evaluate the first argument, then the selected result */
  need(e, argc);
  if (steps)
    guard(e, SLJIT_LESS_EQUAL, FUEL, 0, I, steps);
  flush(e);
  mov(e, SLJIT_R0, 0, M(SP), -(sljit_sw)argc * 8);
  if (argc > 1)
    mov(e, SLJIT_R1, 0, M(SP), (1 - (sljit_sw)argc) * 8);
  if (argc > 2)
    mov(e, SLJIT_R4, 0, M(SP), (2 - (sljit_sw)argc) * 8);
  if (prim == JP_I63MulDiv)
    mov(e, SLJIT_R6, 0, M(SP), -8);
  bool signed63 = prim >= JP_I63Inc && prim <= JP_I63MulDiv;
  bool numeric =
      signed63 || prim <= JP_Bytes || (prim >= JP_Eq && prim <= JP_Cmp);
  if (signed63) {
    (void)sljit_emit_op1(e->c, SLJIT_MOV_U8, SLJIT_R5, 0, M(T),
                         OFF(pl_thread, rplan_f));
    guard(e, SLJIT_EQUAL, SLJIT_R5, 0, I, 0);
  }
  if (numeric) {
    const int args[] = {SLJIT_R0, SLJIT_R1, SLJIT_R4, SLJIT_R6};
    for (unsigned k = 0; k < argc; k++) {
      if (thunk || signed63)
        guard(e, SLJIT_SIG_LESS, args[k], 0, I, 0);
      else
        small_nat(e, args[k]);
    }
  } else {
    for (unsigned k = 0; k < argc; k++) {
      if ((d->strict_mask >> k) & 1u) {
        mov(e, SLJIT_R6, 0, M(SP), ((sljit_sw)k - argc) * 8);
        if (thunk && o[4] != PL_SPEC_BODY)
          guard(e, SLJIT_SIG_LESS, SLJIT_R6, 0, I, 0);
        else
          whnf(e, SLJIT_R6);
      }
    }
  }
  if (signed63) {
    int63_decode(e, SLJIT_R0);
    if (argc > 1)
      int63_decode(e, SLJIT_R1);
  }
  switch (prim) {
  case JP_I63MulDiv:
    int63_muldiv(e);
    break;
  case JP_I63Inc:
  case JP_I63Add:
    binary(e, SLJIT_ADD, SLJIT_R2, SLJIT_R0, 0,
           prim == JP_I63Inc ? I : SLJIT_R1, prim == JP_I63Inc ? 1 : 0);
    break;
  case JP_I63Dec:
  case JP_I63Sub:
    binary(e, SLJIT_SUB, SLJIT_R2, SLJIT_R0, 0,
           prim == JP_I63Dec ? I : SLJIT_R1, prim == JP_I63Dec ? 1 : 0);
    break;
  case JP_I63Neg:
    binary(e, SLJIT_SUB, SLJIT_R2, I, 0, SLJIT_R0, 0);
    break;
  case JP_I63Abs:
    binary(e, SLJIT_ASHR, SLJIT_R5, SLJIT_R0, 0, I, 63);
    binary(e, SLJIT_XOR, SLJIT_R2, SLJIT_R0, 0, SLJIT_R5, 0);
    binary(e, SLJIT_SUB, SLJIT_R2, SLJIT_R2, 0, SLJIT_R5, 0);
    break;
  case JP_I63Mul:
    /* Only the low 63 bits survive; machine-word overflow is intentional. */
    binary(e, SLJIT_MUL, SLJIT_R2, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_I63Div:
  case JP_I63Mod:
    guard(e, SLJIT_EQUAL, SLJIT_R1, 0, I, 0);
    /* -2^62 / -1 fits a machine word, then wraps during encoding. */
    (void)sljit_emit_op0(e->c,
                         prim == JP_I63Div ? SLJIT_DIV_SW : SLJIT_DIVMOD_SW);
    mov(e, SLJIT_R2, 0, prim == JP_I63Div ? SLJIT_R0 : SLJIT_R1, 0);
    break;
  case JP_I63Eq:
    boolean(e, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_I63Ne:
    boolean(e, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_I63Lt:
    boolean(e, SLJIT_SIG_LESS, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_I63Le:
    boolean(e, SLJIT_SIG_LESS_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_I63Gt:
    boolean(e, SLJIT_SIG_GREATER, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_I63Ge:
    boolean(e, SLJIT_SIG_GREATER_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_Inc:
  case JP_Add:
    binary(e, SLJIT_ADD, SLJIT_R2, SLJIT_R0, 0, prim == JP_Inc ? I : SLJIT_R1,
           prim == JP_Inc ? 1 : 0);
    guard(e, SLJIT_SIG_LESS, SLJIT_R2, 0, I, 0);
    break;
  case JP_Dec:
  case JP_Sub:
    binary(e, SLJIT_SUB | SLJIT_SET_LESS, SLJIT_R2, SLJIT_R0, 0,
           prim == JP_Dec ? I : SLJIT_R1, prim == JP_Dec ? 1 : 0);
    (void)sljit_emit_select(e->c, SLJIT_LESS, SLJIT_R2, I, 0, SLJIT_R2);
    break;
  case JP_Mul:
    binary(e, SLJIT_MUL | SLJIT_SET_OVERFLOW, SLJIT_R2, SLJIT_R0, 0, SLJIT_R1,
           0);
    e->exits[e->nexits++] =
        (exit_patch){sljit_emit_jump(e->c, SLJIT_OVERFLOW), e->pc, e->cached};
    guard(e, SLJIT_SIG_LESS, SLJIT_R2, 0, I, 0);
    break;
  case JP_Div:
  case JP_Mod:
    guard(e, SLJIT_EQUAL, SLJIT_R1, 0, I, 0);
    (void)sljit_emit_op0(e->c, prim == JP_Div ? SLJIT_DIV_UW : SLJIT_DIVMOD_UW);
    mov(e, SLJIT_R2, 0, prim == JP_Div ? SLJIT_R0 : SLJIT_R1, 0);
    break;
  case JP_Lsh:
    guard(e, SLJIT_GREATER_EQUAL, SLJIT_R1, 0, I, 63);
    binary(e, SLJIT_LSHR, SLJIT_R5, I, PL_NAT63_MAX, SLJIT_R1, 0);
    guard(e, SLJIT_GREATER, SLJIT_R0, 0, SLJIT_R5, 0);
    binary(e, SLJIT_SHL, SLJIT_R2, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_Rsh: {
    mov(e, SLJIT_R2, 0, I, 0);
    struct sljit_jump* large =
        sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R1, 0, I, 63);
    binary(e, SLJIT_LSHR, SLJIT_R2, SLJIT_R0, 0, SLJIT_R1, 0);
    sljit_set_label(large, sljit_emit_label(e->c));
    break;
  }
  case JP_Bex:
  case JP_Set:
    guard(e, SLJIT_GREATER_EQUAL, SLJIT_R0, 0, I, 63);
    binary(e, SLJIT_SHL, SLJIT_R2, I, 1, SLJIT_R0, 0);
    if (prim == JP_Set)
      binary(e, SLJIT_OR, SLJIT_R2, SLJIT_R2, 0, SLJIT_R1, 0);
    break;
  case JP_Clear:
  case JP_Test:
  case JP_Nib:
  case JP_Load8: {
    unsigned unit = prim == JP_Nib ? 4 : prim == JP_Load8 ? 8 : 1;
    mov(e, SLJIT_R2, 0, prim == JP_Clear ? SLJIT_R1 : I, 0);
    struct sljit_jump* large =
        sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R0, 0, I, 64 / unit);
    if (unit != 1)
      binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, unit == 4 ? 2 : 3);
    if (prim == JP_Clear) {
      binary(e, SLJIT_SHL, SLJIT_R5, I, 1, SLJIT_R0, 0);
      binary(e, SLJIT_XOR, SLJIT_R5, SLJIT_R5, 0, I, -1);
      binary(e, SLJIT_AND, SLJIT_R2, SLJIT_R1, 0, SLJIT_R5, 0);
    } else {
      binary(e, SLJIT_LSHR, SLJIT_R2, SLJIT_R1, 0, SLJIT_R0, 0);
      binary(e, SLJIT_AND, SLJIT_R2, SLJIT_R2, 0, I, (1 << unit) - 1);
    }
    sljit_set_label(large, sljit_emit_label(e->c));
    break;
  }
  case JP_Trunc8:
  case JP_Trunc16:
  case JP_Trunc32:
  case JP_Trunc64: {
    sljit_sw mask = prim == JP_Trunc8    ? 0xff
                    : prim == JP_Trunc16 ? 0xffff
                    : prim == JP_Trunc32 ? UINT32_MAX
                                         : PL_NAT63_MAX;
    binary(e, SLJIT_AND, SLJIT_R2, SLJIT_R0, 0, I, mask);
    break;
  }
  case JP_Trunc: {
    mov(e, SLJIT_R2, 0, SLJIT_R1, 0);
    struct sljit_jump* large =
        sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R0, 0, I, 63);
    binary(e, SLJIT_SHL, SLJIT_R5, I, 1, SLJIT_R0, 0);
    binary(e, SLJIT_SUB, SLJIT_R5, SLJIT_R5, 0, I, 1);
    binary(e, SLJIT_AND, SLJIT_R2, SLJIT_R1, 0, SLJIT_R5, 0);
    sljit_set_label(large, sljit_emit_label(e->c));
    break;
  }
  case JP_LoadVar: {
    mov(e, SLJIT_R2, 0, I, 0);
    struct sljit_jump* large =
        sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R0, 0, I, 8);
    binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 3);
    binary(e, SLJIT_LSHR, SLJIT_R2, SLJIT_R4, 0, SLJIT_R0, 0);
    struct sljit_jump* wide =
        sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R1, 0, I, 8);
    binary(e, SLJIT_SHL, SLJIT_R1, SLJIT_R1, 0, I, 3);
    binary(e, SLJIT_SHL, SLJIT_R5, I, 1, SLJIT_R1, 0);
    binary(e, SLJIT_SUB, SLJIT_R5, SLJIT_R5, 0, I, 1);
    binary(e, SLJIT_AND, SLJIT_R2, SLJIT_R2, 0, SLJIT_R5, 0);
    struct sljit_label* end = sljit_emit_label(e->c);
    sljit_set_label(large, end);
    sljit_set_label(wide, end);
    break;
  }
  case JP_Store8:
    guard(e, SLJIT_GREATER_EQUAL, SLJIT_R0, 0, I, 8);
    binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 3);
    binary(e, SLJIT_AND, SLJIT_R1, SLJIT_R1, 0, I, 0xff);
    binary(e, SLJIT_SHL, SLJIT_R1, SLJIT_R1, 0, SLJIT_R0, 0);
    binary(e, SLJIT_SHL, SLJIT_R5, I, 0xff, SLJIT_R0, 0);
    binary(e, SLJIT_XOR, SLJIT_R5, SLJIT_R5, 0, I, -1);
    binary(e, SLJIT_AND, SLJIT_R2, SLJIT_R4, 0, SLJIT_R5, 0);
    binary(e, SLJIT_OR, SLJIT_R2, SLJIT_R2, 0, SLJIT_R1, 0);
    guard(e, SLJIT_SIG_LESS, SLJIT_R2, 0, I, 0);
    break;
  case JP_Bits:
  case JP_Bytes:
    (void)sljit_emit_op1(e->c, SLJIT_CLZ, SLJIT_R2, 0, SLJIT_R0, 0);
    binary(e, SLJIT_SUB, SLJIT_R2, I, 64, SLJIT_R2, 0);
    if (prim == JP_Bytes) {
      binary(e, SLJIT_ADD, SLJIT_R2, SLJIT_R2, 0, I, 7);
      binary(e, SLJIT_LSHR, SLJIT_R2, SLJIT_R2, 0, I, 3);
    }
    break;
  case JP_Eq:
  case JP_Ne:
  case JP_Lt:
  case JP_Le:
  case JP_Gt:
  case JP_Ge: {
    static const int conditions[] = {SLJIT_EQUAL,   SLJIT_NOT_EQUAL,
                                     SLJIT_LESS,    SLJIT_LESS_EQUAL,
                                     SLJIT_GREATER, SLJIT_GREATER_EQUAL};
    boolean(e, conditions[prim - JP_Eq], SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  }
  case JP_Cmp: {
    mov(e, SLJIT_R2, 0, I, 0);
    struct sljit_jump* less =
        sljit_emit_cmp(e->c, SLJIT_LESS, SLJIT_R0, 0, SLJIT_R1, 0);
    mov(e, SLJIT_R2, 0, I, 1);
    struct sljit_jump* equal =
        sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
    mov(e, SLJIT_R2, 0, I, 2);
    struct sljit_label* end = sljit_emit_label(e->c);
    sljit_set_label(less, end);
    sljit_set_label(equal, end);
    break;
  }
  case JP_Nil:
  case JP_Truth:
    boolean(e, prim == JP_Nil ? SLJIT_EQUAL : SLJIT_NOT_EQUAL, SLJIT_R0, 0, I,
            0);
    break;
  case JP_Nat:
  case JP_IsNat: {
    mov(e, SLJIT_R2, 0, prim == JP_Nat ? SLJIT_R0 : I, prim == JP_Nat ? 0 : 1);
    struct sljit_jump* direct =
        sljit_emit_cmp(e->c, SLJIT_SIG_GREATER_EQUAL, SLJIT_R0, 0, I, 0);
    binary(e, SLJIT_LSHR, SLJIT_R5, SLJIT_R0, 0, I, 56);
    struct sljit_jump* boxed =
        sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R5, 0, I, PL_TAG_NAT);
    mov(e, SLJIT_R2, 0, I, 0);
    struct sljit_label* end = sljit_emit_label(e->c);
    sljit_set_label(direct, end);
    sljit_set_label(boxed, end);
    break;
  }
  case JP_Type:
  case JP_IsPin:
  case JP_IsLaw:
  case JP_IsApp:
    binary(e, SLJIT_LSHR, SLJIT_R5, SLJIT_R0, 0, I, 56);
    if (prim != JP_Type) {
      boolean(e, SLJIT_EQUAL, SLJIT_R5, 0, I,
              prim == JP_IsPin   ? PL_TAG_PIN
              : prim == JP_IsLaw ? PL_TAG_LAW
                                 : PL_TAG_APP);
    } else {
      mov(e, SLJIT_R2, 0, I, 0);
      for (unsigned k = 1; k <= 3; k++) {
        unsigned tag = k == 1 ? PL_TAG_PIN : k == 2 ? PL_TAG_LAW : PL_TAG_APP;
        struct sljit_jump* other =
            sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R5, 0, I, tag);
        mov(e, SLJIT_R2, 0, I, k);
        sljit_set_label(other, sljit_emit_label(e->c));
      }
    }
    break;
  case JP_Arity:
  case JP_Name:
  case JP_Body:
  case JP_Unpin: {
    mov(e, SLJIT_R2, 0, I, 0);
    binary(e, SLJIT_LSHR, SLJIT_R5, SLJIT_R0, 0, I, 56);
    struct sljit_jump* other =
        sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R5, 0, I,
                       prim == JP_Unpin ? PL_TAG_PIN : PL_TAG_LAW);
    binary(e, SLJIT_AND, SLJIT_R0, SLJIT_R0, 0, I, PL_ADDR_MASK);
    if (prim == JP_Unpin) {
      /* Proxy targets require an acquire load; let the runtime resolve them. */
      binary(e, SLJIT_AND, SLJIT_R5, M(SLJIT_R0), 0, I, PL_F_PIN_PROXY << 8);
      guard(e, SLJIT_NOT_EQUAL, SLJIT_R5, 0, I, 0);
    }
    mov(e, SLJIT_R2, 0, M(SLJIT_R0),
        prim == JP_Arity  ? 8
        : prim == JP_Name ? 16
        : prim == JP_Body ? 24
                          : 40);
    sljit_set_label(other, sljit_emit_label(e->c));
    break;
  }
  case JP_Hd:
  case JP_Sz:
  case JP_Last:
  case JP_Ix:
  case JP_Ix0:
  case JP_Ix1:
  case JP_Ix2:
  case JP_Ix3:
  case JP_Ix4:
  case JP_Ix5:
  case JP_Ix6:
  case JP_Ix7:
  case JP_Case: {
    if (prim == JP_Ix)
      small_nat(e, SLJIT_R0);
    bool indexed = prim == JP_Ix || prim == JP_Case;
    int row = indexed ? SLJIT_R1 : SLJIT_R0;
    mov(e, SLJIT_R2, 0,
        prim == JP_Hd     ? SLJIT_R0
        : prim == JP_Case ? SLJIT_R4
                          : I,
        0);
    binary(e, SLJIT_LSHR, SLJIT_R5, row, 0, I, 56);
    struct sljit_jump* other =
        sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R5, 0, I, PL_TAG_APP);
    binary(e, SLJIT_AND, SLJIT_R6, row, 0, I, PL_ADDR_MASK);
    if (prim == JP_Hd) {
      mov(e, SLJIT_R2, 0, M(SLJIT_R6), 8);
    } else {
      binary(e, SLJIT_LSHR, SLJIT_R5, M(SLJIT_R6), 0, I, 32);
      binary(e, SLJIT_SUB, SLJIT_R5, SLJIT_R5, 0, I, 2);
      if (prim == JP_Sz) {
        mov(e, SLJIT_R2, 0, SLJIT_R5, 0);
      } else {
        struct sljit_jump* outside = NULL;
        if (prim == JP_Last) {
          binary(e, SLJIT_SUB, SLJIT_R0, SLJIT_R5, 0, I, 1);
        } else {
          if (!indexed)
            mov(e, SLJIT_R0, 0, I, prim - JP_Ix0);
          outside = sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R0, 0,
                                   SLJIT_R5, 0);
        }
        binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 3);
        binary(e, SLJIT_ADD, SLJIT_R6, SLJIT_R6, 0, SLJIT_R0, 0);
        mov(e, SLJIT_R2, 0, M(SLJIT_R6), 16);
        if (outside)
          sljit_set_label(outside, sljit_emit_label(e->c));
      }
    }
    sljit_set_label(other, sljit_emit_label(e->c));
    break;
  }
  case JP_Or:
  case JP_And:
  case JP_If:
  case JP_Ifz: {
    mov(e, SLJIT_R2, 0, prim == JP_Or ? SLJIT_R0 : SLJIT_R1, 0);
    struct sljit_jump* first =
        sljit_emit_cmp(e->c, prim == JP_Ifz ? SLJIT_EQUAL : SLJIT_NOT_EQUAL,
                       SLJIT_R0, 0, I, 0);
    mov(e, SLJIT_R2, 0,
        prim == JP_Or    ? SLJIT_R1
        : prim == JP_And ? I
                         : SLJIT_R4,
        0);
    sljit_set_label(first, sljit_emit_label(e->c));
    break;
  }
  case JP_Nor: {
    mov(e, SLJIT_R2, 0, I, 0);
    struct sljit_jump* nonzero =
        sljit_emit_cmp(e->c, SLJIT_NOT_EQUAL, SLJIT_R0, 0, I, 0);
    whnf(e, SLJIT_R1);
    boolean(e, SLJIT_EQUAL, SLJIT_R1, 0, I, 0);
    sljit_set_label(nonzero, sljit_emit_label(e->c));
    break;
  }
  case JP_Equal:
    guard(e, SLJIT_SIG_LESS, SLJIT_R0, 0, I, 0);
    guard(e, SLJIT_SIG_LESS, SLJIT_R1, 0, I, 0);
    boolean(e, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);
    break;
  case JP_Force:
  case JP_DeepSeq:
    /* Terminal direct nats need no recursive normalization or writeback. */
    guard(e, SLJIT_SIG_LESS, SLJIT_R0, 0, I, 0);
    mov(e, SLJIT_R2, 0, prim == JP_Force ? SLJIT_R0 : SLJIT_R1, 0);
    break;
  case JP_Seq:
  case JP_Seq2:
  case JP_Seq3:
    mov(e, SLJIT_R2, 0, M(SP), -8);
    break;
  default: { /* Case2 .. Case16, with the fallback in the final slot. */
    mov(e, SLJIT_R2, 0, M(SP), -8);
    struct sljit_jump* outside =
        sljit_emit_cmp(e->c, SLJIT_GREATER_EQUAL, SLJIT_R0, 0, I, argc - 2);
    binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 3);
    binary(e, SLJIT_ADD, SLJIT_R0, SP, 0, SLJIT_R0, 0);
    mov(e, SLJIT_R2, 0, M(SLJIT_R0), (1 - (sljit_sw)argc) * 8);
    sljit_set_label(outside, sljit_emit_label(e->c));
    break;
  }
  }
  /* Returning a selected lazy field must evaluate it, unlike MK_THK's
   * existing projection shortcut. Guard before consuming any operands. */
  if (!thunk && !numeric && prim != JP_Nil && prim != JP_Truth)
    whnf(e, SLJIT_R2);
  if (signed63) {
    if (prim <= JP_I63Mod || prim == JP_I63MulDiv)
      int63_encode(e);
    /* Match op_body's op-83 Memo barrier, after every fallback guard. */
    mov(e, SLJIT_R5, 0, M(T), OFF(pl_thread, effect_epoch));
    binary(e, SLJIT_ADD, SLJIT_R5, SLJIT_R5, 0, I, 1);
    mov(e, M(T), OFF(pl_thread, effect_epoch), SLJIT_R5, 0);
  }
  mov(e, TOP, 0, SLJIT_R2, 0);
  e->cached = true;
  binary(e, SLJIT_SUB, SP, SP, 0, I, (sljit_sw)(argc - 1) * 8);
  if (steps)
    binary(e, SLJIT_SUB, FUEL, FUEL, 0, I, steps);
  if (tail) {
    leave(e, PL_JIT_RETURN, true);
    e->cached = false;
  }
}

/* All guards precede construction: a failure resumes OP_BUILD in C, which
 * reserves/collects with the original stack rooted. No exits, calls or fuel
 * points occur after the first store. R2 is the cursor, TOP the heap, R4 the
 * variable base; the ordinary top cache is flushed for this whole region. */
static void emit_batch(emitter* e, pl_code* code, const pl_batch* b) {
#if defined(PL_GC_STRESS) || defined(PL_CACHE_STATS)
  /* Keep forced collections and allocation instrumentation authoritative. */
  (void)code;
  leave(e, b->start, e->cached);
  e->cached = false;
#else
  flush(e);
  e->cached = false;
  need(e, b->depth);
  mov(e, SLJIT_R0, 0, M(T), OFF(pl_thread, vcap));
  binary(e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 3);
  binary(e, SLJIT_ADD, SLJIT_R0, VS, 0, SLJIT_R0, 0);
  binary(e, SLJIT_ADD, SLJIT_R1, SP, 0, I, (sljit_sw)b->extra * 8);
  guard(e, SLJIT_GREATER, SLJIT_R1, 0, SLJIT_R0, 0);
  if (b->vars) {
    (void)sljit_emit_op1(e->c, SLJIT_MOV_U32, SLJIT_R1, 0, M(F),
                         OFF(pl_frame, argc));
    struct sljit_jump* heap =
        sljit_emit_cmp(e->c, SLJIT_EQUAL, SLJIT_R1, 0, I, 0);
    guard(e, SLJIT_LESS, SLJIT_R1, 0, I, b->vars);
    mov(e, SLJIT_R4, 0, M(F), OFF(pl_frame, b));
    binary(e, SLJIT_SHL, SLJIT_R4, SLJIT_R4, 0, I, 3);
    binary(e, SLJIT_ADD, SLJIT_R4, VS, 0, SLJIT_R4, 0);
    struct sljit_jump* loaded = sljit_emit_jump(e->c, SLJIT_JUMP);
    sljit_set_label(heap, sljit_emit_label(e->c));
    mov(e, SLJIT_R4, 0, M(F), OFF(pl_frame, a));
    binary(e, SLJIT_AND, SLJIT_R4, SLJIT_R4, 0, I, (sljit_sw)PL_ADDR_MASK);
    mov(e, SLJIT_R1, 0, M(SLJIT_R4), 0);
    binary(e, SLJIT_LSHR, SLJIT_R1, SLJIT_R1, 0, I, 32);
    guard(e, SLJIT_LESS_EQUAL, SLJIT_R1, 0, I, b->vars);
    binary(e, SLJIT_ADD, SLJIT_R4, SLJIT_R4, 0, I, 8);
    sljit_set_label(loaded, sljit_emit_label(e->c));
  }
  mov(e, TOP, 0, M(T), OFF(pl_thread, heap));
  mov(e, SLJIT_R2, 0, M(TOP), OFF(pl_heap, free));
  mov(e, SLJIT_R1, 0, M(TOP), OFF(pl_heap, limit));
  binary(e, SLJIT_SUB, SLJIT_R1, SLJIT_R1, 0, SLJIT_R2, 0);
  guard(e, SLJIT_LESS, SLJIT_R1, 0, I, (sljit_sw)b->cells * 8);
  for (uint32_t k = 0; k < b->nsteps; k++) {
    const pl_batch_step* s = b->steps + k;
    if (s->op == OP_PUSH_LIT) {
      mov(e, SLJIT_R0, 0, I, (sljit_sw)(uintptr_t)&code->ops[s->arg]);
      mov(e, SLJIT_R0, 0, M(SLJIT_R0), 0);
    } else if (s->op == OP_PUSH_VAR) {
      mov(e, SLJIT_R0, 0, M(SLJIT_R4), (sljit_sw)s->arg * 8);
    } else if (s->op == OP_PUSH_SLOT) {
      mov(e, SLJIT_R0, 0, M(BASE), (sljit_sw)s->arg * 8);
    } else {
      bool app = s->op == OP_MK_APP;
      bool known = !app && (s->flags & PL_BAN_MASK) == PL_BAN_PRIM_KNOWN;
      uint32_t count = s->arg + (app ? 1u : 0u);
      uint32_t cells = app ? PL_APP_CELLS(s->arg)
                           : PL_THKE_CELLS(s->arg + (known ? 1u : 0u));
      pl_cell hdr =
          pl_hdr_make(app ? PL_K_APP : PL_K_THKE, 0, app ? s->aux : 0, cells);
      mov(e, M(SLJIT_R2), 0, I, (sljit_sw)hdr);
      if (!app) {
        mov(e, M(SLJIT_R2), 8, I,
            (sljit_sw)(known ? s->flags : (uint64_t)(pl_bane)s->flags));
        if (known)
          mov(e, M(SLJIT_R2), 16, I, s->aux);
      }
      for (uint32_t j = 0; j < count; j++) {
        mov(e, SLJIT_R0, 0, M(SP), ((sljit_sw)j - count) * 8);
        mov(e, M(SLJIT_R2),
            ((sljit_sw)j + (app     ? 1
                            : known ? 3
                                    : 2)) *
                8,
            SLJIT_R0, 0);
      }
      binary(e, SLJIT_SUB, SP, SP, 0, I, (sljit_sw)count * 8);
      binary(e, SLJIT_AND, SLJIT_R0, SLJIT_R2, 0, I, (sljit_sw)PL_ADDR_MASK);
      binary(e, SLJIT_OR, SLJIT_R0, SLJIT_R0, 0, I,
             (sljit_sw)((uint64_t)(app ? PL_TAG_APP : PL_TAG_DEFER) << 56));
      binary(e, SLJIT_ADD, SLJIT_R2, SLJIT_R2, 0, I, (sljit_sw)cells * 8);
    }
    mov(e, M(SP), 0, SLJIT_R0, 0);
    binary(e, SLJIT_ADD, SP, SP, 0, I, 8);
  }
  mov(e, M(TOP), OFF(pl_heap, free), SLJIT_R2, 0);
#endif
}

static bool compile_code(pl_code* code) {
  size_t n = code->nops;
  struct sljit_label** labels = calloc(n, sizeof(*labels));
  bool* targets = calloc(n, sizeof(*targets));
  bool* cached = calloc(n, sizeof(*cached));
  sljit_uw* entries = calloc(n, sizeof(*entries));
  emitter e = {.c = sljit_create_compiler(NULL),
               .exits = calloc(n * 12, sizeof(exit_patch)),
               .edges = calloc(n * 2, sizeof(edge_patch))};
  bool ok = false;
  if (!labels || !targets || !cached || !entries || !e.c || !e.exits ||
      !e.edges)
    goto done;
  for (size_t pc = 0; pc < n; pc += pl_bytecode_width(code->ops + pc)) {
    const pl_op_t* o = code->ops + pc;
    if (o[0] == OP_JMP || o[0] == OP_CALL || o[0] == OP_TAILBLK)
      targets[o[1]] = true;
    if (o[0] == OP_BR)
      for (size_t k = 0; k < o[1]; k++)
        targets[o[2 + k]] = true;
  }
  (void)sljit_emit_enter(e.c, 0, SLJIT_ARGS3(W, P, P, W), 7, 6, 0);
  mov(&e, SLJIT_R2, 0, VS, 0); /* third argument is the entry address */
  mov(&e, VS, 0, M(T), OFF(pl_thread, vstack));
  mov(&e, SP, 0, M(T), OFF(pl_thread, vsp));
  binary(&e, SLJIT_SHL, SP, SP, 0, I, 3);
  binary(&e, SLJIT_ADD, SP, VS, 0, SP, 0);
  mov(&e, FUEL, 0, M(T), OFF(pl_thread, fuel));
  (void)sljit_emit_op1(e.c, SLJIT_MOV_U32, BASE, 0, M(F),
                       OFF(pl_frame, argbase));
  binary(&e, SLJIT_SHL, BASE, BASE, 0, I, 3);
  binary(&e, SLJIT_ADD, BASE, VS, 0, BASE, 0);
  (void)sljit_emit_ijump(e.c, SLJIT_JUMP, SLJIT_R2, 0);

  for (size_t pc = 0, next_pc = 0; pc < n; pc = next_pc) {
    const pl_batch* b = pl_batch_at(code, (uint32_t)pc);
    next_pc = b ? b->end : pc + pl_bytecode_width(code->ops + pc);
    const pl_op_t* o = code->ops + pc;
    e.pc = (uint32_t)pc;
    if (targets[pc]) {
      flush(&e);
      e.cached = false;
    }
    cached[pc] = e.cached;
    labels[pc] = sljit_emit_label(e.c);
    if (b) {
      emit_batch(&e, code, b);
      continue;
    }
    if (!supported(o)) {
      leave(&e, (uint32_t)pc, e.cached);
      e.cached = false;
      continue;
    }
    const pl_opdesc* d;
    jit_prim prim = primitive(o, &d);
    /* Retain the compact top-cache path for the original numeric opcodes. */
    if (prim != JP_NONE && o[0] != OP_ADD && o[0] != OP_SUB && o[0] != OP_CMP) {
      emit_primitive(&e, o, prim, d);
      continue;
    }
    switch (o[0]) {
    case OP_PUSH_LIT:
      room(&e);
      /* Load through the owning decoded stream; do not invent ownership
       * or relocation rules for literal objects in executable memory. */
      mov(&e, SLJIT_R0, 0, I, (sljit_sw)(uintptr_t)&code->ops[pc + 1]);
      mov(&e, SLJIT_R0, 0, M(SLJIT_R0), 0);
      push(&e);
      break;
    case OP_PUSH_SLOT:
      room(&e);
      binary(&e, SLJIT_ADD, SLJIT_R1, BASE, 0, I, (sljit_sw)o[1] * 8);
      guard(&e, SLJIT_GREATER_EQUAL, SLJIT_R1, 0, SP, 0);
      flush(&e); /* slot can alias a dirty cached top */
      mov(&e, SLJIT_R0, 0, M(SLJIT_R1), 0);
      push(&e);
      break;
    case OP_PUSH_VAR: {
      room(&e);
      (void)sljit_emit_op1(e.c, SLJIT_MOV_U32, SLJIT_R1, 0, M(F),
                           OFF(pl_frame, argc));
      struct sljit_jump* heap =
          sljit_emit_cmp(e.c, SLJIT_EQUAL, SLJIT_R1, 0, I, 0);
      guard(&e, SLJIT_LESS_EQUAL, SLJIT_R1, 0, I, (sljit_sw)o[1]);
      mov(&e, SLJIT_R0, 0, M(F), OFF(pl_frame, b));
      binary(&e, SLJIT_SHL, SLJIT_R0, SLJIT_R0, 0, I, 3);
      binary(&e, SLJIT_ADD, SLJIT_R0, VS, 0, SLJIT_R0, 0);
      flush(&e);
      mov(&e, SLJIT_R0, 0, M(SLJIT_R0), (sljit_sw)o[1] * 8);
      struct sljit_jump* loaded = sljit_emit_jump(e.c, SLJIT_JUMP);
      sljit_set_label(heap, sljit_emit_label(e.c));
      mov(&e, SLJIT_R0, 0, M(F), OFF(pl_frame, a));
      binary(&e, SLJIT_AND, SLJIT_R0, SLJIT_R0, 0, I, (sljit_sw)PL_ADDR_MASK);
      mov(&e, SLJIT_R1, 0, M(SLJIT_R0), 0);
      binary(&e, SLJIT_LSHR, SLJIT_R1, SLJIT_R1, 0, I, 32);
      guard(&e, SLJIT_LESS_EQUAL, SLJIT_R1, 0, I, (sljit_sw)o[1] + 1);
      mov(&e, SLJIT_R0, 0, M(SLJIT_R0), ((sljit_sw)o[1] + 1) * 8);
      sljit_set_label(loaded, sljit_emit_label(e.c));
      push(&e);
      break;
    }
    case OP_ADD:
    case OP_SUB:
    case OP_CMP: {
      need(&e, 2);
      guard(&e, SLJIT_LESS_EQUAL, FUEL, 0, I, 3);
      top(&e);
      mov(&e, SLJIT_R1, 0, M(SP), -16);
      guard(&e, SLJIT_SIG_LESS, SLJIT_R1, 0, I, 0);
      guard(&e, SLJIT_SIG_LESS, TOP, 0, I, 0);
      if (o[0] == OP_ADD) {
        binary(&e, SLJIT_ADD, SLJIT_R2, SLJIT_R1, 0, TOP, 0);
        guard(&e, SLJIT_SIG_LESS, SLJIT_R2, 0, I, 0);
      } else if (o[0] == OP_SUB) {
        binary(&e, SLJIT_SUB | SLJIT_SET_LESS, SLJIT_R2, SLJIT_R1, 0, TOP, 0);
        (void)sljit_emit_select(e.c, SLJIT_LESS, SLJIT_R2, I, 0, SLJIT_R2);
      } else {
        mov(&e, SLJIT_R2, 0, I, 0);
        struct sljit_jump* less =
            sljit_emit_cmp(e.c, SLJIT_LESS, SLJIT_R1, 0, TOP, 0);
        mov(&e, SLJIT_R2, 0, I, 1);
        struct sljit_jump* equal =
            sljit_emit_cmp(e.c, SLJIT_EQUAL, SLJIT_R1, 0, TOP, 0);
        mov(&e, SLJIT_R2, 0, I, 2);
        struct sljit_label* end = sljit_emit_label(e.c);
        sljit_set_label(less, end);
        sljit_set_label(equal, end);
      }
      mov(&e, TOP, 0, SLJIT_R2, 0);
      binary(&e, SLJIT_SUB, SP, SP, 0, I, 8);
      binary(&e, SLJIT_SUB, FUEL, FUEL, 0, I, 3);
      break;
    }
    case OP_FORCE:
    case OP_FORCE_READY:
      need(&e, 1);
      guard(&e, SLJIT_LESS_EQUAL, FUEL, 0, I, 1);
      top(&e);
      binary(&e, SLJIT_LSHR, SLJIT_R0, TOP, 0, I, 56);
      guard(&e, SLJIT_EQUAL, SLJIT_R0, 0, I, (sljit_sw)PL_TAG_DEFER);
      binary(&e, SLJIT_SUB, FUEL, FUEL, 0, I, 1);
      break;
    case OP_BR: {
      need(&e, 1);
      top(&e);
      guard(&e, SLJIT_GREATER_EQUAL, TOP, 0, I, (sljit_sw)o[1]);
      /* Each arm retains its own backward-edge fuel step. */
      for (size_t k = 0; k < o[1]; k++) {
        struct sljit_jump* next =
            sljit_emit_cmp(e.c, SLJIT_NOT_EQUAL, TOP, 0, I, (sljit_sw)k);
        if (o[k + 2] <= pc) {
          guard(&e, SLJIT_LESS_EQUAL, FUEL, 0, I, 1);
          binary(&e, SLJIT_SUB, FUEL, FUEL, 0, I, 1);
        }
        binary(&e, SLJIT_SUB, SP, SP, 0, I, 8);
        edge(&e, (uint32_t)o[k + 2]);
        sljit_set_label(next, sljit_emit_label(e.c));
      }
      /* Unreachable after range guard; keep malformed state in C. */
      leave(&e, (uint32_t)pc, true);
      e.cached = false;
      break;
    }
    case OP_TAILBLK:
      need(&e, (unsigned)o[2]);
      if (o[1] <= pc)
        guard(&e, SLJIT_LESS_EQUAL, FUEL, 0, I, 1);
      flush(&e);
      /* Destination <= source: forward copies have memmove semantics. */
      for (size_t k = 0; k < o[2]; k++) {
        mov(&e, SLJIT_R0, 0, M(SP), ((sljit_sw)k - (sljit_sw)o[2]) * 8);
        mov(&e, M(BASE), (sljit_sw)k * 8, SLJIT_R0, 0);
      }
      binary(&e, SLJIT_ADD, SP, BASE, 0, I, (sljit_sw)o[2] * 8);
      if (o[1] <= pc)
        binary(&e, SLJIT_SUB, FUEL, FUEL, 0, I, 1);
      edge(&e, (uint32_t)o[1]);
      e.cached = false;
      break;
    case OP_JMP:
      if (o[1] <= pc) {
        guard(&e, SLJIT_LESS_EQUAL, FUEL, 0, I, 1);
        binary(&e, SLJIT_SUB, FUEL, FUEL, 0, I, 1);
      }
      flush(&e);
      edge(&e, (uint32_t)o[1]);
      e.cached = false;
      break;
    default: /* ENTRY and NOP */
      break;
    }
  }
  /* Out-of-line guard exits retain the top-cache state at that instruction. */
  for (size_t k = 0; k < e.nexits; k++) {
    sljit_set_label(e.exits[k].jump, sljit_emit_label(e.c));
    leave(&e, e.exits[k].pc, e.exits[k].cached);
  }
  for (size_t k = 0; k < e.nedges; k++)
    sljit_set_label(e.edges[k].jump, labels[e.edges[k].target]);
  /* A VM resume can enter inside a linear region. Restore its expected
   * register cache from the materialized VM stack before joining the body. */
  for (size_t pc = 0; pc < n; pc += pl_bytecode_width(code->ops + pc)) {
    if (code->jit_ops[pc] != OP_JIT)
      continue;
    struct sljit_label* body = labels[pc];
    labels[pc] = sljit_emit_label(e.c);
    if (cached[pc]) {
      struct sljit_jump* present =
          sljit_emit_cmp(e.c, SLJIT_GREATER, SP, 0, BASE, 0);
      leave(&e, (uint32_t)pc, false);
      sljit_set_label(present, sljit_emit_label(e.c));
      mov(&e, TOP, 0, M(SP), -8);
    }
    sljit_set_label(sljit_emit_jump(e.c, SLJIT_JUMP), body);
  }
  if (sljit_get_compiler_error(e.c) != SLJIT_SUCCESS)
    goto done;
  void* memory = sljit_generate_code(e.c, 0, NULL);
  if (memory == NULL)
    goto done;
  for (size_t pc = 0; pc < n; pc += pl_bytecode_width(code->ops + pc))
    if (code->jit_ops[pc] == OP_JIT)
      entries[pc] = sljit_get_label_addr(labels[pc]);
  code->jit->memory = memory;
  code->jit->entries = entries;
  entries = NULL;
  jit_fn fn;
  static_assert(sizeof(fn) == sizeof(memory), "function pointer size");
  memcpy(&fn, &memory, sizeof(fn));
  atomic_fetch_add(&code_bytes, sljit_get_generated_code_size(e.c));
  atomic_store_explicit(&code->jit->fn, fn, memory_order_release);
  ok = true;
done:
  if (e.c)
    sljit_free_compiler(e.c);
  free(labels);
  free(targets);
  free(cached);
  free(entries);
  free(e.exits);
  free(e.edges);
  return ok;
}

uint32_t pl_jit_run(pl_thread* t, pl_frame* fr, uint32_t pc) {
  struct pl_jit* j = fr->code->jit;
  jit_fn fn = atomic_load_explicit(&j->fn, memory_order_acquire);
  if (fn == NULL &&
      !atomic_load_explicit(&j->attempted, memory_order_acquire)) {
    if (atomic_fetch_add_explicit(&j->hits, 1, memory_order_relaxed) + 1 >=
            threshold &&
        pthread_mutex_trylock(&compile_mu) == 0) {
      if (!atomic_load_explicit(&j->attempted, memory_order_relaxed)) {
        uint64_t start = now_ns();
        bool ok = compile_code(fr->code);
        atomic_fetch_add(&compile_ns, now_ns() - start);
        atomic_fetch_add(ok ? &compiled : &failed, 1);
        atomic_store_explicit(&j->attempted, true, memory_order_release);
      }
      (void)pthread_mutex_unlock(&compile_mu);
      fn = atomic_load_explicit(&j->fn, memory_order_acquire);
    }
  }
  if (fn == NULL)
    return pc;
  if (statistics)
    atomic_fetch_add_explicit(&native_entries, 1, memory_order_relaxed);
  return (uint32_t)fn(t, fr, j->entries[pc]);
}

void pl_jit_free(pl_code* c) {
  if (c->jit != NULL) {
    if (c->jit->memory != NULL)
      sljit_free_code(c->jit->memory, NULL);
    free(c->jit->entries);
    free(c->jit);
    free(c->jit_ops);
    c->jit = NULL;
    c->jit_ops = NULL;
  }
}

bool pl_jit_compiled(const pl_code* c) {
  return c->jit != NULL &&
         atomic_load_explicit(&c->jit->fn, memory_order_acquire) != NULL;
}
#endif
