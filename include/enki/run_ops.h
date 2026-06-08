#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "enki/run.h"

er_val eo_nat(er_val val_v);
er_val eo_inc(enki_gc* gc, er_val val_v);
er_val eo_dec(enki_gc* gc, er_val val_v);
er_val eo_add(enki_gc* gc, er_val a_v, er_val b_v);
er_val eo_sub(enki_gc* gc, er_val a_v, er_val b_v);
er_val eo_mul(enki_gc* gc, er_val a_v, er_val b_v);
er_val eo_div(enki_gc* gc, er_val a_v, er_val b_v);
er_val eo_mod(enki_gc* gc, er_val a_v, er_val b_v);
er_val eo_lsh(enki_gc* gc, er_val a_v, er_val shift_v);
er_val eo_rsh(enki_gc* gc, er_val a_v, er_val shift_v);
er_val eo_cmp(er_val a_v, er_val b_v);
er_val eo_eq(er_val a_v, er_val b_v);
er_val eo_le(er_val a_v, er_val b_v);
er_val eo_test(er_val bit_v, er_val n_v);
er_val eo_load(enki_gc* gc, er_val idx_v, er_val width_v, er_val n_v);
er_val eo_loadn(enki_gc* gc, er_val width_v, er_val idx_v, er_val n_v);
er_val eo_store(enki_gc* gc, er_val idx_v, er_val val_v, er_val width_v,
                er_val n_v);
er_val eo_storen(enki_gc* gc, er_val width_v, er_val idx_v, er_val val_v,
                 er_val n_v);
er_val eo_trunc(enki_gc* gc, er_val width_v, er_val n_v);
er_val eo_truncn(enki_gc* gc, er_val width_v, er_val n_v);
er_val eo_met(er_val width_v, er_val n_v);
er_val eo_bex(enki_gc* gc, er_val bit_v);
er_val eo_bits(er_val n_v);
er_val eo_bytes(er_val n_v);
er_val eo_load8(enki_gc* gc, er_val idx_v, er_val n_v);
er_val eo_store8(enki_gc* gc, er_val idx_v, er_val val_v, er_val n_v);
er_val eo_trunc8(enki_gc* gc, er_val n_v);
er_val eo_trunc16(enki_gc* gc, er_val n_v);
er_val eo_trunc32(enki_gc* gc, er_val n_v);
er_val eo_trunc64(enki_gc* gc, er_val n_v);
bool eo_nat_to_size(er_val val_v, size_t* out_s);

er_val eo_nam(er_val law_v);
er_val eo_body(er_val law_v);
er_val eo_unpin(er_val pin_v);
er_val eo_sz(er_val row_v);
er_val eo_last(er_val row_v);
er_val eo_init(enki_gc* gc, er_val row_v);

er_val eo_rep(enki_gc* gc, er_val hd_v, er_val item_v, er_val count_v);
er_val eo_row(enki_gc* gc, er_val hd_v, er_val count_v, er_val xs_v);
er_val eo_slice(enki_gc* gc, er_val off_v, er_val count_v, er_val row_v);
er_val eo_weld(enki_gc* gc, er_val x_v, er_val y_v);
er_val eo_up(enki_gc* gc, er_val idx_v, er_val val_v, er_val row_v);
er_val eo_coup(enki_gc* gc, er_val hd_v, er_val row_v);
er_val eo_hd(er_val row_v);
er_val eo_ix(er_val idx_v, er_val row_v);

er_val eo_not(er_val val_v);
er_val eo_tru(er_val val_v);
er_val eo_or(er_val a_v, er_val b_v);
er_val eo_and(er_val a_v, er_val b_v);

er_val eo_pin(enki_gc* gc, er_val val_v);
er_val eo_law(enki_gc* gc, er_val nam_v, er_val bod_v, er_val ari_v);
er_val eo_elim(enki_gc* gc, er_val pin_f_v, er_val law_f_v, er_val app_f_v,
               er_val zero_v, er_val nat_f_v, er_val val_v);

bool eo_op66_from_tag(er_val tag_v, int* out_op);
er_val eo_exec_op66(enki_gc* gc, int op, size_t arg_s, const er_val arg_v[]);
er_val eo_exec_op66_tag(enki_gc* gc, er_val tag_v, size_t arg_s,
                        const er_val arg_v[]);
er_val eo_exec_op66_er_app(enki_gc* gc, const er_app* app);
er_val eo_exec_op66_app(enki_gc* gc, er_val row_v);
er_val eo_exec_op0(enki_gc* gc, int op, size_t arg_s, const er_val arg_v[]);
er_val eo_exec_op0_tag(enki_gc* gc, er_val tag_v, size_t arg_s,
                       const er_val arg_v[]);
er_val eo_exec_op0_er_app(enki_gc* gc, const er_app* app);
er_val eo_exec_op0_app(enki_gc* gc, er_val row_v);
