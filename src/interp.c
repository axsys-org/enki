#include "enki/interp.h"
#include "enki/value.h"
#include "enki/gc.h"
#include "enki/primops.h"

enki_interpreter* enki_create_interp(enki_allocator sys, size_t heap, 
    size_t n_const, enki_value* const_table, 
    size_t bc_len, uint8_t bc[]) {
    enki_interpreter* i = (enki_interpreter*)sys.alloc(sizeof(enki_interpreter));
    enki_frame f; 
    f.pc = 0;
    f.bc_len = bc_len;
    f.n_const = n_const;
    f.res_base = 0;
    f.arg_base = 0;
    f.bc = bc;
    f.const_table = const_table;
    i->frame[0] = f;
    i->gc = enki_gc_create(sys, heap, i);
    i->sys = sys;
    i->fp = 0;
    i->sp = 0;
    return i;
}

void enki_trace_interp(enki_interpreter* i) {
    for(size_t k = 0; k < i->sp; k++) {
        i->stack[k] = i->gc->copy(i->gc, i->stack[k]);
    }   
}

void enki_run_interpreter(enki_interpreter* i) {
    enki_frame* f = &i->frame[i->fp];
    while (f->pc < f->bc_len) {
        uint8_t op = f->bc[f->pc++];
        switch (op) {
            case OP_APPLY:
            case OP_PICK: 
            case OP_RETURN:

                break;
            case NO_OP:
                break;
            case OP_PUSH_CONST:
                uint8_t idx = f->bc[f->pc++];
                i->stack[i->sp] = f->const_table[idx];
                i->sp++;
                break;
            case OP_POP:
                i->sp--;
                break;
            case OP_DUP:
                i->stack[i->sp] = i->stack[i->sp - 1];
                i->sp++;
                break;
            case OP_OP0:
                uint8_t sub = f->bc[f->pc++];
                switch (sub) {
                    case 0: primop_mkpin(i->gc, i);  break;
                    case 1: primop_mklaw(i->gc, i);  break;
                    case 2: primop_match(i->gc, i);  break;
                    default: return;
                }
                break;
            default:
                return;
        }
    }
}
