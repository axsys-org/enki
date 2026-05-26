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
#define ENKI_AS(type, v) ((type*)((uintptr_t)(v) & ~(UINT64_C(1) << 63)))
#define ENKI_TO_PTR(v) ENKI_AS(void, v)
#define GET_PAYLOAD(v) (sizeof(enki_value_header) + (char*)ENKI_AS(void, v))
#define ENKI_LAW_CONSTS(l)   ((enki_value*)((l)->data_b))
#define ENKI_LAW_BC(l)  ((l)->data_b + ((l)->n_const_s * sizeof(enki_value)))
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
