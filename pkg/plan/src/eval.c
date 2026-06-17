#include "plan/eval.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "axsys/assume.h"
#include "axsys/perf.h"
#include "internal.h"
#include "plan/build.h"
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
  t->handler = &c->jb;
}

void pl_catch_pop(pl_thread* t, pl_catch* c) {
  t->handler = c->prev;
}

void pl_catch_unwind(pl_thread* t, pl_catch* c) {
  t->handler = c->prev;
  t->vsp = c->vsp;
  t->fsp = c->fsp;
}

/* ── Enter hook seam ───────────────────────────────────────────────────── */

static pl_enter_hook pl_hook = NULL;

void pl_set_enter_hook(pl_enter_hook hook) {
  pl_hook = hook;
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

/* ── The machine ───────────────────────────────────────────────────────── */

static pl_val pl_run(pl_thread* t, pl_val v, size_t base) {
  pl_val env, expr;
  pl_frame* fr;

eval:
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
    default:
      ax_abort("EVAL: bad defer kind");
    }
  }

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
  if (t->fsp == base)
    return v;
  fr = &t->fstack[t->fsp - 1];
  switch (fr->kind) {

  case PL_F_UPDATE:
    pl_thunk_update(t, fr->a, v);
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
      pl_val f2 = pl_vpop(t);
      pl_val x2 = fr->b; /* re-read: collection rewrites frames in place */
      PL_GC_FORBID(t);
      v = pl_mk_app_snoc(t, f2, x2);
      PL_GC_ALLOW(t);
      t->fsp--;
      goto ret;
    }
    /* saturated: ENTER */
    pl_val x = fr->b;
    t->fsp--;
    size_t hbase = t->vsp;
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
    uint32_t argc = (uint32_t)(t->vsp - hbase - 1);
    pl_val head = t->vstack[hbase];

    /* dispatch on the ultimate head */
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
    if (pl_hook != NULL) {
      pl_val out;
      if (pl_hook(t, hbase, argc, &out)) {
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
    pl_cell* lp = pl_lawp(t->vstack[hbase]);
    ax_assume(pl_law_arity(lp) == argc, "JUDGE: arity mismatch");
    size_t cursor = t->vsp;
    pl_vpush(t, pl_law_body(lp));
    uint32_t m = 0;
    for (;;) {
      pl_val b = t->vstack[cursor];
      if (!pl_is_whnf(b)) {
        /* dynamically-built law body: force the chain node */
        b = pl_run(t, b, t->fsp);
        t->vstack[cursor] = b;
      }
      pl_cell* bp = pl_as(PL_TAG_APP, b);
      if (bp != NULL && pl_app_head(bp) == 1 && pl_app_n(bp) == 2) {
        /* bp stays valid across the vpush: growing the value stack
         * reallocs the stack array, never the heap */
        pl_vpush(t, pl_app_args(bp)[0]);
        m++;
        t->vstack[cursor] = pl_app_args(bp)[1];
      } else {
        break;
      }
    }
    uint32_t nslots = 1 + argc + m;
    pl_gc_reserve(t, PL_ENV_CELLS(nslots) + (size_t)(m + 1) * PL_THUNK_CELLS);
    PL_GC_FORBID(t);
    pl_val envv = pl_mk_env(t, nslots);
    pl_val* slots = pl_env_slots(pl_ptr(envv));
    slots[0] = t->vstack[hbase];
    for (uint32_t i = 0; i < argc; i++)
      slots[1 + i] = t->vstack[hbase + 1 + i];
    for (uint32_t j = 0; j < m; j++)
      slots[1 + argc + j] = pl_mk_thunk(t, envv, t->vstack[cursor + 1 + j]);
    v = pl_kal1(t, envv, t->vstack[cursor]);
    PL_GC_ALLOW(t);
    t->vsp = hbase;
    goto eval;
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
    uint32_t argc = (uint32_t)(t->vsp - listbase - 1);
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
    t->vstack[fr->argbase] = v; /* deeply normalized arg 0 */
    goto op_body;
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
    p[0] |= (pl_cell)PL_F_NORMAL << 4;
    v = fr->a;
    t->fsp--;
    goto ret;
  }

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
      if (d->deep) {
        fr->kind = PL_F_OPDEEP;
        pl_push_nf(t);
      }
      v = t->vstack[fr->argbase + fr->k];
      goto eval;
    }
  }
  goto op_body;

op_body:
  fr = &t->fstack[t->fsp - 1];
  {
    const pl_opdesc* d = &pl_ops[fr->op];
    size_t argbase = fr->argbase;
    t->fsp--; /* pop before the body so its frames take this slot */
    pl_val r = d->body(t, argbase);
    t->vsp = argbase - 1; /* drop args and the name slot */
    v = r;
    goto eval;
  }
}

/* ── Entry points ──────────────────────────────────────────────────────── */

pl_val pl_whnf(pl_thread* t, pl_val v) {
  return pl_run(t, v, t->fsp);
}

pl_val pl_apply(pl_thread* t, pl_val f, pl_val x) {
  size_t base = t->fsp;
  pl_frame* fr = pl_fpush(t);
  fr->kind = PL_F_APPLY;
  fr->b = x;
  return pl_run(t, f, base);
}

pl_val pl_nf(pl_thread* t, pl_val v) {
  size_t base = t->fsp;
  pl_frame* fr = pl_fpush(t);
  fr->kind = PL_F_NF;
  return pl_run(t, v, base);
}
