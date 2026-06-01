#pragma once

#include <stddef.h>

#include "enki/allocator.h"
#include "enki/arena.h"
#include "enki/run.h"

typedef struct enki_interpreter enki_interpreter;
typedef struct enki_gc enki_gc;

typedef void (*enki_gc_trace_fn)(enki_gc* gc, void* root);

void* enki_gc_alloc(enki_gc* gc, size_t size_s, size_t align_s);
void* enki_gc_alloc_locked(enki_gc* gc, size_t size_s, size_t align_s);
er_val enki_gc_copy(enki_gc* gc, er_val val_v);

struct enki_gc {
    enki_arena* active_a;
    enki_arena* idle_a;
    size_t cap_s;
    enki_allocator our_a;
    enki_allocator allocator_a;
    size_t lock_depth;

    /*
     * Kept for existing old-interpreter allocation code that still records
     * stats through gc->root. The collector itself traces er roots through
     * trace_root/trace_fn below.
     */
    enki_interpreter* root;

    void* trace_root;
    enki_gc_trace_fn trace_fn;

    er_val* work_v;
    size_t work_s;
    size_t work_i;
    size_t work_cap_s;

    er_val (*copy)(enki_gc* gc, er_val val_v);
    void* (*alloc)(enki_gc* gc, size_t size_s, size_t align_s);
};

enki_gc* enki_gc_create(const enki_allocator* our_a, size_t cap_s, enki_interpreter* root);
void enki_gc_destroy(enki_gc* gc);
void enki_gc_lock(enki_gc* gc);
void enki_gc_unlock(enki_gc* gc);
void enki_gc_collect(enki_gc* gc);

void enki_gc_set_trace_root(enki_gc* gc, void* root, enki_gc_trace_fn trace_fn);
void enki_gc_trace_vm(enki_gc* gc, void* root);
const enki_allocator* enki_gc_as_allocator(enki_gc* gc);
const enki_allocator* enki_gc_parent_allocator(enki_gc* gc);
enki_gc* enki_gc_from_allocator(const enki_allocator* allocator);
