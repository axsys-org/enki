#pragma once

#include <stdint.h>

#include "enki/run.h"

er_val er_law_compile(const enki_allocator* loc_a, er_val nam_v, er_val bod_v,
    uint32_t ari_d);
