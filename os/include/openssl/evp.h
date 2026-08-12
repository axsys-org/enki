#ifndef ENKI_OS_OPENSSL_EVP_H
#define ENKI_OS_OPENSSL_EVP_H
#include <stddef.h>

typedef struct EVP_MD_CTX EVP_MD_CTX;
typedef struct EVP_MD EVP_MD;

EVP_MD_CTX* EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX* ctx);
const EVP_MD* EVP_sha256(void);
int EVP_DigestInit_ex(EVP_MD_CTX* ctx, const EVP_MD* type, void* impl);
int EVP_DigestUpdate(EVP_MD_CTX* ctx, const void* data, size_t size);
int EVP_DigestFinal_ex(EVP_MD_CTX* ctx, unsigned char* out,
                       unsigned int* out_size);

#endif
