#ifndef ENKI_GC_H
#define ENKI_GC_H

#include <stddef.h>

#include "enki/allocator.h"
#include "enki/arena.h"
#include "enki/value.h"

#ifdef __cplusplus
extern "C" {
#endif

struct enki_gc {
    enki_arena*    active;
    enki_arena*    idle;
    enki_value     root;
    size_t         cap;
    enki_allocator sys;
};

enki_gc*    enki_gc_create (enki_allocator sys, size_t cap, enki_value root);
void        enki_gc_destroy(enki_gc* gc);

void*       enki_gc_alloc  (enki_gc* gc, size_t size);   /* auto-collects on full */
void        enki_gc_collect(enki_gc* gc);
enki_value  enki_gc_mark   (enki_gc* gc, enki_value val); /* copy-or-forward */

#ifdef __cplusplus
}
#endif

#endif
