#ifndef PL_HOST_NATIVE_H
#define PL_HOST_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#include "plan/host.h"
#include "plan/wormhole.h"

void pl_native_host_install(void);
void pl_native_host_set_file_root(pl_thread* t, const char* file_root);
const char* pl_native_host_file_root(const pl_thread* t);

pl_val pl_native_buffer_new(pl_thread* t, size_t size);
uint8_t* pl_native_buffer_data(pl_val wormhole, size_t* size);

/* The registry owns fd after adoption and closes it after the last wrapper. */
pl_val pl_native_fd_adopt(pl_thread* t, int fd);
int pl_native_fd_get(pl_val wormhole);

#endif
