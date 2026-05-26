#include <stddef.h>
#include <stdint.h>

#include "enki/interp.h"

enki_value enki_app_alloc(enki_gc* gc, enki_value fn_v, size_t n_args_s);
enki_value enki_app_cont_alloc(enki_gc* gc, size_t n_args_s, enki_value* bas_v);
void enki_app_apply(enki_interpreter* i, size_t n_args_s);
size_t enki_app_arity(enki_value val_v);
