
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "enki/app.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/value.h"
#include "enki/util.h"
#include "enki/print.h"

enki_value enki_app_alloc(enki_gc* gc, enki_value fn_v,
    size_t n_args_s) {
    size_t n = sizeof(enki_app) + (n_args_s * sizeof(enki_value));
    enki_app* new = (enki_app*)enki_gc_alloc_locked(gc, n, _Alignof(enki_app));
    new->h.size_s = n;
    new->h.kind_b = ENKI_APP;
    new->h.state_b = WHNF;
    new->fn_v = fn_v;
    new->n_args_s = n_args_s;
    //if(n_args_s > 0 && args_v != NULL) memcpy(new->args_v, args_v, n_args_s * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}

enki_value enki_app_cont_alloc(enki_gc* gc, size_t n_args_s, enki_value* bas_v) {
    size_t n = sizeof(enki_cont) + (n_args_s * sizeof(enki_value));
    enki_cont* new = (enki_cont*)enki_gc_alloc_locked(gc, n, _Alignof(enki_cont));
    new->h.size_s = n;
    new->h.kind_b = ENKI_CONT;
    new->h.state_b = WHNF;
    new->n_args_s = n_args_s;
    memcpy(new->args_v, bas_v, n_args_s * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}

static void enki_app_complete(size_t arity_s, size_t n_args_s,
    size_t fn_index_i, enki_app* app, enki_interpreter* i) {
    i->stack_v[fn_index_i] = app->fn_v;
    size_t off_o = fn_index_i + 1;
    for (size_t k = n_args_s; k > 0; k--) {
        size_t idx_i = k - 1;
        i->stack_v[off_o + idx_i + app->n_args_s] = i->stack_v[off_o + idx_i];
    }
    for(size_t k = 0; k < app->n_args_s; k++) {
        i->stack_v[off_o + k] = app->args_v[k];
    }
    i->sp = fn_index_i + app->n_args_s + n_args_s + 1;
    enki_law_enter(arity_s, app->fn_v, i);
}

size_t enki_app_arity(enki_value val_v) {
    if(!IS_PTR(val_v)) return 0;
    enki_value_header* h = ENKI_AS(enki_value_header, val_v);
    switch(h->kind_b) {
        case LAW: {
            enki_law* law = ENKI_AS(enki_law, val_v);
            return law->arity_s;
        }
        case APP: {
            enki_app* app = ENKI_AS(enki_app, val_v);
            size_t fn_arity = enki_app_arity(app->fn_v);
            if(fn_arity <= app->n_args_s) return 0;
            return fn_arity - app->n_args_s;
        }
        case PIN:
            return 0; // pins are not transparent
        case NAT:
            return 0;
        default:
            return 0;
    }
}

static void enki_app_make_cont(size_t fn_index_i, size_t needed,
    size_t n_args_s, enki_interpreter* i) {
    size_t xt_args_c_s = n_args_s - needed;
    enki_value* bas_v = &i->stack_v[fn_index_i + 1 + needed];
    enki_value cont_v = enki_app_cont_alloc(i->gc, xt_args_c_s, bas_v);
    enki_frame f;
    i->sp -= xt_args_c_s;
    f.res_base_s = fn_index_i;
    f.pc = 0;
    f.law = 0;
    f.arg_base_s = 0;
    f.cont_v = cont_v;
    i->fp++;
    i->frame[i->fp] = f;
}

static void enki_app_make_partial_apply(enki_interpreter* i, size_t fn_index_i, enki_value fn_v,
    const enki_value* old_args_v, size_t n_old_args_s, size_t n_new_args_s) {
    size_t scratch_s = i->sp;
    if(old_args_v != NULL) {
        for(size_t k = 0; k < n_old_args_s; k++) {
            i->stack_v[i->sp] = old_args_v[k];
            i->sp++;
        }
    }
    enki_value app = enki_app_alloc(i->gc, fn_v, n_old_args_s + n_new_args_s);
    enki_app* ptr = ENKI_AS(enki_app, app);
    if(old_args_v != NULL) {
        enki_app* old_app = ENKI_AS(enki_app, i->stack_v[fn_index_i]);
        ptr->fn_v = old_app->fn_v;
    }
    else {
        ptr->fn_v = i->stack_v[fn_index_i];
    }
    if(n_old_args_s > 0 && old_args_v != NULL) {
        memcpy(ptr->args_v, &i->stack_v[scratch_s], sizeof(enki_value) * n_old_args_s);
    }
    for(size_t k = 0; k < n_new_args_s; k++) {
        ptr->args_v[k + n_old_args_s] = i->stack_v[fn_index_i + 1 + k];
    }
    i->stack_v[fn_index_i] = app; // pop stack_v and set result_v to app
    i->sp = fn_index_i + 1;
}

void enki_app_apply(enki_interpreter* i, size_t n_args_s) {
    size_t fn_index_i = i->sp - ((size_t)n_args_s + 1);
    enki_value head_v = i->stack_v[fn_index_i];
    fprintf(stderr, "applying hd: %s\n", enki_pvalue(&sys_a, head_v));
    if (!IS_PTR(head_v) ) {
      enki_value ret_v = enki_alloc_row(i->gc, head_v, n_args_s, &i->stack_v[i->sp - n_args_s]);
      i->sp -= n_args_s;
      i->stack_v[--(i->sp)] = ret_v;
      return;
    }
    // if(!IS_PTR(head_v)) enki_interp_throw(i, ENKI_ERROR_TYPE, head_v);
    enki_value_header* h = ENKI_AS(enki_value_header, head_v);
    switch(h->kind_b) {
        case LAW: {
            enki_law* law = ENKI_AS(enki_law, head_v);
            if(law->arity_s == n_args_s) {
                enki_law_enter(n_args_s, head_v, i);
            }
            else if(law->arity_s > n_args_s) {
                enki_app_make_partial_apply(i, fn_index_i, head_v, NULL, 0, n_args_s);
            }
            else {
                enki_app_make_cont(fn_index_i, law->arity_s, n_args_s, i);
                enki_law_enter(law->arity_s, head_v, i);
            }
            return;
        }
        case APP: {
             enki_app* app = ENKI_AS(enki_app, head_v);
             size_t arity_s = enki_app_arity(app->fn_v);
             size_t new_arg_c_s = app->n_args_s + (size_t)n_args_s;
             // if(!IS_PTR(app->fn_v)) enki_interp_throw(i, ENKI_ERROR_TYPE, app->fn_v);
             enki_value_header* fn_h = ENKI_AS(enki_value_header, app->fn_v);
             if(new_arg_c_s == arity_s) {
                switch(fn_h->kind_b) {
                    case LAW:
                        enki_app_complete(arity_s, n_args_s, fn_index_i, app, i);
                        break;
                    default:
                        fprintf(stderr, "bad tag %s\n", enki_pvalue(&sys_a, head_v));
                        fflush(stderr);
                        enki_interp_throw(i, ENKI_ERROR_BAD_TAG, head_v);
                }
             }
             else if(new_arg_c_s < arity_s){
                enki_app_make_partial_apply(i, fn_index_i, app->fn_v, app->args_v, app->n_args_s, n_args_s);
                return;
            }
             else {
                switch(fn_h->kind_b) {
                    case LAW: {
                        size_t needed = arity_s - app->n_args_s;
                        enki_app_make_cont(fn_index_i, needed, n_args_s, i);
                        enki_app_complete(arity_s, needed, fn_index_i, app, i);
                        break;
                    }
                    default:
                        fprintf(stderr, "bad tag %s\n", enki_pvalue(&sys_a, head_v));
                        fflush(stderr);
                        enki_interp_throw(i, ENKI_ERROR_BAD_TAG, head_v);
                }
             }
             return;
        }

        default:
            enki_interp_throw(i, ENKI_ERROR_BAD_TAG, head_v);
    }
}

enki_value enki_alloc_pair(enki_gc* gc, enki_value l_v, enki_value r_v)
{
    enki_app* app = enki_alloc_app_bare(gc, l_v, 1);
    app->args_v[0] = r_v;
    return PTR_TO_ENKI(app);
}

enki_value enki_alloc_trel(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, 2);
    app->args_v[0] = one_v;
    app->args_v[1] = two_v;
    return PTR_TO_ENKI(app);
}
enki_value enki_alloc_quad(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v,
                           enki_value tri_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, 3);
    app->args_v[0] = one_v;
    app->args_v[1] = two_v;
    app->args_v[2] = tri_v;
    return PTR_TO_ENKI(app);
}

enki_value enki_alloc_row(enki_gc* gc, enki_value fn_v, size_t arg_s, enki_value* arg_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, arg_s);
    memcpy(app->args_v, arg_v, sizeof(enki_value) * arg_s);
    return PTR_TO_ENKI(app);
}

enki_value enki_app_hd(enki_value app_v)
{
    enki_app* app = (enki_app*)ENKI_TO_PTR(app_v);
    char kin_b = app->h.kind_b;
    ea_assertf(kin_b == ENKI_APP, "not app %u\n", kin_b);
    return app->fn_v;
}
/// XX: this should be discouraged in hot loops,
/// avoid unmasking and remasking
enki_value enki_app_idx(enki_value app_v, size_t idx_s)
{
    enki_app* app = (enki_app*)ENKI_TO_PTR(app_v);
    char kin_b = app->h.kind_b;
    ea_assertf(kin_b == ENKI_APP, "not app %u\n", kin_b);
    return app->args_v[idx_s];
}



enki_value enki_alloc_quin(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v,
                           enki_value tri_v, enki_value qua_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, 4);
    app->args_v[0] = one_v;
    app->args_v[1] = two_v;
    app->args_v[2] = tri_v;
    app->args_v[3] = qua_v;
    return PTR_TO_ENKI(app);
}


enki_app* enki_alloc_app_bare(enki_gc* gc, enki_value fn_v, size_t n_args_s)
{
  enki_value_header* hed = IS_PTR(fn_v) ? ENKI_TO_PTR(fn_v) : NULL;

  ea_assertf(
      hed == NULL || hed->kind_b != ENKI_APP,
      "cannot put app as hd of another app %s",
      enki_print_value(&sys_a, fn_v, NULL));

    size_t n = sizeof(enki_app) + (n_args_s * sizeof(enki_value));
    enki_app* new = (enki_app*)gc->alloc(gc, n, _Alignof(enki_app));
    if (!new)
        return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_APP;
    new->h.state_b = WHNF;
    new->fn_v = fn_v;
    new->n_args_s = n_args_s;
    // if(n_args_s > 0 && args_v != NULL) memcpy(new->args_v, args_v, n_args_s *
    // sizeof(enki_value));
    return new;
}


