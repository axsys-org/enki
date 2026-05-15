#include <string.h>

#include "enki/value.h"
#include "enki/gc.h"

void enki_trace_value(enki_gc* gc, void* obj) {
    enki_value_header* h = obj;
    switch (h->kind) {
        case ENKI_PIN:
          enki_pin* pin = obj;
          pin->inner = gc->copy(gc, pin->inner);
          break;
        case ENKI_LAW:
          enki_law* law = obj;
          law->body = gc->copy(gc, law->body);
          law->name = gc->copy(gc, law->name);
          break;
        case ENKI_APP:
          enki_app* app = obj;
          app->fn = gc->copy(gc, app->fn);
          for (size_t k = 0; k < app->n_args; k++) {
            app->args[k] = gc->copy(gc, app->args[k]);
          }
          break;
        case ENKI_FRWD:
          return;
        case ENKI_BIG_NAT:
          return;
        default:
          return;
    }
}

enki_value enki_alloc_nat(enki_gc* gc, size_t n_bytes, uint8_t bytes[]) {
    


}
enki_value enki_alloc_law(enki_gc* gc, uint32_t arity, enki_value name, enki_value body, uint32_t bc_len, const uint8_t bc[]) {
    size_t n = sizeof(enki_law) + bc_len;
    enki_law* new = (enki_law*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_LAW;
    new->body = body;
    new->name = name;
    new->arity = arity;
    new->bc_len = bc_len;
    if(bc_len > 0) memcpy(new->bc, bc, bc_len);
    return MAKE_PTR(new);
}
enki_value enki_alloc_pin(enki_gc* gc, const uint8_t hash[32], enki_value inner, size_t n_subpins, enki_value subpins[]) {
    size_t n = sizeof(enki_pin) + (n_subpins * sizeof(enki_value));
    enki_pin* new = (enki_pin*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_PIN;
    new->inner = inner;
    new->n_subpins = n_subpins;
    memcpy(new->hash, hash, 32);
    if(n_subpins > 0) memcpy(new->subpins, subpins, n_subpins * sizeof(enki_value));
    return MAKE_PTR(new);
}
enki_value enki_alloc_app(enki_gc* gc, enki_value fn, size_t n_args) {
    size_t n = sizeof(enki_app) + (n_args * sizeof(enki_value));
    enki_app* new = (enki_app*)gc->alloc(gc, n);
    if(!new) return 0;
    new->h.size = n;
    new->h.kind = ENKI_APP;
    new->fn = fn;
    new->n_args = n_args;
    //if(n_args > 0) memcpy(new->args, args, n_args * sizeof(enki_value));
    return MAKE_PTR(new);
}