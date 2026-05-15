
void enki_complete_app(size_t arity, enki_value arg, enki_app* app, enki_interpreter* i) {
    i->sp -= 2;
    i->stack[i->sp] = app->fn;
    i->sp++;
    for(size_t k = 0; k < app->n_args; k++) {
        i->stack[i->sp] = app->args[k];
        i->sp++;
    }
    i->stack[i->sp] = arg;
    i->sp++;
    enki_law* law = (enki_law*)ENKI_TO_PTR(app->fn);
    enki_enter_law(arity, law, i);
}


void enki_enter_law(size_t arity, enki_law* law, enki_interpreter* i) {
    enki_frame f;
    f.bc = law->bc;
    f.bc_len = law->bc_len; 
    f.pc = 0;
    f.n_const = law->n_const;
    f.const_table = law->const_table;  
    size_t call_width = arity + 1; // head + all the args 
    f.res_base = i->sp - call_width; 
    f.arg_base = f.res_base + 1;
    i->fp += 1; // go to next frame and set it
    i->frame[i->fp] = f;
}

size_t enki_arity(enki_value val) {
    if(!IS_PTR(val)) return 0; 
    enki_value_header* h = ENKI_TO_PTR(val);
    switch(h->kind) {
        case LAW: 
            enki_law* law = ENKI_TO_PTR(val);
            return law->arity;
        case APP: 
            enki_app* app = ENKI_TO_PTR(val);
            size_t fn_arity = enki_arity(app->fn);
            if(fn_arity <= app->n_args) return 0;
            return fn_arity - app->n_args;
        case PIN:
            return 0; // pins are not transparent
        case NAT:
            return 0;
        default: 
            return 0;
    }
}

void enki_apply(enki_interpreter* i) {
    enki_value head = i->stack[i->sp - 2];
    enki_value arg = i->stack[i->sp - 1];
    if(!IS_PTR(head)) return; // TODO error out 
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(head);
    switch(h->kind) {
        case PIN:
            return; // TODO later once 
        case LAW:
            enki_law* law = (enki_law*)ENKI_TO_PTR(head);  
            if(law->arity == 0) {
                return; // TODO error
            }     
            else if(law->arity == 1) {
                enki_enter_law(1, law, i);
            }
            else {
                enki_value app = enki_alloc_app(i->gc, head, 1);
                ((enki_app*)ENKI_TO_PTR(app))->args[0] = arg;
                i->stack[i->sp - 2] = app; // pop stack and set result to app
                i->sp--;    
            }
            return;
        case APP:
             enki_app* app = (enki_app*)ENKI_TO_PTR(head);
             size_t arity = enki_arity(app->fn);
             size_t new_arg_c = app->n_args + 1;
             if(new_arg_c == arity) {
                enki_value_header* h = ENKI_TO_PTR(app->fn);
                switch(h->kind) {
                    case LAW:
                        enki_complete_app(arity, arg,  app, i);
                        return;
                    default:
                        // TODO will add thunks, closures, and explicity callable values later 
                        break;
                }
             }
             else if(new_arg_c < arity){
                enki_value new = enki_alloc_app(i->gc, app->fn, new_arg_c);
                enki_app* new_app = (enki_app*)ENKI_TO_PTR(new);
                memcpy(new_app->args, app->args, app->n_args * sizeof(enki_value));
                new_app->args[new_arg_c - 1] = arg;
                i->stack[i->sp - 2] = new;
                i->sp--;
             }
             else {
                // TODO: deal with overapplying later...
             }

        default: 
            return;
    }
}


void enki_apply_n(enki_interpreter* i, size_t n_args) {
    //TODO 
    
}