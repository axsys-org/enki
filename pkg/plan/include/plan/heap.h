#ifndef PL_HEAP_H
#define PL_HEAP_H

/*
 * Semispace Cheney heap with safepoint + reserve discipline.
 *
 * Core invariants (violations abort in debug builds):
 *   I1  the collector runs only inside pl_gc_reserve, never elsewhere
 *   I2  pl_bump never collects; it asserts previously reserved headroom
 *   I3  measure, then build: reserve before each no-collect window
 *   I4  no pl_val in a C local survives a reserve — re-fetch from a root
 *   I6  the collector scans exactly the registered root sources
 *   I9  within one no-collect window, bare pointers are stable
 */

#include <setjmp.h>
#include <stddef.h>
#include <assert.h>

#include "plan/value.h"
#include "plan/bytecode.h"

typedef struct pl_store pl_store;
typedef struct pl_heap pl_heap;
typedef struct pl_thread pl_thread;

/* ── Root sources ──────────────────────────────────────────────────────── */

typedef void (*pl_root_visit)(pl_val* slot, void* gc_ctx);
typedef void (*pl_root_source)(pl_root_visit visit, void* gc_ctx,
                               void* src_ctx);

void pl_gc_add_root_source(pl_heap* h, pl_root_source src, void* src_ctx);
void pl_gc_del_root_source(pl_heap* h, pl_root_source src, void* src_ctx);

/* ── Machine frames (eval) — pl_val fields are roots ───────────────────── */

typedef enum {
  PL_F_UPDATE = 1, /* a: thunk/blackhole to update with the result      */
  PL_F_APPLY,      /* b: pending (lazy) argument                        */
  PL_F_SEQ,        /* b: value to evaluate after discarding the result  */
  PL_F_KAL,        /* a: env; resume body-expr decomposition            */
  PL_F_KAPP,       /* a: env, b: (0 f x) expr; force subexprs, then     */
                   /* interpret them (the reference unapp forces)       */
  PL_F_OPENT,      /* op entry: forcing the op's single argument        */
  PL_F_OPARG,      /* primop strict-arg driver                          */
  PL_F_OPDEEP,     /* primop deep (nf) phase over arg 0                 */
  PL_F_NF,         /* normalize the incoming value                      */
  PL_F_NFOBJ,      /* a: object being normalized, k: field index        */
  PL_F_EXEC,       /* a: env, ip: pointer  */
  PL_F_UPD,        /* a: newstyle thunk update */
} pl_frame_kind;

/** TODO make union */
typedef struct pl_frame {
  uint8_t kind;
  uint32_t k;     /* field index / mask cursor / ip */
  uint32_t op;    /* op descriptor index (F_OPARG/F_OPDEEP) */
  uint64_t opset; /* op set number (F_OPENT) */
  pl_val a;       /* root */
  pl_val b;       /* root */
  size_t argbase; /* offset into vstack (never a pointer) */
  uint32_t argc;
  pl_code* code;
} pl_frame;

/* ── Thread ────────────────────────────────────────────────────────────── */

struct pl_thread {
  pl_heap* heap;

  pl_val* vstack; /* operand/value stack — root source */
  size_t vsp, vcap;

  pl_frame* fstack; /* machine frames — root source */
  size_t fsp, fcap;

  pl_val exn;          /* pending PLAN_EXN value — root slot */
  const char* exn_msg; /* non-NULL: runtime error, not catchable by Try */
  jmp_buf* handler;

  /* The reference vMode: op 82 (rplan I/O) is callable only in RPLAN
   * mode (REPL / snapshot execution), never while assembling modules. */
  bool rplan_f;

  /* When non-NULL, ReadFile resolves its argument relative to this root and
   * refuses paths whose canonical target escapes it. */
  const char* rplan_file_root_c;
};

pl_heap* pl_heap_new(size_t cells, pl_store* store);
void pl_heap_free(pl_heap* h);
pl_store* pl_heap_store(pl_heap* h);

pl_thread* pl_thread_new(pl_heap* h);
void pl_thread_free(pl_thread* t);

/* ── The three allocation entry points ─────────────────────────────────── */

/* The ONLY collecting path; postcondition: headroom >= cells. */
void pl_gc_reserve(pl_thread* t, size_t cells);

/* Bump allocation; never collects; hard-asserts headroom (I2). */
pl_cell* pl_bump(pl_thread* t, size_t cells);

/* Remaining headroom in cells (for tests/diagnostics). */
size_t pl_gc_headroom(pl_thread* t);
/* Cells of live data after the last collection (diagnostics). */
size_t pl_gc_live_cells(pl_heap* h);
void pl_gc_collect_now(pl_thread* t); /* for tests */

/* ── No-collect windows (debug accounting) ─────────────────────────────── */

#ifndef NDEBUG
void pl_gc_forbid(pl_heap* h);
void pl_gc_allow(pl_heap* h);
#define PL_GC_FORBID(t) pl_gc_forbid((t)->heap)
#define PL_GC_ALLOW(t)  pl_gc_allow((t)->heap)
#else
#define PL_GC_FORBID(t) ((void)0)
#define PL_GC_ALLOW(t)  ((void)0)
#endif

/* ── Value/frame stack helpers (plain malloc arrays, growable) ─────────── */

void pl_vstack_grow(pl_thread* t);
void pl_fstack_grow(pl_thread* t);

static inline void pl_vpush(pl_thread* t, pl_val v) {
  if (t->vsp == t->vcap)
    pl_vstack_grow(t);
  t->vstack[t->vsp++] = v;
}

static inline pl_val pl_vpop(pl_thread* t) {
  return t->vstack[--t->vsp];
}

static inline pl_val pl_vreplace(pl_thread* t, uint32_t n, pl_val r) {
  assert(n >= 1 && t->vsp >= n);
  t->vsp -= n - 1;
  t->vstack[t->vsp - 1] = r;
  return r;
}

/* read, n down from TOS */
static inline pl_val* pl_vpeek(pl_thread *t, uint32_t n) {
    assert(n < t->vsp);
    return &t->vstack[t->vsp - n];
}

static inline pl_frame* pl_fpush(pl_thread* t) {
  if (t->fsp == t->fcap)
    pl_fstack_grow(t);
  pl_frame* f = &t->fstack[t->fsp++];
  f->a = 0;
  f->b = 0;
  return f;
}

#endif
