#include "plan/host.h"

#include "axsys/assume.h"
#include "host_internal.h"
#include "plan/eval.h"
#include "plan/heap.h"

static pl_host pl_process_host;
static bool pl_process_host_f;

void pl_host_install(const pl_host* host) {
  ax_assume(host != NULL, "pl_host_install: host required");
  ax_assume(host->effect != NULL, "pl_host_install: effect callback required");
  ax_assume(host->retain != NULL, "pl_host_install: retain callback required");
  ax_assume(host->release != NULL,
            "pl_host_install: release callback required");
  ax_assume(!pl_process_host_f, "pl_host_install: host already installed");
  pl_process_host = *host;
  pl_process_host_f = true;
}

bool pl_host_is_installed(void) {
  return pl_process_host_f;
}

const pl_host* pl_host_get(void) {
  return pl_process_host_f ? &pl_process_host : NULL;
}

void* pl_host_process_context(void) {
  ax_assume(pl_process_host_f, "host not installed");
  return pl_process_host.ctx;
}

pl_val pl_host_effect_run(pl_thread* t, pl_host_op op, size_t argbase) {
  if (!pl_process_host_f)
    pl_raise_msg(t, "host not installed");
  return pl_process_host.effect(pl_process_host.ctx, t->host_scope, t, op,
                                argbase);
}

void pl_host_token_retain(uint64_t token) {
  ax_assume(pl_process_host_f, "wormhole host not installed");
  pl_process_host.retain(pl_process_host.ctx, token);
}

void pl_host_token_release(uint64_t token) {
  ax_assume(pl_process_host_f, "wormhole host not installed");
  pl_process_host.release(pl_process_host.ctx, token);
}

void pl_thread_set_host_scope(pl_thread* t, void* scope) {
  t->host_scope = scope;
}

void* pl_thread_host_scope(const pl_thread* t) {
  return t->host_scope;
}

void pl_thread_set_effect_interceptor(pl_thread* t,
                                      pl_effect_interceptor interceptor,
                                      void* ctx) {
  t->effect_interceptor = interceptor;
  t->effect_interceptor_ctx = ctx;
}
