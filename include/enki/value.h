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
typedef enum {
    ENKI_PIN,
    ENKI_LAW,
    ENKI_APP,
    ENKI_BIG_NAT,
    ENKI_FWD
} TAGS;

typedef struct {
    uint8_t kind;
    size_t size;
} obj_header;

typedef struct {
    obj_header h; 
    uint8_t hash[32];
    enki_value inner;
} enki_pin;

typedef struct {
    obj_header h; 
    uint32_t arity;
    enki_value name;
    enki_value body;
    size_t bc_len;
    uint8_t  bc[];
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

void enki_trace_value(enki_gc* gc, void* obj);

enki_value enki_alloc_nat(enki_gc* gc, size_t n_bytes, uint8_t bytes[]);
enki_value enki_alloc_law(enki_gc* gc, uint32_t arity, enki_value name, enki_value body, uint32_t bc_len, const uint8_t bc[]);
enki_value enki_alloc_pin(enki_gc* gc, const uint8_t hash[32], enki_value inner, size_t n_subpins, enki_value subpins[]); 
enki_value enki_alloc_app(enki_gc* gc, enki_value fn, size_t n_args);


/*

(uint64_t)value & (1ULL << N) - is bit N set?
(uint64_t)value | (1ULL << N) - set bit N 
(uint64_t)value & ~(1ULL << N) - clear bit N 

enki_value: [0 | 63 bit nat data ]

eg. 
[0 | 0] = nat 0
[0 | 42 ] = nat 42
[0 | u63max ] = biggest unboxed nat 

[1 | 63-but pointer ]
[ NAT tag 0x8200 ][ size ][ n_limbs ][ limb0 ][ limb1 ] ... [ limbN ]
[ PIN tag 0xC40x ][ size ][ hash/crc fields ][ inner: enki_value ][ subpins... maybe ]
[ LAW tag 0xC800 ][ size ][ arity ][ name: enki_value ][ body: enki_value ][ bytecode... ]
[ FWD tag ][ new_ptr ]


to interpret :

IS_NAT -
    input: enki_value 
    output: true/false
    is the high bit 0 
IS_PTR -
    input: enki_value 
    output: bool/int 
    is the high bit 1 
MAKE_PTR -
    input: void* a real heap pointer 
    uintptr_t
    output: enki_value - cast needed 
    given a void* return a int64 with the lowest bit set 
AS_PTR 
    input: enki_value
    uintptr_t
    output: void* - cast needed 
    return without lowest bit set 
*/
