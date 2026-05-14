#include "enki/arena.h"

enki_arena* enki_arena_create(enki_allocator sys, size_t cap) {
    size_t total = sizeof(enki_arena) + cap;
    enki_arena* a = sys.alloc(sys.ctx, total);
    if (!a) {
        return NULL;
    }
    a->ptr = (unsigned char*)a;
    a->cap = total;
    a->off = sizeof(enki_arena);
    a->sys = sys;
    return a;
}

void enki_arena_destroy(enki_arena* a) {
    if (!a) return;
    a->sys.free(a->sys.ctx, a);
}

void* enki_arena_alloc(void* ctx, size_t size) {
    if (!ctx) return NULL;
    enki_arena* a = ctx;
    if (a->off + size > a->cap) {
        return NULL;
    }
    size_t old = a->off;
    a->off += size;
    return a->ptr + old;
}

void enki_arena_free(void* ctx, void* ptr) {
    (void)ctx;
    (void)ptr;
}

void enki_arena_reset(enki_arena* a) {
    if (!a) return;
    a->off = sizeof(enki_arena);
}

enki_allocator enki_arena_as_allocator(enki_arena* a) {
    if (!a) return (enki_allocator){0};

    return (enki_allocator){
        .ctx     = a,
        .alloc   = enki_arena_alloc,
        .realloc = NULL,
        .free    = enki_arena_free,
    };
}
