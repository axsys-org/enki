#include "internal.h"

#include <stdlib.h>
#include <string.h>

#include "axsys/util.h"
#include "host_internal.h"
#include "plan/build.h"
#include "plan/host.h"
#include "plan/nat.h"
#include "plan/wasm_io.h"

#define ARG(i) (t->vstack[ab + (i)])

static const pl_wasm_io* rp_need_io(pl_thread* t) {
  if (!pl_host_is_installed())
    pl_raise_msg(t, "op82 browser I/O is not configured");
  return pl_host_process_context();
}

static pl_val rp_want_nat(pl_thread* t, pl_val v) {
  if (!pl_is_nat(v))
    pl_raise_msg(t, "op82 expected nat");
  return v;
}

static uint8_t* rp_nat_bytes(pl_val v, bool drop_top, size_t* out_n) {
  size_t n = pl_nat_byte_len(v);
  if (drop_top && n > 0)
    n -= 1;
  uint8_t* b = malloc(n ? n : 1);
  ax_assume(b != NULL, "oom");
  for (size_t i = 0; i < n; i++)
    b[i] = pl_nat_byte_at(v, i);
  *out_n = n;
  return b;
}

static char* rp_nat_path(pl_val v) {
  size_t n = pl_nat_byte_len(v);
  char* p = malloc(n + 1);
  ax_assume(p != NULL, "oom");
  for (size_t i = 0; i < n; i++)
    p[i] = (char)pl_nat_byte_at(v, i);
  p[n] = '\0';
  return p;
}

static pl_val rp_bar(pl_thread* t, const uint8_t* b, size_t n) {
  uint8_t* bar = malloc(n + 1);
  ax_assume(bar != NULL, "oom");
  memcpy(bar, b, n);
  bar[n] = 0x01;
  pl_val out = pl_nat_from_bytes(t, bar, n + 1);
  free(bar);
  return out;
}

pl_val pl_rplan_read_folder(pl_thread* t, pl_val path) {
  rp_want_nat(t, path);
  return 0;
}

pl_val pl_op82_input(pl_thread* t, size_t ab) {
  const pl_wasm_io* io = rp_need_io(t);
  uint64_t n64 = pl_nat_u64_clamp(rp_want_nat(t, ARG(0)));
  if (n64 > (1u << 26))
    n64 = 1u << 26;
  size_t n = (size_t)n64;
  uint8_t* buf = malloc(n ? n : 1);
  ax_assume(buf != NULL, "oom");
  size_t got = io->input != NULL ? io->input(io->ctx, buf, n) : 0;
  if (got > n)
    got = n;
  pl_val out = rp_bar(t, buf, got);
  free(buf);
  return out;
}

static pl_val rp_output(pl_thread* t, size_t ab, pl_wasm_io_channel channel,
                        bool drop_top) {
  const pl_wasm_io* io = rp_need_io(t);
  size_t n;
  uint8_t* b = rp_nat_bytes(rp_want_nat(t, ARG(0)), drop_top, &n);
  if (io->output != NULL)
    io->output(io->ctx, channel, b, n);
  free(b);
  return 0;
}

pl_val pl_op82_output(pl_thread* t, size_t ab) {
  return rp_output(t, ab, PL_WASM_IO_STDOUT, true);
}

pl_val pl_op82_warn(pl_thread* t, size_t ab) {
  return rp_output(t, ab, PL_WASM_IO_STDERR, true);
}

pl_val pl_op82_print(pl_thread* t, size_t ab) {
  return rp_output(t, ab, PL_WASM_IO_STDOUT, false);
}

pl_val pl_op82_read_file(pl_thread* t, size_t ab) {
  const pl_wasm_io* io = rp_need_io(t);
  if (io->read_file == NULL)
    return 0;
  char* path = rp_nat_path(rp_want_nat(t, ARG(0)));
  uint8_t* bytes = NULL;
  size_t len = 0;
  bool ok = io->read_file(io->ctx, pl_thread_host_scope(t), path, &bytes, &len);
  free(path);
  if (!ok)
    return 0;
  pl_val out = rp_bar(t, bytes, len);
  free(bytes);
  return out;
}

pl_val pl_op82_write_file(pl_thread* t, size_t ab) {
  const pl_wasm_io* io = rp_need_io(t);
  if (io->write_file == NULL)
    return 0;
  char* path = rp_nat_path(rp_want_nat(t, ARG(0)));
  size_t n;
  uint8_t* b = rp_nat_bytes(rp_want_nat(t, ARG(1)), true, &n);
  bool ok = io->write_file(io->ctx, pl_thread_host_scope(t), path, b, n);
  free(b);
  free(path);
  return ok ? 1 : 0;
}

pl_val pl_op82_stamp(pl_thread* t, size_t ab) {
  const pl_wasm_io* io = rp_need_io(t);
  if (io->stamp == NULL)
    return 0;
  char* path = rp_nat_path(rp_want_nat(t, ARG(0)));
  uint64_t mtime = 0;
  bool ok = io->stamp(io->ctx, pl_thread_host_scope(t), path, &mtime);
  free(path);
  return ok ? (pl_val)mtime : 0;
}

pl_val pl_op82_now(pl_thread* t, size_t ab) {
  AX_UNUSED(ab);
  const pl_wasm_io* io = rp_need_io(t);
  return io->now != NULL ? (pl_val)io->now(io->ctx) : 0;
}

static pl_val rp_unavailable(pl_thread* t, size_t ab) {
  AX_UNUSED(ab);
  pl_raise_msg(t, "op82 descriptor/network I/O is unavailable in browsers");
}

pl_val pl_op82_closefd(pl_thread* t, size_t ab) {
  return rp_unavailable(t, ab);
}
pl_val pl_op82_listen(pl_thread* t, size_t ab) {
  return rp_unavailable(t, ab);
}
pl_val pl_op82_accept(pl_thread* t, size_t ab) {
  return rp_unavailable(t, ab);
}
pl_val pl_op82_connect(pl_thread* t, size_t ab) {
  return rp_unavailable(t, ab);
}
pl_val pl_op82_read(pl_thread* t, size_t ab) {
  return rp_unavailable(t, ab);
}
pl_val pl_op82_write(pl_thread* t, size_t ab) {
  return rp_unavailable(t, ab);
}
