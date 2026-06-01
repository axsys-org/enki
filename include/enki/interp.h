#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include "enki/arena.h"
#include "enki/error.h"
#include "enki/gc.h"
#include "enki/store.h"
#include "enki/value.h"

#define STACK_MAX 8192
#define CALL_MAX 1024
#define HANDLER_MAX 1024
#define UPDATE_MAX 1024

typedef enum {
    ENKI_UPDATE_FORCE = 1,
} enki_update_kind;

typedef struct {
    enki_update_kind kind_b;
    uint8_t phase_b;
    enki_value thunk_v;
    size_t result_slot_s;
    size_t base_cp_s;
    size_t arity_s;
    size_t total_args_s;
} enki_update;

typedef enum {
    NO_OP              = 0x00,
    OP_PUSH_CONST      = 0x01,
    OP_PICK            = 0x02,
    OP_POP             = 0x03,
    OP_PUSH_CONST_WIDE = 0x04,
    OP_PICK_WIDE       = 0x05,
    OP_APPLY           = 0x08,
    OP_DUP             = 0x09,
    OP_APPLY_WIDE      = 0x0A,
    OP_OP0             = 0x10,
    OP_OP66            = 0x42,
    OP_OP82            = 0x52,
    OP_RETURN          = 0xFF,
} enki_opcode;


typedef enum {
    OP0_PIN = 0,
    OP0_LAW,
    OP0_ELIM,
} enki_op0_sub;

typedef enum {
    OP66_INC = 0,
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

#define ENKI_OP66_COUNT ((size_t)OP66_PRINT_REX + 1)

typedef struct {
    uint64_t interp_step_s;
    uint64_t law_enter_s;

    uint64_t apply_s;
    uint64_t apply_row_s;
    uint64_t apply_exact_s;
    uint64_t apply_under_s;
    uint64_t apply_over_s;
    uint64_t apply_op_s;

    uint64_t whnf_s;
    uint64_t whnf_immediate_s;
    uint64_t whnf_nat_s;
    uint64_t whnf_pin_s;
    uint64_t whnf_law_s;
    uint64_t whnf_app_whnf_s;
    uint64_t whnf_app_thunk_s;

    uint64_t gc_alloc_s;
    uint64_t gc_alloc_bytes_s;
    uint64_t gc_locked_alloc_s;
    uint64_t gc_locked_alloc_bytes_s;
    uint64_t gc_alloc_fail_s;
    uint64_t gc_collect_s;
    uint64_t gc_copy_s;
    uint64_t gc_copy_bytes_s;
    uint64_t gc_live_bytes_s;
    uint64_t gc_high_water_bytes_s;

    uint64_t op66_s[ENKI_OP66_COUNT];

    uint64_t nat_tmp_alloc_s;
    uint64_t nat_tmp_bytes_s;
    uint64_t nat_heap_alloc_s;
    uint64_t nat_heap_bytes_s;
    uint64_t nat_normalize_s;
    uint64_t nat_requested_limbs_s;
    uint64_t nat_final_limbs_s;
    uint64_t nat_trimmed_limbs_s;
    uint64_t nat_immediate_result_s;
    uint64_t nat_big_result_s;
} enki_stats;

typedef struct {
    enki_value law;
    uint8_t* bc_b;
    enki_value* const_table_v;
    size_t pc;
    size_t res_base_s;
    size_t arg_base_s;
} enki_call;

typedef struct {
    size_t cp;
    size_t sp;
    size_t res_base_s;
} enki_handler;

typedef struct enki_interpreter {
    size_t sp;
    enki_value stack_v[STACK_MAX];
    size_t cp;
    enki_call call_stack_v[CALL_MAX];
    size_t hp;
    enki_handler handler_v[HANDLER_MAX];
    size_t up;
    enki_update update_v[UPDATE_MAX];
    uint8_t* bc_b;
    enki_value* const_table_v;
    size_t pc;
    size_t arg_base_s;
    enki_gc* gc;
    enki_allocator our_a;
    bool halted;
    enki_store store;
    jmp_buf error_jmp;
    bool has_error_jmp;
    int error_code;
    enki_value error_v;
    enki_arena* scratch_a;
    enki_stats stats;
} enki_interpreter;

int enki_interp_run(enki_interpreter* i);
void enki_interp_push(enki_interpreter* i, enki_value val_v);
void enki_interp_step(enki_interpreter* i);
void enki_interp_halt(enki_interpreter* i);
void enki_interp_destroy(enki_interpreter* i);
void enki_interp_enter_call(enki_interpreter* i, enki_value fn_v, size_t n_args_s,
    enki_value* args_v);
void enki_interp_dispatch_op(enki_interpreter* i, uint8_t group);
void enki_stats_reset(enki_interpreter* i);
enki_interpreter* enki_interp_create(const enki_allocator* loc_a, size_t heap,
    const char* store_path_s, size_t store_size_s, size_t scratch_size_s);
void enki_interp_throw(enki_interpreter* i, int error_code, enki_value val);
