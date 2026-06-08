I_OP_PIN:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_pin(vm->gc, prim_a_v));

I_OP_LAW:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_law(vm->gc, prim_b_v, prim_c_v, prim_a_v));

I_OP_ELIM:
    prim_f_v = DPOP();
    prim_e_v = DPOP();
    prim_d_v = DPOP();
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_elim(vm->gc, prim_a_v, prim_b_v, prim_c_v, prim_d_v, prim_e_v,
                            prim_f_v));

I_OP_NAM:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_nam(prim_a_v));

I_OP_BODY:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_body(prim_a_v));

I_OP_NAT:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_nat(prim_a_v));

I_OP_ARI:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE((er_val)er_arity(prim_a_v));

I_OP_UNPIN:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_unpin(prim_a_v));

I_OP_SZ:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_sz(prim_a_v));

I_OP_LAST:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_last(prim_a_v));

I_OP_INIT:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_init(vm->gc, prim_a_v));

I_OP_ADD:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_add(vm->gc, prim_a_v, prim_b_v));

I_OP_SUB:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_sub(vm->gc, prim_a_v, prim_b_v));

I_OP_RSH:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_rsh(vm->gc, prim_a_v, prim_b_v));

I_OP_LSH:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_lsh(vm->gc, prim_a_v, prim_b_v));

I_OP_DIV:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_div(vm->gc, prim_a_v, prim_b_v));

I_OP_MUL:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_mul(vm->gc, prim_a_v, prim_b_v));

I_OP_MOD:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_mod(vm->gc, prim_a_v, prim_b_v));

I_OP_TEST:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_test(prim_a_v, prim_b_v));

I_OP_LOADN:
    prim_word_v = op.as.lit_v;
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_loadn(vm->gc, prim_word_v, prim_a_v, prim_b_v));

I_OP_LOAD:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_load(vm->gc, prim_a_v, prim_b_v, prim_c_v));

I_OP_STOREN:
    prim_word_v = op.as.lit_v;
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_storen(vm->gc, prim_word_v, prim_a_v, prim_b_v, prim_c_v));

I_OP_STORE:
    prim_d_v = DPOP();
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    /* Stack order is idx, width, value, n; eo_store wants idx, value, width, n. */
    PRIM_DONE_VALUE(eo_store(vm->gc, prim_a_v, prim_c_v, prim_b_v, prim_d_v));

I_OP_TRUNCN:
    prim_word_v = op.as.lit_v;
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_truncn(vm->gc, prim_word_v, prim_a_v));

I_OP_TRUNC:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc(vm->gc, prim_a_v, prim_b_v));

I_OP_MET:
    prim_word_v = op.as.lit_v;
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_met(prim_word_v, prim_a_v));

I_OP_MET_DYN:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_met(prim_a_v, prim_b_v));

I_OP_BEX:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_bex(vm->gc, prim_a_v));

I_OP_BITS:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_bits(prim_a_v));

I_OP_BYTES:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_bytes(prim_a_v));

I_OP_LOAD8:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_load8(vm->gc, prim_a_v, prim_b_v));

I_OP_STORE8:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_store8(vm->gc, prim_a_v, prim_b_v, prim_c_v));

I_OP_TRUNC8:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc8(vm->gc, prim_a_v));

I_OP_TRUNC16:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc16(vm->gc, prim_a_v));

I_OP_TRUNC32:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc32(vm->gc, prim_a_v));

I_OP_TRUNC64:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc64(vm->gc, prim_a_v));

I_OP_REP:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_rep(vm->gc, prim_a_v, prim_b_v, prim_c_v));

I_OP_SLICE:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_slice(vm->gc, prim_a_v, prim_b_v, prim_c_v));

I_OP_WELD:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_weld(vm->gc, prim_a_v, prim_b_v));

I_OP_UP:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_up(vm->gc, prim_a_v, prim_b_v, prim_c_v));

I_OP_COUP:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_coup(vm->gc, prim_a_v, prim_b_v));

I_OP_HD:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_hd(prim_a_v));

I_OP_IX:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_ix(prim_a_v, prim_b_v));

I_OP_NOT:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_not(prim_a_v));

I_OP_TRU:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_tru(prim_a_v));

I_OP_OR:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_or(prim_a_v, prim_b_v));

I_OP_AND:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_and(prim_a_v, prim_b_v));

I_OP_EQ:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_eq(prim_a_v, prim_b_v));

I_OP_LE:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_le(prim_a_v, prim_b_v));

I_OP_CMP:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_cmp(prim_a_v, prim_b_v));

I_OP_INC:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_inc(vm->gc, prim_a_v));

I_OP_DEC:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_dec(vm->gc, prim_a_v));
