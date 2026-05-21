#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <gmp.h>
#include "enki/allocator.h"

typedef uint64_t enki_value;
typedef struct enki_gc enki_gc;


// check if highest bit of v is set 
#define IS_PTR(v) (v & (1ULL << 63)) 
 // turn a real pointer into enki_value - set the higest bit
#define PTR_TO_ENKI(v) (enki_value)((uintptr_t)v | (1ULL << 63))
// turn an enki value into a real pointer - unset highest bit
#define ENKI_TO_PTR(v) (void*)((uintptr_t)v & ~(1ULL << 63))
#define GET_PAYLOAD(v) (sizeof(enki_value_header) + (char*)ENKI_TO_PTR(v))
#define ENKI_LAW_CONSTS(l)   ((enki_value*)((l)->data))
#define ENKI_LAW_BC(l)  ((l)->data + ((l)->n_const * sizeof(enki_value)))
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
    uint8_t kind;
    size_t size;
    uint8_t state;
} obj_header;

typedef obj_header enki_value_header;

typedef struct {
    obj_header h; 
    uint8_t hash[32];
    enki_value inner;
    size_t n_subpins;
    enki_value subpins[];
} enki_pin;

typedef struct {
    obj_header h; 
    uint32_t arity;
    enki_value name;
    enki_value body;
    size_t bc_len;
    size_t n_const;
    uint8_t data[];
} enki_law;

typedef struct {
    obj_header h; 
    size_t n_limbs; 
    mp_limb_t limbs[];
} enki_nat;

typedef struct {
    obj_header h;
    enki_value fn; 
    size_t n_args;
    enki_value args[];
}  enki_app; 

typedef struct {
    obj_header h;
    size_t n_args;
    enki_value args[];
} enki_cont;


void enki_trace_value(enki_gc* gc, void* obj);

enki_value enki_alloc_nat(enki_gc* gc, mp_limb_t* out, size_t n_limbs);
enki_value enki_alloc_big_nat(enki_gc* gc, size_t n_limbs, mp_limb_t limbs[]);
enki_value enki_alloc_law(enki_gc* gc, size_t arity, enki_value name, enki_value body, 
    size_t bc_len, size_t n_const, uint8_t* bc, enki_value* const_table);
enki_value enki_alloc_pin(enki_gc* gc, const uint8_t hash[32], enki_value inner, size_t n_subpins, enki_value subpins[]); 
enki_value enki_alloc_app(enki_gc* gc, enki_value fn, size_t n_args);
enki_value enki_alloc_cont(enki_gc* gc, size_t n_args, enki_value* bas);

bool enki_nat_is_zero(enki_value x);
int enki_nat_cmp(enki_value a, enki_value b);
enki_value enki_nat_eq(enki_value a, enki_value b);
enki_value enki_nat_ne(enki_value a, enki_value b);
enki_value enki_nat_lt(enki_value a, enki_value b);
enki_value enki_nat_le(enki_value a, enki_value b);
enki_value enki_nat_gt(enki_value a, enki_value b);
enki_value enki_nat_ge(enki_value a, enki_value b);
enki_value enki_nat_inc(enki_gc* gc, enki_value a);
enki_value enki_nat_dec(enki_gc* gc, enki_value a);
enki_value enki_nat_add(enki_gc* gc, enki_value a, enki_value b);
enki_value enki_nat_sub(enki_gc* gc, enki_value a, enki_value b);
enki_value enki_nat_mul(enki_gc* gc, enki_value a, enki_value b);
enki_value enki_nat_div(enki_gc* gc, enki_value a, enki_value b);
enki_value enki_nat_mod(enki_gc* gc, enki_value a, enki_value b);
enki_value enki_nat_lsh(enki_gc* gc, enki_value a, enki_value shift);
enki_value enki_nat_rsh(enki_gc* gc, enki_value a, enki_value shift);
enki_value enki_nat_bex(enki_gc* gc, enki_value bit);
enki_value enki_nat_test(enki_gc* gc, enki_value bit, enki_value a);
enki_value enki_nat_set(enki_gc* gc, enki_value bit, enki_value a);
enki_value enki_nat_clear(enki_gc* gc, enki_value bit, enki_value a);
enki_value enki_nat_bits(enki_gc* gc, enki_value a);
enki_value enki_nat_bytes(enki_gc* gc, enki_value a);
enki_value enki_nat_trunc(enki_gc* gc, enki_value width, enki_value a);
enki_value enki_nat_trunc8(enki_gc* gc, enki_value a);
enki_value enki_nat_trunc16(enki_gc* gc, enki_value a);
enki_value enki_nat_trunc32(enki_gc* gc, enki_value a);
enki_value enki_nat_trunc64(enki_gc* gc, enki_value a);
enki_value enki_nat_load8(enki_gc* gc, enki_value index, enki_value a);
enki_value enki_nat_store8(enki_gc* gc, enki_value index, enki_value byte, enki_value a);
enki_value enki_nat_nib(enki_gc* gc, enki_value index, enki_value a);
