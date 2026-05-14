#include "enki/interp.h"
#include "enki/value.h"
#include "enki/gc.h"
#include "enki/primops.h"

void interp(enki_gc* gc) {
    enki_interp* i = AS_PTR(gc->root);
    while (i->pc < i->bc_len) {
        uint8_t op = i->bc[i->pc++];
        switch (op) {
            case NO_OP:
                break;
            case OP_PUSH_CONST:
                i->stack[i->sp++] = MAKE_IMM(i->bc[i->pc++]);
                break;
            case OP_POP:
                i->sp--;
                break;
            case OP_DUP:
                i->stack[i->sp] = i->stack[i->sp - 1];
                i->sp++;
                break;
            case OP_OP0: {
                uint8_t sub = i->bc[i->pc++];
                switch (sub) {
                    case 0: primop_mkpin(i);  break;
                    case 1: primop_mklaw(i);  break;
                    case 2: primop_match(i);  break;
                    default: return;
                }
                i = AS_PTR(gc->root);
                break;
            }
            default:
                return;
        }
    }
}
