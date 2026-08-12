#ifndef ENKI_OS_STDLIB_H
#define ENKI_OS_STDLIB_H

#include <stddef.h>

void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
[[noreturn]] void abort(void);
[[noreturn]] void exit(int status);
int atexit(void (*fn)(void));
char* getenv(const char* name);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
double strtod(const char* s, char** end);
void qsort(void* base, size_t count, size_t size,
           int (*compare)(const void*, const void*));

#endif
