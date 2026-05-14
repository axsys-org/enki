#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enki/gc.h"
#include "enki/value.h"

enki_gc* enki_gc_create(enki_allocator sys, size_t cap, enki_value root) {
    enki_gc* gc = sys.alloc(sys.ctx, sizeof(enki_gc));
    if (!gc) return NULL;

    gc->sys    = sys;
    gc->cap    = cap;
    gc->root   = root;
    gc->active = enki_arena_create(sys, cap);
    gc->idle   = enki_arena_create(sys, cap);

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
        if (!new) return NULL;          /* true OOM */
    }
    return new;
}

enki_value enki_gc_mark(enki_gc* gc, enki_value val) {
    if (val == 0)    return 0;          /* bootstrap: no root yet */
    if (IS_IMM(val)) return val;        /* immediate, nothing to copy */

    enki_value_header* h = AS_PTR(val);

    if (h->kind == FORWARDED) {
        return MAKE_PTR(((enki_fwd*)h)->fwd);
    }

    void* dst = enki_arena_alloc(gc->active, h->size);
    if (!dst) {
        fprintf(stderr, "gc: to-space exhausted during collection\n");
        abort();
    }
    memcpy(dst, h, h->size);

    h->kind = FORWARDED;
    ((enki_fwd*)h)->fwd = dst;

    return MAKE_PTR(dst);
}

void enki_gc_collect(enki_gc* gc) {
    if (!gc) return;

    enki_arena* temp = gc->active;
    gc->active = gc->idle;
    gc->idle   = temp;

    gc->root = enki_gc_mark(gc, gc->root);

    size_t scan = sizeof(enki_arena);
    while (scan < gc->active->off) {
        void*               val = gc->active->ptr + scan;
        enki_value_header*  h   = val;
        enki_value_trace(gc, val);
        scan += h->size;
    }

    enki_arena_reset(gc->idle);
}
