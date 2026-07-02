#include "plan/eval.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "axsys/assume.h"
#include "axsys/allocator.h"
#include "axsys/perf.h"
#include "internal.h"
#include "plan/build.h"
#include "plan/canon.h"
#include "plan/nat.h"
#include "plan/store.h"

/* pl_run dispatches with computed gotos (labels-as-values), a GNU
 * extension.  Clang suppresses the diagnostic with the targeted
 * -Wno-gnu-label-as-value (Makefile); gcc has no specific flag — the
 * pedwarn only lives in the -Wpedantic bucket, so silence that for
 * this translation unit. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/* ── Errors ────────────────────────────────────────────────────────────── */

static _Thread_local char pl_msgbuf[256];

[[noreturn]] void pl_raise(pl_thread* t, pl_val v) {
  t->exn = v;
  t->exn_msg = NULL;
  if (t->handler == NULL)
    ax_abort("uncaught PLAN_EXN");
  longjmp(*t->handler, 1);
}

[[noreturn]] void pl_raise_msg(pl_thread* t, const char* msg) {
  t->exn = 0;
  t->exn_msg = msg;
  if (t->handler == NULL)
    ax_abort("uncaught PLAN error: %s", msg);
  longjmp(*t->handler, 1);
}

[[noreturn]] void pl_raise_msgf(pl_thread* t, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(pl_msgbuf, sizeof(pl_msgbuf), fmt, ap);
  va_end(ap);
  pl_raise_msg(t, pl_msgbuf);
}

void pl_catch_init(pl_thread* t, pl_catch* c) {
  c->prev = t->handler;
  c->vsp = t->vsp;
  c->fsp = t->fsp;
  c->centry = t->centry_depth;
  t->handler = &c->jb;
}

void pl_catch_pop(pl_thread* t, pl_catch* c) {
  t->handler = c->prev;
}

void pl_catch_unwind(pl_thread* t, pl_catch* c) {
  t->handler = c->prev;
  t->vsp = c->vsp;
  t->fsp = c->fsp;
  t->centry_depth = c->centry; /* longjmp skipped the region epilogues */
}

/* ── Enter hook seam ───────────────────────────────────────────────────── */

static pl_enter_hook pl_hook = NULL;

void pl_set_enter_hook(pl_enter_hook hook) {
  pl_hook = hook;
}

/* ── Direct-effect interception seam ───────────────────────────────────── */

static pl_io_hook pl_io = NULL;

void pl_set_io_hook(pl_io_hook hook) {
  pl_io = hook;
}

pl_val pl_io_run(pl_thread* t, uint32_t op, size_t argbase) {
  return pl_ops[op].body(t, argbase);
}

const char* pl_io_name(uint32_t op) {
  return pl_ops[op].name_c;
}

uint32_t pl_io_argc(uint32_t op) {
  return pl_ops[op].argc;
}

/* ── KAL: non-forcing law-body operand interpretation ──────────────────── */

/*
 * kal n e expr (the reference):
 *   [N b] | b <= n  -> env slot b            (not forced; may be a thunk)
 *   [N 0, f, x]     -> deferred application  (a fresh K_THUNK)
 *   [N 0, x]        -> x                     (literal escape)
 *   otherwise       -> expr                  (literal)
 *
 * Bump-only: the caller reserves PL_THUNK_CELLS per possible call.
 * expr must be WHNF.
 */
static pl_val pl_kal1(pl_thread* t, pl_val env, pl_val expr) {
  uint32_t n = pl_env_n(pl_ptr(env)) - 1;
  if (pl_is_nat63(expr)) {
    if (expr <= n)
      return pl_env_slots(pl_ptr(env))[expr];
    return expr;
  }
  pl_cell* p = pl_as(PL_TAG_APP, expr);
  if (p != NULL && pl_app_head(p) == 0) {
    uint32_t na = pl_app_n(p);
    if (na == 1)
      return pl_app_args(p)[0];
    if (na == 2)
      return pl_mk_thunk(t, env, expr);
  }
  return expr;
}

/* Law object behind a head value that is a LAW or a pinned LAW. */
static pl_cell* pl_lawp(pl_val head) {
  if (pl_tag(head) == PL_TAG_LAW)
    return pl_ptr(head);
  return pl_ptr(pl_pin_body(pl_ptr(head)));
}

/* Compiled bytecode for a law head, cached on its pin (NULL for an
 * unpinned law or an uncompiled pin). */
static pl_code* pl_law_code(pl_val law) {
  pl_cell* p = pl_as(PL_TAG_PIN, law);
  return p != NULL ? (pl_code*)pl_pin_code(p) : NULL;
}

/* ── Tracy law attribution ─────────────────────────────────────────────── */

#ifdef TRACY_ENABLE
static size_t pl_profile_append(char* buf, size_t pos, size_t cap,
                                const char* s) {
  if (pos >= cap)
    return pos;
  size_t n = strlen(s);
  size_t avail = cap - pos - 1;
  if (n > avail)
    n = avail;
  memcpy(buf + pos, s, n);
  pos += n;
  buf[pos] = '\0';
  return pos;
}

static size_t pl_profile_law_name(pl_cell* lp, char* buf, size_t cap) {
  size_t pos = pl_profile_append(buf, 0, cap, "law:");
  pl_val name = pl_law_name(lp);
  bool printable = pl_is_nat(name);
  size_t n = printable ? pl_nat_byte_len(name) : 0;
  if (n == 0)
    printable = false;
  for (size_t i = 0; printable && i < n; i++) {
    uint8_t b = pl_nat_byte_at(name, i);
    if (b < 0x20 || b > 0x7e)
      printable = false;
  }

  if (printable) {
    size_t room = cap > pos + 24 ? cap - pos - 24 : 0;
    for (size_t i = 0; i < n && i < room; i++)
      buf[pos++] = (char)pl_nat_byte_at(name, i);
    buf[pos] = '\0';
    if (n > room)
      pos = pl_profile_append(buf, pos, cap, "...");
  } else {
    pos = pl_profile_append(buf, pos, cap, "<anon>");
  }

  int wrote = snprintf(buf + pos, cap - pos, "/%" PRIu64, pl_law_arity(lp));
  if (wrote > 0)
    pos += (size_t)wrote < cap - pos ? (size_t)wrote : cap - pos - 1;
  return pos;
}

static void pl_profile_frame_begin(pl_frame* fr) {
  char name[160];
  pl_cell* lp = pl_lawp(fr->a);
  size_t name_s = pl_profile_law_name(lp, name, sizeof(name));
  AX_PROFILE_ZONE_BEGIN_ALLOC_NAME(fr->profile_ctx, name, name_s);
  fr->profile_live = true;
}

static void pl_profile_frame_end(pl_frame* fr) {
  if (fr->kind == PL_F_PROF && fr->profile_live) {
    AX_PROFILE_ZONE_END(fr->profile_ctx);
    fr->profile_live = false;
  }
}

static void pl_profile_law_push(pl_thread* t, pl_val head) {
  pl_frame* fr = pl_fpush(t);
  fr->kind = PL_F_PROF;
  fr->a = head;
  pl_profile_frame_begin(fr);
}

static void pl_profile_close_above(pl_thread* t, size_t base) {
  for (size_t i = t->fsp; i > base; i--)
    pl_profile_frame_end(&t->fstack[i - 1]);
}

static void pl_profile_reopen_above(pl_thread* t, size_t base) {
  for (size_t i = base; i < t->fsp; i++) {
    pl_frame* fr = &t->fstack[i];
    if (fr->kind == PL_F_PROF && !fr->profile_live)
      pl_profile_frame_begin(fr);
  }
}

#else
static void pl_profile_frame_end(pl_frame* fr) {
  (void)fr;
}

static void pl_profile_law_push(pl_thread* t, pl_val head) {
  (void)t;
  (void)head;
}

static void pl_profile_close_above(pl_thread* t, size_t base) {
  (void)t;
  (void)base;
}

static void pl_profile_reopen_above(pl_thread* t, size_t base) {
  (void)t;
  (void)base;
}
#endif

/* ── Suspension slow path ──────────────────────────────────────────────── */

/*
 * Called when the per-step fuel decrement hits zero.  Returns true when
 * the machine should capture a resume point and yield: only under
 * pl_thread_run, and only with no native frames between the trampoline
 * and the current step — returning would abandon live C state.
 * Otherwise the request is deferred: under an executor, fuel is pinned
 * to 1 so every subsequent step funnels back here until depth 0 (the
 * grace path); outside an executor fuel is inert and simply rearmed.
 */
static bool pl_yield_now(pl_thread* t) {
  if (t->suspendable && t->centry_depth == 0) {
    t->pending_yield = false;
    return true;
  }
  if (t->suspendable) {
    t->pending_yield = true;
    t->fuel = 1;
  } else {
    t->fuel = UINT64_MAX;
  }
  return false;
}

/* ── The machine ───────────────────────────────────────────────────────── */

static pl_run_status pl_run_caught(pl_thread* t, pl_val v0, size_t base,
                                   bool at_return0);

static pl_run_status pl_run(pl_thread* t, pl_val v, size_t base,
                            bool at_return) {
  pl_val env, expr;
  pl_frame* fr;
  size_t hbase = 0;
  uint32_t argc = 0;
  /* JUDGE scan coordinates; restored from the F_JUDGE frame on resume
   * (offsets only — the chain itself lives on the value stack) */
  size_t jbase = 0;
  uint32_t jargc = 0;

  /*
   * Computed-goto dispatch tables (labels-as-values; the Makefile
   * carries -Wno-gnu-label-as-value for exactly this).  Frames keep
   * their kind byte — the host pushes frames from outside this
   * function, the TRY scan and the profiler read kinds, and the GC
   * traces a/b regardless — so dispatch is one indexed load here
   * rather than a label stored in the frame.
   */
  static void* const defer_tbl[PL_K_THKE + 1] = {
      [PL_K_THUNK] = &&defer_thunk,
      [PL_K_IND] = &&defer_ind,
      [PL_K_BH] = &&defer_bh,
      [PL_K_THKE] = &&defer_thke,
  };
  /* OP_EVAL/OP_CALL are unimplemented: abort, in all builds.
   * OP_TAILCALL is never emitted by the compiler — the ingest walker
   * fuses MK_THK+RET into it. */
  static void* const op_tbl[PL_OP_COUNT] = {
      [OP_PUSH_VAR] = &&x_push_var, [OP_PUSH_LIT] = &&x_push_lit,
      [OP_MK_THK] = &&x_mk_thk,     [OP_MK_APP] = &&x_mk_app,
      [OP_INTERP] = &&x_interp,     [OP_RET] = &&x_ret,
      [OP_EVAL] = &&x_bad,          [OP_CALL] = &&x_bad,
      [OP_TAILCALL] = &&x_tail,
  };
  static void* const ret_tbl[PL_F_KIND_COUNT] = {
      [PL_F_UPDATE] = &&ret_update, [PL_F_APPLY] = &&ret_apply,
      [PL_F_SEQ] = &&ret_seq,       [PL_F_KAL] = &&ret_kal,
      [PL_F_KAPP] = &&ret_kapp,     [PL_F_OPENT] = &&ret_opent,
      [PL_F_OPARG] = &&ret_oparg,   [PL_F_OPDEEP] = &&ret_opdeep,
      [PL_F_NF] = &&ret_nf,         [PL_F_NFOBJ] = &&ret_nfobj,
      [PL_F_EXEC] = &&ret_exec,     [PL_F_UPD] = &&ret_upd,
      [PL_F_TRY] = &&ret_try,       [PL_F_JUDGE] = &&ret_judge,
      [PL_F_NIL] = &&ret_nil,       [PL_F_PROF] = &&ret_prof,
      [PL_F_APPLYN] = &&ret_applyn,
  };

  if (at_return)
    goto ret;

eval:
  /*
   * The per-step safepoint: one decrement and one branch.  Fuel is the
   * only yield trigger.  At this position the complete machine
   * state is (v, value stack, frame stack) — nothing lives in C locals —
   * so suspension is a two-field capture and a normal return.
   */
  if (ax_unlikely(--t->fuel == 0) && pl_yield_now(t)) {
    t->resume_kind = PL_RES_EVAL;
    t->resume_val = v;
    pl_profile_close_above(t, base);
    return PL_RUN_YIELDED;
  }
  if (pl_is_whnf(v))
    goto ret;
  {
    pl_cell* p = pl_ptr(v);
    /* the kind is a raw 8-bit header field: bound it before indexing */
    pl_kind k = pl_hdr_kind(p[0]);
    if (ax_unlikely(k > PL_K_THKE || defer_tbl[k] == NULL))
      ax_abort("EVAL: bad defer kind");
    goto* defer_tbl[k];

  defer_ind:
    v = pl_ind_target(p);
    goto eval;

  defer_bh:
    pl_raise_msg(t, "<<loop>>");

  defer_thunk:
    env = pl_thunk_env(p);
    expr = pl_thunk_expr(p);
    /* blackhole; the F_UPDATE frame writes the result back */
    p[0] = pl_hdr_make(PL_K_BH, 0, 0, pl_hdr_cells(p[0]));
    p[1] = 0;
    p[2] = 0;
    fr = pl_fpush(t);
    fr->kind = PL_F_UPDATE;
    fr->a = v;
    goto eval_expr;

  defer_thke:
    if ((pl_hdr_flags(p[0]) & PL_F_HOLE) != 0)
      pl_raise_msg(t, "<<loop>>");
    p[0] = pl_hdr_set_flag(p[0], PL_F_HOLE);
    fr = pl_fpush(t);
    fr->kind = PL_F_UPD;
    fr->a = v;
    goto eval_thke;
  }
eval_thke: {
  fr = &t->fstack[t->fsp - 1];
  v = fr->a;
  pl_val* args;
  pl_bane ban = pl_thke_bane(pl_ptr(v));
  argc = pl_thke_n(pl_ptr(v));
  args = pl_thke_args(pl_ptr(v));
  if (argc == 0)
    pl_raise_msg(t, "bad empty bytecode thunk");
  if (ban == PL_BAN_FAST) {
    /*
     * The bane is a hint from installed (untrusted) PLAN code: verify
     * that the head really is a law (or pinned law) applied at exact
     * arity before skipping the generic apply path.  Anything else —
     * including a non-WHNF head — takes the slow path, which is
     * semantically identical.
     */
    pl_val head = args[0];
    pl_cell* lp = NULL;
    if (pl_tag(head) == PL_TAG_LAW)
      lp = pl_ptr(head);
    else if (pl_tag(head) == PL_TAG_PIN &&
             pl_tag(pl_pin_body(pl_ptr(head))) == PL_TAG_LAW)
      lp = pl_ptr(pl_pin_body(pl_ptr(head)));
    if (lp == NULL || pl_law_arity(lp) != argc - 1)
      goto thke_slow;
    hbase = t->vsp;
    for (uint32_t i = 0; i < argc; i++)
      pl_vpush(t, args[i]);
    argc--;
    goto judge;
  }
  if (ban == PL_BAN_PRIM_KNOWN) {
    /* [opidx, e1..en]: ingest already resolved the op — no row, no
     * F_OPENT, no lookup; enter the strict-arg driver directly */
    uint32_t idx = (uint32_t)args[0];
    assert(idx < pl_nops);
    if (pl_ops[idx].opset == 82 && !t->rplan_f)
      pl_raise_msg(t, "Not in RPLAN Mode"); /* the F_OPENT gate */
    size_t listbase = t->vsp;
    for (uint32_t i = 0; i < argc; i++)
      pl_vpush(t, args[i]); /* idx fills the name slot: op_body's
                               vsp = argbase - 1 drops it as usual */
    fr = pl_fpush(t);
    fr->kind = PL_F_OPARG;
    fr->op = idx;
    fr->argbase = (uint32_t)(listbase + 1);
    fr->argc = argc - 1;
    fr->k = 0;
    goto oparg_next;
  }
  if (ban == PL_BAN_PRIM) {
    /* [oppin, arg]: dispatch straight into the primop entry, exactly
     * as fast_apply's pinned-nat case would after re-unwinding */
    pl_cell* pp;
    if (argc != 2 || (pp = pl_as(PL_TAG_PIN, args[0])) == NULL ||
        !pl_is_nat(pl_pin_body(pp)))
      goto thke_slow;
    fr = pl_fpush(t);
    fr->kind = PL_F_OPENT;
    fr->opset = pl_nat_u64_clamp(pl_pin_body(pp));
    v = args[1];
    goto eval;
  }
  if (ax_unlikely(ban != PL_BAN_SLOW))
    ax_abort("EVAL: bad bane");
thke_slow:
  /* [f, p1..pm]: park the pending args on the vstack under a single
   * F_APPLYN frame; once the head is WHNF its arity decides the whole
   * application in one step (ret_applyn) — no per-arg frames, no
   * intermediate partial apps */
  if (argc == 1) { /* bare head: just force it */
    v = args[0];
    goto eval;
  }
  {
    size_t appbase = t->vsp;
    pl_vpush(t, 0); /* head slot, filled at ret_applyn */
    for (uint32_t i = 1; i < argc; i++)
      pl_vpush(t, args[i]);
    fr = pl_fpush(t);
    fr->kind = PL_F_APPLYN;
    fr->argbase = (uint32_t)(appbase + 1);
    fr->argc = argc - 1;
    v = args[0];
    goto eval;
  }
}

exec: {
  /*
   * Threaded bytecode dispatch: each handler ends in its own indirect
   * branch through op_tbl.  The installed compiler is trusted, so the
   * fetch and opcode bounds checks are debug-only; unimplemented (but
   * in-range) opcodes land on x_bad through the table at no cost.
   */
#define NEXT() (assert(fr->k < fr->code->nops), fr->code->ops[fr->k++])
#define DISPATCH()                                                             \
  do {                                                                         \
    pl_op_t op_ = NEXT();                                                      \
    assert(op_ < PL_OP_COUNT);                                                 \
    goto* op_tbl[op_];                                                         \
  } while (0)
  fr = &t->fstack[t->fsp - 1];
  DISPATCH();

x_push_var: {
  pl_op_t slot = NEXT();
  if (slot >= pl_env_n(pl_ptr(fr->a)))
    pl_raise_msg(t, "exec: variable out of range");
  pl_vpush(t, pl_env_slots(pl_ptr(fr->a))[slot]);
  DISPATCH();
}

x_push_lit:
  pl_vpush(t, NEXT());
  DISPATCH();

x_mk_thk: {
  argc = (uint32_t)NEXT();
  pl_bane bane = (pl_bane)NEXT();
  if (argc == 0 || argc > t->vsp - fr->argbase)
    pl_raise_msg(t, "bytecode stack underflow");
  if (bane == PL_BAN_PRIM_KNOWN) {
    /* two extra operands: the op index (resolved at ingest from the
     * emitted opset pin + name) and a dead slot */
    uint32_t idx = (uint32_t)NEXT();
    (void)NEXT();
    pl_gc_reserve(t, PL_THKE_CELLS(argc + 1));
    PL_GC_FORBID(t);
    pl_val thke = pl_mk_thke_known(t, fr->a, idx, argc, pl_vpeek(t, argc));
    pl_vreplace(t, argc, thke);
    PL_GC_ALLOW(t);
    DISPATCH();
  }
  if (bane != PL_BAN_FAST && bane != PL_BAN_SLOW && bane != PL_BAN_PRIM)
    pl_raise_msg(t, "exec: bad bane");
  pl_gc_reserve(t, PL_THKE_CELLS(argc));
  PL_GC_FORBID(t);
  pl_val thke = pl_mk_thke(t, fr->a, bane, argc, pl_vpeek(t, argc));
  pl_vreplace(t, argc, thke);
  PL_GC_ALLOW(t);
  DISPATCH();
}

x_mk_app: {
  argc = (uint32_t)NEXT();
  if (argc == 0 || argc + 1 > t->vsp - fr->argbase)
    pl_raise_msg(t, "bytecode stack underflow");
  pl_gc_reserve(t, PL_APP_CELLS(argc));
  PL_GC_FORBID(t);
  size_t appbase = t->vsp - argc - 1;
  pl_val app =
      pl_mk_app_from(t, t->vstack[appbase], argc, &t->vstack[appbase + 1]);
  t->vsp = appbase;
  pl_vpush(t, app);
  PL_GC_ALLOW(t);
  DISPATCH();
}

x_interp:
  env = fr->a;
  expr = NEXT();
  goto eval_expr;

x_ret:
  if (t->vsp == fr->argbase)
    pl_raise_msg(t, "bytecode stack underflow");
  v = pl_vpop(t);
  t->vsp = fr->argbase;
  t->fsp--;
  goto eval;

x_tail: {
  /*
   * MK_THK+RET fused at ingest: the thunk would be forced immediately
   * and has no other consumer, so enter the application directly at
   * the caller's frame slot — no thunk, no F_UPD, and tail recursion
   * runs in constant frame depth.  The group of n values relocates to
   * tbase, exactly where x_ret's vsp reset would have left the stack.
   */
  argc = (uint32_t)NEXT();
  pl_bane bane = (pl_bane)NEXT();
  uint32_t idx = 0;
  if (bane == PL_BAN_PRIM_KNOWN) {
    idx = (uint32_t)NEXT();
    (void)NEXT();
  }
  if (argc == 0 || argc > t->vsp - fr->argbase)
    pl_raise_msg(t, "bytecode stack underflow");
  if (bane != PL_BAN_FAST && bane != PL_BAN_SLOW && bane != PL_BAN_PRIM &&
      bane != PL_BAN_PRIM_KNOWN)
    pl_raise_msg(t, "exec: bad bane");
  size_t tbase = fr->argbase;
  size_t g = t->vsp - argc;
  /*
   * The tail entry skips the eval: safepoint the thunk force would
   * have hit — take the fuel step here, and on exhaustion fall back
   * to the thunk so the canonical safepoint captures the yield with a
   * complete continuation (rearm to 1 so the wraparound is impossible).
   */
  if (ax_unlikely(--t->fuel == 0)) {
    t->fuel = 1;
    goto tail_fallback;
  }
  if (bane == PL_BAN_FAST) {
    pl_val head = t->vstack[g];
    pl_cell* lp = NULL;
    if (pl_tag(head) == PL_TAG_LAW)
      lp = pl_ptr(head);
    else if (pl_tag(head) == PL_TAG_PIN &&
             pl_tag(pl_pin_body(pl_ptr(head))) == PL_TAG_LAW)
      lp = pl_ptr(pl_pin_body(pl_ptr(head)));
    if (lp == NULL || pl_law_arity(lp) != argc - 1)
      goto tail_fallback; /* mis-hinted: the generic path via the thunk */
    memmove(&t->vstack[tbase], &t->vstack[g], (size_t)argc * sizeof(pl_val));
    t->vsp = tbase + argc;
    t->fsp--;
    hbase = tbase;
    argc--;
    goto judge;
  }
  if (bane == PL_BAN_PRIM_KNOWN) {
    assert(idx < pl_nops);
    if (pl_ops[idx].opset == 82 && !t->rplan_f)
      pl_raise_msg(t, "Not in RPLAN Mode");
    pl_vpush(t, 0); /* room for the name slot when g == tbase */
    memmove(&t->vstack[tbase + 1], &t->vstack[g],
            (size_t)argc * sizeof(pl_val));
    t->vstack[tbase] = idx; /* after the move: aliased when g == tbase */
    t->vsp = tbase + 1 + argc;
    t->fsp--;
    fr = pl_fpush(t);
    fr->kind = PL_F_OPARG;
    fr->op = idx;
    fr->argbase = (uint32_t)(tbase + 1);
    fr->argc = argc;
    fr->k = 0;
    goto oparg_next;
  }
  if (bane == PL_BAN_PRIM) {
    pl_cell* pp;
    if (argc != 2 || (pp = pl_as(PL_TAG_PIN, t->vstack[g])) == NULL ||
        !pl_is_nat(pl_pin_body(pp)))
      goto tail_fallback;
    v = t->vstack[g + 1];
    t->vsp = tbase;
    t->fsp--;
    fr = pl_fpush(t);
    fr->kind = PL_F_OPENT;
    fr->opset = pl_nat_u64_clamp(pl_pin_body(pp));
    goto eval;
  }
  /* PL_BAN_SLOW */
  if (argc == 1) { /* bare head */
    v = t->vstack[g];
    t->vsp = tbase;
    t->fsp--;
    goto eval;
  }
  v = t->vstack[g]; /* before zeroing: aliased when g == tbase */
  memmove(&t->vstack[tbase + 1], &t->vstack[g + 1],
          (size_t)(argc - 1) * sizeof(pl_val));
  t->vstack[tbase] = 0; /* head slot for ret_applyn */
  t->vsp = tbase + argc;
  t->fsp--;
  fr = pl_fpush(t);
  fr->kind = PL_F_APPLYN;
  fr->argbase = (uint32_t)(tbase + 1);
  fr->argc = argc - 1;
  goto eval;

tail_fallback:
  /* build the thunk after all (fuel boundary or mis-hint) and take
   * x_ret's exit: the eval: safepoint owns any yield from here */
  pl_gc_reserve(t, bane == PL_BAN_PRIM_KNOWN ? PL_THKE_CELLS(argc + 1)
                                             : PL_THKE_CELLS(argc));
  PL_GC_FORBID(t);
  pl_val thke = bane == PL_BAN_PRIM_KNOWN
                    ? pl_mk_thke_known(t, fr->a, idx, argc, pl_vpeek(t, argc))
                    : pl_mk_thke(t, fr->a, bane, argc, pl_vpeek(t, argc));
  PL_GC_ALLOW(t);
  v = thke;
  t->vsp = tbase;
  t->fsp--;
  goto eval;
}

x_bad:
  ax_abort("exec: unsupported opcode");
}
#undef DISPATCH
#undef NEXT

  /*
   * Decompose a law-body expression under env.  Mirrors KAL, except a
   * top-level application (0 f x) is evaluated in place (function side
   * pushed through an F_APPLY frame) rather than re-deferred.
   */
eval_expr:
  if (ax_unlikely(!pl_is_whnf(expr))) {
    /* dynamically-built body: force the expr itself first */
    fr = pl_fpush(t);
    fr->kind = PL_F_KAL;
    fr->a = env;
    v = expr;
    goto eval;
  }
  if (pl_is_nat63(expr)) {
    uint32_t n = pl_env_n(pl_ptr(env)) - 1;
    if (expr <= n) {
      v = pl_env_slots(pl_ptr(env))[expr];
      goto eval;
    }
    v = expr;
    goto ret; /* literal nat */
  }
  {
    pl_cell* p = pl_as(PL_TAG_APP, expr);
    if (p != NULL && pl_app_head(p) == 0) {
      uint32_t na = pl_app_n(p);
      if (na == 1) {
        v = pl_app_args(p)[0];
        goto eval; /* literal escape (0 x) */
      }
      if (na == 2) {
        /*
         * (0 f x): interpret both subexpressions, then evaluate the
         * function side with the lazy operand parked in an F_APPLY
         * frame.  The reference kal pattern-matches (unapps) each
         * subexpression, which forces it to WHNF first; the fast path
         * below covers the common already-WHNF case, the F_KAPP frame
         * the dynamically-built ones.
         *
         */
        if (pl_is_whnf(pl_app_args(p)[0]) && pl_is_whnf(pl_app_args(p)[1])) {
          pl_vpush(t, env);
          pl_vpush(t, expr);
          pl_gc_reserve(t, 2 * PL_THUNK_CELLS);
          expr = t->vstack[t->vsp - 1];
          env = t->vstack[t->vsp - 2];
          PL_GC_FORBID(t);
          pl_cell* ep = pl_ptr(expr);
          pl_val xv = pl_kal1(t, env, pl_app_args(ep)[1]);
          pl_val fv = pl_kal1(t, env, pl_app_args(ep)[0]);
          PL_GC_ALLOW(t);
          t->vsp -= 2;
          fr = pl_fpush(t);
          fr->kind = PL_F_APPLY;
          fr->b = xv;
          v = fv;
          goto eval;
        }
        fr = pl_fpush(t);
        fr->kind = PL_F_KAPP;
        fr->a = env;
        fr->b = expr;
        fr->k = 0;
        fr->argbase = (uint32_t)t->vsp;
        pl_vpush(t, 0); /* slot for the interpreted operand */
        v = pl_app_args(p)[1];
        goto eval;
      }
    }
  }
  v = expr; /* literal */
  goto ret;

ret:
  if (t->fsp == base) {
    t->result = v;
    return PL_RUN_DONE;
  }
  fr = &t->fstack[t->fsp - 1];
  if (ax_unlikely(fr->kind >= PL_F_KIND_COUNT || ret_tbl[fr->kind] == NULL))
    ax_abort("RETURN: bad frame kind %d", (int)fr->kind);
  goto* ret_tbl[fr->kind];

ret_update:
  pl_thunk_update(t, fr->a, v);
  t->fsp--;
  goto ret;

ret_upd:
  pl_thke_update(t, fr->a, v);
  t->fsp--;
  goto ret;

ret_kal:
  env = fr->a;
  expr = v;
  t->fsp--;
  goto eval_expr;

ret_kapp: {
  /* v is a WHNF subexpression of the (0 f x) in fr->b */
  if (fr->k == 0) {
    /* interpret the operand, then force the function subexpression */
    t->vstack[fr->argbase] = v; /* park: the reserve may move it */
    pl_gc_reserve(t, PL_THUNK_CELLS);
    PL_GC_FORBID(t);
    t->vstack[fr->argbase] = pl_kal1(t, fr->a, t->vstack[fr->argbase]);
    PL_GC_ALLOW(t);
    fr->k = 1;
    v = pl_app_args(pl_ptr(fr->b))[0];
    goto eval;
  }
  /* phase 1: park the function subexpr in its own slot (argbase
   * still holds the interpreted operand) */
  pl_vpush(t, v);
  pl_gc_reserve(t, PL_THUNK_CELLS);
  PL_GC_FORBID(t);
  pl_val fv = pl_kal1(t, fr->a, t->vstack[t->vsp - 1]);
  PL_GC_ALLOW(t);
  pl_val xv = t->vstack[fr->argbase];
  t->vsp = fr->argbase;
  fr->kind = PL_F_APPLY; /* reuse the frame slot */
  fr->a = 0;
  fr->b = xv;
  fr->k = 0;
  v = fv;
  goto eval;
}

ret_seq:
  v = fr->b;
  t->fsp--;
  goto eval;

ret_apply: {
  /* APPLY-STEP: v is the WHNF head, fr->b the pending argument */
  uint64_t need = pl_arity(v);
  if (need != 1) {
    uint32_t n = 0;
    {
      pl_cell* p = pl_as(PL_TAG_APP, v);
      if (p != NULL)
        n = pl_app_n(p);
    }
    pl_vpush(t, v);
    pl_gc_reserve(t, PL_APP_CELLS(n + 1));
    PL_GC_FORBID(t);
    pl_val f2 = pl_vpop(t);
    pl_val x2 = fr->b; /* re-read: collection rewrites frames in place */
    v = pl_mk_app_snoc(t, f2, x2);
    PL_GC_ALLOW(t);
    t->fsp--;
    goto ret;
  }
  /* saturated: ENTER */
  pl_val x = fr->b;
  t->fsp--;
  hbase = t->vsp;
  {
    pl_cell* p = pl_as(PL_TAG_APP, v);
    if (p != NULL) {
      pl_vpush(t, pl_app_head(p));
      uint32_t n = pl_app_n(p);
      for (uint32_t i = 0; i < n; i++)
        pl_vpush(t, pl_app_args(p)[i]);
    } else {
      pl_vpush(t, v);
    }
  }
  pl_vpush(t, x);
  goto fast_apply;

fast_apply:
  argc = (uint32_t)(t->vsp - hbase - 1);

  /* dispatch on the ultimate head: a LAW or a pinned law falls
   * through to judge, a pinned nat enters the op table */
  pl_val head = t->vstack[hbase];
  if (pl_is_nat63(head))
    ax_abort("ENTER: direct nat head");
  if (pl_tag(head) != PL_TAG_LAW) {
    if (ax_unlikely(pl_tag(head) != PL_TAG_PIN))
      ax_abort("ENTER: bad head tag 0x%llx", (unsigned long long)pl_tag(head));
    pl_val body = pl_pin_body(pl_ptr(head));
    if (pl_is_nat(body)) {
      /* primop: o applied to one argument whose spine is the op row */
      ax_assume(argc == 1, "pinned-nat arity must be 1");
      uint64_t o = pl_nat_u64_clamp(body);
      pl_val arg = t->vstack[hbase + 1];
      t->vsp = hbase;
      fr = pl_fpush(t);
      fr->kind = PL_F_OPENT;
      fr->opset = o;
      v = arg;
      goto eval;
    }
    if (pl_is_nat63(body) || pl_tag(body) != PL_TAG_LAW)
      pl_raise_msg(t, "tried to run a pinned app or pinned pin");
    /* pinned law: fall through to judge */
  }

judge: {
  pl_cell* lp = pl_lawp(t->vstack[hbase]);
  ax_assume(pl_law_arity(lp) == argc, "JUDGE: arity mismatch");
  pl_profile_law_push(t, t->vstack[hbase]);
  if (pl_hook != NULL) {
    pl_val out;
    t->centry_depth++; /* jets are C-entry regions */
    bool handled = pl_hook(t, hbase, argc, &out);
    t->centry_depth--;
    if (handled) {
#ifdef TRACY_ENABLE
      /* pop the PROF frame pl_profile_law_push pushed; without TRACY
       * nothing was pushed and popping would eat the caller's frame */
      pl_profile_frame_end(&t->fstack[t->fsp - 1]);
      t->fsp--;
#endif
      t->vsp = hbase;
      v = out;
      goto eval;
    }
  }
  /*
   * JUDGE: the recursive-let prelude.  Scan the body for the (1 v k)
   * chain, then build the env knot in one no-collect window.
   * Layout on the value stack: [head, args… | cursor, bind-exprs…].
   */
  pl_vpush(t, pl_law_body(lp)); /* the chain cursor slot */
  jbase = hbase;
  jargc = argc;
  goto judge_scan;
}
}

ret_opent: {
  /* v is the WHNF op argument; unapp it to form [name, args…] */
  uint64_t opset = fr->opset;
  t->fsp--;
  size_t listbase = t->vsp;
  {
    pl_cell* p = pl_as(PL_TAG_APP, v);
    if (p != NULL) {
      pl_vpush(t, pl_app_head(p));
      uint32_t n = pl_app_n(p);
      for (uint32_t i = 0; i < n; i++)
        pl_vpush(t, pl_app_args(p)[i]);
    } else {
      pl_vpush(t, v);
    }
  }
  argc = (uint32_t)(t->vsp - listbase - 1);
  pl_val name = t->vstack[listbase];
  if (opset == 82 && !t->rplan_f)
    pl_raise_msg(t, "Not in RPLAN Mode");
  int idx = pl_op_lookup(opset, name, argc);
  if (idx < 0)
    pl_raise_msgf(t, "no primop %llu (argc %u)", (unsigned long long)opset,
                  argc);
  fr = pl_fpush(t);
  fr->kind = PL_F_OPARG;
  fr->op = (uint32_t)idx;
  fr->argbase = (uint32_t)(listbase + 1);
  fr->argc = argc;
  fr->k = 0;
  goto oparg_next;
}

ret_oparg: {
  /* a forced strict arg comes back: park it, move to the next bit */
  t->vstack[fr->argbase + fr->k] = v;
  fr->k++;
  goto oparg_next;
}

ret_opdeep: {
  t->vstack[fr->argbase + fr->k] = v; /* deeply normalized arg k */
  fr->k++;
  goto opdeep_next;
}

ret_nf: {
  if (pl_is_normal(v)) {
    t->fsp--;
    goto ret;
  }
  fr->kind = PL_F_NFOBJ;
  fr->a = v;
  fr->k = 0;
  pl_push_nf(t);
  v = pl_nf_field(v, 0);
  goto eval;
}

ret_nfobj: {
  pl_nf_writeback(fr->a, fr->k, v);
  fr->k++;
  if (fr->k < pl_nf_nfields(fr->a)) {
    pl_push_nf(t);
    v = pl_nf_field(fr->a, fr->k);
    goto eval;
  }
  pl_cell* p = pl_ptr(fr->a);
  p[0] = pl_hdr_make(pl_hdr_kind(p[0]), pl_hdr_flags(p[0]) | PL_F_NORMAL,
                     pl_hdr_meta(p[0]), pl_hdr_cells(p[0]));
  v = fr->a;
  t->fsp--;
  goto ret;
}

ret_exec:
  pl_vpush(t, v); /* deliver to operand stack */
  goto exec;

ret_try: {
  /* force (f x) succeeded under the barrier: the reference planTry's
   * Right, wrapped as (0 v).  The Left path lives in pl_run_caught. */
  t->fsp--;
  pl_vpush(t, v);
  pl_gc_reserve(t, PL_APP_CELLS(1));
  PL_GC_FORBID(t);
  v = pl_mk_app_from(t, 0, 1, &t->vstack[t->vsp - 1]);
  PL_GC_ALLOW(t);
  t->vsp--;
  goto ret;
}

ret_judge: {
  /* v is the forced chain node: write it back and resume the scan */
  jbase = fr->argbase;
  jargc = fr->argc;
  t->fsp--;
  t->vstack[jbase + 1 + jargc] = v;
  goto judge_scan;
}

ret_nil:
  /* planNil of the conditionally-forced value (op 66 Nor) */
  v = v == 0 ? 1 : 0;
  t->fsp--;
  goto ret;

ret_prof:
  pl_profile_frame_end(fr);
  t->fsp--;
  goto ret;

ret_applyn: {
  /* v is the WHNF head; the pending args sit at [pbase, pbase+m) with
   * the reserved head slot at pbase-1.  The head's arity decides the
   * whole application here, in one step. */
  size_t pbase = fr->argbase;
  uint32_t m = fr->argc;
  uint64_t a = pl_arity(v);
  if (a == 0 || a > m) {
    /* data head or under-applied: the result is ONE flat app */
    uint32_t k = 0;
    {
      pl_cell* p = pl_as(PL_TAG_APP, v);
      if (p != NULL)
        k = pl_app_n(p);
    }
    pl_vpush(t, v);
    pl_gc_reserve(t, PL_APP_CELLS(k + m));
    PL_GC_FORBID(t);
    pl_val f2 = pl_vpop(t);
    v = pl_mk_app_cat(t, f2, m, &t->vstack[pbase]);
    PL_GC_ALLOW(t);
    t->vsp = pbase - 1;
    t->fsp--;
    goto ret;
  }
  t->fsp--;
  if (a < m) {
    /* over-applied: the excess tail waits in F_APPLY frames, first
     * excess arg topmost (left-to-right application order) */
    for (uint32_t i = m; i > (uint32_t)a; i--) {
      fr = pl_fpush(t);
      fr->kind = PL_F_APPLY;
      fr->b = t->vstack[pbase + i - 1];
    }
    t->vsp = pbase + (uint32_t)a;
  }
  /* exact arity: enter without building any intermediate app */
  if (pl_tag(v) == PL_TAG_LAW || pl_tag(v) == PL_TAG_PIN) {
    t->vstack[pbase - 1] = v;
    hbase = pbase - 1;
    goto fast_apply;
  }
  {
    /* partial-application head: splice its spine below the pending
     * args (spines are flat, so its head is a LAW or PIN) */
    pl_cell* p = pl_as(PL_TAG_APP, v);
    if (p == NULL)
      ax_abort("APPLYN: bad head tag 0x%llx", (unsigned long long)pl_tag(v));
    uint32_t k = pl_app_n(p);
    for (uint32_t j = 0; j < k; j++)
      pl_vpush(t, 0); /* may realloc the vstack; never collects */
    memmove(&t->vstack[pbase + k], &t->vstack[pbase],
            (size_t)a * sizeof(pl_val));
    t->vstack[pbase - 1] = pl_app_head(p);
    memcpy(&t->vstack[pbase], pl_app_args(p), (size_t)k * sizeof(pl_val));
    hbase = pbase - 1;
    goto fast_apply;
  }
}

oparg_next:
  /* fr is the F_OPARG frame on top of the stack */
  fr = &t->fstack[t->fsp - 1];
  {
    const pl_opdesc* d = &pl_ops[fr->op];
    while (fr->k < fr->argc && ((d->strict_mask >> fr->k) & 1u) == 0)
      fr->k++;
    if (fr->k < fr->argc) {
      v = t->vstack[fr->argbase + fr->k];
      goto eval;
    }
    if (d->deep_mask != 0) {
      fr->kind = PL_F_OPDEEP;
      fr->k = 0;
      goto opdeep_next;
    }
  }
  goto op_body;

judge_scan:
  /*
   * The recursive-let scan, re-enterable: its complete state is the
   * value stack plus (jbase, jargc) — the chain cursor sits at
   * jbase + 1 + jargc and the collected bind exprs above it.  A
   * non-WHNF chain node (dynamically-built law body) is forced through
   * the machine under an F_JUDGE frame, so the thread can suspend or
   * block mid-scan and resume with identical state.
   */
  {
    size_t cursor = jbase + 1 + jargc;
    for (;;) {
      pl_val b = t->vstack[cursor];
      if (!pl_is_whnf(b)) {
        fr = pl_fpush(t);
        fr->kind = PL_F_JUDGE;
        fr->argbase = (uint32_t)jbase;
        fr->argc = jargc;
        v = b;
        goto eval;
      }
      pl_cell* bp = pl_as(PL_TAG_APP, b);
      if (bp != NULL && pl_app_head(bp) == 1 && pl_app_n(bp) == 2) {
        /* bp stays valid across the vpush: growing the value stack
         * reallocs the stack array, never the heap */
        pl_vpush(t, pl_app_args(bp)[0]);
        t->vstack[cursor] = pl_app_args(bp)[1];
      } else {
        break;
      }
    }
    uint32_t m = (uint32_t)(t->vsp - cursor - 1);
    uint32_t nslots = 1 + jargc + m;
    pl_gc_reserve(t, PL_ENV_CELLS(nslots) + (size_t)(m + 1) * PL_THUNK_CELLS);
    PL_GC_FORBID(t);
    pl_val envv = pl_mk_env(t, nslots);
    pl_val* slots = pl_env_slots(pl_ptr(envv));
    slots[0] = t->vstack[jbase];
    for (uint32_t i = 0; i < jargc; i++)
      slots[1 + i] = t->vstack[jbase + 1 + i];
    for (uint32_t j = 0; j < m; j++)
      slots[1 + jargc + j] = pl_mk_thunk(t, envv, t->vstack[cursor + 1 + j]);
    pl_code* code = pl_law_code(t->vstack[jbase]);
    if (code != NULL) {
      t->vsp = jbase;
      fr = pl_fpush(t);
      fr->kind = PL_F_EXEC;
      fr->a = envv;
      fr->code = code;
      fr->k = 0;
      fr->argbase = (uint32_t)t->vsp;
      PL_GC_ALLOW(t);
      goto exec;
    }
    v = pl_kal1(t, envv, t->vstack[cursor]);
    PL_GC_ALLOW(t);
    t->vsp = jbase;
    goto eval;
  }

opdeep_next:
  /* fr is the F_OPDEEP frame on top of the stack.  Deep (nf) phase
   * over the deep_mask args: payload normalization runs through the
   * machine at depth 0, so effects inside it block correctly. */
  fr = &t->fstack[t->fsp - 1];
  {
    const pl_opdesc* d = &pl_ops[fr->op];
    while (fr->k < fr->argc && ((d->deep_mask >> fr->k) & 1u) == 0)
      fr->k++;
    if (fr->k < fr->argc) {
      /* read through the frame before push_nf may move the array */
      size_t slot = fr->argbase + fr->k;
      pl_push_nf(t);
      v = t->vstack[slot];
      goto eval;
    }
  }
  goto op_body;

op_body:
  fr = &t->fstack[t->fsp - 1];
  {
    const pl_opdesc* d = &pl_ops[fr->op];
    uint32_t opi = fr->op;
    size_t argbase = fr->argbase;
    t->fsp--;          /* pop before the body so its frames take this slot */
    t->centry_depth++; /* op bodies are C-entry regions */
    pl_val r;
    /* direct op-82 effects route through the record/replay seam */
    if (!(d->opset == 82 && !d->coord && pl_io != NULL &&
          pl_io(t, opi, argbase, &r)))
      r = d->body(t, argbase);
    t->centry_depth--;
    t->vsp = argbase - 1; /* drop args and the name slot */
    if (ax_unlikely(d->coord)) {
      /*
       * Coordination effect: r is the validated request, not a
       * result.  Initiation is legal only at depth 0 — directly
       * under pl_thread_run — where the machine parks the request and
       * suspends at a RETURN point: the deposited response arrives as
       * the op's value.  At depth > 0 under an executor, blocking is
       * impossible (live native frames sit between the trampoline and
       * this step), so reaching here is a contract violation — only a
       * jet or a host re-entry could do it.  From a plain host entry
       * there is nobody to service the request, so it is a
       * (non-Try-catchable) runtime error.
       */
      if (t->centry_depth > 0) {
        ax_assume(!t->suspendable,
                  "coordination effect initiated in a C-entry region");
        pl_raise_msg(t, "actor op with no executor");
      }
      t->blocked_on = r;
      pl_profile_close_above(t, base);
      return PL_RUN_BLOCKED;
    }
    v = r;
    goto eval;
  }
}

/* ── Exception delivery (frame-based Try) ──────────────────────────────── */

/*
 * Every machine entry runs under this wrapper, which owns PLAN
 * exception delivery.  A raise longjmps here; if an F_TRY barrier
 * exists within THIS entry's frame range (and the exception is a
 * catchable PLAN_EXN, not a runtime error), the stacks unwind to the
 * barrier and the machine resumes by RETURNing (1 exn) — the reference
 * planTry's Left.  Otherwise the entry unwinds and the exception
 * re-raises to the next-outer handler (an enclosing entry's wrapper, a
 * host pl_catch, or pl_thread_run's top-level trap).
 *
 * Because Try is a frame, everything beneath it runs in the same
 * trampoline invocation: fuel yields, blocking coordination effects,
 * and resumption all work under Try, and a suspended continuation
 * carries its barriers across quanta.
 */
static pl_run_status pl_run_caught(pl_thread* t, pl_val v0, size_t base,
                                   bool at_return0) {
  /* modified across setjmp/longjmp iterations */
  volatile pl_val v = v0;
  volatile bool at_return = at_return0;
  for (;;) {
    pl_catch c;
    pl_catch_init(t, &c);
    if (setjmp(c.jb) == 0) {
      pl_run_status s = pl_run(t, v, base, at_return);
      pl_catch_pop(t, &c);
      return s;
    }
    t->handler = c.prev;
    t->centry_depth = c.centry;
    if (t->exn_msg == NULL) { /* runtime errors are not catchable */
      size_t i = t->fsp;
      while (i > base && t->fstack[i - 1].kind != PL_F_TRY)
        i--;
      if (i > base) {
        /* unwind to the barrier and deliver (1 exn) */
        pl_profile_close_above(t, i);
        t->fsp = i - 1;
        t->vsp = t->fstack[i - 1].argbase;
        pl_gc_reserve(t, PL_APP_CELLS(1));
        PL_GC_FORBID(t);
        v = pl_mk_app_from(t, 1, 1, &t->exn);
        PL_GC_ALLOW(t);
        t->exn = 0;
        at_return = true;
        continue;
      }
    }
    /* uncaught within this entry: unwind it and propagate */
    pl_profile_close_above(t, base);
    t->vsp = c.vsp;
    t->fsp = c.fsp;
    if (t->exn_msg != NULL)
      pl_raise_msg(t, t->exn_msg);
    pl_raise(t, t->exn);
  }
}

/* ── Suspendable execution ─────────────────────────────────────────────── */

void pl_thread_start(pl_thread* t, pl_val v) {
  ax_assume(!t->suspendable && t->centry_depth == 0,
            "pl_thread_start: thread is running");
  t->base_vsp = t->vsp;
  t->base_fsp = t->fsp;
  t->resume_kind = PL_RES_EVAL;
  t->resume_val = v;
  t->blocked_on = 0;
  t->pending_yield = false;
  t->status = PL_RUN_YIELDED;
}

void pl_thread_start_nf(pl_thread* t, pl_val v) {
  pl_thread_start(t, v);
  pl_frame* fr = pl_fpush(t); /* above base_fsp: pops exactly at DONE */
  fr->kind = PL_F_NF;
}

void pl_thread_start_call_nf(pl_thread* t, pl_val f, pl_val x) {
  pl_thread_start_nf(t, f);
  pl_frame* fr = pl_fpush(t); /* applied first, then the NF descent */
  fr->kind = PL_F_APPLY;
  fr->b = x;
}

void pl_thread_deposit(pl_thread* t, pl_val response) {
  ax_assume(t->status == PL_RUN_BLOCKED,
            "pl_thread_deposit: thread is not blocked");
  /* the machine resumes by RETURNing the response to the pending frame,
   * which expects a WHNF (executors build rows/nats, never thunks) */
  ax_assume(pl_is_whnf(response), "pl_thread_deposit: response must be WHNF");
  t->resume_kind = PL_RES_RETURN;
  t->resume_val = response;
  t->blocked_on = 0;
  t->status = PL_RUN_YIELDED;
}

pl_val pl_thread_result(pl_thread* t) {
  ax_assume(t->status == PL_RUN_DONE, "pl_thread_result: thread not done");
  return t->result;
}

pl_val pl_thread_request(pl_thread* t) {
  ax_assume(t->status == PL_RUN_BLOCKED,
            "pl_thread_request: thread is not blocked");
  return t->blocked_on;
}

pl_run_status pl_thread_run(pl_thread* t, uint64_t fuel) {
  ax_assume(t->centry_depth == 0 && !t->suspendable,
            "pl_thread_run: re-entered from evaluator code");
  ax_assume(t->status == PL_RUN_YIELDED,
            "pl_thread_run: thread is not runnable (status %d) — "
            "start it or deposit a response first",
            (int)t->status);
  /* the per-step check pre-decrements, so fuel 1 would yield before the
   * first step and the thread could never progress */
  ax_assume(fuel >= 2, "pl_thread_run: fuel quantum must be >= 2");
  t->fuel = fuel;
  t->pending_yield = false;
  t->suspendable = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    /* uncaught at thread top level: unwind to the entry watermarks;
     * t->exn / t->exn_msg carry the payload */
    t->handler = c.prev;
    t->vsp = t->base_vsp;
    t->fsp = t->base_fsp;
    t->centry_depth = 0;
    t->suspendable = false;
    t->fuel = UINT64_MAX;
    t->status = PL_RUN_EXN;
    return PL_RUN_EXN;
  }

  pl_run_status s;
  switch (t->resume_kind) {
  case PL_RES_EVAL:
    pl_profile_reopen_above(t, t->base_fsp);
    s = pl_run_caught(t, t->resume_val, t->base_fsp, false);
    break;
  case PL_RES_RETURN:
    pl_profile_reopen_above(t, t->base_fsp);
    s = pl_run_caught(t, t->resume_val, t->base_fsp, true);
    break;
  default:
    ax_abort("pl_thread_run: bad resume kind %d", (int)t->resume_kind);
  }
  pl_catch_pop(t, &c);
  t->suspendable = false;
  t->fuel = UINT64_MAX;
  t->status = (uint8_t)s;
  return s;
}

/* ── Entry points (host / C-entry) ─────────────────────────────────────── */

/*
 * Re-entrant evaluator call from host or op code: a C-entry region.
 * Runs with fuel inert (or pinned to the grace path under an executor)
 * and can therefore never suspend.
 */
static pl_val pl_run_centry(pl_thread* t, pl_val v, size_t base) {
  t->centry_depth++;
  pl_run_status s = pl_run_caught(t, v, base, false);
  t->centry_depth--;
  ax_assume(s == PL_RUN_DONE, "C-entry run cannot suspend");
  return t->result;
}

#ifdef PL_YIELD_STRESS
/*
 * YIELD_STRESS: at true depth 0, drive every host-API evaluation
 * through pl_thread_run at one machine step per quantum, so
 * the entire existing suite exercises suspension at every safepoint.
 * Results, exceptions, and stack effects must be indistinguishable from
 * the direct path.
 */
static pl_val pl_stress_drive(pl_thread* t, pl_val v, size_t base) {
  /* save any armed-but-not-running suspension state; vals stay rooted */
  size_t save_bvsp = t->base_vsp, save_bfsp = t->base_fsp;
  uint8_t save_kind = t->resume_kind, save_status = t->status;
  size_t mark = t->vsp;
  pl_vpush(t, t->resume_val);
  pl_vpush(t, t->blocked_on);
  pl_vpush(t, t->result);

  t->base_vsp = t->vsp;
  t->base_fsp = base;
  t->resume_kind = PL_RES_EVAL;
  t->resume_val = v;
  t->blocked_on = 0;
  t->pending_yield = false;
  t->status = PL_RUN_YIELDED;

  pl_run_status s;
  do
    s = pl_thread_run(t, 2);
  while (s == PL_RUN_YIELDED);

  pl_val r = (s == PL_RUN_DONE) ? t->result : 0;
  t->result = t->vstack[mark + 2];
  t->blocked_on = t->vstack[mark + 1];
  t->resume_val = t->vstack[mark];
  t->vsp = mark;
  t->base_vsp = save_bvsp;
  t->base_fsp = save_bfsp;
  t->resume_kind = save_kind;
  t->status = save_status;

  if (s == PL_RUN_EXN) {
    /* re-raise to the caller's handler, as the direct path would */
    if (t->exn_msg != NULL)
      pl_raise_msg(t, t->exn_msg);
    pl_raise(t, t->exn);
  }
  if (s == PL_RUN_BLOCKED) {
    /* a coordination op reached depth 0 under the stress executor; the
     * direct path raises at initiation (centry_depth > 0) — match it */
    pl_raise_msg(t, "actor op with no executor");
  }
  return r;
}
#endif

static pl_val pl_eval_public(pl_thread* t, pl_val v, size_t base) {
#ifdef PL_YIELD_STRESS
  if (!t->suspendable && t->centry_depth == 0)
    return pl_stress_drive(t, v, base);
#endif
  return pl_run_centry(t, v, base);
}

pl_val pl_whnf(pl_thread* t, pl_val v) {
  return pl_eval_public(t, v, t->fsp);
}

pl_val pl_apply(pl_thread* t, pl_val f, pl_val x) {
  size_t base = t->fsp;
  pl_frame* fr = pl_fpush(t);
  fr->kind = PL_F_APPLY;
  fr->b = x;
  return pl_eval_public(t, f, base);
}

pl_val pl_nf(pl_thread* t, pl_val v) {
  size_t base = t->fsp;
  pl_frame* fr = pl_fpush(t);
  fr->kind = PL_F_NF;
  return pl_eval_public(t, v, base);
}
