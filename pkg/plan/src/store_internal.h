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

typedef enum pl_store_lock_kind {
  PL_STORE_LOCK_GENERAL,
  PL_STORE_LOCK_SAVE,
  PL_STORE_LOCK_KIND_COUNT,
} pl_store_lock_kind;

typedef enum pl_store_lock_context {
  PL_STORE_LOCK_CONTEXT_OTHER,
  PL_STORE_LOCK_CONTEXT_SAVE_SILO,
  PL_STORE_LOCK_CONTEXT_SAVE_LEGACY,
  PL_STORE_LOCK_CONTEXT_LOAD_SILO,
  PL_STORE_LOCK_CONTEXT_LOAD_LEGACY,
  PL_STORE_LOCK_CONTEXT_COMPILE,
  PL_STORE_LOCK_CONTEXT_COMPILE_EXISTING,
  PL_STORE_LOCK_CONTEXT_COMPILER_INSTALL,
  PL_STORE_LOCK_CONTEXT_SNAPSHOT,
  PL_STORE_LOCK_CONTEXT_ACTOR_MESSAGE,
  PL_STORE_LOCK_CONTEXT_LAZY_ROW,
  PL_STORE_LOCK_CONTEXT_DIRECT_IO,
  PL_STORE_LOCK_CONTEXT_COUNT,
} pl_store_lock_context;

typedef enum pl_store_lock_site {
  PL_STORE_LOCK_SITE_ARENA_ALLOC,
  PL_STORE_LOCK_SITE_ARENA_MARK,
  PL_STORE_LOCK_SITE_ARENA_RELEASE,
  PL_STORE_LOCK_SITE_LAW_INDEX,
  PL_STORE_LOCK_SITE_INTERN_GET,
  PL_STORE_LOCK_SITE_BACKEND_PUT,
  PL_STORE_LOCK_SITE_BACKEND_GET,
  PL_STORE_LOCK_SITE_ROOT_PUT,
  PL_STORE_LOCK_SITE_ROOT_GET,
  PL_STORE_LOCK_SITE_CODE_LOOKUP,
  PL_STORE_LOCK_SITE_CODE_PUBLISH,
  PL_STORE_LOCK_SITE_COMPILE_EXISTING_SCAN,
  PL_STORE_LOCK_SITE_COMPILER_INSTALL,
  PL_STORE_LOCK_SITE_CANONICAL_REGISTER,
  PL_STORE_LOCK_SITE_LAZY_ROW_INIT,
  PL_STORE_LOCK_SITE_SAVE_ROOT,
  PL_STORE_LOCK_SITE_SAVE_PUBLISH,
  PL_STORE_LOCK_SITE_LOAD,
  PL_STORE_LOCK_SITE_SILO_LOAD_CLOSURE,
  PL_STORE_LOCK_SITE_LEGACY_LOAD_PUBLISH,
  PL_STORE_LOCK_SITE_INSTALL_SELF_CHECK,
  PL_STORE_LOCK_SITE_COUNT,
} pl_store_lock_site;

typedef struct pl_store_lock_context_scope {
  pl_store_lock_context previous;
  pl_store* previous_store;
  bool active;
} pl_store_lock_context_scope;

pl_store_lock_context_scope
pl_store_lock_context_begin(pl_store* s, pl_store_lock_context context);
void pl_store_lock_context_end(pl_store_lock_context_scope* scope);

#define PL_STORE_LOCK_JOIN2(a, b) a##b
#define PL_STORE_LOCK_JOIN(a, b)  PL_STORE_LOCK_JOIN2(a, b)
#define PL_STORE_LOCK_CONTEXT(store, context)                                  \
  __attribute__((                                                              \
      cleanup(pl_store_lock_context_end))) pl_store_lock_context_scope         \
  PL_STORE_LOCK_JOIN(pl_store_lock_context_scope_, __LINE__) =                 \
      pl_store_lock_context_begin(store, context)

/* Chrome Trace span shared by store.c and the serializer in pin.c. */
pl_store_profile_scope pl_store_profile_begin(const char* name, size_t name_n);
void pl_store_profile_end(pl_store_profile_scope* scope);

pl_cell* pl_store_alloc(pl_store* s, size_t cells);
size_t pl_store_mark(pl_store* s);
void pl_store_release(pl_store* s, size_t mark);

void pl_store_lock(pl_store* s, pl_store_lock_site site);
void pl_store_unlock(pl_store* s);
bool pl_store_trylock(pl_store* s, pl_store_lock_site site);
void pl_store_save_lock(pl_store* s, pl_store_lock_site site);
void pl_store_save_unlock(pl_store* s);

pl_val pl_store_intern_get(pl_store* s, const uint8_t hash[32]);
void pl_store_intern_put(pl_store* s, const uint8_t hash[32], pl_val pin);

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
