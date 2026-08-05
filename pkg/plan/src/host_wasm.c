#include "plan/host_wasm.h"

#include "axsys/util.h"
#include "internal.h"

__attribute__((import_module("enki"),
               import_name("wormhole_retain"))) extern void
pl_js_wormhole_retain(uint64_t token);
__attribute__((import_module("enki"),
               import_name("wormhole_release"))) extern void
pl_js_wormhole_release(uint64_t token);

static void pl_wasm_retain(void* ctx, uint64_t token) {
  AX_UNUSED(ctx);
  pl_js_wormhole_retain(token);
}

static void pl_wasm_release(void* ctx, uint64_t token) {
  AX_UNUSED(ctx);
  pl_js_wormhole_release(token);
}

static pl_val pl_wasm_effect(void* ctx, void* scope, pl_thread* t,
                             pl_host_op op, size_t ab) {
  AX_UNUSED(ctx);
  AX_UNUSED(scope);
  switch (op) {
  case PL_HOST_OP_INPUT:
    return pl_op82_input(t, ab);
  case PL_HOST_OP_OUTPUT:
    return pl_op82_output(t, ab);
  case PL_HOST_OP_WARN:
    return pl_op82_warn(t, ab);
  case PL_HOST_OP_READ_FILE:
    return pl_op82_read_file(t, ab);
  case PL_HOST_OP_WRITE_FILE:
    return pl_op82_write_file(t, ab);
  case PL_HOST_OP_PRINT:
    return pl_op82_print(t, ab);
  case PL_HOST_OP_STAMP:
    return pl_op82_stamp(t, ab);
  case PL_HOST_OP_NOW:
    return pl_op82_now(t, ab);
  case PL_HOST_OP_CLOSE_FD:
    return pl_op82_closefd(t, ab);
  case PL_HOST_OP_LISTEN:
    return pl_op82_listen(t, ab);
  case PL_HOST_OP_ACCEPT:
    return pl_op82_accept(t, ab);
  case PL_HOST_OP_READ:
    return pl_op82_read(t, ab);
  case PL_HOST_OP_WRITE:
    return pl_op82_write(t, ab);
  case PL_HOST_OP_CONNECT:
    return pl_op82_connect(t, ab);
  case PL_HOST_OP_NONE:
  default:
    pl_raise_msg(t, "unknown WASM host effect");
  }
}

void pl_wasm_host_install(const pl_wasm_io* io) {
  static pl_host host;
  if (host.effect == NULL) {
    host = (pl_host){
        .ctx = (void*)io,
        .effect = pl_wasm_effect,
        .retain = pl_wasm_retain,
        .release = pl_wasm_release,
    };
  }
  const pl_host* installed = pl_host_get();
  if (installed == NULL) {
    pl_host_install(&host);
    return;
  }
  ax_assume(installed->ctx == host.ctx && installed->effect == host.effect &&
                installed->retain == host.retain &&
                installed->release == host.release,
            "WASM host conflicts with installed process host");
}
