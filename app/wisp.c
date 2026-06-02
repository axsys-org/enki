#include <enki/run.h>
#include <enki/wisp.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct boot_module {
    er_val key_v;
    wisp_env_entry* env;
    struct boot_module* next;
} boot_module;

typedef struct boot_ctx {
    const enki_allocator* loc_a;
    wisp_rt* rt;
    const char* src_dir_c;
    boot_module* mod_v;
    bool emit_top_level_f;
} boot_ctx;

static bool boot_is_nat(er_val val_v)
{
    return er_is_cat(val_v) || er_outt(er_tag_bat, val_v) != NULL;
}

static size_t boot_nat_limb_s(er_val val_v)
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

static uint64_t boot_nat_limb(er_val val_v, size_t idx_s)
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

static int boot_nat_cmp(er_val a_v, er_val b_v)
{
    size_t a_s = boot_nat_limb_s(a_v);
    size_t b_s = boot_nat_limb_s(b_v);
    if (a_s == SIZE_MAX || b_s == SIZE_MAX) {
        return a_v < b_v ? -1 : (a_v > b_v ? 1 : 0);
    }
    if (a_s != b_s) {
        return a_s < b_s ? -1 : 1;
    }
    for (size_t i = a_s; i > 0; i--) {
        uint64_t a_q = boot_nat_limb(a_v, i - 1u);
        uint64_t b_q = boot_nat_limb(b_v, i - 1u);
        if (a_q != b_q) {
            return a_q < b_q ? -1 : 1;
        }
    }
    return 0;
}

static bool boot_nat_eq(er_val a_v, er_val b_v)
{
    return boot_nat_cmp(a_v, b_v) == 0;
}

static er_val boot_bytes_nat(boot_ctx* ctx, const char* bytes_c, size_t bytes_s)
{
    if (bytes_s < sizeof(uint64_t)) {
        uint64_t out_q = 0;
        for (size_t i = 0; i < bytes_s; i++) {
            out_q |= ((uint64_t)(uint8_t)bytes_c[i]) << (i * 8u);
        }
        return out_q;
    }

    size_t limb_s = (bytes_s + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
    uint64_t* limb_q = ctx->loc_a->alloc(ctx->loc_a->ctx, limb_s * sizeof(uint64_t));
    if (limb_q == NULL) {
        return 0;
    }
    memset(limb_q, 0, limb_s * sizeof(uint64_t));
    memcpy(limb_q, bytes_c, bytes_s);

    er_bat* bat = er_bat_alloc(ctx->loc_a, limb_s);
    if (bat == NULL) {
        ctx->loc_a->free(ctx->loc_a->ctx, limb_q);
        return 0;
    }
    er_val out_v = er_bat_init(bat, limb_s, limb_q);
    ctx->loc_a->free(ctx->loc_a->ctx, limb_q);
    return out_v;
}

static er_val boot_string_nat(boot_ctx* ctx, const char* str_c)
{
    return boot_bytes_nat(ctx, str_c, strlen(str_c));
}

static size_t boot_nat_byte_s(er_val val_v)
{
    if (er_is_cat(val_v)) {
        size_t bytes_s = 0;
        while (val_v != 0) {
            bytes_s++;
            val_v >>= 8u;
        }
        return bytes_s;
    }

    er_bat* bat = er_outt(er_tag_bat, val_v);
    if (bat == NULL || bat->lim_s == 0) {
        return 0;
    }

    size_t bytes_s = (bat->lim_s - 1u) * sizeof(uint64_t);
    uint64_t top_q = bat->lim_q[bat->lim_s - 1u];
    while (top_q != 0) {
        bytes_s++;
        top_q >>= 8u;
    }
    return bytes_s;
}

static char* boot_nat_string(boot_ctx* ctx, er_val val_v, size_t* out_s)
{
    size_t bytes_s = boot_nat_byte_s(val_v);
    char* str_c = ctx->loc_a->alloc(ctx->loc_a->ctx, bytes_s + 1u);
    if (str_c == NULL) {
        return NULL;
    }

    if (er_is_cat(val_v)) {
        for (size_t i = 0; i < bytes_s; i++) {
            str_c[i] = (char)(val_v & 0xffu);
            val_v >>= 8u;
        }
    } else {
        er_bat* bat = er_outt(er_tag_bat, val_v);
        if (bat != NULL && bytes_s != 0) {
            memcpy(str_c, bat->lim_q, bytes_s);
        }
    }

    str_c[bytes_s] = '\0';
    if (out_s != NULL) {
        *out_s = bytes_s;
    }
    return str_c;
}

static wisp_env_entry* boot_env_find(wisp_env_entry* env, er_val key_v)
{
    if (!boot_is_nat(key_v)) {
        return NULL;
    }
    for (wisp_env_entry* ent = env; ent != NULL; ent = ent->next) {
        if (boot_nat_eq(ent->key_v, key_v)) {
            return ent;
        }
    }
    return NULL;
}

static wisp_env_entry* boot_env_clone(boot_ctx* ctx, wisp_env_entry* env)
{
    wisp_env_entry* head = NULL;
    wisp_env_entry** tail = &head;
    for (wisp_env_entry* ent = env; ent != NULL; ent = ent->next) {
        wisp_env_entry* copy = ctx->loc_a->alloc(ctx->loc_a->ctx, sizeof(wisp_env_entry));
        if (copy == NULL) {
            return NULL;
        }
        *copy = (wisp_env_entry){
            .key_v = ent->key_v,
            .val_v = ent->val_v,
            .mac_f = ent->mac_f,
            .next = NULL,
        };
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

static bool boot_env_put(boot_ctx* ctx, wisp_env_entry** env, wisp_env_entry* src)
{
    wisp_env_entry* old = boot_env_find(*env, src->key_v);
    if (old != NULL) {
        old->val_v = src->val_v;
        old->mac_f = src->mac_f;
        return true;
    }

    wisp_env_entry* copy = ctx->loc_a->alloc(ctx->loc_a->ctx, sizeof(wisp_env_entry));
    if (copy == NULL) {
        return false;
    }
    *copy = (wisp_env_entry){
        .key_v = src->key_v,
        .val_v = src->val_v,
        .mac_f = src->mac_f,
        .next = *env,
    };
    *env = copy;
    return true;
}

static wisp_env_entry* boot_env_merge(boot_ctx* ctx, wisp_env_entry* old, wisp_env_entry* new_env)
{
    wisp_env_entry* out = boot_env_clone(ctx, old);
    if (old != NULL && out == NULL) {
        return NULL;
    }
    for (wisp_env_entry* ent = new_env; ent != NULL; ent = ent->next) {
        if (!boot_env_put(ctx, &out, ent)) {
            return NULL;
        }
    }
    return out;
}

static boot_module* boot_module_find(boot_ctx* ctx, er_val key_v)
{
    for (boot_module* mod = ctx->mod_v; mod != NULL; mod = mod->next) {
        if (boot_nat_eq(mod->key_v, key_v)) {
            return mod;
        }
    }
    return NULL;
}

static bool boot_module_put(boot_ctx* ctx, er_val key_v, wisp_env_entry* env)
{
    boot_module* mod = ctx->loc_a->alloc(ctx->loc_a->ctx, sizeof(boot_module));
    if (mod == NULL) {
        return false;
    }
    *mod = (boot_module){
        .key_v = key_v,
        .env = env,
        .next = ctx->mod_v,
    };
    ctx->mod_v = mod;
    return true;
}

static bool boot_eat(char** str_c)
{
    while (true) {
        if (**str_c == ';') {
            while (**str_c != '\n' && **str_c != 0) {
                (*str_c)++;
            }
        }
        if (**str_c == ' ' || **str_c == '\n') {
            (*str_c)++;
            continue;
        }
        return **str_c == 0;
    }
}

static bool boot_ok_file_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static bool boot_ok_module_name(const char* mod_c)
{
    if (mod_c == NULL || *mod_c == '\0') {
        return false;
    }
    for (const char* cur_c = mod_c; *cur_c != '\0'; cur_c++) {
        if (!boot_ok_file_char(*cur_c)) {
            return false;
        }
    }
    return true;
}

static char* boot_read_include(boot_ctx* ctx, er_val form_v)
{
    if (!boot_is_nat(form_v)) {
        return NULL;
    }

    size_t str_s = 0;
    char* str_c = boot_nat_string(ctx, form_v, &str_s);
    if (str_c == NULL) {
        return NULL;
    }
    if (str_s < 2 || str_c[0] != '@') {
        ctx->loc_a->free(ctx->loc_a->ctx, str_c);
        return NULL;
    }
    for (size_t i = 1; i < str_s; i++) {
        if (!boot_ok_file_char(str_c[i])) {
            ctx->loc_a->free(ctx->loc_a->ctx, str_c);
            return NULL;
        }
    }

    size_t mod_s = str_s - 1u;
    char* mod_c = ctx->loc_a->alloc(ctx->loc_a->ctx, mod_s + 1u);
    if (mod_c == NULL) {
        ctx->loc_a->free(ctx->loc_a->ctx, str_c);
        return NULL;
    }
    memcpy(mod_c, str_c + 1, mod_s);
    mod_c[mod_s] = '\0';
    ctx->loc_a->free(ctx->loc_a->ctx, str_c);
    return mod_c;
}

static char* boot_module_path(boot_ctx* ctx, const char* mod_c)
{
    size_t dir_s = strlen(ctx->src_dir_c);
    size_t mod_s = strlen(mod_c);
    size_t path_s = dir_s + 1u + mod_s + 5u;
    char* path_c = ctx->loc_a->alloc(ctx->loc_a->ctx, path_s + 1u);
    if (path_c == NULL) {
        return NULL;
    }
    (void)snprintf(path_c, path_s + 1u, "%s/%s.plan", ctx->src_dir_c, mod_c);
    return path_c;
}

static char* boot_read_file(boot_ctx* ctx, const char* path_c)
{
    FILE* file = fopen(path_c, "rb");
    if (file == NULL) {
        fprintf(stderr, "wisp: failed to open %s\n", path_c);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        fprintf(stderr, "wisp: failed to seek %s\n", path_c);
        return NULL;
    }
    long file_s_l = ftell(file);
    if (file_s_l < 0) {
        fclose(file);
        fprintf(stderr, "wisp: failed to size %s\n", path_c);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        fprintf(stderr, "wisp: failed to rewind %s\n", path_c);
        return NULL;
    }

    size_t file_s = (size_t)file_s_l;
    char* text_c = ctx->loc_a->alloc(ctx->loc_a->ctx, file_s + 1u);
    if (text_c == NULL) {
        fclose(file);
        return NULL;
    }

    size_t read_s = fread(text_c, 1, file_s, file);
    fclose(file);
    if (read_s != file_s) {
        ctx->loc_a->free(ctx->loc_a->ctx, text_c);
        fprintf(stderr, "wisp: failed to read %s\n", path_c);
        return NULL;
    }
    text_c[file_s] = '\0';
    return text_c;
}

static bool boot_process_file(boot_ctx* ctx, const char* mod_c);

static bool boot_process_form(boot_ctx* ctx, er_val form_v)
{
    char* inc_c = boot_read_include(ctx, form_v);
    if (inc_c != NULL) {
        bool ok_f = boot_process_file(ctx, inc_c);
        ctx->loc_a->free(ctx->loc_a->ctx, inc_c);
        return ok_f;
    }

    er_val exp_v = wisp_macroexpand(ctx->rt, form_v);
    if (exp_v == 0) {
        return true;
    }

    er_val out_v = wisp_thunk(ctx->rt, exp_v);
    if (ctx->emit_top_level_f) {
        char* out_c = wisp_print_value(ctx->rt, out_v, NULL);
        if (out_c == NULL) {
            return false;
        }
        fprintf(stderr, "%s\n", out_c);
        ctx->loc_a->free(ctx->loc_a->ctx, out_c);
    }
    return true;
}

static wisp_env_entry* boot_load_module(boot_ctx* ctx, const char* mod_c)
{
    if (!boot_ok_module_name(mod_c)) {
        fprintf(stderr, "wisp: bad path: %s\n", mod_c);
        return NULL;
    }

    er_val key_v = boot_string_nat(ctx, mod_c);
    boot_module* cached = boot_module_find(ctx, key_v);
    if (cached != NULL) {
        return boot_env_clone(ctx, cached->env);
    }

    char* path_c = boot_module_path(ctx, mod_c);
    if (path_c == NULL) {
        return NULL;
    }
    char* text_c = boot_read_file(ctx, path_c);
    ctx->loc_a->free(ctx->loc_a->ctx, path_c);
    if (text_c == NULL) {
        return NULL;
    }

    ctx->rt->env = NULL;
    char* cur_c = text_c;
    while (!boot_eat(&cur_c)) {
        er_val form_v = wisp_parse(ctx->rt, &cur_c);
        if (!boot_process_form(ctx, form_v)) {
            ctx->loc_a->free(ctx->loc_a->ctx, text_c);
            return NULL;
        }
    }
    ctx->loc_a->free(ctx->loc_a->ctx, text_c);

    wisp_env_entry* module_env = boot_env_clone(ctx, ctx->rt->env);
    if (ctx->rt->env != NULL && module_env == NULL) {
        return NULL;
    }
    wisp_env_entry* cached_env = boot_env_clone(ctx, module_env);
    if (module_env != NULL && cached_env == NULL) {
        return NULL;
    }
    if (!boot_module_put(ctx, key_v, cached_env)) {
        return NULL;
    }
    return module_env;
}

static bool boot_process_file(boot_ctx* ctx, const char* mod_c)
{
    er_val key_v = boot_string_nat(ctx, mod_c);
    wisp_env_entry* old_env = boot_env_clone(ctx, ctx->rt->env);
    if (ctx->rt->env != NULL && old_env == NULL) {
        return false;
    }

    wisp_env_entry* mod_env = boot_load_module(ctx, mod_c);
    if (mod_env == NULL && boot_module_find(ctx, key_v) == NULL) {
        return false;
    }

    wisp_env_entry* merged = boot_env_merge(ctx, old_env, mod_env);
    if ((old_env != NULL || mod_env != NULL) && merged == NULL) {
        return false;
    }
    ctx->rt->env = merged;
    return true;
}

static er_val boot_make_row(boot_ctx* ctx, int argc, char** argv)
{
    if (argc == 0) {
        return 0;
    }

    er_val* arg_v = ctx->loc_a->alloc(ctx->loc_a->ctx, (size_t)argc * sizeof(er_val));
    if (arg_v == NULL) {
        return 0;
    }
    for (int i = 0; i < argc; i++) {
        arg_v[i] = boot_string_nat(ctx, argv[i]);
    }

    er_app* app = er_app_alloc(ctx->loc_a, (size_t)argc);
    if (app == NULL) {
        ctx->loc_a->free(ctx->loc_a->ctx, arg_v);
        return 0;
    }
    er_val row_v = er_app_init(app, 0, (size_t)argc, arg_v);
    ctx->loc_a->free(ctx->loc_a->ctx, arg_v);
    return row_v;
}

static er_val boot_repl_fun(er_val fun_v)
{
    while (true) {
        er_pin* pin = er_outt(er_tag_pin, fun_v);
        if (pin == NULL || er_outt(er_tag_law, pin->val_v) != NULL) {
            return fun_v;
        }
        fun_v = pin->val_v;
    }
}

static bool boot_run_function(boot_ctx* ctx, er_val fun_v, int argc, char** argv)
{
    wisp_env_entry* old_env = ctx->rt->env;
    ctx->rt->env = NULL;

    er_val arg_row_v = boot_make_row(ctx, argc, argv);
    if (argc != 0 && arg_row_v == 0) {
        ctx->rt->env = old_env;
        return false;
    }

    er_val call_arg_v[] = {boot_repl_fun(fun_v), arg_row_v};
    er_thk* thk = er_thk_alloc(ctx->loc_a, 2);
    if (thk == NULL) {
        ctx->rt->env = old_env;
        return false;
    }
    er_val call_v = er_thk_init(thk, ER_XUNK_APP, 2, call_arg_v);
    if (call_v == 0) {
        ctx->rt->env = old_env;
        return false;
    }
    er_val out_v = er_eval(ctx->loc_a, call_v);
    ctx->rt->env = old_env;
    if (out_v == er_bad) {
        fprintf(stderr, "wisp: runtime error\n");
        return false;
    }
    return true;
}

static bool boot_load_assembly(boot_ctx* ctx, const char* mod_c, const char* fn_c, int argc,
                               char** argv)
{
    ctx->rt->env = NULL;
    if (!boot_process_file(ctx, mod_c)) {
        return false;
    }
    if (fn_c == NULL) {
        return true;
    }

    er_val fn_v = boot_string_nat(ctx, fn_c);
    wisp_env_entry* ent = boot_env_find(ctx->rt->env, fn_v);
    if (ent == NULL) {
        fprintf(stderr, "wisp: program unbound: %s\n", fn_c);
        return false;
    }
    return boot_run_function(ctx, ent->val_v, argc, argv);
}

static void boot_usage(const char* argv0_c)
{
    fprintf(stderr, "usage: %s DIR MODULE [FUNCTION ARGS...]\n", argv0_c);
}

int main(int argc, char** argv)
{
    if (argc != 3 && argc < 4) {
        boot_usage(argv[0]);
        return 2;
    }

    const enki_allocator* loc_a = enki_allocator_system();
    wisp_rt* rt = wisp_rt_alloc(loc_a);
    if (rt == NULL) {
        fprintf(stderr, "wisp: oom\n");
        return 1;
    }

    boot_ctx ctx = {
        .loc_a = loc_a,
        .rt = rt,
        .src_dir_c = argv[1],
        .mod_v = NULL,
        .emit_top_level_f = true,
    };

    rt->err_f = true;
    if (setjmp(rt->errjmp) != 0) {
        fprintf(stderr, "wisp: %s\n", rt->msg_c == NULL ? "unknown error" : rt->msg_c);
        wisp_rt_free(loc_a, rt);
        return 1;
    }

    const char* fn_c = argc >= 4 ? argv[3] : NULL;
    int run_argc = argc >= 5 ? argc - 4 : 0;
    char** run_argv = argc >= 5 ? argv + 4 : NULL;
    bool ok_f = boot_load_assembly(&ctx, argv[2], fn_c, run_argc, run_argv);
    wisp_rt_free(loc_a, rt);
    return ok_f ? 0 : 1;
}
