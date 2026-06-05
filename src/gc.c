#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/trace.h"
#include "enki/value.h"
#include "stb_ds.h"

static size_t enki_gc_align_offset(size_t off_o) {
    size_t align_s = _Alignof(enki_value_header);
    return (off_o + align_s - 1) & ~(align_s - 1);
}

static enki_value enki_import_copy(enki_import_ctx* ctx, enki_value val_v) {

    if(!IS_PTR(val_v)) return val_v;

    void* old = ENKI_AS(void, val_v);
    ptrdiff_t idx = hmgeti(ctx->seen, old);
    if(idx >= 0) return ctx->seen[idx].value;

    enki_value_header* h = old;
    if(h->kind_b == ENKI_FRWD) {
        return enki_import_copy(ctx, PTR_TO_ENKI(*(void**)GET_PAYLOAD(val_v)));
    }

    void* new = enki_arena_alloc_aligned(
        ctx->dst_i->gc->active_a,
        h->size_s,
        _Alignof(enki_value_header)
    );
    if(new == NULL) {
        enki_interp_throw(ctx->dst_i, ENKI_ERROR_OOM, 0);
    }

    memcpy(new, old, h->size_s);

    enki_value new_v = PTR_TO_ENKI(new);
    hmput(ctx->seen, old, new_v);
    arrput(ctx->work, new_v);

    return new_v;
}

static void enki_import_fix(enki_import_ctx* ctx, enki_value val_v) {
    enki_value_header* h = ENKI_AS(enki_value_header, val_v);
    switch (h->kind_b) {
        case ENKI_PIN: {
          enki_pin* pin = ENKI_AS(enki_pin, val_v);
          pin->inner_v = enki_import_copy(ctx, pin->inner_v);
          for(size_t k = 0; k < pin->n_subpins_s; k++) {
            pin->subpins_v[k] = enki_import_copy(ctx, pin->subpins_v[k]);
          }
          break;
        }
        case ENKI_LAW: {
            enki_law* law = ENKI_AS(enki_law, val_v);
            law->name_v = enki_import_copy(ctx, law->name_v);
            law->body_v = enki_import_copy(ctx, law->body_v);
            for(size_t k = 0; k < law->n_const_s; k++) {
                ENKI_LAW_CONSTS(law)[k] =
                    enki_import_copy(ctx, ENKI_LAW_CONSTS(law)[k]);
            }
            break;
        }
        case ENKI_APP: {
            enki_app* app = ENKI_AS(enki_app, val_v);
            app->fn_v = enki_import_copy(ctx, app->fn_v);
            for(size_t k = 0; k < app->n_args_s; k++) {
                app->args_v[k] = enki_import_copy(ctx, app->args_v[k]);
            }
            break;
        }
        case ENKI_IND: {
          enki_ind* ind = ENKI_AS(enki_ind, val_v);
          ind->fn_v = enki_import_copy(ctx, ind->fn_v);
          break;
        }
        case ENKI_CONT: {
          enki_cont* cont = ENKI_AS(enki_cont, val_v);
          for(size_t k = 0; k < cont->n_args_s; k++) {
              cont->args_v[k] = enki_import_copy(ctx, cont->args_v[k]);
          }
          break;
        }
        case ENKI_NAT:
          break;
        default:
          abort();
    }
}

enki_value enki_gc_import(enki_interpreter* dst_i, enki_value root_v) {
    enki_import_ctx ctx = {
        .dst_i = dst_i, 
        .seen = NULL,
        .work = NULL,
    };

    enki_value root_copy = enki_import_copy(&ctx, root_v);

    for(size_t k = 0; k < (size_t)arrlen(ctx.work); k++) {
        enki_import_fix(&ctx, ctx.work[k]);
    }

    hmfree(ctx.seen);
    arrfree(ctx.work);

    return root_copy;
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
