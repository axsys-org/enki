#include <criterion/criterion.h>
#include <enki/run.h>
#include <enki/wisp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wisp_rt* rt;

static void setup(void)
{
    rt = wisp_rt_alloc(&sys_a);
}

static void teardown(void)
{
    wisp_rt_free(&sys_a, rt);
}

TestSuite(wisp, .init = setup, .fini = teardown);

static er_val small_strnat(const char* str_c)
{
    size_t str_s = strlen(str_c);
    cr_assert_lt(str_s, 8);
    er_val out_v = 0;
    for (size_t i = 0; i < str_s; i++) {
        out_v |= ((er_val)(uint8_t)str_c[i]) << (i * 8u);
    }
    return out_v;
}

static void assert_small_strnat(er_val value_v, const char* expected_c)
{
    cr_assert_eq(value_v, small_strnat(expected_c));
}

static void assert_strnat_bytes(er_val value_v, const char* expected_c)
{
    size_t expected_s = strlen(expected_c);

    if (expected_s < 8) {
        assert_small_strnat(value_v, expected_c);
        return;
    }

    er_bat* bat = er_outt(er_tag_bat, value_v);
    cr_assert_not_null(bat);

    size_t expected_limbs_s = (expected_s + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
    cr_assert_eq(bat->lim_s, expected_limbs_s);
    cr_assert_eq(memcmp(bat->lim_q, expected_c, expected_s), 0);
}

static er_app* assert_app(er_val value_v, er_val fn_v, size_t arg_s)
{
    er_app* app = er_outt(er_tag_app, value_v);
    cr_assert_not_null(app);
    cr_assert_eq(app->fn_v, fn_v);
    cr_assert_eq(app->arg_s, arg_s);
    return app;
}

static er_app* assert_row(er_val value_v, size_t arg_s)
{
    return assert_app(value_v, 0, arg_s);
}

static er_law* assert_law(er_val value_v)
{
    er_law* law = er_outt(er_tag_law, value_v);
    cr_assert_not_null(law);
    return law;
}

static er_app* assert_quote(er_val value_v, const char* expected_c)
{
    er_app* quote = assert_app(value_v, 1, 1);
    assert_strnat_bytes(quote->arg_v[0], expected_c);
    return quote;
}

static er_val parse_input(char* input_c, char** rest_c)
{
    char* cur_c = input_c;
    er_val value_v = wisp_parse(rt, &cur_c);

    if (rest_c) {
        *rest_c = cur_c;
    }
    return value_v;
}

static er_val eval_input(char* input_c)
{
    char* cur_c = input_c;
    er_val value_v = wisp_parse(rt, &cur_c);
    return wisp_eval(rt, value_v);
}

static void assert_parse_fails(char* input_c, const char* expected_msg_c)
{
    char* cur_c = input_c;
    rt->err_f = 1;

    if (setjmp(rt->errjmp) == 0) {
        (void)wisp_parse(rt, &cur_c);
        rt->err_f = 0;
        cr_assert_fail("expected parse failure");
    }

    rt->err_f = 0;
    cr_assert_not_null(rt->msg_c);
    cr_assert_str_eq(rt->msg_c, expected_msg_c);
    free(rt->msg_c);
    rt->msg_c = NULL;
}

Test(wisp, parse_str)
{
    char* rest_c = NULL;
    er_val law_v = parse_input("\"law\" tail", &rest_c);
    assert_quote(law_v, "law");
    cr_assert_str_eq(rest_c, " tail");

    er_val ind_v = parse_input("\"testing indirect nat parsing\"", &rest_c);
    assert_quote(ind_v, "testing indirect nat parsing");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_symbol_stops_at_delimiter)
{
    char* rest_c = NULL;
    er_val value_v = parse_input("foo bar", &rest_c);

    assert_small_strnat(value_v, "foo");
    cr_assert_str_eq(rest_c, " bar");
}

Test(wisp, parse_parenthesized_row)
{
    char* rest_c = NULL;
    er_app* row = assert_row(parse_input("(add x y)", &rest_c), 3);

    assert_small_strnat(row->arg_v[0], "add");
    assert_small_strnat(row->arg_v[1], "x");
    assert_small_strnat(row->arg_v[2], "y");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_nested_form)
{
    char* rest_c = NULL;
    er_app* outer = assert_row(parse_input("((#pin \"B\") (\"Up\" i v r))", &rest_c), 2);

    er_app* pin_row = assert_row(outer->arg_v[0], 2);
    assert_small_strnat(pin_row->arg_v[0], "#pin");
    assert_quote(pin_row->arg_v[1], "B");

    er_app* up_row = assert_row(outer->arg_v[1], 4);
    assert_quote(up_row->arg_v[0], "Up");
    assert_small_strnat(up_row->arg_v[1], "i");
    assert_small_strnat(up_row->arg_v[2], "v");
    assert_small_strnat(up_row->arg_v[3], "r");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_brackets_and_braces)
{
    char* rest_c = NULL;
    er_app* bracket = assert_row(parse_input("[a b]", &rest_c), 3);
    assert_small_strnat(bracket->arg_v[0], "BRAK");
    assert_small_strnat(bracket->arg_v[1], "a");
    assert_small_strnat(bracket->arg_v[2], "b");
    cr_assert_str_eq(rest_c, "");

    er_app* brace = assert_row(parse_input("{x}", &rest_c), 2);
    assert_small_strnat(brace->arg_v[0], "CURL");
    assert_small_strnat(brace->arg_v[1], "x");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_empty_containers)
{
    char* rest_c = NULL;
    cr_assert_eq(parse_input("()", &rest_c), 0);
    cr_assert_str_eq(rest_c, "");

    er_app* empty_bracket = assert_row(parse_input("[]", &rest_c), 1);
    assert_small_strnat(empty_bracket->arg_v[0], "BRAK");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_juxtaposition)
{
    char* rest_c = NULL;
    er_app* juxt = assert_app(parse_input("foo(bar baz)", &rest_c), 0, 3);

    assert_small_strnat(juxt->arg_v[0], "JUXT");
    assert_small_strnat(juxt->arg_v[1], "foo");

    er_app* args = assert_row(juxt->arg_v[2], 2);
    assert_small_strnat(args->arg_v[0], "bar");
    assert_small_strnat(args->arg_v[1], "baz");
    cr_assert_str_eq(rest_c, "");

    er_app* empty_juxt = assert_app(parse_input("foo()", &rest_c), 0, 3);
    assert_small_strnat(empty_juxt->arg_v[0], "JUXT");
    assert_small_strnat(empty_juxt->arg_v[1], "foo");
    cr_assert_eq(empty_juxt->arg_v[2], 0);
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_comments_and_whitespace)
{
    char* rest_c = NULL;
    er_app* row = assert_row(parse_input(" ; before\n(foo ; inside\n bar)", &rest_c), 2);

    assert_small_strnat(row->arg_v[0], "foo");
    assert_small_strnat(row->arg_v[1], "bar");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_errors)
{
    assert_parse_fails("(", "unclosed delimiter");
    assert_parse_fails("(a]", "mismatched closing delimiter");
    assert_parse_fails(")", "unexpected closing delimiter");
    assert_parse_fails("\"abc", "unclosed string");
    assert_parse_fails(":", "invalid character");
}

Test(wisp, bind_law_and_apply_it)
{
    cr_assert_eq(eval_input("(#bind add 5)"), 5);
    er_val law_v =
        eval_input("(#bind add (#law \"add\" (add x y) ((#pin \"B\") (\"Add\" x y))))");

    cr_assert_not_null(assert_law(law_v));
    cr_assert_eq(eval_input("(add 20 22)"), 42);
}

Test(wisp, recursive_program_validates_extended_snippet)
{
    cr_assert_gt(assert_law(eval_input("(#bind inc (#law \"inc\" (inc x) "
                                       "((#pin \"B\") (\"Inc\" x))))"))
                     ->bc_s,
                 0);
    cr_assert_gt(assert_law(eval_input("(#bind if (#law \"if\" (if cond then else) "
                                       "((#pin \"B\") (\"If\" cond then else))))"))
                     ->bc_s,
                 0);
    cr_assert_gt(assert_law(eval_input("(#bind eq (#law \"eq\" (eq a b) "
                                       "((#pin \"B\") (\"Eq\" a b))))"))
                     ->bc_s,
                 0);
    cr_assert_gt(assert_law(eval_input("(#bind dechelp (#law \"dechelp\" (dechelp count x) "
                                       "(if (eq (inc count) x) count "
                                       "(dechelp (inc count) x))))"))
                     ->bc_s,
                 0);
    cr_assert_gt(assert_law(eval_input("(#bind dec (#law \"dec\" (dec x) (dechelp 0 x)))"))
                     ->bc_s,
                 0);
    cr_assert_gt(assert_law(eval_input("(#bind add (#law \"add\" (add a b) "
                                       "(if (eq 0 a) b (add (dec a) (inc b)))))"))
                     ->bc_s,
                 0);
    cr_assert_gt(assert_law(eval_input("(#bind fib (#law \"fib\" (fib n) "
                                       "(if (eq n 1) 1 (if (eq n 2) 1 "
                                       "(add (fib (dec n)) (fib (dec (dec n))))))))"))
                     ->bc_s,
                 0);

    cr_assert_eq(eval_input("(inc 41)"), 42);
    cr_assert_eq(eval_input("(dec 5)"), 4);
    cr_assert_eq(eval_input("(add 5 8)"), 13);
    cr_assert_eq(eval_input("(fib 5)"), 5);
}
