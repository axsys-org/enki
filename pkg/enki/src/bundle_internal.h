#ifndef EN_BUNDLE_INTERNAL_H
#define EN_BUNDLE_INTERNAL_H
#include "enki/bundle.h"
#include <sys/types.h>

bool en_bundle_error(char* err, size_t cap, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
bool en_bundle_write_all(int fd, const void* bytes, size_t len);
bool en_bundle_copy(int src, int dst, uint64_t off, uint64_t len);
int en_bundle_open_self(char* err, size_t cap);
bool en_bundle_platform_write(int src, int dst, const uint8_t* blob, size_t len,
                              char* err, size_t cap);
bool en_bundle_sign(const char* path, char* err, size_t cap);
#endif
