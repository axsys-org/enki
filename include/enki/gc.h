#pragma once
#include <stddef.h>

#include "enki/allocator.h"
#include "enki/arena.h"
#include "enki/value.h"

typedef struct enki_interpreter enki_interpreter;

typedef struct {
    void* key;
    enki_value value;
} enki_import_ent;

typedef struct {
    enki_interpreter* dst_i;
    enki_import_ent* seen;
    enki_value* work;
} enki_import_ctx;

void* enki_gc_alloc (enki_gc* gc, size_t size_s, size_t align_s);
void* enki_gc_alloc_locked(enki_gc* gc, size_t size_s, size_t align_s);
enki_value enki_gc_copy (enki_gc* gc, enki_value val_v);
enki_value enki_gc_import(enki_interpreter* dst_i, enki_value root_v);

struct enki_gc {
    enki_arena* active_a;
    enki_arena* idle_a;
    enki_interpreter* root;
    size_t cap_s;
    enki_allocator our_a;
    size_t lock_depth;
    enki_value (*copy)(enki_gc* gc, enki_value val_v);
    void* (*alloc)(enki_gc* gc, size_t size_s, size_t align_s);
};

enki_gc* enki_gc_create (const enki_allocator* our_a, size_t cap_s, enki_interpreter* root);
void enki_gc_destroy(enki_gc* gc);
void enki_gc_lock(enki_gc* gc);
void enki_gc_unlock(enki_gc* gc);
void enki_gc_collect(enki_gc* gc);
