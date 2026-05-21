#include "enki/interp.h"
#include "enki/value.h"
#include "enki/gc.h"
#include "enki/primops.h"

enki_interpreter* enki_create_interp(enki_allocator sys, size_t heap, 
   enki_value law) {
    enki_interpreter* i = (enki_interpreter*)sys.alloc(sizeof(enki_interpreter));
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
    i->sys.free(i);
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
        case OP_APPLY: 
            uint8_t arg_c = ENKI_LAW_BC(law)[f->pc++];
            enki_apply(i, arg_c); 
            break;
        case OP_PICK:  
            size_t idx = f->arg_base + (size_t)ENKI_LAW_BC(law)[f->pc++];
            i->stack[i->sp] = i->stack[idx];
            i->sp++;
            break;
        case OP_RETURN:
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
        case NO_OP:
            break;
        case OP_PUSH_CONST:
            uint8_t idx = ENKI_LAW_BC(law)[f->pc++];
            i->stack[i->sp] = ENKI_LAW_CONSTS(law)[idx];
            i->sp++;
            break;
        case OP_POP:
            i->sp--;
            break;
        case OP_DUP:
            if(i->sp == 0) return; // stack underflow 
            i->stack[i->sp] = i->stack[i->sp - 1];
            i->sp++;
            break;
        case OP_OP0:
            uint8_t sub = ENKI_LAW_BC(law)[f->pc++];
            switch (sub) {
                case 0: primop_mkpin(i);  break;
                case 1: primop_mklaw(i);  break;
                case 2: primop_match(i);  break;
                default: return;
            }
            break;
        default:
            return;
    }
}