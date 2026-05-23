#include "enki/interp.h"
#include "enki/apply.h"
#include "enki/value.h"
#include "enki/gc.h"
#include "enki/judge.h"
#include "enki/op66.h"
#include "enki/primops.h"

enki_interpreter* enki_create_interp(enki_allocator sys, size_t heap, 
   enki_value law) {
    enki_interpreter* i = (enki_interpreter*)sys.alloc(sys.ctx, sizeof(enki_interpreter));
    enki_frame f; 
    f.pc = 0;
    f.res_base = 0;
    f.arg_base = 0;
    f.cont = 0;
    f.law = law;
    i->frame[0] = f;
    i->gc = enki_gc_create(sys, heap, i);
    i->sys = sys;
    i->halted = false;
    i->fp = 0;
    i->sp = 0;
    return i;
}
void enki_trace_interp(enki_interpreter* i) {
    for(size_t k = 0; k < i->sp; k++) {
        i->stack[k] = i->gc->copy(i->gc, i->stack[k]);
    } 
    for(size_t k = 0; k <= i->fp; k++) {
        enki_frame* f = &i->frame[k];
        if(f->cont != 0) f->cont = i->gc->copy(i->gc, f->cont);
        if(f->law != 0) f->law = i->gc->copy(i->gc, f->law);
    }  
}
void enki_destroy(enki_interpreter* i) {
    enki_gc_destroy(i->gc);
    i->sys.free(i->sys.ctx, i);
}
void enki_halt(enki_interpreter* i) {
    i->halted = true;
}
void enki_run(enki_interpreter* i) {
    while (!i->halted) {
        enki_step(i);
    }
}

void enki_step(enki_interpreter* i) {
    enki_frame* f = &i->frame[i->fp];
    if(f->cont != 0) {
        enki_cont* cont = (enki_cont*)ENKI_TO_PTR(f->cont);
        i->sp = f->res_base + 1;
        for(size_t k = 0; k < cont->n_args; k++) {
            i->stack[i->sp] = cont->args[k];
            i->sp++;
        }
        i->fp--;
        enki_apply(i, cont->n_args);
        return;
    }
    enki_law* law = (enki_law*)ENKI_TO_PTR(f->law);
    uint8_t op = ENKI_LAW_BC(law)[f->pc++];
    switch (op) {
        case OP_APPLY: {
            uint8_t arg_c = ENKI_LAW_BC(law)[f->pc++];
            enki_apply(i, arg_c); 
            break;
        }
        case OP_PICK: { 
            size_t idx = f->arg_base + (size_t)ENKI_LAW_BC(law)[f->pc++];
            i->stack[i->sp] = i->stack[idx];
            i->sp++;
            break;
        }
        case OP_RETURN: {
            enki_value ret_val = i->stack[i->sp - 1];
            if (i->fp == 0) {
                i->halted = true;
                return;
            }
            i->sp = f->res_base;
            i->stack[i->sp] = ret_val; 
            i->sp++;
            i->fp--;                
            break;
        }
        case NO_OP:
            break;
        case OP_PUSH_CONST: {
            uint8_t idx = ENKI_LAW_BC(law)[f->pc++];
            i->stack[i->sp] = ENKI_LAW_CONSTS(law)[idx];
            i->sp++;
            break;
        }
        case OP_POP:
            i->sp--;
            break;
        case OP_DUP:
            if(i->sp == 0) return; // stack underflow 
            i->stack[i->sp] = i->stack[i->sp - 1];
            i->sp++;
            break;
        case OP_JUDGE: {
            enki_value law_v = f->law;
            size_t arg_base = f->arg_base;
            enki_value res = enki_judge(i, law_v, arg_base);
            i->stack[i->sp] = res;
            i->sp++;
            break;
        }
        case OP_OP0: {
            uint8_t sub = ENKI_LAW_BC(law)[f->pc++];
            switch (sub) {
                case 0: primop_mkpin(i);  break;
                case 1: primop_mklaw(i);  break;
                case 2: primop_match(i);  break;
                default: return;
            }
            break;
        }
        case OP_OP66: {
            uint8_t sub = ENKI_LAW_BC(law)[f->pc++];
            switch (sub) {
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
                case OP66_LOADVAR:    return;
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
                case OP66_TRY:        return;
                case OP66_THROW:      return;
                case OP66_SAVE:       return;
                case OP66_LOAD:       return;
                case OP66_TRACE:      return;
                case OP66_EQUAL:      op66_eq(i);          break;
                case OP66_PARSE_REX:  return;
                case OP66_PRINT_REX:  return;
                default: return;
            }
            break;
        }
        default:
            return;
    }
}
