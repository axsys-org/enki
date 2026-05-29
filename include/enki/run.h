#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>


#include <enki/allocator.h>

// yes if [v] is indirect
#define er_is_ptr(v) (((v) & (UINT64_C(1) << 63)) != 0)
#define er_is_cat(v) (!er_is_ptr(v))
/*
 * following conventions are used:
u63 0nnnnnnnnnnnnnnn - 0x0000..0x7f(n=nat data)
NAT 1000001000000000 - 0x8200
PIN 11000100000000cm - 0xC40x (c=hascrc32, m=ismegapin)
LAW 1100100000000000 - 0xC800
CLZ 11010000ttttzzzz - 0xD0tz (t=tag, z=size)
THK 1110000000000000 - 0xE000
*/
#define er_tag_shift 56
#define er_ptr_mask UINT64_C(0x00ffffffffffffff)
#define er_get_tag(v) ((uint8_t)((uint64_t)(v) >> er_tag_shift))
#define er_is_tag(tag, v) ((uint8_t)(tag) == er_get_tag(v))
#define er_tag_bat 0x82
#define er_tag_pin 0xc4
#define er_tag_law 0xc8
#define er_tag_app 0xd0
#define er_tag_thk 0xe0
#define er_tag_fwd 0xf0
#define er_tag_bad 0xff

#define er_into(tag, ptr) \
  ((((uint64_t)(tag)) << er_tag_shift) | (((uint64_t)(uintptr_t)(ptr)) & er_ptr_mask))
#define er_outa(v) ((void*)(uintptr_t)(((uint64_t)(v)) & er_ptr_mask))
#define er_outt(tag, v) (er_is_tag(tag, v) ? er_outa(v) : NULL)

#define er_is_whnf(v) (er_get_tag(v) <= er_tag_app)

typedef uint64_t er_val;

#define er_bad er_into(er_tag_bad, 0)

typedef struct er_head_raw {
  uint64_t fwd_f: 1;
  uint64_t nf_f: 1;
  uint64_t pad_q: 62;
} er_head_raw;

typedef union er_head {
  size_t siz_s;
  er_head_raw raw;
} er_head;

typedef struct er_bat {
  er_head hed;
  size_t lim_s;
  uint64_t lim_q[];
} er_bat;

typedef struct er_pin {
  er_head hed;
  uint8_t hash_b[32];
  er_val val_v;
  size_t sub_s;
  er_val sub_v[];
} er_pin;

typedef struct er_law {
    er_head h;
    er_val name_v;
    er_val body_v;
    uint32_t ari_d;
    uint32_t start_d;
    uint32_t frame_d;
    uint32_t let_off_d[];
} er_law;

typedef struct er_app {
  er_head h;
  er_val fn_v;
  size_t arg_s;
  er_val arg_v[];
} er_app;

typedef enum er_execf {
  ER_XDONE = 0, // -> [res]
  ER_XUNK_APP,  // -> [f, ...args]
  ER_CALL, // -> [f, ...args]
//  ER_CALL_LET, // -> [num-let, f, ...args]
  ER_XPRIM, // [op-set, arg]
  ER_HOLE,
  ER_SUSP, // -> [pc, frame]
} er_execf;

typedef struct er_thk {
  er_head hed;
  er_execf fun;
  size_t arg_s;
  er_val arg_v[];
} er_thk;

er_bat* er_bat_alloc(const enki_allocator* allocator, size_t lim_s);
er_val er_bat_init(er_bat* bat, size_t lim_s, const uint64_t lim_q[]);

er_pin* er_pin_alloc(const enki_allocator* allocator, size_t sub_s);
er_val er_pin_init(er_pin* pin, const uint8_t hash_b[32], er_val val_v, size_t sub_s,
    const er_val sub_v[]);

er_law* er_law_alloc(const enki_allocator* allocator, size_t n_lets);
er_val er_law_init(er_law* law, er_val name_v, er_val body_v, uint32_t ari_d,
    uint32_t frame_d, size_t n_lets, const uint32_t let_off_d[]);

er_app* er_app_alloc(const enki_allocator* allocator, size_t arg_s);
er_val er_app_init(er_app* app, er_val fn_v, size_t arg_s, const er_val arg_v[]);

er_thk* er_thk_alloc(const enki_allocator* allocator, size_t arg_s);
er_val er_thk_init(er_thk* thk, er_execf fun, size_t arg_s, const er_val arg_v[]);

er_val er_eval(const enki_allocator* loc_a, er_val val_v);
er_thk* er_app_weld(const enki_allocator* allocator, er_val sin_v, er_val dex_v);



// Schematic C. Heap/vector helpers are intentionally abstracted.


typedef enum {
  OP_PUSH_VAR,
  OP_PUSH_LIT,
  OP_MK_APP,
  OP_MK_CALL,
  OP_FORCE,
  OP_DROP,
  OP_JUMP_IF_ZERO,
  OP_ADD_NAT,
  OP_RET,
  OP_COUNT
} er_optag;

typedef struct {
    er_optag tag;
    union {
        uint32_t   u32;
        uintptr_t  slot;
        er_val     lit_v;
    } as;
} er_op;

typedef union {
    void      *lab;
    er_val    *ref;
    er_val    val_v;
    uintptr_t u;
    uint32_t  pc;
} er_kon;

typedef struct {
  er_op* code;
  const enki_allocator* loc_a;

  er_val* dstack;
  er_val* dsp;

  er_kon *kbase;
  er_kon *ksp;
} er_vm;



er_val
plan_eval(er_vm *vm, er_val val_v);

#define PLAN_CH(c) ((uint64_t)(uint8_t)(c))
#define PLAN_S1(a) ((er_val)PLAN_CH(a))
#define PLAN_S2(a, b) ((er_val)(PLAN_CH(a) | (PLAN_CH(b) << 8u)))
#define PLAN_S3(a, b, c) ((er_val)(PLAN_S2(a, b) | (PLAN_CH(c) << 16u)))
#define PLAN_S4(a, b, c, d) ((er_val)(PLAN_S3(a, b, c) | (PLAN_CH(d) << 24u)))
#define PLAN_S5(a, b, c, d, e) ((er_val)(PLAN_S4(a, b, c, d) | (PLAN_CH(e) << 32u)))
#define PLAN_S6(a, b, c, d, e, f) ((er_val)(PLAN_S5(a, b, c, d, e) | (PLAN_CH(f) << 40u)))
#define PLAN_S7(a, b, c, d, e, f, g) ((er_val)(PLAN_S6(a, b, c, d, e, f) | (PLAN_CH(g) << 48u)))
