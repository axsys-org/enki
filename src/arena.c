#include "enki/arena.h"


enki_arena* enki_arena_create(enki_allocator sys_a, size_t cap_s){
    size_t siz_s = sizeof(enki_arena) + cap_s;
    enki_arena* a = (enki_arena*)sys_a.alloc(sys_a.ctx, siz_s);
    if(!a) {
        return NULL;
    }
    a->off_o = sizeof(enki_arena);
    a->ptr = (unsigned char*)a; 
    a->cap_s = siz_s;
    a->sys_a = sys_a;
    return a;
}
void enki_arena_destroy(enki_arena* a) {
    if(!a) return;
    a->sys_a.free(a->sys_a.ctx, a);
}   
void* enki_arena_alloc(void* ctx, size_t size_s) {
    enki_arena* a = ctx;
    if(!a) return NULL;
    size_t align_s = _Alignof(max_align_t);
    size_t old_o = (a->off_o + align_s - 1) & ~(align_s - 1);
    if((old_o + size_s) > a->cap_s) {
        return NULL;
    }
    a->off_o = old_o + size_s; 
    return (void*)(a->ptr + old_o);
}

void enki_arena_free(void* ctx, void* ptr) {
    (void)ctx;
    (void)ptr;
    return;
}

void enki_arena_reset(enki_arena* a) {
    if(!a) return;
    a->off_o = sizeof(enki_arena);
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
    return (void*)(a->ptr + a->cap_s);
}





