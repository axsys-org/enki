#include "enki/arena.h"


enki_arena* enki_arena_create(enki_allocator sys, size_t cap){
    size_t siz = sizeof(enki_arena) + cap;
    enki_arena* a = (enki_arena*)sys.alloc(sys.ctx, siz);
    if(!a) {
        return NULL;
    }
    a->off = sizeof(enki_arena);
    a->ptr = (unsigned char*)a; 
    a->cap = siz;
    a->sys = sys;
    return a;
}
void enki_arena_destroy(enki_arena* a) {
    if(!a) return;
    a->sys.free(a->sys.ctx, a);
}   
void* enki_arena_alloc(void* ctx, size_t size) {
    enki_arena* a = ctx;
    if(!a) return NULL; 
    if((a->off + size) > a->cap) {
        return NULL;
    }
    size_t old = a->off;
    a->off += size; 
    return (void*)(a->ptr + old);
}

void enki_arena_free(void* ctx, void* ptr) {
    (void)ctx;
    (void)ptr;
    return;
}

void enki_arena_reset(enki_arena* a) {
    if(!a) return;
    a->off = sizeof(enki_arena);
}   

enki_allocator enki_arena_as_allocator(enki_arena* a) {

    if(!a) return (enki_allocator){0};
    
    return (enki_allocator){ 
        .ctx = a, 
        .alloc = enki_arena_alloc, 
        .realloc = NULL, 
        .free = enki_arena_free, 
    };
}                                                               


void* enki_arena_start(enki_arena* a) {
    if(!a) return NULL;
    return (void*)(a->ptr + sizeof(enki_arena));
}

void* enki_arena_end(enki_arena* a) {
    if(!a) return NULL;
    return (void*)(a->ptr + a->cap);
}





