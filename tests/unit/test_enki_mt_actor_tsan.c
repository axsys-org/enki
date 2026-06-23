#include <stdio.h>
#include <stdlib.h>

#include "enki/mt_actor.h"
#include "test_plan.h"

/*
 * Race detector for the multithreaded actor executor.  A standalone main
 * (no criterion — its runner is not sanitizer-clean), meant for
 * `make BUILD_TYPE=tsan`: it drives the parallel scenarios many times so
 * the OS scheduler interleaves workers differently across iterations,
 * exercising concurrent payload pins (store lock + intern dedup) and the
 * mailbox BLOCKED<->RUNNABLE wake handshake under ThreadSanitizer.
 */

#define EXPECT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__,   \
              #cond);                                                          \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

/* ── Program builders (mirrors test_enki_mt_actor.c) ───────────────────── */

static pl_val code_lit(pl_thread* t, pl_val v) {
  return test_app1(t, 0, v);
}

static pl_val code_app(pl_thread* t, pl_val f, pl_val x) {
  return test_app2(t, 0, f, x);
}

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

static pl_val send_code(pl_thread* t, pl_val to, pl_val payload) {
  pl_val sargs[2] = {to, payload};
  return code_effect(t, ax_s4('S', 'e', 'n', 'd'), 2, sargs);
}

/* Right-nest fragments parked at vstack[base .. base+n-1] into a Seq. */
static pl_val code_seq_fold(pl_thread* t, size_t base, size_t n) {
  size_t acc = t->vsp;
  pl_vpush(t, t->vstack[base + n - 1]);
  for (size_t i = n - 1; i-- > 0;)
    t->vstack[acc] = code_seq(t, t->vstack[base + i], t->vstack[acc]);
  pl_val out = t->vstack[acc];
  t->vsp = base;
  return out;
}

#define FANOUT  16
#define WORKERS 8
#define ITERS   60

/* Root spawns FANOUT children, each self-sends a law payload then Recvs:
 * concurrent pins of the same value into the shared store. */
static int scenario_concurrent_pins(void) {
  pl_store* store = pl_store_new_mem();
  em_scheduler* sys =
      em_scheduler_new(store, (er_config){.quantum = 3}, WORKERS);
  em_actor* root = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(root);

  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 2, ax_s1('K'), 1));
  pl_vpush(t, send_code(t, 0, t->vstack[base]));
  pl_vpush(t, recv_code(t));
  pl_vpush(t, code_seq(t, t->vstack[base + 1], t->vstack[base + 2]));
  pl_vpush(t, actor_fn(t, t->vstack[base + 3])); /* child fn at base+4 */
  size_t sbase = t->vsp;
  for (size_t i = 0; i < FANOUT; i++) {
    pl_val spargs[1] = {t->vstack[base + 4]};
    pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  }
  pl_val body = code_seq_fold(t, sbase, FANOUT);
  t->vsp = base;
  em_actor_start(root, actor_fn(t, body));

  EXPECT(em_scheduler_run(sys) == ER_RUN_IDLE);
  EXPECT(em_actor_state(root) == ER_ACTOR_HALTED);
  for (uint64_t id = 1; id <= FANOUT; id++) {
    em_actor* c = em_scheduler_actor_by_id(sys, id);
    EXPECT(c != NULL);
    EXPECT(em_actor_state(c) == ER_ACTOR_HALTED);
    pl_cell* r = pl_as(PL_TAG_APP, em_actor_result(c));
    EXPECT(r != NULL);
    pl_cell* law = pl_as(PL_TAG_LAW, pl_app_args(r)[0]);
    EXPECT(law != NULL && pl_law_name(law) == ax_s1('K'));
    EXPECT(pl_store_owns(store, pl_app_args(r)[0]));
  }
  em_scheduler_free(sys);
  pl_store_free(store);
  return EXIT_SUCCESS;
}

/* Root spawns FANOUT recv-blocked children, then Sends each one: every
 * child blocks on one worker and is woken by the Send on another. */
static int scenario_wake_storm(void) {
  pl_store* store = pl_store_new_mem();
  em_scheduler* sys =
      em_scheduler_new(store, (er_config){.quantum = 2}, WORKERS);
  em_actor* root = em_scheduler_actor(sys);
  pl_thread* t = em_actor_thread(root);

  size_t base = t->vsp;
  pl_vpush(t, actor_fn(t, recv_code(t)));
  size_t fbase = t->vsp;
  for (size_t i = 0; i < FANOUT; i++) {
    pl_val spargs[1] = {t->vstack[base]};
    pl_vpush(t, code_effect(t, ax_s5('S', 'p', 'a', 'w', 'n'), 1, spargs));
  }
  for (size_t i = 0; i < FANOUT; i++)
    pl_vpush(t, send_code(t, (pl_val)(i + 1), 77));
  pl_val body = code_seq_fold(t, fbase, 2 * FANOUT);
  t->vsp = base;
  em_actor_start(root, actor_fn(t, body));

  EXPECT(em_scheduler_run(sys) == ER_RUN_IDLE);
  for (uint64_t id = 1; id <= FANOUT; id++) {
    em_actor* c = em_scheduler_actor_by_id(sys, id);
    EXPECT(c != NULL && em_actor_state(c) == ER_ACTOR_HALTED);
    pl_cell* r = pl_as(PL_TAG_APP, em_actor_result(c));
    EXPECT(r != NULL && pl_app_args(r)[0] == 77);
  }
  em_scheduler_free(sys);
  pl_store_free(store);
  return EXIT_SUCCESS;
}

int main(void) {
  for (int i = 0; i < ITERS; i++) {
    if (scenario_concurrent_pins() != EXIT_SUCCESS) {
      fprintf(stderr, "concurrent_pins failed at iter %d\n", i);
      return EXIT_FAILURE;
    }
    if (scenario_wake_storm() != EXIT_SUCCESS) {
      fprintf(stderr, "wake_storm failed at iter %d\n", i);
      return EXIT_FAILURE;
    }
  }
  printf("mt_actor tsan stress: %d iterations clean\n", ITERS);
  return EXIT_SUCCESS;
}
