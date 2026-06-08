#pragma once

#include <stddef.h>
#include <stdint.h>
#include <lmdb.h>
#include "enki/error.h"
#include "enki/run.h"

typedef struct enki_store enki_store;

struct enki_store {
  size_t size_s;
  MDB_env* env;
  MDB_dbi dbi;

  enki_error (*read)(enki_store* store, const uint8_t* key_b, uint8_t* bytes_b,
                     size_t cap_s, size_t* len_s);

  enki_error (*write)(enki_store* store, const uint8_t* key_b,
                      const uint8_t* bytes_b, size_t len_s);

  enki_error (*size)(enki_store* store, const uint8_t* key_b, size_t* len_s);
};

enki_error enki_store_init(const char* path, size_t size_s, enki_store* store);
enki_error enki_store_size(enki_store* store, const uint8_t* key_b,
                           size_t* len_s);
enki_error enki_store_read(enki_store* store, const uint8_t* key_b,
                           uint8_t* bytes_b, size_t cap_s, size_t* len_s);
enki_error enki_store_write(enki_store* store, const uint8_t* key_b,
                            const uint8_t* bytes_b, size_t len_s);
void enki_store_close(enki_store* store);

enki_error er_store_write_root_hash(enki_store* store,
                                    const uint8_t hash_b[32]);
enki_error er_store_save_pin(enki_store* store, enki_gc* gc,
                             const enki_allocator* work_a, er_val pin_v,
                             uint8_t hash_b[32]);
enki_error er_store_load_pin(enki_store* store, enki_gc* gc,
                             const enki_allocator* work_a,
                             const uint8_t hash_b[32], er_val* out_v);
enki_error er_store_save_root(enki_store* store, enki_gc* gc,
                              const enki_allocator* work_a, er_val pin_v);
enki_error er_store_load_root(enki_store* store, enki_gc* gc,
                              const enki_allocator* work_a, er_val* out_v);
