#include <criterion/criterion.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

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

Test(wisp_cli, reaver_module_exits_zero) {
  char command_c[4096];
  int command_s = snprintf(command_c, sizeof(command_c), "%s %s reaver",
                           ENKI_WISP_BIN, reaver_plan_dir());

  cr_assert(command_s >= 0 && (size_t)command_s < sizeof(command_c),
            "failed to build wisp command");

  int status = system(command_c);

  cr_assert_neq(status, -1, "failed to run `%s`", command_c);
  cr_assert(WIFEXITED(status), "`%s` did not exit normally: status=%d",
            command_c, status);
  cr_assert_eq(WEXITSTATUS(status), 0, "`%s` exited with %d", command_c,
               WEXITSTATUS(status));
}
