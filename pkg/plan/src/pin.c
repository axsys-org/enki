#include <stdlib.h>
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
 * Pinning.  The value is normalized, rendered to the canonical
 * snapshot text (plan/canon.h), hashed with SHA-256 — exactly the
 * reference mkPin — then interned, and on an intern miss deep-copied
 * into the non-moving store region.  Nothing here allocates on the
 * moving heap, so the source graph is stable and bare pointers are safe
 * throughout.
 *
 * The persistence backend keeps a binary rendering (below), because
 * rehydration must not depend on the enki-layer assembler; it is keyed
 * by the same canonical-text hash.
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

static pl_val pin_from_canon(pl_store* s, canon_ctx* c, pl_val body) {
  size_t nsub = (size_t)ax_arrlen(c->subpins);
  /* assemble the full canonical buffer: header + body bytes */
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

  /* the content hash is SHA-256 of the canonical TEXT (mkPin) */
  uint8_t hash[32];
  {
    size_t text_n;
    char* text = pl_canonize(ax_allocator_system(), body, &text_n);
    ax_sha256((const uint8_t*)text, text_n, hash);
    ax_free(ax_allocator_system(), text);
  }

  /* The intern-or-copy step is the store's only mutation outside load:
   * hold the store lock so the get/copy/put is atomic against parallel
   * pins from other worker threads (a no-op when not concurrent). */
  pl_store_lock(s);
  pl_val pin = pl_store_intern_get(s, hash);
  if (pin == 0) {
    copy_entry* map = NULL;
    pl_val body_copy = store_copy(s, &map, body);
    ax_hmfree(map);
    pin = pl_store_mk_pin(s, hash, body_copy, (uint32_t)nsub, c->subpins);
    pl_store_intern_put(s, hash, pin);
    ax_assume(pl_store_backend_put(s, hash, full, (size_t)ax_arrlen(full)),
              "store backend put failed");
  }
  pl_store_unlock(s);
  ax_arrfree(full);
  return pin;
}

pl_val pl_pin(pl_thread* t, pl_val v) {
  pl_store* s = pl_heap_store(t->heap);
  ax_assume(s != NULL, "pinning requires a store");
  /* v is rooted by the machine while it normalizes; afterwards nothing
   * below can collect (serialization buffers are malloc'd and the pin
   * itself is built in the non-moving store region), so the bare val is
   * safe.  Taking a slot pointer here would be wrong: evaluation may
   * grow (realloc) the very stacks most callers' slots live in. */
  v = pl_nf(t, v);

  canon_ctx c = {0};
  collect_subpins(&c, v);
  serialize(&c, v);
  pl_val pin = pin_from_canon(s, &c, v);
  ax_arrfree(c.buf);
  ax_arrfree(c.subpins);
  ax_hmfree(c.idx);
  return pin;
}

pl_val pl_store_pin_of_nat(pl_store* s, uint64_t n) {
  ax_assume(n <= PL_NAT63_MAX, "pin_of_nat: too large");
  canon_ctx c = {0};
  serialize(&c, n);
  pl_val pin = pin_from_canon(s, &c, n);
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

/* The recursive load worker; the public entry holds the store lock so
 * the whole transitive load (this node and its sub-pins) is one atomic
 * critical section against parallel pins/loads. */
static pl_val store_load_rec(pl_thread* t, pl_store* s,
                             const uint8_t hash[32]) {
  pl_val hit = pl_store_intern_get(s, hash);
  if (hit != 0)
    return hit;
  uint8_t* bytes;
  size_t n;
  if (!pl_store_backend_get(s, hash, &bytes, &n))
    pl_raise_msgf(t, "store_load: missing pin");
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
    ax_arrpush(subs, store_load_rec(t, s, sub));
  }
  d.subpins = subs;
  pl_val body = deser(s, &d);
  pl_val pin = pl_store_mk_pin(s, hash, body, (uint32_t)d.nsub, subs);
  pl_store_intern_put(s, hash, pin);
  ax_arrfree(subs);
  free(bytes);
  return pin;
}

/*
 * Loading is a single-threaded / quiescent operation (module boot and
 * the Compile op); the concurrent executor never reaches it — eval holds
 * already-resident pins, it does not rehydrate by hash.  So the lock here
 * is effectively uncontended, and the catchable "missing pin" raise in
 * the worker cannot strand it (a raising load only happens off the
 * concurrent path, where the lock is a no-op).
 */
pl_val pl_store_load(pl_thread* t, const uint8_t hash[32]) {
  pl_store* s = pl_heap_store(t->heap);
  ax_assume(s != NULL, "store_load requires a store");
  pl_store_lock(s);
  pl_val pin = store_load_rec(t, s, hash);
  pl_store_unlock(s);
  return pin;
}
