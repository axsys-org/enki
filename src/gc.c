#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/profile.h"
#include "enki/trace.h"
#include "enki/value.h"

static size_t enki_gc_align_offset(size_t off_o) {
    size_t align_s = _Alignof(enki_value_header);
    return (off_o + align_s - 1) & ~(align_s - 1);
}

enki_gc* enki_gc_create(const enki_allocator* loc_a, size_t cap_s, enki_interpreter* root) {
    if(!loc_a) return NULL;
    enki_gc* gc = loc_a->alloc(loc_a->ctx, sizeof(enki_gc));
    if (!gc) return NULL;
    gc->our_a = *loc_a;
    gc->cap_s = cap_s;
    gc->lock_depth = 0;
    gc->root = root;
    gc->copy = enki_gc_copy;
    gc->alloc = enki_gc_alloc;
    gc->active_a = enki_arena_create(loc_a, cap_s);
    gc->idle_a = enki_arena_create(loc_a, cap_s);
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
    gc->our_a.free(gc->our_a.ctx, gc);
}

void* enki_gc_alloc(enki_gc* gc, size_t size_s, size_t align_s) {
    ENKI_PROFILE_ZONE("enki_gc_alloc");
    if (!gc) abort();
    void* new = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
    if (!new) {
        gc->root->stats.gc_alloc_fail_s++;
        if(gc->lock_depth > 0) {
            enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
        }
        enki_gc_collect(gc);
        new = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
        if (!new) {
            gc->root->stats.gc_alloc_fail_s++;
            enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
        }
    }
    gc->root->stats.gc_alloc_s++;
    gc->root->stats.gc_alloc_bytes_s += size_s;
    size_t live_s = gc->active_a->off_o - sizeof(enki_arena);
    if(live_s > gc->root->stats.gc_high_water_bytes_s) {
        gc->root->stats.gc_high_water_bytes_s = live_s;
    }
    return new;
}

void* enki_gc_alloc_locked(enki_gc* gc, size_t size_s, size_t align_s) {
    ENKI_PROFILE_ZONE("enki_gc_alloc_locked");
    if(!gc) abort();
    enki_gc_lock(gc);
    void* new = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
    enki_gc_unlock(gc);
    if(!new) {
        gc->root->stats.gc_alloc_fail_s++;
        enki_interp_throw(gc->root, ENKI_ERROR_OOM, 0);
    }
    gc->root->stats.gc_locked_alloc_s++;
    gc->root->stats.gc_locked_alloc_bytes_s += size_s;
    size_t live_s = gc->active_a->off_o - sizeof(enki_arena);
    if(live_s > gc->root->stats.gc_high_water_bytes_s) {
        gc->root->stats.gc_high_water_bytes_s = live_s;
    }
    return new;
}

enki_value enki_gc_copy(enki_gc* gc, enki_value val_v) {
    ENKI_PROFILE_ZONE("enki_gc_copy");
    if(!IS_PTR(val_v)) return val_v;
    enki_value_header* h = ENKI_AS(enki_value_header, val_v);
    if(h->kind_b == ENKI_FRWD) {
        return PTR_TO_ENKI(*(void**)GET_PAYLOAD(val_v));
    }
    void* new = enki_arena_alloc_aligned(gc->active_a, h->size_s, _Alignof(enki_value_header));
    if(!new) abort();
    gc->root->stats.gc_copy_s++;
    gc->root->stats.gc_copy_bytes_s += h->size_s;
    size_t live_s = gc->active_a->off_o - sizeof(enki_arena);
    if(live_s > gc->root->stats.gc_high_water_bytes_s) {
        gc->root->stats.gc_high_water_bytes_s = live_s;
    }
    void* payload = GET_PAYLOAD(val_v);
    memcpy(new, ENKI_AS(void, val_v), h->size_s);
    h->kind_b = ENKI_FRWD;
    memcpy(payload, &new, sizeof(new));
    return PTR_TO_ENKI(new);
}

void enki_gc_collect(enki_gc* gc) {
    ENKI_PROFILE_ZONE("enki_gc_collect");
    if (!gc) return;
    gc->root->stats.gc_collect_s++;
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
    gc->root->stats.gc_live_bytes_s = gc->active_a->off_o - sizeof(enki_arena);
    if(gc->root->stats.gc_live_bytes_s > gc->root->stats.gc_high_water_bytes_s) {
        gc->root->stats.gc_high_water_bytes_s = gc->root->stats.gc_live_bytes_s;
    }
    enki_arena_reset(gc->idle_a);
}
