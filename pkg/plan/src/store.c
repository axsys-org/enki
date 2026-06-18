#include "plan/store.h"

#include <lmdb.h>
#include <stdlib.h>
#include <string.h>

#include "axsys/arena.h"
#include "axsys/assume.h"
#include "axsys/stb_ds.h"
#include "axsys/util.h"
#include "internal.h"
#include "plan/build.h"
#include "store_internal.h"

/* Default reserved (overcommitted) size of the store region. */
#ifndef PL_STORE_REGION_BYTES
#define PL_STORE_REGION_BYTES (((size_t)1) << 38)
#endif

typedef struct pl_intern_entry {
  pl_hash key;
  pl_val value;
} pl_intern_entry;

typedef struct pl_code_entry {
  pl_hash key;
  pl_code* value;
} pl_code_entry;

struct pl_store {
  ax_arena* region;
  uint8_t* lo;
  uint8_t* hi;
  pl_intern_entry* intern; /* stb_ds hashmap: hash -> PIN val */
  pl_code_entry* code;     /* stb_ds hashmap: hash -> bytecode */
  pl_store_backend be;
  pl_val ix0_expr, ix1_expr;
  uint8_t compiler[32];
  bool compiler_f;
};

/* ── Region allocation ─────────────────────────────────────────────────── */

pl_cell* pl_store_alloc(pl_store* s, size_t cells) {
  void* p = ax_arena_alloc_aligned(s->region, cells * sizeof(pl_cell), 8);
  ax_assume(p != NULL, "store region exhausted");
  return p;
}

size_t pl_store_mark(pl_store* s) {
  return s->region->off_o;
}

void pl_store_release(pl_store* s, size_t mark) {
  s->region->off_o = mark;
}

bool pl_store_owns(pl_store* s, pl_val v) {
  uint8_t* p = (uint8_t*)pl_ptr(v);
  return p >= s->lo && p < s->hi;
}

/* ── Intern table ──────────────────────────────────────────────────────── */

pl_val pl_store_intern_get(pl_store* s, const uint8_t hash[32]) {
  pl_hash k;
  memcpy(k.b, hash, 32);
  ptrdiff_t i = hmgeti(s->intern, k);
  return i < 0 ? 0 : s->intern[i].value;
}

void pl_store_intern_put(pl_store* s, const uint8_t hash[32], pl_val pin) {
  pl_hash k;
  memcpy(k.b, hash, 32);
  hmput(s->intern, k, pin);
}

bool pl_store_backend_put(pl_store* s, const uint8_t hash[32], const uint8_t* b,
                          size_t n) {
  return s->be.put(s->be.ctx, hash, b, n);
}

bool pl_store_backend_get(pl_store* s, const uint8_t hash[32], uint8_t** out_b,
                          size_t* out_s) {
  return s->be.get(s->be.ctx, hash, out_b, out_s);
}

bool pl_store_put_root(pl_store* s, const uint8_t hash[32]) {
  return s->be.put_root(s->be.ctx, hash);
}

bool pl_store_get_root(pl_store* s, uint8_t hash[32]) {
  return s->be.get_root(s->be.ctx, hash);
}

bool pl_store_get_code(pl_store* s, const uint8_t hash[32], pl_code** out) {
  pl_hash k;
  memcpy(k.b, hash, 32);
  ptrdiff_t i = hmgeti(s->code, k);
  if (i < 0)
    return false;
  *out = s->code[i].value;
  ax_assume(*out != NULL, "oom");
  return true;
}

void pl_store_put_code(pl_thread* t, const uint8_t hash[32]) {
  pl_store* s = pl_heap_store(t->heap);
  pl_hash k;
  if (!s->compiler_f) {
    fprintf(stderr, "no compiler set! failing compile\n");
    return;
  }
  PL_GC_FORBID(t);
  pl_val compiler = pl_store_load(t, s->compiler);
  pl_val fun = pl_store_load(t, hash);
  pl_val res = pl_apply(t, compiler, fun);
  pl_val pin = pl_pin(t, &res);
  pl_cell* p = pl_as(PL_TAG_PIN, pin);
  ax_assume(p, "wack");
  pl_code* code = pl_bytecode_from_val(pl_pin_body(p));
  PL_GC_ALLOW(t);
  if (code != NULL) {
    memcpy(k.b, hash, 32);
    hmput(s->code, k, code);
  }
}

void pl_store_put_compiler(pl_store* s, const uint8_t hash[32]) {
  s->compiler_f = hash[0] ? memcmp(hash, hash + 1, 31) != 0 : true;
  memcpy(s->compiler, hash, 32);
  // moar leaks, TODO: fix
  hmfree(s->code);
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

pl_val pl_store_mk_pin(pl_store* s, const uint8_t hash[32], pl_val body,
                       uint32_t npins, const pl_val* subpins) {
  pl_cell* p = pl_store_alloc(s, PL_PIN_CELLS(npins));
  p[0] = pl_hdr_make(PL_K_PIN, PL_F_NORMAL, npins, PL_PIN_CELLS(npins));
  memcpy(p + 1, hash, 32);
  p[5] = body;
  if (npins > 0)
    memcpy(p + 6, subpins, npins * sizeof(pl_val));
  return pl_make(PL_TAG_PIN, p);
}

const uint8_t* pl_pin_hash(pl_val pin) {
  pl_cell* p = pl_as(PL_TAG_PIN, pin);
  ax_assume(p != NULL, "pl_pin_hash on a non-pin");
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
  if (s->ix0_expr != 0)
    return;
  pl_val p66 = pl_store_pin_of_nat(s, 66);
  s->ix0_expr = st_ix_expr(s, p66, ax_s3('I', 'x', '0'));
  s->ix1_expr = st_ix_expr(s, p66, ax_s3('I', 'x', '1'));
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
  if (s->be.close != NULL)
    s->be.close(s->be.ctx);
  hmfree(s->intern);
  ax_arena_destroy(s->region);
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
  ptrdiff_t i = hmgeti(m->map, k);
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
  if (hmgeti(m->map, k) >= 0)
    return true;
  uint8_t* copy = malloc(n);
  ax_assume(copy != NULL, "oom");
  memcpy(copy, b, n);
  hmput(m->map, k, ((typeof(m->map[0].value)){copy, n}));
  return true;
}

static bool mem_has(void* ctx, const uint8_t hash[32]) {
  mem_backend* m = ctx;
  pl_hash k;
  memcpy(k.b, hash, 32);
  return hmgeti(m->map, k) >= 0;
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
  for (ptrdiff_t i = 0; i < hmlen(m->map); i++)
    free(m->map[i].value.b);
  hmfree(m->map);
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
