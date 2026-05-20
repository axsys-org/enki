/*
IMPORTANT:
WILL PORT BACK GC FOR ENKI VALUE ALLOCATION LATER, use enki_allocator temporarily now
enki_alloc_nat owns/frees p_limbs
all returned heap values are caller/arena-owned
enki_alloc_app copies p_args if present
*/
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "enki/allocator.h"

typedef uint64_t enki_value;
typedef uint64_t mp_limb_t;

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
    ENKI_NAT,
} TAGS;

typedef struct {
    uint8_t kind;
    size_t size;
} obj_header;

typedef struct {
    obj_header h; 
    uint8_t hash[32];
    enki_value inner;
    // port back
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

// owns p_limbs
// caller owns result
enki_value enki_alloc_nat(enki_allocator a_alloc, mp_limb_t* p_limbs, size_t n_limbs);
// p_bc optional if cb_bc is 0
// p_const_table optional if n_const is 0
// caller owns result
enki_value enki_alloc_law(enki_allocator a_alloc, size_t n_arity, enki_value v_name, enki_value v_body, 
    size_t cb_bc, size_t n_const, uint8_t* p_bc, enki_value* p_const_table);
// p_hash required
// p_subpins optional if n_subpins is 0
// caller owns result
enki_value enki_alloc_pin(enki_allocator a_alloc, const uint8_t p_hash[32], enki_value v_inner, size_t n_subpins, enki_value p_subpins[]); 
// p_args optional
// caller owns result
enki_value enki_alloc_app(enki_allocator a_alloc, enki_value v_fn, size_t n_args, enki_value* p_args);
