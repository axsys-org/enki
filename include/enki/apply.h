#include <stddef.h>
#include <stdint.h>

#include "enki/interp.h"

void enki_apply(enki_interpreter* i, size_t n_args);
size_t enki_arity(enki_value val);
void enki_enter_law(size_t arity, enki_value val, enki_interpreter* i);
