#include "enki/allocator.h"
#include "enki/gc.h"
#include "enki/arena.h"
#include "enki/print.h"
#include "enki/run.h"

#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>

static enki_arena* fixture_arena;
static const enki_allocator* fixture_allocator;
static enki_gc* fixture_gc;

static void setup(void) {
  fixture_arena = enki_arena_create(enki_allocator_system(), 1024 * 1024);
  cr_assert_not_null(fixture_arena);
  fixture_allocator = enki_arena_as_allocator(fixture_arena);
  cr_assert_not_null(fixture_allocator);
  fixture_gc = enki_gc_create(enki_allocator_system(), 1024 * 1024, NULL);
  cr_assert_not_null(fixture_gc);
}

static void teardown(void) {
  enki_gc_destroy(fixture_gc);
  fixture_gc = NULL;
  enki_arena_destroy(fixture_arena);
  fixture_arena = NULL;
  fixture_allocator = NULL;
}

static void assert_prints(er_val value_v, const char* expected_c) {
  size_t expected_s = strlen(expected_c);
  size_t out_s = 0;
  char* out_c = enki_print_value(fixture_allocator, value_v, &out_s);

  cr_assert_not_null(out_c);
  cr_assert_eq(out_s, expected_s, "want <%s>, have <%s>", expected_c, out_c);
  cr_assert_eq(memcmp(out_c, expected_c, expected_s), 0, "want <%s>, have <%s>",
               expected_c, out_c);
  cr_assert_eq(out_c[out_s], '\0');
}

static er_val make_bat(size_t lim_s, const uint64_t* lim_q) {
  er_bat* bat = er_bat_alloc(fixture_gc, lim_s);
  cr_assert_not_null(bat);
  er_val bat_v = er_bat_init(bat, lim_s, lim_q);
  cr_assert_eq(er_get_tag(bat_v), er_tag_bat);
  return bat_v;
}

static er_val make_bat_bytes(const char* bytes_c, size_t bytes_s) {
  uint64_t limbs_q[4] = {0};
  size_t limb_s = (bytes_s + sizeof(uint64_t) - 1u) / sizeof(uint64_t);

  cr_assert_gt(bytes_s, 0);
  cr_assert_leq(limb_s, sizeof(limbs_q) / sizeof(limbs_q[0]));

  memcpy(limbs_q, bytes_c, bytes_s);
  return make_bat(limb_s, limbs_q);
}

static er_val make_app(er_val fn_v, size_t arg_s, const er_val* arg_v) {
  er_app* app = er_app_alloc(fixture_gc, arg_s);
  cr_assert_not_null(app);
  er_val app_v = er_app_init(app, fn_v, arg_s, arg_v);
  cr_assert_eq(er_get_tag(app_v), er_tag_app);
  return app_v;
}

static er_val make_pin(er_val value_v) {
  er_pin* pin = er_pin_alloc(fixture_gc, 0);
  cr_assert_not_null(pin);
  er_val pin_v = er_pin_init(fixture_gc, pin, NULL, value_v, 0, NULL);
  cr_assert_eq(er_get_tag(pin_v), er_tag_pin);
  return pin_v;
}

static er_val make_law(uint32_t arity_d, er_val name_v, er_val body_v) {
  er_op op_v[] = {{.tag = OP_RET}};
  er_op* labels_v[] = {op_v};
  size_t label_len_v[] = {1};
  er_law* law = er_law_alloc(fixture_gc, 1, 1);
  cr_assert_not_null(law);
  er_val law_v =
      er_law_init(law, name_v, body_v, arity_d, 0, 1, labels_v, label_len_v);
  cr_assert_eq(er_get_tag(law_v), er_tag_law);
  return law_v;
}

static er_val make_thunk(er_execf fun) {
  er_thk* thk = er_thk_alloc(fixture_gc, 0);
  cr_assert_not_null(thk);
  er_val thk_v = er_thk_init(thk, fun, 0, NULL);
  cr_assert_eq(er_get_tag(thk_v), er_tag_thk);
  return thk_v;
}

TestSuite(print, .init = setup, .fini = teardown);

Test(print, cat_nats_and_packed_text) {
  assert_prints(0, "0");
  assert_prints(42, "42");
  assert_prints((UINT64_C(1) << 63) - 1u, "9223372036854775807");
  assert_prints(PLAN_S5('h', 'e', 'l', 'l', 'o'), "\"hello\"");
}

Test(print, printable_bats_print_as_quoted_text) {
  assert_prints(make_bat_bytes("hello", strlen("hello")), "\"hello\"");
  assert_prints(make_bat_bytes("ABCDEFGHIJKLMNOP", strlen("ABCDEFGHIJKLMNOP")),
                "\"ABCDEFGHIJKLMNOP\"");
}

Test(print, non_printable_bat_prints_as_decimal_text) {
  uint64_t limbs_q[] = {UINT64_C(1) << 63u};

  assert_prints(make_bat(1, limbs_q), "9223372036854775808");
}

Test(print, apps_print_function_and_arguments_in_parentheses) {
  er_val no_args_v = make_app(7, 0, NULL);
  er_val args_v[] = {2, 3, 4};
  er_val app_v = make_app(1, 3, args_v);

  assert_prints(no_args_v, "(7)");
  assert_prints(app_v, "(1 2 3 4)");
}

Test(print, nested_apps_recurse) {
  er_val inner_args_v[] = {9};
  er_val inner_v = make_app(8, 1, inner_args_v);
  er_val outer_args_v[] = {inner_v};
  er_val outer_v = make_app(7, 1, outer_args_v);

  assert_prints(outer_v, "(7 (8 9))");
}

Test(print, pins_wrap_printed_inner_value_in_angle_brackets) {
  er_val pin_v = make_pin(42);
  er_val nested_pin_v = make_pin(pin_v);

  assert_prints(pin_v, "<42>");
  assert_prints(nested_pin_v, "<<42>>");
}

Test(print, laws_print_name_arity_and_body) {
  assert_prints(make_law(0, 7, 42), "{7/0 42}");
  assert_prints(make_law(3, PLAN_S3('a', 'd', 'd'), 42), "{add/3 42}");
  assert_prints(make_law(1, make_bat_bytes("longname", strlen("longname")), 42),
                "{longname/1 42}");
}

Test(print, thunks_and_tanks_print_debug_placeholders) {
  assert_prints(make_thunk(ER_CALL), "<thk/2>");
  assert_prints(er_tank_make(fixture_gc, 42, "boom"), "<tank: boom>");
}

Test(print, bad_and_unknown_heap_tags_print_bad_placeholder) {
  assert_prints(er_bad, "<<bad>>");
  assert_prints(er_into(er_tag_fwd, 0), "<<bad>>");
}
