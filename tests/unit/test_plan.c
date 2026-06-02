#include "enki/allocator.h"
#include "enki/app.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/pin.h"
#include "enki/plan.h"
#include "enki/value.h"
#include "test_interp.h"

#include <criterion/criterion.h>
#include <stdint.h>

static enki_interpreter* fixture_interp;
static enki_plan fixture_plan;

static void setup(void)
{
    fixture_interp = enki_test_interp_create(1024 * 1024, 0);
    cr_assert_not_null(fixture_interp);
    enki_plan_init(&fixture_plan, fixture_interp->gc);
}

static void teardown(void)
{
    enki_test_interp_destroy(fixture_interp);
    fixture_interp = NULL;
}

static enki_value pin_value(enki_value inner_v)
{
    uint8_t hash_b[32] = {0};
    return enki_pin_alloc(fixture_interp->gc, hash_b, inner_v, 0, NULL);
}

static enki_value pin_nat(enki_value inner_v)
{
    return pin_value(inner_v);
}

static enki_value app1(enki_value fn_v, enki_value a)
{
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 1);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->args_v[0] = a;
    return value_v;
}

static enki_value app2(enki_value fn_v, enki_value a, enki_value b)
{
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 2);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->args_v[0] = a;
    app->args_v[1] = b;
    return value_v;
}

static enki_value strnat(const char* str_c)
{
    return enki_alloc_cstrnat(fixture_interp->gc, (char*)str_c);
}

static enki_value eval_op66_arity(enki_value input_v)
{
    enki_value pin_b = pin_nat(66);
    enki_value row_v = app1(strnat("Arity"), input_v);
    enki_value out_v = 0;

    cr_assert_eq(enki_plan_apply(&fixture_plan, pin_b, 1, &row_v, &out_v), ENKI_ERROR_OK);
    return out_v;
}

TestSuite(plan, .init = setup, .fini = teardown);

Test(plan, pinned_op66_add_evaluates_row)
{
    enki_value pin_b = pin_nat(66);
    enki_value row_v = app2(strnat("Add"), 20, 22);
    enki_value out_v = 0;

    cr_assert_eq(enki_plan_apply(&fixture_plan, pin_b, 1, &row_v, &out_v), ENKI_ERROR_OK);

    cr_assert_eq(out_v, 42);
}

Test(plan, op66_arity_reports_only_raw_laws)
{
    enki_value raw_law_v = enki_law_alloc(fixture_interp->gc, 3, 0, 0, 0, 0, NULL, NULL);
    enki_value pinned_law_v = pin_value(raw_law_v);
    enki_value primitive_pin_v = pin_nat(66);
    enki_value partial_app_v = app1(raw_law_v, 99);

    cr_assert_eq(eval_op66_arity(raw_law_v), 3);
    cr_assert_eq(eval_op66_arity(pinned_law_v), 0);
    cr_assert_eq(eval_op66_arity(primitive_pin_v), 0);
    cr_assert_eq(eval_op66_arity(partial_app_v), 0);
    cr_assert_eq(eval_op66_arity(42), 0);
}

Test(plan, treewalks_law_body_with_vars_and_application)
{
    enki_value pin_b = pin_nat(66);
    enki_value add_v = strnat("Add");

    enki_value row_body_v = app2(0, app2(0, app1(0, add_v), 1), 2);
    enki_value body_v = app2(0, app1(0, pin_b), row_body_v);
    enki_value law_v = enki_law_alloc(fixture_interp->gc, 2, 0, body_v, 0, 0, NULL, NULL);
    enki_value args_v[] = {20, 22};
    enki_value out_v = 0;

    cr_assert_eq(enki_plan_apply(&fixture_plan, law_v, 2, args_v, &out_v), ENKI_ERROR_OK);

    cr_assert_eq(out_v, 42);
}

Test(plan, partial_app_spines_flatten_before_reduce)
{
    enki_value pin_b = pin_nat(66);
    enki_value add_v = strnat("Add");

    enki_value row_body_v = app2(0, app2(0, app1(0, add_v), 1), 2);
    enki_value body_v = app2(0, app1(0, pin_b), row_body_v);
    enki_value law_v = enki_law_alloc(fixture_interp->gc, 2, 0, body_v, 0, 0, NULL, NULL);
    enki_value partial_v = app1(law_v, 20);
    enki_value arg_v = 22;
    enki_value out_v = 0;

    cr_assert_eq(enki_plan_apply(&fixture_plan, partial_v, 1, &arg_v, &out_v), ENKI_ERROR_OK);

    cr_assert_eq(out_v, 42);
}

Test(plan, op0_pin_pins_whnf_value)
{
    enki_value pin_zero_v = pin_nat(0);
    enki_value row_v = app2(0, 0, 99);
    enki_value out_v = 0;

    cr_assert_eq(enki_plan_apply(&fixture_plan, pin_zero_v, 1, &row_v, &out_v), ENKI_ERROR_OK);

    cr_assert(IS_PTR(out_v));
    cr_assert_eq(ENKI_AS(enki_value_header, out_v)->kind_b, ENKI_PIN);
    cr_assert_eq(ENKI_AS(enki_pin, out_v)->inner_v, 99);
}

Test(plan, law_body_zero_resolves_to_self)
{
    enki_value law_v = enki_law_alloc(fixture_interp->gc, 1, 0, 0, 0, 0, NULL, NULL);
    enki_value arg_v = 99;
    enki_value out_v = 0;

    cr_assert_eq(enki_plan_apply(&fixture_plan, law_v, 1, &arg_v, &out_v), ENKI_ERROR_OK);

    cr_assert_eq(out_v, law_v);
}
