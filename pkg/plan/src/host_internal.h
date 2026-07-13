#ifndef PL_HOST_INTERNAL_H
#define PL_HOST_INTERNAL_H

#include "plan/host.h"

pl_val pl_host_effect_run(pl_thread* t, pl_host_op op, size_t argbase);
void pl_host_token_retain(uint64_t token);
void pl_host_token_release(uint64_t token);
void* pl_host_process_context(void);

#endif
