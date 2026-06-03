#include <enki/print.h>
#include <enki/string_builder.h>
#include <enki/util.h>

#include <ctype.h>
#include <gmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void enki_print_value_sb(const enki_allocator* cat_a, enki_string_builder* sb, er_val val_v);

static void enki_gmp_free(void* ptr, size_t size_s)
{
    if (ptr == NULL) {
        return;
    }
    void (*free_fn)(void*, size_t) = NULL;
    mp_get_memory_functions(NULL, NULL, &free_fn);
    free_fn(ptr, size_s);
}

static bool enki_buf_is_print(const char* buf_c, size_t buf_s)
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

static bool enki_direct_is_print(uint64_t dir_q)
{
    if (dir_q < 256) {
        return false;
    }
    while (dir_q > 0) {
        unsigned char byt_b = (unsigned char)(dir_q & 0xffu);
        if (!isprint(byt_b)) {
            return false;
        }
        dir_q >>= 8u;
    }
    return true;
}

static void enki_print_direct_text(enki_string_builder* sb, uint64_t dir_q)
{
    enki_sb_append_lit(sb, "\"");
    while (dir_q > 0) {
        enki_sb_append_char(sb, (char)(dir_q & 0xffu));
        dir_q >>= 8u;
    }
    enki_sb_append_lit(sb, "\"");
}

static void enki_print_direct_text_raw(enki_string_builder* sb, uint64_t dir_q)
{
    while (dir_q > 0) {
        enki_sb_append_char(sb, (char)(dir_q & 0xffu));
        dir_q >>= 8u;
    }
}

static size_t enki_bat_byte_size(const er_bat* bat)
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

static void enki_print_bat(const enki_allocator* cat_a, enki_string_builder* sb, const er_bat* bat)
{
    size_t byt_s = enki_bat_byte_size(bat);
    if (enki_buf_is_print((const char*)bat->lim_q, byt_s)) {
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
    enki_gmp_free(dec_c, dec_s + 1u);
    mpz_clear(z);
    UNUSED(cat_a);
}

static bool enki_print_law_name_raw(const enki_allocator* cat_a, enki_string_builder* sb,
                                    er_val name_v)
{
    if (er_is_cat(name_v)) {
        if (!enki_direct_is_print(name_v)) {
            return false;
        }
        enki_print_direct_text_raw(sb, name_v);
        return true;
    }

    er_bat* bat = er_outt(er_tag_bat, name_v);
    if (bat == NULL) {
        return false;
    }
    size_t byt_s = enki_bat_byte_size(bat);
    if (!enki_buf_is_print((const char*)bat->lim_q, byt_s)) {
        return false;
    }
    enki_sb_append_ref(sb, (const char*)bat->lim_q, byt_s);
    UNUSED(cat_a);
    return true;
}

static void enki_print_law(const enki_allocator* cat_a, enki_string_builder* sb, const er_law* law)
{
    enki_sb_append_lit(sb, "{");
    if (!enki_print_law_name_raw(cat_a, sb, law->name_v)) {
        enki_print_value_sb(cat_a, sb, law->name_v);
    }
    enki_sb_append_lit(sb, "/");
    enki_sb_append_u64(sb, law->ari_d);
    enki_sb_append_lit(sb, " ");
    enki_print_value_sb(cat_a, sb, law->body_v);
    enki_sb_append_lit(sb, "}");
}

static void enki_print_app(const enki_allocator* cat_a, enki_string_builder* sb, const er_app* app)
{
    enki_sb_append_lit(sb, "(");
    enki_print_value_sb(cat_a, sb, app->fn_v);
    for (size_t i = 0; i < app->arg_s; i++) {
        enki_sb_append_lit(sb, " ");
        enki_print_value_sb(cat_a, sb, app->arg_v[i]);
    }
    enki_sb_append_lit(sb, ")");
}

static void enki_print_value_sb(const enki_allocator* cat_a, enki_string_builder* sb, er_val val_v)
{
    if (er_is_cat(val_v)) {
        if (enki_direct_is_print(val_v)) {
            enki_print_direct_text(sb, val_v);
        } else {
            enki_sb_append_u64(sb, val_v);
        }
        return;
    }

    er_bat* bat = er_outt(er_tag_bat, val_v);
    if (bat != NULL) {
        enki_print_bat(cat_a, sb, bat);
        return;
    }

    er_app* app = er_outt(er_tag_app, val_v);
    if (app != NULL) {
        enki_print_app(cat_a, sb, app);
        return;
    }

    er_pin* pin = er_outt(er_tag_pin, val_v);
    if (pin != NULL) {
        enki_sb_append_lit(sb, "<");
        enki_print_value_sb(cat_a, sb, pin->val_v);
        enki_sb_append_lit(sb, ">");
        return;
    }

    er_law* law = er_outt(er_tag_law, val_v);
    if (law != NULL) {
        enki_print_law(cat_a, sb, law);
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

char* enki_print_value(const enki_allocator* cat_a, er_val val_v, size_t* out_s)
{
    size_t def_s = 0;
    if (out_s == NULL) {
        out_s = &def_s;
    }
    enki_string_builder sb;
    enki_sb_init(&sb, cat_a);
    enki_print_value_sb(cat_a, &sb, val_v);
    return enki_sb_build(&sb, out_s);
}

char* enki_log_value(
    const enki_allocator* cat_a,
    er_val val_v,
    size_t* out_s
)
{
  UNUSED(cat_a); UNUSED(val_v); UNUSED(out_s);
  return "";
  // size_t key_s = 0;
  // char* key_c = wisp_print_value(rt, val_v, &key_s);
  // size_t str_s = strlen(msg_c) + key_s + 5;
  // char* str_c;
  // assert(str_c = ea_calloc(loc_a, char, str_s));
  // if ( out_s != NULL ) {
  //   *out_s = str_s;
  // }
  //
  // snprintf(str_c, str_s, "%s: %s\n", msg_c, key_c);
  // return str_c;
}
