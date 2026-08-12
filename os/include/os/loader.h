#ifndef ENKI_OS_LOADER_H
#define ENKI_OS_LOADER_H

#include <stdbool.h>

#include "enki/wisp.h"

typedef struct os_loader os_loader;

os_loader* os_loader_new(en_wisp* wisp);
void os_loader_free(os_loader* loader);
bool os_loader_load(os_loader* loader, const char* module);
bool os_loader_binding(os_loader* loader, const char* name, pl_val* out);

#endif
