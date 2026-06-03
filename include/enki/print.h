
#ifndef ENKI_PRINT_H
#define ENKI_PRINT_H
#include <enki/run.h>

char* enki_print_value(
    const enki_allocator* cat_a,
    er_val val_v,
    size_t* out_s
);



char* enki_log_value(
    const enki_allocator* cat_a,
    er_val val_v,
    size_t* out_s
);

#define enki_pvalue(loc, v) enki_print_value(loc, v, NULL)

#define enki_log_val(file, loc, v) enki_log_value(loc, v, NULL)

#endif
