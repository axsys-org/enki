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

#define MOTE_JUXT ea_s4('J', 'U', 'X', 'T')
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

static er_val _wisp_run_apply(wisp_rt* rt, size_t val_s, const er_val* val_v)
{
    ENKI_PROFILE_ZONE("_wisp_run_apply");
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
    er_val res_v = er_eval(rt->loc_a, thk_v);
    if (res_v == er_bad) {
        _wisp_fail_with_val(rt, "runtime error", thk_v);
    }
    return res_v;
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
    return val_v == MOTE_HJUXT || val_v == MOTE_JUXT;
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

static const char_class char_classes[256] = {
    /* 0x00 NUL */ 0, 8, 8, 8, 8, 8, 8, 8, 8, 8, 1, 8, 8, 8, 8, 8,
    /* 0x10     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0x20 ' ' */ 1, 8, 2, 7, 7, 7, 7, 7, 4, 3, 7, 7, 7, 7, 7, 7,
    /* 0x30 '0' */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 1, 8, 8, 8, 8,
    /* 0x40 '@' */ 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    /* 0x50 'P' */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 5, 8, 3, 8, 7,
    /* 0x60 '`' */ 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    /* 0x70 'p' */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 6, 8, 3, 8, 8,
    /* 0x80     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0x90     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0xA0     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0xB0     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0xC0     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0xD0     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0xE0     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    /* 0xF0     */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
};

static char_class wisp_class(char c)
{
    return char_classes[(unsigned char)c];
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
        wisp_fail(rt, "unclosed string");
    }

    size_t val_s = (size_t)(cur_c - sin_c);
    *str_c = cur_c + 1; // drop closing quote
    return _wisp_quote(rt, wisp_bytes_nat(rt, sin_c, val_s));
}

static bool _wisp_is_close(char c)
{
    return c == ')' || c == ']' || c == '}';
}

static void _wisp_expect_open(wisp_rt* rt, char** cur_c, char open_c)
{
    if (**cur_c != open_c) {
        wisp_fail(rt, "expected opening delimiter");
    }
    (*cur_c)++;
}

static bool _wisp_take_close(wisp_rt* rt, char** cur_c, char close_c)
{
    if (wisp_eat(cur_c)) {
        wisp_fail(rt, "unclosed delimiter");
    }
    if (**cur_c == close_c) {
        (*cur_c)++;
        return true;
    }
    if (_wisp_is_close(**cur_c)) {
        wisp_fail(rt, "mismatched closing delimiter");
    }
    return false;
}

static er_val _wisp_parse_par(wisp_rt* rt, char** str_c)
{
    char* cur_c = *str_c;
    er_val* stack_v = NULL;

    while (1) {
        if (_wisp_take_close(rt, &cur_c, ')')) {
            break;
        }
        arrpush(stack_v, wisp_parse(rt, &cur_c));
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

    while (1) {
        if (_wisp_take_close(rt, &cur_c, close_c)) {
            break;
        }
        arrpush(stack_v, wisp_parse(rt, &cur_c));
    }
    size_t stack_s = arrlen(stack_v);

    *str_c = cur_c;
    er_val ret_v = wisp_app_make(rt, 0, stack_s, stack_v);
    arrfree(stack_v);
    return ret_v;
}

static er_val _wisp_parse_seq_cur(wisp_rt* rt, char** str_c)
{
    return _wisp_parse_seq(rt, wisp_bytes_nat(rt, "CURL", 4), str_c, '}');
}

static er_val _wisp_parse_seq_bra(wisp_rt* rt, char** str_c)
{
    return _wisp_parse_seq(rt, wisp_bytes_nat(rt, "BRAK", 4), str_c, ']');
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
    if (nex_b == CL_ERR) {
        wisp_fail(rt, "invalid character");
    }
    if (has_jux) {
        return wisp_app3(rt, 0, wisp_bytes_nat(rt, "JUXT", 4), sym_v, jux_v);
    }
    return sym_v;
}

er_val wisp_parse(wisp_rt* rt, char** str_c)
{
    ENKI_PROFILE_ZONE("wisp_parse");
    char* cur_c = *str_c;
    if (wisp_eat(&cur_c)) {
        wisp_fail(rt, "Reached EOF");
    }
    er_val ret;
    switch (wisp_class(*cur_c)) {
    case CL_EOF:
        wisp_fail(rt, "Reached EOF");
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
    case CL_ERR:
        wisp_fail(rt, "invalid character");
    default:
        wisp_fail(rt, "fallthrough");
    }
    wisp_fail(rt, "fallthrough");
}

// MARK: - Environment

static wisp_env_entry* wisp_getenv(wisp_rt* rt, er_val key_v)
{
    if (!er_is_cat(key_v)) {
        return NULL;
    }
    for (wisp_env_entry* ent = rt->env; ent != NULL; ent = ent->next) {
        if (ent->key_v == key_v) {
            return ent;
        }
    }
    return NULL;
}

static void wisp_putenv(wisp_rt* rt, er_val key_v, bool mac_f, er_val val_v)
{
    if (!er_is_cat(key_v)) {
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

static er_val wisp_expand_user(wisp_rt* rt, er_val mac_v, er_val val_v)
{
    UNUSED(rt);
    UNUSED(mac_v);
    UNUSED(val_v);
    return 0;
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
        !er_is_cat(app->arg_v[1])) {
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
        if (loc[i].nam_v == nam_v) {
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

static bool wisp_law_quote_payload(er_val val_v, er_val* out_v)
{
    er_app* app = wisp_as_app(val_v);
    if (app != NULL && app->fn_v == 0 && app->arg_s == 1) {
        *out_v = app->arg_v[0];
        return true;
    }
    return false;
}

static bool wisp_is_pin_set(er_val val_v, er_val set_v)
{
    er_val payload_v = 0;
    if (!wisp_quote_payload(val_v, &payload_v)) {
        return false;
    }
    er_pin* pin = er_outt(er_tag_pin, payload_v);
    return pin != NULL && pin->val_v == set_v;
}

static er_val wisp_prim_placeholder(wisp_rt* rt, er_val name_v, uint32_t ari_d)
{
    er_val body_v = wisp_law_quote(rt, 0);
    er_val law_v = er_law_make(rt->loc_a, name_v, body_v, ari_d);
    if (law_v == 0) {
        wisp_fail(rt, "failed to compile primitive placeholder");
    }
    return law_v;
}

static bool wisp_prim_arity(er_val name_v, uint32_t* ari_d)
{
    switch (name_v) {
    case ea_s3('P', 'i', 'n'):
        *ari_d = 1;
        return true;
    case ea_s3('L', 'a', 'w'):
        *ari_d = 3;
        return true;
    case ea_s4('E', 'l', 'i', 'm'):
        *ari_d = 6;
        return true;
    case ea_s3('N', 'a', 't'):
    case ea_s5('A', 'r', 'i', 't', 'y'):
    case ea_s4('N', 'a', 'm', 'e'):
    case ea_s4('B', 'o', 'd', 'y'):
    case ea_s5('U', 'n', 'p', 'i', 'n'):
    case ea_s2('S', 'z'):
    case ea_s4('L', 'a', 's', 't'):
    case ea_s4('I', 'n', 'i', 't'):
    case ea_s3('I', 'n', 'c'):
    case ea_s3('D', 'e', 'c'):
    case ea_s4('B', 'i', 't', 's'):
    case ea_s5('B', 'y', 't', 'e', 's'):
    case ea_s5('F', 'o', 'r', 'c', 'e'):
    case ea_s2('H', 'd'):
    case ea_s3('N', 'i', 'l'):
    case ea_s5('T', 'r', 'u', 't', 'h'):
        *ari_d = 1;
        return true;
    case ea_s3('A', 'd', 'd'):
    case ea_s3('S', 'u', 'b'):
    case ea_s3('R', 's', 'h'):
    case ea_s3('L', 's', 'h'):
    case ea_s3('D', 'i', 'v'):
    case ea_s3('M', 'u', 'l'):
    case ea_s3('M', 'o', 'd'):
    case ea_s4('T', 'e', 's', 't'):
    case ea_s5('T', 'r', 'u', 'n', 'c'):
    case ea_s4('W', 'e', 'l', 'd'):
    case ea_s2('I', 'x'):
    case ea_s2('O', 'r'):
    case ea_s3('A', 'n', 'd'):
    case ea_s2('I', 'f'):
    case ea_s2('E', 'q'):
    case ea_s2('L', 'e'):
    case ea_s3('C', 'm', 'p'):
        *ari_d = name_v == ea_s2('I', 'f') ? 3u : 2u;
        return true;
    case ea_s7('L', 'o', 'a', 'd', 'V', 'a', 'r'):
    case ea_s3('R', 'e', 'p'):
    case ea_s5('S', 'l', 'i', 'c', 'e'):
    case ea_s2('U', 'p'):
        *ari_d = 3;
        return true;
    case ea_s5('S', 't', 'o', 'r', 'e'):
        *ari_d = 4;
        return true;
    case ea_s4('C', 'o', 'u', 'p'):
        *ari_d = 2;
        return true;
    default:
        return false;
    }
}

static bool wisp_direct_prim_wrapper(er_val law_v, er_val* prim_law_v)
{
    er_law* law = er_outt(er_tag_law, law_v);
    if (law == NULL) {
        return false;
    }

    er_val cur_v = law->body_v;
    er_val* arg_v = NULL;
    er_app* app = NULL;
    while ((app = wisp_as_app(cur_v)) != NULL && app->fn_v == 0 && app->arg_s == 2) {
        arrpush(arg_v, app->arg_v[1]);
        cur_v = app->arg_v[0];
    }

    er_val payload_v = 0;
    er_law* prim_law = NULL;
    uint32_t prim_ari_d = 0;
    size_t arg_s = (size_t)arrlen(arg_v);
    bool ok_f = wisp_law_quote_payload(cur_v, &payload_v) &&
                (prim_law = er_outt(er_tag_law, payload_v)) != NULL &&
                wisp_prim_arity(prim_law->name_v, &prim_ari_d) &&
                prim_ari_d == law->ari_d && (size_t)prim_ari_d == arg_s;

    for (size_t k = 0; ok_f && k < arg_s; k++) {
        ok_f = arg_v[k] == (er_val)(arg_s - k);
    }

    if (ok_f) {
        *prim_law_v = payload_v;
    }
    arrfree(arg_v);
    return ok_f;
}

static bool wisp_compile_pin66_call(wisp_rt* rt, size_t loc_s, wisp_local* loc, er_app* app,
                                    er_val* out_v)
{
    if (app->arg_s != 2 || !wisp_is_pin_set(app->arg_v[0], (er_val)'B')) {
        return false;
    }

    er_app* row = wisp_as_app(app->arg_v[1]);
    if (row == NULL) {
        return false;
    }

    er_val tag_v = row->fn_v;
    const er_val* arg_v = row->arg_v;
    size_t arg_s = row->arg_s;
    if (tag_v == 0) {
        if (arg_s == 0) {
            return false;
        }
        tag_v = row->arg_v[0];
        arg_v = row->arg_v + 1;
        arg_s = row->arg_s - 1;
    }

    er_val name_v = 0;
    if (!wisp_quote_payload(tag_v, &name_v)) {
        return false;
    }

    uint32_t ari_d = 0;
    if (!wisp_prim_arity(name_v, &ari_d)) {
        return false;
    }

    er_val prim_law_v = wisp_prim_placeholder(rt, name_v, ari_d);
    er_val ret_v = wisp_law_quote(rt, prim_law_v);
    for (size_t i = 0; i < arg_s; i++) {
        ret_v = wisp_app2(rt, 0, ret_v, compile_expr(rt, loc_s, loc, arg_v[i]));
    }
    *out_v = ret_v;
    return true;
}

static er_val compile_expr(wisp_rt* rt, size_t loc_s, wisp_local* loc, er_val val_v)
{
#define recur(x) compile_expr(rt, loc_s, loc, x)
#define law_quote(x) wisp_law_quote(rt, x)
    if (val_v == 0) {
        return law_quote(0);
    }
    if (er_is_cat(val_v)) {
        for (size_t i = 0; i < loc_s; i++) {
            if (loc[i].nam_v == val_v) {
                return loc[i].idx_q;
            }
        }
        wisp_env_entry* ent = wisp_getenv(rt, val_v);
        if (ent == NULL) {
            _wisp_fail_with_val(rt, "unbound", val_v);
        }
        er_val prim_law_v = 0;
        if (wisp_direct_prim_wrapper(ent->val_v, &prim_law_v)) {
            return law_quote(prim_law_v);
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

    er_val pin66_v = 0;
    if (wisp_compile_pin66_call(rt, loc_s, loc, app, &pin66_v)) {
        return pin66_v;
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
    if (!er_is_cat(nam_v)) {
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
        if (!er_is_cat(arg_v[i - 1])) {
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

    er_val* let_v = NULL;
    for (size_t j = 0; j < bin_s; j++) {
        er_val exp_v = _wisp_macroexpand(rt, loc_s, loc, loc[j + arg_s + 1u].exp_v);
        arrpush(let_v, compile_expr(rt, loc_s, loc, exp_v));
    }

    er_val bod_exp_v = _wisp_macroexpand(rt, loc_s, loc, bod_v);
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

static er_val wisp_bind(wisp_rt* rt, er_val nam_v, er_val val_v)
{
    er_val vel_v = wisp_eval(rt, val_v);
    wisp_putenv(rt, nam_v, false, vel_v);
    return nam_v;
}

static er_val _wisp_expand1(wisp_rt* rt, size_t loc_s, wisp_local* loc, uint64_t mac_q,
                            er_val val_v)
{
    er_app* app = wisp_as_app(val_v);
    if (app == NULL) {
        wisp_fail(rt, "expected row expanding macro");
    }

    wisp_env_entry* ent = wisp_getenv(rt, mac_q);
    if (ent && ent->mac_f) {
        return wisp_expand_user(rt, ent->val_v, val_v);
    }

#define RECUR(x) _wisp_macroexpand(rt, loc_s, loc, x)
    switch (mac_q) {
    case MOTE_HBIND:
        if (app->arg_s != 3) {
            _wisp_fail_with_val(rt, "invalid #bind", val_v);
        }
        return wisp_bind(rt, app->arg_v[1], app->arg_v[2]);
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
        if (app->arg_s < 2) {
            _wisp_fail_with_val(rt, "invalid #app", val_v);
        }
        er_val* args_v = NULL;
        for (size_t i = 1; i < app->arg_s; i++) {
            arrpush(args_v, RECUR(app->arg_v[i]));
        }
        size_t args_s = arrlen(args_v);
        er_val ret_v = wisp_app(rt, args_s, args_v);
        arrfree(args_v);
        return ret_v;
    }
    default:
        _wisp_fail_with_val(rt, "unknown macro", mac_q);
    }
#undef RECUR
    return 0;
}

static bool is_sys_macro(uint64_t mac_q)
{
    return ((mac_q == MOTE_HBIND) || (mac_q == MOTE_HLAW) || (mac_q == MOTE_HPIN) ||
            (mac_q == MOTE_HAPP) || (mac_q == MOTE_HEXPORT));
}

static er_val _wisp_macroexpand(wisp_rt* rt, size_t loc_s, wisp_local* loc, er_val val_v)
{
    ENKI_PROFILE_ZONE("_wisp_macroexpand");

    if (er_is_cat(val_v)) {
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
        er_is_cat(app->arg_v[0]) && wisp_find_local(loc_s, loc, app->arg_v[0], &local_idx_q);
    wisp_env_entry* ent = wisp_getenv(rt, app->arg_v[0]);
    if (!head_is_local && ((ent && ent->mac_f) || is_sys_macro(app->arg_v[0]))) {
        return _wisp_expand1(rt, loc_s, loc, app->arg_v[0], val_v);
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

static er_val _wisp_thunk(wisp_rt* rt, er_val val_v)
{
    ENKI_PROFILE_ZONE("_wisp_thunk");
    if (val_v == 0) {
        return 0;
    }
    if (er_is_cat(val_v)) {
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

    er_val* nex_v = malloc(app->arg_s * sizeof(er_val));
    if (nex_v == NULL && app->arg_s != 0) {
        wisp_fail(rt, "oom");
    }
    for (size_t i = 0; i < app->arg_s; i++) {
        nex_v[i] = _wisp_thunk(rt, app->arg_v[i]);
    }
    er_val ret_v = _wisp_apple(rt, app->arg_s, nex_v);
    free(nex_v);
    return ret_v;
}

er_val wisp_eval(wisp_rt* rt, er_val val_v)
{
    ENKI_PROFILE_ZONE("wisp_eval");
    er_val exp_v = _wisp_macroexpand(rt, 0, NULL, val_v);
    return _wisp_thunk(rt, exp_v);
}
