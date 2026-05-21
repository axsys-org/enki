#pragma once
#include <stddef.h>

#include "enki/allocator.h"
#include "enki/arena.h"
#include "enki/value.h"

typedef struct enki_interpreter enki_interpreter;

void* enki_gc_alloc (enki_gc* gc, size_t size);
enki_value enki_gc_copy (enki_gc* gc, enki_value val);

struct enki_gc {
    enki_arena* active;
    enki_arena* idle;
    enki_interpreter* root;
    size_t cap;
    enki_allocator sys;
    enki_value (*copy)(enki_gc* gc, enki_value val);  
    void* (*alloc)(enki_gc* gc, size_t size);    
};

enki_gc* enki_gc_create (enki_allocator sys, size_t cap, enki_interpreter* root);
void enki_gc_destroy(enki_gc* gc);
void enki_gc_collect(enki_gc* gc);
