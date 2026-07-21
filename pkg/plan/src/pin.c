#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "axsys/allocator.h"
#include "axsys/assume.h"
#include "axsys/sha256.h"
#include "axsys/ds.h"
#include "internal.h"
#include "plan/build.h"
#include "plan/canon.h"
#include "plan/nat.h"
#include "plan/store.h"
#include "store_internal.h"

/*
 * Pinning.  The value is normalized and deep-copied into the non-moving store
 * region.  Legacy stores immediately hash and intern it using the reference
 * canonical snapshot text.  Silo stores leave it provisional until Save,
 * which hashes the complete canonical stream and persists the reachable PIN
 * closure.  Nothing here allocates on the moving heap, so the source graph is
 * stable and bare pointers are safe throughout.
 *
 * The persistence backend keeps a binary rendering (below), because
 * rehydration must not depend on the enki-layer assembler; it is keyed
 * by the selected serialization hash.
 *
 * Backend byte format (version 1):
 *   u8  version
 *   u64 nsub                       (LE)
 *   32B * nsub                     (sub-pin hashes, first-occurrence LTR)
 *   body:
 *     'n' u64 len, bytes           (nat, little-endian, trimmed)
 *     'p' u32 index                (reference to sub-pin)
 *     'l' u64 arity, name, body    (law)
 *     'a' u32 nargs, head, args…   (app)
 */

#define PL_CANON_VERSION 1u

typedef struct pin_idx_entry {
  pl_val key;
  uint32_t value;
} pin_idx_entry;

typedef struct canon_ctx {
  uint8_t* buf;       /* stb_ds array */
  pl_val* subpins;    /* stb_ds array, first-occurrence order */
  pin_idx_entry* idx; /* stb_ds hashmap: pin val -> index */
} canon_ctx;

static void cput8(canon_ctx* c, uint8_t b) {
  ax_arrpush(c->buf, b);
}

static void cput32(canon_ctx* c, uint32_t v) {
  for (int i = 0; i < 4; i++)
    ax_arrpush(c->buf, (uint8_t)(v >> (8 * i)));
}

static void cput64(canon_ctx* c, uint64_t v) {
  for (int i = 0; i < 8; i++)
    ax_arrpush(c->buf, (uint8_t)(v >> (8 * i)));
}

/* Collect sub-pins, shallow, first-occurrence, left-to-right. */
static void collect_subpins(canon_ctx* c, pl_val v) {
  if (pl_is_nat(v))
    return;
  pl_cell* p = pl_ptr(v);
  switch (pl_tag(v)) {
  case PL_TAG_PIN:
    if (ax_hmgeti(c->idx, v) < 0) {
      ax_hmput(c->idx, v, (uint32_t)ax_arrlen(c->subpins));
      ax_arrpush(c->subpins, v);
    }
    return;
  case PL_TAG_LAW:
    collect_subpins(c, pl_law_name(p));
    collect_subpins(c, pl_law_body(p));
    return;
  case PL_TAG_APP: {
    collect_subpins(c, pl_app_head(p));
    uint32_t n = pl_app_n(p);
    for (uint32_t i = 0; i < n; i++)
      collect_subpins(c, pl_app_args(p)[i]);
    return;
  }
  default:
    ax_abort("collect_subpins: non-normal value (tag 0x%llx)",
             (unsigned long long)pl_tag(v));
  }
}

static void serialize(canon_ctx* c, pl_val v) {
  if (pl_is_nat(v)) {
    size_t len = pl_nat_byte_len(v);
    cput8(c, 'n');
    cput64(c, len);
    for (size_t i = 0; i < len; i++)
      cput8(c, pl_nat_byte_at(v, i));
    return;
  }
  pl_cell* p = pl_ptr(v);
  switch (pl_tag(v)) {
  case PL_TAG_PIN: {
    ptrdiff_t i = ax_hmgeti(c->idx, v);
    ax_assume(i >= 0, "serialize: pin not collected");
    cput8(c, 'p');
    cput32(c, c->idx[i].value);
    return;
  }
  case PL_TAG_LAW:
    cput8(c, 'l');
    cput64(c, pl_law_arity(p));
    serialize(c, pl_law_name(p));
    serialize(c, pl_law_body(p));
    return;
  case PL_TAG_APP: {
    uint32_t n = pl_app_n(p);
    cput8(c, 'a');
    cput32(c, n);
    serialize(c, pl_app_head(p));
    for (uint32_t i = 0; i < n; i++)
      serialize(c, pl_app_args(p)[i]);
    return;
  }
  default:
    ax_abort("serialize: non-normal value");
  }
}

/* ── Deep copy of a normalized graph into the store region ─────────────── */

typedef struct copy_entry {
  pl_val key;
  pl_val value;
} copy_entry;

static pl_val store_copy(pl_store* s, copy_entry** map, pl_val v) {
  if (pl_is_nat63(v))
    return v;
  if (pl_store_owns(s, v))
    return v; /* sub-pins and prior pinned data */
  ptrdiff_t hit = ax_hmgeti(*map, v);
  if (hit >= 0)
    return (*map)[hit].value;
  pl_cell* p = pl_ptr(v);
  pl_val nv;
  switch (pl_tag(v)) {
  case PL_TAG_NAT: {
    uint32_t used = pl_nat_limbs(p);
    pl_cell* np = pl_store_alloc(s, PL_NAT_CELLS(used));
    np[0] = pl_hdr_make(PL_K_NAT, PL_F_NORMAL, used, PL_NAT_CELLS(used));
    memcpy(np + 1, pl_nat_limb_ptr(p), used * 8);
    nv = pl_make(PL_TAG_NAT, np);
    break;
  }
  case PL_TAG_LAW: {
    pl_cell* np = pl_store_alloc(s, PL_LAW_CELLS);
    np[0] = pl_hdr_make(PL_K_LAW, PL_F_NORMAL, 0, PL_LAW_CELLS);
    np[1] = pl_law_arity(p);
    nv = pl_make(PL_TAG_LAW, np);
    ax_hmput(*map, v, nv);
    np[2] = store_copy(s, map, pl_law_name(p));
    np[3] = store_copy(s, map, pl_law_body(p));
    return nv;
  }
  case PL_TAG_APP: {
    uint32_t n = pl_app_n(p);
    pl_cell* np = pl_store_alloc(s, PL_APP_CELLS(n));
    np[0] = pl_hdr_make(PL_K_APP, PL_F_NORMAL, pl_app_need(p), PL_APP_CELLS(n));
    nv = pl_make(PL_TAG_APP, np);
    ax_hmput(*map, v, nv);
    np[1] = store_copy(s, map, pl_app_head(p));
    for (uint32_t i = 0; i < n; i++)
      np[2 + i] = store_copy(s, map, pl_app_args(pl_ptr(v))[i]);
    return nv;
  }
  default:
    ax_abort("store_copy: non-normal value (tag 0x%llx)",
             (unsigned long long)pl_tag(v));
  }
  ax_hmput(*map, v, nv);
  return nv;
}

/* ── Pinning ───────────────────────────────────────────────────────────── */

static pl_val pin_from_normal(pl_store* s, canon_ctx* c, pl_val body) {
  size_t nsub = (size_t)ax_arrlen(c->subpins);
  if (s->format == PL_STORE_FORMAT_SILO_V1) {
    ax_assume(nsub <= PL_SILO_MAX_PIN_COUNT,
              "Silo PIN exceeds the direct PIN-table limit");
    copy_entry* map = NULL;
    pl_val body_copy = store_copy(s, &map, body);
    ax_hmfree(map);
    return pl_store_mk_pin(s, NULL, body_copy, (uint32_t)nsub, c->subpins);
  }

  /* Legacy identity is SHA-256 of canonical text (reference mkPin). */
  uint8_t hash[32];
  size_t text_n;
  char* text = pl_canonize(ax_allocator_system(), body, &text_n);
  ax_sha256((const uint8_t*)text, text_n, hash);
  ax_free(ax_allocator_system(), text);

  pl_store_lock(s);
  pl_val pin = pl_store_intern_get(s, hash);
  if (pin == 0) {
    /* Legacy persistence bytes: fixed header + direct pin table + body. */
    uint8_t* full = NULL;
    ax_arrpush(full, PL_CANON_VERSION);
    for (int i = 0; i < 8; i++)
      ax_arrpush(full, (uint8_t)((uint64_t)nsub >> (8 * i)));
    for (size_t j = 0; j < nsub; j++) {
      const uint8_t* h = pl_pin_hash(c->subpins[j]);
      for (int i = 0; i < 32; i++)
        ax_arrpush(full, h[i]);
    }
    for (ptrdiff_t i = 0; i < ax_arrlen(c->buf); i++)
      ax_arrpush(full, c->buf[i]);
    ax_assume(pl_store_backend_put(s, hash, full, (size_t)ax_arrlen(full)),
              "store backend put failed");
    ax_arrfree(full);
    copy_entry* map = NULL;
    pl_val body_copy = store_copy(s, &map, body);
    ax_hmfree(map);
    pin = pl_store_mk_pin(s, hash, body_copy, (uint32_t)nsub, c->subpins);
    pl_store_intern_put(s, hash, pin);
    if (pl_tag(body_copy) == PL_TAG_LAW)
      pl_store_put_code(s, hash);
  }
  pl_store_unlock(s);
  return pin;
}

/* ── Silo Save-time finalization ───────────────────────────────────────── */

typedef struct save_visit {
  pl_val key;
  uint8_t value; /* 1 = active, 2 = complete */
} save_visit;

typedef struct save_hash {
  pl_val key;
  pl_hash value;
} save_hash;

typedef struct save_ctx {
  pl_store* store;
  pl_silo_batch* batch;
  save_visit* visit;
  save_hash* pending;       /* provisional PIN pointer -> computed hash */
  pl_val* pins;             /* complete closure in postorder */
  pl_intern_entry* compile; /* unique finalized LAW hashes */
  char* err;
  size_t err_cap;
} save_ctx;

static bool save_error(save_ctx* c, const char* fmt, ...) {
  if (c->err != NULL && c->err_cap != 0) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(c->err, c->err_cap, fmt, ap);
    va_end(ap);
  }
  return false;
}

static const uint8_t* save_pin_hash(void* ctx, pl_val pin) {
  save_ctx* c = ctx;
  const uint8_t* persistent = pl_pin_hash(pin);
  if (persistent != NULL)
    return persistent;
  ptrdiff_t at = ax_hmgeti(c->pending, pin);
  return at >= 0 ? c->pending[at].value.b : NULL;
}

static bool save_silo_pin(save_ctx* c, pl_val pin, uint32_t depth) {
  if (depth > PL_SILO_MAX_DEPTH)
    return save_error(c, "Silo PIN closure exceeds depth %u",
                      PL_SILO_MAX_DEPTH);
  pl_cell* p = pl_as(PL_TAG_PIN, pin);
  if (p == NULL || !pl_store_owns(c->store, pin))
    return save_error(c, "Silo Save encountered a non-store PIN");

  ptrdiff_t seen_at = ax_hmgeti(c->visit, pin);
  if (seen_at >= 0) {
    if (c->visit[seen_at].value == 1)
      return save_error(c, "cyclic Silo PIN dependency");
    return true;
  }
  ax_hmput(c->visit, pin, 1);

  uint32_t nsub = pl_pin_npins(p);
  pl_val* subpins = pl_pin_subpins(p);
  for (uint32_t i = 0; i < nsub; i++)
    if (!save_silo_pin(c, subpins[i], depth + 1))
      return false;

  /* Provisional values may contain distinct, semantically equal PIN
   * pointers. Build the canonical direct table separately; mutating the
   * runtime table before commit would make a failed Save unsafe to retry. */
  pl_intern_entry* unique = NULL;
  pl_val* canonical_subpins = NULL;
  for (uint32_t i = 0; i < nsub; i++) {
    const uint8_t* hash = save_pin_hash(c, subpins[i]);
    if (hash == NULL) {
      ax_hmfree(unique);
      ax_arrfree(canonical_subpins);
      return save_error(c, "Silo child PIN hash was not prepared");
    }
    pl_hash key;
    memcpy(key.b, hash, 32);
    if (ax_hmgeti(unique, key) < 0) {
      ax_hmput(unique, key, subpins[i]);
      ax_arrpush(canonical_subpins, subpins[i]);
    }
  }
  ax_hmfree(unique);

  const uint8_t* known_hash = pl_pin_hash(pin);
  const uint8_t* hash = known_hash;
  bool present = false;
  if (known_hash != NULL &&
      !pl_store_silo_batch_contains(c->batch, known_hash, &present, c->err,
                                    c->err_cap)) {
    ax_arrfree(canonical_subpins);
    return false;
  }

  uint8_t calculated_hash[32];
  if (known_hash == NULL || !present) {
    uint8_t* bytes = NULL;
    size_t len = 0;
    bool encoded = pl_silo_encode_buffer(
        pl_pin_body(p), canonical_subpins, (size_t)ax_arrlen(canonical_subpins),
        save_pin_hash, c, &bytes, &len, calculated_hash, c->err, c->err_cap);
    ax_arrfree(canonical_subpins);
    canonical_subpins = NULL;
    if (!encoded)
      return false;
    if (known_hash != NULL && memcmp(known_hash, calculated_hash, 32) != 0) {
      ax_arrfree(bytes);
      return save_error(c, "runtime PIN hash does not match its Silo stream");
    }
    hash = known_hash != NULL ? known_hash : calculated_hash;
    bool stored =
        pl_store_silo_batch_put(c->batch, hash, bytes, len, c->err, c->err_cap);
    ax_arrfree(bytes);
    if (!stored)
      return false;
  } else {
    ax_arrfree(canonical_subpins);
  }

  if (known_hash == NULL) {
    pl_hash prepared;
    memcpy(prepared.b, hash, sizeof(prepared.b));
    ax_hmput(c->pending, pin, prepared);
  }

  /* A previous Save may have indexed and finalized this law before failing
   * to publish its root.  Queue any still-interpreted law in the closure so a
   * successful retry performs the deferred compilation. */
  if (c->store->compiler_f && pl_tag(pl_pin_body(p)) == PL_TAG_LAW &&
      pl_pin_code(p) == NULL) {
    pl_hash key;
    memcpy(key.b, hash, 32);
    if (ax_hmgeti(c->compile, key) < 0)
      ax_hmput(c->compile, key, pin);
  }

  ptrdiff_t at = ax_hmgeti(c->visit, pin);
  ax_assume(at >= 0, "Silo Save visit disappeared");
  c->visit[at].value = 2;
  ax_arrpush(c->pins, pin);
  return true;
}

static void save_finalize_runtime(save_ctx* c) {
  /* Canonicalize every direct table before publishing any new hash flag.  The
   * pending resolver lets this happen without making child hashes visible
   * early, and keeps a hashed PIN from ever exposing its pre-save table. */
  for (ptrdiff_t i = 0; i < ax_arrlen(c->pins); i++) {
    pl_cell* p = pl_ptr(c->pins[i]);
    uint32_t nsub = pl_pin_npins(p);
    pl_val* subpins = pl_pin_subpins(p);
    pl_intern_entry* unique = NULL;
    uint32_t out_n = 0;
    for (uint32_t j = 0; j < nsub; j++) {
      const uint8_t* hash = save_pin_hash(c, subpins[j]);
      ax_assume(hash != NULL, "committed Silo child has no hash");
      pl_hash key;
      memcpy(key.b, hash, sizeof(key.b));
      if (ax_hmgeti(unique, key) < 0) {
        ax_hmput(unique, key, subpins[j]);
        subpins[out_n++] = subpins[j];
      }
    }
    ax_hmfree(unique);
    p[0] = pl_hdr_set_meta(p[0], out_n);
  }

  /* Publish runtime hash flags only after every object and the root are
   * durable in the atomic backend transaction and every table is canonical.
   * Postorder ensures child flags are published before their parents. */
  for (ptrdiff_t i = 0; i < ax_arrlen(c->pins); i++) {
    pl_val pin = c->pins[i];
    if (pl_pin_is_hashed(pin))
      continue;
    const uint8_t* hash = save_pin_hash(c, pin);
    ax_assume(hash != NULL, "committed Silo PIN has no prepared hash");
    pl_cell* p = pl_ptr(pin);
    memcpy(pl_pin_hash_bytes(p), hash, 32);
    p[0] = pl_hdr_set_flag(p[0], PL_F_PIN_HASHED);
    if (pl_store_intern_get(c->store, hash) == 0)
      pl_store_intern_put(c->store, hash, pin);
  }
}

bool pl_store_save_root(pl_store* s, pl_val pin, uint8_t out_hash[32],
                        char* err, size_t err_cap) {
  if (s == NULL || s->format != PL_STORE_FORMAT_SILO_V1) {
    if (err != NULL && err_cap != 0)
      (void)snprintf(err, err_cap, "store is not a Silo backend");
    return false;
  }

  save_ctx c = {.store = s, .err = err, .err_cap = err_cap};
  pl_store_lock(s);
  bool ok = pl_store_silo_batch_begin(s, &c.batch, err, err_cap);
  if (ok)
    ok = save_silo_pin(&c, pin, 0);
  const uint8_t* root_hash = ok ? save_pin_hash(&c, pin) : NULL;
  if (ok && root_hash == NULL)
    ok = save_error(&c, "Silo root PIN hash was not prepared");
  if (ok) {
    pl_silo_batch* batch = c.batch;
    c.batch = NULL;
    ok = pl_store_silo_batch_commit(batch, root_hash, err, err_cap);
  }
  if (!ok && c.batch != NULL) {
    pl_store_silo_batch_abort(c.batch);
    c.batch = NULL;
  }
  if (ok) {
    save_finalize_runtime(&c);
    if (out_hash != NULL)
      memcpy(out_hash, root_hash, 32);
  }
  bool compile = ok && s->compiler_f;
  pl_store_unlock(s);

  if (compile)
    for (ptrdiff_t i = 0; i < ax_hmlen(c.compile); i++)
      pl_store_put_code(s, c.compile[i].key.b);
  ax_arrfree(c.pins);
  ax_hmfree(c.pending);
  ax_hmfree(c.compile);
  ax_hmfree(c.visit);
  return ok;
}

pl_val pl_pin(pl_thread* t, pl_val v) {
  pl_store* s = pl_heap_store(t->heap);
  ax_assume(s != NULL, "pinning requires a store");
  pl_store_lock(s);
  /* v is rooted by the machine while it normalizes; afterwards nothing
   * below can collect (serialization buffers are malloc'd and the pin
   * itself is built in the non-moving store region), so the bare val is
   * safe.  Taking a slot pointer here would be wrong: evaluation may
   * grow (realloc) the very stacks most callers' slots live in. */
  v = pl_nf(t, v);

  canon_ctx c = {0};
  collect_subpins(&c, v);
  if (s->format == PL_STORE_FORMAT_LEGACY_V1)
    serialize(&c, v);
  pl_val pin = pin_from_normal(s, &c, v);
  ax_arrfree(c.buf);
  ax_arrfree(c.subpins);
  ax_hmfree(c.idx);
  pl_store_unlock(s);
  return pin;
}

pl_val pl_store_pin_of_nat(pl_store* s, uint64_t n) {
  ax_assume(n <= PL_NAT63_MAX, "pin_of_nat: too large");
  canon_ctx c = {0};
  if (s->format == PL_STORE_FORMAT_LEGACY_V1)
    serialize(&c, n);
  pl_val pin = pin_from_normal(s, &c, n);
  ax_arrfree(c.buf);
  ax_arrfree(c.subpins);
  ax_hmfree(c.idx);
  return pin;
}

/* ── Loading ───────────────────────────────────────────────────────────── */

typedef struct deser_ctx {
  const uint8_t* b;
  size_t n;
  size_t off;
  pl_val* subpins;
  size_t nsub;
  pl_thread* t;
} deser_ctx;

static uint64_t dget(deser_ctx* d, size_t width) {
  ax_assume(d->off + width <= d->n, "pin bytes truncated");
  uint64_t v = 0;
  for (size_t i = 0; i < width; i++)
    v |= (uint64_t)d->b[d->off + i] << (8 * i);
  d->off += width;
  return v;
}

static pl_val deser(pl_store* s, deser_ctx* d) {
  uint64_t tag = dget(d, 1);
  switch (tag) {
  case 'n': {
    uint64_t len = dget(d, 8);
    ax_assume(d->off + len <= d->n, "pin bytes truncated");
    const uint8_t* bytes = d->b + d->off;
    d->off += len;
    if (len < 8) {
      uint64_t v = 0;
      memcpy(&v, bytes, len);
      return v;
    }
    if (len == 8) {
      uint64_t v;
      memcpy(&v, bytes, 8);
      if (v <= PL_NAT63_MAX)
        return v;
    }
    size_t limbs = (len + 7) / 8;
    pl_cell* np = pl_store_alloc(s, PL_NAT_CELLS(limbs));
    np[0] = pl_hdr_make(PL_K_NAT, PL_F_NORMAL, (uint32_t)limbs,
                        PL_NAT_CELLS(limbs));
    np[limbs] = 0;
    memcpy(np + 1, bytes, len);
    return pl_nat_trim(pl_make(PL_TAG_NAT, np));
  }
  case 'p': {
    uint64_t i = dget(d, 4);
    ax_assume(i < d->nsub, "pin index out of range");
    return d->subpins[i];
  }
  case 'l': {
    uint64_t arity = dget(d, 8);
    pl_cell* np = pl_store_alloc(s, PL_LAW_CELLS);
    np[0] = pl_hdr_make(PL_K_LAW, PL_F_NORMAL, 0, PL_LAW_CELLS);
    np[1] = arity;
    np[2] = deser(s, d);
    np[3] = deser(s, d);
    return pl_make(PL_TAG_LAW, np);
  }
  case 'a': {
    uint64_t n = dget(d, 4);
    ax_assume(n >= 1, "empty app in pin bytes");
    pl_cell* np = pl_store_alloc(s, PL_APP_CELLS(n));
    np[0] = pl_hdr_make(PL_K_APP, PL_F_NORMAL, 0, PL_APP_CELLS(n));
    pl_val nv = pl_make(PL_TAG_APP, np);
    np[1] = deser(s, d);
    for (uint64_t i = 0; i < n; i++)
      np[2 + i] = deser(s, d);
    /* fix the need cache now that the head exists */
    uint64_t a = pl_arity((pl_val)np[1]);
    uint32_t need = (a == 0 || a <= n) ? 0 : (uint32_t)(a - n);
    np[0] = pl_hdr_make(PL_K_APP, PL_F_NORMAL, need, PL_APP_CELLS(n));
    return nv;
  }
  default:
    ax_abort("bad tag in pin bytes: %u", (unsigned)tag);
  }
}

static bool silo_hash_loading(pl_store* s, const uint8_t hash[32]) {
  for (ptrdiff_t i = 0; i < ax_arrlen(s->loading); i++)
    if (memcmp(s->loading[i].b, hash, 32) == 0)
      return true;
  return false;
}

static bool load_silo_pin(pl_thread* t, pl_store* s, const uint8_t hash[32],
                          pl_val* out, char* err, size_t err_cap) {
  pl_val hit = pl_store_intern_get(s, hash);
  if (hit != 0) {
    *out = hit;
    return true;
  }
  if (silo_hash_loading(s, hash)) {
    (void)snprintf(err, err_cap, "cyclic Silo PIN dependency");
    return false;
  }
  pl_hash loading;
  memcpy(loading.b, hash, 32);
  ax_arrpush(s->loading, loading);

  pl_silo_reader reader = {0};
  pl_silo_scan scan = {0};
  pl_val* resolved = NULL;
  pl_val* subpins = NULL;
  size_t mark = 0;
  bool marked = false;
  bool reader_open = false;
  bool ok = pl_store_silo_open(s, hash, &reader, err, err_cap);
  if (!ok)
    goto done;
  reader_open = true;
  if (!pl_silo_scan_stream(&reader, false, &scan, err, err_cap)) {
    ok = false;
    goto done;
  }
  resolved = calloc(scan.pin_count ? scan.pin_count : 1, sizeof(*resolved));
  if (resolved == NULL) {
    (void)snprintf(err, err_cap, "out of memory resolving Silo PINs");
    ok = false;
    goto done;
  }
  for (size_t i = 0; i < scan.used_count; i++) {
    uint32_t index = scan.used[i];
    pl_val sub;
    if (!load_silo_pin(t, s, scan.pins[index].b, &sub, err, err_cap)) {
      ok = false;
      goto done;
    }
    resolved[index] = sub;
    ax_arrpush(subpins, sub);
  }

  mark = pl_store_mark(s);
  marked = true;
  pl_val body;
  if (!pl_silo_build_stream(&reader, s, &scan, resolved, &body, err, err_cap)) {
    ok = false;
    goto done;
  }

  uint8_t actual[32];
  if (!pl_silo_hash(body, subpins, (size_t)ax_arrlen(subpins), actual, err,
                    err_cap)) {
    ok = false;
    goto done;
  }
  if (memcmp(actual, hash, 32) != 0) {
    (void)snprintf(err, err_cap,
                   "Silo value does not match canonical stream hash");
    ok = false;
    goto done;
  }

  size_t pin_cells = PL_PIN_CELLS((uint32_t)scan.used_count);
  size_t region_mark = pl_store_mark(s);
  if (region_mark > s->region->cap_s ||
      pin_cells > (s->region->cap_s - region_mark) / sizeof(pl_cell)) {
    (void)snprintf(err, err_cap,
                   "decoded PIN exceeds remaining store-region capacity");
    ok = false;
    goto done;
  }

  *out = pl_store_mk_pin(s, hash, body, (uint32_t)scan.used_count, subpins);
  pl_store_intern_put(s, hash, *out);
  marked = false; /* the successful allocations now belong to the store */
  ok = true;

done:
  if (!ok && marked)
    pl_store_release(s, mark);
  if (reader_open)
    pl_store_silo_close_reader(&reader);
  pl_silo_scan_free(&scan);
  free(resolved);
  ax_arrfree(subpins);
  ax_assume(ax_arrlen(s->loading) > 0, "Silo loading stack underflow");
  (void)stbds_arrpop(s->loading);
  if (ok && pl_tag(pl_pin_body(pl_ptr(*out))) == PL_TAG_LAW)
    pl_store_put_code(s, hash);
  return ok;
}

pl_val pl_store_load(pl_thread* t, const uint8_t hash[32]) {
  pl_store* s = pl_heap_store(t->heap);
  ax_assume(s != NULL, "store_load requires a store");
  pl_store_lock(s);
  pl_val hit = pl_store_intern_get(s, hash);
  if (hit != 0) {
    pl_store_unlock(s);
    return hit;
  }
  if (s->format == PL_STORE_FORMAT_SILO_V1) {
    char err[192] = {0};
    pl_val pin = 0;
    bool ok = load_silo_pin(t, s, hash, &pin, err, sizeof(err));
    pl_store_unlock(s);
    if (!ok)
      pl_raise_msgf(t, "store_load: %s", err[0] != '\0' ? err : "bad Silo");
    return pin;
  }
  uint8_t* bytes;
  size_t n;
  if (!pl_store_backend_get(s, hash, &bytes, &n)) {
    pl_store_unlock(s);
    pl_raise_msgf(t, "store_load: missing pin");
  }
  deser_ctx d = {.b = bytes, .n = n, .t = t};
  uint64_t ver = dget(&d, 1);
  ax_assume(ver == PL_CANON_VERSION, "bad pin version %u", (unsigned)ver);
  d.nsub = dget(&d, 8);
  pl_val* subs = NULL;
  for (size_t i = 0; i < d.nsub; i++) {
    uint8_t sub[32];
    ax_assume(d.off + 32 <= d.n, "pin bytes truncated");
    memcpy(sub, d.b + d.off, 32);
    d.off += 32;
    ax_arrpush(subs, pl_store_load(t, sub));
  }
  d.subpins = subs;
  pl_val body = deser(s, &d);
  pl_val pin = pl_store_mk_pin(s, hash, body, (uint32_t)d.nsub, subs);
  pl_store_intern_put(s, hash, pin);
  if (pl_tag(body) == PL_TAG_LAW)
    pl_store_put_code(s, hash);
  ax_arrfree(subs);
  free(bytes);
  pl_store_unlock(s);
  return pin;
}
