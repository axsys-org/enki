#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enki/gc.h"
#include "enki/value.h"

enki_gc* enki_gc_create(enki_allocator sys, size_t cap, enki_interpreter* root) {
    enki_gc* gc = sys.alloc(sys.ctx, sizeof(enki_gc));
    if (!gc) return NULL;
    gc->sys = sys;
    gc->cap = cap;
    gc->root = root;
    gc->copy = enki_gc_copy;
    gc->alloc = enki_gc_alloc; 
    gc->active = enki_arena_create(sys, cap);
    gc->idle = enki_arena_create(sys, cap);
    if (!gc->active || !gc->idle) {
        enki_gc_destroy(gc);
        return NULL;
    }
    return gc;
}

void enki_gc_destroy(enki_gc* gc) {
    if (!gc) return;
    enki_arena_destroy(gc->idle);
    enki_arena_destroy(gc->active);
    gc->sys.free(gc->sys.ctx, gc);
}

void* enki_gc_alloc(enki_gc* gc, size_t size) {
    if (!gc) return NULL;
    void* new = enki_arena_alloc(gc->active, size);
    if (!new) {
        enki_gc_collect(gc);
        new = enki_arena_alloc(gc->active, size);
        if (!new) return NULL; 
    }
    return new;
}

enki_value enki_gc_copy(enki_gc* gc, enki_value val) {
    if(!IS_PTR(val)) return val;
    enki_value_header* h = ENKI_TO_PTR(val);
    if(h->kind == ENKI_FRWD) {
        return PTR_TO_ENKI(*(void**)GET_PAYLOAD(val));
    }
    void* new = enki_arena_alloc(gc->active, h->size);
    void* payload = GET_PAYLOAD(val);
    memcpy(new, ENKI_TO_PTR(val), h->size);
    h->kind = ENKI_FRWD;
    memcpy(payload, &new, sizeof(new));
    return PTR_TO_ENKI(new);
}

void enki_gc_collect(enki_gc* gc) {
    if (!gc) return;
    enki_arena* temp = gc->active;
    gc->active = gc->idle;
    gc->idle = temp;
    enki_trace_interp(gc->root);
    size_t scan = sizeof(enki_arena);
    while (scan < gc->active->off) {
        void* obj = (gc->active->ptr + scan);
        enki_value_header* h = obj; 
        enki_trace_value(gc, obj);
        scan += h->size;
    }
    enki_arena_reset(gc->idle);
}