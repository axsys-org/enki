#include <criterion/criterion.h>
#include <enki/interp.h>
#include <enki/wisp.h>
#include <stdlib.h>
#include <string.h>

static wisp_rt* rt;

static void setup(void)
{
    rt = wisp_rt_alloc(enki_allocator_system());
}

static void teardown(void)
{
    wisp_rt_free(enki_allocator_system(), rt);
}

TestSuite(wisp, .init = setup, .fini = teardown);

static enki_value small_strnat(const char* str_c)
{
    size_t str_s = strlen(str_c);
    cr_assert_lt(str_s, 8);
    return enki_alloc_strnat(rt->gc, (char*)str_c, str_s);
}

static void assert_small_strnat(enki_value value_v, const char* expected_c)
{
    cr_assert_eq(value_v, small_strnat(expected_c));
}

static void assert_strnat_bytes(enki_value value_v, const char* expected_c)
{
    size_t expected_s = strlen(expected_c);

    if (expected_s < 8) {
        assert_small_strnat(value_v, expected_c);
        return;
    }

    cr_assert(IS_PTR(value_v));
    obj_header* header = ENKI_TO_PTR(value_v);
    cr_assert_eq(header->kind_b, ENKI_BIG_NAT);

    enki_nat* nat = (enki_nat*)header;
    size_t expected_limbs_s = (expected_s + sizeof(mp_limb_t) - 1u) / sizeof(mp_limb_t);
    cr_assert_eq(nat->n_limbs_s, expected_limbs_s);
    cr_assert_eq(memcmp(nat->limbs, expected_c, expected_s), 0);
}

static enki_app* assert_app(enki_value value_v, enki_value fn_v, size_t n_args_s)
{
    cr_assert(IS_PTR(value_v));
    obj_header* header = ENKI_TO_PTR(value_v);
    cr_assert_eq(header->kind_b, ENKI_APP);

    enki_app* app = (enki_app*)header;
    cr_assert_eq(app->fn_v, fn_v);
    cr_assert_eq(app->n_args_s, n_args_s);
    return app;
}

static enki_app* assert_row(enki_value value_v, size_t n_args_s)
{
    return assert_app(value_v, 0, n_args_s);
}

static enki_app* assert_quote(enki_value value_v, const char* expected_c)
{
    enki_app* quote = assert_app(value_v, 1, 1);
    assert_strnat_bytes(quote->args_v[0], expected_c);
    return quote;
}

static enki_value parse_input(char* input_c, char** rest_c)
{
    char* cur_c = input_c;
    enki_value value_v = wisp_parse(rt, &cur_c);

    if (rest_c) {
        *rest_c = cur_c;
    }
    return value_v;
}

static enki_value eval_input(char* input_c)
{
    char* cur_c = input_c;
    enki_value value_v = wisp_parse(rt, &cur_c);
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
    enki_value law_v = parse_input("\"law\" tail", &rest_c);
    assert_quote(law_v, "law");
    cr_assert_str_eq(rest_c, " tail");

    enki_value ind_v = parse_input("\"testing indirect nat parsing\"", &rest_c);
    assert_quote(ind_v, "testing indirect nat parsing");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_symbol_stops_at_delimiter)
{
    char* rest_c = NULL;
    enki_value value_v = parse_input("foo bar", &rest_c);

    assert_small_strnat(value_v, "foo");
    cr_assert_str_eq(rest_c, " bar");
}

Test(wisp, parse_parenthesized_row)
{
    char* rest_c = NULL;
    enki_app* row = assert_row(parse_input("(add x y)", &rest_c), 3);

    assert_small_strnat(row->args_v[0], "add");
    assert_small_strnat(row->args_v[1], "x");
    assert_small_strnat(row->args_v[2], "y");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_nested_form)
{
    char* rest_c = NULL;
    enki_app* outer = assert_row(parse_input("((#pin \"B\") (\"Up\" i v r))", &rest_c), 2);

    enki_app* pin_row = assert_row(outer->args_v[0], 2);
    assert_small_strnat(pin_row->args_v[0], "#pin");
    assert_quote(pin_row->args_v[1], "B");

    enki_app* up_row = assert_row(outer->args_v[1], 4);
    assert_quote(up_row->args_v[0], "Up");
    assert_small_strnat(up_row->args_v[1], "i");
    assert_small_strnat(up_row->args_v[2], "v");
    assert_small_strnat(up_row->args_v[3], "r");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_brackets_and_braces)
{
    char* rest_c = NULL;
    enki_app* bracket = assert_row(parse_input("[a b]", &rest_c), 3);
    assert_small_strnat(bracket->args_v[0], "BRAK");
    assert_small_strnat(bracket->args_v[1], "a");
    assert_small_strnat(bracket->args_v[2], "b");
    cr_assert_str_eq(rest_c, "");

    enki_app* brace = assert_row(parse_input("{x}", &rest_c), 2);
    assert_small_strnat(brace->args_v[0], "CURL");
    assert_small_strnat(brace->args_v[1], "x");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_empty_containers)
{
    char* rest_c = NULL;
    cr_assert_eq(parse_input("()", &rest_c), 0);
    cr_assert_str_eq(rest_c, "");

    enki_app* empty_bracket = assert_row(parse_input("[]", &rest_c), 1);
    assert_small_strnat(empty_bracket->args_v[0], "BRAK");
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_juxtaposition)
{
    char* rest_c = NULL;
    enki_app* juxt = assert_app(parse_input("foo(bar baz)", &rest_c), 0, 3);

    assert_small_strnat(juxt->args_v[0], "JUXT");
    assert_small_strnat(juxt->args_v[1], "foo");

    enki_app* args = assert_row(juxt->args_v[2], 2);
    assert_small_strnat(args->args_v[0], "bar");
    assert_small_strnat(args->args_v[1], "baz");
    cr_assert_str_eq(rest_c, "");

    enki_app* empty_juxt = assert_app(parse_input("foo()", &rest_c), 0, 3);
    assert_small_strnat(empty_juxt->args_v[0], "JUXT");
    assert_small_strnat(empty_juxt->args_v[1], "foo");
    cr_assert_eq(empty_juxt->args_v[2], 0);
    cr_assert_str_eq(rest_c, "");
}

Test(wisp, parse_comments_and_whitespace)
{
    char* rest_c = NULL;
    enki_app* row = assert_row(parse_input(" ; before\n(foo ; inside\n bar)", &rest_c), 2);

    assert_small_strnat(row->args_v[0], "foo");
    assert_small_strnat(row->args_v[1], "bar");
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
    enki_value law_v = eval_input("(#bind plus (#law \"plus\" (plus x y) (add x y)))");
    cr_assert(IS_PTR(law_v));
    cr_assert_eq(((obj_header*)ENKI_TO_PTR(law_v))->kind_b, ENKI_LAW);

    cr_assert_eq(eval_input("(#app plus 20 22)"), 42);
}

Test(wisp, law_self_is_pick_zero)
{
    enki_value law_v = eval_input("(#bind selfer (#law \"selfer\" (selfer x) selfer))");
    cr_assert(IS_PTR(law_v));
    cr_assert_eq(((obj_header*)ENKI_TO_PTR(law_v))->kind_b, ENKI_LAW);

    enki_law* law = (enki_law*)ENKI_TO_PTR(law_v);
    uint8_t* bc_b = ENKI_LAW_BC(law);
    cr_assert_eq(bc_b[0], OP_PICK);
    cr_assert_eq(bc_b[1], 0);

    cr_assert_eq(eval_input("(#app selfer 99)"), law_v);
}
