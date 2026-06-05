#include "enki/allocator.h"
#include "test_interp.h"
#include "enki/app.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <inttypes.h>

static enki_interpreter* fixture_interp;

static void setup(void) {
  fixture_interp = enki_test_interp_create(1024 * 1024, 0);
  cr_assert_not_null(fixture_interp);
}

static void teardown(void) {
  enki_test_interp_destroy(fixture_interp);
  fixture_interp = NULL;
}

static enki_value app3(enki_value fn_v, enki_value a, enki_value b,
                       enki_value c) {
  enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 3);
  enki_app* app = ENKI_AS(enki_app, value_v);
  app->args_v[0] = a;
  app->args_v[1] = b;
  app->args_v[2] = c;
  return value_v;
}

TestSuite(op66_rows, .init = setup, .fini = teardown);

Test(op66_rows, hd_last_init_and_ix_read_app_shape) {
  enki_value row_v = app3(99, 10, 20, 30);

  fixture_interp->stack_v[0] = row_v;
  fixture_interp->sp = 1;
  op66_hd(fixture_interp);
  cr_assert_eq(fixture_interp->stack_v[0], 99);

  fixture_interp->stack_v[0] = row_v;
  op66_last(fixture_interp);
  cr_assert_eq(fixture_interp->stack_v[0], 30);

  fixture_interp->stack_v[0] = row_v;
  op66_ix1(fixture_interp);
  cr_assert_eq(fixture_interp->stack_v[0], 20);

  fixture_interp->stack_v[0] = row_v;
  op66_init(fixture_interp);
  cr_assert(IS_PTR(fixture_interp->stack_v[0]));
  enki_app* init = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
  cr_assert_eq(init->fn_v, 99);
  cr_assert_eq(init->n_args_s, 2);
  cr_assert_eq(init->args_v[0], 10);
  cr_assert_eq(init->args_v[1], 20);
}

Test(op66_rows, slice_weld_and_up_allocate_expected_rows) {
  enki_value row_v = app3(0, 10, 20, 30);

  fixture_interp->stack_v[0] = 1;
  fixture_interp->stack_v[1] = 2;
  fixture_interp->stack_v[2] = row_v;
  fixture_interp->sp = 3;
  op66_slice(fixture_interp);

  enki_value slice_v = fixture_interp->stack_v[0];
  cr_assert(IS_PTR(slice_v));
  enki_app* slice_app = ENKI_AS(enki_app, slice_v);
  cr_assert_eq(slice_app->n_args_s, 2);
  cr_assert_eq(slice_app->args_v[0], 20);
  cr_assert_eq(slice_app->args_v[1], 30);

  fixture_interp->stack_v[0] = row_v;
  fixture_interp->stack_v[1] = slice_v;
  fixture_interp->sp = 2;
  op66_weld(fixture_interp);

  enki_value welded_v = fixture_interp->stack_v[0];
  cr_assert(IS_PTR(welded_v));
  enki_app* welded_app = ENKI_AS(enki_app, welded_v);
  cr_assert_eq(welded_app->n_args_s, 5);
  cr_assert_eq(welded_app->args_v[0], 10);
  cr_assert_eq(welded_app->args_v[3], 20);

  enki_value row_for_up = app3(0, 10, 20, 30);
  enki_app* row_for_up_app = ENKI_AS(enki_app, row_for_up);
  cr_assert_eq(row_for_up_app->args_v[2], 30);

  fixture_interp->stack_v[0] = 1;
  fixture_interp->stack_v[1] = 77;
  fixture_interp->stack_v[2] = row_for_up;
  fixture_interp->sp = 3;
  cr_assert_eq((ENKI_AS(enki_app, fixture_interp->stack_v[2]))->args_v[2], 30);
  op66_up(fixture_interp);

  enki_app* updated_v = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
  cr_assert_eq(updated_v->n_args_s, 3);
  cr_assert_eq(updated_v->args_v[0], 10);
  cr_assert_eq(updated_v->args_v[1], 77);
  cr_assert_eq(updated_v->args_v[2], 30, "actual args_v[2]=%" PRIu64,
               updated_v->args_v[2]);
}

Test(op66_rows, case_and_boolean_ops_choose_expected_values) {
  enki_value row_v = app3(0, 11, 22, 33);

  fixture_interp->stack_v[0] = 2;
  fixture_interp->stack_v[1] = row_v;
  fixture_interp->stack_v[2] = 99;
  fixture_interp->sp = 3;
  op66_case(fixture_interp);
  cr_assert_eq(fixture_interp->stack_v[0], 33);

  fixture_interp->stack_v[0] = 0;
  fixture_interp->sp = 1;
  op66_nil(fixture_interp);
  cr_assert_eq(fixture_interp->stack_v[0], 1);

  fixture_interp->stack_v[0] = 0;
  fixture_interp->stack_v[1] = 55;
  fixture_interp->sp = 2;
  op66_or(fixture_interp);
  cr_assert_eq(fixture_interp->stack_v[0], 55);

  fixture_interp->stack_v[0] = 1;
  fixture_interp->stack_v[1] = 44;
  fixture_interp->stack_v[2] = 55;
  fixture_interp->sp = 3;
  op66_if(fixture_interp);
  cr_assert_eq(fixture_interp->stack_v[0], 44);
}
