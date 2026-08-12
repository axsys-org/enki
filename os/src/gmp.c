#include <gmp.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static size_t trim(const mp_limb_t* limbs, size_t size) {
  while (size != 0 && limbs[size - 1] == 0)
    size--;
  return size;
}

int mpn_cmp(mp_srcptr a, mp_srcptr b, mp_size_t n) {
  while (n > 0) {
    n--;
    if (a[n] != b[n])
      return a[n] < b[n] ? -1 : 1;
  }
  return 0;
}

mp_limb_t mpn_add(mp_ptr out, mp_srcptr a, mp_size_t an,
                  mp_srcptr b, mp_size_t bn) {
  __uint128_t carry = 0;
  mp_size_t i = 0;
  for (; i < bn; i++) {
    __uint128_t sum = (__uint128_t)a[i] + b[i] + carry;
    out[i] = (mp_limb_t)sum;
    carry = sum >> 64;
  }
  for (; i < an; i++) {
    __uint128_t sum = (__uint128_t)a[i] + carry;
    out[i] = (mp_limb_t)sum;
    carry = sum >> 64;
  }
  return (mp_limb_t)carry;
}

mp_limb_t mpn_sub(mp_ptr out, mp_srcptr a, mp_size_t an,
                  mp_srcptr b, mp_size_t bn) {
  mp_limb_t borrow = 0;
  mp_size_t i = 0;
  for (; i < bn; i++) {
    __uint128_t sub = (__uint128_t)b[i] + borrow;
    mp_limb_t low = (mp_limb_t)sub;
    mp_limb_t next = ((sub >> 64) != 0) || a[i] < low;
    out[i] = a[i] - low;
    borrow = next;
  }
  for (; i < an; i++) {
    mp_limb_t next = a[i] < borrow;
    out[i] = a[i] - borrow;
    borrow = next;
  }
  return borrow;
}

mp_limb_t mpn_add_1(mp_ptr out, mp_srcptr a, mp_size_t n, mp_limb_t value) {
  __uint128_t sum = (__uint128_t)a[0] + value;
  out[0] = (mp_limb_t)sum;
  mp_limb_t carry = (mp_limb_t)(sum >> 64);
  for (mp_size_t i = 1; i < n; i++) {
    sum = (__uint128_t)a[i] + carry;
    out[i] = (mp_limb_t)sum;
    carry = (mp_limb_t)(sum >> 64);
  }
  return carry;
}

mp_limb_t mpn_sub_1(mp_ptr out, mp_srcptr a, mp_size_t n, mp_limb_t value) {
  mp_limb_t borrow = a[0] < value;
  out[0] = a[0] - value;
  for (mp_size_t i = 1; i < n; i++) {
    mp_limb_t next = a[i] < borrow;
    out[i] = a[i] - borrow;
    borrow = next;
  }
  return borrow;
}

void mpn_mul(mp_ptr out, mp_srcptr a, mp_size_t an,
             mp_srcptr b, mp_size_t bn) {
  memset(out, 0, (size_t)(an + bn) * sizeof(*out));
  for (mp_size_t i = 0; i < an; i++) {
    __uint128_t carry = 0;
    for (mp_size_t j = 0; j < bn; j++) {
      __uint128_t value = (__uint128_t)a[i] * b[j] + out[i + j] + carry;
      out[i + j] = (mp_limb_t)value;
      carry = value >> 64;
    }
    mp_size_t at = i + bn;
    while (carry != 0) {
      __uint128_t value = (__uint128_t)out[at] + carry;
      out[at] = (mp_limb_t)value;
      carry = value >> 64;
      at++;
    }
  }
}

mp_limb_t mpn_lshift(mp_ptr out, mp_srcptr in, mp_size_t n, unsigned shift) {
  mp_limb_t carry = 0;
  for (mp_size_t i = 0; i < n; i++) {
    mp_limb_t value = in[i];
    out[i] = (value << shift) | carry;
    carry = value >> (64 - shift);
  }
  return carry;
}

mp_limb_t mpn_rshift(mp_ptr out, mp_srcptr in, mp_size_t n, unsigned shift) {
  mp_limb_t carry = 0;
  for (mp_size_t i = n; i > 0; i--) {
    mp_limb_t value = in[i - 1];
    out[i - 1] = (value >> shift) | carry;
    carry = value << (64 - shift);
  }
  return carry;
}

static int compare_var(const mp_limb_t* a, size_t an,
                       const mp_limb_t* b, size_t bn) {
  an = trim(a, an);
  bn = trim(b, bn);
  if (an != bn)
    return an < bn ? -1 : 1;
  return mpn_cmp(a, b, (mp_size_t)an);
}

void mpn_tdiv_qr(mp_ptr quotient, mp_ptr remainder, mp_size_t qxn,
                 mp_srcptr numerator, mp_size_t nn,
                 mp_srcptr denominator, mp_size_t dn) {
  (void)qxn;
  size_t qn = (size_t)(nn - dn + 1);
  memset(quotient, 0, qn * sizeof(*quotient));
  memset(remainder, 0, (size_t)dn * sizeof(*remainder));
  size_t bits = (size_t)nn * 64;
  while (bits != 0 && ((numerator[(bits - 1) / 64] >> ((bits - 1) % 64)) & 1) == 0)
    bits--;
  for (size_t bit = bits; bit > 0; bit--) {
    mp_limb_t carry = 0;
    for (mp_size_t i = 0; i < dn; i++) {
      mp_limb_t next = remainder[i] >> 63;
      remainder[i] = (remainder[i] << 1) | carry;
      carry = next;
    }
    size_t src_bit = bit - 1;
    remainder[0] |= (numerator[src_bit / 64] >> (src_bit % 64)) & 1;
    /* A shifted remainder may carry one bit beyond the dn-limb buffer.
     * In that case it is necessarily >= denominator; subtraction modulo
     * 2^(64*dn) produces the exact (now fitting) remainder. */
    if (carry != 0 ||
        compare_var(remainder, (size_t)dn, denominator, (size_t)dn) >= 0) {
      (void)mpn_sub(remainder, remainder, dn, denominator, dn);
      size_t qbit = src_bit;
      if (qbit / 64 < qn)
        quotient[qbit / 64] |= UINT64_C(1) << (qbit % 64);
    }
  }
}

static bool mpz_reserve(__mpz_struct* value, size_t capacity) {
  if (capacity <= value->capacity)
    return true;
  size_t next = value->capacity == 0 ? 2 : value->capacity;
  while (next < capacity) {
    if (next > SIZE_MAX / 2)
      return false;
    next *= 2;
  }
  mp_limb_t* limbs = realloc(value->limbs, next * sizeof(*limbs));
  if (limbs == NULL)
    return false;
  memset(limbs + value->capacity, 0,
         (next - value->capacity) * sizeof(*limbs));
  value->limbs = limbs;
  value->capacity = next;
  return true;
}

static bool mpz_mul_small(__mpz_struct* value, uint32_t multiplier) {
  if (value->size == 0)
    return true;
  if (!mpz_reserve(value, value->size + 1))
    return false;
  __uint128_t carry = 0;
  for (size_t i = 0; i < value->size; i++) {
    __uint128_t product = (__uint128_t)value->limbs[i] * multiplier + carry;
    value->limbs[i] = (mp_limb_t)product;
    carry = product >> 64;
  }
  if (carry != 0)
    value->limbs[value->size++] = (mp_limb_t)carry;
  return true;
}

static bool mpz_add_small(__mpz_struct* value, uint32_t addend) {
  if (!mpz_reserve(value, value->size + 1))
    return false;
  if (value->size == 0)
    value->size = 1;
  __uint128_t carry = addend;
  for (size_t i = 0; i < value->size && carry != 0; i++) {
    carry += value->limbs[i];
    value->limbs[i] = (mp_limb_t)carry;
    carry >>= 64;
  }
  if (carry != 0)
    value->limbs[value->size++] = (mp_limb_t)carry;
  value->size = trim(value->limbs, value->size);
  return true;
}

void mpz_init(mpz_t value) {
  *value = (__mpz_struct){0};
}

void mpz_clear(mpz_t value) {
  free(value->limbs);
  *value = (__mpz_struct){0};
}

int mpz_set_str(mpz_t value, const char* text, int base) {
  if (base != 10)
    return -1;
  value->size = 0;
  value->sign = 1;
  if (*text == '+' || *text == '-') {
    if (*text++ == '-') value->sign = -1;
  }
  if (*text == '\0')
    return -1;
  for (; *text != '\0'; text++) {
    if (!isdigit((unsigned char)*text))
      return -1;
    if (!mpz_mul_small(value, 10) || !mpz_add_small(value, (uint32_t)(*text - '0')))
      return -1;
  }
  if (value->size == 0)
    value->sign = 0;
  return 0;
}

int mpz_sgn(const mpz_t value) { return value->size == 0 ? 0 : value->sign; }
size_t mpz_size(const mpz_t value) { return value->size; }

void* mpz_export(void* out, size_t* count, int order, size_t size,
                 int endian, size_t nails, const mpz_t value) {
  (void)endian;
  (void)nails;
  if (size != sizeof(mp_limb_t) || (order != -1 && order != 1))
    return NULL;
  if (out == NULL)
    out = malloc(value->size * size);
  if (out == NULL && value->size != 0)
    return NULL;
  mp_limb_t* limbs = out;
  for (size_t i = 0; i < value->size; i++)
    limbs[order == -1 ? i : value->size - i - 1] = value->limbs[i];
  if (count != NULL)
    *count = value->size;
  return out;
}

void mpz_import(mpz_t value, size_t count, int order, size_t size,
                int endian, size_t nails, const void* in) {
  (void)endian;
  (void)nails;
  value->size = 0;
  value->sign = 0;
  if (size != sizeof(mp_limb_t) || (order != -1 && order != 1) ||
      !mpz_reserve(value, count))
    return;
  const mp_limb_t* limbs = in;
  for (size_t i = 0; i < count; i++)
    value->limbs[i] = limbs[order == -1 ? i : count - i - 1];
  value->size = trim(value->limbs, count);
  value->sign = value->size == 0 ? 0 : 1;
}

static uint32_t div_small(mp_limb_t* limbs, size_t* size, uint32_t divisor) {
  uint64_t remainder = 0;
  for (size_t i = *size; i > 0; i--) {
    uint64_t high = (remainder << 32) | (limbs[i - 1] >> 32);
    uint64_t high_quotient = high / divisor;
    remainder = high % divisor;
    uint64_t low = (remainder << 32) | (uint32_t)limbs[i - 1];
    uint64_t low_quotient = low / divisor;
    remainder = low % divisor;
    limbs[i - 1] = (high_quotient << 32) | low_quotient;
  }
  *size = trim(limbs, *size);
  return (uint32_t)remainder;
}

char* mpz_get_str(char* out, int base, const mpz_t value) {
  if (base != 10)
    return NULL;
  if (value->size == 0) {
    if (out == NULL) out = malloc(2);
    if (out != NULL) strcpy(out, "0");
    return out;
  }
  size_t chunks_cap = value->size * 3 + 1;
  uint32_t* chunks = malloc(chunks_cap * sizeof(*chunks));
  mp_limb_t* copy = malloc(value->size * sizeof(*copy));
  if (chunks == NULL || copy == NULL) {
    free(chunks); free(copy); return NULL;
  }
  memcpy(copy, value->limbs, value->size * sizeof(*copy));
  size_t copy_size = value->size;
  size_t chunks_n = 0;
  while (copy_size != 0)
    chunks[chunks_n++] = div_small(copy, &copy_size, 1000000000u);
  size_t capacity = chunks_n * 9 + 2;
  if (out == NULL) out = malloc(capacity);
  if (out == NULL) { free(chunks); free(copy); return NULL; }
  char* cursor = out;
  if (value->sign < 0) *cursor++ = '-';
  uint32_t top = chunks[chunks_n - 1];
  char temp[10];
  size_t top_n = 0;
  do { temp[top_n++] = (char)('0' + top % 10); top /= 10; } while (top != 0);
  while (top_n != 0) *cursor++ = temp[--top_n];
  for (size_t i = chunks_n - 1; i > 0; i--) {
    uint32_t chunk = chunks[i - 1];
    for (int digit = 8; digit >= 0; digit--) {
      uint32_t power = 1;
      for (int p = 0; p < digit; p++) power *= 10;
      *cursor++ = (char)('0' + (chunk / power) % 10);
    }
  }
  *cursor = '\0';
  free(chunks);
  free(copy);
  return out;
}

static void* gmp_alloc(size_t size) { return malloc(size); }
static void* gmp_realloc(void* ptr, size_t old_size, size_t new_size) {
  (void)old_size;
  return realloc(ptr, new_size);
}
static void gmp_free(void* ptr, size_t size) { (void)size; free(ptr); }

void mp_get_memory_functions(void* (**alloc_fn)(size_t),
                             void* (**realloc_fn)(void*, size_t, size_t),
                             void (**free_fn)(void*, size_t)) {
  if (alloc_fn != NULL) *alloc_fn = gmp_alloc;
  if (realloc_fn != NULL) *realloc_fn = gmp_realloc;
  if (free_fn != NULL) *free_fn = gmp_free;
}
