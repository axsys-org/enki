#include "store_internal.h"

#include <string.h>

struct pl_silo_batch { int unused; };

static bool fail(char* error, size_t capacity) {
  const char* message = "Silo unsupported on enki-os";
  if (error != NULL && capacity != 0) {
    size_t size = strlen(message);
    if (size >= capacity) size = capacity - 1;
    memcpy(error, message, size);
    error[size] = '\0';
  }
  return false;
}

bool pl_store_silo_batch_begin(pl_store* store, pl_silo_batch** out,
                               char* error, size_t capacity) {
  (void)store; (void)out; return fail(error, capacity);
}
bool pl_store_silo_batch_contains(pl_silo_batch* batch, const uint8_t hash[32],
                                  bool* out, char* error, size_t capacity) {
  (void)batch; (void)hash; (void)out; return fail(error, capacity);
}
bool pl_store_silo_batch_put(pl_silo_batch* batch, const uint8_t hash[32],
                             const uint8_t* bytes, size_t size,
                             char* error, size_t capacity) {
  (void)batch; (void)hash; (void)bytes; (void)size; return fail(error, capacity);
}
bool pl_store_silo_batch_commit(pl_silo_batch* batch,
                                const uint8_t root_hash[32],
                                char* error, size_t capacity) {
  (void)batch; (void)root_hash; return fail(error, capacity);
}
void pl_store_silo_batch_abort(pl_silo_batch* batch) { (void)batch; }
bool pl_store_silo_open(pl_store* store, const uint8_t hash[32],
                        pl_silo_reader* out, char* error, size_t capacity) {
  (void)store; (void)hash; (void)out; return fail(error, capacity);
}
void pl_store_silo_close_reader(pl_silo_reader* reader) { (void)reader; }
pl_store* pl_store_new_silo(const char* path, size_t map_size) {
  (void)path; (void)map_size; return NULL;
}
