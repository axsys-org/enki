#include <criterion/criterion.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ENKI_WISP_BIN
#define ENKI_WISP_BIN "wisp"
#endif

#ifndef ENKI_REAVER_PLAN_DIR
#define ENKI_REAVER_PLAN_DIR "./reaver/src/plan"
#endif

static const char* reaver_plan_dir(void) {
  const char* env_c = getenv("ENKI_REAVER_PLAN_DIR");
  return env_c != NULL && env_c[0] != '\0' ? env_c : ENKI_REAVER_PLAN_DIR;
}

static const char* wisp_bin(void) {
  const char* env_c = getenv("ENKI_WISP_BIN");
  return env_c != NULL && env_c[0] != '\0' ? env_c : ENKI_WISP_BIN;
}

Test(wisp_cli, reaver_module_exits_zero) {
  char command_c[4096];
  int command_s = snprintf(command_c, sizeof(command_c), "%s %s reaver",
                           wisp_bin(), reaver_plan_dir());

  cr_assert(command_s >= 0 && (size_t)command_s < sizeof(command_c),
            "failed to build wisp command");

  int status = system(command_c);

  cr_assert_neq(status, -1, "failed to run `%s`", command_c);
  cr_assert(WIFEXITED(status), "`%s` did not exit normally: status=%d",
            command_c, status);
  cr_assert_eq(WEXITSTATUS(status), 0, "`%s` exited with %d", command_c,
               WEXITSTATUS(status));
}

/*
 * Save / snapshot round trip, the same flow as the reference x/reaver
 * driver (PlanAssembler.hs): assemble a module in BPLAN mode that pins a
 * program and calls Save, then resume from the snapshot directory in
 * RPLAN mode and run the bound program.
 */

static int run_cmd(const char* cmd, char* out, size_t out_cap) {
  FILE* p = popen(cmd, "r");
  if (p == NULL)
    return -1;
  size_t n = 0;
  int ch;
  while ((ch = fgetc(p)) != EOF && n + 1 < out_cap)
    out[n++] = (char)ch;
  out[n] = '\0';
  return pclose(p);
}

Test(wisp_cli, save_snapshot_round_trip) {
  char dir[] = "/tmp/enki-snaptest-XXXXXX";
  cr_assert_not_null(mkdtemp(dir), "failed to make temp dir");

  char path[512];
  (void)snprintf(path, sizeof(path), "%s/save1.plan", dir);
  FILE* f = fopen(path, "w");
  cr_assert_not_null(f);
  /* Print writes the raw bytes of a plain string nat (op 82, RPLAN). */
  fprintf(f,
          "(#bind Print\n"
          "  (#pin (#law \"Print\" (Print x) ((#pin \"R\") (\"Print\" x)))))\n"
          "(#bind Pin (#pin (#law \"Pin\" (Pin x) ((#pin \"B\") (\"Pin\" "
          "x)))))\n"
          "(#bind Save (#pin (#law \"Save\" (Save x) ((#pin \"B\") (\"Save\" "
          "x)))))\n"
          "(#bind prog\n"
          "  (#pin (#law \"prog\" (prog args) ((#pin \"R\") (\"Print\" "
          "\"snap-ok\")))))\n"
          "(Save prog)\n");
  fclose(f);

  char cmd[1024];
  char out[256];

  /* BPLAN assemble: pins prog, Save writes snap/ under the cwd */
  (void)snprintf(cmd, sizeof(cmd), "cd %s && %s %s save1 >/dev/null 2>&1", dir,
                 wisp_bin(), dir);
  int st = system(cmd);
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) == 0, "assemble failed");

  (void)snprintf(path, sizeof(path), "%s/snap/root.plan", dir);
  cr_assert_eq(access(path, F_OK), 0, "snap/root.plan not written");

  /* RPLAN resume: run the saved program, which prints "snap-ok" */
  (void)snprintf(cmd, sizeof(cmd), "cd %s && %s snap root _ 2>/dev/null", dir,
                 wisp_bin());
  st = run_cmd(cmd, out, sizeof(out));
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) == 0, "resume failed");
  cr_assert_not_null(strstr(out, "snap-ok"),
                     "resumed program did not print; got `%s`", out);
}

Test(wisp_cli, rplan_op_rejected_in_bplan) {
  char dir[] = "/tmp/enki-bplan-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));

  char path[512];
  (void)snprintf(path, sizeof(path), "%s/bad.plan", dir);
  FILE* f = fopen(path, "w");
  cr_assert_not_null(f);
  /* calling an op-82 (rplan) op at top level in BPLAN mode must fail */
  fprintf(f, "((#pin \"R\") (\"Print\" \"nope\"))\n");
  fclose(f);

  char cmd[1024];
  (void)snprintf(cmd, sizeof(cmd), "%s %s bad >/dev/null 2>&1", wisp_bin(),
                 dir);
  int st = system(cmd);
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) != 0,
            "RPLAN op should be rejected outside RPLAN mode");
}

/*
 * P4: actor scenarios through the boot driver — the wisp boot thread is
 * the root actor (the reference withNewRts), spawned actors run under
 * the deterministic executor, and op-82 coordination effects in the run
 * function block and resume instead of raising.
 */

static int run_scenario(const char* bin, const char* fn, char* out,
                        size_t cap) {
  char cmd[1024];
  (void)snprintf(cmd, sizeof(cmd), "%s tests/plan actors %s 2>/dev/null", bin,
                 fn);
  return run_cmd(cmd, out, cap);
}

Test(wisp_cli, actor_ping_echo) {
  char out[256];
  int st = run_scenario(wisp_bin(), "ping", out, sizeof(out));
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) == 0, "ping run failed");
  cr_assert_str_eq(out, "hello-via-echo");
}

Test(wisp_cli, actor_counter_holds_state) {
  char out[256];
  int st = run_scenario(wisp_bin(), "count", out, sizeof(out));
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) == 0, "count run failed");
  cr_assert_str_eq(out, "3");
}

Test(wisp_cli, actor_deadlock_is_reported) {
  char out[256];
  int st = run_scenario(wisp_bin(), "stuck", out, sizeof(out));
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) != 0,
            "an unanswerable Recv must fail the run");
}

Test(wisp_cli, actor_recv_under_try) {
  char out[256];
  int st = run_scenario(wisp_bin(), "tryrecv", out, sizeof(out));
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) == 0, "tryrecv run failed");
  cr_assert_str_eq(out, "tm");
}

/* Differential vs the reaver oracle (a local Haskell reference build);
 * skipped where the binary is absent (e.g. the nix sandbox). */
Test(wisp_cli, actor_differential_vs_reaver) {
  const char* oracle = getenv("ENKI_REAVER_BIN");
  if (oracle == NULL || oracle[0] == '\0')
    oracle = "./reaver/dist-newstyle/build/aarch64-osx/ghc-9.10.3/"
             "plan-assembler-0.1.0.0/x/plan-assembler/build/plan-assembler/"
             "plan-assembler";
  if (access(oracle, X_OK) != 0)
    cr_skip_test("reaver oracle binary not available");

  static const char* const fns[] = {"ping", "count", "tryrecv"};
  for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
    char ours[256], theirs[256];
    int st1 = run_scenario(wisp_bin(), fns[i], ours, sizeof(ours));
    int st2 = run_scenario(oracle, fns[i], theirs, sizeof(theirs));
    cr_assert(WIFEXITED(st1) && WEXITSTATUS(st1) == 0, "wisp %s failed",
              fns[i]);
    cr_assert(WIFEXITED(st2) && WEXITSTATUS(st2) == 0, "reaver %s failed",
              fns[i]);
    cr_assert_str_eq(ours, theirs, "differential mismatch on %s", fns[i]);
  }
}

Test(wisp_cli, read_file_honors_file_root) {
  char dir[] = "/tmp/enki-fileroot-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));

  char path[512];
  (void)snprintf(path, sizeof(path), "%s/snap", dir);
  cr_assert_eq(mkdir(path, 0700), 0);
  (void)snprintf(path, sizeof(path), "%s/files", dir);
  cr_assert_eq(mkdir(path, 0700), 0);

  (void)snprintf(path, sizeof(path), "%s/files/allowed.txt", dir);
  FILE* f = fopen(path, "w");
  cr_assert_not_null(f);
  fputs("inside-ok", f);
  fclose(f);

  (void)snprintf(path, sizeof(path), "%s/secret.txt", dir);
  f = fopen(path, "w");
  cr_assert_not_null(f);
  fputs("outside-secret", f);
  fclose(f);

  (void)snprintf(path, sizeof(path), "%s/snap/root.plan", dir);
  f = fopen(path, "w");
  cr_assert_not_null(f);
  fprintf(f, "(#bind Output\n"
             "  (#pin (#law \"Output\" (Output x) ((#pin \"R\") (\"Output\" "
             "x)))))\n"
             "(#bind ReadFile\n"
             "  (#pin (#law \"ReadFile\" (ReadFile x) ((#pin \"R\") "
             "(\"ReadFile\" x)))))\n"
             "(#bind inside\n"
             "  (#pin (#law \"inside\" (inside args) ((#pin \"R\") (\"Output\" "
             "((#pin \"R\") (\"ReadFile\" \"allowed.txt\")))))))\n"
             "(#bind escape\n"
             "  (#pin (#law \"escape\" (escape args) ((#pin \"R\") (\"Output\" "
             "((#pin \"R\") (\"ReadFile\" \"../secret.txt\")))))))\n");
  fclose(f);

  char cmd[1024];
  char out[256];

  (void)snprintf(cmd, sizeof(cmd),
                 "cd %s && %s --file-root files snap root inside 2>/dev/null",
                 dir, wisp_bin());
  int st = run_cmd(cmd, out, sizeof(out));
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) == 0, "inside run failed");
  cr_assert_not_null(strstr(out, "inside-ok"), "got `%s`", out);

  (void)snprintf(cmd, sizeof(cmd),
                 "cd %s && %s --file-root files snap root escape 2>/dev/null",
                 dir, wisp_bin());
  st = run_cmd(cmd, out, sizeof(out));
  cr_assert(WIFEXITED(st) && WEXITSTATUS(st) == 0, "escape run failed");
  cr_assert_null(strstr(out, "outside-secret"), "escaped file root: `%s`", out);
}
