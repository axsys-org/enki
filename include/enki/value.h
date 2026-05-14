#ifndef ENKI_VALUE_H
#define ENKI_VALUE_H

#include <stddef.h>
#include <stdint.h>

#include <gmp.h>

#include "enki/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STACK_MAX 8192

/* ── Tagged value handle ────────────────────────────────────────────────── */

typedef uint64_t enki_value;

/* Low bit:  0 → heap pointer (heap allocations are 2-byte aligned)
            1 → immediate nat (value in bits 1..63)                       */

#define IS_PTR(v)   (((v) & 1) == 0)
#define IS_IMM(v)   (((v) & 1) == 1)
#define AS_PTR(v)   ((void*)(uintptr_t)(v))
#define AS_IMM(v)   ((uint64_t)((v) >> 1))
#define MAKE_PTR(p) ((enki_value)(uintptr_t)(p))
#define MAKE_IMM(n) ((((enki_value)(n)) << 1) | 1)
#define IMM_MAX     ((uint64_t)1 << 63)

/* ── Kinds ──────────────────────────────────────────────────────────────── */

enum {
    PIN,
    LAW,
    APP,
    INTERP,
    NAT,
    FORWARDED,
};

/* ── Forward decl of GC (defined in gc.h) ──────────────────────────────── */

typedef struct enki_gc enki_gc;

/* ── Common header shared by every heap value ──────────────────────────── */

typedef struct {
    uint8_t  kind;
    uint32_t size;
} enki_value_header;

/* ── Subtype structs (FAM is always the last field) ────────────────────── */

typedef struct {
    enki_value_header h;
    uint8_t           hash[32];
    enki_value        inner;
} enki_pin;

typedef struct {
    enki_value_header h;
    uint32_t          arity;
    enki_value        name;
    enki_value        body;
    uint32_t          bc_len;
    uint8_t           bc[];        /* FAM */
} enki_law;

typedef struct {
    enki_value_header h;
    enki_value        fn;
    uint32_t          n_args;
    enki_value        args[];      /* FAM */
} enki_app;

typedef struct {
    enki_value_header h;
    uint32_t          n_limbs;
    mp_limb_t         limbs[];     /* FAM */
} enki_nat;

typedef struct {
    enki_value_header h;
    uint32_t          pc;
    uint32_t          sp;
    enki_value        stack[STACK_MAX];
    uint32_t          bc_len;
    uint8_t           bc[];        /* FAM */
} enki_interp;

/* ── Forwarding tombstone (overlays any value's first bytes) ───────────── */

typedef struct {
    enki_value_header h;            /* h.kind = FORWARDED */
    void*             fwd;          /* new address (in to-space) */
} enki_fwd;

/* ── Trace + constructors ──────────────────────────────────────────────── */

void       enki_value_trace(enki_gc* gc, void* obj);

enki_value make_pin   (enki_gc* gc, enki_value inner);
enki_value make_law   (enki_gc* gc, uint32_t arity, enki_value name,
                                    enki_value body,
                                    const uint8_t* bc, size_t bc_len);
enki_value make_app   (enki_gc* gc, enki_value fn,
                                    const enki_value* args, uint32_t n_args);
enki_value make_nat   (enki_gc* gc, const mp_limb_t* limbs, uint32_t n_limbs);
enki_value make_interp(enki_gc* gc, const uint8_t* bc, size_t bc_len);

#ifdef __cplusplus
}
#endif

#endif
