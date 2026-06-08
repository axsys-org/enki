#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <gmp.h>
#include <openssl/sha.h>

#include "enki/interp.h"
#include "enki/app.h"
#include "enki/gc.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/pin.h"
#include "enki/value.h"
#include "enki/vector.h"

#define VERSION 0x01
enki_value enki_pin_alloc(enki_gc* gc, const uint8_t hash_b[32],
                          enki_value inner_v, size_t n_subpins_s,
                          enki_value subpins_v[]) {
  size_t n = sizeof(enki_pin) + (n_subpins_s * sizeof(enki_value));
  enki_pin* new = (enki_pin*)enki_gc_alloc_locked(gc, n, _Alignof(enki_pin));
  new->h.size_s = n;
  new->h.kind_b = ENKI_PIN;
  new->h.state_b = WHNF;
  new->inner_v = inner_v;
  new->n_subpins_s = n_subpins_s;
  memcpy(new->hash_b, hash_b, 32);
  if (n_subpins_s > 0)
    memcpy(new->subpins_v, subpins_v, n_subpins_s * sizeof(enki_value));
  return PTR_TO_ENKI(new);
}

enki_value enki_pin_deserialize(enki_interpreter* i, uint8_t* buff,
                                size_t n_buff, size_t* off) {
  if (*off > n_buff || n_buff - *off < 1)
    enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
  uint8_t tag = buff[*off];
  *off += 1;
  switch (tag) {
  case ENKI_NAT: {
    size_t byte_len = 0;
    if (*off > n_buff || n_buff - *off < 8)
      enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
    for (size_t k = 0; k < 8; k++) {
      byte_len |= ((size_t)buff[*off] << (k * 8));
      *off += 1;
    }
    if (*off > n_buff || n_buff - *off < byte_len)
      enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
    if (byte_len <= 8) {
      enki_value imm = 0;
      for (size_t k = 0; k < byte_len; k++) {
        imm |= ((enki_value)buff[*off + k] << (k * 8));
      }
      *off += byte_len;
      return imm;
    }
    size_t n_limbs = (byte_len + 7) / 8;
    size_t n = sizeof(enki_nat) + (n_limbs * sizeof(mp_limb_t));
    enki_nat* nat = (enki_nat*)i->gc->alloc(i->gc, n, _Alignof(enki_nat));
    nat->h.size_s = n;
    nat->h.kind_b = ENKI_NAT;
    nat->h.state_b = NF;
    nat->n_limbs_s = n_limbs;
    i->stack_v[i->sp++] = PTR_TO_ENKI(nat);
    memset(nat->limbs, 0, n_limbs * sizeof(mp_limb_t));
    for (size_t k = 0; k < byte_len; k++) {
      size_t limb_i = k / 8;
      size_t shift = (k % 8) * 8;
      nat->limbs[limb_i] |= ((mp_limb_t)buff[*off + k] << shift);
    }
    *off += byte_len;
    return PTR_TO_ENKI(nat);
  }
  case ENKI_PIN: {
    size_t n = sizeof(enki_pin);
    enki_pin* pin = (enki_pin*)i->gc->alloc(i->gc, n, _Alignof(enki_pin));
    pin->h.size_s = n;
    pin->h.kind_b = ENKI_PIN;
    pin->h.state_b = WHNF;
    pin->n_subpins_s = 0;
    pin->inner_v = 0;
    i->stack_v[i->sp++] = PTR_TO_ENKI(pin);
    if (*off > n_buff || n_buff - *off < 32)
      enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
    for (size_t k = 0; k < 32; k++) {
      pin->hash_b[k] = buff[*off + k];
    }
    *off += 32;
    return PTR_TO_ENKI(pin);
  }
  case ENKI_LAW: {
    size_t arity = 0;
    if (*off > n_buff || n_buff - *off < 8)
      enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
    for (size_t k = 0; k < 8; k++) {
      arity |= ((size_t)buff[*off] << (k * 8));
      *off += 1;
    }
    enki_value name = enki_pin_deserialize(i, buff, n_buff, off);
    i->stack_v[i->sp++] = name;
    enki_value body = enki_pin_deserialize(i, buff, n_buff, off);
    i->stack_v[i->sp++] = body;
    enki_vector* bc_v = enki_vector_create_sized_or_throw(
        i, enki_arena_as_allocator(i->scratch_a), sizeof(uint8_t));
    enki_vector* const_v = enki_vector_create_sized_or_throw(
        i, enki_arena_as_allocator(i->scratch_a), sizeof(enki_value));
    enki_law_compile(i, body, (size_t)arity, bc_v, const_v);
    size_t bc_len_s = enki_vector_len(bc_v);
    uint8_t* bc_b = (uint8_t*)enki_vector_data(bc_v);
    size_t n_const_s = enki_vector_len(const_v);
    enki_value* const_table_v = (enki_value*)enki_vector_data(const_v);
    enki_value law = enki_law_alloc(i->gc, arity, name, body, bc_len_s,
                                    n_const_s, bc_b, const_table_v);
    i->stack_v[i->sp++] = law;
    return law;
  }
  case ENKI_APP: {
    enki_value fn = enki_pin_deserialize(i, buff, n_buff, off);
    size_t n_args = 0;
    if (*off > n_buff || n_buff - *off < 8)
      enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
    for (size_t k = 0; k < 8; k++) {
      n_args |= ((size_t)buff[*off] << (k * 8));
      *off += 1;
    }
    enki_value app = enki_app_alloc(i->gc, fn, n_args);
    size_t res_base = i->sp;
    i->stack_v[i->sp++] = app;
    for (size_t k = 0; k < n_args; k++) {
      enki_value arg = enki_pin_deserialize(i, buff, n_buff, off);
      i->stack_v[i->sp++] = arg;
      enki_app* ptr = ENKI_AS(enki_app, i->stack_v[res_base]);
      ptr->args_v[k] = arg;
    }
    return i->stack_v[res_base];
  }
  default:
    enki_interp_throw(i, ENKI_ERROR_BAD_PIN, 0);
  }
  return 0;
}

void enki_pin_serialize(enki_interpreter* i, enki_value val, enki_vector* out) {
  val = enki_value_unind(val);

  if (!IS_PTR(val)) {
    enki_vector_push_u8_or_throw(i, out, ENKI_NAT);
    size_t byte_len = 0;
    enki_value tmp = val;
    while (tmp != 0) {
      byte_len += 1;
      tmp >>= 8;
    }
    for (size_t k = 0; k < 8; k++) {
      enki_vector_push_u8_or_throw(i, out,
                                   (uint8_t)((byte_len >> (8 * k)) & 0xFF));
    }
    for (size_t k = 0; k < byte_len; k++) {
      enki_vector_push_u8_or_throw(i, out, (uint8_t)((val >> (8 * k)) & 0xFF));
    }
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, val);
  enki_vector_push_u8_or_throw(i, out, h->kind_b);
  switch (h->kind_b) {
  case ENKI_NAT: {
    size_t bytes_len = enki_nat_bytes(i->gc, val);
    for (size_t k = 0; k < 8; k++) {
      enki_vector_push_u8_or_throw(i, out,
                                   (uint8_t)((bytes_len >> (8 * k)) & 0xFF));
    }
    for (size_t k = 0; k < bytes_len; k++) {
      enki_vector_push_u8_or_throw(i, out,
                                   (uint8_t)enki_nat_load8(i->gc, k, val));
    }
    break;
  }
  case ENKI_PIN: {
    enki_pin* pin = ENKI_AS(enki_pin, val);
    for (size_t k = 0; k < 32; k++) {
      enki_vector_push_u8_or_throw(i, out, pin->hash_b[k]);
    }
    break;
  }
  case ENKI_LAW: {
    enki_law* law = ENKI_AS(enki_law, val);
    for (size_t k = 0; k < 8; k++) {
      enki_vector_push_u8_or_throw(i, out,
                                   (uint8_t)((law->arity_s >> (8 * k)) & 0xFF));
    }
    enki_pin_serialize(i, law->name_v, out);
    enki_pin_serialize(i, law->body_v, out);
    break;
  }
  case ENKI_APP: {
    enki_app* app = ENKI_AS(enki_app, val);
    enki_pin_serialize(i, app->fn_v, out);
    for (size_t k = 0; k < 8; k++) {
      enki_vector_push_u8_or_throw(
          i, out, (uint8_t)((app->n_args_s >> (8 * k)) & 0xFF));
    }
    for (size_t k = 0; k < app->n_args_s; k++) {
      enki_pin_serialize(i, app->args_v[k], out);
    }
    break;
  }
  default:
    enki_interp_throw(i, ENKI_ERROR_BAD_TAG, val);
  }
}
void enki_pin_save_root(enki_interpreter* i, enki_value val) {
  uint8_t hash_b[32];
  enki_pin_save(i, val, hash_b);
  uint8_t key[32] = "root";
  enki_store* store = enki_store_current();
  if (store == NULL)
    enki_interp_throw(i, ENKI_STORE_ERROR, val);
  enki_error st = store->write(store, key, hash_b, 32);
  if (st != ENKI_ERROR_OK)
    enki_interp_throw(i, (int)st, val);
}
enki_value enki_pin_load_root(enki_interpreter* i) {
  uint8_t key[32] = "root";
  size_t n_out;
  uint8_t hash_b[32];
  enki_store* store = enki_store_current();
  if (store == NULL)
    enki_interp_throw(i, ENKI_STORE_ERROR, 0);
  enki_error st = store->read(store, key, hash_b, 32, &n_out);
  if (st != ENKI_ERROR_OK)
    enki_interp_throw(i, (int)st, 0);
  if (n_out != 32)
    enki_interp_throw(i, ENKI_ERROR_BAD_PIN, 0);
  uint8_t hash_copy_b[32];
  memcpy(hash_copy_b, hash_b, 32);
  return enki_pin_load(i, hash_copy_b);
}
static enki_vector* enki_pin_build(enki_interpreter* i, enki_value val,
                                   uint8_t* hash_b) {
  if (!IS_PTR(val)) {
    enki_interp_throw(i, ENKI_ERROR_TYPE, val);
  }
  enki_value_header* h = ENKI_AS(enki_value_header, val);
  if (h->kind_b != ENKI_PIN) {
    enki_interp_throw(i, ENKI_ERROR_TYPE, val);
  }
  enki_pin* pin = ENKI_AS(enki_pin, val);
  enki_vector* out_subpins_v = enki_vector_create_sized_or_throw(
      i, enki_arena_as_allocator(i->scratch_a), sizeof(enki_value));
  enki_pin_collect_subpins(i, pin->inner_v, out_subpins_v);
  size_t n_subpins_s = enki_vector_len(out_subpins_v);
  enki_value* subpins_v = (enki_value*)enki_vector_data(out_subpins_v);
  enki_vector* out_v = enki_vector_create_sized_or_throw(
      i, enki_arena_as_allocator(i->scratch_a), sizeof(uint8_t));
  enki_vector_push_u8_or_throw(i, out_v, VERSION);
  for (size_t k = 0; k < 8; k++) {
    enki_vector_push_u8_or_throw(i, out_v,
                                 (uint8_t)((n_subpins_s >> (8 * k)) & 0xFF));
  }
  for (size_t k = 0; k < n_subpins_s; k++) {
    uint8_t hash[32];
    enki_pin_save(i, subpins_v[k], hash); // not sure...
    for (size_t j = 0; j < 32; j++) {
      enki_vector_push_u8_or_throw(i, out_v, hash[j]);
    }
  }
  enki_vector* out_inner_v = enki_vector_create_sized_or_throw(
      i, enki_arena_as_allocator(i->scratch_a), sizeof(uint8_t));
  enki_pin_serialize(i, pin->inner_v, out_inner_v);
  size_t n_inner_s = enki_vector_len(out_inner_v);
  uint8_t* inner_v = (uint8_t*)enki_vector_data(out_inner_v);
  for (size_t k = 0; k < n_inner_s; k++) {
    enki_vector_push_u8_or_throw(i, out_v, inner_v[k]);
  }
  SHA256(enki_vector_data(out_v), enki_vector_len(out_v), hash_b);
  return out_v;
}
void enki_pin_hash(enki_interpreter* i, enki_value val, uint8_t* hash_b) {
  enki_pin_build(i, val, hash_b);
}
void enki_pin_save(enki_interpreter* i, enki_value val, uint8_t* hash_b) {
  enki_vector* out_v = enki_pin_build(i, val, hash_b);
  enki_pin* pin = ENKI_AS(enki_pin, val);
  memcpy(pin->hash_b, hash_b, 32);
  uint8_t* out = enki_vector_data(out_v);
  size_t n_out = enki_vector_len(out_v);
  enki_store* store = enki_store_current();
  if (store == NULL)
    enki_interp_throw(i, ENKI_STORE_ERROR, val);
  enki_error st = store->write(store, hash_b, out, n_out);
  if (st != ENKI_ERROR_OK) {
    enki_interp_throw(i, (int)st, val);
  }
}
enki_value enki_pin_load(enki_interpreter* i, uint8_t* hash) {
  size_t off = 0;
  size_t n_out;
  enki_store* store = enki_store_current();
  if (store == NULL)
    enki_interp_throw(i, ENKI_STORE_ERROR, 0);
  enki_error st = store->size(store, hash, &n_out);
  if (st != ENKI_ERROR_OK) {
    enki_interp_throw(i, (int)st, 0);
  }
  uint8_t* out = enki_arena_alloc(i->scratch_a, n_out);
  if (!out) {
    enki_interp_throw(i, ENKI_ERROR_OOM, 0);
  }
  st = store->read(store, hash, out, n_out, &n_out);
  if (st != ENKI_ERROR_OK) {
    enki_interp_throw(i, (int)st, 0);
  }
  if (n_out < 1) {
    enki_interp_throw(i, ENKI_ERROR_BAD_PIN, 0);
  }
  uint8_t ver = out[0];
  if (ver != VERSION) {
    enki_interp_throw(i, ENKI_ERROR_BAD_PIN, 0);
  }
  off += 1;
  if (off > n_out || n_out - off < 8) {
    enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
  }
  size_t n_subpins_s = 0;
  for (size_t k = 0; k < 8; k++) {
    n_subpins_s |= ((size_t)out[off] << (k * 8));
    off += 1;
  }
  size_t res_base = i->sp;
  size_t n = sizeof(enki_pin) + (n_subpins_s * sizeof(enki_value));
  enki_pin* pin = i->gc->alloc(i->gc, n, _Alignof(enki_pin));
  pin->n_subpins_s = n_subpins_s;
  pin->h.size_s = n;
  pin->h.state_b = NF;
  pin->h.kind_b = ENKI_PIN;
  memcpy(pin->hash_b, hash, 32);
  i->stack_v[i->sp++] = PTR_TO_ENKI(pin);
  for (size_t k = 0; k < n_subpins_s; k++) {
    uint8_t hash_b[32];
    if (off > n_out || n_out - off < 32) {
      enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
    }
    memcpy(hash_b, (out + off), 32);
    enki_value v = enki_pin_load(i, hash_b);
    pin = ENKI_AS(enki_pin, i->stack_v[res_base]);
    pin->subpins_v[k] = v;
    off += 32;
  }
  if (off >= n_out) {
    enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
  }
  enki_value inner = enki_pin_deserialize(i, out, n_out, &off);
  pin = ENKI_AS(enki_pin, i->stack_v[res_base]);
  pin->inner_v = inner;
  i->stack_v[res_base] = PTR_TO_ENKI(pin);
  i->sp = res_base;
  return PTR_TO_ENKI(pin);
}
void enki_pin_collect_subpins(enki_interpreter* i, enki_value inner,
                              enki_vector* subpins_v) {
  inner = enki_value_unind(inner);
  if (!IS_PTR(inner))
    return;
  enki_value_header* h = ENKI_AS(enki_value_header, inner);
  switch (h->kind_b) {
  case ENKI_NAT:
    return;
  case ENKI_LAW: {
    enki_law* law = ENKI_AS(enki_law, inner);
    enki_pin_collect_subpins(i, law->name_v, subpins_v);
    enki_pin_collect_subpins(i, law->body_v, subpins_v);
    for (size_t k = 0; k < law->n_const_s; k++) {
      enki_pin_collect_subpins(i, ENKI_LAW_CONSTS(law)[k], subpins_v);
    }
    return;
  }
  case ENKI_PIN:
    enki_vector_push_copy_or_throw(i, subpins_v, &inner);
    return;
  case ENKI_APP: {
    enki_app* app = ENKI_AS(enki_app, inner);
    enki_pin_collect_subpins(i, app->fn_v, subpins_v);
    for (size_t k = 0; k < app->n_args_s; k++) {
      enki_pin_collect_subpins(i, app->args_v[k], subpins_v);
    }
    return;
  }
  default:
    break;
  }
}

static unsigned char empty_pinhash[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

enki_value enki_make_pin(enki_gc* gc, enki_value val_v) {
  return enki_pin_alloc(gc, empty_pinhash, val_v, 0, NULL);
}
