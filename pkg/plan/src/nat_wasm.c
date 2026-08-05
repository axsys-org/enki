#include "plan/nat.h"

#include <stdlib.h>
#include <string.h>

#include "axsys/assume.h"
#include "plan/build.h"
#include "plan/eval.h"

typedef struct pl_limbs {
  const uint64_t* p;
  size_t n;
} pl_limbs;

static size_t trim_n(uint64_t* p, size_t n) {
  while (n > 0 && p[n - 1] == 0)
    n--;
  return n;
}

static pl_limbs pl_limb_view(const pl_val* v, uint64_t* tmp) {
  if (pl_is_nat63(*v)) {
    *tmp = *v;
    return (pl_limbs){tmp, *v == 0 ? 0 : 1};
  }
  pl_cell* p = pl_ptr(*v);
  ax_assume(pl_hdr_kind(p[0]) == PL_K_NAT, "limb view of non-nat");
  return (pl_limbs){pl_nat_limb_ptr(p), pl_nat_limbs(p)};
}

static uint64_t* copy_limbs(pl_val v, size_t* out_n) {
  uint64_t tmp;
  pl_limbs l = pl_limb_view(&v, &tmp);
  uint64_t* out = calloc(l.n == 0 ? 1 : l.n, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  memcpy(out, l.p, l.n * sizeof(uint64_t));
  *out_n = l.n;
  return out;
}

static pl_val nat_from_limbs(pl_thread* t, const uint64_t* limbs, size_t n) {
  n = trim_n((uint64_t*)limbs, n);
  if (n == 0)
    return 0;
  if (n == 1 && limbs[0] <= PL_NAT63_MAX)
    return limbs[0];
  pl_gc_reserve(t, PL_NAT_CELLS(n));
  PL_GC_FORBID(t);
  uint64_t* out;
  pl_val r = pl_mk_nat_limbs(t, n, &out);
  memcpy(out, limbs, n * sizeof(uint64_t));
  r = pl_nat_trim(r);
  PL_GC_ALLOW(t);
  return r;
}

size_t pl_nat_limb_len(pl_val v) {
  uint64_t tmp;
  return pl_limb_view(&v, &tmp).n;
}

uint64_t pl_nat_limb_at(pl_val v, size_t i) {
  if (pl_is_nat63(v))
    return i == 0 ? v : 0;
  pl_cell* p = pl_ptr(v);
  return i < pl_nat_limbs(p) ? pl_nat_limb_ptr(p)[i] : 0;
}

int pl_nat_cmp(pl_val a, pl_val b) {
  uint64_t ta, tb;
  pl_limbs la = pl_limb_view(&a, &ta);
  pl_limbs lb = pl_limb_view(&b, &tb);
  if (la.n != lb.n)
    return la.n < lb.n ? -1 : 1;
  for (size_t i = la.n; i > 0; i--) {
    if (la.p[i - 1] != lb.p[i - 1])
      return la.p[i - 1] < lb.p[i - 1] ? -1 : 1;
  }
  return 0;
}

bool pl_nat_eq(pl_val a, pl_val b) {
  return pl_nat_cmp(a, b) == 0;
}

bool pl_nat_is_zero(pl_val v) {
  return v == 0;
}

uint64_t pl_nat_u64_clamp(pl_val v) {
  if (pl_is_nat63(v))
    return v;
  pl_cell* p = pl_ptr(v);
  uint32_t n = pl_nat_limbs(p);
  if (n == 0)
    return 0;
  if (n == 1)
    return pl_nat_limb_ptr(p)[0];
  return UINT64_MAX;
}

size_t pl_nat_bit_len(pl_val v) {
  size_t n = pl_nat_limb_len(v);
  if (n == 0)
    return 0;
  uint64_t top = pl_nat_limb_at(v, n - 1);
  return (n - 1) * 64 + (64 - (size_t)__builtin_clzll(top));
}

size_t pl_nat_byte_len(pl_val v) {
  return (pl_nat_bit_len(v) + 7) / 8;
}

uint8_t pl_nat_byte_at(pl_val v, size_t i) {
  uint64_t limb = pl_nat_limb_at(v, i / 8);
  return (uint8_t)(limb >> ((i % 8) * 8));
}

bool pl_nat_test_bit(pl_val bit, pl_val a) {
  uint64_t i = pl_nat_u64_clamp(bit);
  if (i / 64 >= pl_nat_limb_len(a))
    return false;
  return ((pl_nat_limb_at(a, i / 64) >> (i % 64)) & 1u) != 0;
}

pl_val pl_nat_inc(pl_thread* t, pl_val* a) {
  pl_val one = 1;
  return pl_nat_add(t, a, &one);
}

pl_val pl_nat_dec(pl_thread* t, pl_val* a) {
  if (pl_is_nat63(*a))
    return *a == 0 ? 0 : *a - 1;
  size_t n;
  uint64_t* out = copy_limbs(*a, &n);
  for (size_t i = 0; i < n; i++) {
    if (out[i]-- != 0)
      break;
  }
  pl_val r = nat_from_limbs(t, out, n);
  free(out);
  return r;
}

pl_val pl_nat_add(pl_thread* t, pl_val* a, pl_val* b) {
  if (pl_is_nat63(*a) && pl_is_nat63(*b)) {
    uint64_t s;
    if (!__builtin_add_overflow(*a, *b, &s) && s <= PL_NAT63_MAX)
      return s;
  }
  uint64_t ta, tb;
  pl_limbs va = pl_limb_view(a, &ta);
  pl_limbs vb = pl_limb_view(b, &tb);
  size_t lr = (va.n > vb.n ? va.n : vb.n) + 1;
  uint64_t* out = calloc(lr, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  unsigned __int128 carry = 0;
  for (size_t i = 0; i < lr - 1; i++) {
    unsigned __int128 s = carry;
    if (i < va.n)
      s += va.p[i];
    if (i < vb.n)
      s += vb.p[i];
    out[i] = (uint64_t)s;
    carry = s >> 64u;
  }
  out[lr - 1] = (uint64_t)carry;
  pl_val r = nat_from_limbs(t, out, lr);
  free(out);
  return r;
}

pl_val pl_nat_sub(pl_thread* t, pl_val* a, pl_val* b) {
  if (pl_nat_cmp(*b, *a) >= 0)
    return 0;
  if (pl_is_nat63(*a) && pl_is_nat63(*b))
    return *a - *b;
  uint64_t ta, tb;
  pl_limbs va = pl_limb_view(a, &ta);
  pl_limbs vb = pl_limb_view(b, &tb);
  uint64_t* out = calloc(va.n, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  uint64_t borrow = 0;
  for (size_t i = 0; i < va.n; i++) {
    uint64_t bi = i < vb.n ? vb.p[i] : 0;
    uint64_t sub = bi + borrow;
    borrow = (sub < bi || va.p[i] < sub) ? 1 : 0;
    out[i] = va.p[i] - sub;
  }
  pl_val r = nat_from_limbs(t, out, va.n);
  free(out);
  return r;
}

pl_val pl_nat_mul(pl_thread* t, pl_val* a, pl_val* b) {
  if (pl_is_nat63(*a) && pl_is_nat63(*b)) {
    uint64_t p;
    if (!__builtin_mul_overflow(*a, *b, &p) && p <= PL_NAT63_MAX)
      return p;
  }
  if (*a == 0 || *b == 0)
    return 0;
  uint64_t ta, tb;
  pl_limbs va = pl_limb_view(a, &ta);
  pl_limbs vb = pl_limb_view(b, &tb);
  size_t lr = va.n + vb.n;
  uint64_t* out = calloc(lr, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  for (size_t i = 0; i < va.n; i++) {
    unsigned __int128 carry = 0;
    for (size_t j = 0; j < vb.n; j++) {
      unsigned __int128 cur =
          (unsigned __int128)va.p[i] * vb.p[j] + out[i + j] + carry;
      out[i + j] = (uint64_t)cur;
      carry = cur >> 64u;
    }
    out[i + vb.n] += (uint64_t)carry;
  }
  pl_val r = nat_from_limbs(t, out, lr);
  free(out);
  return r;
}

static int limb_cmp(const uint64_t* a, size_t an, const uint64_t* b,
                    size_t bn) {
  an = trim_n((uint64_t*)a, an);
  bn = trim_n((uint64_t*)b, bn);
  if (an != bn)
    return an < bn ? -1 : 1;
  for (size_t i = an; i > 0; i--) {
    if (a[i - 1] != b[i - 1])
      return a[i - 1] < b[i - 1] ? -1 : 1;
  }
  return 0;
}

static void limb_sub_inplace(uint64_t* a, size_t an, const uint64_t* b,
                             size_t bn) {
  uint64_t borrow = 0;
  for (size_t i = 0; i < an; i++) {
    uint64_t bi = i < bn ? b[i] : 0;
    uint64_t sub = bi + borrow;
    borrow = (sub < bi || a[i] < sub) ? 1 : 0;
    a[i] -= sub;
  }
}

static pl_val pl_nat_divmod(pl_thread* t, pl_val* a, pl_val* b, bool mod_f) {
  if (*b == 0)
    pl_raise_msg(t, "division by zero");
  if (pl_is_nat63(*a) && pl_is_nat63(*b))
    return mod_f ? *a % *b : *a / *b;
  if (pl_nat_cmp(*a, *b) < 0)
    return mod_f ? *a : 0;

  size_t an, bn;
  uint64_t* av = copy_limbs(*a, &an);
  uint64_t* bv = copy_limbs(*b, &bn);
  size_t qn = an;
  uint64_t* q = calloc(qn, sizeof(uint64_t));
  uint64_t* r = calloc(an + 1, sizeof(uint64_t));
  ax_assume(q != NULL && r != NULL, "oom");

  size_t bits = pl_nat_bit_len(*a);
  for (size_t pos = bits; pos > 0; pos--) {
    uint64_t bit = pl_nat_limb_at(*a, (pos - 1) / 64) >> ((pos - 1) % 64);
    uint64_t carry = bit & 1u;
    for (size_t i = 0; i < an; i++) {
      uint64_t next = r[i] >> 63u;
      r[i] = (r[i] << 1u) | carry;
      carry = next;
    }
    if (limb_cmp(r, an, bv, bn) >= 0) {
      limb_sub_inplace(r, an, bv, bn);
      q[(pos - 1) / 64] |= UINT64_C(1) << ((pos - 1) % 64);
    }
  }

  pl_val out = nat_from_limbs(t, mod_f ? r : q, mod_f ? an : qn);
  free(av);
  free(bv);
  free(q);
  free(r);
  return out;
}

pl_val pl_nat_div(pl_thread* t, pl_val* a, pl_val* b) {
  return pl_nat_divmod(t, a, b, false);
}

pl_val pl_nat_mod(pl_thread* t, pl_val* a, pl_val* b) {
  return pl_nat_divmod(t, a, b, true);
}

pl_val pl_nat_lsh(pl_thread* t, pl_val* a, pl_val* sh) {
  if (*a == 0)
    return 0;
  uint64_t cnt = pl_nat_u64_clamp(*sh);
  uint64_t ta;
  pl_limbs va = pl_limb_view(a, &ta);
  size_t limb_shift = (size_t)(cnt / 64);
  uint32_t bit_shift = (uint32_t)(cnt % 64);
  size_t lr = va.n + limb_shift + 1;
  ax_assume(lr < (1u << 20), "left shift result too large");
  uint64_t* out = calloc(lr, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  for (size_t i = 0; i < va.n; i++) {
    out[i + limb_shift] |= va.p[i] << bit_shift;
    if (bit_shift != 0)
      out[i + limb_shift + 1] |= va.p[i] >> (64u - bit_shift);
  }
  pl_val r = nat_from_limbs(t, out, lr);
  free(out);
  return r;
}

pl_val pl_nat_rsh(pl_thread* t, pl_val* a, pl_val* sh) {
  uint64_t cnt = pl_nat_u64_clamp(*sh);
  if (pl_is_nat63(*a))
    return cnt >= 64 ? 0 : *a >> cnt;
  uint64_t ta;
  pl_limbs va = pl_limb_view(a, &ta);
  size_t limb_shift = (size_t)(cnt / 64);
  uint32_t bit_shift = (uint32_t)(cnt % 64);
  if (limb_shift >= va.n)
    return 0;
  size_t lr = va.n - limb_shift;
  uint64_t* out = calloc(lr, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  for (size_t i = 0; i < lr; i++) {
    out[i] = va.p[i + limb_shift] >> bit_shift;
    if (bit_shift != 0 && i + limb_shift + 1 < va.n)
      out[i] |= va.p[i + limb_shift + 1] << (64u - bit_shift);
  }
  pl_val r = nat_from_limbs(t, out, lr);
  free(out);
  return r;
}

pl_val pl_nat_bex(pl_thread* t, pl_val* bits) {
  uint64_t n = pl_nat_u64_clamp(*bits);
  if (n < 63)
    return UINT64_C(1) << n;
  size_t lr = (size_t)(n / 64) + 1;
  ax_assume(lr < (1u << 20), "bex result too large");
  uint64_t* out = calloc(lr, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  out[n / 64] = UINT64_C(1) << (n % 64);
  pl_val r = nat_from_limbs(t, out, lr);
  free(out);
  return r;
}

pl_val pl_nat_set_bit(pl_thread* t, pl_val* bit, pl_val* a) {
  uint64_t i = pl_nat_u64_clamp(*bit);
  if (pl_is_nat63(*a) && i < 63)
    return *a | (UINT64_C(1) << i);
  size_t n;
  uint64_t* out = copy_limbs(*a, &n);
  size_t lr = (size_t)(i / 64) + 1;
  if (lr > n) {
    out = realloc(out, lr * sizeof(uint64_t));
    ax_assume(out != NULL, "oom");
    memset(out + n, 0, (lr - n) * sizeof(uint64_t));
    n = lr;
  }
  out[i / 64] |= UINT64_C(1) << (i % 64);
  pl_val r = nat_from_limbs(t, out, n);
  free(out);
  return r;
}

pl_val pl_nat_clear_bit(pl_thread* t, pl_val* bit, pl_val* a) {
  uint64_t i = pl_nat_u64_clamp(*bit);
  if (pl_is_nat63(*a))
    return i < 63 ? (*a & ~(UINT64_C(1) << i)) : *a;
  size_t n;
  uint64_t* out = copy_limbs(*a, &n);
  if (i / 64 < n)
    out[i / 64] &= ~(UINT64_C(1) << (i % 64));
  pl_val r = nat_from_limbs(t, out, n);
  free(out);
  return r;
}

pl_val pl_nat_trunc(pl_thread* t, pl_val* width, pl_val* a) {
  uint64_t w = pl_nat_u64_clamp(*width);
  if (w == 0)
    return 0;
  if (pl_nat_bit_len(*a) <= w)
    return *a;
  if (pl_is_nat63(*a))
    return w >= 63 ? *a : (*a & ((UINT64_C(1) << w) - 1));
  size_t lr = (size_t)((w + 63) / 64);
  size_t n;
  uint64_t* out = copy_limbs(*a, &n);
  if (lr < n)
    n = lr;
  if (w % 64 != 0)
    out[lr - 1] &= (UINT64_C(1) << (w % 64)) - 1;
  pl_val r = nat_from_limbs(t, out, n);
  free(out);
  return r;
}

static uint64_t clamp_to_width(uint64_t w, uint64_t a) {
  if (w >= 8)
    return 0;
  return a & ((UINT64_C(1) << (w * 8)) - 1);
}

pl_val pl_nat_load_var(pl_thread* t, pl_val* off, pl_val* width, pl_val* a) {
  uint64_t w = pl_nat_u64_clamp(*width);
  uint64_t o = pl_nat_u64_clamp(*off);
  if (w == 0)
    return 0;
  if (pl_is_nat63(*a))
    return clamp_to_width(w, *a >> (8 * o));
  size_t n = (size_t)w;
  uint8_t* bytes = calloc(n, 1);
  ax_assume(bytes != NULL, "oom");
  for (size_t i = 0; i < n; i++)
    bytes[i] = pl_nat_byte_at(*a, (size_t)o + i);
  pl_val r = pl_nat_from_bytes(t, bytes, n);
  free(bytes);
  return r;
}

pl_val pl_nat_store_byte(pl_thread* t, pl_val* idx, pl_val* byte, pl_val* a) {
  uint64_t i = pl_nat_u64_clamp(*idx);
  uint8_t b = (uint8_t)pl_nat_u64_clamp(pl_nat_coerce(*byte));
  size_t n;
  uint64_t* out = copy_limbs(*a, &n);
  size_t lr = (size_t)(i / 8) + 1;
  if (lr > n) {
    out = realloc(out, lr * sizeof(uint64_t));
    ax_assume(out != NULL, "oom");
    memset(out + n, 0, (lr - n) * sizeof(uint64_t));
    n = lr;
  }
  ((uint8_t*)out)[i] = b;
  pl_val r = nat_from_limbs(t, out, n);
  free(out);
  return r;
}

pl_val pl_nat_from_bytes(pl_thread* t, const uint8_t* b, size_t n) {
  while (n > 0 && b[n - 1] == 0)
    n--;
  if (n == 0)
    return 0;
  if (n < 8) {
    uint64_t v = 0;
    memcpy(&v, b, n);
    return v;
  }
  size_t limbs = (n + 7) / 8;
  uint64_t* out = calloc(limbs, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  memcpy(out, b, n);
  pl_val r = nat_from_limbs(t, out, limbs);
  free(out);
  return r;
}

static void limbs_mul_small(uint64_t** p, size_t* n, size_t* cap, uint32_t m) {
  unsigned __int128 carry = 0;
  for (size_t i = 0; i < *n; i++) {
    unsigned __int128 x = (unsigned __int128)(*p)[i] * m + carry;
    (*p)[i] = (uint64_t)x;
    carry = x >> 64u;
  }
  if (carry != 0) {
    if (*n == *cap) {
      *cap *= 2;
      *p = realloc(*p, *cap * sizeof(uint64_t));
      ax_assume(*p != NULL, "oom");
    }
    (*p)[(*n)++] = (uint64_t)carry;
  }
}

static void limbs_add_small(uint64_t** p, size_t* n, size_t* cap, uint32_t a) {
  if (*n == 0) {
    (*p)[0] = a;
    *n = a == 0 ? 0 : 1;
    return;
  }
  uint64_t old = (*p)[0];
  (*p)[0] += a;
  uint64_t carry = (*p)[0] < old ? 1 : 0;
  for (size_t i = 1; carry != 0 && i < *n; i++) {
    old = (*p)[i];
    (*p)[i]++;
    carry = (*p)[i] < old ? 1 : 0;
  }
  if (carry != 0) {
    if (*n == *cap) {
      *cap *= 2;
      *p = realloc(*p, *cap * sizeof(uint64_t));
      ax_assume(*p != NULL, "oom");
    }
    (*p)[(*n)++] = 1;
  }
}

pl_val pl_nat_from_decimal(pl_thread* t, const char* s, size_t n, bool* ok) {
  size_t cap = 4;
  size_t limbs = 0;
  uint64_t* out = calloc(cap, sizeof(uint64_t));
  ax_assume(out != NULL, "oom");
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      free(out);
      *ok = false;
      return 0;
    }
    limbs_mul_small(&out, &limbs, &cap, 10);
    limbs_add_small(&out, &limbs, &cap, (uint32_t)(s[i] - '0'));
  }
  pl_val r = nat_from_limbs(t, out, limbs);
  free(out);
  *ok = true;
  return r;
}
