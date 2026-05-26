#include "test_interp.h"
#include "enki/app.h"
#include "enki/error.h"
#include "enki/eval.h"
#include "enki/interp.h"
#include "enki/op0.h"
#include "enki/op66.h"
#include "enki/pin.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>

static enki_interpreter* fixture_interp;

typedef void (*throw_fn)(void*);

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

static int capture_throw(throw_fn fn, void* ctx)
{
    fixture_interp->has_error_jmp = true;
    if(setjmp(fixture_interp->error_jmp) == 0) {
        fn(ctx);
        fixture_interp->has_error_jmp = false;
        enki_arena_reset(fixture_interp->scratch_a);
        return ENKI_ERROR_OK;
    }
    fixture_interp->has_error_jmp = false;
    enki_arena_reset(fixture_interp->scratch_a);
    return fixture_interp->error_code;
}

static enki_value app1(enki_value fn_v, enki_value a)
{
    size_t base_s = fixture_interp->sp;
    fixture_interp->stack_v[fixture_interp->sp++] = fn_v;
    fixture_interp->stack_v[fixture_interp->sp++] = a;
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 1);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->fn_v = fixture_interp->stack_v[base_s];
    app->args_v[0] = fixture_interp->stack_v[base_s + 1];
    fixture_interp->sp = base_s;
    return value_v;
}

static enki_value make_pin(enki_value inner_v)
{
    fixture_interp->stack_v[0] = inner_v;
    fixture_interp->sp = 1;
    op0_pin(fixture_interp);
    return fixture_interp->stack_v[0];
}

static void assert_hash_eq(const uint8_t a[32], const uint8_t b[32])
{
    cr_assert_eq(memcmp(a, b, 32), 0);
}

TestSuite(pin, .init = setup, .fini = teardown);

Test(pin, op0_pin_wraps_inner_with_zero_hash)
{
    enki_value pin_v = make_pin(42);

    cr_assert(IS_PTR(pin_v));
    enki_pin* pin = ENKI_AS(enki_pin, pin_v);
    uint8_t zero_hash_b[32] = {0};
    cr_assert_eq(pin->h.kind_b, ENKI_PIN);
    cr_assert_eq(pin->h.state_b, WHNF);
    cr_assert_eq(pin->inner_v, 42);
    cr_assert_eq(pin->n_subpins_s, 0);
    assert_hash_eq(pin->hash_b, zero_hash_b);
}

Test(pin, op0_pin_collects_nested_subpins)
{
    enki_value child_pin_v = make_pin(11);
    enki_value body_v = app1(child_pin_v, 99);
    enki_value parent_pin_v = make_pin(body_v);

    enki_pin* parent = ENKI_AS(enki_pin, parent_pin_v);
    cr_assert_eq(parent->n_subpins_s, 1);
    cr_assert_eq(parent->subpins_v[0], child_pin_v);
}

Test(pin, hash_is_deterministic_and_sets_hash_on_eval_nf)
{
    enki_value pin_v = make_pin(42);
    uint8_t a_hash_b[32];
    uint8_t b_hash_b[32];

    enki_pin_hash(fixture_interp, pin_v, a_hash_b);
    enki_pin_hash(fixture_interp, pin_v, b_hash_b);
    assert_hash_eq(a_hash_b, b_hash_b);

    enki_value result_v = enki_eval_nf(fixture_interp, pin_v);
    enki_pin* pin = ENKI_AS(enki_pin, result_v);
    cr_assert_eq(pin->h.state_b, NF);
    assert_hash_eq(pin->hash_b, a_hash_b);
}

Test(pin, save_and_load_roundtrip_simple_pin)
{
    enki_value pin_v = make_pin(42);
    uint8_t hash_b[32];

    enki_pin_save(fixture_interp, pin_v, hash_b);
    enki_value loaded_v = enki_pin_load(fixture_interp, hash_b);

    cr_assert(IS_PTR(loaded_v));
    enki_pin* loaded = ENKI_AS(enki_pin, loaded_v);
    cr_assert_eq(loaded->h.kind_b, ENKI_PIN);
    cr_assert_eq(loaded->h.state_b, NF);
    cr_assert_eq(loaded->inner_v, 42);
    cr_assert_eq(loaded->n_subpins_s, 0);
    assert_hash_eq(loaded->hash_b, hash_b);
}

Test(pin, save_and_load_roundtrip_nested_subpin_graph)
{
    enki_value child_pin_v = make_pin(11);
    enki_value body_v = app1(child_pin_v, 99);
    enki_value parent_pin_v = make_pin(body_v);
    uint8_t parent_hash_b[32];

    enki_pin_save(fixture_interp, parent_pin_v, parent_hash_b);
    enki_value loaded_v = enki_pin_load(fixture_interp, parent_hash_b);

    enki_pin* loaded = ENKI_AS(enki_pin, loaded_v);
    cr_assert_eq(loaded->n_subpins_s, 1);
    cr_assert(IS_PTR(loaded->subpins_v[0]));
    enki_pin* loaded_child = ENKI_AS(enki_pin, loaded->subpins_v[0]);
    cr_assert_eq(loaded_child->inner_v, 11);

    cr_assert(IS_PTR(loaded->inner_v));
    enki_app* inner = ENKI_AS(enki_app, loaded->inner_v);
    cr_assert_eq(inner->h.kind_b, ENKI_APP);
    cr_assert_eq(inner->args_v[0], 99);
    cr_assert(IS_PTR(inner->fn_v));
    enki_pin* inner_ref = ENKI_AS(enki_pin, inner->fn_v);
    assert_hash_eq(inner_ref->hash_b, loaded_child->hash_b);
}

Test(pin, root_save_and_load_roundtrip_through_op66)
{
    enki_value pin_v = make_pin(1234);
    fixture_interp->stack_v[0] = pin_v;
    fixture_interp->sp = 1;

    op66_save(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 0);

    fixture_interp->stack_v[0] = 0;
    fixture_interp->sp = 1;
    op66_load(fixture_interp);

    enki_value loaded_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(loaded_v));
    enki_pin* loaded = ENKI_AS(enki_pin, loaded_v);
    cr_assert_eq(loaded->inner_v, 1234);
}

static void hash_non_pin(void* ctx)
{
    uint8_t hash_b[32];
    (void)ctx;
    enki_pin_hash(fixture_interp, 42, hash_b);
}

Test(pin, hash_rejects_non_pin)
{
    cr_assert_eq(capture_throw(hash_non_pin, NULL), ENKI_ERROR_TYPE);
}

static void eval_bad_pin_hash(void* ctx)
{
    uint8_t bad_hash_b[32] = {1};
    enki_value pin_v = enki_pin_alloc(fixture_interp->gc, bad_hash_b, 42, 0, NULL);
    (void)ctx;
    (void)enki_eval_nf(fixture_interp, pin_v);
}

Test(pin, eval_nf_rejects_hash_mismatch)
{
    cr_assert_eq(capture_throw(eval_bad_pin_hash, NULL), ENKI_ERROR_BAD_PIN);
}

static void load_missing_root(void* ctx)
{
    (void)ctx;
    (void)enki_pin_load_root(fixture_interp);
}

Test(pin, load_root_reports_missing_root)
{
    cr_assert_eq(capture_throw(load_missing_root, NULL), ENKI_STORE_NOT_FOUND);
}

typedef struct bad_pin_case {
    uint8_t key_b[32];
    uint8_t bytes_b[16];
    size_t len_s;
} bad_pin_case;

static void load_bad_pin_bytes(void* ctx)
{
    bad_pin_case* c = ctx;
    enki_error st = fixture_interp->store.write(&fixture_interp->store, c->key_b, c->bytes_b, c->len_s);
    cr_assert_eq(st, ENKI_ERROR_OK);
    (void)enki_pin_load(fixture_interp, c->key_b);
}

Test(pin, load_rejects_bad_version)
{
    bad_pin_case c = {0};
    c.key_b[0] = 1;
    c.bytes_b[0] = 0xFF;
    c.len_s = 1;

    cr_assert_eq(capture_throw(load_bad_pin_bytes, &c), ENKI_ERROR_BAD_PIN);
}

Test(pin, load_rejects_truncated_pin_file)
{
    bad_pin_case c = {0};
    c.key_b[0] = 2;
    c.bytes_b[0] = 0x01;
    c.bytes_b[1] = 0x00;
    c.len_s = 2;

    cr_assert_eq(capture_throw(load_bad_pin_bytes, &c), ENKI_ERROR_BOUNDS);
}

typedef struct deserialize_case {
    uint8_t bytes_b[64];
    size_t len_s;
} deserialize_case;

static void deserialize_bad_bytes(void* ctx)
{
    deserialize_case* c = ctx;
    size_t off_s = 0;
    (void)enki_pin_deserialize(fixture_interp, c->bytes_b, c->len_s, &off_s);
}

Test(pin, deserialize_rejects_truncated_nat_length)
{
    deserialize_case c = {0};
    c.bytes_b[0] = ENKI_NAT;
    c.len_s = 1;

    cr_assert_eq(capture_throw(deserialize_bad_bytes, &c), ENKI_ERROR_BOUNDS);
}

Test(pin, deserialize_rejects_truncated_nat_payload)
{
    deserialize_case c = {0};
    c.bytes_b[0] = ENKI_NAT;
    c.bytes_b[1] = 4;
    c.len_s = 9;

    cr_assert_eq(capture_throw(deserialize_bad_bytes, &c), ENKI_ERROR_BOUNDS);
}

Test(pin, deserialize_rejects_truncated_pin_ref)
{
    deserialize_case c = {0};
    c.bytes_b[0] = ENKI_PIN;
    c.len_s = 8;

    cr_assert_eq(capture_throw(deserialize_bad_bytes, &c), ENKI_ERROR_BOUNDS);
}

Test(pin, deserialize_rejects_truncated_app_arg_count)
{
    deserialize_case c = {0};
    c.bytes_b[0] = ENKI_APP;
    c.bytes_b[1] = ENKI_NAT;
    c.len_s = 10;

    cr_assert_eq(capture_throw(deserialize_bad_bytes, &c), ENKI_ERROR_BOUNDS);
}

Test(pin, deserialize_rejects_unknown_tag)
{
    deserialize_case c = {0};
    c.bytes_b[0] = 0xFF;
    c.len_s = 1;

    cr_assert_eq(capture_throw(deserialize_bad_bytes, &c), ENKI_ERROR_BAD_PIN);
}

Test(pin, deserialize_fuzzes_short_malformed_inputs_without_crashing)
{
    for(size_t len_s = 0; len_s < 32; len_s++) {
        for(size_t seed_s = 0; seed_s < 64; seed_s++) {
            deserialize_case c = {0};
            c.len_s = len_s;
            for(size_t k = 0; k < len_s; k++) {
                c.bytes_b[k] = (uint8_t)((seed_s * 33u) ^ (k * 17u) ^ (len_s * 9u));
            }
            (void)capture_throw(deserialize_bad_bytes, &c);
            fixture_interp->sp = 0;
        }
    }
}
