#ifndef ENKI_ARENA_H
#define ENKI_ARENA_H

#include <stddef.h>

#include "enki/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct enki_arena {
    unsigned char* ptr;
    size_t         cap_s;
    size_t         off_o;
    enki_allocator sys_a;
} enki_arena;

enki_arena*    enki_arena_create(enki_allocator parent_a, size_t cap_s);
void           enki_arena_destroy(enki_arena* a);
void*          enki_arena_alloc(void* ctx, size_t size_s);
void           enki_arena_free(void* ctx, void* ptr);   // no-op_b
void           enki_arena_reset(enki_arena* a);

enki_allocator enki_tmp_allocator();
#define EA_TMP_ALLOC enki_tmp_allocator()

enki_allocator enki_arena_as_allocator(enki_arena* a);

void* enki_arena_start(enki_arena* a);
void* enki_arena_end(enki_arena* a);

#ifdef __cplusplus
}
#endif

#endif
