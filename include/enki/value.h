#pragma once
#include <stddef.h>
#include <stdint.h>
#include <gmp.h>
#include "enki/allocator.h"

typedef uint64_t enki_value;


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

typedef enum {
    WFNF,
    NF,
    THUNK,
} STATES;

typedef struct {
    uint8_t kind;
    size_t size;
    uint8_t state;
} obj_header;

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
enki_value enki_alloc_law(enki_gc* gc, size_t arity, enki_value name, enki_value body, 
    size_t bc_len, size_t n_const, uint8_t* bc, enki_value* const_table);
enki_value enki_alloc_pin(enki_gc* gc, const uint8_t hash[32], enki_value inner, size_t n_subpins, enki_value subpins[]); 
enki_value enki_alloc_app(enki_gc* gc, enki_value fn, size_t n_args);
enki_value enki_alloc_cont(enki_gc* gc, size_t n_args, enki_value* bas);