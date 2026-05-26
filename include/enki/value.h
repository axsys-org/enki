#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <gmp.h>
#include "enki/allocator.h"

typedef uint64_t enki_value;
typedef struct enki_gc enki_gc;


// check if highest bit_v of v is set
#define IS_PTR(v) (v & (1ULL << 63))
 // turn a_v real pointer into enki_value - set the higest bit_v
#define PTR_TO_ENKI(v) (enki_value)((uintptr_t)v | (1ULL << 63))
// turn an enki value_v into a_v real pointer - unset highest bit_v
#define ENKI_TO_PTR(v) (void*)((uintptr_t)v & ~(1ULL << 63))
#define GET_PAYLOAD(v) (sizeof(enki_value_header) + (char*)ENKI_TO_PTR(v))
#define ENKI_LAW_CONSTS(l)   ((enki_value*)((l)->data_b))
#define ENKI_LAW_BC(l)  ((l)->data_b + ((l)->n_const_s * sizeof(enki_value)))

#define ENKI_TO_APP(v) \
  ((IS_PTR(v) && ((obj_header*)v)->kind_b == ENKI_APP) ? (ENKI_TO_PTR(v)) : NULL)
#define ENKI_TO_LAW(v) \
  ((IS_PTR(v) && ((obj_header*)v)->kind_b == ENKI_LAW) ? (ENKI_TO_PTR(v)) : NULL)
typedef enum {
    ENKI_PIN,
    ENKI_LAW,
    ENKI_APP,
    ENKI_BIG_NAT,
    ENKI_FWD,
    ENKI_CONT,
} TAGS;

#define ENKI_NAT ENKI_BIG_NAT
#define ENKI_FRWD ENKI_FWD
#define PIN ENKI_PIN
#define LAW ENKI_LAW
#define APP ENKI_APP
#define NAT ENKI_NAT

typedef enum {
    WHNF,
    NF,
    THUNK,
} STATES;

typedef struct {
    uint8_t kind_b;
    size_t size_s;
    uint8_t state_b;
} obj_header;

typedef obj_header enki_value_header;

typedef struct {
    obj_header h;
    uint8_t hash_b[32];
    enki_value inner_v;
    size_t n_subpins_s;
    enki_value subpins_v[];
} enki_pin;

typedef struct {
    obj_header h;
    uint32_t arity_s;
    enki_value name_v;
    enki_value body_v;
    size_t bc_len_s;
    size_t n_const_s;
    uint8_t data_b[];
} enki_law;

typedef struct {
    obj_header h;
    size_t n_limbs_s;
    mp_limb_t limbs[];
} enki_nat;

typedef struct {
    obj_header h;
    enki_value fn_v;
    size_t n_args_s;
    enki_value args_v[];
}  enki_app;

typedef struct {
    obj_header h;
    size_t n_args_s;
    enki_value args_v[];
} enki_cont;


void enki_trace_value(enki_gc* gc, void* obj);

enki_value enki_alloc_nat(enki_gc* gc, mp_limb_t* out, size_t n_limbs_s);
enki_value enki_alloc_big_nat(enki_gc* gc, size_t n_limbs_s, mp_limb_t limbs[]);
enki_value enki_alloc_law(enki_gc* gc, size_t arity_s, enki_value name_v, enki_value body_v,
    size_t bc_len_s, size_t n_const_s, uint8_t* bc_b, enki_value* const_table_v);
enki_value enki_alloc_pin(enki_gc* gc, const uint8_t hash_b[32], enki_value inner_v, size_t n_subpins_s, enki_value subpins_v[]);
enki_value enki_alloc_app(enki_gc* gc, enki_value fn_v, size_t n_args_s);
enki_value enki_alloc_cont(enki_gc* gc, size_t n_args_s, enki_value* bas_v);

bool enki_nat_is_zero(enki_value x_v);
int enki_nat_cmp(enki_value a_v, enki_value b_v);
enki_value enki_nat_eq(enki_value a_v, enki_value b_v);
enki_value enki_nat_ne(enki_value a_v, enki_value b_v);
enki_value enki_nat_lt(enki_value a_v, enki_value b_v);
enki_value enki_nat_le(enki_value a_v, enki_value b_v);
enki_value enki_nat_gt(enki_value a_v, enki_value b_v);
enki_value enki_nat_ge(enki_value a_v, enki_value b_v);
enki_value enki_nat_inc(enki_gc* gc, enki_value a_v);
enki_value enki_nat_dec(enki_gc* gc, enki_value a_v);
enki_value enki_nat_add(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_sub(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_mul(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_div(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_mod(enki_gc* gc, enki_value a_v, enki_value b_v);
enki_value enki_nat_lsh(enki_gc* gc, enki_value a_v, enki_value shift_v);
enki_value enki_nat_rsh(enki_gc* gc, enki_value a_v, enki_value shift_v);
enki_value enki_nat_bex(enki_gc* gc, enki_value bit_v);
enki_value enki_nat_test(enki_gc* gc, enki_value bit_v, enki_value a_v);
enki_value enki_nat_set(enki_gc* gc, enki_value bit_v, enki_value a_v);
enki_value enki_nat_clear(enki_gc* gc, enki_value bit_v, enki_value a_v);
enki_value enki_nat_bits(enki_gc* gc, enki_value a_v);
enki_value enki_nat_bytes(enki_gc* gc, enki_value a_v);
size_t enki_bat_met_bytes(enki_value a_v);
enki_value enki_nat_trunc(enki_gc* gc, enki_value width_v, enki_value a_v);
enki_value enki_nat_trunc8(enki_gc* gc, enki_value a_v);
enki_value enki_nat_trunc16(enki_gc* gc, enki_value a_v);
enki_value enki_nat_trunc32(enki_gc* gc, enki_value a_v);
enki_value enki_nat_trunc64(enki_gc* gc, enki_value a_v);
enki_value enki_nat_load8(enki_gc* gc, enki_value index_i, enki_value a_v);
enki_value enki_nat_store8(enki_gc* gc, enki_value index_i, enki_value byte_b, enki_value a_v);
enki_value enki_nat_nib(enki_gc* gc, enki_value index_i, enki_value a_v);


enki_value enki_alloc_strnat(enki_gc* gc, char* str_c, size_t str_s);
enki_value enki_alloc_cstrnat(enki_gc* gc, char* str_c);
enki_value enki_alloc_big_nat_bytes(enki_gc* gc, size_t lim_s, char* lim_b);
enki_value enki_alloc_pair(enki_gc* gc, enki_value l_v, enki_value r_v);
enki_value enki_alloc_trel(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v);

enki_value enki_alloc_quad(
    enki_gc* gc,
    enki_value fn_v,
    enki_value one_v,
    enki_value two_v,
    enki_value tri_v
);
enki_value enki_alloc_row(
    enki_gc* gc,
    enki_value fn_v,
    size_t arg_s,
    enki_value* arg_v
);

enki_value enki_app_hd(enki_value app_v);
enki_value enki_app_idx(enki_value app_v, size_t idx_s);
enki_app* enki_alloc_app_bare(enki_gc* gc, enki_value fn_v, size_t n_args_s);
enki_value enki_alloc_quin(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v,
                           enki_value tri_v, enki_value qua_v);

enki_value enki_make_pin(enki_gc* gc, enki_value val_v);


enki_value enki_app_weld(enki_gc* gc, enki_app* old, size_t add_s, enki_value*
add_v);

enki_value enki_unpin(enki_value pin_v);

int enki_nat_le_bool(enki_value a_v, enki_value b_v);

enki_value enki_alloc_big_nat_bytes(enki_gc* gc, size_t byt_s, char* byt_b);
