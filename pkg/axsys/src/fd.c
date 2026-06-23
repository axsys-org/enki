#include "axsys/fd.h"

#include <stdint.h>
#include <unistd.h>
#include <errno.h>

ax_fd_table ax_global_fd_table = {
    .next = 3,
    .slots =
        {
            [0] = {.live = true, .fd = 0, .refs = 1},
            [1] = {.live = true, .fd = 1, .refs = 1},
            [2] = {.live = true, .fd = 2, .refs = 1},
        },
};

size_t ax_fd_add(int fd) {
  if (ax_global_fd_table.next < AX_FD_MAX) {
    size_t hdl = ax_global_fd_table.next++;
    ax_global_fd_table.slots[hdl] =
        (ax_fd_slot){.live = true, .fd = fd, .refs = 1};
    return hdl;
  }
  for (size_t k = 0; k < AX_FD_MAX; k++) {
    ax_fd_slot* slot = &ax_global_fd_table.slots[k];
    if (!slot->live) {
      slot->fd = fd;
      slot->refs = 1;
      slot->live = true;
      return k;
    }
  }
  return AX_FD_INVALID;
}

int ax_fd_get(size_t hdl) {
  if (hdl >= AX_FD_MAX)
    return -1;
  ax_fd_slot* slot = &ax_global_fd_table.slots[hdl];
  return slot->live ? slot->fd : -1;
}

int ax_fd_close(size_t hdl) {
  if (hdl >= AX_FD_MAX) {
    errno = EMFILE;
    return -1;
  }
  ax_fd_slot* slot = &ax_global_fd_table.slots[hdl];
  if (!slot->live) {
    errno = EBADF;
    return -1;
  }
  int rc = close(slot->fd);
  slot->fd = -1;
  slot->refs = 0;
  slot->live = false;
  return rc;
}

int ax_fd_retain(size_t hdl) {
  if (hdl >= AX_FD_MAX)
    return -1;
  ax_fd_slot* slot = &ax_global_fd_table.slots[hdl];
  if (!slot->live || slot->refs == SIZE_MAX)
    return -1;
  slot->refs += 1;
  return 0;
}

int ax_fd_release(size_t hdl) {
  if (hdl >= AX_FD_MAX)
    return -1;
  ax_fd_slot* slot = &ax_global_fd_table.slots[hdl];
  if (!slot->live || slot->refs == 0)
    return -1;
  slot->refs -= 1;
  if (slot->refs == 0)
    return ax_fd_close(hdl);
  return 0;
}
