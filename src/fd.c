#include <stdint.h>
#include <unistd.h>

#include "enki/fd.h"

enki_fd_table enki_global_fd_table = {
  .next = 3,
  .slots = {
    [0] = {.live = true, .fd = 0, .refs = 1},
    [1] = {.live = true, .fd = 1, .refs = 1},
    [2] = {.live = true, .fd = 2, .refs = 1},
  },
};

size_t enki_fd_add(int fd) {
    if(enki_global_fd_table.next < FD_MAX) {
        size_t hdl = enki_global_fd_table.next++;
        enki_global_fd_table.slots[hdl] =
            (enki_fd_slot){.live = true, .fd = fd, .refs = 1};
        return hdl;
    }
    for(size_t k = 0; k < FD_MAX; k++) {
        enki_fd_slot* slot = &enki_global_fd_table.slots[k];
        if(!slot->live) {
            slot->fd = fd;
            slot->refs = 1;
            slot->live = true;
            return k;
        }
    }
    return ENKI_FD_INVALID;
}

int enki_fd_get(size_t hdl) {
    if(hdl >= FD_MAX) return -1;
    enki_fd_slot* slot = &enki_global_fd_table.slots[hdl];
    if(!slot->live) return -1;
    return slot->fd;
}

int enki_fd_close(size_t hdl) {
    if(hdl >= FD_MAX) return -1;
    enki_fd_slot* slot = &enki_global_fd_table.slots[hdl];
    if(!slot->live) return -1;
    int rc = close(slot->fd);
    slot->fd = -1;
    slot->refs = 0;
    slot->live = false;
    return rc;
}

int enki_fd_retain(size_t hdl) {
    if(hdl >= FD_MAX) return -1;
    enki_fd_slot* slot = &enki_global_fd_table.slots[hdl];
    if(!slot->live) return -1;
    if(slot->refs == SIZE_MAX) return -1;
    slot->refs += 1;
    return 0;
}

int enki_fd_release(size_t hdl) {
    if(hdl >= FD_MAX) return -1;
    enki_fd_slot* slot = &enki_global_fd_table.slots[hdl];
    if(!slot->live) return -1;
    if(slot->refs == 0) return -1;
    slot->refs -= 1;
    if(slot->refs == 0) {
        return enki_fd_close(hdl);
    }
    return 0;
}
