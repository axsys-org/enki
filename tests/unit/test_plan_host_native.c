#include <criterion/criterion.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "plan/host_native.h"
#include "test_plan.h"

Test(host_native, mutable_buffer_survives_clone) {
  test_rt rt = test_rt_new();
  pl_vpush(rt.t, pl_native_buffer_new(rt.t, 4));
  size_t size = 0;
  uint8_t* data = pl_native_buffer_data(rt.t->vstack[0], &size);
  cr_assert_eq(size, 4);
  data[2] = 0xa5;

  pl_vpush(rt.t, pl_wormhole_clone(rt.t, rt.t->vstack[0]));
  pl_wormhole_close(rt.t->vstack[0]);
  data = pl_native_buffer_data(rt.t->vstack[1], &size);
  cr_assert_eq(data[2], 0xa5);
  pl_wormhole_close(rt.t->vstack[1]);
  test_rt_free(&rt);
}

Test(host_native, adopted_descriptor_closes_after_last_wrapper) {
  int fds[2];
  cr_assert_eq(pipe(fds), 0);
  test_rt rt = test_rt_new();
  pl_val fd = pl_native_fd_adopt(rt.t, fds[1]);
  cr_assert_eq(pl_native_fd_get(fd), fds[1]);
  pl_wormhole_close(fd);

  errno = 0;
  cr_assert_eq(fcntl(fds[1], F_GETFD), -1);
  cr_assert_eq(errno, EBADF);
  cr_assert_eq(close(fds[0]), 0);
  test_rt_free(&rt);
}

Test(host_native, jplan_eval_reports_unavailable) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, pl_native_buffer_new(t, 0));
  pl_val args[3] = {t->vstack[base], 0, 0};
  pl_vpush(t, test_app(t, ax_s4('E', 'v', 'a', 'l'), 3, args));
  pl_vpush(t, 74);
  pl_vpush(t, pl_pin(t, t->vstack[base + 2]));

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)pl_apply(t, t->vstack[base + 3], t->vstack[base + 1]);
    cr_assert_fail("expected native JPLAN error");
  }
  pl_catch_unwind(t, &c);
  cr_assert_not_null(t->exn_msg);
  cr_assert_str_eq(t->exn_msg, "JPLAN Eval is unavailable on the native host");
  t->vsp = base + 1;
  test_rt_free(&rt);
}
