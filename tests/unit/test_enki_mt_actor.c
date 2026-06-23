#include <criterion/criterion.h>

#include "enki/mt_actor.h"
#include "test_plan.h"

/*
 * Multithreaded actor executor (em_scheduler): a worker pool drives the
 * same op-82 coordination effects as the deterministic er_scheduler.
 * These tests check semantic parity on the cases whose *results* are
 * scheduling-independent, plus parallel stress (many actors, concurrent
 * payload pins, cross-worker Recv wakes) meant to be run under TSan.
 *
 * Program-building mirrors test_enki_actor.c: arity-1 boot laws whose
 * body is KAL code applying the op-82 pin to hand-built effect rows.
 */

static pl_val code_lit(pl_thread* t, pl_val v) {
  return test_app1(t, 0, v);
}

static pl_val code_app(pl_thread* t, pl_val f, pl_val x) {
  return test_app2(t, 0, f, x);
}

/* The pin of nat 82 (rplan I/O). */
static pl_val test_p82(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, 82);
  pl_val pin = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return pin;
}

static pl_val code_effect(pl_thread* t, pl_val name, size_t n,
                          const pl_val* args) {
  size_t base = t->vsp;
  pl_vpush(t, code_lit(t, test_app(t, name, n, args)));
  pl_vpush(t, code_lit(t, test_p82(t)));
  pl_val out = code_app(t, t->vstack[base + 1], t->vstack[base]);
  t->vsp = base;
  return out;
}

/* Code sequencing a then b: (P66 % (Seq a b)), Seq strict only in a. */
static pl_val code_seq(pl_thread* t, pl_val a, pl_val b) {
  size_t base = t->vsp;
  pl_vpush(t, a);
  pl_vpush(t, b);
  pl_vpush(t, code_lit(t, ax_s3('S', 'e', 'q')));
  pl_vpush(t, code_app(t, t->vstack[base + 2], t->vstack[base]));
  pl_vpush(t, code_app(t, t->vstack[base + 3], t->vstack[base + 1]));
  pl_vpush(t, code_lit(t, test_p66(t)));
  pl_val out = code_app(t, t->vstack[base + 5], t->vstack[base + 4]);
  t->vsp = base;
  return out;
}

static pl_val actor_fn(pl_thread* t, pl_val body) {
  return test_law(t, 1, 0, body);
}

static pl_val recv_code(pl_thread* t) {
  pl_val args[1] = {0};
  return code_effect(t, ax_s4('R', 'e', 'c', 'v'), 1, args);
}

/* Code: Send `payload` to handle `to`. */
static pl_val send_code(pl_thread* t, pl_val to, pl_val payload) {
  pl_val sargs[2] = {to, payload};
  return code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs);
}

/* ── Parity with the deterministic executor ────────────────────────────── */

Test(mt_actor, single_actor_halts_with_result) {
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){0}, 4);
  em_actor* a = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(a);
  em_actor_start(a, actor_fn(t, 7));
  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(em_actor_state(a), ER_ACTOR_HALTED);
  cr_assert_eq(em_actor_result(a), 7);
  cr_assert_eq(em_actor_id(a), 0);
  em_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(mt_actor, send_to_self_then_recv) {
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){0}, 4);
  em_actor* a = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(a);
  size_t base = t->vsp;
  pl_vpush(t, send_code(t, 0, 42));
  pl_vpush(t, recv_code(t));
  pl_val body = code_seq(t, t->vstack[base], t->vstack[base + 1]);
  t->vsp = base;
  em_actor_start(a, actor_fn(t, body));

  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(em_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, em_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_n(r), 2);
  cr_assert_eq(pl_app_args(r)[0], 42);
  cr_assert_eq(pl_app_args(r)[1], 0);
  em_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(mt_actor, crash_is_isolated) {
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){0}, 4);
  em_actor* a = em_scheduler_actor(sys);
  em_actor* b = em_scheduler_actor(sys);
  {
    pl_thread* t = em_actor_thread(a);
    size_t base = t->vsp;
    pl_vpush(t, code_lit(t, ax_s5('T', 'h', 'r', 'o', 'w')));
    pl_vpush(t, code_lit(t, 9));
    pl_vpush(t, code_app(t, t->vstack[base], t->vstack[base + 1]));
    pl_vpush(t, code_lit(t, test_p66(t)));
    pl_val body = code_app(t, t->vstack[base + 3], t->vstack[base + 2]);
    t->vsp = base;
    em_actor_start(a, actor_fn(t, body));
  }
  {
    pl_thread* t = em_actor_thread(b);
    em_actor_start(b, actor_fn(t, 7));
  }
  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(em_actor_state(a), ER_ACTOR_CRASHED);
  cr_assert_eq(em_actor_state(b), ER_ACTOR_HALTED);
  cr_assert_eq(em_actor_result(b), 7);
  em_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(mt_actor, recv_blocks_until_injection) {
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){0}, 4);
  em_actor* a = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(a);
  em_actor_start(a, actor_fn(t, recv_code(t)));

  cr_assert_eq(em_scheduler_run(sys), ER_RUN_QUIESCENT);
  cr_assert_eq(em_actor_state(a), ER_ACTOR_BLOCKED);

  em_scheduler_inject(sys, a, 99);
  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(em_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, em_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 99);
  em_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(mt_actor, spawn_runs_child_and_returns_handle) {
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){0}, 4);
  em_actor* parent = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(parent);
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, 7)); /* child halts with 7 */
  pl_val spargs[1] = {t->vstack[base]};
  pl_val body = code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs);
  t->vsp = base;
  em_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(em_actor_state(parent), ER_ACTOR_HALTED);
  cr_assert_eq(em_actor_result(parent), 1); /* first minted handle */
  em_actor* child = em_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(em_actor_state(child), ER_ACTOR_HALTED);
  cr_assert_eq(em_actor_result(child), 7);
  em_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(mt_actor, cross_actor_payload_is_store_resident) {
  /* Spawn a recv-blocked child, then Send it a law: it crosses through
   * the store and the child's Recv wakes on another worker. */
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){0}, 4);
  em_actor* parent = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(parent);
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, recv_code(t)));     /* child: Recv, halt */
  pl_vpush(t, test_law(t, 2, ax_s1('K'), 1)); /* payload: the K law */
  pl_val spargs[1] = {t->vstack[base]};
  pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  pl_vpush(t, send_code(t, 1, t->vstack[base + 1]));
  pl_val body = code_seq(t, t->vstack[base + 2], t->vstack[base + 3]);
  t->vsp = base;
  em_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  em_actor* child = em_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(em_actor_state(child), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, em_actor_result(child));
  cr_assert_not_null(r);
  pl_cell* law = pl_as(PL_TAG_LAW, pl_app_args(r)[0]);
  cr_assert_not_null(law);
  cr_assert_eq(pl_law_arity(law), 2);
  cr_assert_eq(pl_law_name(law), ax_s1('K'));
  cr_assert(pl_store_owns(rt.store, pl_app_args(r)[0]),
            "payload must live in the shared store");
  em_scheduler_free(sys);
  test_rt_free(&rt);
}

/* ── Parallel stress (run me under tsan) ───────────────────────────────── */

/*
 * Right-nest n code fragments parked at vstack[base .. base+n-1] into a
 * Seq chain.  The fragments and the running accumulator stay rooted on
 * the vstack so the chain-building reserves (code_seq) cannot strand a
 * moved heap value (invariant I4).  Pops back to `base` and returns the
 * chain (no allocation follows the pop, so the bare value is safe).
 */
static pl_val code_seq_fold(pl_thread* t, size_t base, size_t n) {
  ax_assume(n >= 1, "code_seq_fold: empty");
  size_t acc = t->vsp;
  pl_vpush(t, t->vstack[base + n - 1]);
  for (size_t i = n - 1; i-- > 0;)
    t->vstack[acc] = code_seq(t, t->vstack[base + i], t->vstack[acc]);
  pl_val out = t->vstack[acc];
  t->vsp = base;
  return out;
}

#define MT_FANOUT 24

/*
 * A root spawns MT_FANOUT children, each running a self-send of a
 * structured (must-be-pinned) payload followed by a Recv.  The children
 * run concurrently across the worker pool, so they pin the same law into
 * the shared store at the same time — exercising the store's pin lock and
 * intern dedup under contention.  Every child must halt with [K-law, 0].
 */
Test(mt_actor, many_children_concurrent_pins) {
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){0}, 8);
  em_actor* root = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(root);

  size_t base = t->vsp;
  /* child body: Seq(Send 0 (K-law), Recv); each intermediate rooted. */
  pl_vpush(t, test_law(t, 2, ax_s1('K'), 1)); /* [base] K law payload */
  pl_vpush(t, send_code(t, 0, t->vstack[base]));
  pl_vpush(t, recv_code(t));
  pl_vpush(t, code_seq(t, t->vstack[base + 1], t->vstack[base + 2]));
  pl_vpush(t, actor_fn(t, t->vstack[base + 3])); /* [base+4] child fn */

  /* root body: Spawn child fn, MT_FANOUT times.  Park each spawn
   * fragment on the vstack (rooted across the next code_effect's GC). */
  size_t sbase = t->vsp;
  for (size_t i = 0; i < MT_FANOUT; i++) {
    pl_val spargs[1] = {t->vstack[base + 4]};
    pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  }
  pl_val body = code_seq_fold(t, sbase, MT_FANOUT);
  t->vsp = base;
  em_actor_start(root, actor_fn(t, body));

  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(em_actor_state(root), ER_ACTOR_HALTED);
  for (uint64_t id = 1; id <= MT_FANOUT; id++) {
    em_actor* c = em_scheduler_actor_by_id(sys, id);
    cr_assert_not_null(c);
    cr_assert_eq(em_actor_state(c), ER_ACTOR_HALTED, "child %llu not halted",
                 (unsigned long long)id);
    pl_cell* r = pl_as(PL_TAG_APP, em_actor_result(c));
    cr_assert_not_null(r);
    pl_cell* law = pl_as(PL_TAG_LAW, pl_app_args(r)[0]);
    cr_assert_not_null(law);
    cr_assert_eq(pl_law_name(law), ax_s1('K'));
    cr_assert_eq(pl_app_args(r)[1], 0);
  }
  em_scheduler_free(sys);
  test_rt_free(&rt);
}

/*
 * Cross-worker wake storm: the root spawns MT_FANOUT recv-blocked
 * children, then Sends each a nat payload.  Each child blocks on Recv on
 * some worker and is woken by the root's Send on another — stressing the
 * mailbox / BLOCKED<->RUNNABLE handshake.  Handles are deterministically
 * 1..MT_FANOUT (minted by the root in spawn order).
 */
Test(mt_actor, fanout_send_wakes_blocked_children) {
  test_rt rt = test_rt_new();
  em_scheduler* sys = em_scheduler_new(rt.store, (er_config){.quantum = 2}, 8);
  em_actor* root = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(root);

  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, recv_code(t))); /* [base] recv child fn */
  /* fragments parked on the vstack: K spawns, then K sends. */
  size_t fbase = t->vsp;
  for (size_t i = 0; i < MT_FANOUT; i++) {
    pl_val spargs[1] = {t->vstack[base]};
    pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  }
  for (size_t i = 0; i < MT_FANOUT; i++)
    pl_vpush(t, send_code(t, (pl_val)(i + 1), 77));
  pl_val body = code_seq_fold(t, fbase, 2 * MT_FANOUT);
  t->vsp = base;
  em_actor_start(root, actor_fn(t, body));

  cr_assert_eq(em_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(em_actor_state(root), ER_ACTOR_HALTED);
  for (uint64_t id = 1; id <= MT_FANOUT; id++) {
    em_actor* c = em_scheduler_actor_by_id(sys, id);
    cr_assert_not_null(c);
    cr_assert_eq(em_actor_state(c), ER_ACTOR_HALTED, "child %llu not halted",
                 (unsigned long long)id);
    pl_cell* r = pl_as(PL_TAG_APP, em_actor_result(c));
    cr_assert_not_null(r);
    cr_assert_eq(pl_app_args(r)[0], 77);
  }
  em_scheduler_free(sys);
  test_rt_free(&rt);
}
