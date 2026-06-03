#include <enki/motes.h>
#include <enki/profile.h>
#include <enki/string_builder.h>
#include <enki/util.h>
#include <enki/wisp.h>

#include <ctype.h>
#include <gmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WISP_SMALL_MAX UINT64_C(0x7fffffffffffffff)

// MARK: - Runtime lifecycle
wisp_rt* wisp_rt_alloc(const enki_allocator* loc_a)
{
    wisp_rt* rt = ea_calloc(loc_a, wisp_rt, 1);
    if (rt == NULL) {
        return NULL;
    }
    rt->loc_a = loc_a;
    rt->err_f = 0;
    rt->msg_c = NULL;
    rt->env = NULL;
    return rt;
}

void wisp_rt_free(const enki_allocator* loc_a, wisp_rt* rt)
{
    if (rt == NULL) {
        return;
    }

    wisp_env_entry* ent = rt->env;
    while (ent != NULL) {
        wisp_env_entry* next = ent->next;
        ea_free(loc_a, ent);
        ent = next;
    }
    free(rt->msg_c);
    ea_free(loc_a, rt);
}

[[noreturn]] static void wisp_fail_fn(wisp_rt* rt, const char* msg, int line)
{
    if (rt->err_f) {
        free(rt->msg_c);
        rt->msg_c = malloc(strlen(msg) + 1);
        strcpy(rt->msg_c, msg);
        longjmp(rt->errjmp, 1);
    } else {
        fprintf(stderr, "wisp.c:%i fail: %s\r\n", line, msg);
        abort();
    }
}

#define wisp_fail(rt, msg) wisp_fail_fn(rt, msg, __LINE__)

static er_app* wisp_as_app(er_val val_v)
{
    return er_outt(er_tag_app, val_v);
}

static er_val wisp_app_make(wisp_rt* rt, er_val fn_v, size_t arg_s, const er_val* arg_v)
{
    er_app* app = er_app_alloc(rt->loc_a, arg_s);
    if (app == NULL) {
        wisp_fail(rt, "oom");
    }
    er_val out_v = er_app_init(app, fn_v, arg_s, arg_v);
    if (out_v == 0) {
        wisp_fail(rt, "oom");
    }
    return out_v;
}

static er_val wisp_app1(wisp_rt* rt, er_val fn_v, er_val a_v)
{
    er_val arg_v[] = {a_v};
    return wisp_app_make(rt, fn_v, 1, arg_v);
}

static er_val wisp_app2(wisp_rt* rt, er_val fn_v, er_val a_v, er_val b_v)
{
    er_val arg_v[] = {a_v, b_v};
    return wisp_app_make(rt, fn_v, 2, arg_v);
}

static er_val wisp_app3(wisp_rt* rt, er_val fn_v, er_val a_v, er_val b_v, er_val c_v)
{
    er_val arg_v[] = {a_v, b_v, c_v};
    return wisp_app_make(rt, fn_v, 3, arg_v);
}

static er_val _wisp_quote(wisp_rt* rt, er_val val_v)
{
    return wisp_app1(rt, 1, val_v);
}

static er_val wisp_law_quote(wisp_rt* rt, er_val val_v)
{
    return wisp_app1(rt, 0, val_v);
}

static void wisp_gmp_free(void* ptr, size_t size_s)
{
    void (*free_fn)(void*, size_t) = NULL;
    mp_get_memory_functions(NULL, NULL, &free_fn);
    free_fn(ptr, size_s);
}

static er_val wisp_mpz_to_nat(wisp_rt* rt, const mpz_t z)
{
    if (mpz_sgn(z) <= 0) {
        return 0;
    }

    size_t limb_s = 0;
    uint64_t* limb_q = (uint64_t*)mpz_export(NULL, &limb_s, -1, sizeof(uint64_t), 0, 0, z);
    if (limb_s == 0) {
        return 0;
    }
    if (limb_s == 1 && limb_q[0] <= WISP_SMALL_MAX) {
        er_val out_v = limb_q[0];
        wisp_gmp_free(limb_q, limb_s * sizeof(uint64_t));
        return out_v;
    }

    er_bat* bat = er_bat_alloc(rt->loc_a, limb_s);
    if (bat == NULL) {
        wisp_gmp_free(limb_q, limb_s * sizeof(uint64_t));
        wisp_fail(rt, "oom");
    }
    er_val out_v = er_bat_init(bat, limb_s, limb_q);
    wisp_gmp_free(limb_q, limb_s * sizeof(uint64_t));
    if (out_v == 0) {
        wisp_fail(rt, "oom");
    }
    return out_v;
}

static er_val wisp_bytes_nat(wisp_rt* rt, const char* bytes_c, size_t bytes_s)
{
    if (bytes_s < sizeof(uint64_t)) {
        uint64_t out_q = 0;
        for (size_t i = 0; i < bytes_s; i++) {
            out_q |= ((uint64_t)(uint8_t)bytes_c[i]) << (i * 8u);
        }
        return out_q;
    }

    size_t limb_s = (bytes_s + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
    uint64_t* limb_q = ea_calloc(rt->loc_a, uint64_t, limb_s);
    if (limb_q == NULL) {
        wisp_fail(rt, "oom");
    }
    memcpy(limb_q, bytes_c, bytes_s);

    er_bat* bat = er_bat_alloc(rt->loc_a, limb_s);
    if (bat == NULL) {
        ea_free(rt->loc_a, limb_q);
        wisp_fail(rt, "oom");
    }
    er_val out_v = er_bat_init(bat, limb_s, limb_q);
    ea_free(rt->loc_a, limb_q);
    if (out_v == 0) {
        wisp_fail(rt, "oom");
    }
    return out_v;
}

static bool wisp_is_nat(er_val val_v)
{
    return er_is_cat(val_v) || er_outt(er_tag_bat, val_v) != NULL;
}

static size_t wisp_nat_limb_s(er_val val_v)
{
    if (er_is_cat(val_v)) {
        return val_v == 0 ? 0 : 1;
    }

    er_bat* bat = er_outt(er_tag_bat, val_v);
    if (bat == NULL) {
        return SIZE_MAX;
    }

    size_t lim_s = bat->lim_s;
    while (lim_s > 0 && bat->lim_q[lim_s - 1u] == 0) {
        lim_s--;
    }
    return lim_s;
}

static uint64_t wisp_nat_limb(er_val val_v, size_t idx_s)
{
    if (er_is_cat(val_v)) {
        return idx_s == 0 ? val_v : 0;
    }

    er_bat* bat = er_outt(er_tag_bat, val_v);
    if (bat == NULL || idx_s >= bat->lim_s) {
        return 0;
    }
    return bat->lim_q[idx_s];
}

static int wisp_nat_cmp(er_val a_v, er_val b_v)
{
    size_t a_s = wisp_nat_limb_s(a_v);
    size_t b_s = wisp_nat_limb_s(b_v);
    if (a_s == SIZE_MAX || b_s == SIZE_MAX) {
        return a_v < b_v ? -1 : (a_v > b_v ? 1 : 0);
    }
    if (a_s != b_s) {
        return a_s < b_s ? -1 : 1;
    }
    for (size_t i = a_s; i > 0; i--) {
        uint64_t a_q = wisp_nat_limb(a_v, i - 1u);
        uint64_t b_q = wisp_nat_limb(b_v, i - 1u);
        if (a_q != b_q) {
            return a_q < b_q ? -1 : 1;
        }
    }
    return 0;
}

static bool wisp_nat_eq(er_val a_v, er_val b_v)
{
    return wisp_nat_cmp(a_v, b_v) == 0;
}

static void _wisp_fail_with_val(wisp_rt* rt, const char* msg_c, er_val val_v)
{
    char* key_c = wisp_print_value(rt, val_v, NULL);
    size_t str_s = strlen(msg_c) + strlen(key_c) + 5;
    char* str_c = ea_calloc(rt->loc_a, char, str_s);
    if (str_c == NULL) {
        wisp_fail(rt, msg_c);
    }
    snprintf(str_c, str_s, "%s: %s\n", msg_c, key_c);
    wisp_fail(rt, str_c);
}

static void _wisp_fail_with_result(wisp_rt* rt, er_val result_v, er_val fallback_v)
{
    er_tank* tank = er_outt(er_tag_tank, result_v);
    if (tank != NULL) {
        _wisp_fail_with_val(rt, tank->msg_c, tank->val_v);
    }
    _wisp_fail_with_val(rt, "runtime error", fallback_v);
}

static er_val _wisp_run_apply_mode(wisp_rt* rt, size_t val_s, const er_val* val_v,
                                   er_eval_mode mode)
{
    ENKI_PROFILE_ZONE("_wisp_run_apply");
    if (val_s == 0) {
        return 0;
    }
    if (val_s == 1) {
        er_val out_v = mode == ER_EVAL_WHNF ? val_v[0] : er_eval_to(rt->loc_a, val_v[0], mode);
        if (!er_is_good(out_v)) {
            _wisp_fail_with_result(rt, out_v, val_v[0]);
        }
        return out_v;
    }

    er_thk* thk = er_thk_alloc(rt->loc_a, val_s);
    if (thk == NULL) {
        wisp_fail(rt, "oom");
    }
    er_val thk_v = er_thk_init(thk, ER_XUNK_APP, val_s, val_v);
    if (thk_v == 0) {
        wisp_fail(rt, "oom");
    }
    er_val res_v = er_eval_to(rt->loc_a, thk_v, mode);
    if (!er_is_good(res_v)) {
        _wisp_fail_with_result(rt, res_v, thk_v);
    }
    return res_v;
}

static er_val _wisp_run_apply(wisp_rt* rt, size_t val_s, const er_val* val_v)
{
    return _wisp_run_apply_mode(rt, val_s, val_v, ER_EVAL_WHNF);
}

// MARK: - Printing

static bool wisp_buf_is_print(const char* buf_c, size_t buf_s)
{
    if (buf_s == 0) {
        return false;
    }
    for (size_t i = 0; i < buf_s; i++) {
        if (!isprint((unsigned char)buf_c[i])) {
            return false;
        }
    }
    return true;
}

static bool wisp_direct_is_print(uint64_t dir_q)
{
    if (dir_q < 256) {
        return false;
    }
    while (dir_q > 0) {
        unsigned char byt_b = (unsigned char)(dir_q & 0xffu);
        if (!isprint(byt_b)) {
            return false;
        }
        dir_q >>= 8;
    }
    return true;
}

static void wisp_print_direct_text(enki_string_builder* sb, uint64_t dir_q)
{
    enki_sb_append_lit(sb, "\"");
    while (dir_q > 0) {
        enki_sb_append_char(sb, (char)(dir_q & 0xffu));
        dir_q >>= 8;
    }
    enki_sb_append_lit(sb, "\"");
}

static void wisp_print_direct_text_raw(enki_string_builder* sb, uint64_t dir_q)
{
    while (dir_q > 0) {
        enki_sb_append_char(sb, (char)(dir_q & 0xffu));
        dir_q >>= 8;
    }
}

static size_t wisp_bat_byte_size(er_bat* bat)
{
    if (bat == NULL || bat->lim_s == 0) {
        return 0;
    }
    size_t bytes_s = (bat->lim_s - 1u) * sizeof(uint64_t);
    uint64_t top_q = bat->lim_q[bat->lim_s - 1u];
    do {
        bytes_s++;
        top_q >>= 8u;
    } while (top_q != 0);
    return bytes_s;
}

static void wisp_print_value_sb(wisp_rt* rt, enki_string_builder* sb, er_val val_v);

static void wisp_print_bat(wisp_rt* rt, enki_string_builder* sb, er_bat* bat)
{
    size_t byt_s = wisp_bat_byte_size(bat);
    if (wisp_buf_is_print((const char*)bat->lim_q, byt_s)) {
        enki_sb_append_lit(sb, "\"");
        enki_sb_append_ref(sb, (const char*)bat->lim_q, byt_s);
        enki_sb_append_lit(sb, "\"");
        return;
    }

    mpz_t z;
    mpz_init(z);
    mpz_import(z, bat->lim_s, -1, sizeof(uint64_t), 0, 0, bat->lim_q);
    char* dec_c = mpz_get_str(NULL, 10, z);
    size_t dec_s = strlen(dec_c);
    for (size_t i = 0; i < dec_s; i++) {
        enki_sb_append_char(sb, dec_c[i]);
    }
    wisp_gmp_free(dec_c, dec_s + 1u);
    mpz_clear(z);
    UNUSED(rt);
}

static bool wisp_print_law_name_raw(wisp_rt* rt, enki_string_builder* sb, er_val name_v)
{
    if (er_is_cat(name_v)) {
        if (!wisp_direct_is_print(name_v)) {
            return false;
        }
        wisp_print_direct_text_raw(sb, name_v);
        return true;
    }

    er_bat* bat = er_outt(er_tag_bat, name_v);
    if (bat == NULL) {
        return false;
    }
    size_t byt_s = wisp_bat_byte_size(bat);
    if (!wisp_buf_is_print((const char*)bat->lim_q, byt_s)) {
        return false;
    }
    enki_sb_append_ref(sb, (const char*)bat->lim_q, byt_s);
    UNUSED(rt);
    return true;
}

static void wisp_print_law(wisp_rt* rt, enki_string_builder* sb, er_law* law)
{
    enki_sb_append_lit(sb, "{");
    if (!wisp_print_law_name_raw(rt, sb, law->name_v)) {
        wisp_print_value_sb(rt, sb, law->name_v);
    }
    enki_sb_append_lit(sb, "/");
    enki_sb_append_u64(sb, law->ari_d);
    enki_sb_append_lit(sb, " ");
    wisp_print_value_sb(rt, sb, law->body_v);
    enki_sb_append_lit(sb, "}");
}

static void wisp_print_app(wisp_rt* rt, enki_string_builder* sb, er_app* app)
{
    enki_sb_append_lit(sb, "(");
    wisp_print_value_sb(rt, sb, app->fn_v);
    for (size_t i = 0; i < app->arg_s; i++) {
        enki_sb_append_lit(sb, " ");
        wisp_print_value_sb(rt, sb, app->arg_v[i]);
    }
    enki_sb_append_lit(sb, ")");
}

static void wisp_print_value_sb(wisp_rt* rt, enki_string_builder* sb, er_val val_v)
{
    if (er_is_cat(val_v)) {
        if (wisp_direct_is_print(val_v)) {
            wisp_print_direct_text(sb, val_v);
        } else {
            enki_sb_append_u64(sb, val_v);
        }
        return;
    }

    er_bat* bat = er_outt(er_tag_bat, val_v);
    if (bat != NULL) {
        wisp_print_bat(rt, sb, bat);
        return;
    }

    er_app* app = er_outt(er_tag_app, val_v);
    if (app != NULL) {
        wisp_print_app(rt, sb, app);
        return;
    }

    er_pin* pin = er_outt(er_tag_pin, val_v);
    if (pin != NULL) {
        enki_sb_append_lit(sb, "<");
        wisp_print_value_sb(rt, sb, pin->val_v);
        enki_sb_append_lit(sb, ">");
        return;
    }

    er_law* law = er_outt(er_tag_law, val_v);
    if (law != NULL) {
        wisp_print_law(rt, sb, law);
        return;
    }

    er_thk* thk = er_outt(er_tag_thk, val_v);
    if (thk != NULL) {
        enki_sb_append_lit(sb, "<thk/");
        enki_sb_append_u64(sb, (uint64_t)thk->fun);
        enki_sb_append_lit(sb, ">");
        return;
    }

    er_tank* tank = er_outt(er_tag_tank, val_v);
    if (tank != NULL) {
        enki_sb_append_lit(sb, "<tank: ");
        enki_sb_append_cstr(sb, tank->msg_c == NULL ? "" : tank->msg_c);
        enki_sb_append_lit(sb, ">");
        return;
    }

    enki_sb_append_lit(sb, "<<bad>>");
}

char* wisp_print_value(wisp_rt* rt, er_val val_v, size_t* out_s)
{
    size_t def_s = 0;
    if (out_s == NULL) {
        out_s = &def_s;
    }
    enki_string_builder sb;
    enki_sb_init(&sb, rt->loc_a);
    wisp_print_value_sb(rt, &sb, val_v);
    return enki_sb_build(&sb, out_s);
}

// MARK: parser

static bool wisp_is_juxt(er_val val_v)
{
    return val_v == MOTE_HJUXT;
}

typedef enum _char_class {
    CL_EOF = 0,
    CL_WS = 1,
    CL_STR = 2,
    CL_END = 3,
    CL_PAR = 4,
    CL_BRA = 5,
    CL_CUR = 6,
    CL_SYM = 7,
    CL_ERR = 8
} char_class;

static char_class wisp_class(char c)
{
    switch (c) {
    case 0:
        return CL_EOF;
    case ' ':
    case '\n':
    case ';':
        return CL_WS;
    case '"':
        return CL_STR;
    case ')':
    case ']':
    case '}':
        return CL_END;
    case '(':
        return CL_PAR;
    case '[':
        return CL_BRA;
    case '{':
        return CL_CUR;
    default:
        return CL_SYM;
    }
}

static bool wisp_eat(char** str)
{
    while (1) {
        if (**str == ';') {
            while (**str != '\n' && **str != 0) {
                (*str)++;
            }
        }
        if (**str == ' ' || **str == '\n') {
            (*str)++;
            continue;
        }
        if (**str == 0) {
            return true;
        }
        break;
    }
    return false;
}

static er_val _wisp_parse_str(wisp_rt* rt, char** str_c)
{
    char* sin_c = *str_c + 1; // drop opening quote
    char* cur_c = sin_c;

    while (wisp_class(*cur_c) != CL_STR && wisp_class(*cur_c) != CL_EOF) {
        cur_c++;
    }

    if (wisp_class(*cur_c) == CL_EOF) {
        wisp_fail(rt, "unterminated string");
    }

    size_t val_s = (size_t)(cur_c - sin_c);
    *str_c = cur_c + 1; // drop closing quote
    return _wisp_quote(rt, wisp_bytes_nat(rt, sin_c, val_s));
}

static bool _wisp_is_close(char c)
{
    return c == ')' || c == ']' || c == '}';
}

static bool _wisp_is_gap(char c)
{
    return c == ' ' || c == '\n' || c == ';';
}

static void _wisp_expect_open(wisp_rt* rt, char** cur_c, char open_c)
{
    if (**cur_c != open_c) {
        wisp_fail(rt, "expected opening delimiter");
    }
    (*cur_c)++;
}

static bool _wisp_take_seq_close(wisp_rt* rt, char** cur_c)
{
    UNUSED(rt);
    if (_wisp_is_close(**cur_c)) {
        (*cur_c)++;
        return true;
    }
    return false;
}

static er_val _wisp_parse_par(wisp_rt* rt, char** str_c)
{
    char* cur_c = *str_c;
    er_val* stack_v = NULL;
    bool first_f = true;

    while (1) {
        if (*cur_c == 0) {
            wisp_fail(rt, "eof in list");
        }
        if (_wisp_take_seq_close(rt, &cur_c)) {
            break;
        }
        if (!first_f && !_wisp_is_gap(*cur_c)) {
            wisp_fail(rt, "bad list");
        }
        if (!first_f) {
            if (wisp_eat(&cur_c)) {
                wisp_fail(rt, "eof in list");
            }
            if (_wisp_take_seq_close(rt, &cur_c)) {
                break;
            }
        }
        arrpush(stack_v, wisp_parse(rt, &cur_c));
        first_f = false;
    }
    size_t stack_s = arrlen(stack_v);
    *str_c = cur_c;
    if (stack_s == 0) {
        arrfree(stack_v);
        return 0;
    }
    er_val ret_v = wisp_app_make(rt, 0, stack_s, stack_v);
    arrfree(stack_v);
    return ret_v;
}

static er_val _wisp_parse_seq(wisp_rt* rt, er_val tag_v, char** str_c, char close_c)
{
    char* cur_c = *str_c;
    er_val* stack_v = NULL;
    arrpush(stack_v, tag_v);
    bool first_f = true;
    UNUSED(close_c);

    while (1) {
        if (*cur_c == 0) {
            wisp_fail(rt, "eof in list");
        }
        if (_wisp_take_seq_close(rt, &cur_c)) {
            break;
        }
        if (!first_f && !_wisp_is_gap(*cur_c)) {
            wisp_fail(rt, "bad list");
        }
        if (!first_f) {
            if (wisp_eat(&cur_c)) {
                wisp_fail(rt, "eof in list");
            }
            if (_wisp_take_seq_close(rt, &cur_c)) {
                break;
            }
        }
        arrpush(stack_v, wisp_parse(rt, &cur_c));
        first_f = false;
    }
    size_t stack_s = arrlen(stack_v);

    *str_c = cur_c;
    er_val ret_v = wisp_app_make(rt, 0, stack_s, stack_v);
    arrfree(stack_v);
    return ret_v;
}

static er_val _wisp_parse_seq_cur(wisp_rt* rt, char** str_c)
{
    return _wisp_parse_seq(rt, wisp_bytes_nat(rt, "#curl", 5), str_c, '}');
}

static er_val _wisp_parse_seq_bra(wisp_rt* rt, char** str_c)
{
    return _wisp_parse_seq(rt, wisp_bytes_nat(rt, "#brak", 5), str_c, ']');
}

static er_val _wisp_parse_num(wisp_rt* rt, char* str_c, size_t str_s)
{
    char* buf_c = ea_calloc(rt->loc_a, char, str_s + 1u);
    if (buf_c == NULL) {
        wisp_fail(rt, "oom");
    }
    memcpy(buf_c, str_c, str_s);

    mpz_t z;
    mpz_init(z);
    if (mpz_set_str(z, buf_c, 10) != 0) {
        mpz_clear(z);
        ea_free(rt->loc_a, buf_c);
        wisp_fail(rt, "invalid number");
    }
    er_val out_v = wisp_mpz_to_nat(rt, z);
    mpz_clear(z);
    ea_free(rt->loc_a, buf_c);
    return out_v;
}

static er_val _wisp_parse_sym(wisp_rt* rt, char** str_c)
{
    char* cur_c = *str_c;
    bool num_f = true;
    while (wisp_class(*cur_c) == CL_SYM) {
        num_f &= (*cur_c <= '9') && (*cur_c >= '0');
        cur_c++;
    }

    char* sin_c = *str_c;
    *str_c = cur_c;
    size_t ret_s = (size_t)(cur_c - sin_c);

    if (num_f) {
        return _wisp_quote(rt, _wisp_parse_num(rt, sin_c, ret_s));
    }
    return wisp_bytes_nat(rt, sin_c, ret_s);
}

static er_val _wisp_parse_atom(wisp_rt* rt, char** str_c)
{
    er_val sym_v = _wisp_parse_sym(rt, str_c);
    char* cur_c = *str_c;
    char_class nex_b = wisp_class(*cur_c);
    bool has_jux = false;
    er_val jux_v = 0;

    if (nex_b == CL_PAR) {
        has_jux = true;
        _wisp_expect_open(rt, &cur_c, '(');
        jux_v = _wisp_parse_par(rt, &cur_c);
    } else if (nex_b == CL_BRA) {
        has_jux = true;
        _wisp_expect_open(rt, &cur_c, '[');
        jux_v = _wisp_parse_seq_bra(rt, &cur_c);
    } else if (nex_b == CL_CUR) {
        has_jux = true;
        _wisp_expect_open(rt, &cur_c, '{');
        jux_v = _wisp_parse_seq_cur(rt, &cur_c);
    } else if (nex_b == CL_STR) {
        has_jux = true;
        jux_v = _wisp_parse_str(rt, &cur_c);
    }
    *str_c = cur_c;
    if (has_jux) {
        return wisp_app3(rt, 0, MOTE_HJUXT, sym_v, jux_v);
    }
    return sym_v;
}

er_val wisp_parse(wisp_rt* rt, char** str_c)
{
    ENKI_PROFILE_ZONE("wisp_parse");
    char* cur_c = *str_c;
    if (wisp_eat(&cur_c)) {
        wisp_fail(rt, "eof");
    }
    er_val ret;
    switch (wisp_class(*cur_c)) {
    case CL_EOF:
        wisp_fail(rt, "eof");
    case CL_WS:
        wisp_fail(rt, "invariant: failed to consume whitespace");
    case CL_STR:
        ret = _wisp_parse_str(rt, &cur_c);
        *str_c = cur_c;
        return ret;
    case CL_SYM:
        ret = _wisp_parse_atom(rt, &cur_c);
        *str_c = cur_c;
        return ret;
    case CL_PAR:
        _wisp_expect_open(rt, &cur_c, '(');
        ret = _wisp_parse_par(rt, &cur_c);
        *str_c = cur_c;
        return ret;
    case CL_BRA:
        _wisp_expect_open(rt, &cur_c, '[');
        ret = _wisp_parse_seq_bra(rt, &cur_c);
        *str_c = cur_c;
        return ret;
    case CL_CUR:
        _wisp_expect_open(rt, &cur_c, '{');
        ret = _wisp_parse_seq_cur(rt, &cur_c);
        *str_c = cur_c;
        return ret;
    case CL_END:
        wisp_fail(rt, "unexpected closing delimiter");
    default:
        wisp_fail(rt, "fallthrough");
    }
    wisp_fail(rt, "fallthrough");
}

// MARK: - Environment

static wisp_env_entry* wisp_getenv(wisp_rt* rt, er_val key_v)
{
    if (!wisp_is_nat(key_v)) {
        return NULL;
    }
    for (wisp_env_entry* ent = rt->env; ent != NULL; ent = ent->next) {
        if (wisp_nat_eq(ent->key_v, key_v)) {
            return ent;
        }
    }
    return NULL;
}

static void wisp_putenv(wisp_rt* rt, er_val key_v, bool mac_f, er_val val_v)
{
    if (!wisp_is_nat(key_v)) {
        _wisp_fail_with_val(rt, "bad env key", key_v);
    }
    wisp_env_entry* ent = wisp_getenv(rt, key_v);
    if (ent == NULL) {
        ent = ea_calloc(rt->loc_a, wisp_env_entry, 1);
        if (ent == NULL) {
            wisp_fail(rt, "oom");
        }
        ent->key_v = key_v;
        ent->next = rt->env;
        rt->env = ent;
    }
    ent->mac_f = mac_f;
    ent->val_v = val_v;
}

typedef struct wisp_env_tmp {
    wisp_env_entry* ent;
    struct wisp_env_tmp* left;
    struct wisp_env_tmp* right;
} wisp_env_tmp;

static wisp_env_tmp* wisp_env_tmp_alloc(wisp_rt* rt, wisp_env_entry* ent)
{
    wisp_env_tmp* node = ea_calloc(rt->loc_a, wisp_env_tmp, 1);
    if (node == NULL) {
        wisp_fail(rt, "oom");
    }
    node->ent = ent;
    return node;
}

static wisp_env_tmp* wisp_env_tmp_put(wisp_rt* rt, wisp_env_tmp* root, wisp_env_entry* ent)
{
    if (root == NULL) {
        return wisp_env_tmp_alloc(rt, ent);
    }
    int cmp_i = wisp_nat_cmp(ent->key_v, root->ent->key_v);
    if (cmp_i < 0) {
        root->left = wisp_env_tmp_put(rt, root->left, ent);
    } else if (cmp_i > 0) {
        root->right = wisp_env_tmp_put(rt, root->right, ent);
    } else {
        root->ent = ent;
    }
    return root;
}

static er_val wisp_env_tmp_value(wisp_rt* rt, wisp_env_tmp* root)
{
    if (root == NULL) {
        return 0;
    }
    er_val args_v[] = {
        root->ent->mac_f ? MOTE_MACRO : MOTE_VALUE,
        root->ent->val_v,
        wisp_env_tmp_value(rt, root->left),
        wisp_env_tmp_value(rt, root->right),
    };
    return wisp_app_make(rt, root->ent->key_v, 4, args_v);
}

static void wisp_env_tmp_free(wisp_rt* rt, wisp_env_tmp* root)
{
    if (root == NULL) {
        return;
    }
    wisp_env_tmp_free(rt, root->left);
    wisp_env_tmp_free(rt, root->right);
    ea_free(rt->loc_a, root);
}

static er_val wisp_env_value(wisp_rt* rt)
{
    wisp_env_entry** ent_v = NULL;
    for (wisp_env_entry* ent = rt->env; ent != NULL; ent = ent->next) {
        arrpush(ent_v, ent);
    }

    wisp_env_tmp* root = NULL;
    for (size_t i = (size_t)arrlen(ent_v); i > 0; i--) {
        root = wisp_env_tmp_put(rt, root, ent_v[i - 1u]);
    }
    arrfree(ent_v);

    er_val out_v = wisp_env_tmp_value(rt, root);
    wisp_env_tmp_free(rt, root);
    return out_v;
}

static er_val wisp_expand_user(wisp_rt* rt, er_val mac_v, er_val val_v)
{
    er_val env_v = wisp_env_value(rt);
    er_val args_v[] = {mac_v, env_v, val_v};
    return _wisp_run_apply_mode(rt, 3, args_v, ER_EVAL_NF);
}

static er_val wisp_pin(wisp_rt* rt, er_val val_v)
{
    er_val pin_v = er_pin_make(rt->loc_a, wisp_eval(rt, val_v));
    if (pin_v == 0) {
        wisp_fail(rt, "oom");
    }
    return _wisp_quote(rt, pin_v);
}

static er_val wisp_app(wisp_rt* rt, size_t exp_s, er_val* exp_v)
{
    if (exp_s == 0) {
        return _wisp_quote(rt, 0);
    }

    er_val* val_v = malloc(exp_s * sizeof(er_val));
    if (val_v == NULL) {
        wisp_fail(rt, "oom");
    }

    for (size_t i = 0; i < exp_s; i++) {
        val_v[i] = wisp_eval(rt, exp_v[i]);
    }

    er_val res_v = _wisp_run_apply(rt, exp_s, val_v);
    free(val_v);
    return _wisp_quote(rt, res_v);
}

static void wisp_parse_bind(wisp_rt* rt, er_val bin_v, er_val* nam_v, er_val* exp_v)
{
    er_app* app = wisp_as_app(bin_v);
    if (app == NULL || app->arg_s != 3 || !wisp_is_juxt(app->arg_v[0]) ||
        !wisp_is_nat(app->arg_v[1])) {
        _wisp_fail_with_val(rt, "bad bind", bin_v);
    }
    *nam_v = app->arg_v[1];
    *exp_v = app->arg_v[2];
}

typedef struct _wisp_local {
    er_val nam_v;
    uint64_t idx_q;
    er_val exp_v; // 0 if argument and no expression
} wisp_local;

static bool wisp_quote_payload(er_val val_v, er_val* out_v);
static er_val compile_expr(wisp_rt* rt, size_t loc_s, wisp_local* loc, er_val val_v);
static er_val _wisp_macroexpand(wisp_rt* rt, size_t loc_s, wisp_local* loc, er_val val_v);

static bool wisp_find_local(size_t loc_s, wisp_local* loc, er_val nam_v, uint64_t* idx_q)
{
    for (size_t i = 0; i < loc_s; i++) {
        if (wisp_nat_eq(loc[i].nam_v, nam_v)) {
            *idx_q = loc[i].idx_q;
            return true;
        }
    }
    return false;
}

static bool wisp_quote_payload(er_val val_v, er_val* out_v)
{
    er_app* app = wisp_as_app(val_v);
    if (app != NULL && app->fn_v == 1 && app->arg_s == 1) {
        *out_v = app->arg_v[0];
        return true;
    }
    return false;
}

static er_val compile_expr(wisp_rt* rt, size_t loc_s, wisp_local* loc, er_val val_v)
{
#define recur(x) compile_expr(rt, loc_s, loc, x)
#define law_quote(x) wisp_law_quote(rt, x)
    if (val_v == 0) {
        return law_quote(0);
    }
    if (wisp_is_nat(val_v)) {
        for (size_t i = 0; i < loc_s; i++) {
            if (wisp_nat_eq(loc[i].nam_v, val_v)) {
                return loc[i].idx_q;
            }
        }
        wisp_env_entry* ent = wisp_getenv(rt, val_v);
        if (ent == NULL) {
            _wisp_fail_with_val(rt, "unbound", val_v);
        }
        return law_quote(ent->val_v);
    }

    er_val payload_v = 0;
    if (wisp_quote_payload(val_v, &payload_v)) {
        return law_quote(payload_v);
    }

    er_app* app = wisp_as_app(val_v);
    if (app == NULL || app->fn_v != 0) {
        return law_quote(val_v);
    }
    if (app->arg_s == 0) {
        return law_quote(0);
    }
    if (app->arg_s == 3 && app->arg_v[0] == MOTE_HJUXT && app->arg_v[1] == (er_val)'#') {
        return wisp_eval(rt, app->arg_v[2]);
    }

    er_val ret = recur(app->arg_v[0]);
    for (size_t i = 1; i < app->arg_s; i++) {
        ret = wisp_app2(rt, 0, ret, recur(app->arg_v[i]));
    }
    return ret;
#undef recur
#undef law_quote
}

static er_val wisp_law(wisp_rt* rt, er_val tag_v, er_val sig_v, er_val bod_v,
                       size_t bin_s, // binders
                       er_val* bin_v)
{
    er_val teg_v = wisp_eval(rt, tag_v);
    er_app* app = wisp_as_app(sig_v);
    if (app == NULL || app->fn_v != 0 || app->arg_s < 2) {
        _wisp_fail_with_val(rt, "bad law signature", sig_v);
    }
    er_val nam_v = app->arg_v[0];
    if (!wisp_is_nat(nam_v)) {
        _wisp_fail_with_val(rt, "bad law name", nam_v);
    }

    size_t arg_s = app->arg_s - 1u;
    if (arg_s > UINT32_MAX) {
        _wisp_fail_with_val(rt, "law arity overflow", sig_v);
    }
    er_val* arg_v = app->arg_v + 1;

    size_t loc_s = arg_s + bin_s + 1u;
    wisp_local* loc = ea_calloc(rt->loc_a, wisp_local, loc_s);
    if (loc == NULL) {
        wisp_fail(rt, "oom");
    }
    loc[0].nam_v = nam_v;
    loc[0].idx_q = 0;
    loc[0].exp_v = 0;
    size_t i;
    for (i = 1; i <= arg_s; i++) {
        if (!wisp_is_nat(arg_v[i - 1])) {
            _wisp_fail_with_val(rt, "bad law argument", arg_v[i - 1]);
        }
        loc[i].nam_v = arg_v[i - 1];
        loc[i].idx_q = i;
        loc[i].exp_v = 0;
    }
    for (; i < loc_s; i++) {
        size_t idx_s = i - arg_s - 1u;
        loc[i].idx_q = i;
        wisp_parse_bind(rt, bin_v[idx_s], &loc[i].nam_v, &loc[i].exp_v);
    }

    er_val* exp_v = NULL;
    for (size_t j = 0; j < bin_s; j++) {
        arrpush(exp_v, _wisp_macroexpand(rt, loc_s, loc, loc[j + arg_s + 1u].exp_v));
    }

    er_val bod_exp_v = _wisp_macroexpand(rt, loc_s, loc, bod_v);

    er_val* let_v = NULL;
    for (size_t j = 0; j < bin_s; j++) {
        arrpush(let_v, compile_expr(rt, loc_s, loc, exp_v[j]));
    }
    arrfree(exp_v);

    bod_exp_v = compile_expr(rt, loc_s, loc, bod_exp_v);
    for (size_t j = bin_s; j > 0; j--) {
        bod_exp_v = wisp_app2(rt, 1, let_v[j - 1u], bod_exp_v);
    }
    arrfree(let_v);

    er_val law_v = er_law_make(rt->loc_a, teg_v, bod_exp_v, (uint32_t)arg_s);
    if (law_v == 0) {
        _wisp_fail_with_val(rt, "failed to compile law", bod_exp_v);
    }
    ea_free(rt->loc_a, loc);
    return _wisp_quote(rt, law_v);
}

static er_val wisp_bind(wisp_rt* rt, er_val nam_v, er_val val_v, bool mac_f)
{
    if (!wisp_is_nat(nam_v)) {
        _wisp_fail_with_val(rt, "bad env key", nam_v);
    }
    er_val vel_v = wisp_eval(rt, val_v);
    wisp_putenv(rt, nam_v, mac_f, vel_v);
    return _wisp_quote(rt, nam_v);
}

static er_val wisp_export(wisp_rt* rt, size_t sym_s, const er_val sym_v[])
{
    wisp_env_entry** keep_v = NULL;
    for (size_t i = 0; i < sym_s; i++) {
        if (!wisp_is_nat(sym_v[i])) {
            _wisp_fail_with_val(rt, "bad export", sym_v[i]);
        }
        wisp_env_entry* ent = wisp_getenv(rt, sym_v[i]);
        if (ent == NULL) {
            _wisp_fail_with_val(rt, "unbound export", sym_v[i]);
        }
        arrpush(keep_v, ent);
    }

    rt->env = NULL;
    for (size_t i = 0; i < sym_s; i++) {
        wisp_putenv(rt, keep_v[i]->key_v, (bool)keep_v[i]->mac_f, keep_v[i]->val_v);
    }
    arrfree(keep_v);
    return 0;
}

static er_val _wisp_expand1(wisp_rt* rt, size_t loc_s, wisp_local* loc, uint64_t mac_q,
                            er_val val_v)
{
    (void)loc_s;
    (void)loc;

    er_app* app = wisp_as_app(val_v);
    if (app == NULL) {
        wisp_fail(rt, "expected row expanding macro");
    }

    switch (mac_q) {
    case MOTE_HBIND:
        if (app->arg_s != 3) {
            _wisp_fail_with_val(rt, "invalid #bind", val_v);
        }
        return wisp_bind(rt, app->arg_v[1], app->arg_v[2], false);
    case MOTE_HMACRO:
        if (app->arg_s != 3) {
            _wisp_fail_with_val(rt, "invalid #macro", val_v);
        }
        return wisp_bind(rt, app->arg_v[1], app->arg_v[2], true);
    case MOTE_HPIN:
        if (app->arg_s != 2) {
            _wisp_fail_with_val(rt, "invalid #pin", val_v);
        }
        return wisp_pin(rt, app->arg_v[1]);
    case MOTE_HLAW:
        if (app->arg_s < 4) {
            _wisp_fail_with_val(rt, "invalid #law", val_v);
        }
        return wisp_law(rt, app->arg_v[1], app->arg_v[2], app->arg_v[app->arg_s - 1u],
                        app->arg_s - 4u, &app->arg_v[3]);
    case MOTE_HAPP: {
        if (app->arg_s < 1) {
            _wisp_fail_with_val(rt, "invalid #app", val_v);
        }
        return wisp_app(rt, app->arg_s - 1u, app->arg_v + 1);
    }
    case MOTE_HEXPORT:
        return wisp_export(rt, app->arg_s - 1u, app->arg_v + 1);
    default:
        _wisp_fail_with_val(rt, "unknown macro", mac_q);
    }
    return 0;
}

static bool is_sys_macro(uint64_t mac_q)
{
    return ((mac_q == MOTE_HBIND) || (mac_q == MOTE_HLAW) || (mac_q == MOTE_HPIN) ||
            (mac_q == MOTE_HMACRO) || (mac_q == MOTE_HAPP) || (mac_q == MOTE_HEXPORT));
}

static er_val _wisp_macroexpand(wisp_rt* rt, size_t loc_s, wisp_local* loc, er_val val_v)
{
    ENKI_PROFILE_ZONE("_wisp_macroexpand");

    if (wisp_is_nat(val_v)) {
        return val_v;
    }
    er_app* app = wisp_as_app(val_v);
    if (app == NULL) {
        return val_v;
    }

    // Quoted forms
    if (app->fn_v != 0) {
        return val_v;
    }

    if (app->arg_s == 3 && wisp_is_juxt(app->arg_v[0]) && app->arg_v[1] == '#' &&
        loc_s > 0) {
        er_val inn_v = _wisp_macroexpand(rt, loc_s, loc, app->arg_v[2]);
        return wisp_app3(rt, 0, app->arg_v[0], '#', inn_v);
    }

    if (app->arg_s == 0) {
        return val_v;
    }

    uint64_t local_idx_q = 0;
    bool head_is_local =
        wisp_is_nat(app->arg_v[0]) && wisp_find_local(loc_s, loc, app->arg_v[0], &local_idx_q);
    if (!head_is_local && wisp_is_nat(app->arg_v[0])) {
        wisp_env_entry* ent = wisp_getenv(rt, app->arg_v[0]);
        if (ent != NULL) {
            if (ent->mac_f) {
                er_val out_v = wisp_expand_user(rt, ent->val_v, val_v);
                return _wisp_macroexpand(rt, loc_s, loc, out_v);
            }
        } else if (is_sys_macro(app->arg_v[0])) {
            er_val out_v = _wisp_expand1(rt, loc_s, loc, app->arg_v[0], val_v);
            return _wisp_macroexpand(rt, loc_s, loc, out_v);
        }
    }

    er_val* nex_v = NULL;
    for (size_t i = 0; i < app->arg_s; i++) {
        arrpush(nex_v, _wisp_macroexpand(rt, loc_s, loc, app->arg_v[i]));
    }
    er_val out_v = wisp_app_make(rt, 0, arrlen(nex_v), nex_v);
    arrfree(nex_v);
    return out_v;
}

static er_val _wisp_apple(wisp_rt* rt, size_t val_s, er_val* val_v)
{
    return _wisp_run_apply(rt, val_s, val_v);
}

static er_val _wisp_thunk(wisp_rt* rt, er_val val_v);
static er_val _wisp_delay(wisp_rt* rt, er_val val_v);

static er_val _wisp_delay_apply(wisp_rt* rt, size_t val_s, const er_val* val_v)
{
    if (val_s == 0) {
        return 0;
    }
    if (val_s == 1) {
        return val_v[0];
    }
    er_thk* thk = er_thk_alloc(rt->loc_a, val_s);
    if (thk == NULL) {
        wisp_fail(rt, "oom");
    }
    er_val thk_v = er_thk_init(thk, ER_XUNK_APP, val_s, val_v);
    if (thk_v == 0) {
        wisp_fail(rt, "oom");
    }
    return thk_v;
}

static er_val _wisp_delay(wisp_rt* rt, er_val val_v)
{
    if (val_v == 0) {
        return 0;
    }
    if (wisp_is_nat(val_v)) {
        wisp_env_entry* ent = wisp_getenv(rt, val_v);
        if (!ent) {
            _wisp_fail_with_val(rt, "unbound thk", val_v);
        }
        return ent->val_v;
    }

    er_app* app = wisp_as_app(val_v);
    if (app == NULL) {
        return val_v;
    }
    if (app->fn_v == 1 && app->arg_s == 1) {
        return app->arg_v[0];
    }
    if (app->fn_v != 0) {
        _wisp_fail_with_val(rt, "thunk: expected list", val_v);
    }

    er_val* nex_v = malloc(app->arg_s * sizeof(er_val));
    if (nex_v == NULL && app->arg_s != 0) {
        wisp_fail(rt, "oom");
    }
    for (size_t i = 0; i < app->arg_s; i++) {
        nex_v[i] = _wisp_delay(rt, app->arg_v[i]);
    }
    er_val ret_v = _wisp_delay_apply(rt, app->arg_s, nex_v);
    free(nex_v);
    return ret_v;
}

static er_val _wisp_thunk(wisp_rt* rt, er_val val_v)
{
    ENKI_PROFILE_ZONE("_wisp_thunk");
    if (val_v == 0) {
        return 0;
    }
    if (wisp_is_nat(val_v)) {
        wisp_env_entry* ent = wisp_getenv(rt, val_v);
        if (!ent) {
            _wisp_fail_with_val(rt, "unbound thk", val_v);
        }
        return ent->val_v;
    }

    er_app* app = wisp_as_app(val_v);
    if (app == NULL) {
        return val_v;
    }
    if (app->fn_v == 1 && app->arg_s == 1) {
        return app->arg_v[0];
    }
    if (app->fn_v != 0) {
        _wisp_fail_with_val(rt, "thunk: expected list", val_v);
    }

    er_val* nex_v = malloc(app->arg_s * sizeof(er_val));
    if (nex_v == NULL && app->arg_s != 0) {
        wisp_fail(rt, "oom");
    }
    for (size_t i = 0; i < app->arg_s; i++) {
        nex_v[i] = i == 0 ? _wisp_thunk(rt, app->arg_v[i]) : _wisp_delay(rt, app->arg_v[i]);
    }
    er_val ret_v = _wisp_apple(rt, app->arg_s, nex_v);
    free(nex_v);
    return ret_v;
}

er_val wisp_macroexpand(wisp_rt* rt, er_val val_v)
{
    return _wisp_macroexpand(rt, 0, NULL, val_v);
}

er_val wisp_thunk(wisp_rt* rt, er_val val_v)
{
    return _wisp_thunk(rt, val_v);
}

er_val wisp_eval(wisp_rt* rt, er_val val_v)
{
    ENKI_PROFILE_ZONE("wisp_eval");
    er_val exp_v = wisp_macroexpand(rt, val_v);
    return wisp_thunk(rt, exp_v);
}
