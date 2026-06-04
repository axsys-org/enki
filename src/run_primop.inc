PRIMOP: {
    if (prim_set == 66) {
        goto PRIM66_DECODE;
    }
    if (prim_set == 0) {
        goto PRIM0_DECODE;
    }
    FAIL_TANK("bad primitive set", prim_arg);
}

PRIM0_DECODE: {
    prim_row = er_outt(er_tag_app, prim_arg);
    if (prim_row == NULL) {
        FAIL_TANK("expected primitive row", prim_arg);
    }
    prim_tag_v = prim_row->fn_v;
    prim_arg_v = prim_row->arg_v;
    prim_arg_s = prim_row->arg_s;
    if (prim_tag_v == 0 && prim_arg_s > 0 && er_is_cat(prim_arg_v[0]) &&
        prim_arg_v[0] <= (er_val)OP0_ELIM) {
        prim_tag_v = prim_arg_v[0];
        prim_arg_v = prim_row->arg_v + 1;
        prim_arg_s = prim_row->arg_s - 1;
    }
    if (!er_is_cat(prim_tag_v) || prim_tag_v > (er_val)OP0_ELIM) {
        FAIL_TANK("bad primitive tag", prim_tag_v);
    }
    prim_op_s = (size_t)prim_tag_v;
    if (!er_bc_prim0_route(prim_op_s, &prim_route)) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    PRIM_SELECT_ROUTE(prim_route);
}

PRIM66_DECODE: {
    prim_row = er_outt(er_tag_app, prim_arg);
    prim_tag_v = prim_arg;
    prim_arg_v = NULL;
    prim_arg_s = 0;
    if (prim_row != NULL) {
        prim_tag_v = prim_row->fn_v;
        prim_arg_v = prim_row->arg_v;
        prim_arg_s = prim_row->arg_s;
        if (prim_tag_v == 0 && prim_row->arg_s > 0) {
            prim_tag_v = prim_row->arg_v[0];
            prim_arg_v = prim_row->arg_v + 1;
            prim_arg_s = prim_row->arg_s - 1;
        }
    }

    prim_desc = er_outt(er_tag_app, prim_tag_v);
    if (prim_desc != NULL) {
        if (prim_desc->arg_s != 1) {
            FAIL_TANK("bad primitive descriptor", prim_tag_v);
        }
        prim_name_v = prim_desc->fn_v;
        prim_word_v = prim_desc->arg_v[0];
        if (!er_prim66_op_from_descriptor(prim_name_v, &prim_op_i) || prim_op_i < 0 ||
            (size_t)prim_op_i >= ER_OP66_COUNT) {
            FAIL_TANK("bad primitive descriptor", prim_tag_v);
        }
        if (!er_bc_prim66_route(prim_op_i, &prim_route)) {
            FAIL_TANK("bad primitive route", prim_tag_v);
        }
        switch (prim_op_i) {
        case OP66_LOAD:
            PRIM_SELECT(prim_word_v == 0 ? OP_LOAD : OP_LOADN, prim_word_v == 0 ? 3 : 2);
        case ER_OP66_STORE:
            PRIM_SELECT(prim_word_v == 0 ? OP_STORE : OP_STOREN, prim_word_v == 0 ? 4 : 3);
        case OP66_TRUNC:
            PRIM_SELECT(prim_word_v == 0 ? OP_TRUNC : OP_TRUNCN, prim_word_v == 0 ? 2 : 1);
        case ER_OP66_MET:
            PRIM_SELECT(OP_MET, 1);
        default:
            PRIM_SELECT_ROUTE(prim_route);
        }
    }
    if (!eo_op66_from_tag(prim_tag_v, &prim_op_i) || prim_op_i < 0 ||
        (size_t)prim_op_i >= ER_OP66_COUNT) {
        FAIL_TANK("bad primitive tag", prim_tag_v);
    }
    switch (prim_op_i) {
    case OP66_TYPE:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        switch (er_get_tag(prim_a_v)) {
        case er_tag_bat:
            PRIM_DONE_VALUE(0);
        case er_tag_pin:
            PRIM_DONE_VALUE(1);
        case er_tag_law:
            PRIM_DONE_VALUE(2);
        case er_tag_app:
            PRIM_DONE_VALUE(3);
        default:
            if (er_is_cat(prim_a_v)) {
                PRIM_DONE_VALUE(0);
            }
            FAIL_TANK("bad value tag", prim_a_v);
        }
    case OP66_IS_PIN:
    case OP66_IS_LAW:
    case OP66_IS_APP:
    case OP66_IS_NAT:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        if (prim_op_i == OP66_IS_PIN) {
            PRIM_DONE_VALUE(er_is_tag(er_tag_pin, prim_a_v) ? 1 : 0);
        }
        if (prim_op_i == OP66_IS_LAW) {
            PRIM_DONE_VALUE(er_is_tag(er_tag_law, prim_a_v) ? 1 : 0);
        }
        if (prim_op_i == OP66_IS_APP) {
            PRIM_DONE_VALUE(er_is_tag(er_tag_app, prim_a_v) ? 1 : 0);
        }
        PRIM_DONE_VALUE((er_is_cat(prim_a_v) || er_is_tag(er_tag_bat, prim_a_v)) ? 1 : 0);
    case OP66_NE:
    case OP66_LT:
    case OP66_GT:
    case OP66_GE:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        if (prim_op_i == OP66_NE) {
            PRIM_DONE_VALUE(eo_eq(prim_a_v, prim_b_v) == 0 ? 1 : 0);
        }
        prim_c_v = eo_cmp(prim_a_v, prim_b_v);
        if (prim_op_i == OP66_LT) {
            PRIM_DONE_VALUE(prim_c_v == 0 ? 1 : 0);
        }
        if (prim_op_i == OP66_GT) {
            PRIM_DONE_VALUE(prim_c_v == 2 ? 1 : 0);
        }
        PRIM_DONE_VALUE(prim_c_v == 0 ? 0 : 1);
    case OP66_SET:
    case OP66_CLEAR:
    case OP66_NIB:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        if (prim_op_i == OP66_NIB) {
            PRIM_DONE_VALUE(eo_load(vm->loc_a, prim_a_v, 4, prim_b_v));
        }
        PRIM_DONE_VALUE(eo_store(vm->loc_a, prim_a_v, prim_op_i == OP66_SET ? 1 : 0, 1,
                                 prim_b_v));
    case OP66_CASE: {
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        size_t case_idx_s = 0;
        er_app* case_row = er_outt(er_tag_app, prim_b_v);
        if (!eo_nat_to_size(prim_a_v, &case_idx_s) || case_row == NULL ||
            case_idx_s >= case_row->arg_s) {
            r = prim_arg_v[2];
        } else {
            r = case_row->arg_v[case_idx_s];
        }
        goto FORCE_ENTRY;
    }
    case OP66_CASE2:
    case OP66_CASE3:
    case OP66_CASE4:
    case OP66_CASE5:
    case OP66_CASE6:
    case OP66_CASE7:
    case OP66_CASE8:
    case OP66_CASE9:
    case OP66_CASE10:
    case OP66_CASE11:
    case OP66_CASE12:
    case OP66_CASE13:
    case OP66_CASE14:
    case OP66_CASE15:
    case OP66_CASE16: {
        size_t case_s = (size_t)(prim_op_i - OP66_CASE2 + 2);
        if (prim_arg_s != case_s + 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        size_t case_idx_s = 0;
        if (!eo_nat_to_size(prim_a_v, &case_idx_s) || case_idx_s >= case_s - 1) {
            r = prim_arg_v[case_s];
        } else {
            r = prim_arg_v[case_idx_s + 1];
        }
        goto FORCE_ENTRY;
    }
    case OP66_IX0:
    case OP66_IX1:
    case OP66_IX2:
    case OP66_IX3:
    case OP66_IX4:
    case OP66_IX5:
    case OP66_IX6:
    case OP66_IX7:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        prim_a_v = (er_val)(prim_op_i - OP66_IX0);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[0]);
        PRIM_DONE_VALUE(eo_ix(prim_a_v, prim_b_v));
    case OP66_IF:
    case OP66_IFZ:
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        r = (prim_op_i == OP66_IF ? prim_a_v != 0 : prim_a_v == 0) ? prim_arg_v[1]
                                                                    : prim_arg_v[2];
        goto FORCE_ENTRY;
    case OP66_NOR:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        if (prim_a_v != 0) {
            PRIM_DONE_VALUE(0);
        }
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        PRIM_DONE_VALUE(prim_b_v == 0 ? 1 : 0);
    case OP66_ROW:
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        prim_a_v = eo_nat(prim_a_v);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        prim_c_v = prim_arg_v[2];
        {
            size_t row_count_s = 0;
            (void)eo_nat_to_size(prim_b_v, &row_count_s);
            if (row_count_s == 0) {
                PRIM_DONE_VALUE(prim_a_v);
            }

            GC_SYNC();
            GC_ROOT_PRIMS();
            er_app* row_app = er_app_alloc(vm->loc_a, row_count_s);
            prim_a_v = vm->gc_tmp_v[0];
            prim_b_v = vm->gc_tmp_v[1];
            prim_c_v = vm->gc_tmp_v[2];
            GC_CLEAR_ROOTS();
            if (row_app == NULL) {
                FAIL_ALLOC();
            }

            er_val row_v = er_app_init(row_app, prim_a_v, row_count_s, NULL);
            CHECK_ALLOC(row_v);
            er_val* row_root_p = dsp;
            DPUSH(row_v);

            for (size_t row_k_s = 0; row_k_s < row_count_s; row_k_s++) {
                PRIM_FORCE_VALUE_WHNF(prim_c_v, prim_c_v);
                row_app = er_outt(er_tag_app, *row_root_p);
                if (row_app == NULL) {
                    FAIL_ALLOC();
                }
                row_app->arg_v[row_k_s] = eo_ix(0, prim_c_v);
                prim_c_v = eo_ix(1, prim_c_v);
            }

            r = DPOP();
            goto FORCE_ENTRY;
        }
    case OP66_TRACE:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        fprintf(stderr, "trace: %s\n", enki_pvalue(vm->loc_a, prim_arg_v[0]));
        r = prim_arg_v[1];
        goto FORCE_ENTRY;
    case OP66_SEQ:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        r = prim_arg_v[1];
        goto FORCE_ENTRY;
    case OP66_SEQ2:
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        r = prim_arg_v[2];
        goto FORCE_ENTRY;
    case OP66_SEQ3:
        if (prim_arg_s != 4) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        PRIM_FORCE_VALUE_WHNF(prim_c_v, prim_arg_v[2]);
        r = prim_arg_v[3];
        goto FORCE_ENTRY;
    case OP66_SAP:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        prim_a_v = prim_arg_v[0];
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        GC_SYNC();
        GC_ROOT_PRIMS();
        r = er_thk_make_unk_app(vm->loc_a, 2, vm->gc_tmp_v);
        GC_CLEAR_ROOTS();
        CHECK_ALLOC(r);
        goto FORCE_ENTRY;
    case OP66_SAP2:
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        prim_a_v = prim_arg_v[0];
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        PRIM_FORCE_VALUE_WHNF(prim_c_v, prim_arg_v[2]);
        prim_d_v = prim_c_v;
        GC_SYNC();
        GC_ROOT_PRIMS();
        r = er_thk_make_unk_app(vm->loc_a, 2, vm->gc_tmp_v);
        GC_CLEAR_ROOTS();
        CHECK_ALLOC(r);
        GC_SYNC();
        GC_ROOT_PRIMS();
        prim_a_v = plan_eval_whnf_preserve(vm, r);
        GC_CLEAR_ROOTS();
        pc = vm->pc;
        CHECK_PRIM(prim_a_v);
        prim_b_v = prim_d_v;
        GC_SYNC();
        GC_ROOT_PRIMS();
        r = er_thk_make_unk_app(vm->loc_a, 2, vm->gc_tmp_v);
        GC_CLEAR_ROOTS();
        CHECK_ALLOC(r);
        goto FORCE_ENTRY;
    case OP66_FORCE:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_NF(prim_a_v, prim_arg_v[0]);
        PRIM_DONE_VALUE(prim_a_v);
    case OP66_DEEPSEQ:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_NF(prim_a_v, prim_arg_v[0]);
        r = prim_arg_v[1];
        goto FORCE_ENTRY;
    case OP66_TRY:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        prim_a_v = prim_arg_v[0];
        prim_b_v = prim_arg_v[1];
        GC_SYNC();
        GC_ROOT_PRIMS();
        r = er_thk_make_unk_app(vm->loc_a, 2, vm->gc_tmp_v);
        GC_CLEAR_ROOTS();
        CHECK_ALLOC(r);
        GC_SYNC();
        prim_c_v = plan_eval_nf_inner(vm, r);
        pc = vm->pc;
        {
            er_val tag_v = 0;
            er_val try_val_v = prim_c_v;
            er_tank* tank = er_outt(er_tag_tank, prim_c_v);
            if (tank != NULL && tank->msg_c != NULL && strcmp(tank->msg_c, "throw") == 0) {
                tag_v = 1;
                try_val_v = tank->val_v;
            } else {
                er_app* thrown_app = er_outt(er_tag_app, prim_c_v);
                if (thrown_app != NULL && thrown_app->arg_s == 1 &&
                    (thrown_app->fn_v == PLAN_S5('T', 'h', 'r', 'o', 'w') ||
                     thrown_app->fn_v == PLAN_S5('E', 'r', 'r', 'o', 'r'))) {
                    tag_v = 1;
                    try_val_v = thrown_app->arg_v[0];
                }
            }
            if (tag_v == 1) {
                PRIM_FORCE_VALUE_NF(try_val_v, try_val_v);
            } else if (!er_is_good(prim_c_v)) {
                vm->dsp = dsp;
                vm->ksp = ksp;
                return prim_c_v;
            }
            vm->gc_tmp_v[0] = try_val_v;
            vm->gc_tmp_s = 1;
            GC_SYNC();
            r = er_app_make(vm->loc_a, tag_v, 1, vm->gc_tmp_v);
            GC_CLEAR_ROOTS();
            CHECK_ALLOC(r);
        }
        PRIM_DONE_VALUE(r);
    case OP66_THROW:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_NF(prim_a_v, prim_arg_v[0]);
        FAIL_TANK("throw", prim_a_v);
    default:
        break;
    }
    if (!er_bc_prim66_route(prim_op_i, &prim_route)) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    PRIM_SELECT_ROUTE(prim_route);
}

PRIM_ROUTE_DISPATCH: {
    if (prim_byte_op >= OP_COUNT || dispatch[prim_byte_op] == NULL) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    if (prim_arg_s != prim_need_s || prim_need_s > ER_BC_MAX_PRIM_ARITY ||
        prim_route_depth_s >= ER_PRIM_ROUTE_MAX_DEPTH) {
        PRIM_BAD_ARITY();
    }
    for (size_t prim_k_s = 0; prim_k_s < prim_need_s; prim_k_s++) {
        DPUSH(prim_arg_v[prim_k_s]);
    }

    size_t prim_route_slot_s = prim_route_depth_s++;
    er_val prim_lit_v = 0;
    switch (prim_byte_op) {
    case OP_LOADN:
    case OP_STOREN:
    case OP_TRUNCN:
    case OP_MET:
        prim_lit_v = prim_word_v;
        break;
    default:
        break;
    }
    er_bcpc prim_route_pc = ER_BCPC_NONE;
    er_bcpc prim_route_end_pc = ER_BCPC_NONE;
    if (!er_prim_route_intern(prim_byte_op, (uint32_t)prim_need_s, prim_route.arg_eval_v,
                              prim_lit_v, &prim_route_pc, &prim_route_end_pc)) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    prim_route_start_pc_v[prim_route_slot_s] = prim_route_pc;
    prim_route_final_pc_v[prim_route_slot_s] = prim_route_end_pc;

    CODE_SET_PC(prim_route_pc);
    DISPATCH();
}

PRIM_DONE: {
    bool prim_route_done_f = false;
    if (prim_route_depth_s > 0) {
        er_bcpc route_start_pc = prim_route_start_pc_v[prim_route_depth_s - 1u];
        er_bcpc route_end_pc = prim_route_final_pc_v[prim_route_depth_s - 1u];
        prim_route_done_f = last_pc >= route_start_pc && last_pc < route_end_pc &&
                            pc == route_end_pc;
    }
    if (prim_route_done_f) {
      prim_route_depth_s--;
      r = DPOP();
      goto FORCE_ENTRY;
    }
    DISPATCH();
}
