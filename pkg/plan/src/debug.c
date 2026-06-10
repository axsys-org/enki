#include "plan/debug.h"

#include <ctype.h>
#include <gmp.h>
#include <string.h>

#include "axsys/sb.h"
#include "plan/nat.h"
#include "plan/value.h"

static void pl_gmp_free(void* ptr, size_t size_s) {
  if (ptr == NULL)
    return;
  void (*free_fn)(void*, size_t) = NULL;
  mp_get_memory_functions(NULL, NULL, &free_fn);
  free_fn(ptr, size_s);
}

static bool pl_nat_printable(pl_val v) {
  size_t n = pl_nat_byte_len(v);
  if (n == 0 || (pl_is_nat63(v) && v < 256))
    return false;
  for (size_t i = 0; i < n; i++) {
    if (!isprint(pl_nat_byte_at(v, i)))
      return false;
  }
  return true;
}

static void pl_show_nat(ax_sb* sb, pl_val v) {
  if (pl_nat_printable(v)) {
    ax_sb_append_lit(sb, "\"");
    size_t n = pl_nat_byte_len(v);
    for (size_t i = 0; i < n; i++)
      ax_sb_append_char(sb, (char)pl_nat_byte_at(v, i));
    ax_sb_append_lit(sb, "\"");
    return;
  }
  if (pl_is_nat63(v)) {
    ax_sb_append_u64(sb, v);
    return;
  }
  mpz_t z;
  mpz_init(z);
  size_t limbs = pl_nat_limb_len(v);
  mpz_import(z, limbs, -1, sizeof(uint64_t), 0, 0, pl_nat_limb_ptr(pl_ptr(v)));
  char* dec = mpz_get_str(NULL, 10, z);
  ax_sb_append_cstr(sb, dec);
  pl_gmp_free(dec, strlen(dec) + 1);
  mpz_clear(z);
}

static bool pl_show_name_raw(ax_sb* sb, pl_val name) {
  if (!pl_is_nat(name) || !pl_nat_printable(name))
    return false;
  size_t n = pl_nat_byte_len(name);
  for (size_t i = 0; i < n; i++)
    ax_sb_append_char(sb, (char)pl_nat_byte_at(name, i));
  return true;
}

void pl_show_sb(ax_sb* sb, pl_val v) {
  while (!pl_is_nat63(v) && pl_tag(v) == PL_TAG_DEFER &&
         pl_hdr_kind(*pl_ptr(v)) == PL_K_IND)
    v = pl_ind_target(pl_ptr(v));

  if (pl_is_nat(v)) {
    pl_show_nat(sb, v);
    return;
  }
  pl_cell* p = pl_ptr(v);
  switch (pl_tag(v)) {
  case PL_TAG_APP: {
    ax_sb_append_lit(sb, "(");
    pl_show_sb(sb, pl_app_head(p));
    uint32_t n = pl_app_n(p);
    for (uint32_t i = 0; i < n; i++) {
      ax_sb_append_lit(sb, " ");
      pl_show_sb(sb, pl_app_args(p)[i]);
    }
    ax_sb_append_lit(sb, ")");
    return;
  }
  case PL_TAG_LAW: {
    ax_sb_append_lit(sb, "{");
    if (!pl_show_name_raw(sb, pl_law_name(p)))
      pl_show_sb(sb, pl_law_name(p));
    ax_sb_append_lit(sb, "/");
    ax_sb_append_u64(sb, pl_law_arity(p));
    ax_sb_append_lit(sb, " ");
    pl_show_sb(sb, pl_law_body(p));
    ax_sb_append_lit(sb, "}");
    return;
  }
  case PL_TAG_PIN:
    ax_sb_append_lit(sb, "<");
    pl_show_sb(sb, pl_pin_body(p));
    ax_sb_append_lit(sb, ">");
    return;
  case PL_TAG_ENV:
    ax_sb_append_lit(sb, "<env/");
    ax_sb_append_u64(sb, pl_env_n(p));
    ax_sb_append_lit(sb, ">");
    return;
  case PL_TAG_DEFER:
    if (pl_hdr_kind(p[0]) == PL_K_BH)
      ax_sb_append_lit(sb, "<bh>");
    else
      ax_sb_append_lit(sb, "<thk>");
    return;
  default:
    ax_sb_append_lit(sb, "<<bad>>");
    return;
  }
}

char* pl_show(const ax_allocator* a, pl_val v, size_t* out_s) {
  ax_sb sb;
  ax_sb_init(&sb, a);
  pl_show_sb(&sb, v);
  char* out = ax_sb_build(&sb, out_s);
  ax_sb_free(&sb);
  return out;
}
