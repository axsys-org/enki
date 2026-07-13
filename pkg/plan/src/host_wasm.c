#include "plan/host_wasm.h"

#include <stdlib.h>

#include "axsys/assume.h"
#include "axsys/util.h"
#include "internal.h"
#include "plan/nat.h"
#include "plan/wormhole.h"

__attribute__((import_module("enki"),
               import_name("wormhole_retain"))) extern void
pl_js_wormhole_retain(uint64_t token);
__attribute__((import_module("enki"),
               import_name("wormhole_release"))) extern void
pl_js_wormhole_release(uint64_t token);
__attribute__((import_module("enki"),
               import_name("jplan_eval"))) extern uint64_t
pl_js_jplan_eval(uint64_t environment_token, const uint64_t* object_tokens,
                 size_t object_count, const uint8_t* source, size_t source_len);

static void pl_wasm_retain(void* ctx, uint64_t token) {
  AX_UNUSED(ctx);
  pl_js_wormhole_retain(token);
}

static void pl_wasm_release(void* ctx, uint64_t token) {
  AX_UNUSED(ctx);
  pl_js_wormhole_release(token);
}

static uint64_t pl_wasm_open_token(pl_thread* t, pl_val v,
                                   const char* expected) {
  if (!pl_is_wormhole(v))
    pl_raise_msg(t, expected);
  if (pl_wormhole_is_closed(v))
    pl_raise_msg(t, "JPLAN Eval received a closed wormhole");
  return pl_wormhole_token(v);
}

static pl_val pl_wasm_jplan_eval(pl_thread* t, size_t ab) {
  pl_val environment = t->vstack[ab];
  pl_val objects = t->vstack[ab + 1];
  pl_val source = t->vstack[ab + 2];
  uint64_t environment_token = pl_wasm_open_token(
      t, environment, "JPLAN Eval expected an environment wormhole");

  uint32_t object_count = 0;
  pl_val* object_values = NULL;
  if (objects != 0) {
    pl_cell* row = pl_as(PL_TAG_APP, objects);
    if (row == NULL || pl_app_head(row) != 0)
      pl_raise_msg(t, "JPLAN Eval expected an object row");
    object_count = pl_app_n(row);
    object_values = pl_app_args(row);
  }

  uint64_t* object_tokens = NULL;
  if (object_count != 0) {
    object_tokens = malloc((size_t)object_count * sizeof(*object_tokens));
    ax_assume(object_tokens != NULL, "oom");
    for (uint32_t i = 0; i < object_count; i++)
      object_tokens[i] = pl_wasm_open_token(
          t, object_values[i], "JPLAN Eval object row contains a non-wormhole");
  }

  if (!pl_is_nat(source)) {
    free(object_tokens);
    pl_raise_msg(t, "JPLAN Eval expected source-code bytes");
  }
  size_t source_len = pl_nat_byte_len(source);
  uint8_t* source_bytes = NULL;
  if (source_len != 0) {
    source_bytes = malloc(source_len);
    ax_assume(source_bytes != NULL, "oom");
    for (size_t i = 0; i < source_len; i++)
      source_bytes[i] = pl_nat_byte_at(source, i);
  }

  uint64_t result_token = pl_js_jplan_eval(
      environment_token, object_tokens, object_count, source_bytes, source_len);
  free(source_bytes);
  free(object_tokens);
  if (result_token == 0)
    pl_raise_msg(t, "JPLAN Eval returned an invalid wormhole token");
  return pl_wormhole_adopt(t, result_token);
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
  case PL_HOST_OP_JPLAN_EVAL:
    return pl_wasm_jplan_eval(t, ab);
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
