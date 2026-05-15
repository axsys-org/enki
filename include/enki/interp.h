#pragma once
#include "enki/gc.h"

#define STACK_MAX 8096
#define FRAME_MAX 8096

typedef enum {
    NO_OP         = 0x00,
    OP_PUSH_CONST = 0x01,
    OP_PICK       = 0x02,
    OP_POP        = 0x03,
    OP_APPLY      = 0x08,
    OP_DUP        = 0x03,
    OP_OP0        = 0x10,    
    OP_OP66       = 0x42,    
    OP_OP82       = 0x52,    
    OP_RETURN     = 0xFF,
} enki_opcode;

typedef enum {
    OP66_INC = 0,
    OP66_DEC = 1,
    OP66_ADD = 2,
    OP66_SUB = 3,
    OP66_EQ  = 4,
    // ...
} enki_op66_sub;


typedef struct {
    size_t n_const;
    enki_value* const_table;
    size_t bc_len;
    uint8_t* bc;
    size_t pc;
    size_t res_base;
    size_t arg_base;
} enki_frame;

typedef struct {
    size_t sp;
    enki_value stack[STACK_MAX];
    size_t fp;
    enki_frame frame[FRAME_MAX];
    enki_gc* gc;
    enki_allocator sys;
} enki_interpreter;

void enki_trace_interp(enki_interpreter* i);
void enki_run_interpreter(enki_interpreter* i);
enki_interpreter* enki_create_interp(enki_allocator sys, size_t heap, 
    size_t n_const, enki_value* const_table, 
    size_t bc_len, uint8_t bc[]);