#pragma once

#include <stddef.h>
#include <stdint.h>

#include "enki/interp.h"
#include "enki/value.h"
#include "enki/vector.h"

enki_value enki_pin_alloc(enki_gc* gc, const
    uint8_t hash_b[32], enki_value inner_v, size_t n_subpins_s, enki_value subpins_v[]);
enki_value enki_pin_deserialize(enki_interpreter* i, uint8_t* buff, size_t n_buff, size_t* off);
void enki_pin_serialize(enki_interpreter* i, enki_value val, enki_vector* out);
void enki_pin_save_root(enki_interpreter* i, enki_value val);
enki_value enki_pin_load_root(enki_interpreter* i);
void enki_pin_hash(enki_interpreter* i, enki_value val, uint8_t* hash_b);
void enki_pin_save(enki_interpreter* i, enki_value val, uint8_t* hash_b);
enki_value enki_pin_load(enki_interpreter* i, uint8_t* hash);
void enki_pin_collect_subpins(enki_interpreter* i, enki_value inner, enki_vector* subpins_v);


enki_value enki_make_pin(enki_gc* gc, enki_value val_v);
