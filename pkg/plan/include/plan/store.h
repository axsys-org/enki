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
#include <stdint.h>

#include "plan/heap.h"
#include "plan/value.h"
#include "plan/bytecode.h"

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

pl_store* pl_store_new(pl_store_backend backend);
pl_store* pl_store_new_mem(void);
/* NULL on failure (path must be an existing directory). */
pl_store* pl_store_new_lmdb(const char* path, size_t map_size);
void pl_store_free(pl_store* s);

/* Address-range test used by the collector (store vals are terminal). */
bool pl_store_owns(pl_store* s, pl_val v);

/*
 * Pin a value: nf, canonize + SHA-256, intern.  Returns the
 * (store-resident, deduplicated) PIN.  v is rooted internally for the
 * normalization; the caller's copy may be stale afterwards and should
 * be re-fetched from a root if reused.
 */
pl_val pl_pin(pl_thread* t, pl_val v);

/* Hash bytes of a PIN value (32 bytes, borrowed). */
const uint8_t* pl_pin_hash(pl_val pin);

/* Intern-or-load a pin by hash; raises if the backend lacks it. */
pl_val pl_store_load(pl_thread* t, const uint8_t hash[32]);

/* Persist / fetch the root hash (event-log replay seam). */
bool pl_store_put_root(pl_store* s, const uint8_t hash[32]);
bool pl_store_get_root(pl_store* s, uint8_t hash[32]);

/* Runtime singletons used by lazy Row construction (see op.c). */
pl_val pl_store_ix0_expr(pl_store* s);
pl_val pl_store_ix1_expr(pl_store* s);

/* bytecode manipulation */
void pl_store_put_code(pl_thread* t, const uint8_t hash[32]);
bool pl_store_get_code(pl_store* s, const uint8_t hash[32], pl_code** out);

void pl_store_put_compiler(pl_store* s, const uint8_t hash[32]);
#endif
