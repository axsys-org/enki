#pragma once
#include <stdbool.h>
#include "enki/gc.h"

#define STACK_MAX 8096
#define FRAME_MAX 1024

/*
TODO assembler
     big nats 
     op66 ops 
     real pinning 
*/

typedef enum {
    NO_OP         = 0x00,
    OP_PUSH_CONST = 0x01,
    OP_PICK       = 0x02,
    OP_POP        = 0x03,
    OP_APPLY      = 0x08,
    OP_DUP        = 0x09,
    OP_OP0        = 0x10,    
    OP_OP66       = 0x42,    
    OP_OP82       = 0x52,    
    OP_RETURN     = 0xFF,
} enki_opcode;

typedef enum {
    OP66_PIN = 0,
    OP66_LAW,
    OP66_ELIM,

    OP66_INC,
    OP66_DEC,
    OP66_ADD,
    OP66_SUB,
    OP66_MUL,
    OP66_DIV,
    OP66_MOD,

    OP66_EQ,
    OP66_NE,
    OP66_LT,
    OP66_LE,
    OP66_GT,
    OP66_GE,
    OP66_CMP,

    OP66_RSH,
    OP66_LSH,
    OP66_TEST,
    OP66_SET,
    OP66_CLEAR,
    OP66_BEX,
    OP66_BITS,
    OP66_BYTES,
    OP66_NIB,
    OP66_LOAD8,
    OP66_STORE8,
    OP66_LOADVAR,
    OP66_TRUNC,
    OP66_TRUNC8,
    OP66_TRUNC16,
    OP66_TRUNC32,
    OP66_TRUNC64,

    OP66_TYPE,
    OP66_IS_PIN,
    OP66_IS_LAW,
    OP66_IS_APP,
    OP66_IS_NAT,
    OP66_NAT,
    OP66_UNPIN,
    OP66_ARITY,
    OP66_NAME,
    OP66_BODY,
    OP66_HD,
    OP66_LAST,
    OP66_INIT,

    OP66_ROW,
    OP66_REP,
    OP66_SLICE,
    OP66_WELD,
    OP66_UP,
    OP66_UP_UNIQ,
    OP66_COUP,
    OP66_SZ,
    OP66_IX,
    OP66_IX0,
    OP66_IX1,
    OP66_IX2,
    OP66_IX3,
    OP66_IX4,
    OP66_IX5,
    OP66_IX6,
    OP66_IX7,

    OP66_CASE,
    OP66_CASE2,
    OP66_CASE3,
    OP66_CASE4,
    OP66_CASE5,
    OP66_CASE6,
    OP66_CASE7,
    OP66_CASE8,
    OP66_CASE9,
    OP66_CASE10,
    OP66_CASE11,
    OP66_CASE12,
    OP66_CASE13,
    OP66_CASE14,
    OP66_CASE15,
    OP66_CASE16,

    OP66_NIL,
    OP66_TRUTH,
    OP66_OR,
    OP66_NOR,
    OP66_AND,
    OP66_IF,
    OP66_IFZ,

    OP66_SEQ,
    OP66_SEQ2,
    OP66_SEQ3,
    OP66_SAP,
    OP66_SAP2,
    OP66_FORCE,
    OP66_DEEPSEQ,
    OP66_TRY,
    OP66_THROW,

    OP66_SAVE,
    OP66_LOAD,
    OP66_TRACE,
    OP66_EQUAL,
    OP66_PARSE_REX,
    OP66_PRINT_REX,
} enki_op66_sub;



typedef struct {
    enki_value law;
    size_t pc;
    size_t res_base_s;
    size_t arg_base_s;
    enki_value cont_v;
} enki_frame;

typedef struct enki_interpreter {
    size_t sp;
    enki_value stack_v[STACK_MAX];
    size_t fp;
    enki_frame frame[FRAME_MAX];
    enki_gc* gc;
    enki_allocator sys_a;
    bool halted; 
} enki_interpreter;

void enki_trace_interp(enki_interpreter* i);
void enki_run(enki_interpreter* i);
void enki_step(enki_interpreter* i);
void enki_halt(enki_interpreter* i);
void enki_destroy(enki_interpreter* i);
enki_value enki_eval_whnf(enki_interpreter* i, enki_value x);
enki_value enki_eval_nf(enki_interpreter* i, enki_value x);
enki_interpreter* enki_create_interp(enki_allocator sys_a, size_t heap, enki_value law);
