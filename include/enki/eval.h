#pragma once
#include "enki/interp.h"

enki_value enki_eval_whnf(enki_interpreter* i, enki_value x);
enki_value enki_eval_nf(enki_interpreter* i, enki_value x);
