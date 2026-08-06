#ifndef PL_STORE_INTERNAL_H
#define PL_STORE_INTERNAL_H

/* Shared between store.c (region, backends) and pin.c (canonize, copy). */

#include "plan/store.h"
#include "plan/value.h"
#include "silo_internal.h"

typedef struct pl_store_profile_scope {
  uint64_t lane;
  uint64_t span;
  const uint8_t* name;
  size_t name_n;
  bool active;
} pl_store_profile_scope;

/* Chrome Trace span shared by store.c and the serializer in pin.c. */
pl_store_profile_scope pl_store_profile_begin(const char* name, size_t name_n);
void pl_store_profile_end(pl_store_profile_scope* scope);

/* Scoped span: ends at the end of the enclosing block. */
#define PL_STORE_PROFILE(name)                                                 \
  __attribute__((cleanup(pl_store_profile_end))) pl_store_profile_scope        \
      pl_store_profile_scope_ = pl_store_profile_begin(name, sizeof(name) - 1)

pl_cell* pl_store_alloc(pl_store* s, size_t cells);
size_t pl_store_mark(pl_store* s);
void pl_store_release(pl_store* s, size_t mark);

void pl_store_lock(pl_store* s);
void pl_store_unlock(pl_store* s);
bool pl_store_trylock(pl_store* s);
void pl_store_save_lock(pl_store* s);
void pl_store_save_unlock(pl_store* s);

pl_val pl_store_intern_get(pl_store* s, const uint8_t hash[32]);
void pl_store_intern_put(pl_store* s, const uint8_t hash[32], pl_val pin);

/* ── Compiler tiers (store.c) and the staging worker (stage.c) ──────────
 *
 * `opt` selects the tier a code row belongs to: false is the fast
 * generation compiled on the interning thread, true the optimized
 * generation compiled by the staging worker.  The optimized tier wins
 * wherever both have a row for the same law. */

/* Key of the persistent (compiler, law) -> code-row cache. */
void pl_store_code_key(const uint8_t compiler_hash[32],
                       const uint8_t law_hash[32], uint8_t out[32]);

/* True when `tier` already holds code for this law. */
bool pl_store_code_have(pl_store* s, const uint8_t law_hash[32], bool opt);

/* Serve the law from the per-snap and machine-global caches, publishing the
 * decoded row into the tier.  Row loads run on `t`; the caller holds save_mu
 * and owns `t`.  True when the law was served (the compiler can be skipped). */
bool pl_store_code_serve(pl_store* s, pl_thread* t, const uint8_t law_hash[32],
                         const uint8_t cache_key[32], bool opt);

/* Publish decoded code into a tier and attach it to every registered PIN.
 * Consumes `code`: a tier that already has a row for this law frees it and
 * keeps the existing pointer (suspended frames may hold it). */
void pl_store_code_publish(pl_store* s, const uint8_t law_hash[32],
                           pl_code* code, bool opt);

/* Persist the compiled row in `t`'s vstack slot `row_slot` under `cache_key`,
 * in both cache layers.  Best-effort; the caller holds save_mu. */
void pl_store_code_persist(pl_store* s, pl_thread* t, size_t row_slot,
                           const uint8_t cache_key[32]);

/* Queue a law for background upgrade; no-op when staging is inactive or the
 * law has already been queued for this optimizing generation. */
void pl_stage_enqueue(pl_store* s, const uint8_t law_hash[32]);

/* Record that a law needs no upgrade (its optimized row was already in the
 * cache), so it is never queued for this generation. */
void pl_stage_mark_done(pl_store* s, const uint8_t law_hash[32]);

/* Stop the workers, drop queued work, and forget the generation.  Called
 * whenever a compiler generation changes: the queue describes work for a
 * compiler that is no longer installed.  Never call it from a worker — that
 * would wait for the caller to go idle. */
void pl_stage_reset(pl_store* s);

/* True on a staging worker thread.  Ops that quiesce the stage refuse to
 * run there rather than deadlocking against themselves. */
bool pl_stage_in_worker(void);

/* Stop and free the workers (store teardown).  In-flight compiles are
 * abandoned at the next quantum boundary rather than waited out. */
void pl_stage_shutdown(pl_store* s);

bool pl_store_backend_put(pl_store* s, const uint8_t hash[32], const uint8_t* b,
                          size_t n);
bool pl_store_backend_get(pl_store* s, const uint8_t hash[32], uint8_t** out_b,
                          size_t* out_s);

typedef struct pl_silo_batch pl_silo_batch;

bool pl_store_silo_batch_begin(pl_store* s, pl_silo_batch** out, char* err,
                               size_t err_cap);
bool pl_store_silo_batch_contains(pl_silo_batch* batch, const uint8_t hash[32],
                                  bool* out, char* err, size_t err_cap);
bool pl_store_silo_batch_put(pl_silo_batch* batch, const uint8_t hash[32],
                             const uint8_t* bytes, size_t len, char* err,
                             size_t err_cap);
/* Commit consumes batch on both success and failure. */
bool pl_store_silo_batch_commit(pl_silo_batch* batch,
                                const uint8_t root_hash[32], char* err,
                                size_t err_cap);
void pl_store_silo_batch_abort(pl_silo_batch* batch);
bool pl_store_silo_open(pl_store* s, const uint8_t hash[32],
                        pl_silo_reader* out, char* err, size_t err_cap);
void pl_store_silo_close_reader(pl_silo_reader* r);
bool pl_store_load_silo_stream(pl_thread* t, pl_store* s, const uint8_t* bytes,
                               size_t len, pl_val* out, char* err,
                               size_t err_cap);

pl_val pl_store_mk_pin(pl_store* s, const uint8_t* hash, pl_val body,
                       uint32_t npins, const pl_val* subpins);

/* Fixed-size, closed, non-persistent PIN proxy.  Store proxies are deliberately
 * absent from the intern table, canonical PIN list, and code indexes. */
pl_val pl_store_mk_proxy(pl_store* s, pl_val body);

/* Publish a fully initialized canonical store PIN in the runtime indexes. */
void pl_store_register_canonical(pl_store* s, pl_val pin);

/* Intern the pin of a small nat (used for the op-66 row exprs). */
pl_val pl_store_pin_of_nat(pl_store* s, uint64_t n);

#endif
