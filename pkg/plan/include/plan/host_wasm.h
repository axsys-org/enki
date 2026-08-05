#ifndef PL_HOST_WASM_H
#define PL_HOST_WASM_H

#include "plan/host.h"
#include "plan/wasm_io.h"

/* Install the browser/WASM adapter as the process-global PLAN host. */
void pl_wasm_host_install(const pl_wasm_io* io);

#endif
