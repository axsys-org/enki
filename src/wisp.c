#include <enki/app.h>
#include <enki/bst.h>
#include <enki/interp.h>
#include <enki/law.h>
#include <enki/motes.h>
#include <enki/plan.h>
#include <enki/print.h>
#include <enki/profile.h>
#include <enki/util.h>
#include <enki/vector.h>
#include <enki/wisp.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#define MOTE_JUXT ea_s4('J', 'U', 'X', 'T')
#define WISP_HEAP_SIZE ((size_t)1 << 27)
#define WISP_STORE_SIZE ((size_t)1 << 14)
#define WISP_SCRATCH_SIZE ((size_t)1 << 24)

// MARK: - Runtime lifecycle
wisp_rt* wisp_rt_alloc(const enki_allocator* loc_a)
{
    wisp_rt* rt = ea_calloc(loc_a, wisp_rt, 1);
    rt->i = enki_interp_create(loc_a, WISP_HEAP_SIZE, "./snap", WISP_STORE_SIZE,
                               WISP_SCRATCH_SIZE);
    rt->gc = rt->i->gc;
    rt->loc_a = &sys_a;
    rt->env = ea_calloc(loc_a, enki_bst_tree, 1);
    enki_bst_init(rt->env, *loc_a);
    rt->err_f = 0; //
    rt->msg_c = NULL;
    return rt;
}

void wisp_rt_free(const enki_allocator* loc_a, wisp_rt* rt)
{
    enki_gc_destroy(rt->gc);
    free(rt->msg_c);
    ea_free(loc_a, rt);
}

[[noreturn]] static void wisp_fail_fn(wisp_rt* rt, const char* msg, int line)
{
    if (rt->err_f) {
        free(rt->msg_c);
        rt->msg_c = malloc(strlen(msg) + 1);
        strcpy(rt->msg_c, msg);
        longjmp(rt->errjmp, 0);
    } else {
        fprintf(stderr, "wisp.c:%i fail: %s\r\n", line, msg);
        abort();
    }
}

#define wisp_fail(rt, msg) wisp_fail_fn(rt, msg, __LINE__)

static void _wisp_fail_with_val(wisp_rt* rt, char* msg_c, enki_value val_v)
{
    char* key_c = enki_pvalue(rt->loc_a, val_v);
    size_t str_s = strlen(msg_c) + strlen(key_c) + 5;
    char* str_c = ea_calloc(rt->loc_a, char, str_s);
    snprintf(str_c, str_s, "%s: %s\n", msg_c, key_c);
    wisp_fail(rt, str_c);
}

static uint64_t wisp_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static enki_value _wisp_plan_apply(wisp_rt* rt, size_t val_s, enki_value* val_v)
{
    ENKI_PROFILE_ZONE("_wisp_plan_apply");
    assert(val_s > 0);
    if (val_s == 1) {
        return val_v[0];
    }

    uint64_t start_ns = wisp_now_ns();
    enki_plan plan;
    enki_plan_init(&plan, rt->gc);

    enki_value res_v = 0;
    enki_error err = enki_plan_apply(&plan, val_v[0], val_s - 1, &val_v[1], &res_v);
    uint64_t end_ns = wisp_now_ns();
    uint64_t elapsed_ns = end_ns >= start_ns ? end_ns - start_ns : 0;
    fprintf(stderr, "wisp apply %zu arg(s): %" PRIu64 " ns, %" PRIu64
                    " WHNF eval(s), %" PRIu64 " apply(s)\n",
            val_s - 1, elapsed_ns, plan.whnf_eval_s, plan.apply_s);

    if (err != ENKI_ERROR_OK) {
        char msg_c[64];
        snprintf(msg_c, sizeof(msg_c), "plan error %d", err);
        _wisp_fail_with_val(rt, msg_c, plan.error_v);
    }

    return res_v;
}

// MARK: parser

static bool wisp_is_juxt(enki_value val_v)
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

static enki_value _wisp_quote(wisp_rt* rt, enki_value val_v)
{
    return enki_alloc_pair(rt->gc, 1, val_v);
}

static enki_value _wisp_parse_str(wisp_rt* rt, char** str_c)
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
    return _wisp_quote(rt, enki_alloc_strnat(rt->gc, sin_c, val_s));
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

static enki_value _wisp_parse_par(wisp_rt* rt, char** str_c)
{
    char* cur_c = *str_c;
    enki_value* stack_v = NULL;

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
    enki_value ret_v = enki_alloc_row(rt->gc, 0, stack_s, stack_v);
    arrfree(stack_v);
    return ret_v;
}

static enki_value _wisp_parse_seq(wisp_rt* rt, enki_value tag_v, char** str_c, char close_c)
{
    char* cur_c = *str_c;
    enki_value* stack_v = NULL;
    arrpush(stack_v, tag_v);

    while (1) {
        if (_wisp_take_close(rt, &cur_c, close_c)) {
            break;
        }
        arrpush(stack_v, wisp_parse(rt, &cur_c));
    }
    size_t stack_s = arrlen(stack_v);

    *str_c = cur_c;
    enki_value ret_v = enki_alloc_row(rt->gc, 0, stack_s, stack_v);
    arrfree(stack_v);
    return ret_v;
}

static enki_value _wisp_parse_seq_cur(wisp_rt* rt, char** str_c)
{
    return _wisp_parse_seq(rt, enki_alloc_cstrnat(rt->gc, "CURL"), str_c, '}');
}

static enki_value _wisp_parse_seq_bra(wisp_rt* rt, char** str_c)
{
    return _wisp_parse_seq(rt, enki_alloc_cstrnat(rt->gc, "BRAK"), str_c, ']');
}

static enki_value _wisp_parse_num(wisp_rt* rt, char* str_c, size_t str_s)
{
    // 10^18 < 2^63
    if (str_s < 18) {
        char* buf_c = ea_calloc(rt->loc_a, char, str_s);
        strncpy(buf_c, str_c, str_s);
        buf_c[str_s] = 0;
        return strtoll(buf_c, NULL, 10);
    } else {
        // XX: divide and conquer subquadratic
        enki_value res_v = 0;
        size_t cur_s = str_s - 1;
        uint64_t bas_q = 0;
        while (cur_s-- > 0) {
            char dig_b = str_c[cur_s] - '0';
            assert(dig_b < 10);
            enki_value add_v = enki_nat_mul(rt->gc, dig_b, bas_q);
            res_v = enki_nat_add(rt->gc, add_v, res_v);
        }
        return res_v;
    }
}

static enki_value _wisp_parse_sym(wisp_rt* rt, char** str_c)
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
        enki_value num_v = _wisp_parse_num(rt, sin_c, ret_s);
        return _wisp_quote(rt, num_v);
    }
    enki_value ret_v = enki_alloc_strnat(rt->gc, sin_c, ret_s);
    return ret_v;
}

static enki_value _wisp_parse_atom(wisp_rt* rt, char** str_c)
{
    enki_value sym_v = _wisp_parse_sym(rt, str_c);
    char* cur_c = *str_c;
    char_class nex_b = wisp_class(*cur_c);
    bool has_jux = false;
    enki_value jux_v = 0;

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
        return enki_alloc_quad(rt->gc, 0, enki_alloc_cstrnat(rt->gc, "JUXT"), sym_v, jux_v);
    }
    return sym_v;
}

enki_value wisp_parse(wisp_rt* rt, char** str_c)
{
    ENKI_PROFILE_ZONE("wisp_parse");
    char* cur_c = *str_c;
    if (wisp_eat(&cur_c)) {
        wisp_fail(rt, "Reached EOF");
    }
    enki_value ret;
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

static wisp_env_entry* wisp_getenv(wisp_rt* rt, enki_value key_v)
{
    if (IS_PTR(key_v)) {
        return NULL;
    }
    wisp_env_entry* ent = ea_calloc(rt->loc_a, wisp_env_entry, 1);
    if (enki_bst_get(rt->env->root, key_v, &ent->mac_f, &ent->val_v, NULL)) {
        return ent;
    }
    ea_free(rt->loc_a, ent);
    return NULL;
}

static void wisp_putenv(wisp_rt* rt, enki_value key_v, bool mac_f, enki_value val_v)
{
    wisp_env_entry* ent = ea_calloc(rt->loc_a, wisp_env_entry, 1);
    ent->mac_f = mac_f;
    ent->val_v = val_v;

    enki_bst_tree_put(rt->env, key_v, val_v, mac_f);
    // shput(rt->env, key_c, ent);
}

static enki_value wisp_expand_user(wisp_rt* rt, enki_value mac_v, enki_value val_v)
{
    UNUSED(rt);
    UNUSED(mac_v);
    UNUSED(val_v);
    return 0;
}

static enki_value wisp_pin(wisp_rt* rt, enki_value val_v)
{
    enki_value pin_v = enki_make_pin(rt->gc, wisp_eval(rt, val_v));
    return _wisp_quote(rt, pin_v);
}

static enki_value wisp_app(wisp_rt* rt, size_t exp_s, enki_value* exp_v)
{
    if (exp_s == 0) {
        return _wisp_quote(rt, 0);
    }

    enki_value* val_v = malloc(exp_s * sizeof(enki_value));
    if (val_v == NULL) {
        wisp_fail(rt, "oom");
    }

    for (size_t i = 0; i < exp_s; i++) {
        val_v[i] = wisp_eval(rt, exp_v[i]);
    }

    enki_value res_v = _wisp_plan_apply(rt, exp_s, val_v);
    free(val_v);
    return _wisp_quote(rt, res_v);
}

static void wisp_parse_bind(wisp_rt* rt, enki_value bin_v, enki_value* nam_v, enki_value* exp_v)
{
    enki_app* app = ENKI_TO_APP(bin_v);
    if (!app || app->n_args_s != 3 || !wisp_is_juxt(app->args_v[0]) || IS_PTR(app->args_v[1])) {
        _wisp_fail_with_val(rt, "bad bind", bin_v);
        return;
    }
    *nam_v = app->args_v[1];
    *exp_v = app->args_v[2];
}

typedef struct _wisp_local {
    enki_value nam_v;
    uint64_t idx_q;
    enki_value exp_v; // 0 if argument and no expression
} wisp_local;

enki_value wisp_eval(wisp_rt* rt, enki_value val_v);
static bool wisp_quote_payload(enki_value val_v, enki_value* out_v);
static bool wisp_law_quote_payload(enki_value val_v, enki_value* out_v);

static enki_value compile_expr(wisp_rt* rt, size_t loc_s, wisp_local* loc, enki_value val_v)
{
#define recur(x) compile_expr(rt, loc_s, loc, x)
#define law_quote(x) enki_alloc_pair(rt->gc, 0, x)
    if (val_v == 0) {
        return law_quote(0);
    }
    if (!IS_PTR(val_v)) {
        for (size_t i = 0; i < loc_s; i++) {
            if (loc[i].nam_v == val_v) {
                return loc[i].idx_q;
            }
        }
        wisp_env_entry* ent = wisp_getenv(rt, val_v);
        if (ent == NULL) {
            _wisp_fail_with_val(rt, "unbound", val_v);
            return 0;
        }
        return law_quote(ent->val_v);
    }
    enki_app* app;
    app = ENKI_TO_APP(val_v);

    enki_value payload_v;
    if (wisp_quote_payload(val_v, &payload_v)) {
        return law_quote(payload_v);
    }

    if (app == NULL || app->fn_v != 0) {
        return law_quote(val_v);
    }

    if (app->n_args_s == 0) {
        return law_quote(0);
    }

    enki_value ret = recur(app->args_v[0]);
    for (size_t i = 1; i < app->n_args_s; i++) {
        ret = enki_alloc_trel(rt->gc, 0, ret, recur(app->args_v[i]));
    }
    return ret;
#undef recur
#undef law_quote
}

static enki_value _wisp_macroexpand(wisp_rt* rt, size_t loc_s, wisp_local* loc, enki_value val_v);

static bool wisp_find_local(size_t loc_s, wisp_local* loc, enki_value nam_v, uint64_t* idx_q)
{
    for (size_t i = 0; i < loc_s; i++) {
        if (loc[i].nam_v == nam_v) {
            *idx_q = loc[i].idx_q;
            return true;
        }
    }
    return false;
}

static bool wisp_quote_payload(enki_value val_v, enki_value* out_v)
{
    enki_app* app = ENKI_TO_APP(val_v);
    if (app != NULL && app->fn_v == 1 && app->n_args_s == 1) {
        *out_v = app->args_v[0];
        return true;
    }
    return false;
}

static bool wisp_law_quote_payload(enki_value val_v, enki_value* out_v)
{
    enki_app* app = ENKI_TO_APP(val_v);
    if (app != NULL && app->fn_v == 0 && app->n_args_s == 1) {
        *out_v = app->args_v[0];
        return true;
    }
    return false;
}

static enki_value wisp_law(wisp_rt* rt, enki_value tag_v, enki_value sig_v, enki_value bod_v,
                           size_t bin_s,     // binders
                           enki_value* bin_v //
)
{
    enki_value teg_v = wisp_eval(rt, tag_v);
    enki_app* app = ENKI_TO_APP(sig_v);
    if (app == NULL || app->fn_v != 0 || app->n_args_s < 2) {
        _wisp_fail_with_val(rt, "bad law signature", sig_v);
        return 0;
    }
    enki_value nam_v = app->args_v[0];
    if (IS_PTR(nam_v)) {
        _wisp_fail_with_val(rt, "bad law name", nam_v);
        return 0;
    }

    size_t arg_s = app->n_args_s - 1;
    enki_value* arg_v = app->args_v + 1;

    size_t loc_s = arg_s + bin_s + 1;
    wisp_local* loc = ea_calloc(rt->loc_a, wisp_local, loc_s);
    loc[0].nam_v = nam_v;
    loc[0].idx_q = 0;
    loc[0].exp_v = 0;
    uint64_t i;
    for (i = 1; i <= arg_s; i++) {
        if (IS_PTR(arg_v[i - 1])) {
            _wisp_fail_with_val(rt, "bad law argument", arg_v[i - 1]);
            return 0;
        }
        loc[i].nam_v = arg_v[i - 1];
        loc[i].idx_q = i;
        loc[i].exp_v = 0;
    }
    for (; i < loc_s; i++) {
        size_t idx_s = i - arg_s - 1;
        loc[i].idx_q = i;
        wisp_parse_bind(rt, bin_v[idx_s], &loc[i].nam_v, &loc[i].exp_v);
    }
    enki_app* bin_exp = enki_alloc_app_bare(rt->gc, 0, bin_s);
    for (size_t j = 0; j < bin_s; j++) {
        bin_exp->args_v[j] = _wisp_macroexpand(rt, loc_s, loc, loc[j + arg_s + 1].exp_v);
        bin_exp->args_v[j] = compile_expr(rt, loc_s, loc, bin_exp->args_v[j]);
    }

    enki_value bod_exp_v = _wisp_macroexpand(rt, loc_s, loc, bod_v);
    bod_exp_v = compile_expr(rt, loc_s, loc, bod_exp_v);

    enki_value law = enki_law_alloc(rt->gc, arg_s, teg_v, bod_exp_v, 0, 0, NULL, NULL);

    return _wisp_quote(rt, law);
}

static enki_value wisp_bind(wisp_rt* rt, enki_value nam_v, enki_value val_v)
{
    // enki_value nem_v = wisp_eval(rt, nam_v);
    enki_value vel_v = wisp_eval(rt, val_v);
    // printf("binding %s to %s\n", enki_pvalue(nam_v), enki_pvalue(vel_v));
    wisp_putenv(rt, nam_v, false, vel_v);

    return nam_v;
}

static enki_value _wisp_expand1(wisp_rt* rt, size_t loc_s, wisp_local* loc, uint64_t mac_q,
                                enki_value val_v)
{
    if (!IS_PTR(val_v)) {
        goto fail;
    }
    enki_value_header* val = ENKI_TO_PTR(val_v);
    if (val->kind_b != ENKI_APP) {
        goto fail;
    }
    enki_app* app = (enki_app*)val;

    wisp_env_entry* ent = wisp_getenv(rt, mac_q);
    if (ent && ent->mac_f) {
        return wisp_expand_user(rt, ent->val_v, val_v);
    }

#define RECUR(x) _wisp_macroexpand(rt, loc_s, loc, x)
    switch (mac_q) {
    case MOTE_HBIND:
        if (app->n_args_s != 3) {
            _wisp_fail_with_val(rt, "invalid #bind", val_v);
            return 0;
        }
        return wisp_bind(rt, app->args_v[1], app->args_v[2]);
    case MOTE_HPIN:
        if (app->n_args_s != 2) {
            _wisp_fail_with_val(rt, "invalid #pin", val_v);
            return 0;
        }
        return wisp_pin(rt, app->args_v[1]);
    case MOTE_HLAW:
        if (app->n_args_s < 4) {
            _wisp_fail_with_val(rt, "invalid #law", val_v);
            return 0;
        }
        return wisp_law(rt, app->args_v[1], app->args_v[2], app->args_v[app->n_args_s - 1],
                        app->n_args_s - 4, &app->args_v[3]);
    case MOTE_HAPP:
        if (app->n_args_s < 2) {
            _wisp_fail_with_val(rt, "invalid #app", val_v);
            return 0;
        }
        enki_value* args_v = NULL;
        for (size_t i = 1; i < app->n_args_s; i++) {
            arrpush(args_v, RECUR(app->args_v[i]));
        }
        size_t args_s = arrlen(args_v);

        return wisp_app(rt, args_s, args_v);
    }
#undef RECUR
    _wisp_fail_with_val(rt, "unknown macro", mac_q);
    return 0;

fail:
    wisp_fail(rt, "expected row expanding macro");
    return 0;
}

static bool is_sys_macro(uint64_t mac_q)
{
    return ((mac_q == MOTE_HBIND) || (mac_q == MOTE_HLAW) || (mac_q == MOTE_HPIN) ||
            (mac_q == MOTE_HAPP) || (mac_q == MOTE_HEXPORT));
}

static enki_value _wisp_macroexpand(wisp_rt* rt, size_t loc_s, wisp_local* loc, enki_value val_v)
{
    ENKI_PROFILE_ZONE("_wisp_macroexpand");

    if (!IS_PTR(val_v)) {
        return val_v;
    }
    enki_value_header* val = ENKI_TO_PTR(val_v);
    if (val->kind_b != ENKI_APP) {
        return val_v;
    }

    enki_app* app = (enki_app*)val;
    // Quoted forms
    if (app->fn_v != 0) {
        return val_v;
    }

    if (app->n_args_s == 3 && wisp_is_juxt(app->args_v[0]) && app->args_v[1] == '#' && loc_s > 0) {
        enki_value inn_v = _wisp_macroexpand(rt, loc_s, loc, app->args_v[2]);
        return enki_alloc_quad(rt->gc, 0, app->args_v[0], '#', inn_v);
    }
    uint64_t local_idx_q = 0;
    bool head_is_local =
        !IS_PTR(app->args_v[0]) && wisp_find_local(loc_s, loc, app->args_v[0], &local_idx_q);
    wisp_env_entry* ent = wisp_getenv(rt, app->args_v[0]);
    if (!head_is_local && ((ent && ent->mac_f) || is_sys_macro(app->args_v[0]))) {
        return _wisp_expand1(rt, loc_s, loc, app->args_v[0], val_v);
    }

    enki_app* nex = enki_alloc_app_bare(rt->gc, 0, app->n_args_s);
    for (size_t i = 0; i < app->n_args_s; i++) {
        nex->args_v[i] = _wisp_macroexpand(rt, loc_s, loc, app->args_v[i]);
    }
    return PTR_TO_ENKI(nex);
}

static enki_value _wisp_apple(wisp_rt* rt, size_t val_s, enki_value* val_v)
{
    return _wisp_plan_apply(rt, val_s, val_v);
}

static enki_value _wisp_thunk(wisp_rt* rt, enki_value val_v)
{
    ENKI_PROFILE_ZONE("_wisp_thunk");
    if (val_v == 0) {
        return 0;
    }
    if (!IS_PTR(val_v)) {
        wisp_env_entry* ent = wisp_getenv(rt, val_v);
        if (!ent) {
            _wisp_fail_with_val(rt, "unbound thk", val_v);
            return 0;
        }
        return ent->val_v;
    }
    obj_header* h = (obj_header*)ENKI_TO_PTR(val_v);
    if (h->kind_b != ENKI_APP) {
        // _wisp_fail_with_val(rt, "not app in thunk", val_v);
        return val_v;
    }
    enki_app* app = (enki_app*)h;
    if (app->fn_v == 1 && app->n_args_s == 1) {
        return app->args_v[0];
    } else {
        // enki_app* nex = enki_alloc_app_bare(rt->gc, app->fn_v, app->n_args_s);
        enki_value* nex_v =
            ea_calloc(enki_arena_as_allocator(rt->i->scratch_a), enki_value, app->n_args_s);
        for (size_t i = 0; i < app->n_args_s; i++) {
            nex_v[i] = _wisp_thunk(rt, app->args_v[i]);
        }
        return _wisp_apple(rt, app->n_args_s, nex_v);

        // return PTR_TO_ENKI(nex);
    }
}

enki_value wisp_eval(wisp_rt* rt, enki_value val_v)
{
    ENKI_PROFILE_ZONE("wisp_eval");
    enki_value exp_v = _wisp_macroexpand(rt, 0, NULL, val_v);
    enki_value thk_v = _wisp_thunk(rt, exp_v);
    return thk_v;
}
