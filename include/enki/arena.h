#ifndef ENKI_ARENA_H
#define ENKI_ARENA_H

#include <stddef.h>

#include "enki/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct enki_arena {
    unsigned char* ptr;
    size_t         cap;
    size_t         off;
    enki_allocator sys;
} enki_arena;

enki_arena*    enki_arena_create(enki_allocator parent, size_t cap);
void           enki_arena_destroy(enki_arena* a);
void*          enki_arena_alloc(void* ctx, size_t size);
void           enki_arena_free(void* ctx, void* ptr);   // no-op
void           enki_arena_reset(enki_arena* a);

enki_allocator enki_arena_as_allocator(enki_arena* a);

void* enki_arena_start(enki_arena* a);
void* enki_arena_end(enki_arena* a);

#ifdef __cplusplus
}
#endif

#endif
