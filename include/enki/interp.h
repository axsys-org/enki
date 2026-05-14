#ifndef ENKI_INTERP_H
#define ENKI_INTERP_H

#include "enki/gc.h"

typedef enum {
    NO_OP         = 0x00,
    OP_PUSH_CONST = 0x01,    // push a constant by index from the const table
    OP_POP        = 0x02,
    OP_DUP        = 0x03,
    OP_OP0        = 0x10,    // family 0: pin/law/match — next byte = sub-op
    OP_OP66       = 0x42,    // family 66: standard lib — next byte = sub-op
    OP_OP82       = 0x52,    // family 82: IO — next byte = sub-op
    OP_RETURN     = 0xFF,
} enki_opcode;

// Sub-opcodes for op 66
typedef enum {
    OP66_INC = 0,
    OP66_DEC = 1,
    OP66_ADD = 2,
    OP66_SUB = 3,
    OP66_EQ  = 4,
    // ...
} enki_op66_sub;

void interp(enki_gc* gc);

#endif
