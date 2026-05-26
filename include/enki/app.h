#include <stddef.h>
#include <stdint.h>

#include "enki/interp.h"

enki_value enki_app_alloc(enki_gc* gc, enki_value fn_v, size_t n_args_s);
enki_value enki_app_cont_alloc(enki_gc* gc, size_t n_args_s, enki_value* bas_v);
void enki_app_apply(enki_interpreter* i, size_t n_args_s);
size_t enki_app_arity(enki_value val_v);

enki_value enki_app_hd(enki_value app_v);
enki_value enki_app_idx(enki_value app_v, size_t idx_s);
enki_app* enki_alloc_app_bare(enki_gc* gc, enki_value fn_v, size_t n_args_s);
enki_value enki_alloc_pair(enki_gc* gc, enki_value l_v, enki_value r_v);
enki_value enki_alloc_trel(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v);

enki_value enki_alloc_quad(
    enki_gc* gc,
    enki_value fn_v,
    enki_value one_v,
    enki_value two_v,
    enki_value tri_v
);
enki_value enki_alloc_row(
    enki_gc* gc,
    enki_value fn_v,
    size_t arg_s,
    enki_value* arg_v
);


enki_value enki_alloc_quin(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v,
                           enki_value tri_v, enki_value qua_v);

