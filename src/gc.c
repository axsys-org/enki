#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "enki/gc.h"
#include "enki/profile.h"

static bool enki_gc_is_known_tag(er_val val_v)
{
    switch (er_get_tag(val_v)) {
    case er_tag_bat:
    case er_tag_pin:
    case er_tag_law:
    case er_tag_app:
    case er_tag_thk:
    case er_tag_tank:
        return true;
    default:
        return false;
    }
}

static size_t enki_gc_obj_size(const er_head* h)
{
    return h->siz_s & ~(size_t)0x3u;
}

static er_val* enki_gc_forward_slot(er_head* h)
{
    return (er_val*)((unsigned char*)h + sizeof(er_head));
}

static void enki_gc_work_reset(enki_gc* gc)
{
    gc->work_s = 0;
    gc->work_i = 0;
}

static void enki_gc_work_push(enki_gc* gc, er_val val_v)
{
    if (gc->work_s == gc->work_cap_s) {
        size_t next_s = gc->work_cap_s == 0 ? 256 : gc->work_cap_s * 2u;
        if (next_s < gc->work_cap_s) {
            abort();
        }
        er_val* next_v = gc->our_a.realloc != NULL
                             ? gc->our_a.realloc(gc->our_a.ctx, gc->work_v,
                                                 next_s * sizeof(er_val))
                             : NULL;
        if (next_v == NULL) {
            next_v = gc->our_a.alloc(gc->our_a.ctx, next_s * sizeof(er_val));
            if (next_v == NULL) {
                abort();
            }
            if (gc->work_v != NULL) {
                memcpy(next_v, gc->work_v, gc->work_s * sizeof(er_val));
                gc->our_a.free(gc->our_a.ctx, gc->work_v);
            }
        }
        gc->work_v = next_v;
        gc->work_cap_s = next_s;
    }
    gc->work_v[gc->work_s++] = val_v;
}

static void* enki_gc_allocator_alloc(void* ctx, size_t size_s)
{
    return enki_gc_alloc((enki_gc*)ctx, size_s, _Alignof(er_head));
}

static void enki_gc_allocator_free(void* ctx, void* ptr)
{
    (void)ctx;
    (void)ptr;
}

enki_gc* enki_gc_from_allocator(const enki_allocator* allocator)
{
    if (allocator == NULL || allocator->alloc != enki_gc_allocator_alloc) {
        return NULL;
    }
    return (enki_gc*)allocator->ctx;
}

const enki_allocator* enki_gc_as_allocator(enki_gc* gc)
{
    return gc == NULL ? NULL : &gc->allocator_a;
}

const enki_allocator* enki_gc_parent_allocator(enki_gc* gc)
{
    return gc == NULL ? NULL : &gc->our_a;
}

enki_gc* enki_gc_create(const enki_allocator* loc_a, size_t cap_s, enki_interpreter* root)
{
    if (!loc_a) {
        return NULL;
    }
    enki_gc* gc = loc_a->alloc(loc_a->ctx, sizeof(enki_gc));
    if (!gc) {
        return NULL;
    }
    memset(gc, 0, sizeof(*gc));
    gc->our_a = *loc_a;
    gc->allocator_a = (enki_allocator){
        .ctx = gc,
        .alloc = enki_gc_allocator_alloc,
        .realloc = NULL,
        .free = enki_gc_allocator_free,
    };
    gc->cap_s = cap_s;
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

void enki_gc_set_trace_root(enki_gc* gc, void* root, enki_gc_trace_fn trace_fn)
{
    if (gc == NULL) {
        return;
    }
    gc->trace_root = root;
    gc->trace_fn = trace_fn;
}

void enki_gc_lock(enki_gc* gc)
{
    if (!gc) {
        abort();
    }
    gc->lock_depth++;
}

void enki_gc_unlock(enki_gc* gc)
{
    if (!gc || gc->lock_depth == 0) {
        abort();
    }
    gc->lock_depth--;
}

void enki_gc_destroy(enki_gc* gc)
{
    if (!gc) {
        return;
    }
    if (gc->work_v != NULL) {
        gc->our_a.free(gc->our_a.ctx, gc->work_v);
    }
    enki_arena_destroy(gc->idle_a);
    enki_arena_destroy(gc->active_a);
    gc->our_a.free(gc->our_a.ctx, gc);
}

void* enki_gc_alloc(enki_gc* gc, size_t size_s, size_t align_s)
{
    ENKI_PROFILE_ZONE("enki_gc_alloc");
    if (!gc) {
        abort();
    }
    void* new_p = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
    if (new_p == NULL) {
        if (gc->lock_depth > 0) {
            abort();
        }
        enki_gc_collect(gc);
        new_p = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
        if (new_p == NULL) {
            abort();
        }
    }
    return new_p;
}

void* enki_gc_alloc_locked(enki_gc* gc, size_t size_s, size_t align_s)
{
    ENKI_PROFILE_ZONE("enki_gc_alloc_locked");
    if (!gc) {
        abort();
    }
    enki_gc_lock(gc);
    void* new_p = enki_arena_alloc_aligned(gc->active_a, size_s, align_s);
    enki_gc_unlock(gc);
    if (!new_p) {
        abort();
    }
    return new_p;
}

er_val enki_gc_copy(enki_gc* gc, er_val val_v)
{
    ENKI_PROFILE_ZONE("enki_gc_copy");
    if (gc == NULL || er_is_cat(val_v)) {
        return val_v;
    }
    if (!enki_gc_is_known_tag(val_v)) {
        abort();
    }
    if (er_is_tank(val_v)) {
        er_tank* tank = er_outa(val_v);
        er_tank* new_tank = enki_arena_alloc_aligned(gc->active_a, sizeof(er_tank),
                                                     _Alignof(er_tank));
        if (new_tank == NULL) {
            abort();
        }
        memcpy(new_tank, tank, sizeof(er_tank));
        er_val new_v = er_into(er_tag_tank, new_tank);
        enki_gc_work_push(gc, new_v);
        return new_v;
    }

    er_head* h = er_outa(val_v);
    if (h->raw.fwd_f) {
        return *enki_gc_forward_slot(h);
    }

    size_t size_s = enki_gc_obj_size(h);
    void* new_p = enki_arena_alloc_aligned(gc->active_a, size_s, _Alignof(er_head));
    if (new_p == NULL) {
        abort();
    }
    memcpy(new_p, h, size_s);

    er_val new_v = er_into(er_get_tag(val_v), new_p);
    h->raw.fwd_f = 1;
    *enki_gc_forward_slot(h) = new_v;
    enki_gc_work_push(gc, new_v);
    return new_v;
}

static void enki_gc_trace_ref(enki_gc* gc, er_val* ref_v)
{
    if (ref_v != NULL && !er_is_cat(*ref_v) && enki_gc_is_known_tag(*ref_v)) {
        *ref_v = enki_gc_copy(gc, *ref_v);
    }
}

void enki_gc_trace_vm(enki_gc* gc, void* root)
{
    er_vm* vm = root;
    if (gc == NULL || vm == NULL) {
        return;
    }

    for (er_val* cur_v = vm->dstack; cur_v < vm->dsp; cur_v++) {
        enki_gc_trace_ref(gc, cur_v);
    }

    for (er_kon* cur = vm->kbase; cur < vm->ksp; cur++) {
        if (!er_is_cat(cur->val_v) && enki_gc_is_known_tag(cur->val_v)) {
            cur->val_v = enki_gc_copy(gc, cur->val_v);
        }
    }

    enki_gc_trace_ref(gc, vm->gc_rp);
    enki_gc_trace_ref(gc, &vm->code_law_v);
    for (size_t k = 0; k < vm->gc_tmp_s; k++) {
        enki_gc_trace_ref(gc, &vm->gc_tmp_v[k]);
    }
}

static void enki_gc_trace_object(enki_gc* gc, er_val val_v)
{
    switch (er_get_tag(val_v)) {
    case er_tag_bat:
        return;
    case er_tag_pin: {
        er_pin* pin = er_outa(val_v);
        enki_gc_trace_ref(gc, &pin->val_v);
        for (size_t k = 0; k < pin->sub_s; k++) {
            enki_gc_trace_ref(gc, &pin->sub_v[k]);
        }
        return;
    }
    case er_tag_law: {
        er_law* law = er_outa(val_v);
        enki_gc_trace_ref(gc, &law->name_v);
        enki_gc_trace_ref(gc, &law->body_v);
        for (size_t label_s = 0; label_s < law->bc_s; label_s++) {
            er_op* code_v = er_law_label_code(law, label_s);
            if (code_v == NULL) {
                continue;
            }
            for (size_t op_s = 0; op_s < law->bc_v[label_s].op_s; op_s++) {
                if (code_v[op_s].tag == OP_PUSH_LIT) {
                    enki_gc_trace_ref(gc, &code_v[op_s].as.lit_v);
                }
            }
        }
        return;
    }
    case er_tag_app: {
        er_app* app = er_outa(val_v);
        enki_gc_trace_ref(gc, &app->fn_v);
        for (size_t k = 0; k < app->arg_s; k++) {
            enki_gc_trace_ref(gc, &app->arg_v[k]);
        }
        return;
    }
    case er_tag_thk: {
        er_thk* thk = er_outa(val_v);
        for (size_t k = 0; k < thk->arg_s; k++) {
            enki_gc_trace_ref(gc, &thk->arg_v[k]);
        }
        return;
    }
    case er_tag_tank: {
        er_tank* tank = er_outa(val_v);
        enki_gc_trace_ref(gc, &tank->val_v);
        return;
    }
    default:
        abort();
    }
}

void enki_gc_collect(enki_gc* gc)
{
    ENKI_PROFILE_ZONE("enki_gc_collect");
    if (!gc) {
        return;
    }
    if (gc->lock_depth > 0) {
        abort();
    }

    enki_arena* from_a = gc->active_a;
    gc->active_a = gc->idle_a;
    gc->idle_a = from_a;
    enki_arena_reset(gc->active_a);
    enki_gc_work_reset(gc);

    if (gc->trace_fn != NULL) {
        gc->trace_fn(gc, gc->trace_root);
    }

    while (gc->work_i < gc->work_s) {
        enki_gc_trace_object(gc, gc->work_v[gc->work_i++]);
    }

    enki_arena_reset(gc->idle_a);
    enki_gc_work_reset(gc);
}
