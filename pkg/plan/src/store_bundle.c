#include "store_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "axsys/ds.h"
#include "axsys/sha256.h"
#include "plan/build.h"
#include "plan/nat.h"

/* The envelope is itself Silo, with no external index or private wire tags. */
typedef struct bundle_entry {
  pl_hash key;
  struct {
    uint8_t* bytes;
    size_t len;
  } value;
} bundle_entry;

typedef struct bundle_store {
  bundle_entry* entries;
  uint8_t root[32];
} bundle_store;

typedef struct bundle_visit {
  pl_val key;
  unsigned value;
} bundle_visit;

static bool bundle_error(char* err, size_t cap, const char* msg) {
  if (err != NULL && cap != 0) {
    (void)snprintf(err, cap, "%s", msg);
  }
  return false;
}

static bool bundle_walk(pl_val pin, bundle_visit** seen, pl_val** order,
                        unsigned depth, char* err, size_t cap) {
  if (depth > PL_SILO_MAX_DEPTH || !pl_pin_is_hashed(pin)) {
    return bundle_error(err, cap, "invalid or excessively deep pin closure");
  }
  pl_cell* p = pl_pin_resolved(pl_ptr(pin));
  pin = pl_make(PL_TAG_PIN, p);
  ptrdiff_t at = ax_hmgeti(*seen, pin);
  if (at >= 0) {
    return (*seen)[at].value == 2 ||
           bundle_error(err, cap, "cyclic pin closure");
  }
  ax_hmput(*seen, pin, 1);
  for (uint32_t i = 0; i < pl_pin_npins(p); i++) {
    if (!bundle_walk(pl_pin_subpins(p)[i], seen, order, depth + 1, err, cap)) {
      return false;
    }
  }
  ax_hmput(*seen, pin, 2);
  ax_arrpush(*order, pin);
  return true;
}

bool pl_store_bundle_encode(pl_val entry, uint8_t** bytes, size_t* len,
                            char* err, size_t err_cap) {
  *bytes = NULL;
  *len = 0;
  bundle_visit* seen = NULL;
  pl_val* order = NULL;
  bool ok = bundle_walk(entry, &seen, &order, 0, err, err_cap);
  pl_store* scratch = pl_store_new_mem();
  pl_heap* heap = pl_heap_new(1U << 16, scratch);
  pl_thread* t = pl_thread_new(heap);
  if ((size_t)ax_arrlen(order) > PL_HDR_META_MAX) {
    ok = bundle_error(err, err_cap, "too many pins in bundle");
  }
  for (ptrdiff_t i = 0; ok && i < ax_arrlen(order); i++) {
    pl_cell* p = pl_ptr(order[i]);
    uint8_t* stream = NULL;
    size_t n = 0;
    uint8_t hash[32];
    ok = pl_silo_encode_buffer(pl_pin_body(p), pl_pin_subpins(p),
                               pl_pin_npins(p), NULL, NULL, &stream, &n, hash,
                               err, err_cap);
    if (ok && memcmp(hash, pl_pin_hash(order[i]), 32) != 0) {
      ok = bundle_error(err, err_cap,
                        "entry store must use Silo SHA-256 pin identities");
    }
    if (ok) {
      /* A bar preserves trailing zero bytes in the embedded Silo stream. */
      ax_arrpush(stream, 1);
      pl_vpush(t, pl_nat_from_bytes(t, stream, n + 1));
    }
    ax_arrfree(stream);
  }
  if (ok) {
    uint32_t n = (uint32_t)t->vsp;
    pl_gc_reserve(t, PL_APP_CELLS(n));
    pl_val row = pl_mk_app_from(t, 0, n, t->vstack);
    uint8_t hash[32];
    uint8_t* encoded = NULL;
    ok = pl_silo_encode_buffer(row, NULL, 0, NULL, NULL, &encoded, len, hash,
                               err, err_cap);
    if (ok) {
      *bytes = malloc(*len);
      if (*bytes == NULL) {
        ok = bundle_error(err, err_cap, "out of memory encoding bundle");
      } else {
        memcpy(*bytes, encoded, *len);
      }
    }
    ax_arrfree(encoded);
  }
  pl_thread_free(t);
  pl_heap_free(heap);
  pl_store_free(scratch);
  ax_hmfree(seen);
  ax_arrfree(order);
  return ok;
}

static bool bundle_get(void* ctx, const uint8_t hash[32], uint8_t** bytes,
                       size_t* len) {
  bundle_store* b = ctx;
  if (b->entries == NULL) {
    return false;
  }
  pl_hash key;
  memcpy(key.b, hash, 32);
  ptrdiff_t at = ax_hmgeti(b->entries, key);
  if (at < 0) {
    return false;
  }
  *len = b->entries[at].value.len;
  *bytes = malloc(*len);
  if (*bytes == NULL) {
    return false;
  }
  memcpy(*bytes, b->entries[at].value.bytes, *len);
  return true;
}

static bool bundle_has(void* ctx, const uint8_t hash[32]) {
  bundle_store* b = ctx;
  pl_hash key;
  memcpy(key.b, hash, 32);
  return b->entries != NULL && ax_hmgeti(b->entries, key) >= 0;
}

static bool bundle_put(void* ctx, const uint8_t hash[32], const uint8_t* bytes,
                       size_t len) {
  (void)ctx;
  (void)hash;
  (void)bytes;
  (void)len;
  return false;
}

static bool bundle_put_root(void* ctx, const uint8_t hash[32]) {
  (void)ctx;
  (void)hash;
  return false;
}

static bool bundle_get_root(void* ctx, uint8_t hash[32]) {
  memcpy(hash, ((bundle_store*)ctx)->root, 32);
  return true;
}

static void bundle_close(void* ctx) {
  bundle_store* b = ctx;
  for (ptrdiff_t i = 0; i < ax_hmlen(b->entries); i++) {
    free(b->entries[i].value.bytes);
  }
  ax_hmfree(b->entries);
  free(b);
}

typedef struct bundle_reader {
  const uint8_t* bytes;
  size_t len, pos;
} bundle_reader;
static bool bundle_read(void* ctx, uint8_t* bytes, size_t len) {
  bundle_reader* r = ctx;
  if (len > r->len - r->pos) {
    return false;
  }
  memcpy(bytes, r->bytes + r->pos, len);
  r->pos += len;
  return true;
}
static bool bundle_rewind(void* ctx) {
  ((bundle_reader*)ctx)->pos = 0;
  return true;
}
static pl_silo_reader bundle_reader_new(bundle_reader* r) {
  return (pl_silo_reader){
      .ctx = r, .read = bundle_read, .rewind = bundle_rewind, .len = r->len};
}

pl_store* pl_store_new_bundle(const uint8_t* bytes, size_t len,
                              const uint8_t entry_hash[32], pl_val* entry,
                              char* err, size_t err_cap) {
  *entry = 0;
  bundle_store* b = calloc(1, sizeof(*b));
  if (b == NULL) {
    (void)bundle_error(err, err_cap, "out of memory opening bundle");
    return NULL;
  }
  memcpy(b->root, entry_hash, 32);
  pl_store* s = pl_store_new((pl_store_backend){.ctx = b,
                                                .get = bundle_get,
                                                .put = bundle_put,
                                                .has = bundle_has,
                                                .get_root = bundle_get_root,
                                                .put_root = bundle_put_root,
                                                .close = bundle_close});
  s->format = PL_STORE_FORMAT_BUNDLE_V1;
  pl_store* scratch = pl_store_new_mem();
  pl_heap* heap = pl_heap_new(1U << 16, s);
  pl_thread* t = pl_thread_new(heap);
  bundle_reader mem = {.bytes = bytes, .len = len};
  pl_silo_reader reader = bundle_reader_new(&mem);
  pl_silo_scan scan = {0};
  bool ok = pl_silo_scan_stream(&reader, true, &scan, err, err_cap);
  pl_val row = 0;
  if (ok && scan.pin_count != 0) {
    ok = bundle_error(err, err_cap, "bundle inventory has external pins");
  }
  if (ok) {
    ok =
        pl_silo_build_stream(&reader, scratch, &scan, NULL, &row, err, err_cap);
  }
  pl_silo_scan_free(&scan);
  pl_cell* app = pl_as(PL_TAG_APP, row);
  if (ok && (app == NULL || pl_app_head(app) != 0)) {
    ok = bundle_error(err, err_cap, "bundle inventory is not a nonempty row");
  }
  uint32_t count = ok ? pl_app_n(app) : 0;
  for (uint32_t i = 0; ok && i < count; i++) {
    pl_val bar = pl_app_args(app)[i];
    size_t n = pl_is_nat(bar) ? pl_nat_byte_len(bar) : 0;
    if (n <= 1 || pl_nat_byte_at(bar, n - 1) != 1) {
      ok = bundle_error(err, err_cap, "invalid bundle stream bar");
      break;
    }
    n--;
    uint8_t* stream = malloc(n);
    if (stream == NULL) {
      ok = bundle_error(err, err_cap, "out of memory decoding bundle");
      break;
    }
    for (size_t j = 0; j < n; j++) {
      stream[j] = pl_nat_byte_at(bar, j);
    }
    bundle_reader inner = {.bytes = stream, .len = n};
    pl_silo_reader rd = bundle_reader_new(&inner);
    pl_silo_scan pins = {0};
    ok = pl_silo_scan_stream(&rd, true, &pins, err, err_cap);
    for (size_t j = 0; ok && j < pins.pin_count; j++) {
      if (!bundle_has(b, pins.pins[j].b)) {
        ok = bundle_error(err, err_cap,
                          "missing or out-of-order bundle dependency");
      }
    }
    pl_silo_scan_free(&pins);
    pl_hash key;
    ax_sha256(stream, n, key.b);
    if (ok && bundle_has(b, key.b)) {
      ok = bundle_error(err, err_cap, "duplicate bundle pin");
    }
    if (ok) {
      ok = pl_store_load_silo_stream(t, s, stream, n, entry, err, err_cap);
    }
    if (ok) {
      bundle_entry item = {.key = key, .value = {.bytes = stream, .len = n}};
      ax_hmput(b->entries, key, item.value);
    } else {
      free(stream);
    }
  }
  if (ok && memcmp(pl_pin_hash(*entry), entry_hash, 32) != 0) {
    ok = bundle_error(err, err_cap, "bundle entry hash mismatch");
  }
  bundle_visit* seen = NULL;
  pl_val* order = NULL;
  if (ok) {
    ok = bundle_walk(*entry, &seen, &order, 0, err, err_cap);
  }
  if (ok && (size_t)ax_arrlen(order) != count) {
    ok = bundle_error(err, err_cap, "unreachable pins in bundle");
  }
  ax_hmfree(seen);
  ax_arrfree(order);
  pl_thread_free(t);
  pl_heap_free(heap);
  pl_store_free(scratch);
  if (!ok) {
    pl_store_free(s);
    *entry = 0;
    return NULL;
  }
  return s;
}
