#include "store_internal.h"

#include "axsys/util.h"

/* Persistent Silo storage is a native LMDB/pack-file backend. Browser stores
 * remain memory-backed; mounted Silo streams are decoded by pin.c/silo.c. */

bool pl_store_silo_batch_begin(pl_store* s, pl_silo_batch** out, char* err,
                               size_t err_cap) {
  AX_UNUSED(s);
  AX_UNUSED(out);
  AX_UNUSED(err);
  AX_UNUSED(err_cap);
  return false;
}

bool pl_store_silo_batch_contains(pl_silo_batch* batch, const uint8_t hash[32],
                                  bool* out, char* err, size_t err_cap) {
  AX_UNUSED(batch);
  AX_UNUSED(hash);
  AX_UNUSED(out);
  AX_UNUSED(err);
  AX_UNUSED(err_cap);
  return false;
}

bool pl_store_silo_batch_put(pl_silo_batch* batch, const uint8_t hash[32],
                             const uint8_t* bytes, size_t len, char* err,
                             size_t err_cap) {
  AX_UNUSED(batch);
  AX_UNUSED(hash);
  AX_UNUSED(bytes);
  AX_UNUSED(len);
  AX_UNUSED(err);
  AX_UNUSED(err_cap);
  return false;
}

bool pl_store_silo_batch_commit(pl_silo_batch* batch,
                                const uint8_t root_hash[32], char* err,
                                size_t err_cap) {
  AX_UNUSED(batch);
  AX_UNUSED(root_hash);
  AX_UNUSED(err);
  AX_UNUSED(err_cap);
  return false;
}

void pl_store_silo_batch_abort(pl_silo_batch* batch) {
  AX_UNUSED(batch);
}

bool pl_store_silo_open(pl_store* s, const uint8_t hash[32],
                        pl_silo_reader* out, char* err, size_t err_cap) {
  AX_UNUSED(s);
  AX_UNUSED(hash);
  AX_UNUSED(out);
  AX_UNUSED(err);
  AX_UNUSED(err_cap);
  return false;
}

void pl_store_silo_close_reader(pl_silo_reader* r) {
  AX_UNUSED(r);
}

pl_store* pl_store_new_silo(const char* path, size_t map_size) {
  AX_UNUSED(path);
  AX_UNUSED(map_size);
  return NULL;
}
