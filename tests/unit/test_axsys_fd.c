#include <criterion/criterion.h>

#include <stdint.h>
#include <unistd.h>

#include "axsys/fd.h"

Test(axsys_fd, close_defers_until_blocking_borrow_finishes) {
  int pipe_fd[2];
  cr_assert_eq(pipe(pipe_fd), 0);
  size_t handle = ax_fd_add(pipe_fd[0]);
  cr_assert_neq(handle, AX_FD_INVALID);

  int borrowed = -1;
  cr_assert_eq(ax_fd_acquire(handle, &borrowed), 0);
  cr_assert_eq(borrowed, pipe_fd[0]);
  cr_assert_eq(ax_fd_close(handle), 0);
  cr_assert_eq(ax_fd_get(handle), -1); /* new effects see a closed handle */

  uint8_t sent = 42, received = 0;
  cr_assert_eq(write(pipe_fd[1], &sent, 1), 1);
  cr_assert_eq(read(borrowed, &received, 1), 1);
  cr_assert_eq(received, sent); /* the in-flight effect retained the fd */
  cr_assert_eq(ax_fd_release(handle), 0);
  cr_assert_eq(close(pipe_fd[1]), 0);
}
