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

static void pl_law_code(pl_store* s, pl_val law, pl_code** out) {
  pl_cell* p = pl_as(PL_TAG_PIN, law);
  *out = NULL;
  if (!p) {
    return;
  }
  pl_store_get_code(s, pl_pin_hash_bytes(p), out);
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
    switch (pl_hdr_kind(p[0])) {
    case PL_K_IND:
      v = pl_ind_target(p);
      goto eval;
    case PL_K_BH:
      pl_raise_msg(t, "<<loop>>");
    case PL_K_THUNK: {
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
    }
    case PL_K_THKE: {
      /* TODO: set blackhole */
      p[0] = pl_hdr_set_flag(p[0], PL_F_HOLE);
      fr = pl_fpush(t);
      fr->kind = PL_F_UPD;
      fr->a = v;
      goto eval_thke;
    }
    default:
      ax_abort("EVAL: bad defer kind");
    }
  }
eval_thke: {
  fr = &t->fstack[t->fsp - 1];
  v = fr->a;
  pl_val* args;
  switch (pl_thke_bane(pl_ptr(v))) {
  case PL_BAN_FAST:
    hbase = t->vsp;
    argc = pl_thke_n(pl_ptr(v));
    args = pl_thke_args(pl_ptr(v));
    for (uint32_t i = 0; i < argc; i++) {
      pl_vpush(t, args[i]);
    }
    argc--;
    goto judge;
  case PL_BAN_PRIM:
    ax_abort("EVAL: no prim yet");
  case PL_BAN_SLOW:
  default:
    ax_abort("EVAL: bad bane");
  }
}

exec: {
#define NEXT() (fr->code->ops[fr->k++])
  fr = &t->fstack[t->fsp - 1];
  pl_op_t op;
  for (;;) {
    op = NEXT();
    switch (op) {
    case OP_PUSH_VAR:
      pl_vpush(t, pl_env_slots(pl_ptr(fr->a))[NEXT()]);
      break;
    case OP_PUSH_LIT:
      pl_vpush(t, NEXT());
      break;
    case OP_MK_THK: {
      argc = (uint32_t)NEXT();
      pl_bane bane = (pl_bane)NEXT();
      pl_gc_reserve(t, PL_THKE_CELLS(argc));
      PL_GC_FORBID(t);
      pl_val thke = pl_mk_thke(t, fr->a, bane, argc, pl_vpeek(t, argc));
      pl_vreplace(t, argc, thke);
      PL_GC_ALLOW(t);
      break;
    };

    case OP_INTERP:
      env = fr->a;
      expr = NEXT();
      goto eval_expr;
    case OP_RET:
      v = pl_vpop(t);
      t->vsp = fr->argbase;
      t->fsp--;
      goto ret;

    case OP_EVAL:
    default:
      ax_abort("exec: unsupported op");
    }
  }
}
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
        fr->argbase = t->vsp;
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
  /** TODO: store jump labels direct in frame */
  switch (fr->kind) {

  case PL_F_UPDATE:
    pl_thunk_update(t, fr->a, v);
    t->fsp--;
    goto ret;
  case PL_F_UPD:
    pl_thke_update(t, fr->a, v);
    t->fsp--;
    goto ret;

  case PL_F_KAL:
    env = fr->a;
    expr = v;
    t->fsp--;
    goto eval_expr;

  case PL_F_KAPP: {
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

  case PL_F_SEQ:
    v = fr->b;
    t->fsp--;
    goto eval;

  case PL_F_APPLY: {
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

    /* dispatch on the ultimate head */
    pl_val head = t->vstack[hbase];
    if (pl_is_nat63(head))
      ax_abort("ENTER: direct nat head");
    switch (pl_tag(head)) {
    case PL_TAG_LAW:
      goto judge;
    case PL_TAG_PIN: {
      pl_val body = pl_pin_body(pl_ptr(head));
      if (!pl_is_nat63(body) && pl_tag(body) == PL_TAG_LAW)
        goto judge;
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
      pl_raise_msg(t, "tried to run a pinned app or pinned pin");
    }
    default:
      ax_abort("ENTER: bad head tag 0x%llx", (unsigned long long)pl_tag(head));
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
        pl_profile_frame_end(&t->fstack[t->fsp - 1]);
        t->fsp--;
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

  case PL_F_OPENT: {
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
    fr->argbase = listbase + 1;
    fr->argc = argc;
    fr->k = 0;
    goto oparg_next;
  }

  case PL_F_OPARG: {
    /* a forced strict arg comes back: park it, move to the next bit */
    t->vstack[fr->argbase + fr->k] = v;
    fr->k++;
    goto oparg_next;
  }

  case PL_F_OPDEEP: {
    t->vstack[fr->argbase + fr->k] = v; /* deeply normalized arg k */
    fr->k++;
    goto opdeep_next;
  }

  case PL_F_NF: {
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

  case PL_F_NFOBJ: {
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

  case PL_F_EXEC:
    pl_vpush(t, v); /* deliver to operand stack */
    goto exec;

  case PL_F_TRY: {
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

  case PL_F_JUDGE: {
    /* v is the forced chain node: write it back and resume the scan */
    jbase = fr->argbase;
    jargc = fr->argc;
    t->fsp--;
    t->vstack[jbase + 1 + jargc] = v;
    goto judge_scan;
  }

  case PL_F_NIL:
    /* planNil of the conditionally-forced value (op 66 Nor) */
    v = v == 0 ? 1 : 0;
    t->fsp--;
    goto ret;

  case PL_F_PROF:
    pl_profile_frame_end(fr);
    t->fsp--;
    goto ret;

  default:
    ax_abort("RETURN: bad frame kind %d", (int)fr->kind);
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
        fr->argbase = jbase;
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
    // XX: works, revive when compiler done
    // pl_code* code = NULL;
    // pl_store* s = pl_heap_store(t->heap);
    // if (s != NULL)
    // pl_law_code(s, t->vstack[jbase], &code);
    // if (code != NULL) {
    //   t->vsp = jbase;
    //   fr = pl_fpush(t);
    //   fr->kind = PL_F_EXEC;
    //   fr->a = envv;
    //   fr->code = code;
    //   fr->k = 0;
    //   fr->argbase = t->vsp;
    //   PL_GC_ALLOW(t);
    //   goto exec;
    // }
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
