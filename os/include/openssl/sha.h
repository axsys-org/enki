#ifndef ENKI_OS_OPENSSL_SHA_H
#define ENKI_OS_OPENSSL_SHA_H
#include <stddef.h>
unsigned char* SHA256(const unsigned char* data, size_t size,
                      unsigned char out[32]);
#endif
