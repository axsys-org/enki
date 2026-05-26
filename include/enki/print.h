
#ifndef ENKI_PRINT_H
#define ENKI_PRINT_H
#include <enki/value.h>

char* enki_print_value(
    const enki_allocator* cat_a,
    enki_value val_v,
    size_t* out_s
);

#define enki_pvalue(loc, v) enki_print_value(loc, v, NULL)

#endif
