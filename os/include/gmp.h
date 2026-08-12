#ifndef ENKI_OS_GMP_H
#define ENKI_OS_GMP_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t mp_limb_t;
typedef long mp_size_t;
typedef mp_limb_t* mp_ptr;
typedef const mp_limb_t* mp_srcptr;

typedef struct __mpz_struct {
  mp_limb_t* limbs;
  size_t size;
  size_t capacity;
  int sign;
} __mpz_struct;
typedef __mpz_struct mpz_t[1];

int mpn_cmp(mp_srcptr a, mp_srcptr b, mp_size_t n);
mp_limb_t mpn_add(mp_ptr out, mp_srcptr a, mp_size_t an,
                  mp_srcptr b, mp_size_t bn);
mp_limb_t mpn_sub(mp_ptr out, mp_srcptr a, mp_size_t an,
                  mp_srcptr b, mp_size_t bn);
mp_limb_t mpn_add_1(mp_ptr out, mp_srcptr a, mp_size_t n, mp_limb_t value);
mp_limb_t mpn_sub_1(mp_ptr out, mp_srcptr a, mp_size_t n, mp_limb_t value);
void mpn_mul(mp_ptr out, mp_srcptr a, mp_size_t an,
             mp_srcptr b, mp_size_t bn);
mp_limb_t mpn_lshift(mp_ptr out, mp_srcptr in, mp_size_t n, unsigned shift);
mp_limb_t mpn_rshift(mp_ptr out, mp_srcptr in, mp_size_t n, unsigned shift);
void mpn_tdiv_qr(mp_ptr quotient, mp_ptr remainder, mp_size_t qxn,
                 mp_srcptr numerator, mp_size_t nn,
                 mp_srcptr denominator, mp_size_t dn);

void mpz_init(mpz_t value);
void mpz_clear(mpz_t value);
int mpz_set_str(mpz_t value, const char* text, int base);
int mpz_sgn(const mpz_t value);
size_t mpz_size(const mpz_t value);
void* mpz_export(void* out, size_t* count, int order, size_t size,
                 int endian, size_t nails, const mpz_t value);
void mpz_import(mpz_t value, size_t count, int order, size_t size,
                int endian, size_t nails, const void* in);
char* mpz_get_str(char* out, int base, const mpz_t value);
void mp_get_memory_functions(void* (**alloc_fn)(size_t),
                             void* (**realloc_fn)(void*, size_t, size_t),
                             void (**free_fn)(void*, size_t));

#endif
