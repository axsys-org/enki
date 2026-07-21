#include "plan/store.h"

#include <pthread.h>
#include <lmdb.h>
#include <setjmp.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include "axsys/arena.h"
#include "axsys/assume.h"
#include "axsys/ds.h"
#include "axsys/util.h"
#include "internal.h"
#include "plan/build.h"
#include "store_internal.h"

/* Default reserved (overcommitted) size of the store region. */
#ifndef PL_STORE_REGION_BYTES
#define PL_STORE_REGION_BYTES (((size_t)1) << 38)
#endif

/* Lock order is save_mu -> mu.  save_mu serializes persistence operations and
 * the compiler machine; mu protects the arena and in-memory registries. */

void pl_store_lock(pl_store* s) {
  ax_assume(pthread_mutex_lock(&s->mu) == 0, "pthread_mutex_lock");
}

bool pl_store_trylock(pl_store* s) {
  int rc = pthread_mutex_trylock(&s->mu);
  if (!rc) {
    return true;
  } else {
    ax_assume(rc == EBUSY, "pthread_mutex_trylock");
    return false;
  }
}

void pl_store_unlock(pl_store* s) {
  ax_assume(pthread_mutex_unlock(&s->mu) == 0, "pthread_mutex_unlock");
}

void pl_store_save_lock(pl_store* s) {
  ax_assume(pthread_mutex_lock(&s->save_mu) == 0, "save pthread_mutex_lock");
}

void pl_store_save_unlock(pl_store* s) {
  ax_assume(pthread_mutex_unlock(&s->save_mu) == 0,
            "save pthread_mutex_unlock");
}

/* ── Region allocation ─────────────────────────────────────────────────── */

pl_cell* pl_store_alloc(pl_store* s, size_t cells) {
  pl_store_lock(s);
  void* p = ax_arena_alloc_aligned(s->region, cells * sizeof(pl_cell), 8);
  ax_assume(p != NULL, "store region exhausted");
  pl_store_unlock(s);
  return p;
}

size_t pl_store_mark(pl_store* s) {
  pl_store_lock(s);
  size_t mark = s->region->off_o;
  pl_store_unlock(s);
  return mark;
}

void pl_store_release(pl_store* s, size_t mark) {
  pl_store_lock(s);
  s->region->off_o = mark;
  pl_store_unlock(s);
}

/* ── Intern table ──────────────────────────────────────────────────────── */

static pl_cell* pl_store_require_canonical_pin(pl_store* s, pl_val pin,
                                               const char* caller) {
  /* Check the original value before using the proxy-transparent accessors:
   * a resolved moving proxy has a canonical hash and body, but retaining the
   * proxy itself in a store registry would leave a dangling heap pointer. */
  ax_assume(pl_tag(pin) == PL_TAG_PIN && pl_store_owns(s, pin),
            "%s: PIN is not store-owned", caller);
  pl_cell* p = pl_ptr(pin);
  ax_assume(pl_hdr_kind(p[0]) == PL_K_PIN && !pl_pin_is_proxy(p) &&
                (pl_hdr_flags(p[0]) & PL_F_PIN_HASHED) != 0 &&
                pl_hdr_cells(p[0]) == PL_PIN_CELLS(pl_hdr_meta(p[0])),
            "%s: PIN is not canonical", caller);
  ax_assume(pl_is_nat63((pl_val)p[5]) || pl_store_owns(s, (pl_val)p[5]),
            "%s: PIN body is not a closed store graph", caller);
  return p;
}

static void pl_store_index_hashed_law_locked(pl_store* s, pl_val pin) {
  pl_cell* p = pl_store_require_canonical_pin(s, pin, "index_hashed_law");
  pl_val body = (pl_val)p[5];
  ax_assume(pl_tag(body) == PL_TAG_LAW &&
                pl_hdr_kind(pl_ptr(body)[0]) == PL_K_LAW,
            "index_hashed_law: non-LAW PIN");
  pl_hash key;
  memcpy(key.b, pl_pin_hash_bytes(p), sizeof(key.b));
  ptrdiff_t at = ax_hmgeti(s->code_targets, key);
  if (at < 0) {
    pl_val* targets = NULL;
    ax_arrpush(targets, pin);
    ax_hmput(s->code_targets, key, targets);
  } else {
    ax_arrpush(s->code_targets[at].value, pin);
  }
  ptrdiff_t code_at = ax_hmgeti(s->code_cache, key);
  if (code_at >= 0)
    pl_pin_set_code(p, s->code_cache[code_at].value);
}

void pl_store_index_hashed_law(pl_store* s, pl_val pin) {
  pl_store_lock(s);
  pl_store_index_hashed_law_locked(s, pin);
  pl_store_unlock(s);
}

pl_val pl_store_intern_get(pl_store* s, const uint8_t hash[32]) {
  pl_store_lock(s);
  pl_hash k;
  memcpy(k.b, hash, 32);
  ptrdiff_t i = ax_hmgeti(s->intern, k);
  pl_val value = i < 0 ? 0 : s->intern[i].value;
  pl_store_unlock(s);
  return value;
}

void pl_store_intern_put(pl_store* s, const uint8_t hash[32], pl_val pin) {
  ax_assume(hash != NULL, "intern_put: NULL hash");
  pl_cell* p = pl_store_require_canonical_pin(s, pin, "intern_put");
  ax_assume(memcmp(hash, pl_pin_hash_bytes(p), 32) == 0,
            "intern_put: hash does not match canonical PIN");
  /* Registration keeps every canonical-PIN index in sync and rejects a
   * second representative for an existing hash. */
  pl_store_register_canonical(s, pin);
}

bool pl_store_backend_put(pl_store* s, const uint8_t hash[32], const uint8_t* b,
                          size_t n) {
  pl_store_save_lock(s);
  bool ok = s->be.put(s->be.ctx, hash, b, n);
  pl_store_save_unlock(s);
  return ok;
}

bool pl_store_backend_get(pl_store* s, const uint8_t hash[32], uint8_t** out_b,
                          size_t* out_s) {
  pl_store_save_lock(s);
  bool ok = s->be.get(s->be.ctx, hash, out_b, out_s);
  pl_store_save_unlock(s);
  return ok;
}

bool pl_store_put_root(pl_store* s, const uint8_t hash[32]) {
  pl_store_save_lock(s);
  bool ok = s->be.put_root(s->be.ctx, hash);
  pl_store_save_unlock(s);
  return ok;
}

bool pl_store_get_root(pl_store* s, uint8_t hash[32]) {
  pl_store_save_lock(s);
  bool ok = s->be.get_root(s->be.ctx, hash);
  pl_store_save_unlock(s);
  return ok;
}

/* ── Explicit non-persistent lifetime boundary ────────────────────────── */

typedef struct pl_snapshot_entry {
  pl_val key;
  pl_val value;
} pl_snapshot_entry;

static pl_val snapshot_copy(pl_store* s, pl_snapshot_entry** map, pl_val v) {
  if (pl_is_nat63(v))
    return v;

  ptrdiff_t hit = ax_hmgeti(*map, v);
  if (hit >= 0)
    return (*map)[hit].value;

  pl_cell* p = pl_ptr(v);
  if (pl_tag(v) == PL_TAG_PIN && pl_pin_is_proxy(p)) {
    pl_val target = pl_pin_proxy_target(p);
    if (target != 0) {
      ax_assume(pl_tag(target) == PL_TAG_PIN && pl_store_owns(s, target),
                "snapshot: proxy target is not a canonical store PIN");
      ax_hmput(*map, v, target);
      return target;
    }
    if (pl_store_owns(s, v))
      return v; /* an unresolved store proxy is already a closed graph */

    ax_assume(pl_hdr_cells(p[0]) == PL_PIN_CELLS(0),
              "snapshot: heap proxy has an inline PIN table");
    pl_val proxy = pl_store_mk_proxy(s, 0);
    ax_hmput(*map, v, proxy); /* before the body: preserve graph sharing */
    pl_ptr(proxy)[5] = snapshot_copy(s, map, (pl_val)p[5]);
    return proxy;
  }

  if (pl_store_owns(s, v))
    return v; /* all store graphs are closed by construction */

  pl_val out;
  switch (pl_tag(v)) {
  case PL_TAG_NAT: {
    uint32_t used = pl_nat_limbs(p);
    pl_cell* np = pl_store_alloc(s, PL_NAT_CELLS(used));
    np[0] = pl_hdr_make(PL_K_NAT, PL_F_NORMAL, used, PL_NAT_CELLS(used));
    memcpy(np + 1, pl_nat_limb_ptr(p), (size_t)used * sizeof(pl_cell));
    out = pl_make(PL_TAG_NAT, np);
    ax_hmput(*map, v, out);
    return out;
  }
  case PL_TAG_LAW: {
    pl_cell* np = pl_store_alloc(s, PL_LAW_CELLS);
    np[0] = pl_hdr_make(PL_K_LAW, PL_F_NORMAL, 0, PL_LAW_CELLS);
    np[1] = pl_law_arity(p);
    out = pl_make(PL_TAG_LAW, np);
    ax_hmput(*map, v, out);
    np[2] = snapshot_copy(s, map, pl_law_name(p));
    np[3] = snapshot_copy(s, map, pl_law_body(p));
    return out;
  }
  case PL_TAG_APP: {
    uint32_t n = pl_app_n(p);
    pl_cell* np = pl_store_alloc(s, PL_APP_CELLS(n));
    np[0] = pl_hdr_make(PL_K_APP, PL_F_NORMAL, pl_app_need(p), PL_APP_CELLS(n));
    out = pl_make(PL_TAG_APP, np);
    ax_hmput(*map, v, out);
    np[1] = snapshot_copy(s, map, pl_app_head(p));
    for (uint32_t i = 0; i < n; i++)
      np[2 + i] = snapshot_copy(s, map, pl_app_args(p)[i]);
    return out;
  }
  case PL_TAG_PIN:
    ax_abort("snapshot: non-proxy PIN outside the store");
  default:
    ax_abort("snapshot: non-normal value (tag 0x%llx)",
             (unsigned long long)pl_tag(v));
  }
}

pl_val pl_store_snapshot_normal(pl_thread* t, pl_val v) {
  pl_store* s = pl_heap_store(t->heap);
  ax_assume(s != NULL, "snapshot requires a store");
  ax_assume(pl_is_normal(v), "snapshot requires a normal value");
  if (pl_is_nat63(v))
    return v;
  if (pl_tag(v) != PL_TAG_PIN && pl_store_owns(s, v))
    return v;

  pl_snapshot_entry* map = NULL;
  pl_val out = snapshot_copy(s, &map, v);
  ax_hmfree(map);
  ax_assume(pl_is_nat63(out) || pl_store_owns(s, out),
            "snapshot produced a non-store value");
  return out;
}

void pl_store_put_code(pl_store* s, const uint8_t hash[32]) {
  pl_store_save_lock(s);
  pl_store_lock(s);
  if (!s->compiler_f || s->compiler_t == NULL) {
    pl_store_unlock(s);
    pl_store_save_unlock(s);
    return;
  }
  pl_hash key;
  memcpy(key.b, hash, sizeof(key.b));
  ptrdiff_t targets_at = ax_hmgeti(s->code_targets, key);
  if (targets_at < 0) {
    pl_store_unlock(s); /* only persistent LAW PINs execute bytecode */
    pl_store_save_unlock(s);
    return;
  }
  ptrdiff_t cached_at = ax_hmgeti(s->code_cache, key);
  if (cached_at >= 0) {
    pl_code* code = s->code_cache[cached_at].value;
    for (ptrdiff_t i = 0; i < ax_arrlen(s->code_targets[targets_at].value); i++)
      pl_pin_set_code(pl_ptr(s->code_targets[targets_at].value[i]), code);
    pl_store_unlock(s);
    pl_store_save_unlock(s);
    return;
  }
  pl_thread* t = s->compiler_t;
  uint8_t compiler_hash[32];
  memcpy(compiler_hash, s->compiler, 32);
  /* save_mu serializes this single PLAN machine and prevents replacement.
   * The machine's loads and snapshots take mu only around their own arena and
   * registry accesses, so do not retain the general lock while PLAN runs. */
  pl_store_unlock(s);
  /* the compiler is installed PLAN code: a compile failure must not
   * take the runtime down — the law just stays interpreted */
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    fprintf(stderr, "bytecode compile raised: %s\n",
            t->exn_msg != NULL ? t->exn_msg : "PLAN exn");
    pl_store_save_unlock(s);
    return;
  }
  pl_val compiler = pl_store_load(t, compiler_hash);
  pl_val fun = pl_store_load(t, hash);
  pl_val res = pl_apply(t, compiler, fun);
  /* Decoded OP_PUSH_LIT operands are retained in malloc-owned bytecode after
   * this compiler heap moves or is replaced.  Normalize and cross the explicit
   * store-lifetime boundary before the decoder borrows any pl_val pointers. */
  size_t result_at = t->vsp;
  pl_vpush(t, res);
  t->vstack[result_at] = pl_nf(t, t->vstack[result_at]);
  pl_val stable = pl_store_snapshot_normal(t, t->vstack[result_at]);
  t->vsp = result_at;
  pl_catch_pop(t, &c);
  pl_code* code = pl_bytecode_from_val(stable);
  if (code != NULL) {
    /* Attach the cached code to every canonical LAW registered for this hash.
     */
    pl_store_lock(s);
    cached_at = ax_hmgeti(s->code_cache, key);
    if (cached_at >= 0) {
      pl_bytecode_free(code); /* a re-entrant compile won the generation */
      code = s->code_cache[cached_at].value;
    } else {
      ax_arrpush(s->codes, code);
      ax_hmput(s->code_cache, key, code);
    }
    targets_at = ax_hmgeti(s->code_targets, key);
    ax_assume(targets_at >= 0, "compiled law left the hash index");
    for (ptrdiff_t i = 0; i < ax_arrlen(s->code_targets[targets_at].value); i++)
      pl_pin_set_code(pl_ptr(s->code_targets[targets_at].value[i]), code);
    pl_store_unlock(s);
  }
  pl_store_save_unlock(s);
}

/*
 * Compile every law pin already interned.  Canonical laws registered before
 * the compiler was installed (the boot prelude and loaded snapshots) would
 * otherwise never get bytecode.  Snapshot the hashes under the registry lock
 * so compilation can run afterward through the save_mu -> mu lock order.
 */
static void pl_store_compile_existing(pl_store* s) {
  pl_intern_entry* laws = NULL;
  pl_store_lock(s);
  for (ptrdiff_t i = 0; i < ax_arrlen(s->pins); i++) {
    pl_cell* p = pl_ptr(s->pins[i]);
    if ((pl_hdr_flags(p[0]) & PL_F_PIN_HASHED) == 0 ||
        pl_tag(pl_pin_body(p)) != PL_TAG_LAW)
      continue;
    pl_hash key;
    memcpy(key.b, pl_pin_hash_bytes(p), 32);
    if (ax_hmgeti(laws, key) < 0)
      ax_hmput(laws, key, s->pins[i]);
  }
  pl_store_unlock(s);
  for (ptrdiff_t i = 0; i < ax_hmlen(laws); i++)
    pl_store_put_code(s, laws[i].key.b);
  ax_hmfree(laws);
}

static void pl_store_invalidate_code_locked(pl_store* s) {
  for (ptrdiff_t i = 0; i < ax_arrlen(s->pins); i++)
    pl_pin_set_code(pl_ptr(s->pins[i]), NULL);
  ax_hmfree(s->code_cache);
  s->code_cache = NULL;
}

static void pl_store_free_code(pl_store* s) {
  for (ptrdiff_t i = 0; i < ax_arrlen(s->codes); i++)
    pl_bytecode_free(s->codes[i]);
  ax_arrfree(s->codes);
  s->codes = NULL;
}

bool pl_store_put_compiler(pl_store* s, const uint8_t hash[32]) {
  pl_store_save_lock(s);
  pl_store_lock(s);
  bool enabled = hash[0] ? memcmp(hash, hash + 1, 31) != 0 : true;
  if (s->compiler_f == enabled &&
      memcmp(s->compiler, hash, sizeof(s->compiler)) == 0) {
    pl_store_unlock(s);
    pl_store_save_unlock(s);
    return false;
  }

  pl_store_invalidate_code_locked(s);
  if (s->compiler_t != NULL) {
    pl_thread_free(s->compiler_t);
    s->compiler_t = NULL;
  }
  if (s->compiler_h != NULL) {
    pl_heap_free(s->compiler_h);
    s->compiler_h = NULL;
  }
  s->compiler_f = enabled;
  memcpy(s->compiler, hash, 32);
  bool sweep = s->compiler_f;
  if (sweep) {
    s->compiler_h = pl_heap_new(((size_t)1 << 26), s);
    s->compiler_t = pl_thread_new(s->compiler_h);
  }
  pl_store_unlock(s);
  pl_store_save_unlock(s);
  if (sweep) {
    fprintf(stderr, "store: installing bytecode compiler\r\n");
    pl_store_compile_existing(s);
  }
  return true;
}

/* ── Store-resident value construction (no GC interaction) ─────────────── */

static pl_val st_nat_small(pl_val n) {
  ax_assume(pl_is_nat63(n), "st_nat_small");
  return n;
}

static pl_val st_app(pl_store* s, pl_val head, uint32_t n, const pl_val* args) {
  pl_cell* p = pl_store_alloc(s, PL_APP_CELLS(n));
  uint64_t a = pl_arity(head);
  uint32_t need = (a == 0 || a <= n) ? 0 : (uint32_t)(a - n);
  p[0] = pl_hdr_make(PL_K_APP, PL_F_NORMAL, need, PL_APP_CELLS(n));
  p[1] = head;
  memcpy(p + 2, args, n * sizeof(pl_val));
  return pl_make(PL_TAG_APP, p);
}

pl_val pl_store_mk_proxy(pl_store* s, pl_val body) {
  ax_assume(pl_is_nat63(body) || pl_store_owns(s, body),
            "store proxy body is not closed");
  pl_cell* p = pl_store_alloc(s, PL_PIN_CELLS(0));
  p[0] =
      pl_hdr_make(PL_K_PIN, PL_F_NORMAL | PL_F_PIN_PROXY, 0, PL_PIN_CELLS(0));
  memset(p + 1, 0, 4 * sizeof(pl_cell));
  p[5] = body;
  __atomic_store_n(&p[6], (pl_cell)0, __ATOMIC_RELEASE);
  return pl_make(PL_TAG_PIN, p);
}

void pl_store_register_canonical(pl_store* s, pl_val pin) {
  pl_cell* p = pl_store_require_canonical_pin(s, pin, "register_canonical");

  pl_hash key;
  memcpy(key.b, pl_pin_hash_bytes(p), sizeof(key.b));
  pl_store_lock(s);
  ptrdiff_t at = ax_hmgeti(s->intern, key);
  if (at >= 0) {
    pl_val existing = s->intern[at].value;
    ax_assume(pl_store_owns(s, existing),
              "register_canonical: intern table retained a moving PIN");
    ax_assume(existing == pin,
              "register_canonical: duplicate canonical PIN for one hash");
    pl_store_unlock(s);
    return; /* idempotent registration of the same representative */
  }
  ax_hmput(s->intern, key, pin);
  ax_arrpush(s->pins, pin);
  if (pl_tag((pl_val)p[5]) == PL_TAG_LAW)
    pl_store_index_hashed_law_locked(s, pin);
  pl_store_unlock(s);
}

pl_val pl_store_mk_pin(pl_store* s, const uint8_t* hash, pl_val body,
                       uint32_t npins, const pl_val* subpins) {
  ax_assume(hash != NULL, "pl_store_mk_pin requires a canonical hash");
  ax_assume(npins <= PL_HDR_META_MAX,
            "canonical PIN exceeds the direct PIN-table limit");
  ax_assume(pl_is_nat63(body) || pl_store_owns(s, body),
            "canonical PIN body is not closed");
  pl_cell* p = pl_store_alloc(s, PL_PIN_CELLS(npins));
  p[0] = pl_hdr_make(PL_K_PIN, PL_F_NORMAL | PL_F_PIN_HASHED, npins,
                     PL_PIN_CELLS(npins));
  memcpy(p + 1, hash, 32);
  p[5] = body;
  pl_pin_set_code(p, NULL);
  if (npins > 0)
    memcpy(p + 7, subpins, npins * sizeof(pl_val));
  pl_val pin = pl_make(PL_TAG_PIN, p);
  for (uint32_t i = 0; i < npins; i++)
    ax_assume(pl_store_owns(s, subpins[i]),
              "canonical PIN table retained a moving PIN");
  pl_store_register_canonical(s, pin);
  return pin;
}

bool pl_pin_is_hashed(pl_val pin) {
  pl_cell* p = pl_as(PL_TAG_PIN, pin);
  if (p == NULL)
    return false;
  if (pl_pin_is_proxy(p)) {
    pin = pl_pin_proxy_target(p);
    if (pin == 0)
      return false;
    p = pl_as(PL_TAG_PIN, pin);
    ax_assume(p != NULL && !pl_pin_is_proxy(p),
              "resolved PIN proxy does not target a canonical PIN");
  }
  return (pl_hdr_flags(p[0]) & PL_F_PIN_HASHED) != 0;
}

const uint8_t* pl_pin_hash(pl_val pin) {
  pl_cell* p = pl_as(PL_TAG_PIN, pin);
  ax_assume(p != NULL, "pl_pin_hash on a non-pin");
  if (pl_pin_is_proxy(p)) {
    pin = pl_pin_proxy_target(p);
    if (pin == 0)
      return NULL;
    p = pl_as(PL_TAG_PIN, pin);
    ax_assume(p != NULL && !pl_pin_is_proxy(p),
              "resolved PIN proxy does not target a canonical PIN");
  }
  if ((pl_hdr_flags(p[0]) & PL_F_PIN_HASHED) == 0)
    return NULL;
  return pl_pin_hash_bytes(p);
}

/* ── Lazy Row machinery (op_row): ix0/ix1 body expressions ─────────────── */

/*
 * Element k of a lazy row is the unforced `Ix0 (Ix1^k xs)`.  We encode
 * those redexes as thunks over a 2-slot env [_, prefix] whose body is
 * the law-body expression
 *
 *   (0 (0 P66) (0 (0 "IxK") 1))   ==  apply(P66, apply("IxK", slot1))
 *
 * where P66 is the interned pin of nat 66.  Both expressions are built
 * once, store-resident, at first use.
 */
static pl_val st_ix_expr(pl_store* s, pl_val p66, uint64_t name) {
  pl_val q66 = st_app(s, 0, 1, &p66);    /* (0 P66)        */
  pl_val qname = st_app(s, 0, 1, &name); /* (0 "IxK")      */
  pl_val inner[2] = {qname, 1};          /* (0 (0 n) 1)    */
  pl_val row = st_app(s, 0, 2, inner);
  pl_val outer[2] = {q66, row}; /* (0 (0 P66) ..) */
  return st_app(s, 0, 2, outer);
}

static void pl_store_init_ix(pl_store* s) {
  pl_store_lock(s);
  if (s->ix0_expr != 0)
    goto done;
  pl_val p66 = pl_store_pin_of_nat(s, 66);
  s->ix0_expr = st_ix_expr(s, p66, ax_s3('I', 'x', '0'));
  s->ix1_expr = st_ix_expr(s, p66, ax_s3('I', 'x', '1'));
done:
  pl_store_unlock(s);
}

pl_val pl_store_ix0_expr(pl_store* s) {
  pl_store_init_ix(s);
  return s->ix0_expr;
}

pl_val pl_store_ix1_expr(pl_store* s) {
  pl_store_init_ix(s);
  return s->ix1_expr;
}

/* ── Store lifecycle ───────────────────────────────────────────────────── */

pl_store* pl_store_new(pl_store_backend backend) {
  pl_store* s = calloc(1, sizeof(*s));
  ax_assume(s != NULL, "oom");
  pthread_mutexattr_t attr;
  ax_assume(pthread_mutexattr_init(&attr) == 0, "pthread_mutexattr_init");
  ax_assume(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0,
            "pthread_mutexattr_settype");
  ax_assume(pthread_mutex_init(&s->save_mu, &attr) == 0,
            "save pthread_mutex_init");
  ax_assume(pthread_mutex_init(&s->mu, &attr) == 0, "pthread_mutex_init");
  pthread_mutexattr_destroy(&attr);
  s->region = ax_arena_create_overcommit(PL_STORE_REGION_BYTES);
  ax_assume(s->region != NULL, "store region reservation failed");
  s->lo = (uint8_t*)s->region;
  s->hi = (uint8_t*)ax_arena_end(s->region);
  ax_assume(((uintptr_t)s->hi & ~PL_ADDR_MASK) == 0,
            "store address exceeds 56 bits");
  s->intern = NULL;
  s->be = backend;
  return s;
}

void pl_store_free(pl_store* s) {
  if (s == NULL)
    return;
  if (s->compiler_t != NULL)
    pl_thread_free(s->compiler_t);
  if (s->compiler_h != NULL)
    pl_heap_free(s->compiler_h);
  pl_store_invalidate_code_locked(s);
  pl_store_free_code(s);
  for (ptrdiff_t i = 0; i < ax_hmlen(s->code_targets); i++)
    ax_arrfree(s->code_targets[i].value);
  ax_hmfree(s->code_targets);
  ax_hmfree(s->intern);
  ax_arrfree(s->pins);
  ax_arrfree(s->loading);
  if (s->be.close != NULL)
    s->be.close(s->be.ctx);
  ax_arena_destroy(s->region);
  pthread_mutex_destroy(&s->mu);
  pthread_mutex_destroy(&s->save_mu);
  free(s);
}

/* ── Memory backend ────────────────────────────────────────────────────── */

typedef struct mem_entry {
  pl_hash key;
  struct {
    uint8_t* b;
    size_t n;
  } value;
} mem_entry;

typedef struct mem_backend {
  mem_entry* map;
  uint8_t root[32];
  bool has_root;
} mem_backend;

static bool mem_get(void* ctx, const uint8_t hash[32], uint8_t** out_b,
                    size_t* out_s) {
  mem_backend* m = ctx;
  pl_hash k;
  memcpy(k.b, hash, 32);
  ptrdiff_t i = ax_hmgeti(m->map, k);
  if (i < 0)
    return false;
  *out_b = malloc(m->map[i].value.n);
  ax_assume(*out_b != NULL, "oom");
  memcpy(*out_b, m->map[i].value.b, m->map[i].value.n);
  *out_s = m->map[i].value.n;
  return true;
}

static bool mem_put(void* ctx, const uint8_t hash[32], const uint8_t* b,
                    size_t n) {
  mem_backend* m = ctx;
  pl_hash k;
  memcpy(k.b, hash, 32);
  if (ax_hmgeti(m->map, k) >= 0)
    return true;
  uint8_t* copy = malloc(n);
  ax_assume(copy != NULL, "oom");
  memcpy(copy, b, n);
  ax_hmput(m->map, k, ((typeof(m->map[0].value)){copy, n}));
  return true;
}

static bool mem_has(void* ctx, const uint8_t hash[32]) {
  mem_backend* m = ctx;
  pl_hash k;
  memcpy(k.b, hash, 32);
  return ax_hmgeti(m->map, k) >= 0;
}

static bool mem_put_root(void* ctx, const uint8_t hash[32]) {
  mem_backend* m = ctx;
  memcpy(m->root, hash, 32);
  m->has_root = true;
  return true;
}

static bool mem_get_root(void* ctx, uint8_t hash[32]) {
  mem_backend* m = ctx;
  if (!m->has_root)
    return false;
  memcpy(hash, m->root, 32);
  return true;
}

static void mem_close(void* ctx) {
  mem_backend* m = ctx;
  for (ptrdiff_t i = 0; i < ax_hmlen(m->map); i++)
    free(m->map[i].value.b);
  ax_hmfree(m->map);
  free(m);
}

pl_store* pl_store_new_mem(void) {
  mem_backend* m = calloc(1, sizeof(*m));
  ax_assume(m != NULL, "oom");
  return pl_store_new((pl_store_backend){
      .ctx = m,
      .get = mem_get,
      .put = mem_put,
      .has = mem_has,
      .put_root = mem_put_root,
      .get_root = mem_get_root,
      .close = mem_close,
  });
}

/* ── LMDB backend ──────────────────────────────────────────────────────── */

typedef struct lmdb_backend {
  MDB_env* env;
  MDB_dbi dbi;
} lmdb_backend;

static const uint8_t pl_root_key[32] = {'r', 'o', 'o', 't'};

static bool lmdb_put_kv(lmdb_backend* l, const uint8_t key[32],
                        const uint8_t* b, size_t n) {
  MDB_txn* txn;
  if (mdb_txn_begin(l->env, NULL, 0, &txn) != 0)
    return false;
  MDB_val k = {32, (void*)key};
  MDB_val v = {n, (void*)b};
  if (mdb_put(txn, l->dbi, &k, &v, 0) != 0) {
    mdb_txn_abort(txn);
    return false;
  }
  return mdb_txn_commit(txn) == 0;
}

static bool lmdb_get_kv(lmdb_backend* l, const uint8_t key[32], uint8_t** out_b,
                        size_t* out_s) {
  MDB_txn* txn;
  if (mdb_txn_begin(l->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;
  MDB_val k = {32, (void*)key};
  MDB_val v;
  if (mdb_get(txn, l->dbi, &k, &v) != 0) {
    mdb_txn_abort(txn);
    return false;
  }
  *out_b = malloc(v.mv_size);
  ax_assume(*out_b != NULL, "oom");
  memcpy(*out_b, v.mv_data, v.mv_size);
  *out_s = v.mv_size;
  mdb_txn_abort(txn);
  return true;
}

static bool lmdb_get(void* ctx, const uint8_t hash[32], uint8_t** out_b,
                     size_t* out_s) {
  return lmdb_get_kv(ctx, hash, out_b, out_s);
}

static bool lmdb_put(void* ctx, const uint8_t hash[32], const uint8_t* b,
                     size_t n) {
  return lmdb_put_kv(ctx, hash, b, n);
}

static bool lmdb_has(void* ctx, const uint8_t hash[32]) {
  uint8_t* b;
  size_t n;
  if (!lmdb_get_kv(ctx, hash, &b, &n))
    return false;
  free(b);
  return true;
}

static bool lmdb_put_root(void* ctx, const uint8_t hash[32]) {
  return lmdb_put_kv(ctx, pl_root_key, hash, 32);
}

static bool lmdb_get_root(void* ctx, uint8_t hash[32]) {
  uint8_t* b;
  size_t n;
  if (!lmdb_get_kv(ctx, pl_root_key, &b, &n))
    return false;
  bool ok = n == 32;
  if (ok)
    memcpy(hash, b, 32);
  free(b);
  return ok;
}

static void lmdb_close(void* ctx) {
  lmdb_backend* l = ctx;
  mdb_dbi_close(l->env, l->dbi);
  mdb_env_close(l->env);
  free(l);
}

pl_store* pl_store_new_lmdb(const char* path, size_t map_size) {
  lmdb_backend* l = calloc(1, sizeof(*l));
  if (l == NULL)
    return NULL;
  if (mdb_env_create(&l->env) != 0) {
    free(l);
    return NULL;
  }
  if (mdb_env_set_mapsize(l->env, map_size) != 0 ||
      mdb_env_open(l->env, path, 0, 0664) != 0) {
    mdb_env_close(l->env);
    free(l);
    return NULL;
  }
  MDB_txn* txn;
  if (mdb_txn_begin(l->env, NULL, 0, &txn) != 0 ||
      mdb_dbi_open(txn, NULL, MDB_CREATE, &l->dbi) != 0 ||
      mdb_txn_commit(txn) != 0) {
    mdb_env_close(l->env);
    free(l);
    return NULL;
  }
  return pl_store_new((pl_store_backend){
      .ctx = l,
      .get = lmdb_get,
      .put = lmdb_put,
      .has = lmdb_has,
      .put_root = lmdb_put_root,
      .get_root = lmdb_get_root,
      .close = lmdb_close,
  });
}
