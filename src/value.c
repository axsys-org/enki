#include <string.h>

#include "enki/value.h"
#include "enki/gc.h"

void enki_value_trace(enki_gc* gc, void* obj) {
    enki_value_header* h = obj;
    switch (h->kind) {

        case PIN: {
            enki_pin* pin = obj;
            pin->inner = enki_gc_mark(gc, pin->inner);
            break;
        }

        case LAW: {
            enki_law* law = obj;
            law->name = enki_gc_mark(gc, law->name);
            law->body = enki_gc_mark(gc, law->body);
            break;
        }

        case APP: {
            enki_app* app = obj;
            app->fn = enki_gc_mark(gc, app->fn);
            for (uint32_t k = 0; k < app->n_args; k++) {
                app->args[k] = enki_gc_mark(gc, app->args[k]);
            }
            break;
        }

        case INTERP: {
            enki_interp* interp = obj;
            for (uint32_t k = 0; k < interp->sp; k++) {
                interp->stack[k] = enki_gc_mark(gc, interp->stack[k]);
            }
            break;
        }

        case NAT:
            break;

        case FORWARDED:
            break;
    }
}

/*
enki_value* collect_subpins(enki_value inner, size_t* n_subpins) {
 collect_subpins(value):                                                                                                                                                                                                                                                    
      visited = {}            # hash → bool                       
      result  = []                            
      walk(value)                         
      return result
                                                                                                                                                                                                                                                                             
  walk(v):
      case NAT:    return                                                                                                                                                                                                                                                    
      case PIN:                                                   
          if v.hash not in visited:       
              visited[v.hash] = true
              result.append(v)                                                                                                                                                                                                                                               
          return                        # stop — don't recurse into v.inner
      case APP:                                                                                                                                                                                                                                                              
          walk(v.fn)                                              
          for k in 0..v.n_args:                                                                                                                                                                                                                                              
              walk(v.args[k])                 
      case LAW:                                                                                                                                                                                                                                                              
          walk(v.name)                                            
          walk(v.body)                                                                                                                                                                                                                                                       
      case INTERP: # optional — only if interps can be pinned
          for k in 0..v.sp:                                                                                                                                                                                                                                                  
              walk(v.stack[k])                                    
      case FORWARDED:                         
          assert(false)                 # shouldn't appear in user values  

}
*/
enki_value make_pin(enki_gc* gc, enki_value inner) {
    (void)gc; (void)inner;
   // size_t n_subpins; 
   /// enki_value* subpins = collect_subpins(inner, &n_subpins);
    size_t total = sizeof(enki_pin); // + n_subpins * sizeof(enki_value)
    enki_pin* new = enki_gc_alloc(gc, total);
    memset(new->hash, 0, 32);
//    memcpy(new->subpins, subpins, sizeof(enki_value) * n_subpins);
    new->inner = inner;
    new->h.size = total;
    new->h.kind = PIN;
    return MAKE_PTR(new);
}

enki_value make_nat(enki_gc* gc, const mp_limb_t* limbs, uint32_t n_limbs) {
    (void)gc; (void)limbs; (void)n_limbs;
    return 0;
}

enki_value make_law(enki_gc* gc, uint32_t arity, enki_value name,
                    enki_value body, const uint8_t* bc, size_t bc_len)
{
    size_t total = sizeof(enki_law) + bc_len;
    enki_law* new = enki_gc_alloc(gc, total);
    if (!new) return 0;

    new->h.kind = LAW;
    new->h.size = (uint32_t)total;
    new->arity  = arity;
    new->name   = name;
    new->body   = body;
    new->bc_len = (uint32_t)bc_len;
    if (bc_len > 0) memcpy(new->bc, bc, bc_len);

    return MAKE_PTR(new);
}

enki_value make_app(enki_gc* gc, enki_value fn,
                    const enki_value* args, uint32_t n_args)
{
    size_t total = sizeof(enki_app) + (size_t)n_args * sizeof(enki_value);
    enki_app* new = enki_gc_alloc(gc, total);
    if (!new) return 0;

    new->h.kind = APP;
    new->h.size = (uint32_t)total;
    new->fn     = fn;
    new->n_args = n_args;
    if (n_args > 0) memcpy(new->args, args, (size_t)n_args * sizeof(enki_value));

    return MAKE_PTR(new);
}

enki_value make_interp(enki_gc* gc, const uint8_t* bc, size_t bc_len) {
    size_t total = sizeof(enki_interp) + bc_len;
    enki_interp* new = enki_gc_alloc(gc, total);
    if (!new) return 0;

    new->h.kind = INTERP;
    new->h.size = (uint32_t)total;
    new->pc     = 0;
    new->sp     = 0;
    new->bc_len = (uint32_t)bc_len;
    if (bc_len > 0) memcpy(new->bc, bc, bc_len);

    return MAKE_PTR(new);
}
