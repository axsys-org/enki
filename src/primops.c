
enki_value enki_alloc_nat(enki_gc* gc, size_t n_bytes, uint8_t bytes[]);
enki_value enki_alloc_law(enki_gc* gc, uint32_t arity, enki_value name, enki_value body, uint32_t bc_len, const uint8_t bc[]);
enki_value enki_alloc_pin(enki_gc* gc, const uint8_t hash[32], enki_value inner, size_t n_subpins, enki_value subpins[]); 
enki_value enki_alloc_app(enki_gc* gc, enki_value fn, size_t n_args, const enki_value args[]);

void primop_mkpin(enki_gc* gc, enki_interp* i) {
    enki_value inner = i->stack[i->sp - 1];
    /* TODO: TEMPORARY */
    const enki_value* subpins = NULL; 
    size_t n_subpins = 0;
    uint8_t hash[32];
    memset(hash, 0, 32); 
    ///
    enki_value pin = enki_alloc_pin(gc, hash, inner, n_subpins, subpins);
    i->stack[i->sp - 1] = pin;
}

void primop_mklaw(enki_interp* i) {

    enki_value a = i->stack[i->sp - 3];
    enki_value name = i->stack[i->sp - 2];
    enki_value body = i->stack[i->sp - 1];
    // TODO handle Big nat arity
    size_t arity;
    if(!IS_PTR(a)) { arity = (size_t)a + 1; } 
    /*TODO: TEMPORARY later call compiler on body for bc cache*/
    size_t bc_len = 0;
    const uint8_t* bc = NULL;
    ///
    enki_value law = enki_alloc_law(i->gc, arity, name, body, bc_len, bc);
    i->sp -= 2;
    i->stack[i->sp - 1] = law; 
}

void primop_match(enki_interp* i) {


    enki_value o = i->stack[i->sp - 5];
    enki_value p = i->stack[i->sp - 4];
    enki_value l = i->stack[i->sp - 3];
    enki_value n = i->stack[i->sp - 2];
    enki_value a = i->stack[i->sp - 1];


    

    // TODO  
}


void apply() {



}


/* 
TODO after 
SMALL NAT OPS:
Inc Dec Add Sub Eq Ne Lt Le Gt Ge Type IsNat IsPin IsLaw IsApp

Rows/App utilities 
Sz Ix Hd Last Init Row

Law Entry/Frames:
save return pc/bc/consts
install law bc/consts
arrange args
return

Pin real semantics: 
walk inner
collect subpins
canonicalize
hash

Boxed NAT later 
*/
