#ifndef PL_STORE_INTERNAL_H
#define PL_STORE_INTERNAL_H

/* Shared between store.c (region, backends) and pin.c (canonize, copy). */

#include "plan/store.h"
#include "plan/value.h"

typedef struct pl_hash {
  uint8_t b[32];
} pl_hash;

pl_cell* pl_store_alloc(pl_store* s, size_t cells);
size_t pl_store_mark(pl_store* s);
void pl_store_release(pl_store* s, size_t mark);

pl_val pl_store_intern_get(pl_store* s, const uint8_t hash[32]);
void pl_store_intern_put(pl_store* s, const uint8_t hash[32], pl_val pin);

bool pl_store_backend_put(pl_store* s, const uint8_t hash[32],
                          const uint8_t* b, size_t n);
bool pl_store_backend_get(pl_store* s, const uint8_t hash[32], uint8_t** out_b,
                          size_t* out_s);

pl_val pl_store_mk_pin(pl_store* s, const uint8_t hash[32], pl_val body,
                       uint32_t npins, const pl_val* subpins);

/* Intern the pin of a small nat (used for the op-66 row exprs). */
pl_val pl_store_pin_of_nat(pl_store* s, uint64_t n);

#endif
