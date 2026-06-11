#include <criterion/criterion.h>

#include "enki/actor.h"
#include "test_plan.h"

/*
 * Actor runtime (spec §7–§8): er_scheduler drives er_actors — each one
 * deep normalization of (fn 0) — servicing the op-82 coordination
 * effects parked by PL_RUN_BLOCKED.
 *
 * Test programs are arity-1 laws whose body code applies the op-82 pin
 * to effect rows.  In KAL body code under the 2-slot env [self, arg]:
 * (0 f x) is application (operands deferred as thunks), (0 v) is a
 * literal, and nats >= 2 are self-literals.  Effect rows are hand-built
 * APP spines (data by the under-application invariant); rows built *by*
 * the program arise from applying name nats, which have arity 0 and so
 * also accumulate as data.
 *
 * GC discipline: a heap pl_val in a C local must not be a sibling
 * argument of another allocating call — every intermediate is parked on
 * the value stack and re-read from its slot.
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

/* Code performing (P82 % row) for a hand-built effect row. */
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

/* An arity-1 law (the actor boot fn) with the given body code. */
static pl_val actor_fn(pl_thread* t, pl_val body) {
  return test_law(t, 1, 0, body);
}

static pl_val recv_code(pl_thread* t) {
  pl_val args[1] = {0};
  return code_effect(t, ax_s4('R', 'e', 'c', 'v'), 1, args);
}

Test(actor, single_actor_halts_with_result) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  er_actor_start(a, actor_fn(t, 7)); /* body: self-literal 7 */
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(a), 7);
  cr_assert_eq(er_actor_id(a), 0);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, crash_is_isolated) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  er_actor* b = er_scheduler_actor(sys);
  {
    /* a: (P66 % (Throw 9)) */
    pl_thread* t = er_actor_thread(a);
    size_t base = t->vsp;
    pl_vpush(t, code_lit(t, ax_s5('T', 'h', 'r', 'o', 'w')));
    pl_vpush(t, code_lit(t, 9));
    pl_vpush(t, code_app(t, t->vstack[base], t->vstack[base + 1]));
    pl_vpush(t, code_lit(t, test_p66(t)));
    pl_val body = code_app(t, t->vstack[base + 3], t->vstack[base + 2]);
    t->vsp = base;
    er_actor_start(a, actor_fn(t, body));
  }
  {
    pl_thread* t = er_actor_thread(b);
    er_actor_start(b, actor_fn(t, 7));
  }
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_CRASHED);
  cr_assert_eq(er_actor_state(b), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(b), 7);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

/* Send 42 to self (handle 0), then Recv it: result [42, 0]. */
static void run_self_ping(pl_store* store, uint64_t quantum) {
  er_scheduler* sys = er_scheduler_new(store, (er_config){.quantum = quantum});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  size_t base = t->vsp;
  pl_val sargs[2] = {0, 42};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_vpush(t, recv_code(t));
  pl_val body = code_seq(t, t->vstack[base], t->vstack[base + 1]);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_head(r), 0);
  cr_assert_eq(pl_app_n(r), 2);
  cr_assert_eq(pl_app_args(r)[0], 42);
  cr_assert_eq(pl_app_args(r)[1], 0); /* no caps: the empty row is 0 */
  er_scheduler_free(sys);
}

Test(actor, send_to_self_then_recv) {
  test_rt rt = test_rt_new();
  run_self_ping(rt.store, 0);
  test_rt_free(&rt);
}

Test(actor, results_independent_of_quantum) {
  /* R4 smoke: one machine step per quantum and a huge quantum agree */
  test_rt rt = test_rt_new();
  run_self_ping(rt.store, 2);
  run_self_ping(rt.store, 1 << 20);
  test_rt_free(&rt);
}

Test(actor, tiny_heaps_collect_through_service) {
  /* 256-cell semispaces force collection (and growth) inside pinning
   * and response building — a GC-pressure pass over the service paths
   * without the PL_GC_STRESS build flag. */
  test_rt rt = test_rt_new();
  er_scheduler* sys =
      er_scheduler_new(rt.store, (er_config){.quantum = 2, .heap_cells = 256});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  size_t base = t->vsp;
  pl_val sargs[2] = {0, 42};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_vpush(t, recv_code(t));
  pl_val body = code_seq(t, t->vstack[base], t->vstack[base + 1]);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 42);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, recv_blocks_until_injection) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  er_actor_start(a, actor_fn(t, recv_code(t)));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_QUIESCENT);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_BLOCKED);

  er_scheduler_inject(sys, a, 99);
  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(a));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 99);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, spawn_runs_child_and_returns_handle) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* parent = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(parent);
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, 7)); /* child halts with 7 */
  pl_val spargs[1] = {t->vstack[base]};
  pl_val body = code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs);
  t->vsp = base;
  er_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(parent), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(parent), 1); /* first minted handle */
  er_actor* child = er_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(er_actor_state(child), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(child), 7);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, sendcaps_reminted_in_receiver) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* parent = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(parent);
  /* parent: Seq(Spawn <recv-child>, SendCaps 1 5 [0]); the child handle
   * is deterministically 1 (first minted), so the row embeds it. */
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, recv_code(t))); /* child: Recv, halt */
  pl_val spargs[1] = {t->vstack[base]};
  pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  pl_vpush(t, test_app1(t, 0, 0)); /* caps row [0]: self */
  pl_val scargs[3] = {1, 5, t->vstack[base + 2]};
  pl_vpush(t, code_effect(t, ax_s8('S', 'e', 'n', 'd', 'C', 'a', 'p', 's'), 3,
                          scargs));
  pl_val body = code_seq(t, t->vstack[base + 1], t->vstack[base + 3]);
  t->vsp = base;
  er_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(parent), ER_ACTOR_HALTED);
  cr_assert_eq(er_actor_result(parent), 0); /* SendCaps response */
  er_actor* child = er_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(er_actor_state(child), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(child));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_args(r)[0], 5);
  pl_cell* caps = pl_as(PL_TAG_APP, pl_app_args(r)[1]);
  cr_assert_not_null(caps);
  cr_assert_eq(pl_app_n(caps), 1);
  cr_assert_eq(pl_app_args(caps)[0], 1); /* fresh receiver-local handle */
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, send_to_closed_handle_crashes_sender) {
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* a = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(a);
  /* Seq(CloseHandle 0, Send 0 9): the self handle is revocable; the
   * send through the closed handle is the reference's host error. */
  size_t base = t->vsp;
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)"CloseHandle", 11));
  pl_val cargs[1] = {0};
  pl_vpush(t, code_effect(t, t->vstack[base], 1, cargs));
  pl_val sargs[2] = {0, 9};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_val body = code_seq(t, t->vstack[base + 1], t->vstack[base + 2]);
  t->vsp = base;
  er_actor_start(a, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  cr_assert_eq(er_actor_state(a), ER_ACTOR_CRASHED);
  er_scheduler_free(sys);
  test_rt_free(&rt);
}

Test(actor, cross_actor_payload_is_store_resident) {
  /* Send a structured payload (a law) to a recv-blocked child: it must
   * arrive intact through the store, forced at send (M1/M2, D4). */
  test_rt rt = test_rt_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  er_actor* parent = er_scheduler_actor(sys);
  pl_thread* t = er_actor_thread(parent);
  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, recv_code(t)));     /* child: Recv, halt */
  pl_vpush(t, test_law(t, 2, ax_s1('K'), 1)); /* payload: the K law */
  pl_val spargs[1] = {t->vstack[base]};
  pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  pl_val sargs[2] = {1, t->vstack[base + 1]};
  pl_vpush(t, code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs));
  pl_val body = code_seq(t, t->vstack[base + 2], t->vstack[base + 3]);
  t->vsp = base;
  er_actor_start(parent, actor_fn(t, body));

  cr_assert_eq(er_scheduler_run(sys), ER_RUN_IDLE);
  er_actor* child = er_scheduler_actor_by_id(sys, 1);
  cr_assert_not_null(child);
  cr_assert_eq(er_actor_state(child), ER_ACTOR_HALTED);
  pl_cell* r = pl_as(PL_TAG_APP, er_actor_result(child));
  cr_assert_not_null(r);
  pl_cell* law = pl_as(PL_TAG_LAW, pl_app_args(r)[0]);
  cr_assert_not_null(law);
  cr_assert_eq(pl_law_arity(law), 2);
  cr_assert_eq(pl_law_name(law), ax_s1('K'));
  cr_assert(pl_store_owns(rt.store, pl_app_args(r)[0]),
            "payload must live in the shared store");
  er_scheduler_free(sys);
  test_rt_free(&rt);
}
