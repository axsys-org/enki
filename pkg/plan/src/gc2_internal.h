#ifndef PL_GC2_INTERNAL_H
#define PL_GC2_INTERNAL_H
#include "plan/store.h"

void pl_gc2_set_finalizer(pl_store* s, pl_val v, void (*drop)(void*),
                          void* context);
void pl_gc2_init(pl_store* s, size_t capacity);
void pl_gc2_destroy(pl_store* s);
pl_cell* pl_gc2_alloc(pl_store* s, size_t cells);
size_t pl_gc2_checkpoint(pl_store* s);
void pl_gc2_rollback(pl_store* s, size_t checkpoint);
bool pl_gc2_live(pl_store* s, pl_val v);
void pl_gc2_visit_code(const pl_code* code, pl_root_visit visit, void* ctx);
void pl_store_gc_roots(pl_store* s, pl_root_visit visit,
                       void (*visit_code)(const pl_code*, void*), void* ctx);
void pl_store_gc_weak(pl_store* s);
#endif
