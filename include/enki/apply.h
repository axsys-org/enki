#include <stddef.h>
#include <stdint.h>

#include "enki/interp.h"

void enki_apply(enki_interpreter* i, size_t n_args_s);
//size_t enki_arity(enki_value val_v);
void enki_enter_law(size_t arity_s, enki_value val_v, enki_interpreter* i);
