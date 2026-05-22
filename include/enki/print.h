
#ifndef ENKI_PRINT_H
#define ENKI_PRINT_H
#include <enki/value.h>

char* enki_print_value(
    enki_allocator cat_a,
    enki_value val_v,
    size_t* out_s
);

#define enki_pvalue(v) enki_print_value(EA_TMP_ALLOC, v, NULL)

#endif
