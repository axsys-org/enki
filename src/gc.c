#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/value.h"

enki_gc* enki_gc_create(enki_allocator sys_a, size_t cap_s, enki_interpreter* root) {
    enki_gc* gc = sys_a.alloc(sys_a.ctx, sizeof(enki_gc));
    if (!gc) return NULL;
    gc->sys_a = sys_a;
    gc->cap_s = cap_s;
    gc->root = root;
    gc->copy = enki_gc_copy;
    gc->alloc = enki_gc_alloc; 
    gc->active_a = enki_arena_create(sys_a, cap_s);
    gc->idle_a = enki_arena_create(sys_a, cap_s);
    if (!gc->active_a || !gc->idle_a) {
        enki_gc_destroy(gc);
        return NULL;
    }
    return gc;
}


void enki_gc_destroy(enki_gc* gc) {
    if (!gc) return;
    enki_arena_destroy(gc->idle_a);
    enki_arena_destroy(gc->active_a);
    gc->sys_a.free(gc->sys_a.ctx, gc);
}

void* enki_gc_alloc(enki_gc* gc, size_t size_s) {
    if (!gc) return NULL;
    void* new = enki_arena_alloc(gc->active_a, size_s);
    if (!new) {
        enki_gc_collect(gc);
        new = enki_arena_alloc(gc->active_a, size_s);
        if (!new) return NULL; 
    }
    return new;
}

enki_value enki_gc_copy(enki_gc* gc, enki_value val_v) {
    if(!IS_PTR(val_v)) return val_v;
    enki_value_header* h = ENKI_TO_PTR(val_v);
    if(h->kind_b == ENKI_FRWD) {
        return PTR_TO_ENKI(*(void**)GET_PAYLOAD(val_v));
    }
    void* new = enki_arena_alloc(gc->active_a, h->size_s);
    void* payload = GET_PAYLOAD(val_v);
    memcpy(new, ENKI_TO_PTR(val_v), h->size_s);
    h->kind_b = ENKI_FRWD;
    memcpy(payload, &new, sizeof(new));
    return PTR_TO_ENKI(new);
}

void enki_gc_collect(enki_gc* gc) {
    if (!gc) return;
    enki_arena* temp = gc->active_a;
    gc->active_a = gc->idle_a;
    gc->idle_a = temp;
    enki_trace_interp(gc->root);
    size_t scan_o = sizeof(enki_arena);
    while (scan_o < gc->active_a->off_o) {
        void* obj = (gc->active_a->ptr + scan_o);
        enki_value_header* h = obj; 
        enki_trace_value(gc, obj);
        scan_o += h->size_s;
    }
    enki_arena_reset(gc->idle_a);
}
