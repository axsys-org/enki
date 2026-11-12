#include "internal.h"
#include "plan/mesa.h"
#include "plan/nat.h"
#include "plan/rplan.h"
#include "plan/store.h"
#include <stdlib.h>
#include <string.h>
#ifdef PL_MESA
#include "../../mesa/include/bridge.h"
#endif
#define MESA_LIMIT ((size_t)16 << 20)
static const char mesa_type;
bool pl_is_mesa_handle(pl_val v) {
  return pl_wormhole_data(v, &mesa_type) != NULL &&
         pl_wormhole_size(v) == sizeof(void*);
}
typedef struct {
  uint8_t* data;
  size_t n, cap;
} mesa_bytes;
static bool put(mesa_bytes* b, const void* src, size_t n) {
  if (n > MESA_LIMIT - b->n)
    return false;
  if (b->n + n > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + n)
      cap *= 2;
    uint8_t* p = realloc(b->data, cap);
    ax_assume(p != NULL, "oom");
    b->data = p;
    b->cap = cap;
  }
  if (n)
    memcpy(b->data + b->n, src, n);
  b->n += n;
  return true;
}
static bool header(mesa_bytes* b, uint8_t tag, size_t n) {
  uint8_t h[5] = {tag, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16),
                  (uint8_t)(n >> 24)};
  return n <= UINT32_MAX && put(b, h, sizeof(h));
}
static bool encode(mesa_bytes* b, pl_val v, unsigned depth) {
  if (depth > 64)
    return false;
  v = pl_resolve(v);
  if (pl_is_nat(v)) {
    size_t n = pl_nat_byte_len(v);
    if (n > MESA_LIMIT || !header(b, 0, n))
      return false;
    for (size_t i = 0; i < n; i++) {
      uint8_t c = pl_nat_byte_at(v, i);
      if (!put(b, &c, 1))
        return false;
    }
    return true;
  }
  if (pl_tag(v) != PL_TAG_APP)
    return false;
  pl_cell* p = pl_ptr(v);
  if (pl_app_head(p) != 0)
    return false;
  uint32_t n = pl_app_n(p);
  if (!header(b, 1, n))
    return false;
  for (uint32_t i = 0; i < n; i++)
    if (!encode(b, pl_app_args(p)[i], depth + 1))
      return false;
  return true;
}
static pl_val row(pl_thread* t, size_t base) {
  size_t n = t->vsp - base;
  if (!n)
    return 0;
  pl_gc_reserve(t, PL_APP_CELLS(n));
  pl_val v = pl_mk_app_from(t, 0, (uint32_t)n, t->vstack + base);
  t->vsp = base;
  return v;
}
static pl_val decode(pl_thread* t, const uint8_t** cursor, const uint8_t* end,
                     unsigned depth) {
  const uint8_t* b = *cursor;
  ax_assume(depth <= 64 && end - b >= 5, "bad Mesa reply");
  uint8_t tag = b[0];
  uint32_t n = (uint32_t)b[1] | ((uint32_t)b[2] << 8) | ((uint32_t)b[3] << 16) |
               ((uint32_t)b[4] << 24);
  *cursor += 5;
  if (tag == 0) {
    ax_assume((size_t)(end - *cursor) >= n, "bad Mesa nat");
    pl_val v = pl_nat_from_bytes(t, *cursor, n);
    *cursor += n;
    return v;
  }
  ax_assume(tag == 1, "bad Mesa row");
  size_t base = t->vsp;
  for (uint32_t i = 0; i < n; i++)
    pl_vpush(t, decode(t, cursor, end, depth + 1));
  return row(t, base);
}
static pl_val failure(pl_thread* t, const char* message) {
  size_t base = t->vsp, n = strlen(message);
  uint8_t* bytes = malloc(n + 1);
  ax_assume(bytes != NULL, "oom");
  memcpy(bytes, message, n);
  bytes[n] = 1;
  pl_vpush(t, 1);
  pl_vpush(t, pl_nat_from_bytes(t, bytes, n + 1));
  free(bytes);
  pl_vpush(t, 0);
  return row(t, base);
}
/* Store operations receive the same canonical path policy as file syscalls. */
static bool encode_data(pl_thread* t, const char* op, pl_val data,
                        mesa_bytes* b) {
  if (strcmp(op, "MesaStoreCreate") && strcmp(op, "MesaStoreOpen") &&
      strcmp(op, "MesaStoreInspect"))
    return encode(b, data, 0);
  if (pl_tag(data) != PL_TAG_APP || pl_app_head(pl_ptr(data)) != 0 ||
      pl_app_n(pl_ptr(data)) != 2)
    return false;
  pl_val path = pl_resolve(pl_app_args(pl_ptr(data))[0]);
  if (!pl_is_nat(path))
    return false;
  size_t n = pl_nat_byte_len(path);
  if (n == 0 || n > 4097 || pl_nat_byte_at(path, n - 1) != 1)
    return false;
  char* in = malloc(n);
  ax_assume(in != NULL, "oom");
  for (size_t i = 0; i + 1 < n; i++) {
    in[i] = (char)pl_nat_byte_at(path, i);
    if (!in[i]) {
      free(in);
      return false;
    }
  }
  in[n - 1] = 0;
  char* out = NULL;
  bool ok =
      pl_rplan_resolve_path(t, in, strcmp(op, "MesaStoreInspect") != 0, &out);
  free(in);
  if (!ok)
    return false;
  n = strlen(out);
  uint8_t sentinel = 1;
  ok = header(b, 1, 2) && header(b, 0, n + 1) && put(b, out, n) &&
       put(b, &sentinel, 1) && encode(b, pl_app_args(pl_ptr(data))[1], 1);
  free(out);
  return ok;
}
pl_val pl_mesa_call(pl_thread* t, const char* name, pl_val handles,
                    pl_val data) {
#ifndef PL_MESA
  (void)name;
  (void)handles;
  (void)data;
  return failure(t, "Mesa support was disabled at build time");
#else
  if (t->rplan_no_resources)
    return failure(
        t, "Mesa native resources are unavailable during record/replay");
  pl_cell* p = handles == 0 ? NULL : pl_as(PL_TAG_APP, handles);
  if (handles != 0 && (!p || pl_app_head(p) != 0))
    return failure(t, "Mesa handles must be a row");
  size_t n = p ? pl_app_n(p) : 0;
  if (n > 64)
    return failure(t, "Mesa handle limit");
  em_handle* native[64];
  for (size_t i = 0; i < n; i++) {
    pl_val v = pl_resolve(pl_app_args(p)[i]);
    if (!pl_is_mesa_handle(v))
      return failure(t, "expected Mesa wormhole");
    memcpy(&native[i], pl_wormhole_data(v, &mesa_type), sizeof(native[i]));
  }
  mesa_bytes bytes = {0};
  if (!encode_data(t, name, data, &bytes)) {
    free(bytes.data);
    return failure(t, "invalid Mesa data, path, or size limit");
  }
  /* The Rust call never enters PLAN; all input roots are retained below. */
  size_t base = t->vsp;
  pl_vpush(t, handles);
  pl_vpush(t, data);
  em_reply* reply = em_call(name, native, n, bytes.data, bytes.n);
  free(bytes.data);
  for (size_t i = 0; i < n; i++) {
    pl_val v = pl_resolve(pl_app_args(pl_ptr(t->vstack[base]))[i]);
    pl_store_gc_charge(pl_heap_store(t->heap), v, em_handle_charge(native[i]));
  }
  size_t result = t->vsp;
  pl_vpush(t, em_reply_status(reply));
  size_t len;
  const uint8_t* begin = em_reply_bytes(reply, &len);
  const uint8_t* at = begin;
  pl_vpush(t, decode(t, &at, begin + len, 0));
  ax_assume(at == begin + len, "trailing Mesa reply");
  size_t hb = t->vsp, count = em_reply_count(reply);
  for (size_t i = 0; i < count; i++) {
    em_handle* h = em_reply_take(reply, i);
    pl_val v = pl_mk_resource(t, &mesa_type, &h, sizeof(h), em_handle_free, h);
    pl_vpush(t, v);
    pl_store_gc_charge(pl_heap_store(t->heap), v, em_handle_charge(h));
  }
  em_reply_free(reply);
  pl_vpush(t, row(t, hb));
  pl_val out = row(t, result);
  t->vsp = base;
  return out;
#endif
}
pl_val pl_op83_mesa(pl_thread* t, size_t ab) {
  pl_val name = t->vstack[ab - 1];
  if (pl_is_nat63(name) && name < pl_nops && pl_ops[name].body == pl_op83_mesa)
    return pl_mesa_call(t, pl_ops[name].name_c, t->vstack[ab],
                        t->vstack[ab + 1]);
  size_t n = pl_nat_byte_len(name);
  ax_assume(n < 80, "Mesa name too long");
  char op[80];
  for (size_t i = 0; i < n; i++)
    op[i] = (char)pl_nat_byte_at(name, i);
  op[n] = 0;
  return pl_mesa_call(t, op, t->vstack[ab], t->vstack[ab + 1]);
}
