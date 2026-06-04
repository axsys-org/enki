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

static void assert_eval_fails(char* input_c, const char* expected_prefix_c)
{
    char* cur_c = input_c;
    rt->err_f = 1;

    if (setjmp(rt->errjmp) == 0) {
        er_val value_v = wisp_parse(rt, &cur_c);
        (void)wisp_eval(rt, value_v);
        rt->err_f = 0;
        cr_assert_fail("expected eval failure");
    }

    rt->err_f = 0;
    cr_assert_not_null(rt->msg_c);
    cr_assert(strncmp(rt->msg_c, expected_prefix_c, strlen(expected_prefix_c)) == 0,
              "expected prefix <%s>, got <%s>", expected_prefix_c, rt->msg_c);
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
    assert_small_strnat(bracket->arg_v[0], "#brak");
    assert_small_strnat(bracket->arg_v[1], "a");
    assert_small_strnat(bracket->arg_v[2], "b");
    cr_assert_str_eq(rest_c, "");

    er_app* brace = assert_row(parse_input("{x}", &rest_c), 2);
    assert_small_strnat(brace->arg_v[0], "#curl");
    assert_small_strnat(brace->arg_v[1], "x");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_empty_containers)
{
    char* rest_c = NULL;
    cr_assert_eq(parse_input("()", &rest_c), 0);
    cr_assert_str_eq(rest_c, "");

    er_app* empty_bracket = assert_row(parse_input("[]", &rest_c), 1);
    assert_small_strnat(empty_bracket->arg_v[0], "#brak");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_juxtaposition)
{
    char* rest_c = NULL;
    er_app* juxt = assert_app(parse_input("foo(bar baz)", &rest_c), 0, 3);

    assert_small_strnat(juxt->arg_v[0], "#juxt");
    assert_small_strnat(juxt->arg_v[1], "foo");

    er_app* args = assert_row(juxt->arg_v[2], 2);
    assert_small_strnat(args->arg_v[0], "bar");
    assert_small_strnat(args->arg_v[1], "baz");
    cr_assert_str_eq(rest_c, "");

    er_app* empty_juxt = assert_app(parse_input("foo()", &rest_c), 0, 3);
    assert_small_strnat(empty_juxt->arg_v[0], "#juxt");
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
    assert_parse_fails("(", "eof in list");
    assert_parse_fails(")", "unexpected closing delimiter");
    assert_parse_fails("\"abc", "unterminated string");
    assert_parse_fails("(a b", "eof in list");
}

Test(wisp, parser_accepts_plan_symbol_characters_and_any_closer)
{
    char* rest_c = NULL;
    assert_small_strnat(parse_input("@module tail", &rest_c), "@module");
    cr_assert_str_eq(rest_c, " tail");

    assert_small_strnat(parse_input(": tail", &rest_c), ":");
    cr_assert_str_eq(rest_c, " tail");

    er_app* row = assert_row(parse_input("(a]", &rest_c), 1);
    assert_small_strnat(row->arg_v[0], "a");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, bind_law_and_apply_it)
{
    assert_small_strnat(eval_input("(#bind add 5)"), "add");
    cr_assert_eq(eval_input("add"), 5);

    assert_small_strnat(
        eval_input("(#bind add (#law \"add\" (add x y) ((#pin \"B\") (\"Add\" x y))))"),
        "add");
    er_val law_v = eval_input("add");
    cr_assert_not_null(assert_law(law_v));
    cr_assert_eq(eval_input("(add 20 22)"), 42);
}

Test(wisp, law_body_hash_juxtaposition_splices_raw_expression)
{
    er_law* raw = assert_law(eval_input("(#law \"raw\" (raw x) #(42))"));
    cr_assert_eq(raw->body_v, 42);

    assert_small_strnat(eval_input("(#bind id (#pin (#law \"id\" (id x) x)))"), "id");
    cr_assert_eq(eval_input("((#law \"caller\" (caller x) (#(id) x)) 42)"), 42);
}

Test(wisp, law_body_plain_hash_list_is_not_compile_time_eval)
{
    assert_eval_fails("(#law \"plain\" (plain x) (# 42))", "unbound");
}

Test(wisp, numeric_pin66_wrapper_compiles_pessimistically)
{
    assert_small_strnat(eval_input("(#bind add (#law \"add\" (add x y) "
                                   "((#pin 66) (\"Add\" x y))))"),
                        "add");
    cr_assert_not_null(assert_law(eval_input("add")));
    er_law* law = assert_law(eval_input("(#law \"caller\" (caller ignored) (add 20 22))"));
    const er_op* code = er_law_label_code_const(law, 0);
    cr_assert_not_null(code);
    cr_assert_gt(law->bc_v[0].op_s, 0);

    size_t apply_s = 0;
    for (size_t k = 0; k < law->bc_v[0].op_s; k++) {
        switch (code[k].tag) {
        case OP_PUSH_VAR:
        case OP_PUSH_LIT:
        case OP_APPLY_UNK:
        case OP_RET:
            break;
        default:
            cr_assert_fail("compiler emitted opcode %u at op %zu", (unsigned int)code[k].tag,
                           k);
        }
        if (code[k].tag == OP_APPLY_UNK) {
            cr_assert_eq(code[k].as.u32, 2);
            apply_s++;
        }
    }
    cr_assert_gt(apply_s, 0);
    cr_assert_eq(code[law->bc_v[0].op_s - 1].tag, OP_RET);
    cr_assert_eq(eval_input("((#law \"caller\" (caller ignored) (add 20 22)) 0)"), 42);
}

Test(wisp, macro_export_and_system_shadowing_match_plan_assembler)
{
    assert_small_strnat(eval_input("(#macro zap (#law \"zap\" (zap env form) 0))"), "zap");
    cr_assert_eq(eval_input("(zap anything)"), 0);

    assert_small_strnat(eval_input("(#bind a 1)"), "a");
    assert_small_strnat(eval_input("(#bind b 2)"), "b");
    cr_assert_eq(eval_input("(#export a)"), 0);
    cr_assert_eq(eval_input("a"), 1);
    assert_eval_fails("b", "unbound thk");
}

Test(wisp, user_macro_results_are_forced_to_nf_before_syntax_interpretation)
{
    assert_strnat_bytes(eval_input("(#macro make-law (#law \"make-law\" (make-law env form) "
                                   "sig((0 \"made\" \"x\")) "
                                   "(0 \"#law\" (1 \"made\") sig \"x\")))"),
                        "make-law");

    er_law* law = assert_law(eval_input("(make-law)"));
    cr_assert_eq(law->ari_d, 1);
    assert_strnat_bytes(law->name_v, "made");
}

Test(wisp, app_evaluates_raw_operands_in_order)
{
    assert_strnat_bytes(eval_input("(#bind choose (#law \"choose\" (choose x y) x))"), "choose");
    assert_strnat_bytes(eval_input("(#bind current 1)"), "current");

    cr_assert_eq(eval_input("(#app choose current (#bind current 2))"), 1);
    cr_assert_eq(eval_input("current"), 2);
}

Test(wisp, bind_and_macro_validate_key_before_rhs_eval)
{
    assert_eval_fails("(#bind (0) (#bind bindleak 1))", "bad env key");
    assert_eval_fails("bindleak", "unbound thk");

    assert_eval_fails("(#macro (0) (#bind macroleak 2))", "bad env key");
    assert_eval_fails("macroleak", "unbound thk");
}

Test(wisp, law_macroexpands_all_binds_and_body_before_compiling_binds)
{
    assert_strnat_bytes(
        eval_input("(#macro install (#law \"install\" (install env form) "
                   "(0 \"#macro\" \"late\" "
                   "(0 \"#law\" (1 \"late\") (0 \"late\" \"env\" \"form\") (1 99)))))"),
        "install");

    er_val value_v = eval_input("((#law \"outer\" (outer install) "
                                "tmp(#((install))) "
                                "(late)) 0)");
    cr_assert_not_null(assert_law(value_v));
}

Test(wisp, law_self_reference_is_usable_as_value)
{
    assert_strnat_bytes(eval_input("(#bind self (#law \"self\" (self x) self))"), "self");
    er_val self_v = eval_input("self");
    cr_assert_eq(eval_input("(self 0)"), self_v);
}

Test(wisp, long_nat_symbols_are_environment_and_local_keys)
{
    assert_strnat_bytes(eval_input("(#bind abcdefgh 9)"), "abcdefgh");
    cr_assert_eq(eval_input("abcdefgh"), 9);

    assert_strnat_bytes(
        eval_input("(#bind abcdefgh (#law \"abcdefgh\" (abcdefgh longargx) longargx))"),
        "abcdefgh");
    cr_assert_eq(eval_input("(abcdefgh 55)"), 55);

    assert_strnat_bytes(eval_input("(#bind abcdefgi 10)"), "abcdefgi");
    cr_assert_eq(eval_input("(#export abcdefgh)"), 0);
    cr_assert_not_null(assert_law(eval_input("abcdefgh")));
    assert_eval_fails("abcdefgi", "unbound thk");
}

Test(wisp, env_values_shadow_system_macros)
{
    assert_small_strnat(eval_input("(#bind #pin 7)"), "#pin");
    er_app* app = assert_app(eval_input("(#pin 1)"), 7, 1);
    cr_assert_eq(app->arg_v[0], 1);
}

Test(wisp, recursive_program_validates_extended_snippet)
{
    assert_small_strnat(eval_input("(#bind inc (#law \"inc\" (inc x) "
                                   "((#pin \"B\") (\"Inc\" x))))"),
                        "inc");
    cr_assert_gt(assert_law(eval_input("inc"))->bc_s, 0);
    assert_small_strnat(eval_input("(#bind if (#law \"if\" (if cond then else) "
                                   "((#pin \"B\") (\"If\" cond then else))))"),
                        "if");
    cr_assert_gt(assert_law(eval_input("if"))->bc_s, 0);
    assert_small_strnat(eval_input("(#bind eq (#law \"eq\" (eq a b) "
                                   "((#pin \"B\") (\"Eq\" a b))))"),
                        "eq");
    cr_assert_gt(assert_law(eval_input("eq"))->bc_s, 0);
    assert_small_strnat(eval_input("(#bind dechelp (#law \"dechelp\" (dechelp count x) "
                                   "(if (eq (inc count) x) count "
                                   "(dechelp (inc count) x))))"),
                        "dechelp");
    cr_assert_gt(assert_law(eval_input("dechelp"))->bc_s, 0);
    assert_small_strnat(eval_input("(#bind dec (#law \"dec\" (dec x) (dechelp 0 x)))"),
                        "dec");
    cr_assert_gt(assert_law(eval_input("dec"))->bc_s, 0);
    assert_small_strnat(eval_input("(#bind add (#law \"add\" (add a b) "
                                   "(if (eq 0 a) b (add (dec a) (inc b)))))"),
                        "add");
    cr_assert_gt(assert_law(eval_input("add"))->bc_s, 0);
    assert_small_strnat(eval_input("(#bind fib (#law \"fib\" (fib n) "
                                   "(if (eq n 1) 1 (if (eq n 2) 1 "
                                   "(add (fib (dec n)) (fib (dec (dec n))))))))"),
                        "fib");
    cr_assert_gt(assert_law(eval_input("fib"))->bc_s, 0);

    cr_assert_eq(eval_input("(inc 41)"), 42);
    cr_assert_eq(eval_input("(dec 5)"), 4);
    cr_assert_eq(eval_input("(add 5 8)"), 13);
    cr_assert_eq(eval_input("(fib 5)"), 5);
}
