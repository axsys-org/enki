
#include <enki/print.h>
#include <enki/string_builder.h>
#include <enki/util.h>

static void enki_print_value_sb(const enki_allocator* cat_a, enki_string_builder* sb, enki_value val_v);

static char buf_is_print(char* buf_c, size_t buf_s)
{
    for (size_t i = 0; i < buf_s; i++) {
        if (!isprint((unsigned char)buf_c[i])) {
          return false;
        }
    }
    return true;
}

static void enki_print_bat(const enki_allocator* cat_a, enki_string_builder* sb, enki_nat* nat)
{
    size_t byt_s = enki_bat_met_bytes(PTR_TO_ENKI(nat));
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

static void enki_print_app(const enki_allocator* cat_a, enki_string_builder* sb, enki_app* app)
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

static void enki_print_direct_text_raw(enki_string_builder* sb, uint64_t dir_q)
{
    while (dir_q > 0) {
        enki_sb_append_char(sb, (char)(dir_q & 0xffu));
        dir_q >>= 8;
    }
}

static bool enki_print_law_name_raw(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    enki_value name_v
) {
    if (!IS_PTR(name_v)) {
        if (!direct_is_print(name_v)) {
            return false;
        }
        enki_print_direct_text_raw(sb, name_v);
        return true;
    }

    obj_header* h = ENKI_TO_PTR(name_v);
    if (h->kind_b != ENKI_BIG_NAT) {
        return false;
    }

    enki_nat* nat = (enki_nat*)h;
    size_t byt_s = enki_bat_met_bytes(PTR_TO_ENKI(nat));
    if (!buf_is_print((char*)nat->limbs, byt_s)) {
        return false;
    }

    UNUSED(cat_a);
    enki_sb_append_ref(sb, (const char*)nat->limbs, byt_s);
    return true;
}

static const char law_names[] = {
  'a',
  'b',
  'c',
  'd',
  'e',
  'f',
  'g',
  'h',
  'i',
  'j',
  'k',
  'l',
  'm',
  'n',
  'o',
  'p',
  'q',
  'r',
  's',
  't',
  'u',
  'v',
  'w',
  'x',
  'y',
  'z',
};

static char* get_law_name(const enki_allocator* loc_a, size_t idx_o)
{
  uint8_t idx_b[4] = {0};
  size_t idx_s = 0;
  do {
    idx_b[idx_s] = (idx_o % 26);
    idx_o /= 26;
    idx_s++;
  } while (idx_o != 0 && idx_s < sizeof(idx_b));

  char* ret_c = ea_calloc(loc_a, char, idx_s + 1);
  for(size_t i = 0; i < idx_s; i++) {
    ret_c[i] = law_names[idx_b[i]];
  }
  ret_c[idx_s] = '\0';
  return ret_c;
}
static void print_calls(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    size_t dep_s,
    enki_value self_v,
    enki_value val_v
);

static void enki_print_law_local(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    enki_value self_v,
    size_t idx_s
) {
  if (idx_s == 0) {
    if (!enki_print_law_name_raw(cat_a, sb, self_v)) {
      enki_sb_append_lit(sb, "self");
    }
    return;
  }

  enki_sb_append_cstr(sb, get_law_name(cat_a, idx_s - 1));
}

static void flatten_calls(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    size_t dep_s,
    enki_value self_v,
    enki_app* app
) {
  enki_value cur = PTR_TO_ENKI(app);
  enki_app* hd = NULL;
  enki_value* arg_v = NULL;
  while ((hd = ENKI_TO_APP(cur)), hd != NULL && hd->fn_v == 0 && hd->n_args_s == 2) {
    arrpush(arg_v, hd->args_v[1]);
    cur = hd->args_v[0];
  }
  size_t len_s = arrlen(arg_v);
  enki_sb_append_lit(sb, "(");
  print_calls(cat_a, sb, dep_s, self_v, cur);
  for(size_t i = len_s; i > 0; i-- ) {
    enki_sb_append_lit(sb, " ");
    print_calls(cat_a, sb, dep_s, self_v, arg_v[i - 1]);
  }
  enki_sb_append_lit(sb, ")");
  arrfree(arg_v);

}


static void print_call_quote(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    enki_value val_v
) {
  enki_law* law = NULL;
  if ( !IS_PTR(val_v) ) {
    enki_print_value_sb(cat_a, sb, val_v);
    return;
  }
  obj_header* h = (obj_header*)(ENKI_TO_PTR(val_v));
  if ( h->kind_b == ENKI_LAW ) {
    law = (enki_law*)h;
    enki_sb_append_lit(sb, "@");
    enki_print_value_sb(cat_a, sb, law->name_v);
    return;
  } else if ( h->kind_b == ENKI_PIN ) {
    enki_pin* pin = (enki_pin*)h;

    if ( IS_PTR(pin->inner_v)
        && ((obj_header*)(ENKI_TO_PTR(pin->inner_v)))->kind_b == ENKI_LAW ) {
      enki_sb_append_lit(sb, "<");
      print_call_quote(cat_a, sb, pin->inner_v);
      enki_sb_append_lit(sb, ">");
      return;
    }
  }
  enki_print_value_sb(cat_a, sb, val_v);
}



static void print_calls(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    size_t dep_s,
    enki_value self_v,
    enki_value val_v
) {
  enki_app* app = NULL;
  if ( !IS_PTR(val_v) && val_v < dep_s ) {
    enki_print_law_local(cat_a, sb, self_v, val_v);
    return;
  }

  app = ENKI_TO_APP(val_v);
  if (app != NULL && app->fn_v == 0 && app->n_args_s == 1) {
    print_call_quote(cat_a, sb, app->args_v[0]);
  } else if (app != NULL && app->fn_v == 0 && app->n_args_s == 2) {
    flatten_calls(cat_a, sb, dep_s, self_v, app);
  } else {
    enki_sb_append_lit(sb, "#(");
    enki_print_value_sb(cat_a, sb, val_v);
    enki_sb_append_lit(sb, ")");
  }
}


static void enki_print_law_let(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    size_t idx_s,
    size_t dep_s,
    enki_value self_v,
    enki_value val_v
) {
  enki_print_law_local(cat_a, sb, self_v, idx_s);
  print_calls(cat_a, sb, dep_s, self_v, val_v);
}

static void enki_print_law(
    const enki_allocator* cat_a,
    enki_string_builder* sb,
    enki_law* law
) {
  enki_sb_append_lit(sb, "{");
  enki_print_value_sb(cat_a, sb, law->name_v);
  enki_sb_append_lit(sb, " ");
  size_t ari_s = law->arity_s;
  enki_sb_append_lit(sb, "(");
  enki_print_law_local(cat_a, sb, law->name_v, 0);

  for (size_t i = 1; i <= ari_s; i++) {
    enki_sb_append_lit(sb, " ");
    enki_print_law_local(cat_a, sb, law->name_v, i);
  }

  enki_sb_append_lit(sb, ")\n");

  enki_value cur_v = law->body_v;
  enki_app* app = NULL;
  enki_value* lets_v = NULL;

  while ( (app = ENKI_TO_APP(cur_v)) && app != NULL && app->fn_v == 1 ) {
    arrpush(lets_v, app->args_v[0]);
    cur_v = app->args_v[1];
  }
  size_t n_lets_s = (size_t)arrlen(lets_v);
  size_t dep_s = ari_s + 1 + n_lets_s;
  for(size_t i = 0; i < n_lets_s; i++ ) {
    size_t idx_s = ari_s + 1 + i;
    enki_print_law_let(cat_a, sb, idx_s, dep_s, law->name_v, lets_v[i]);
    enki_sb_append_lit(sb, "\n");
  }

  app = NULL;
  if ( (app = ENKI_TO_APP(cur_v)) && app != NULL && app->fn_v == 0 ) {
    print_calls( cat_a, sb, dep_s, law->name_v, cur_v);
  } else {
    enki_sb_append_lit(sb, "#(");
    enki_print_value_sb(cat_a, sb, cur_v);
    enki_sb_append_lit(sb, "#)");
  }
  enki_sb_append_lit(sb, "}");
  arrfree(lets_v);
}

static void enki_print_value_sb(const enki_allocator* cat_a, enki_string_builder* sb, enki_value val_v)
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
        case ENKI_IND:
            enki_print_value_sb(cat_a, sb, ((enki_ind*)h)->fn_v);
            return;
        case ENKI_PIN:
            enki_sb_append_lit(sb, "<");
            enki_print_value_sb(cat_a, sb, ((enki_pin*)h)->inner_v);
            enki_sb_append_lit(sb, ">");
            return;
        case ENKI_LAW:
            enki_print_law(cat_a, sb, (enki_law*)h);
            return;
        default:
            enki_sb_append_lit(sb, "<<>>");
            return;
        }
    }
}


char* enki_print_value(const enki_allocator* cat_a, enki_value val_v, size_t* out_s)
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
