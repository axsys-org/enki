#ifndef ENKI_ASSUME_H
#define ENKI_ASSUME_H
#include <enki/util.h>
#include <limits.h>

static_assert(CHAR_BIT == 8,            "byte must be 8 bits");
static_assert(CHAR_MIN == 0,            "char must be unsigned (build with -funsigned-char)");
static_assert(sizeof(void *) == 8,      "tagged pointer scheme assumes 64-bit");
static_assert(sizeof(size_t) == sizeof(uintptr_t), "size_t must match pointer width");

#endif
