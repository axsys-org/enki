#pragma once
#include <stddef.h>

#include "enki/allocator.h"
#include "enki/arena.h"
#include "enki/value.h"

typedef struct enki_interpreter enki_interpreter;

void* enki_gc_alloc (enki_gc* gc, size_t size_s);
enki_value enki_gc_copy (enki_gc* gc, enki_value val_v);

struct enki_gc {
    enki_arena* active_a;
    enki_arena* idle_a;
    enki_interpreter* root;
    size_t cap_s;
    enki_allocator sys_a;
    enki_value (*copy)(enki_gc* gc, enki_value val_v);  
    void* (*alloc)(enki_gc* gc, size_t size_s);    
};

enki_gc* enki_gc_create (enki_allocator sys_a, size_t cap_s, enki_interpreter* root);
void enki_gc_destroy(enki_gc* gc);
void enki_gc_collect(enki_gc* gc);
