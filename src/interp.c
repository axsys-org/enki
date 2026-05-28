#include <stdlib.h>
#include <string.h>

#include "enki/interp.h"
#include "enki/app.h"
#include "enki/value.h"
#include "enki/gc.h"
#include "enki/util.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/op66.h"
#include "enki/op0.h"

enki_interpreter* enki_interp_create(const enki_allocator* loc_a, size_t heap,
    const char* store_path_s, size_t store_size_s, size_t scratch_size_s) {
    if(!loc_a) abort();
    enki_interpreter* i = (enki_interpreter*)loc_a->alloc(loc_a->ctx, sizeof(enki_interpreter));
    if(!i) abort();
    i->gc = enki_gc_create(loc_a, heap, i);
    if(i->gc == NULL) {
        loc_a->free(loc_a->ctx, i);
        ea_abort("Failed to init gc");
    }
    i->our_a = *loc_a;
    i->halted = false;
    i->arg_base_s = 0;
    i->cp = 0;
    i->sp = 0;
    i->hp = 0;
    enki_stats_reset(i);
    i->scratch_a = enki_arena_create(loc_a, scratch_size_s);
    if(!i->scratch_a) {
        enki_gc_destroy(i->gc);
        loc_a->free(loc_a->ctx, i);
        ea_abort("failed to init scratch");
    }
    enki_error st = enki_store_init(store_path_s, store_size_s, &i->store);
    if(st != ENKI_ERROR_OK) {
        enki_arena_destroy(i->scratch_a);
        enki_gc_destroy(i->gc);
        loc_a->free(loc_a->ctx, i);
        ea_abort("failed to init store");
    }
    return i;
}
void enki_stats_reset(enki_interpreter* i) {
    memset(&i->stats, 0, sizeof(i->stats));
}
void enki_interp_destroy(enki_interpreter* i) {
    if(i->gc) enki_gc_destroy(i->gc);
    if(i->scratch_a) enki_arena_destroy(i->scratch_a);
    i->our_a.free(i->our_a.ctx, i);
}
void enki_interp_halt(enki_interpreter* i) {
    i->halted = true;
}
void enki_interp_throw(enki_interpreter* i, int error_code, enki_value val) {
    i->error_v = val;
    i->error_code = error_code;
    if(i->hp > 0 && error_code == ENKI_ERROR_THROW) {
        enki_handler hdlr = i->handler_v[i->hp - 1];
        size_t val_slot_s = i->sp;
        i->stack_v[val_slot_s] = val;
        i->sp++;
        enki_value app = enki_app_alloc(i->gc, (enki_value)1, 1);
        enki_app* ptr = ENKI_AS(enki_app, app);
        ptr->args_v[0] = i->stack_v[val_slot_s];
        i->stack_v[hdlr.res_base_s] = app;
        i->sp = hdlr.sp;
        i->cp = hdlr.cp;
        if(i->cp > 0) {
            enki_call* call = &i->call_stack_v[i->cp - 1];
            enki_law* law = ENKI_AS(enki_law, call->law);
            i->bc_b = ENKI_LAW_BC(law);
            i->const_table_v = ENKI_LAW_CONSTS(law);
            i->pc = call->pc;
            i->arg_base_s = call->arg_base_s;
        }
        i->hp--;
        return;
    }
    if(i->has_error_jmp) longjmp(i->error_jmp, 1);
    abort();
}
int enki_interp_run(enki_interpreter* i) {
    i->has_error_jmp = true;
    if(setjmp(i->error_jmp) == 0) {
        while (!i->halted) {
            enki_interp_step(i);
            enki_arena_reset(i->scratch_a);
        }
        enki_arena_reset(i->scratch_a);
        i->has_error_jmp = false;
        return 0;
    }
    enki_arena_reset(i->scratch_a);
    i->has_error_jmp = false;
    return 1;
}

void enki_interp_push(enki_interpreter* i, enki_value val_v)
{
  i->stack_v[i->sp++] = val_v;
}

void enki_interp_enter_call(enki_interpreter* i, enki_value fn_v, 
    size_t n_args_s, enki_value* args_v) {
    i->stack_v[i->sp++] = fn_v;
    for(size_t k = 0; k < n_args_s; k++) {
        i->stack_v[i->sp++] = args_v[k];
    }
    enki_app_apply(i, n_args_s);
}


static void enki_interp_dispatch_op0(enki_interpreter* i, uint8_t sub_t) {
    switch(sub_t) {
        case 0: op0_pin(i);  break;
        case 1: op0_law(i);  break;
        case 2: op0_elim(i);  break;
    }
}

static void enki_interp_dispatch_op66(enki_interpreter* i, uint8_t sub_b) {
    if(sub_b < ENKI_OP66_COUNT) {
        i->stats.op66_s[sub_b]++;
    }
    switch (sub_b) {
        case OP66_INC: {
            enki_value a = i->stack_v[i->sp - 1];
            i->stack_v[i->sp - 1] = enki_nat_inc(i->gc, a);
            break;
        }
        case OP66_DEC: {
            enki_value a = i->stack_v[i->sp - 1];
            i->stack_v[i->sp - 1] = enki_nat_dec(i->gc, a);
            break;
        }
        case OP66_ADD: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_add(i->gc, a, b);
            break;
        }
        case OP66_SUB: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_sub(i->gc, a, b);
            break;
        }
        case OP66_MUL: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_mul(i->gc, a, b);
            break;
        }
        case OP66_DIV:        op66_div(i);         break;
        case OP66_MOD:        op66_mod(i);         break;
        case OP66_EQ: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_eq(a, b);
            break;
        }
        case OP66_NE: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_ne(a, b);
            break;
        }
        case OP66_LT: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_lt(a, b);
            break;
        }
        case OP66_LE: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_le(a, b);
            break;
        }
        case OP66_GT: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_gt(a, b);
            break;
        }
        case OP66_GE: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            i->sp--;
            i->stack_v[i->sp - 1] = enki_nat_ge(a, b);
            break;
        }
        case OP66_CMP: {
            enki_value a = i->stack_v[i->sp - 2];
            enki_value b = i->stack_v[i->sp - 1];
            int cmp = enki_nat_cmp(a, b);
            i->sp--;
            i->stack_v[i->sp - 1] =
                (cmp < 0) ? (enki_value)0 : (cmp == 0 ? (enki_value)1 : (enki_value)2);
            break;
        }
        case OP66_RSH:        op66_rsh(i);         break;
        case OP66_LSH:        op66_lsh(i);         break;
        case OP66_TEST:       op66_test(i);        break;
        case OP66_SET:        op66_set(i);         break;
        case OP66_CLEAR:      op66_clear(i);       break;
        case OP66_BEX:        op66_bex(i);         break;
        case OP66_BITS:       op66_bits(i);        break;
        case OP66_BYTES:      op66_bytes(i);       break;
        case OP66_NIB:        op66_nib(i);         break;
        case OP66_LOAD8:      op66_load8(i);       break;
        case OP66_STORE8:     op66_store8(i);      break;
        case OP66_TRUNC:      op66_trunc(i);       break;
        case OP66_TRUNC8:     op66_trunc8(i);      break;
        case OP66_TRUNC16:    op66_trunc16(i);     break;
        case OP66_TRUNC32:    op66_trunc32(i);     break;
        case OP66_TRUNC64:    op66_trunc64(i);     break;
        case OP66_TYPE:       op66_type(i);        break;
        case OP66_IS_PIN:     op66_is_pin(i);      break;
        case OP66_IS_LAW:     op66_is_law(i);      break;
        case OP66_IS_APP:     op66_is_app(i);      break;
        case OP66_IS_NAT:     op66_is_nat(i);      break;
        case OP66_NAT:        op66_nat(i);         break;
        case OP66_UNPIN:      op66_unpin(i);       break;
        case OP66_ARITY:      op66_arity(i);       break;
        case OP66_NAME:       op66_name(i);        break;
        case OP66_BODY:       op66_body(i);        break;
        case OP66_HD:         op66_hd(i);          break;
        case OP66_LAST:       op66_last(i);        break;
        case OP66_INIT:       op66_init(i);        break;
        case OP66_ROW:        op66_row(i);         break;
        case OP66_REP:        op66_rep(i);         break;
        case OP66_SLICE:      op66_slice(i);       break;
        case OP66_WELD:       op66_weld(i);        break;
        case OP66_UP:         op66_up(i);          break;
        case OP66_UP_UNIQ:    op66_up_uniq(i);     break;
        case OP66_COUP:       op66_coup(i);        break;
        case OP66_SZ:         op66_sz(i);          break;
        case OP66_IX:         op66_ix(i);          break;
        case OP66_IX0:        op66_ix0(i);         break;
        case OP66_IX1:        op66_ix1(i);         break;
        case OP66_IX2:        op66_ix2(i);         break;
        case OP66_IX3:        op66_ix3(i);         break;
        case OP66_IX4:        op66_ix4(i);         break;
        case OP66_IX5:        op66_ix5(i);         break;
        case OP66_IX6:        op66_ix6(i);         break;
        case OP66_IX7:        op66_ix7(i);         break;
        case OP66_CASE:       op66_case(i);        break;
        case OP66_CASE2:      op66_case2(i);       break;
        case OP66_CASE3:      op66_case3(i);       break;
        case OP66_CASE4:      op66_case4(i);       break;
        case OP66_CASE5:      op66_case5(i);       break;
        case OP66_CASE6:      op66_case6(i);       break;
        case OP66_CASE7:      op66_case7(i);       break;
        case OP66_CASE8:      op66_case8(i);       break;
        case OP66_CASE9:      op66_case9(i);       break;
        case OP66_CASE10:     op66_case10(i);      break;
        case OP66_CASE11:     op66_case11(i);      break;
        case OP66_CASE12:     op66_case12(i);      break;
        case OP66_CASE13:     op66_case13(i);      break;
        case OP66_CASE14:     op66_case14(i);      break;
        case OP66_CASE15:     op66_case15(i);      break;
        case OP66_CASE16:     op66_case16(i);      break;
        case OP66_NIL:        op66_nil(i);         break;
        case OP66_TRUTH:      op66_truth(i);       break;
        case OP66_OR:         op66_or(i);          break;
        case OP66_NOR:        op66_nor(i);         break;
        case OP66_AND:        op66_and(i);         break;
        case OP66_IF: {
            enki_value c = i->stack_v[i->sp - 3];
            enki_value t = i->stack_v[i->sp - 2];
            enki_value e = i->stack_v[i->sp - 1];
            i->sp -= 2;
            i->stack_v[i->sp - 1] = (c != 0) ? t : e;
            break;
        }
        case OP66_IFZ: {
            enki_value c = i->stack_v[i->sp - 3];
            enki_value t = i->stack_v[i->sp - 2];
            enki_value e = i->stack_v[i->sp - 1];
            i->sp -= 2;
            i->stack_v[i->sp - 1] = (c == 0) ? t : e;
            break;
        }
        case OP66_SEQ:        op66_seq(i);         break;
        case OP66_SEQ2:       op66_seq2(i);        break;
        case OP66_SEQ3:       op66_seq3(i);        break;
        case OP66_SAP:        op66_sap(i);         break;
        case OP66_SAP2:       op66_sap2(i);        break;
        case OP66_FORCE:      op66_force(i);       break;
        case OP66_DEEPSEQ:    op66_deepseq(i);     break;
        case OP66_TRY:        op66_try(i);         break;
        case OP66_THROW:      op66_throw(i);       break;
        case OP66_SAVE:       op66_save(i);        break;
        case OP66_LOAD:       op66_load(i);        break;
        case OP66_TRACE:      return;
        case OP66_EQUAL:      op66_equal(i);       break;
        case OP66_PARSE_REX:  return; // TODO: liam
        case OP66_PRINT_REX:  return; // TODO: liam
        default: enki_interp_throw(i, ENKI_ERROR_BAD_TAG, sub_b);
    }
}

void enki_interp_dispatch_op(enki_interpreter* i, uint8_t group) {
    enki_value row_v = enki_value_unind(i->stack_v[i->sp - 1]);
    if(!IS_PTR(row_v)) {
        enki_interp_throw(i, ENKI_ERROR_BAD_TAG, row_v);
    }
    enki_value_header* h = ENKI_AS(enki_value_header, row_v);
    if(h->kind_b != APP) {
        enki_interp_throw(i, ENKI_ERROR_BAD_TAG, row_v);
    }
    enki_app* row = ENKI_AS(enki_app, row_v);
    enki_value tag = row->fn_v;
    enki_value* args = row->args_v;
    size_t n_args = row->n_args_s;
    if(IS_PTR(tag)) {
        enki_interp_throw(i, ENKI_ERROR_BAD_TAG, row_v);
    }
    size_t base_sp = i->sp - 2;
    for(size_t k = 0; k < n_args; k++) {
        i->stack_v[base_sp + k] = args[k];
    }
    i->sp = base_sp + n_args;
    switch (group) {
        case 0: enki_interp_dispatch_op0(i, (uint8_t)tag); break;
        case 66: enki_interp_dispatch_op66(i, (uint8_t)tag); break;
        default: enki_interp_throw(i, ENKI_ERROR_BAD_TAG, tag);
    }
}

void enki_interp_step(enki_interpreter* i) {
    i->stats.interp_step_s++;
    uint8_t op_b = i->bc_b[i->pc++];
    switch (op_b) {
        case OP_APPLY_WIDE: {
            uint8_t lo = i->bc_b[i->pc++];
            uint8_t hi = i->bc_b[i->pc++];
            size_t arg_c = ((size_t)hi << 8) | lo;
            enki_app_apply(i, arg_c);
            break;
        }
        case OP_APPLY: {
            uint8_t arg_c = i->bc_b[i->pc++];
            enki_app_apply(i, arg_c);
            break;
        }
        case OP_PICK_WIDE: {
            uint8_t lo = i->bc_b[i->pc++];
            uint8_t hi = i->bc_b[i->pc++];
            size_t pick_i = ((size_t)hi << 8) | lo;
            size_t stack_i = i->arg_base_s + pick_i;
            i->stack_v[i->sp] = i->stack_v[stack_i];
            i->sp++;
            break;
        }
        case OP_PICK: {
            size_t idx_i = i->arg_base_s + (size_t)i->bc_b[i->pc++];
            i->stack_v[i->sp] = i->stack_v[idx_i];
            i->sp++;
            break;
        }
        case OP_RETURN: {
            if(i->cp == 0) enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
            enki_value ret = i->stack_v[i->sp - 1];
            enki_call* curr = &i->call_stack_v[i->cp - 1];
            i->sp = curr->res_base_s;
            i->stack_v[i->sp] = ret;
            i->cp--;
            if(i->hp > 0) {
                enki_handler hdlr = i->handler_v[i->hp - 1];
                if(hdlr.cp == i->cp) {
                    enki_value app = enki_app_alloc(i->gc, (enki_value)0, 1);
                    enki_app* ptr = ENKI_AS(enki_app, app);
                    ptr->args_v[0] = i->stack_v[i->sp];
                    i->stack_v[i->sp] = app;
                    i->hp--;
                }
            }
            i->sp++;
            if(i->cp == 0){
                i->halted = true;
                return;
            }
            enki_call* caller = &i->call_stack_v[i->cp - 1];
            enki_law* law = ENKI_AS(enki_law, caller->law);
            i->bc_b = ENKI_LAW_BC(law);
            i->const_table_v = ENKI_LAW_CONSTS(law);
            i->pc = caller->pc;
            i->arg_base_s = caller->arg_base_s;
            break;
        }
        case NO_OP:
            break;
        case OP_PUSH_CONST_WIDE: {
            uint8_t lo = i->bc_b[i->pc++];
            uint8_t hi = i->bc_b[i->pc++];
            size_t idx_i = ((size_t)hi << 8) | lo;
            i->stack_v[i->sp] = i->const_table_v[idx_i];
            i->sp++;
            break;
        }
        case OP_PUSH_CONST: {
            uint8_t idx_i = i->bc_b[i->pc++];
            i->stack_v[i->sp] = i->const_table_v[idx_i];
            i->sp++;
            break;
        }
        case OP_POP:
            i->sp--;
            break;
        case OP_DUP:
            if(i->sp == 0) {
                enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
            }
            i->stack_v[i->sp] = i->stack_v[i->sp - 1];
            i->sp++;
            break;
        case OP_OP0: {
            uint8_t sub_b = i->bc_b[i->pc++];
            enki_interp_dispatch_op0(i, sub_b);
            break;
        }
        case OP_OP66: {
            uint8_t sub_b = i->bc_b[i->pc++];
            switch(sub_b) {
                case OP66_INC: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 1];
                    i->stack_v[i->sp - 1] = enki_nat_inc(i->gc, a);
                    break;
                }
                case OP66_DEC: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 1];
                    i->stack_v[i->sp - 1] = enki_nat_dec(i->gc, a);
                    break;
                }
                case OP66_ADD: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_add(i->gc, a, b);
                    break;
                }
                case OP66_SUB: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_sub(i->gc, a, b);
                    break;
                }
                case OP66_MUL: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_mul(i->gc, a, b);
                    break;
                }
                case OP66_EQ: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_eq(a, b);
                    break;
                }
                case OP66_NE: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_ne(a, b);
                    break;
                }
                case OP66_LT: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_lt(a, b);
                    break;
                }
                case OP66_LE: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_le(a, b);
                    break;
                }
                case OP66_GT: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_gt(a, b);
                    break;
                }
                case OP66_GE: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    i->sp--;
                    i->stack_v[i->sp - 1] = enki_nat_ge(a, b);
                    break;
                }
                case OP66_CMP: {
                    i->stats.op66_s[sub_b]++;
                    enki_value a = i->stack_v[i->sp - 2];
                    enki_value b = i->stack_v[i->sp - 1];
                    int cmp = enki_nat_cmp(a, b);
                    i->sp--;
                    i->stack_v[i->sp - 1] =
                        (cmp < 0) ? (enki_value)0 : (cmp == 0 ? (enki_value)1 : (enki_value)2);
                    break;
                }
                case OP66_IF: {
                    i->stats.op66_s[sub_b]++;
                    enki_value c = i->stack_v[i->sp - 3];
                    enki_value t = i->stack_v[i->sp - 2];
                    enki_value e = i->stack_v[i->sp - 1];
                    i->sp -= 2;
                    i->stack_v[i->sp - 1] = (c != 0) ? t : e;
                    break;
                }
                case OP66_IFZ: {
                    i->stats.op66_s[sub_b]++;
                    enki_value c = i->stack_v[i->sp - 3];
                    enki_value t = i->stack_v[i->sp - 2];
                    enki_value e = i->stack_v[i->sp - 1];
                    i->sp -= 2;
                    i->stack_v[i->sp - 1] = (c == 0) ? t : e;
                    break;
                }
                default:
                    enki_interp_dispatch_op66(i, sub_b);
                    break;
            }
            break;
        }
        default:
            enki_interp_throw(i, ENKI_ERROR_BAD_TAG, op_b);
    }
}
