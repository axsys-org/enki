#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "enki/interp.h"
#include "enki/run.h"

typedef enum er_bc_eval_req {
    ER_BC_EVAL_NONE = 0,
    ER_BC_EVAL_WHNF,
    ER_BC_EVAL_NF,
} er_bc_eval_req;

enum { ER_BC_MAX_PRIM_ARITY = 8 };

enum {
    ER_OP66_STORE = OP66_PRINT_REX + 1,
    ER_OP66_MET,
    ER_OP66_COUNT,
};

typedef struct er_bc_prim_route {
    er_optag tag;
    size_t arg_s;
    bool valid_f;
    er_bc_eval_req arg_eval_v[ER_BC_MAX_PRIM_ARITY];
} er_bc_prim_route;

bool er_bc_prim_route_strict(er_optag tag, size_t arg_s, er_bc_prim_route* out);
bool er_bc_prim0_route(size_t op_s, er_bc_prim_route* out);
bool er_bc_prim66_route(int op_i, er_bc_prim_route* out);

bool er_bc_emit_prim_route_fragment(er_op out_v[], size_t cap_s, er_optag tag, size_t arg_s,
                                    const er_bc_eval_req arg_eval_v[], er_val lit_v,
                                    size_t* out_s);
