#include <stdlib.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/judge.h"
#include "enki/value.h"

enki_value enki_kal(enki_interpreter* i, size_t depth, size_t env, enki_value body) {
    if(!IS_PTR(body) && body <= depth && body != 0){
        return i->stack[env + depth - body];
    }
    if(!IS_PTR(body)) return body;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(body);
    if(h->kind != ENKI_APP) return body;
    enki_app* app = (enki_app*)ENKI_TO_PTR(body);
    if(app->fn == 0 && app->n_args == 2) { 
        size_t base_fp = i->fp;
        size_t base_sp = i->sp;
        i->stack[i->sp++] = enki_kal(i, depth, env, app->args[0]);
        i->stack[i->sp++] = enki_kal(i, depth, env, app->args[1]);
        enki_apply(i, 1);
        while(!i->halted && i->fp > base_fp) {
            enki_step(i);
        }
        enki_value res = i->stack[base_sp];
        i->sp = base_sp;
        return res;
    }
    else if(app->fn == 0 && app->n_args == 1) {
        return app->args[0];
    } 
    return body;
}

static int enki_is_letrec(enki_value body, enki_value* v_out, enki_value* k_out) {

    if(!IS_PTR(body)) return 0;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(body);
    if(h->kind != ENKI_APP) return 0;
    enki_app* app = (enki_app*)ENKI_TO_PTR(body);
    if(app->fn != (enki_value)1) return 0;
    if(app->n_args != 2) return 0;
    *v_out = app->args[0];
    *k_out = app->args[1];
    return 1;
}

enki_value enki_judge(enki_interpreter* i, enki_value val, size_t arg_base) {
    if(!IS_PTR(val)) exit(1);
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(val);
    if(h->kind != ENKI_LAW) exit(1);
    enki_law* law = (enki_law*)ENKI_TO_PTR(val);
    size_t depth = law->arity;
    size_t env_base = i->sp;
    for(size_t k = 0; k < law->arity; k++) {
        i->stack[i->sp++] = i->stack[arg_base + k];
    }
    enki_value body = law->body;
    enki_value k_out;
    enki_value v_out;
    while (enki_is_letrec(body, &v_out, &k_out)) {
        enki_value local = enki_kal(i, depth, env_base, v_out);
        i->stack[i->sp++] = local;
        depth++;
        body = k_out;
    }
    enki_value res = enki_kal(i, depth, env_base, body);
    i->sp = env_base;
    return res;
}
