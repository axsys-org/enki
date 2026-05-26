#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/trace.h"
#include "enki/value.h"

static size_t enki_gc_align_offset(size_t off_o) {
    size_t align_s = _Alignof(enki_value_header);
    return (off_o + align_s - 1) & ~(align_s - 1);
}

enki_gc* enki_gc_create(const enki_allocator* sys_a, size_t cap_s, enki_interpreter* root) {
    if(!sys_a) return NULL;
    enki_gc* gc = sys_a->alloc(sys_a->ctx, sizeof(enki_gc));
    if (!gc) return NULL;
    gc->sys_a = *sys_a;
    gc->cap_s = cap_s;
    gc->lock_depth = 0;
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

void enki_gc_lock(enki_gc* gc) {
    if(!gc) abort();
    gc->lock_depth++;
}

void enki_gc_unlock(enki_gc* gc) {
    if(!gc || gc->lock_depth == 0) abort();
    gc->lock_depth--;
}

void enki_gc_destroy(enki_gc* gc) {
    if (!gc) return;
    enki_arena_destroy(gc->idle_a);
    enki_arena_destroy(gc->active_a);
    gc->sys_a.free(gc->sys_a.ctx, gc);
}

void* enki_gc_alloc(enki_gc* gc, size_t size_s, size_t align_s) {
    if (!gc) abort();
    void* new = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
    if (!new) {
        if(gc->lock_depth > 0) {
            enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
        }
        enki_gc_collect(gc);
        new = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
        if (!new) {
            enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
        }
    }
    return new;
}

void* enki_gc_alloc_locked(enki_gc* gc, size_t size_s, size_t align_s) {
    if(!gc) abort();
    enki_gc_lock(gc);
    void* new = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
    enki_gc_unlock(gc);
    if(!new) {
        enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
    }
    return new;
}

enki_value enki_gc_copy(enki_gc* gc, enki_value val_v) {
    if(!IS_PTR(val_v)) return val_v;
    enki_value_header* h = ENKI_AS(enki_value_header, val_v);
    if(h->kind_b == ENKI_FRWD) {
        return PTR_TO_ENKI(*(void**)GET_PAYLOAD(val_v));
    }
    void* new = enki_arena_alloc_aligned(gc->active_a, h->size_s, _Alignof(enki_value_header));
    if(!new) abort();
    void* payload = GET_PAYLOAD(val_v);
    memcpy(new, ENKI_AS(void, val_v), h->size_s);
    h->kind_b = ENKI_FRWD;
    memcpy(payload, &new, sizeof(new));
    return PTR_TO_ENKI(new);
}

void enki_gc_collect(enki_gc* gc) {
    if (!gc) return;
    if(gc->lock_depth > 0) {
        enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
    }
    enki_arena* temp = gc->active_a;
    gc->active_a = gc->idle_a;
    gc->idle_a = temp;
    enki_trace_interp(gc->root);
    size_t scan_o = sizeof(enki_arena);
    while (scan_o < gc->active_a->off_o) {
        void* obj = (gc->active_a->ptr + scan_o);
        enki_value_header* h = obj; 
        enki_trace_value(gc, obj);
        scan_o = enki_gc_align_offset(scan_o + h->size_s);
    }
    enki_arena_reset(gc->idle_a);
}
