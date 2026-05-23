#pragma once

#include <stddef.h>

#include "enki/interp.h"
#include "enki/value.h"

enki_value enki_kal(enki_interpreter* i, size_t depth, size_t env, enki_value body);
enki_value enki_judge(enki_interpreter* i, enki_value val, size_t arg_base);
