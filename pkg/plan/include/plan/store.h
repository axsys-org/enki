#ifndef PL_STORE_H
#define PL_STORE_H

/*
 * Pins and the content-addressed store.
 *
 * Two tiers behind one interface:
 *   - the store region: a non-moving arena holding pinned closures in
 *     object format, directly traversable by the evaluator.  Store-region
 *     objects reference only store-region objects (the closure
 *     invariant); the collector treats store addresses as terminal.
 *   - the persistence backend: content-addressed byte storage keyed by
 *     pin hash.  Production backend is LMDB; a memory backend exists for
 *     tests and for linking libplan without LMDB.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "axsys/arena.h"
#include "plan/heap.h"
#include "plan/value.h"
#include "plan/bytecode.h"

typedef struct pl_hash {
  uint8_t b[32];
} pl_hash;

typedef enum pl_store_format {
  PL_STORE_FORMAT_LEGACY_V1 = 0,
  PL_STORE_FORMAT_SILO_V1 = 1,
} pl_store_format;

typedef struct pl_store_backend {
  void* ctx;
  /* get returns a malloc'd buffer the caller frees; false if missing. */
  bool (*get)(void* ctx, const uint8_t hash[32], uint8_t** out_b,
              size_t* out_s);
  bool (*put)(void* ctx, const uint8_t hash[32], const uint8_t* b, size_t s);
  bool (*has)(void* ctx, const uint8_t hash[32]);
  bool (*put_root)(void* ctx, const uint8_t hash[32]);
  bool (*get_root)(void* ctx, uint8_t hash[32]);
  void (*close)(void* ctx);
  void (*get_code)(void* ctx, const uint8_t hash[32], pl_code** out);
  void (*put_code)(void* ctx, const uint8_t hash[32], pl_code* out);
} pl_store_backend;

typedef struct pl_intern_entry {
  pl_hash key;
  pl_val value;
} pl_intern_entry;

/* Canonical store LAW PINs grouped by persistent hash.  This lets a decoded
 * compiler result be attached to every registered target for that hash. */
typedef struct pl_code_targets_entry {
  pl_hash key;
  pl_val* value;
} pl_code_targets_entry;

/* Decoded code for the active compiler generation.  The pointed-to code is
 * owned by pl_store.codes, including after this generation is retired. */
typedef struct pl_code_cache_entry {
  pl_hash key;
  pl_code* value;
} pl_code_cache_entry;

typedef struct pl_store {
  /* Serializes persistence transactions and Save publication.  Always acquire
   * this before `mu` on paths that need both locks. */
  pthread_mutex_t save_mu;
  pthread_mutex_t mu;
  ax_arena* region;
  uint8_t* lo;
  uint8_t* hi;
  pl_intern_entry* intern;             /* stb_ds hashmap: hash -> PIN val */
  pl_code_targets_entry* code_targets; /* law hash -> all runtime PINs */
  pl_code_cache_entry* code_cache;     /* active generation: hash -> code */
  pl_val* pins;                        /* canonical hashed store PINs only */
  /* All decoded code owned by the store.  Replaced compiler generations are
   * retained until store teardown because suspended evaluator frames can
   * still hold raw pointers into an old generation. */
  pl_code** codes;
  pl_hash* loading; /* active Silo loads; cycle detection */
  pl_store_backend be;
  pl_store_format format;
  pl_val ix0_expr, ix1_expr;
  uint8_t compiler[32];
  pl_thread* compiler_t;
  pl_heap* compiler_h;
  uint64_t pin_profile_us;
  bool compiler_f;
  bool pin_profile_f;
} pl_store;

pl_store* pl_store_new(pl_store_backend backend);
pl_store* pl_store_new_mem(void);
/* NULL on failure (path must be an existing directory). */
pl_store* pl_store_new_lmdb(const char* path, size_t map_size);
/* Canonical Silo streams in pins.pack, indexed by LMDB. */
pl_store* pl_store_new_silo(const char* path, size_t map_size);
void pl_store_free(pl_store* s);

/* Print each newly frozen PIN whose exclusive serialization and store
 * insertion time exceeds `threshold_us`. */
void pl_store_profile_pins(pl_store* s, uint64_t threshold_us);

/* Address-range test used by the collector (store vals are terminal).  The
 * region bounds are immutable after construction, and this runs for every
 * non-immediate edge visited by GC. */
static inline bool pl_store_owns(const pl_store* s, pl_val v) {
  uintptr_t p = (uintptr_t)pl_ptr(v);
  return p >= (uintptr_t)s->lo && p < (uintptr_t)s->hi;
}

/*
 * Normalize a value and wrap it in a fixed-size moving-heap PIN proxy.  This
 * is deliberately cheap and non-persistent: Save later promotes the reachable
 * PIN closure into canonical store objects and resolves each proxy in place.
 */
pl_val pl_pin(pl_thread* t, pl_val v);

/*
 * Copy a normal value owned by `t` into the non-moving store region.  The
 * returned graph is closed over that region and may therefore outlive or be
 * shared independently of the caller's heap.  This is a lifetime boundary,
 * not a persistence boundary: it does not hash, intern, compile, or publish a
 * root.  `v` must be reachable from a registered root for the duration of the
 * call.
 */
pl_val pl_store_snapshot_normal(pl_thread* t, pl_val v);

/* True when a PIN has a persistent content hash. */
bool pl_pin_is_hashed(pl_val pin);

/* Hash bytes of a PIN value (32 bytes, borrowed), or NULL while unresolved. */
const uint8_t* pl_pin_hash(pl_val pin);

/* Finalize and persist a PIN closure, resolve its proxies, and publish root.
 * An unresolved moving proxy must belong to a heap whose store is `s`; this
 * keeps its eventual canonical target within the collector's store-lifetime
 * domain.  Store-owned and already-canonical PINs must likewise belong to
 * `s`. */
bool pl_store_save_root(pl_store* s, pl_val pin, uint8_t out_hash[32],
                        char* err, size_t err_cap);

/* Intern-or-load a pin by hash; raises if the backend lacks it. */
pl_val pl_store_load(pl_thread* t, const uint8_t hash[32]);

/* Persist / fetch the root hash (event-log replay seam). */
bool pl_store_put_root(pl_store* s, const uint8_t hash[32]);
bool pl_store_get_root(pl_store* s, uint8_t hash[32]);

/* Runtime singletons used by lazy Row construction (see op.c). */
pl_val pl_store_ix0_expr(pl_store* s);
pl_val pl_store_ix1_expr(pl_store* s);

/* Compile the law pin `hash` with the installed compiler and cache the
 * result on the pin itself (read back via pl_pin_code). */
void pl_store_put_code(pl_store* s, const uint8_t hash[32]);

/* Install a compiler generation.  Returns false when the same hash is already
 * installed and no work was performed. */
bool pl_store_put_compiler(pl_store* s, const uint8_t hash[32]);

/* Register a LAW PIN after its persistent hash becomes visible. */
void pl_store_index_hashed_law(pl_store* s, pl_val pin);
#endif
