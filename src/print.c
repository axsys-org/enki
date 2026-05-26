
#include <enki/print.h>
#include <enki/string_builder.h>
#include <enki/util.h>

static void enki_print_value_sb(enki_allocator cat_a, enki_string_builder* sb, enki_value val_v);

static char buf_is_print(char* buf_c, size_t buf_s)
{
    for (size_t i = 0; i < buf_s; i++) {
        if (!isprint((unsigned char)buf_c[i])) {
            return false;
        }
    }
    return true;
}

static void enki_print_bat(enki_allocator cat_a, enki_string_builder* sb, enki_nat* nat)
{
    size_t byt_s = enki_bat_met_bytes((enki_value)nat);
    if (buf_is_print((char*)nat->limbs, byt_s)) {
        enki_sb_append_lit(sb, "\"");
        enki_sb_append_ref(sb, (const char*)nat->limbs, byt_s);
        enki_sb_append_lit(sb, "\"");
    } else {
        size_t buf_s = mpn_sizeinbase(nat->limbs, nat->n_limbs_s, 10); // upper bound, off by ≤1
        unsigned char* buf_c = ea_calloc(cat_a, unsigned char, buf_s);
        size_t str_s = mpn_get_str(buf_c, 10, nat->limbs, nat->n_limbs_s);
        for (size_t i = 0; i < str_s; i++) {
            buf_c[i] = buf_c[i] < 10 ? buf_c[i] + '0' : buf_c[i] - 10 + 'a';
        }
        enki_sb_append_ref(sb, (const char*)buf_c, str_s);
    }
}

static void enki_print_app(enki_allocator cat_a, enki_string_builder* sb, enki_app* app)
{
    enki_sb_append_lit(sb, "(");
    enki_print_value_sb(cat_a, sb, app->fn_v);
    for (size_t i = 0; i < app->n_args_s; i++) {
        enki_sb_append_lit(sb, " ");
        enki_print_value_sb(cat_a, sb, app->args_v[i]);
    }
    enki_sb_append_lit(sb, ")");
}

static bool direct_is_print(uint64_t dir_q)
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

static void enki_print_direct_text(enki_string_builder* sb, uint64_t dir_q)
{
    enki_sb_append_lit(sb, "\"");
    while (dir_q > 0) {
        enki_sb_append_char(sb, (char)(dir_q & 0xffu));
        dir_q >>= 8;
    }
    enki_sb_append_lit(sb, "\"");
}

static void enki_print_value_sb(enki_allocator cat_a, enki_string_builder* sb, enki_value val_v)
{
    if (!IS_PTR(val_v)) {
        if (direct_is_print(val_v)) {
            enki_print_direct_text(sb, val_v);
        } else {
            enki_sb_append_u64(sb, val_v);
        }
    } else {
        obj_header* h = ENKI_TO_PTR(val_v);
        switch (h->kind_b) {
        case ENKI_BIG_NAT:
            enki_print_bat(cat_a, sb, (enki_nat*)h);
            return;
        case ENKI_APP:
            enki_print_app(cat_a, sb, (enki_app*)h);
            return;
        case ENKI_PIN:
            enki_sb_append_lit(sb, "<");
            enki_print_value_sb(cat_a, sb, ((enki_pin*)h)->inner_v);
            enki_sb_append_lit(sb, ">");
            return;
        default:
            enki_sb_append_lit(sb, "<<>>");
            return;
        }
    }
}

char* enki_print_value(enki_allocator cat_a, enki_value val_v, size_t* out_s)
{
  size_t def_s = 0;
  if (out_s == NULL ) {
    out_s = &def_s;
  }
  enki_string_builder sb;
  enki_sb_init(&sb, cat_a);
  enki_print_value_sb(cat_a, &sb, val_v);
  return enki_sb_build(&sb, out_s);
}

// char* enki_print_nat(enki_allocator cat_a, enki_value val_v, size_t* out_s)
// {
//     if (!IS_PTR(val_v)) {
//         char* str_c = ea_alloc(cat_a, 20);
//         int ret_i;
//         if (ret_i = snprintf(str_c, 20, "%llu", val_v), ret_i < 0 ) {
//           die("failed to alloc str");
//         }
//         *out_s = (size_t)ret_i;
//         return str_c;
//     } else {
//         obj_header* h = ENKI_TO_PTR(val_v);
//         ea_assertf(h->kind_b == ENKI_BIG_NAT, "bad tag %u", h->kind_b);
//         enki_print_bat(cat_a, sb, (enki_nat*)h);
//             return;
//
// }
