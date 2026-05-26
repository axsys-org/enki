#include "test_interp.h"
#include "enki/error.h"
#include "enki/store.h"

#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>

static enki_interpreter* fixture_interp;

static void setup(void)
{
    fixture_interp = enki_test_interp_create(1024 * 1024, 0);
    cr_assert_not_null(fixture_interp);
}

static void teardown(void)
{
    enki_test_interp_destroy(fixture_interp);
    fixture_interp = NULL;
}

TestSuite(store, .init = setup, .fini = teardown);

Test(store, write_size_and_read_roundtrip)
{
    uint8_t key_b[32] = {1};
    uint8_t bytes_b[] = {4, 5, 6, 7};
    uint8_t out_b[4] = {0};
    size_t len_s = 0;

    cr_assert_eq(fixture_interp->store.write(&fixture_interp->store, key_b, bytes_b, sizeof(bytes_b)), ENKI_ERROR_OK);
    cr_assert_eq(fixture_interp->store.size(&fixture_interp->store, key_b, &len_s), ENKI_ERROR_OK);
    cr_assert_eq(len_s, sizeof(bytes_b));
    cr_assert_eq(fixture_interp->store.read(&fixture_interp->store, key_b, out_b, sizeof(out_b), &len_s), ENKI_ERROR_OK);
    cr_assert_eq(len_s, sizeof(bytes_b));
    cr_assert_eq(memcmp(out_b, bytes_b, sizeof(bytes_b)), 0);
}

Test(store, missing_key_reports_not_found)
{
    uint8_t key_b[32] = {2};
    uint8_t out_b[4] = {0};
    size_t len_s = 99;

    cr_assert_eq(fixture_interp->store.size(&fixture_interp->store, key_b, &len_s), ENKI_STORE_NOT_FOUND);
    cr_assert_eq(len_s, 0);
    cr_assert_eq(fixture_interp->store.read(&fixture_interp->store, key_b, out_b, sizeof(out_b), &len_s), ENKI_STORE_NOT_FOUND);
    cr_assert_eq(len_s, 0);
}

Test(store, read_reports_too_small_and_required_size)
{
    uint8_t key_b[32] = {3};
    uint8_t bytes_b[] = {8, 9, 10, 11};
    uint8_t out_b[2] = {0};
    size_t len_s = 0;

    cr_assert_eq(fixture_interp->store.write(&fixture_interp->store, key_b, bytes_b, sizeof(bytes_b)), ENKI_ERROR_OK);
    cr_assert_eq(fixture_interp->store.read(&fixture_interp->store, key_b, out_b, sizeof(out_b), &len_s), ENKI_STORE_TOO_SMALL);
    cr_assert_eq(len_s, sizeof(bytes_b));
}

Test(store, rejects_invalid_arguments)
{
    uint8_t key_b[32] = {4};
    uint8_t bytes_b[] = {1};
    size_t len_s = 0;

    cr_assert_eq(enki_store_size(NULL, key_b, &len_s), ENKI_STORE_ERROR);
    cr_assert_eq(enki_store_size(&fixture_interp->store, NULL, &len_s), ENKI_STORE_ERROR);
    cr_assert_eq(enki_store_size(&fixture_interp->store, key_b, NULL), ENKI_STORE_ERROR);
    cr_assert_eq(enki_store_read(NULL, key_b, bytes_b, sizeof(bytes_b), &len_s), ENKI_STORE_ERROR);
    cr_assert_eq(enki_store_write(NULL, key_b, bytes_b, sizeof(bytes_b)), ENKI_STORE_ERROR);
    cr_assert_eq(enki_store_write(&fixture_interp->store, key_b, NULL, sizeof(bytes_b)), ENKI_STORE_ERROR);
}
