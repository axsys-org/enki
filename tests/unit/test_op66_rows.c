#include "enki/allocator.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/value.h"

#include <criterion/criterion.h>

static enki_interpreter* fixture_interp;

static void setup(void)
{
    fixture_interp = enki_create_interp(enki_allocator_system(), 1024 * 1024, 0);
    cr_assert_not_null(fixture_interp);
}

static void teardown(void)
{
    enki_destroy(fixture_interp);
    fixture_interp = NULL;
}

static enki_value app3(enki_value fn, enki_value a, enki_value b, enki_value c)
{
    enki_value value = enki_alloc_app(fixture_interp->gc, fn, 3);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value);
    app->args[0] = a;
    app->args[1] = b;
    app->args[2] = c;
    return value;
}

TestSuite(op66_rows, .init = setup, .fini = teardown);

Test(op66_rows, hd_last_init_and_ix_read_app_shape)
{
    enki_value row = app3(99, 10, 20, 30);

    fixture_interp->stack[0] = row;
    fixture_interp->sp = 1;
    op66_hd(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 99);

    fixture_interp->stack[0] = row;
    op66_last(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 30);

    fixture_interp->stack[0] = row;
    op66_ix1(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 20);

    fixture_interp->stack[0] = row;
    op66_init(fixture_interp);
    cr_assert(IS_PTR(fixture_interp->stack[0]));
    enki_app* init = (enki_app*)ENKI_TO_PTR(fixture_interp->stack[0]);
    cr_assert_eq(init->fn, 99);
    cr_assert_eq(init->n_args, 2);
    cr_assert_eq(init->args[0], 10);
    cr_assert_eq(init->args[1], 20);
}

Test(op66_rows, slice_weld_and_up_allocate_expected_rows)
{
    enki_value row = app3(0, 10, 20, 30);

    fixture_interp->stack[0] = 1;
    fixture_interp->stack[1] = 2;
    fixture_interp->stack[2] = row;
    fixture_interp->sp = 3;
    op66_slice(fixture_interp);

    enki_value slice = fixture_interp->stack[0];
    cr_assert(IS_PTR(slice));
    enki_app* slice_app = (enki_app*)ENKI_TO_PTR(slice);
    cr_assert_eq(slice_app->n_args, 2);
    cr_assert_eq(slice_app->args[0], 20);
    cr_assert_eq(slice_app->args[1], 30);

    fixture_interp->stack[0] = row;
    fixture_interp->stack[1] = slice;
    fixture_interp->sp = 2;
    op66_weld(fixture_interp);

    enki_value welded = fixture_interp->stack[0];
    cr_assert(IS_PTR(welded));
    enki_app* welded_app = (enki_app*)ENKI_TO_PTR(welded);
    cr_assert_eq(welded_app->n_args, 5);
    cr_assert_eq(welded_app->args[0], 10);
    cr_assert_eq(welded_app->args[3], 20);

    enki_value row_for_up = app3(0, 10, 20, 30);
    enki_app* row_for_up_app = (enki_app*)ENKI_TO_PTR(row_for_up);
    cr_assert_eq(row_for_up_app->args[2], 30);

    fixture_interp->stack[0] = 1;
    fixture_interp->stack[1] = 77;
    fixture_interp->stack[2] = row_for_up;
    fixture_interp->sp = 3;
    cr_assert_eq(((enki_app*)ENKI_TO_PTR(fixture_interp->stack[2]))->args[2], 30);
    op66_up(fixture_interp);

    enki_app* updated = (enki_app*)ENKI_TO_PTR(fixture_interp->stack[0]);
    cr_assert_eq(updated->n_args, 3);
    cr_assert_eq(updated->args[0], 10);
    cr_assert_eq(updated->args[1], 77);
    cr_assert_eq(updated->args[2], 30, "actual args[2]=%llu", (unsigned long long)updated->args[2]);
}

Test(op66_rows, case_and_boolean_ops_choose_expected_values)
{
    enki_value row = app3(0, 11, 22, 33);

    fixture_interp->stack[0] = 2;
    fixture_interp->stack[1] = row;
    fixture_interp->stack[2] = 99;
    fixture_interp->sp = 3;
    op66_case(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 33);

    fixture_interp->stack[0] = 0;
    fixture_interp->sp = 1;
    op66_nil(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 1);

    fixture_interp->stack[0] = 0;
    fixture_interp->stack[1] = 55;
    fixture_interp->sp = 2;
    op66_or(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 55);

    fixture_interp->stack[0] = 1;
    fixture_interp->stack[1] = 44;
    fixture_interp->stack[2] = 55;
    fixture_interp->sp = 3;
    op66_if(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 44);
}
