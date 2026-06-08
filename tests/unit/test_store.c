#include "test_interp.h"
#include "enki/error.h"
#include "enki/store.h"

#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>

static enki_interpreter* fixture_interp;

static void setup(void) {
  fixture_interp = enki_test_interp_create(1024 * 1024, 0);
  cr_assert_not_null(fixture_interp);
}

static void teardown(void) {
  enki_test_interp_destroy(fixture_interp);
  fixture_interp = NULL;
}

TestSuite(store, .init = setup, .fini = teardown);

Test(store, write_size_and_read_roundtrip) {
  uint8_t key_b[32] = {1};
  uint8_t bytes_b[] = {4, 5, 6, 7};
  uint8_t out_b[4] = {0};
  size_t len_s = 0;

  cr_assert_eq(fixture_interp->store.write(&fixture_interp->store, key_b,
                                           bytes_b, sizeof(bytes_b)),
               ENKI_ERROR_OK);
  cr_assert_eq(
      fixture_interp->store.size(&fixture_interp->store, key_b, &len_s),
      ENKI_ERROR_OK);
  cr_assert_eq(len_s, sizeof(bytes_b));
  cr_assert_eq(fixture_interp->store.read(&fixture_interp->store, key_b, out_b,
                                          sizeof(out_b), &len_s),
               ENKI_ERROR_OK);
  cr_assert_eq(len_s, sizeof(bytes_b));
  cr_assert_eq(memcmp(out_b, bytes_b, sizeof(bytes_b)), 0);
}

Test(store, missing_key_reports_not_found) {
  uint8_t key_b[32] = {2};
  uint8_t out_b[4] = {0};
  size_t len_s = 99;

  cr_assert_eq(
      fixture_interp->store.size(&fixture_interp->store, key_b, &len_s),
      ENKI_STORE_NOT_FOUND);
  cr_assert_eq(len_s, 0);
  cr_assert_eq(fixture_interp->store.read(&fixture_interp->store, key_b, out_b,
                                          sizeof(out_b), &len_s),
               ENKI_STORE_NOT_FOUND);
  cr_assert_eq(len_s, 0);
}

Test(store, read_reports_too_small_and_required_size) {
  uint8_t key_b[32] = {3};
  uint8_t bytes_b[] = {8, 9, 10, 11};
  uint8_t out_b[2] = {0};
  size_t len_s = 0;

  cr_assert_eq(fixture_interp->store.write(&fixture_interp->store, key_b,
                                           bytes_b, sizeof(bytes_b)),
               ENKI_ERROR_OK);
  cr_assert_eq(fixture_interp->store.read(&fixture_interp->store, key_b, out_b,
                                          sizeof(out_b), &len_s),
               ENKI_STORE_TOO_SMALL);
  cr_assert_eq(len_s, sizeof(bytes_b));
}

static er_val er_test_app2(enki_gc* gc, er_val fn_v, er_val a_v, er_val b_v) {
  er_val args_v[] = {a_v, b_v};
  er_app* app = er_app_alloc(gc, 2);
  cr_assert_not_null(app);
  er_val app_v = er_app_init(app, fn_v, 2, args_v);
  cr_assert_eq(er_get_tag(app_v), er_tag_app);
  return app_v;
}

static er_val er_test_bat(enki_gc* gc) {
  uint64_t limbs_q[] = {0, 1};
  er_bat* bat = er_bat_alloc(gc, 2);
  cr_assert_not_null(bat);
  er_val bat_v = er_bat_init(bat, 2, limbs_q);
  cr_assert_eq(er_get_tag(bat_v), er_tag_bat);
  return bat_v;
}

Test(store, er_pin_save_and_load_roundtrip_new_value_rep) {
  enki_gc* gc = fixture_interp->gc;
  const enki_allocator* work_a = enki_gc_parent_allocator(gc);
  er_val child_v = er_pin_make(gc, 11);
  cr_assert_eq(er_get_tag(child_v), er_tag_pin);

  er_val big_v = er_test_bat(gc);
  er_val inner_v = er_test_app2(gc, 77, child_v, big_v);
  er_val parent_v = er_pin_make(gc, inner_v);
  cr_assert_eq(er_get_tag(parent_v), er_tag_pin);

  uint8_t hash_b[32];
  cr_assert_eq(
      er_store_save_pin(&fixture_interp->store, gc, work_a, parent_v, hash_b),
      ENKI_ERROR_OK);

  er_val loaded_v = 0;
  cr_assert_eq(
      er_store_load_pin(&fixture_interp->store, gc, work_a, hash_b, &loaded_v),
      ENKI_ERROR_OK);

  er_pin* loaded = er_outt(er_tag_pin, loaded_v);
  cr_assert_not_null(loaded);
  cr_assert_eq(loaded->sub_s, 1);
  cr_assert_eq(memcmp(loaded->hash_b, hash_b, 32), 0);

  er_pin* loaded_child = er_outt(er_tag_pin, loaded->sub_v[0]);
  cr_assert_not_null(loaded_child);
  cr_assert_eq(loaded_child->val_v, 11);

  er_app* inner = er_outt(er_tag_app, loaded->val_v);
  cr_assert_not_null(inner);
  cr_assert_eq(inner->fn_v, 77);
  cr_assert_eq(inner->arg_s, 2);

  er_pin* child_ref = er_outt(er_tag_pin, inner->arg_v[0]);
  cr_assert_not_null(child_ref);
  cr_assert_eq(memcmp(child_ref->hash_b, loaded_child->hash_b, 32), 0);

  er_bat* loaded_big = er_outt(er_tag_bat, inner->arg_v[1]);
  cr_assert_not_null(loaded_big);
  cr_assert_eq(loaded_big->lim_s, 2);
  cr_assert_eq(loaded_big->lim_q[0], 0);
  cr_assert_eq(loaded_big->lim_q[1], 1);
}

Test(store, er_root_save_and_load_roundtrip_new_value_rep) {
  enki_gc* gc = fixture_interp->gc;
  const enki_allocator* work_a = enki_gc_parent_allocator(gc);
  er_val pin_v = er_pin_make(gc, 1234);
  cr_assert_eq(er_store_save_root(&fixture_interp->store, gc, work_a, pin_v),
               ENKI_ERROR_OK);

  er_val loaded_v = 0;
  cr_assert_eq(
      er_store_load_root(&fixture_interp->store, gc, work_a, &loaded_v),
      ENKI_ERROR_OK);

  er_pin* loaded = er_outt(er_tag_pin, loaded_v);
  cr_assert_not_null(loaded);
  cr_assert_eq(loaded->val_v, 1234);
}

Test(store, rejects_invalid_arguments) {
  uint8_t key_b[32] = {4};
  uint8_t bytes_b[] = {1};
  size_t len_s = 0;

  cr_assert_eq(enki_store_size(NULL, key_b, &len_s), ENKI_STORE_ERROR);
  cr_assert_eq(enki_store_size(&fixture_interp->store, NULL, &len_s),
               ENKI_STORE_ERROR);
  cr_assert_eq(enki_store_size(&fixture_interp->store, key_b, NULL),
               ENKI_STORE_ERROR);
  cr_assert_eq(enki_store_read(NULL, key_b, bytes_b, sizeof(bytes_b), &len_s),
               ENKI_STORE_ERROR);
  cr_assert_eq(enki_store_write(NULL, key_b, bytes_b, sizeof(bytes_b)),
               ENKI_STORE_ERROR);
  cr_assert_eq(
      enki_store_write(&fixture_interp->store, key_b, NULL, sizeof(bytes_b)),
      ENKI_STORE_ERROR);
}
