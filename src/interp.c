#include <stdint.h>
#include <string.h>

#include "enki/apply.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/primops.h"
#include "enki/value.h"

static uint8_t pop_byte(enki_interpreter* i)
{
    return *(i->pc++);
}

static void load_law_registers(enki_interpreter* i, enki_law* law)
{
    i->law = law;
    i->byt_b = law == NULL ? NULL : ENKI_LAW_BC(law);
    i->pc = i->byt_b;
    i->dep_q = law == NULL ? 0 : law->arity_s;
}

static void trace_law_register(enki_gc* gc, enki_law** law, uint8_t** pc)
{
    if (*law == NULL) {
        return;
    }

    size_t pc_off_s = 0;
    bool has_pc = pc != NULL && *pc != NULL;
    if (has_pc) {
        pc_off_s = (size_t)((uintptr_t)(*pc) - (uintptr_t)ENKI_LAW_BC(*law));
    }

    enki_value copied_v = gc->copy(gc, PTR_TO_ENKI(*law));
    *law = (enki_law*)ENKI_TO_PTR(copied_v);

    if (has_pc) {
        *pc = ENKI_LAW_BC(*law) + pc_off_s;
    }
}

enki_interpreter* enki_create_interp(enki_allocator sys_a, size_t heap, enki_law* law)
{
    enki_interpreter* i = (enki_interpreter*)sys_a.alloc(sys_a.ctx, sizeof(enki_interpreter));
    if (i == NULL) {
        return NULL;
    }

    memset(i, 0, sizeof(*i));
    i->sys_a = sys_a;
    i->gc = enki_gc_create(sys_a, heap, i);
    if (i->gc == NULL) {
        sys_a.free(sys_a.ctx, i);
        return NULL;
    }

    i->halted = false;
    load_law_registers(i, law);
    return i;
}

void enki_trace_interp(enki_interpreter* i)
{
    for (size_t k = 0; k < i->sp; k++) {
        i->stack_v[k] = i->gc->copy(i->gc, i->stack_v[k]);
    }

    trace_law_register(i->gc, &i->law, &i->pc);
    i->byt_b = i->law == NULL ? NULL : ENKI_LAW_BC(i->law);

    for (size_t k = 0; k < i->fp; k++) {
        trace_law_register(i->gc, &i->frame[k].law, &i->frame[k].pc);
    }
}

void enki_destroy(enki_interpreter* i)
{
    enki_gc_destroy(i->gc);
    i->sys_a.free(i->sys_a.ctx, i);
}

void enki_halt(enki_interpreter* i)
{
    i->halted = true;
}

void enki_run(enki_interpreter* i)
{
    while (!i->halted) {
        enki_step(i);
    }
}

void enki_save_frame(enki_interpreter* i)
{
    enki_frame* frame = &i->frame[i->fp++];
    frame->pc = i->pc;
    frame->bp = i->bp;
    frame->sp = i->sp;
    frame->dep_q = i->dep_q;
    frame->law = i->law;
}

void enki_restore_frame(enki_interpreter* i)
{
    if (i->fp == 0) {
        i->law = NULL;
        i->pc = NULL;
        i->byt_b = NULL;
        i->bp = 0;
        i->dep_q = 0;
        i->halted = true;
        return;
    }

    enki_frame frame = i->frame[--i->fp];
    i->pc = frame.pc;
    i->bp = frame.bp;
    i->dep_q = frame.dep_q;
    i->law = frame.law;
    i->byt_b = i->law == NULL ? NULL : ENKI_LAW_BC(i->law);
}

void enki_load_frame(enki_interpreter* i, enki_law* law)
{
    i->fp = 0;
    i->bp = 0;
    load_law_registers(i, law);
    i->halted = law == NULL;
}

void enki_run_until(enki_interpreter* i)
{
    size_t fp_s = i->fp;
    enki_law* law = i->law;
    do {
        enki_step(i);
    } while (!i->halted && (i->fp > fp_s || i->law != law));
}

static void dispatch_op66(enki_interpreter* i, uint8_t sub_b)
{
    switch (sub_b) {
        case OP66_PIN:        primop_mkpin(i);     break;
        case OP66_LAW:        primop_mklaw(i);     break;
        case OP66_ELIM:       primop_match(i);     break;
        case OP66_INC:        op66_inc(i);         break;
        case OP66_DEC:        op66_dec(i);         break;
        case OP66_ADD:        op66_add(i);         break;
        case OP66_SUB:        op66_sub(i);         break;
        case OP66_MUL:        op66_mul(i);         break;
        case OP66_DIV:        op66_div(i);         break;
        case OP66_MOD:        op66_mod(i);         break;
        case OP66_EQ:         op66_eq(i);          break;
        case OP66_NE:         op66_ne(i);          break;
        case OP66_LT:         op66_lt(i);          break;
        case OP66_LE:         op66_le(i);          break;
        case OP66_GT:         op66_gt(i);          break;
        case OP66_GE:         op66_ge(i);          break;
        case OP66_CMP:        op66_cmp(i);         break;
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
        case OP66_LOADVAR:    break;
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
        case OP66_IF:         op66_if(i);          break;
        case OP66_IFZ:        op66_ifz(i);         break;
        case OP66_SEQ:        op66_seq(i);         break;
        case OP66_SEQ2:       op66_seq2(i);        break;
        case OP66_SEQ3:       op66_seq3(i);        break;
        case OP66_SAP:        op66_sap(i);         break;
        case OP66_SAP2:       op66_sap2(i);        break;
        case OP66_FORCE:      op66_force(i);       break;
        case OP66_DEEPSEQ:    op66_deepseq(i);     break;
        case OP66_EQUAL:      op66_eq(i);          break;
        default:                                      break;
    }
}

void enki_step(enki_interpreter* i)
{
    if (i->law == NULL || i->pc == NULL) {
        enki_halt(i);
        return;
    }

    uint8_t op_b = pop_byte(i);
    switch (op_b) {
        case NO_OP:
            break;
        case OP_PUSH_CONST: {
            uint8_t idx_i = pop_byte(i);
            i->stack_v[i->sp++] = ENKI_LAW_CONSTS(i->law)[idx_i];
            break;
        }
        case OP_PICK: {
            uint8_t idx_i = pop_byte(i);
            i->stack_v[i->sp++] = i->stack_v[i->bp + idx_i];
            break;
        }
        case OP_POP:
            if (i->sp > 0) {
                i->sp--;
            }
            break;
        case OP_UPD: {
            uint8_t width_b = pop_byte(i);
            if (width_b > 0 && i->sp >= width_b) {
                enki_upd_lets(i, &i->stack_v[i->sp - width_b], width_b);
            }
            break;
        }
        case OP_EVAL: {
            uint8_t depth_b = pop_byte(i);
            if (i->sp > depth_b) {
                size_t off_s = i->sp - 1u - depth_b;
                i->stack_v[off_s] = enki_eval_whnf(i, i->stack_v[off_s]);
            }
            break;
        }
        case OP_CALL:
        case OP_APPLY: {
            uint8_t arg_c = pop_byte(i);
            enki_apply(i, arg_c);
            break;
        }
        case OP_RETURN: {
            enki_value ret_v = i->sp == 0 ? 0 : i->stack_v[i->sp - 1];
            size_t res_s = i->bp;
            i->stack_v[res_s] = ret_v;
            i->sp = res_s + 1;
            enki_restore_frame(i);
            break;
        }
        case OP_DUP:
            if (i->sp > 0) {
                i->stack_v[i->sp] = i->stack_v[i->sp - 1];
                i->sp++;
            }
            break;
        case OP_OP0: {
            uint8_t sub_b = pop_byte(i);
            switch (sub_b) {
                case 0: primop_mkpin(i); break;
                case 1: primop_mklaw(i); break;
                case 2: primop_match(i); break;
                default:                  break;
            }
            break;
        }
        case OP_OP66:
            dispatch_op66(i, pop_byte(i));
            break;
        default:
            enki_halt(i);
            break;
    }
}
