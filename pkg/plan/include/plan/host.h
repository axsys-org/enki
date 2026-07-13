#ifndef PL_HOST_H
#define PL_HOST_H

/*
 * Process-global host boundary.  A program selects exactly one host before
 * performing direct effects or creating wormholes.  Individual PLAN threads
 * may carry an opaque host scope and an effect interceptor, but the host
 * implementation itself is immutable for the lifetime of the process.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plan/value.h"

typedef struct pl_thread pl_thread;

typedef enum pl_host_op {
  PL_HOST_OP_NONE = 0,
  PL_HOST_OP_INPUT = 1,
  PL_HOST_OP_OUTPUT = 2,
  PL_HOST_OP_WARN = 3,
  PL_HOST_OP_READ_FILE = 4,
  PL_HOST_OP_WRITE_FILE = 5,
  PL_HOST_OP_PRINT = 6,
  PL_HOST_OP_STAMP = 7,
  PL_HOST_OP_NOW = 8,
  PL_HOST_OP_CLOSE_FD = 9,
  PL_HOST_OP_LISTEN = 10,
  PL_HOST_OP_ACCEPT = 11,
  PL_HOST_OP_READ = 12,
  PL_HOST_OP_WRITE = 13,
  PL_HOST_OP_CONNECT = 14,
} pl_host_op;

typedef pl_val (*pl_host_effect_fn)(void* process_ctx, void* thread_scope,
                                    pl_thread* t, pl_host_op op,
                                    size_t argbase);
typedef void (*pl_host_token_fn)(void* process_ctx, uint64_t token);

typedef struct pl_host {
  void* ctx;
  pl_host_effect_fn effect;
  pl_host_token_fn retain;
  pl_host_token_fn release;
} pl_host;

/* Installation is one-shot and must precede every host-mediated operation. */
void pl_host_install(const pl_host* host);
bool pl_host_is_installed(void);
const pl_host* pl_host_get(void);

void pl_thread_set_host_scope(pl_thread* t, void* scope);
void* pl_thread_host_scope(const pl_thread* t);

/* The interceptor receives the op-table index (stable name/arity identity),
 * not pl_host_op.  Return true with *out filled to replace the effect. */
typedef bool (*pl_effect_interceptor)(void* ctx, pl_thread* t, uint32_t op,
                                      size_t argbase, pl_val* out);
void pl_thread_set_effect_interceptor(pl_thread* t,
                                      pl_effect_interceptor interceptor,
                                      void* ctx);

#endif
