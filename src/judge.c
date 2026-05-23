#include <stdlib.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/judge.h"
#include "enki/value.h"

enki_value enki_kal(enki_interpreter* i, size_t depth_s, size_t env_i, enki_value body_v) {
    if(!IS_PTR(body_v) && body_v <= depth_s && body_v != 0){
        return i->stack_v[env_i + depth_s - body_v];
    }
    if(!IS_PTR(body_v)) return body_v;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(body_v);
    if(h->kind_b != ENKI_APP) return body_v;
    enki_app* app = (enki_app*)ENKI_TO_PTR(body_v);
    if(app->fn_v == 0 && app->n_args_s == 2) {
        size_t base_fp = i->fp;
        size_t base_sp = i->sp;
        i->stack_v[i->sp++] = enki_kal(i, depth_s, env_i, app->args_v[0]);
        i->stack_v[i->sp++] = enki_kal(i, depth_s, env_i, app->args_v[1]);
        enki_apply(i, 1);
        while(!i->halted && i->fp > base_fp) {
            enki_step(i);
        }
        enki_value res_v = i->stack_v[base_sp];
        i->sp = base_sp;
        return res_v;
    }
    else if(app->fn_v == 0 && app->n_args_s == 1) {
        return app->args_v[0];
    } 
    return body_v;
}

static int enki_is_letrec(enki_value body_v, enki_value* v_out, enki_value* k_out) {

    if(!IS_PTR(body_v)) return 0;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(body_v);
    if(h->kind_b != ENKI_APP) return 0;
    enki_app* app = (enki_app*)ENKI_TO_PTR(body_v);
    if(app->fn_v != (enki_value)1) return 0;
    if(app->n_args_s != 2) return 0;
    *v_out = app->args_v[0];
    *k_out = app->args_v[1];
    return 1;
}

enki_value enki_judge(enki_interpreter* i, enki_value val_v, size_t arg_base_s) {
    if(!IS_PTR(val_v)) exit(1);
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(val_v);
    if(h->kind_b != ENKI_LAW) exit(1);
    enki_law* law = (enki_law*)ENKI_TO_PTR(val_v);
    size_t depth_s = law->arity_s;
    size_t env_base = i->sp;
    for(size_t k = 0; k < law->arity_s; k++) {
        i->stack_v[i->sp++] = i->stack_v[arg_base_s + k];
    }
    enki_value body_v = law->body_v;
    enki_value k_out;
    enki_value v_out;
    while (enki_is_letrec(body_v, &v_out, &k_out)) {
        enki_value local_v = enki_kal(i, depth_s, env_base, v_out);
        i->stack_v[i->sp++] = local_v;
        depth_s++;
        body_v = k_out;
    }
    enki_value res_v = enki_kal(i, depth_s, env_base, body_v);
    i->sp = env_base;
    return res_v;
}
