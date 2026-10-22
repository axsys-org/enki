#include "jit.h"

#ifdef PL_SLJIT
#include <sljitLir.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Experimental, non-allocating native regions. There are no native calls,
 * GC safepoints, or surviving native continuations. Every exit spills the
 * operand stack and fuel, before executing the pending bytecode once in C.
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
  enabled = v != NULL && strcmp(v, "1") == 0;
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

/* Called only on a successfully validated stream. */
static size_t width(const pl_op_t* o) {
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

static bool supported(const pl_op_t* o) {
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
  /* Instrument only runs of at least three supported instructions. Native
   * compilation can also traverse shorter runs reached by internal edges. */
  pl_op_t* overlay = malloc(c->nops * sizeof(*overlay));
  struct pl_jit* j = calloc(1, sizeof(*j));
  if (overlay == NULL || j == NULL) {
    free(overlay);
    free(j);
    return;
  }
  memcpy(overlay, c->ops, c->nops * sizeof(*overlay));
  size_t count = 0, start = 0;
  bool any = false;
  for (size_t pc = 0; pc <= c->nops;) {
    if (pc < c->nops && supported(c->ops + pc)) {
      if (count++ == 0)
        start = pc;
    } else {
      if (count >= 3) {
        any = true;
        for (size_t k = start; k < pc; k += width(c->ops + k))
          overlay[k] = OP_JIT;
      }
      count = 0;
    }
    if (pc == c->nops)
      break;
    pc += width(c->ops + pc);
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
 * R0-R2 and R4 are scratch. No heap pointers survive return to the VM.
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

static bool compile_code(pl_code* code) {
  size_t n = code->nops;
  struct sljit_label** labels = calloc(n, sizeof(*labels));
  bool* targets = calloc(n, sizeof(*targets));
  bool* cached = calloc(n, sizeof(*cached));
  sljit_uw* entries = calloc(n, sizeof(*entries));
  emitter e = {.c = sljit_create_compiler(NULL),
               .exits = calloc(n * 8, sizeof(exit_patch)),
               .edges = calloc(n * 2, sizeof(edge_patch))};
  bool ok = false;
  if (!labels || !targets || !cached || !entries || !e.c || !e.exits ||
      !e.edges)
    goto done;
  for (size_t pc = 0; pc < n; pc += width(code->ops + pc)) {
    const pl_op_t* o = code->ops + pc;
    if (o[0] == OP_JMP || o[0] == OP_CALL || o[0] == OP_TAILBLK)
      targets[o[1]] = true;
    if (o[0] == OP_BR)
      for (size_t k = 0; k < o[1]; k++)
        targets[o[2 + k]] = true;
  }
  (void)sljit_emit_enter(e.c, 0, SLJIT_ARGS3(W, P, P, W), 5, 6, 0);
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

  for (size_t pc = 0; pc < n; pc += width(code->ops + pc)) {
    const pl_op_t* o = code->ops + pc;
    e.pc = (uint32_t)pc;
    if (targets[pc]) {
      flush(&e);
      e.cached = false;
    }
    cached[pc] = e.cached;
    labels[pc] = sljit_emit_label(e.c);
    if (!supported(o)) {
      leave(&e, (uint32_t)pc, e.cached);
      e.cached = false;
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
  for (size_t pc = 0; pc < n; pc += width(code->ops + pc)) {
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
  for (size_t pc = 0; pc < n; pc += width(code->ops + pc))
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
