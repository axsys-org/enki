#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "enki/apply.h"
#include "enki/interp.h"
#include "enki/value.h"

static void load_callee(enki_interpreter* i, enki_law* law)
{
    i->law = law;
    i->byt_b = ENKI_LAW_BC(law);
    i->pc = i->byt_b;
    i->dep_q = law->arity_s;
    i->halted = false;
}

static void enki_enter_law_at(
    size_t arity_s,
    enki_value val_v,
    enki_interpreter* i,
    size_t fn_index_i)
{
    enki_law* law = (enki_law*)ENKI_TO_PTR(val_v);

    if (i->law != NULL) {
        enki_save_frame(i);
    }

    i->bp = fn_index_i;
    i->sp = i->bp + arity_s + 1;
    load_callee(i, law);
}

static void enki_run_until_restored(enki_interpreter* i, size_t fp_s, enki_law* law)
{
    while (!i->halted && (i->fp > fp_s || i->law != law)) {
        enki_step(i);
    }
}

static void enki_apply_saved_continuation(
    enki_interpreter* i,
    size_t cont_i,
    size_t result_i,
    bool was_halted)
{
    enki_value cont_v = i->stack_v[cont_i];
    enki_value result_v = i->stack_v[result_i];
    enki_cont* cont = (enki_cont*)ENKI_TO_PTR(cont_v);

    i->stack_v[cont_i] = result_v;
    for (size_t k = 0; k < cont->n_args_s; k++) {
        i->stack_v[cont_i + 1 + k] = cont->args_v[k];
    }
    i->sp = cont_i + 1 + cont->n_args_s;
    i->halted = was_halted;
    enki_apply(i, cont->n_args_s);
}

static void enki_make_partial_apply(enki_interpreter* i, size_t fn_index_i, enki_value fn_v,
    const enki_value* old_args_v, size_t n_old_args_s, size_t n_new_args_s)
{
    enki_value app = enki_alloc_app(i->gc, fn_v, n_old_args_s + n_new_args_s);
    enki_app* ptr = (enki_app*)ENKI_TO_PTR(app);
    if (n_old_args_s > 0 && old_args_v != NULL) {
        memcpy(ptr->args_v, old_args_v, sizeof(enki_value) * n_old_args_s);
    }
    for (size_t k = 0; k < n_new_args_s; k++) {
        ptr->args_v[k + n_old_args_s] = i->stack_v[fn_index_i + 1 + k];
    }
    i->stack_v[fn_index_i] = app;
    i->sp = fn_index_i + 1;
}

static void enki_make_neutral_app(enki_interpreter* i, size_t fn_index_i, size_t n_args_s)
{
    enki_value fn_v = i->stack_v[fn_index_i];
    enki_value app = enki_alloc_row(i->gc, fn_v, n_args_s, &i->stack_v[fn_index_i + 1]);
    i->stack_v[fn_index_i] = app;
    i->sp = fn_index_i + 1;
}

static void enki_complete_app(size_t arity_s, size_t n_args_s, size_t fn_index_i,
    enki_app* app, enki_interpreter* i)
{
    size_t old_s = app->n_args_s;
    size_t off_o = fn_index_i + 1;
    for (size_t k = n_args_s; k > 0; k--) {
        size_t idx_i = k - 1;
        i->stack_v[off_o + old_s + idx_i] = i->stack_v[off_o + idx_i];
    }
    for (size_t k = 0; k < old_s; k++) {
        i->stack_v[off_o + k] = app->args_v[k];
    }
    i->stack_v[fn_index_i] = app->fn_v;
    i->sp = fn_index_i + old_s + n_args_s + 1;
    enki_enter_law(arity_s, app->fn_v, i);
}

void enki_enter_law(size_t arity_s, enki_value val_v, enki_interpreter* i)
{
    size_t fn_index_i = i->sp - (arity_s + 1);
    enki_enter_law_at(arity_s, val_v, i, fn_index_i);
}

static void enki_overapply_law(
    enki_interpreter* i,
    size_t fn_index_i,
    enki_value law_v,
    size_t arity_s,
    size_t n_args_s)
{
    size_t extra_s = n_args_s - arity_s;
    enki_value cont_v =
        enki_alloc_cont(i->gc, extra_s, &i->stack_v[fn_index_i + 1 + arity_s]);
    law_v = i->stack_v[fn_index_i];

    for (size_t pos = fn_index_i + arity_s + 1; pos > fn_index_i; pos--) {
        i->stack_v[pos] = i->stack_v[pos - 1];
    }
    i->stack_v[fn_index_i] = cont_v;
    i->sp = fn_index_i + arity_s + 2;

    size_t base_fp_s = i->fp;
    enki_law* base_law = i->law;
    bool was_halted = i->halted;

    enki_enter_law_at(arity_s, law_v, i, fn_index_i + 1);
    enki_run_until_restored(i, base_fp_s, base_law);
    enki_apply_saved_continuation(i, fn_index_i, fn_index_i + 1, was_halted);
}

static void enki_overapply_app(
    enki_interpreter* i,
    size_t fn_index_i,
    enki_app* app,
    size_t arity_s,
    size_t n_args_s)
{
    size_t needed_s = arity_s - app->n_args_s;
    size_t extra_s = n_args_s - needed_s;
    enki_value cont_v =
        enki_alloc_cont(i->gc, extra_s, &i->stack_v[fn_index_i + 1 + needed_s]);
    app = (enki_app*)ENKI_TO_PTR(i->stack_v[fn_index_i]);

    for (size_t k = needed_s; k > 0; k--) {
        i->stack_v[fn_index_i + 1 + app->n_args_s + k] = i->stack_v[fn_index_i + k];
    }
    for (size_t k = 0; k < app->n_args_s; k++) {
        i->stack_v[fn_index_i + 2 + k] = app->args_v[k];
    }
    i->stack_v[fn_index_i + 1] = app->fn_v;
    i->stack_v[fn_index_i] = cont_v;
    i->sp = fn_index_i + arity_s + 2;

    size_t base_fp_s = i->fp;
    enki_law* base_law = i->law;
    bool was_halted = i->halted;

    enki_enter_law_at(arity_s, app->fn_v, i, fn_index_i + 1);
    enki_run_until_restored(i, base_fp_s, base_law);
    enki_apply_saved_continuation(i, fn_index_i, fn_index_i + 1, was_halted);
}

void enki_apply(enki_interpreter* i, size_t n_args_s)
{
    if (i->sp < n_args_s + 1) {
        return;
    }

    size_t fn_index_i = i->sp - (n_args_s + 1);
    enki_value head_v = i->stack_v[fn_index_i];
    if (!IS_PTR(head_v)) {
        enki_make_neutral_app(i, fn_index_i, n_args_s);
        return;
    }

    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(head_v);
    switch (h->kind_b) {
        case ENKI_LAW: {
            enki_law* law = (enki_law*)ENKI_TO_PTR(head_v);
            if (law->arity_s == n_args_s) {
                enki_enter_law(n_args_s, head_v, i);
            } else if (law->arity_s > n_args_s) {
                enki_make_partial_apply(i, fn_index_i, head_v, NULL, 0, n_args_s);
            } else {
                enki_overapply_law(i, fn_index_i, head_v, law->arity_s, n_args_s);
            }
            return;
        }
        case ENKI_APP: {
            enki_app* app = (enki_app*)ENKI_TO_PTR(head_v);
            size_t arity_s = enki_arity(app->fn_v);
            size_t new_arg_s = app->n_args_s + n_args_s;
            if (arity_s > 0 && new_arg_s == arity_s) {
                enki_complete_app(arity_s, n_args_s, fn_index_i, app, i);
            } else if (arity_s > 0 && new_arg_s < arity_s) {
                enki_make_partial_apply(
                    i, fn_index_i, app->fn_v, app->args_v, app->n_args_s, n_args_s);
            } else if (arity_s > 0 && app->n_args_s < arity_s) {
                enki_overapply_app(i, fn_index_i, app, arity_s, n_args_s);
            } else {
                enki_value app_v =
                    enki_app_weld(i->gc, app, n_args_s, &i->stack_v[fn_index_i + 1]);
                i->stack_v[fn_index_i] = app_v;
                i->sp = fn_index_i + 1;
            }
            return;
        }
        default:
            enki_make_neutral_app(i, fn_index_i, n_args_s);
            return;
    }
}
