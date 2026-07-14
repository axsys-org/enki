#ifndef PL_SILO_INTERNAL_H
#define PL_SILO_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plan/store.h"
#include "plan/value.h"

#define PL_SILO_MAX_DEPTH     4096u
#define PL_SILO_MAX_PIN_COUNT ((UINT32_C(1) << 20) - 1u)
#define PL_SILO_MAX_NAT_BYTES ((uint64_t)PL_SILO_MAX_PIN_COUNT * 8u)
#define PL_SILO_LEAF_BYTES    1024u
#define PL_SILO_PAGE_BYTES    65536u
#define PL_SILO_NAT_CUTOVER   8192u

typedef struct pl_silo_writer {
  void* ctx;
  bool (*write)(void* ctx, const uint8_t* bytes, size_t len);
  uint64_t pos;
} pl_silo_writer;

typedef struct pl_silo_reader {
  void* ctx;
  bool (*read)(void* ctx, uint8_t* bytes, size_t len);
  bool (*rewind)(void* ctx);
  uint64_t len;
  uint64_t pos;
} pl_silo_reader;

typedef struct pl_silo_scan {
  pl_hash* pins;
  size_t pin_count;
  uint32_t* used; /* first-occurrence indices into pins */
  size_t used_count;
} pl_silo_scan;

/* Canonical revision-5 writer.  subpins must be the direct, unique,
 * first-occurrence pin list for root. */
bool pl_silo_encode(pl_silo_writer* w, pl_val root, const pl_val* subpins,
                    size_t nsub, char* err, size_t err_cap);

/* General-v1 validation.  When canonical is true, also enforce the unique
 * canonical profile.  The scan owns malloc'd pins/used arrays until freed. */
bool pl_silo_scan_stream(pl_silo_reader* r, bool canonical, pl_silo_scan* scan,
                         char* err, size_t err_cap);
void pl_silo_scan_free(pl_silo_scan* scan);

/* Re-read and materialize a stream already accepted by scan_stream.
 * resolved has scan->pin_count entries; only scan->used entries are read. */
bool pl_silo_build_stream(pl_silo_reader* r, pl_store* store,
                          const pl_silo_scan* scan, const pl_val* resolved,
                          pl_val* out, char* err, size_t err_cap);

#endif
