#include <criterion/criterion.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "axsys/profile.h"
#include "test_plan.h"

/*
 * op 83 (HTTP driver) at the plan layer: Fetch is a coordination
 * effect like Send/Recv — the body rebuilds the request spine
 * [Fetch, req, cfg] and the machine parks it with PL_RUN_BLOCKED.
 * Both args deep-normalize at initiation.  All shape/URL validation
 * happens at service time in pkg/enki; none of it is visible here.
 *
 * Redexes are driven through lazy thunks with KAL body code, as in
 * test_plan_op82.c.
 */

/* A lazy thunk whose body expr runs under an empty (1-slot) env. */
static pl_val test_thunk(pl_thread* t, pl_val expr) {
  size_t base = t->vsp;
  pl_vpush(t, expr);
  pl_gc_reserve(t, PL_ENV_CELLS(1) + PL_THUNK_CELLS);
  pl_val env = pl_mk_env(t, 1);
  pl_val out = pl_mk_thunk(t, env, t->vstack[base]);
  t->vsp = base;
  return out;
}

/* The pin of nat 83, the HTTP driver op set. */
static pl_val test_p83(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, 83);
  pl_val pin = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return pin;
}

/* A thunk forcing (P83 % (name args…)): body (0 (0 P83) (0 row)). */
static pl_val test_op83_thunk(pl_thread* t, pl_val name, size_t n,
                              const pl_val* args) {
  size_t base = t->vsp;
  pl_vpush(t, test_app(t, name, n, args));
  pl_vpush(t, test_app1(t, 0, t->vstack[base])); /* (0 row) */
  pl_vpush(t, test_app1(t, 0, test_p83(t)));     /* (0 P83) */
  pl_val expr = test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]);
  t->vsp = base;
  return test_thunk(t, expr);
}

static pl_val test_throwing(pl_thread* t, uint64_t code) {
  size_t base = t->vsp;
  pl_val args[1] = {code};
  pl_vpush(t, test_app(t, ax_s5('T', 'h', 'r', 'o', 'w'), 1, args));
  pl_vpush(t, test_app1(t, 0, t->vstack[base]));
  pl_vpush(t, test_app1(t, 0, test_p66(t)));
  pl_val expr = test_app2(t, 0, t->vstack[base + 2], t->vstack[base + 1]);
  t->vsp = base;
  return test_thunk(t, expr);
}

/* Run until the thread leaves the runnable set (2-fuel quanta). */
static pl_run_status test_run(pl_thread* t) {
  pl_run_status s;
  int quanta = 0;
  do {
    s = pl_thread_run(t, 2);
    cr_assert_lt(++quanta, 1 << 20, "runaway resume loop");
  } while (s == PL_RUN_YIELDED);
  return s;
}

#define FETCH ax_s5('F', 'e', 't', 'c', 'h')

static pl_val test_zone_call(pl_thread* t, const char* op_c, pl_val arg,
                             pl_run_status want) {
  size_t base = t->vsp;
  pl_vpush(t, arg);
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)op_c, strlen(op_c)));
  pl_val args[1] = {t->vstack[base]};
  pl_thread_start(t, test_op83_thunk(t, t->vstack[base + 1], 1, args));
  cr_assert_eq(test_run(t), want);
  pl_val result = want == PL_RUN_DONE ? pl_thread_result(t) : 0;
  t->vsp = base;
  return result;
}

static char* test_read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  cr_assert_not_null(f, "failed to open `%s`", path);
  cr_assert_eq(fseek(f, 0, SEEK_END), 0);
  long end = ftell(f);
  cr_assert_geq(end, 0);
  cr_assert_eq(fseek(f, 0, SEEK_SET), 0);
  char* data = malloc((size_t)end + 1);
  cr_assert_not_null(data);
  cr_assert_eq(fread(data, 1, (size_t)end, f), (size_t)end);
  data[end] = '\0';
  cr_assert_eq(fclose(f), 0);
  return data;
}

static size_t test_count(const char* haystack, const char* needle) {
  size_t count = 0;
  size_t needle_n = strlen(needle);
  for (const char* p = haystack; (p = strstr(p, needle)) != NULL; p += needle_n)
    count++;
  return count;
}

Test(op83, zones_have_distinct_handles_and_end_non_lifo) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;

  pl_val one = pl_nat_from_bytes(t, (const uint8_t*)"one", 3);
  pl_val h1 = test_zone_call(t, "ZoneStart", one, PL_RUN_DONE);
  pl_vpush(t, h1);
  pl_val two = pl_nat_from_bytes(t, (const uint8_t*)"two", 3);
  pl_val h2 = test_zone_call(t, "ZoneStart", two, PL_RUN_DONE);
  pl_vpush(t, h2);

  cr_assert_neq(t->vstack[base], t->vstack[base + 1]);
  cr_assert_eq(t->profile_zone_n, 2);
  cr_assert_eq(test_zone_call(t, "ZoneEnd", t->vstack[base], PL_RUN_DONE), 0);
  cr_assert_eq(t->profile_zone_n, 1);
  cr_assert_eq(t->profile_zones[0].handle, t->vstack[base + 1]);
  cr_assert_eq(test_zone_call(t, "ZoneEnd", t->vstack[base + 1], PL_RUN_DONE),
               0);
  cr_assert_eq(t->profile_zone_n, 0);

  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, zone_handles_reject_duplicate_unknown_and_cross_thread) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  pl_val h = test_zone_call(t, "ZoneStart", 7, PL_RUN_DONE);
  pl_vpush(t, h);
  (void)test_zone_call(t, "ZoneEnd", t->vstack[base], PL_RUN_DONE);
  (void)test_zone_call(t, "ZoneEnd", t->vstack[base], PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);

  pl_val h2 = test_zone_call(t, "ZoneStart", 8, PL_RUN_DONE);
  pl_vpush(t, h2);
  pl_thread* other = pl_thread_new(rt.heap);
  other->rplan_f = true;
  (void)test_zone_call(other, "ZoneEnd", t->vstack[base + 1], PL_RUN_EXN);
  cr_assert_not_null(other->exn_msg);
  cr_assert_eq(t->profile_zone_n, 1);

  pl_thread_free(other);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, zone_start_requires_nat_label) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  pl_val not_nat = test_app1(t, 0, 1);
  (void)test_zone_call(t, "ZoneStart", not_nat, PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);
  cr_assert_eq(t->profile_zone_n, 0);
  test_rt_free(&rt);
}

Test(op83, zone_handle_and_copied_label_survive_gc) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  static const uint8_t label[] = "a label longer than an immediate nat";
  pl_val h = test_zone_call(t, "ZoneStart",
                            pl_nat_from_bytes(t, label, sizeof(label) - 1),
                            PL_RUN_DONE);
  cr_assert_not_null(pl_as(PL_TAG_APP, h));

  pl_gc_collect_now(t);
  cr_assert_eq(t->profile_zone_n, 1);
  cr_assert_eq(t->profile_zones[0].name_n, sizeof(label) - 1);
  cr_assert_arr_eq(t->profile_zones[0].name, label, sizeof(label) - 1);
  (void)test_zone_call(t, "ZoneEnd", t->profile_zones[0].handle, PL_RUN_DONE);
  cr_assert_eq(t->profile_zone_n, 0);
  test_rt_free(&rt);
}

Test(op83, host_unwind_discards_zones_created_inside_catch) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)test_zone_call(t, "ZoneStart", 9, PL_RUN_DONE);
    cr_assert_eq(t->profile_zone_n, 1);
    pl_raise(t, 42);
  }
  pl_catch_unwind(t, &c);
  cr_assert_eq(t->profile_zone_n, 0);
  test_rt_free(&rt);
}

Test(op83, zone_pauses_across_yield_block_and_resume) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  (void)test_zone_call(t, "ZoneStart", 10, PL_RUN_DONE);

  pl_val args[2] = {11, 12};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);
  cr_assert_eq(t->profile_zone_n, 1);
  cr_assert_not(t->profile_zones[0].live);
  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(t->profile_zone_n, 1);
  cr_assert_not(t->profile_zones[0].live);

  (void)test_zone_call(t, "ZoneEnd", t->profile_zones[0].handle, PL_RUN_DONE);
  test_rt_free(&rt);
}

static pl_val test_start_then_throw_fetch(pl_thread* t) {
  size_t base = t->vsp;
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)"ZoneStart", 9));
  pl_val start_args[1] = {13};
  pl_vpush(t, test_op83_thunk(t, t->vstack[base], 1, start_args));
  pl_vpush(t, test_throwing(t, 77));
  pl_val fetch_args[2] = {t->vstack[base + 1], t->vstack[base + 2]};
  pl_val out = test_op83_thunk(t, FETCH, 2, fetch_args);
  t->vsp = base;
  return out;
}

Test(op83, uncaught_exception_discards_zones_from_failed_run) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  pl_thread_start(t, test_start_then_throw_fetch(t));
  cr_assert_eq(test_run(t), PL_RUN_EXN);
  cr_assert_null(t->exn_msg);
  cr_assert_eq(t->exn, 77);
  cr_assert_eq(t->profile_zone_n, 0);
  test_rt_free(&rt);
}

Test(op83, try_discards_zones_created_inside_caught_region) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  pl_vpush(t, test_start_then_throw_fetch(t));
  pl_vpush(t, test_law(t, 1, 0, t->vstack[base]));
  pl_val args[2] = {t->vstack[base + 1], 0};
  pl_val result = test_op66(t, ax_s3('T', 'r', 'y'), 2, args);
  pl_cell* p = pl_as(PL_TAG_APP, result);
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), 1);
  cr_assert_eq(pl_app_args(p)[0], 77);
  cr_assert_eq(t->profile_zone_n, 0);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, chrome_json_escapes_labels_and_splits_host_entries) {
  char path[] = "/tmp/enki-profile-zone-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  cr_assert_eq(close(fd), 0);

  cr_assert(ax_profile_json_start(path));
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  const uint8_t label[] = {'a', '"', '\\', '/', '\n', 0x80};
  pl_val h = test_zone_call(
      t, "ZoneStart", pl_nat_from_bytes(t, label, sizeof(label)), PL_RUN_DONE);
  (void)test_zone_call(t, "ZoneEnd", h, PL_RUN_DONE);
  test_rt_free(&rt);
  cr_assert(ax_profile_json_finish());

  char* json = test_read_file(path);
  cr_assert_not_null(strstr(json, "{\"traceEvents\":["));
  cr_assert_not_null(strstr(json, "\"cat\":\"splan.zone\""));
  cr_assert_not_null(strstr(json, "\"ph\":\"M\""));
  cr_assert_not_null(strstr(json, "PLAN thread "));
  cr_assert_not_null(strstr(json, "a\\\"\\\\\\/\\n\\u0080"));
  size_t begins = test_count(json, "\"ph\":\"B\"");
  size_t ends = test_count(json, "\"ph\":\"E\"");
  cr_assert_geq(begins, 2); /* at least one segment per host entry */
  cr_assert_eq(begins, ends);
  cr_assert_not_null(strstr(json, "\"args\":{\"zone\":"));
  cr_assert_not_null(strstr(json, "],\"displayTimeUnit\":\"ms\"}"));
  free(json);
  cr_assert_eq(unlink(path), 0);
}

typedef struct test_json_worker_args {
  uint64_t tid;
} test_json_worker_args;

static void* test_json_worker(void* arg_v) {
  test_json_worker_args* arg = arg_v;
  char name[32];
  int n = snprintf(name, sizeof(name), "PLAN thread %llu",
                   (unsigned long long)arg->tid);
  ax_profile_json_thread_name(arg->tid, name, (size_t)n);
  for (uint64_t i = 0; i < 100; i++) {
    ax_profile_json_zone_begin(arg->tid, i, (const uint8_t*)"work", 4);
    ax_profile_json_zone_end(arg->tid, i, (const uint8_t*)"work", 4);
  }
  return NULL;
}

Test(op83, chrome_json_serializes_concurrent_lanes) {
  char path[] = "/tmp/enki-profile-concurrent-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  cr_assert_eq(close(fd), 0);
  cr_assert(ax_profile_json_start(path));

  pthread_t threads[2];
  test_json_worker_args args[2] = {{.tid = 101}, {.tid = 202}};
  for (size_t i = 0; i < 2; i++)
    cr_assert_eq(pthread_create(&threads[i], NULL, test_json_worker, &args[i]),
                 0);
  for (size_t i = 0; i < 2; i++)
    cr_assert_eq(pthread_join(threads[i], NULL), 0);
  cr_assert(ax_profile_json_finish());

  char* json = test_read_file(path);
  cr_assert_eq(test_count(json, "\"ph\":\"M\""), 2);
  cr_assert_eq(test_count(json, "\"ph\":\"B\""), 200);
  cr_assert_eq(test_count(json, "\"ph\":\"E\""), 200);
  cr_assert_not_null(strstr(json, "\"tid\":101"));
  cr_assert_not_null(strstr(json, "\"tid\":202"));
  free(json);
  cr_assert_eq(unlink(path), 0);
}

Test(op83, readfolder_parks_then_deposit_resumes) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)"ReadFolder", 10));
  pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)"folder", 6));
  pl_val args[1] = {t->vstack[base + 1]};
  pl_thread_start(t, test_op83_thunk(t, t->vstack[base], 1, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);

  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), t->vstack[base]);
  cr_assert_eq(pl_app_n(p), 1);
  cr_assert_eq(pl_app_args(p)[0], t->vstack[base + 1]);

  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 0);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, fetch_parks_then_deposit_resumes) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t vsp0 = t->vsp, fsp0 = t->fsp;
  pl_val args[2] = {11, 22}; /* opaque to the plan layer */
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);

  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_n(p), 2);
  cr_assert_eq(pl_app_args(p)[0], 11);
  cr_assert_eq(pl_app_args(p)[1], 22);

  /* a synthetic (0 resp) result row, as the executor would deposit */
  size_t rb = t->vsp;
  pl_vpush(t, test_app1(t, 0, 200));
  pl_val resp = t->vstack[rb];
  t->vsp = rb;
  pl_thread_deposit(t, resp);
  cr_assert_eq(t->blocked_on, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  pl_cell* r = pl_as(PL_TAG_APP, pl_thread_result(t));
  cr_assert_not_null(r);
  cr_assert_eq(pl_app_head(r), 0);
  cr_assert_eq(pl_app_args(r)[0], 200);
  cr_assert_eq(t->vsp, vsp0);
  cr_assert_eq(t->fsp, fsp0);
  test_rt_free(&rt);
}

Test(op83, fetch_normalizes_both_args_at_initiation) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  pl_vpush(t, test_thunk(t, 7)); /* req: forced by the deep mask */
  pl_vpush(t, test_thunk(t, 9)); /* cfg: forced by the deep mask */
  pl_val args[2] = {t->vstack[base], t->vstack[base + 1]};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);

  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_n(p), 2);
  /* both payloads were deep-normalized before the request parked */
  cr_assert_eq(pl_app_args(p)[0], 7);
  cr_assert_eq(pl_app_args(p)[1], 9);

  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 0);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, payload_effects_block_before_the_request) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  /* A Fetch whose cfg normalization performs a Recv: the inner effect
   * blocks FIRST, as the caller's own execution, and only then does
   * the Fetch request park with the settled payload. */
  size_t base = t->vsp;
  pl_val rargs[1] = {0};
  pl_vpush(t, test_app(t, ax_s4('R', 'e', 'c', 'v'), 1, rargs));
  pl_vpush(t, test_app1(t, 0, t->vstack[base])); /* (0 recvrow) */
  size_t pb = t->vsp;
  pl_vpush(t, 82);
  pl_vpush(t, pl_pin(t, t->vstack[pb]));
  pl_vpush(t, test_app1(t, 0, t->vstack[pb + 1])); /* (0 P82) */
  pl_vpush(t, test_app2(t, 0, t->vstack[pb + 2], t->vstack[base + 1]));
  pl_val recv = test_thunk(t, t->vstack[pb + 3]);
  t->vsp = base;
  pl_vpush(t, recv);

  pl_val fargs[2] = {5, t->vstack[base]};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, fargs));

  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);
  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), ax_s4('R', 'e', 'c', 'v')); /* inner first */

  pl_thread_deposit(t, 6);
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);
  p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_args(p)[0], 5);
  cr_assert_eq(pl_app_args(p)[1], 6); /* the deposited response */

  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 0);
  t->vsp = base;
  test_rt_free(&rt);
}

Test(op83, requires_rplan_mode) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t; /* rplan_f defaults to false */
  pl_val args[2] = {11, 22};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);
  test_rt_free(&rt);
}

Test(op83, unknown_op_is_runtime_error) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  pl_val args[2] = {11, 22};
  pl_thread_start(t, test_op83_thunk(t, ax_s4('F', 'r', 'o', 'b'), 2, args));
  cr_assert_eq(test_run(t), PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);
  test_rt_free(&rt);
}

Test(op83, wrong_arity_is_runtime_error) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  /* Fetch is argc 2; a 1-arg row has no matching bucket entry. */
  pl_val args[1] = {11};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 1, args));
  cr_assert_eq(test_run(t), PL_RUN_EXN);
  cr_assert_not_null(t->exn_msg);
  test_rt_free(&rt);
}

Test(op83, blocked_request_survives_gc) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  pl_vpush(t, test_thunk(t, 7));
  pl_val args[2] = {t->vstack[base], 3};
  pl_thread_start(t, test_op83_thunk(t, FETCH, 2, args));
  cr_assert_eq(test_run(t), PL_RUN_BLOCKED);

  pl_gc_collect_now(t); /* moves the request and the parked continuation */
  pl_cell* p = pl_as(PL_TAG_APP, pl_thread_request(t));
  cr_assert_not_null(p);
  cr_assert_eq(pl_app_head(p), FETCH);
  cr_assert_eq(pl_app_args(p)[0], 7);
  cr_assert_eq(pl_app_args(p)[1], 3);

  pl_thread_deposit(t, 0);
  cr_assert_eq(test_run(t), PL_RUN_DONE);
  cr_assert_eq(pl_thread_result(t), 0);
  t->vsp = base;
  test_rt_free(&rt);
}
