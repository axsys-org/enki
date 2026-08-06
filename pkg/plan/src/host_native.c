#include "plan/host_native.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "axsys/assume.h"
#include "internal.h"

typedef enum {
  PL_NATIVE_BUFFER = 1,
  PL_NATIVE_FD,
} pl_native_kind;

typedef struct pl_native_resource {
  uint64_t token;
  size_t refs;
  pl_native_kind kind;
  union {
    struct {
      uint8_t* data;
      size_t size;
    } buffer;
    int fd;
  };
  struct pl_native_resource* next;
} pl_native_resource;

static pthread_mutex_t pl_native_mu = PTHREAD_MUTEX_INITIALIZER;
static pl_native_resource* pl_native_resources;
static uint64_t pl_native_next_token = 1;

static pl_native_resource* pl_native_find(uint64_t token) {
  for (pl_native_resource* r = pl_native_resources; r != NULL; r = r->next)
    if (r->token == token)
      return r;
  return NULL;
}

static uint64_t pl_native_register(pl_native_resource* r) {
  ax_assume(pthread_mutex_lock(&pl_native_mu) == 0, "pthread_mutex_lock");
  ax_assume(pl_native_next_token != 0, "native host token space exhausted");
  r->token = pl_native_next_token++;
  r->refs = 1;
  r->next = pl_native_resources;
  pl_native_resources = r;
  ax_assume(pthread_mutex_unlock(&pl_native_mu) == 0, "pthread_mutex_unlock");
  return r->token;
}

static void pl_native_retain(void* ctx, uint64_t token) {
  AX_UNUSED(ctx);
  ax_assume(pthread_mutex_lock(&pl_native_mu) == 0, "pthread_mutex_lock");
  pl_native_resource* r = pl_native_find(token);
  ax_assume(r != NULL && r->refs < SIZE_MAX,
            "native host retain of unknown token");
  r->refs++;
  ax_assume(pthread_mutex_unlock(&pl_native_mu) == 0, "pthread_mutex_unlock");
}

static void pl_native_release(void* ctx, uint64_t token) {
  AX_UNUSED(ctx);
  ax_assume(pthread_mutex_lock(&pl_native_mu) == 0, "pthread_mutex_lock");
  pl_native_resource** link = &pl_native_resources;
  while (*link != NULL && (*link)->token != token)
    link = &(*link)->next;
  pl_native_resource* r = *link;
  ax_assume(r != NULL && r->refs > 0, "native host release of unknown token");
  r->refs--;
  bool destroy = r->refs == 0;
  if (destroy)
    *link = r->next;
  ax_assume(pthread_mutex_unlock(&pl_native_mu) == 0, "pthread_mutex_unlock");
  if (!destroy)
    return;
  if (r->kind == PL_NATIVE_BUFFER)
    free(r->buffer.data);
  else if (r->kind == PL_NATIVE_FD)
    ax_assume(close(r->fd) == 0, "native host close");
  free(r);
}

static pl_val pl_native_effect(void* ctx, void* scope, pl_thread* t,
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
    pl_raise_msg(t, "JPLAN Eval is unavailable on the native host");
  case PL_HOST_OP_NONE:
  default:
    pl_raise_msg(t, "unknown native host effect");
  }
}

static const pl_host pl_native_host = {
    .effect = pl_native_effect,
    .retain = pl_native_retain,
    .release = pl_native_release,
};

void pl_native_host_install(void) {
  const pl_host* installed = pl_host_get();
  if (installed == NULL) {
    pl_host_install(&pl_native_host);
    return;
  }
  ax_assume(installed->ctx == pl_native_host.ctx &&
                installed->effect == pl_native_host.effect &&
                installed->retain == pl_native_host.retain &&
                installed->release == pl_native_host.release,
            "native host conflicts with installed process host");
}

void pl_native_host_set_file_root(pl_thread* t, const char* file_root) {
  pl_thread_set_host_scope(t, (void*)file_root);
}

const char* pl_native_host_file_root(const pl_thread* t) {
  return (const char*)pl_thread_host_scope(t);
}

pl_val pl_native_buffer_new(pl_thread* t, size_t size) {
  pl_native_host_install();
  pl_native_resource* r = calloc(1, sizeof(*r));
  ax_assume(r != NULL, "oom");
  r->kind = PL_NATIVE_BUFFER;
  r->buffer.data = calloc(size == 0 ? 1 : size, 1);
  ax_assume(r->buffer.data != NULL, "oom");
  r->buffer.size = size;
  return pl_wormhole_adopt(t, pl_native_register(r));
}

uint8_t* pl_native_buffer_data(pl_val wormhole, size_t* size) {
  uint64_t token = pl_wormhole_token(wormhole);
  ax_assume(pthread_mutex_lock(&pl_native_mu) == 0, "pthread_mutex_lock");
  pl_native_resource* r = pl_native_find(token);
  ax_assume(r != NULL && r->kind == PL_NATIVE_BUFFER,
            "native buffer access through wrong handle kind");
  uint8_t* data = r->buffer.data;
  if (size != NULL)
    *size = r->buffer.size;
  ax_assume(pthread_mutex_unlock(&pl_native_mu) == 0, "pthread_mutex_unlock");
  return data;
}

pl_val pl_native_fd_adopt(pl_thread* t, int fd) {
  ax_assume(fd >= 0, "pl_native_fd_adopt: invalid fd");
  pl_native_host_install();
  pl_native_resource* r = calloc(1, sizeof(*r));
  ax_assume(r != NULL, "oom");
  r->kind = PL_NATIVE_FD;
  r->fd = fd;
  return pl_wormhole_adopt(t, pl_native_register(r));
}

int pl_native_fd_get(pl_val wormhole) {
  uint64_t token = pl_wormhole_token(wormhole);
  ax_assume(pthread_mutex_lock(&pl_native_mu) == 0, "pthread_mutex_lock");
  pl_native_resource* r = pl_native_find(token);
  ax_assume(r != NULL && r->kind == PL_NATIVE_FD,
            "native fd access through wrong handle kind");
  int fd = r->fd;
  ax_assume(pthread_mutex_unlock(&pl_native_mu) == 0, "pthread_mutex_unlock");
  return fd;
}
