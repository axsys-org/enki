#include <criterion/criterion.h>

#include <stdlib.h>
#include <sys/wait.h>

#ifndef ENKI_WISP_BIN
#define ENKI_WISP_BIN "./build/debug/bin/wisp"
#endif

#define WISP_REAVER_COMMAND ENKI_WISP_BIN " ./reaver/src/plan reaver"

Test(wisp_cli, reaver_module_exits_zero) {
  int status = system(WISP_REAVER_COMMAND);

  cr_assert_neq(status, -1, "failed to run `%s`", WISP_REAVER_COMMAND);
  cr_assert(WIFEXITED(status), "`%s` did not exit normally: status=%d",
            WISP_REAVER_COMMAND, status);
  cr_assert_eq(WEXITSTATUS(status), 0, "`%s` exited with %d",
               WISP_REAVER_COMMAND, WEXITSTATUS(status));
}
