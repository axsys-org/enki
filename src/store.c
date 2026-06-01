#include "enki/store.h"

#include <openssl/sha.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ER_STORE_VERSION 0x02u
#define ER_STORE_SMALL_MAX UINT64_C(0x7fffffffffffffff)

typedef struct er_store_buf {
    const enki_allocator* a;
    uint8_t* data_b;
    size_t len_s;
    size_t cap_s;
} er_store_buf;

typedef struct er_store_vals {
    const enki_allocator* a;
    er_val* val_v;
    size_t len_s;
    size_t cap_s;
} er_store_vals;

static const uint8_t er_store_root_key_b[32] = {'r', 'o', 'o', 't'};

static bool er_store_allocator_ok(const enki_allocator* a)
{
    return a != NULL && a->alloc != NULL && a->free != NULL;
}

static bool er_store_mul_size(size_t a_s, size_t b_s, size_t* out_s)
{
    if (b_s != 0 && a_s > SIZE_MAX / b_s) {
        return false;
    }
    *out_s = a_s * b_s;
    return true;
}

static enki_error er_store_buf_reserve(er_store_buf* buf, size_t need_s)
{
    if (buf == NULL || !er_store_allocator_ok(buf->a)) {
        return ENKI_STORE_ERROR;
    }
    if (need_s <= buf->cap_s) {
        return ENKI_ERROR_OK;
    }
    size_t next_s = buf->cap_s == 0 ? 64 : buf->cap_s;
    while (next_s < need_s) {
        if (next_s > SIZE_MAX / 2u) {
            return ENKI_ERROR_OOM;
        }
        next_s *= 2u;
    }

    uint8_t* next_b = NULL;
    if (buf->a->realloc != NULL) {
        next_b = buf->a->realloc(buf->a->ctx, buf->data_b, next_s);
    } else {
        next_b = buf->a->alloc(buf->a->ctx, next_s);
        if (next_b != NULL && buf->data_b != NULL && buf->len_s > 0) {
            memcpy(next_b, buf->data_b, buf->len_s);
            buf->a->free(buf->a->ctx, buf->data_b);
        }
    }
    if (next_b == NULL) {
        return ENKI_ERROR_OOM;
    }
    buf->data_b = next_b;
    buf->cap_s = next_s;
    return ENKI_ERROR_OK;
}

static enki_error er_store_buf_push(er_store_buf* buf, const void* data, size_t len_s)
{
    if (len_s == 0) {
        return ENKI_ERROR_OK;
    }
    if (data == NULL || buf->len_s > SIZE_MAX - len_s) {
        return ENKI_STORE_ERROR;
    }
    enki_error err = er_store_buf_reserve(buf, buf->len_s + len_s);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    memcpy(buf->data_b + buf->len_s, data, len_s);
    buf->len_s += len_s;
    return ENKI_ERROR_OK;
}

static enki_error er_store_buf_push_u8(er_store_buf* buf, uint8_t val_b)
{
    return er_store_buf_push(buf, &val_b, sizeof(val_b));
}

static enki_error er_store_buf_push_u64(er_store_buf* buf, uint64_t val_q)
{
    for (size_t k = 0; k < 8u; k++) {
        enki_error err = er_store_buf_push_u8(buf, (uint8_t)((val_q >> (k * 8u)) & 0xffu));
        if (err != ENKI_ERROR_OK) {
            return err;
        }
    }
    return ENKI_ERROR_OK;
}

static void er_store_buf_free(er_store_buf* buf)
{
    if (buf != NULL && buf->data_b != NULL && er_store_allocator_ok(buf->a)) {
        buf->a->free(buf->a->ctx, buf->data_b);
    }
    if (buf != NULL) {
        buf->data_b = NULL;
        buf->len_s = 0;
        buf->cap_s = 0;
    }
}

static enki_error er_store_vals_reserve(er_store_vals* vals, size_t need_s)
{
    if (vals == NULL || !er_store_allocator_ok(vals->a)) {
        return ENKI_STORE_ERROR;
    }
    if (need_s <= vals->cap_s) {
        return ENKI_ERROR_OK;
    }
    size_t next_s = vals->cap_s == 0 ? 8 : vals->cap_s;
    while (next_s < need_s) {
        if (next_s > SIZE_MAX / 2u) {
            return ENKI_ERROR_OOM;
        }
        next_s *= 2u;
    }
    size_t next_bytes_s = 0;
    if (!er_store_mul_size(next_s, sizeof(er_val), &next_bytes_s)) {
        return ENKI_ERROR_OOM;
    }

    er_val* next_v = NULL;
    if (vals->a->realloc != NULL) {
        next_v = vals->a->realloc(vals->a->ctx, vals->val_v, next_bytes_s);
    } else {
        next_v = vals->a->alloc(vals->a->ctx, next_bytes_s);
        if (next_v != NULL && vals->val_v != NULL && vals->len_s > 0) {
            memcpy(next_v, vals->val_v, vals->len_s * sizeof(er_val));
            vals->a->free(vals->a->ctx, vals->val_v);
        }
    }
    if (next_v == NULL) {
        return ENKI_ERROR_OOM;
    }
    vals->val_v = next_v;
    vals->cap_s = next_s;
    return ENKI_ERROR_OK;
}

static enki_error er_store_vals_push(er_store_vals* vals, er_val val_v)
{
    enki_error err = er_store_vals_reserve(vals, vals->len_s + 1u);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    vals->val_v[vals->len_s++] = val_v;
    return ENKI_ERROR_OK;
}

static void er_store_vals_free(er_store_vals* vals)
{
    if (vals != NULL && vals->val_v != NULL && er_store_allocator_ok(vals->a)) {
        vals->a->free(vals->a->ctx, vals->val_v);
    }
    if (vals != NULL) {
        vals->val_v = NULL;
        vals->len_s = 0;
        vals->cap_s = 0;
    }
}

static enki_error er_store_read_u64(const uint8_t* data_b, size_t data_s, size_t* off_s,
                                    uint64_t* out_q)
{
    if (data_b == NULL || off_s == NULL || out_q == NULL || *off_s > data_s ||
        data_s - *off_s < 8u) {
        return ENKI_ERROR_BOUNDS;
    }
    uint64_t out = 0;
    for (size_t k = 0; k < 8u; k++) {
        out |= ((uint64_t)data_b[*off_s + k]) << (k * 8u);
    }
    *off_s += 8u;
    *out_q = out;
    return ENKI_ERROR_OK;
}

static bool er_store_u64_to_size(uint64_t val_q, size_t* out_s)
{
    *out_s = (size_t)val_q;
    return (uint64_t)*out_s == val_q;
}

static er_val er_store_limbs_to_nat(const enki_allocator* heap_a, size_t limb_s,
                                    const uint64_t limb_q[])
{
    while (limb_s > 0 && limb_q[limb_s - 1u] == 0) {
        limb_s--;
    }
    if (limb_s == 0) {
        return 0;
    }
    if (limb_s == 1 && limb_q[0] <= ER_STORE_SMALL_MAX) {
        return limb_q[0];
    }

    er_bat* bat = er_bat_alloc(heap_a, limb_s);
    if (bat == NULL) {
        return er_bad;
    }
    er_val out_v = er_bat_init(bat, limb_s, limb_q);
    return out_v == 0 ? er_bad : out_v;
}

static size_t er_store_cat_byte_size(er_val val_v)
{
    size_t byte_s = 0;
    while (val_v != 0) {
        byte_s++;
        val_v >>= 8u;
    }
    return byte_s;
}

static uint8_t er_store_bat_byte(const er_bat* bat, size_t idx_s)
{
    uint64_t limb_q = bat->lim_q[idx_s / 8u];
    return (uint8_t)((limb_q >> ((idx_s % 8u) * 8u)) & 0xffu);
}

static size_t er_store_bat_byte_size(const er_bat* bat)
{
    size_t byte_s = bat->lim_s * sizeof(uint64_t);
    while (byte_s > 0 && er_store_bat_byte(bat, byte_s - 1u) == 0) {
        byte_s--;
    }
    return byte_s;
}

static enki_error er_store_serialize_value(er_store_buf* buf, er_val val_v);

static enki_error er_store_serialize_nat(er_store_buf* buf, er_val val_v)
{
    enki_error err = er_store_buf_push_u8(buf, er_tag_bat);
    if (err != ENKI_ERROR_OK) {
        return err;
    }

    if (er_is_cat(val_v)) {
        size_t byte_s = er_store_cat_byte_size(val_v);
        err = er_store_buf_push_u64(buf, (uint64_t)byte_s);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        for (size_t k = 0; k < byte_s; k++) {
            err = er_store_buf_push_u8(buf, (uint8_t)((val_v >> (k * 8u)) & 0xffu));
            if (err != ENKI_ERROR_OK) {
                return err;
            }
        }
        return ENKI_ERROR_OK;
    }

    er_bat* bat = er_outt(er_tag_bat, val_v);
    if (bat == NULL) {
        return ENKI_ERROR_BAD_TAG;
    }
    size_t byte_s = er_store_bat_byte_size(bat);
    err = er_store_buf_push_u64(buf, (uint64_t)byte_s);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    for (size_t k = 0; k < byte_s; k++) {
        err = er_store_buf_push_u8(buf, er_store_bat_byte(bat, k));
        if (err != ENKI_ERROR_OK) {
            return err;
        }
    }
    return ENKI_ERROR_OK;
}

static enki_error er_store_serialize_value(er_store_buf* buf, er_val val_v)
{
    if (er_is_cat(val_v) || er_is_tag(er_tag_bat, val_v)) {
        return er_store_serialize_nat(buf, val_v);
    }

    er_pin* pin = er_outt(er_tag_pin, val_v);
    if (pin != NULL) {
        enki_error err = er_store_buf_push_u8(buf, er_tag_pin);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        return er_store_buf_push(buf, pin->hash_b, sizeof(pin->hash_b));
    }

    er_law* law = er_outt(er_tag_law, val_v);
    if (law != NULL) {
        enki_error err = er_store_buf_push_u8(buf, er_tag_law);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        err = er_store_buf_push_u64(buf, law->ari_d);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        err = er_store_serialize_value(buf, law->name_v);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        return er_store_serialize_value(buf, law->body_v);
    }

    er_app* app = er_outt(er_tag_app, val_v);
    if (app != NULL) {
        enki_error err = er_store_buf_push_u8(buf, er_tag_app);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        err = er_store_serialize_value(buf, app->fn_v);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        err = er_store_buf_push_u64(buf, (uint64_t)app->arg_s);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        for (size_t k = 0; k < app->arg_s; k++) {
            err = er_store_serialize_value(buf, app->arg_v[k]);
            if (err != ENKI_ERROR_OK) {
                return err;
            }
        }
        return ENKI_ERROR_OK;
    }

    return ENKI_ERROR_BAD_TAG;
}

static enki_error er_store_collect_subpins(er_store_vals* pins, er_val val_v)
{
    if (er_is_cat(val_v) || er_is_tag(er_tag_bat, val_v)) {
        return ENKI_ERROR_OK;
    }

    er_pin* pin = er_outt(er_tag_pin, val_v);
    if (pin != NULL) {
        (void)pin;
        return er_store_vals_push(pins, val_v);
    }

    er_law* law = er_outt(er_tag_law, val_v);
    if (law != NULL) {
        enki_error err = er_store_collect_subpins(pins, law->name_v);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        return er_store_collect_subpins(pins, law->body_v);
    }

    er_app* app = er_outt(er_tag_app, val_v);
    if (app != NULL) {
        enki_error err = er_store_collect_subpins(pins, app->fn_v);
        if (err != ENKI_ERROR_OK) {
            return err;
        }
        for (size_t k = 0; k < app->arg_s; k++) {
            err = er_store_collect_subpins(pins, app->arg_v[k]);
            if (err != ENKI_ERROR_OK) {
                return err;
            }
        }
        return ENKI_ERROR_OK;
    }

    return ENKI_ERROR_BAD_TAG;
}

static enki_error er_store_deserialize_value(const enki_allocator* heap_a,
                                             const enki_allocator* work_a,
                                             const uint8_t* data_b, size_t data_s,
                                             size_t* off_s, er_val* out_v);

static enki_error er_store_deserialize_nat(const enki_allocator* heap_a,
                                           const enki_allocator* work_a,
                                           const uint8_t* data_b, size_t data_s,
                                           size_t* off_s, er_val* out_v)
{
    uint64_t byte_q = 0;
    enki_error err = er_store_read_u64(data_b, data_s, off_s, &byte_q);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    size_t byte_s = 0;
    if (!er_store_u64_to_size(byte_q, &byte_s) || *off_s > data_s || data_s - *off_s < byte_s) {
        return ENKI_ERROR_BOUNDS;
    }
    if (byte_s == 0) {
        *out_v = 0;
        return ENKI_ERROR_OK;
    }

    size_t limb_s = (byte_s + 7u) / 8u;
    uint64_t local_q = 0;
    uint64_t* limb_q = &local_q;
    if (limb_s > 1u) {
        size_t limb_bytes_s = 0;
        if (!er_store_mul_size(limb_s, sizeof(uint64_t), &limb_bytes_s)) {
            return ENKI_ERROR_OOM;
        }
        limb_q = work_a->alloc(work_a->ctx, limb_bytes_s);
        if (limb_q == NULL) {
            return ENKI_ERROR_OOM;
        }
        memset(limb_q, 0, limb_bytes_s);
    }

    for (size_t k = 0; k < byte_s; k++) {
        limb_q[k / 8u] |= ((uint64_t)data_b[*off_s + k]) << ((k % 8u) * 8u);
    }
    *off_s += byte_s;
    *out_v = er_store_limbs_to_nat(heap_a, limb_s, limb_q);
    if (limb_q != &local_q) {
        work_a->free(work_a->ctx, limb_q);
    }
    return *out_v == er_bad ? ENKI_ERROR_OOM : ENKI_ERROR_OK;
}

static enki_error er_store_deserialize_pin_ref(const enki_allocator* heap_a,
                                               const uint8_t* data_b, size_t data_s,
                                               size_t* off_s, er_val* out_v)
{
    if (*off_s > data_s || data_s - *off_s < 32u) {
        return ENKI_ERROR_BOUNDS;
    }
    er_pin* pin = er_pin_alloc(heap_a, 0);
    if (pin == NULL) {
        return ENKI_ERROR_OOM;
    }
    *out_v = er_pin_init(pin, data_b + *off_s, 0, 0, NULL);
    *off_s += 32u;
    return *out_v == 0 ? ENKI_ERROR_OOM : ENKI_ERROR_OK;
}

static enki_error er_store_deserialize_law(const enki_allocator* heap_a,
                                           const enki_allocator* work_a,
                                           const uint8_t* data_b, size_t data_s,
                                           size_t* off_s, er_val* out_v)
{
    uint64_t ari_q = 0;
    enki_error err = er_store_read_u64(data_b, data_s, off_s, &ari_q);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    if (ari_q > UINT32_MAX - 1u) {
        return ENKI_ERROR_BOUNDS;
    }

    er_val name_v = 0;
    err = er_store_deserialize_value(heap_a, work_a, data_b, data_s, off_s, &name_v);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    er_val body_v = 0;
    err = er_store_deserialize_value(heap_a, work_a, data_b, data_s, off_s, &body_v);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    *out_v = er_law_make(heap_a, name_v, body_v, (uint32_t)ari_q);
    return *out_v == 0 ? ENKI_ERROR_BAD_PIN : ENKI_ERROR_OK;
}

static enki_error er_store_deserialize_app(const enki_allocator* heap_a,
                                           const enki_allocator* work_a,
                                           const uint8_t* data_b, size_t data_s,
                                           size_t* off_s, er_val* out_v)
{
    er_val fn_v = 0;
    enki_error err = er_store_deserialize_value(heap_a, work_a, data_b, data_s, off_s, &fn_v);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    uint64_t arg_q = 0;
    err = er_store_read_u64(data_b, data_s, off_s, &arg_q);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    size_t arg_s = 0;
    if (!er_store_u64_to_size(arg_q, &arg_s)) {
        return ENKI_ERROR_BOUNDS;
    }

    er_val* arg_v = NULL;
    if (arg_s > 0) {
        size_t arg_bytes_s = 0;
        if (!er_store_mul_size(arg_s, sizeof(er_val), &arg_bytes_s)) {
            return ENKI_ERROR_OOM;
        }
        arg_v = work_a->alloc(work_a->ctx, arg_bytes_s);
        if (arg_v == NULL) {
            return ENKI_ERROR_OOM;
        }
        for (size_t k = 0; k < arg_s; k++) {
            err = er_store_deserialize_value(heap_a, work_a, data_b, data_s, off_s, &arg_v[k]);
            if (err != ENKI_ERROR_OK) {
                work_a->free(work_a->ctx, arg_v);
                return err;
            }
        }
    }

    er_app* app = er_app_alloc(heap_a, arg_s);
    if (app == NULL) {
        if (arg_v != NULL) {
            work_a->free(work_a->ctx, arg_v);
        }
        return ENKI_ERROR_OOM;
    }
    *out_v = er_app_init(app, fn_v, arg_s, arg_v);
    if (arg_v != NULL) {
        work_a->free(work_a->ctx, arg_v);
    }
    return *out_v == 0 ? ENKI_ERROR_OOM : ENKI_ERROR_OK;
}

static enki_error er_store_deserialize_value(const enki_allocator* heap_a,
                                             const enki_allocator* work_a,
                                             const uint8_t* data_b, size_t data_s,
                                             size_t* off_s, er_val* out_v)
{
    if (data_b == NULL || off_s == NULL || out_v == NULL || *off_s >= data_s) {
        return ENKI_ERROR_BOUNDS;
    }
    uint8_t tag_b = data_b[(*off_s)++];
    switch (tag_b) {
    case er_tag_bat:
        return er_store_deserialize_nat(heap_a, work_a, data_b, data_s, off_s, out_v);
    case er_tag_pin:
        return er_store_deserialize_pin_ref(heap_a, data_b, data_s, off_s, out_v);
    case er_tag_law:
        return er_store_deserialize_law(heap_a, work_a, data_b, data_s, off_s, out_v);
    case er_tag_app:
        return er_store_deserialize_app(heap_a, work_a, data_b, data_s, off_s, out_v);
    default:
        return ENKI_ERROR_BAD_TAG;
    }
}

enki_error enki_store_init(const char* path, size_t size_s, enki_store* store) {
    if(store == NULL) return ENKI_STORE_ERROR;
    MDB_env* env;
    int rc = mdb_env_create(&env);
    if(rc != 0) return ENKI_STORE_ERROR;
    rc = mdb_env_set_mapsize(env, size_s); // 1024ULL * 1024ULL * 1024ULL
    if(rc != 0) {
        mdb_env_close(env);
        return ENKI_STORE_ERROR;
    }
    rc = mdb_env_open(env, path, 0, 0664);
    if(rc != 0) {
        mdb_env_close(env);
        return ENKI_STORE_ERROR;
    }
    MDB_txn* txn;
    MDB_dbi dbi;
    rc = mdb_txn_begin(env, NULL, 0, &txn);
    if(rc != 0) {
        mdb_env_close(env);
        return ENKI_STORE_ERROR;
    }
    rc = mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi);
    if(rc != 0) {
        mdb_txn_abort(txn);
        mdb_env_close(env);
        return ENKI_STORE_ERROR;
    }
    rc = mdb_txn_commit(txn);
    if(rc != 0) {
        mdb_env_close(env);
        return ENKI_STORE_ERROR;
    }
    store->size_s = size_s;
    store->read = enki_store_read;
    store->write = enki_store_write;
    store->size = enki_store_size;
    store->env = env;
    store->dbi = dbi;
    return ENKI_ERROR_OK;
}
enki_error enki_store_size(enki_store* store, const uint8_t* key_b, size_t* len_s) {
    if(store == NULL || store->env == NULL || key_b == NULL || len_s == NULL) {
        return ENKI_STORE_ERROR;
    }
    MDB_txn* txn;
    int rc = mdb_txn_begin(store->env, NULL, MDB_RDONLY, &txn);
    if(rc != 0) {
        *len_s = 0;
        return ENKI_STORE_ERROR;
    }
    MDB_val key = {.mv_size = 32, .mv_data = (void*)key_b };
    MDB_val val;
    rc = mdb_get(txn, store->dbi, &key, &val);
    if(rc == MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        *len_s = 0;
        return ENKI_STORE_NOT_FOUND;
    }
    if(rc != 0) {
        mdb_txn_abort(txn);
        *len_s = 0;
        return ENKI_STORE_ERROR;
    }
    *len_s = val.mv_size;
    mdb_txn_abort(txn);
    return ENKI_ERROR_OK;
}
enki_error enki_store_read(enki_store* store, const uint8_t* key_b, uint8_t* bytes, size_t cap_s, size_t* len_s) {
    if(store == NULL || store->env == NULL || key_b == NULL || len_s == NULL) {
        return ENKI_STORE_ERROR;
    }
    MDB_txn* txn;
    int rc = mdb_txn_begin(store->env, NULL, MDB_RDONLY, &txn);
    if(rc != 0) {
        *len_s = 0;
        return ENKI_STORE_ERROR;
    }
    MDB_val key = {.mv_size = 32, .mv_data = (void*)key_b };
    MDB_val val;
    rc = mdb_get(txn, store->dbi, &key, &val);
    if(rc == MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        *len_s = 0;
        return ENKI_STORE_NOT_FOUND;
    }
    if(rc != 0) {
        mdb_txn_abort(txn);
        *len_s = 0;
        return ENKI_STORE_ERROR;
    }
    if(val.mv_size > cap_s) {
        *len_s = val.mv_size;
        mdb_txn_abort(txn);
        return ENKI_STORE_TOO_SMALL;
    }
    if(val.mv_size > 0 && bytes == NULL) {
        *len_s = val.mv_size;
        mdb_txn_abort(txn);
        return ENKI_STORE_TOO_SMALL;
    }
    memcpy(bytes, val.mv_data, val.mv_size);
    *len_s = val.mv_size;
    mdb_txn_abort(txn);
    return ENKI_ERROR_OK;
}
enki_error enki_store_write(enki_store* store, const uint8_t* key_b, const uint8_t* bytes, size_t len) {
    if(store == NULL || store->env == NULL || key_b == NULL || bytes == NULL) {
        return ENKI_STORE_ERROR;
    }
    MDB_txn* txn;
    int rc = mdb_txn_begin(store->env, NULL, 0, &txn);
    if(rc != 0) return ENKI_STORE_ERROR;
    MDB_val key = {.mv_size = 32, .mv_data = (void*)key_b };
    MDB_val val = {.mv_size = len, .mv_data = (void*)bytes };
    rc = mdb_put(txn, store->dbi, &key, &val, 0);
    if(rc != 0) {
        mdb_txn_abort(txn);
        return ENKI_STORE_ERROR;
    }
    rc = mdb_txn_commit(txn);
    if(rc != 0) return ENKI_STORE_ERROR;
    return ENKI_ERROR_OK;
}
void enki_store_close(enki_store* store) {
    if(store == NULL || store->env == NULL) return;
    mdb_dbi_close(store->env, store->dbi);
    mdb_env_close(store->env);
    store->env = NULL;
    store->dbi = 0;
}

enki_error er_store_save_pin(enki_store* store, const enki_allocator* heap_a,
    const enki_allocator* work_a, er_val pin_v, uint8_t hash_b[32])
{
    if (store == NULL || hash_b == NULL || !er_store_allocator_ok(heap_a) ||
        !er_store_allocator_ok(work_a)) {
        return ENKI_STORE_ERROR;
    }
    er_pin* pin = er_outt(er_tag_pin, pin_v);
    if (pin == NULL) {
        return ENKI_ERROR_TYPE;
    }

    er_store_vals subpins = {.a = work_a};
    enki_error err = er_store_collect_subpins(&subpins, pin->val_v);
    if (err != ENKI_ERROR_OK) {
        er_store_vals_free(&subpins);
        return err;
    }

    er_store_buf buf = {.a = work_a};
    err = er_store_buf_push_u8(&buf, ER_STORE_VERSION);
    if (err == ENKI_ERROR_OK) {
        err = er_store_buf_push_u64(&buf, (uint64_t)subpins.len_s);
    }
    for (size_t k = 0; err == ENKI_ERROR_OK && k < subpins.len_s; k++) {
        uint8_t sub_hash_b[32];
        err = er_store_save_pin(store, heap_a, work_a, subpins.val_v[k], sub_hash_b);
        if (err == ENKI_ERROR_OK) {
            err = er_store_buf_push(&buf, sub_hash_b, sizeof(sub_hash_b));
        }
    }
    if (err == ENKI_ERROR_OK) {
        err = er_store_serialize_value(&buf, pin->val_v);
    }
    if (err == ENKI_ERROR_OK) {
        SHA256(buf.data_b, buf.len_s, hash_b);
        memcpy(pin->hash_b, hash_b, 32);
        err = enki_store_write(store, hash_b, buf.data_b, buf.len_s);
    }

    er_store_buf_free(&buf);
    er_store_vals_free(&subpins);
    return err;
}

enki_error er_store_load_pin(enki_store* store, const enki_allocator* heap_a,
    const enki_allocator* work_a, const uint8_t hash_b[32], er_val* out_v)
{
    if (store == NULL || hash_b == NULL || out_v == NULL || !er_store_allocator_ok(heap_a) ||
        !er_store_allocator_ok(work_a)) {
        return ENKI_STORE_ERROR;
    }
    *out_v = 0;

    size_t len_s = 0;
    enki_error err = enki_store_size(store, hash_b, &len_s);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    uint8_t* data_b = work_a->alloc(work_a->ctx, len_s);
    if (data_b == NULL) {
        return ENKI_ERROR_OOM;
    }
    err = enki_store_read(store, hash_b, data_b, len_s, &len_s);
    if (err != ENKI_ERROR_OK) {
        work_a->free(work_a->ctx, data_b);
        return err;
    }

    uint8_t actual_hash_b[32];
    SHA256(data_b, len_s, actual_hash_b);
    if (memcmp(actual_hash_b, hash_b, 32) != 0) {
        work_a->free(work_a->ctx, data_b);
        return ENKI_ERROR_BAD_PIN;
    }

    size_t off_s = 0;
    if (len_s < 1u || data_b[off_s++] != ER_STORE_VERSION) {
        work_a->free(work_a->ctx, data_b);
        return ENKI_ERROR_BAD_PIN;
    }

    uint64_t subpin_q = 0;
    err = er_store_read_u64(data_b, len_s, &off_s, &subpin_q);
    size_t subpin_s = 0;
    if (err == ENKI_ERROR_OK && !er_store_u64_to_size(subpin_q, &subpin_s)) {
        err = ENKI_ERROR_BOUNDS;
    }

    er_val* subpin_v = NULL;
    if (err == ENKI_ERROR_OK && subpin_s > 0) {
        size_t subpin_bytes_s = 0;
        if (!er_store_mul_size(subpin_s, sizeof(er_val), &subpin_bytes_s)) {
            err = ENKI_ERROR_OOM;
        } else {
            subpin_v = work_a->alloc(work_a->ctx, subpin_bytes_s);
            if (subpin_v == NULL) {
                err = ENKI_ERROR_OOM;
            }
        }
    }

    for (size_t k = 0; err == ENKI_ERROR_OK && k < subpin_s; k++) {
        if (off_s > len_s || len_s - off_s < 32u) {
            err = ENKI_ERROR_BOUNDS;
            break;
        }
        uint8_t sub_hash_b[32];
        memcpy(sub_hash_b, data_b + off_s, 32);
        off_s += 32u;
        err = er_store_load_pin(store, heap_a, work_a, sub_hash_b, &subpin_v[k]);
    }

    er_val inner_v = 0;
    if (err == ENKI_ERROR_OK) {
        err = er_store_deserialize_value(heap_a, work_a, data_b, len_s, &off_s, &inner_v);
    }
    if (err == ENKI_ERROR_OK && off_s != len_s) {
        err = ENKI_ERROR_BAD_PIN;
    }
    if (err == ENKI_ERROR_OK) {
        er_pin* pin = er_pin_alloc(heap_a, subpin_s);
        if (pin == NULL) {
            err = ENKI_ERROR_OOM;
        } else {
            *out_v = er_pin_init(pin, hash_b, inner_v, subpin_s, subpin_v);
            if (*out_v == 0) {
                err = ENKI_ERROR_OOM;
            }
        }
    }

    if (subpin_v != NULL) {
        work_a->free(work_a->ctx, subpin_v);
    }
    work_a->free(work_a->ctx, data_b);
    return err;
}

enki_error er_store_save_root(enki_store* store, const enki_allocator* heap_a,
    const enki_allocator* work_a, er_val pin_v)
{
    uint8_t hash_b[32];
    enki_error err = er_store_save_pin(store, heap_a, work_a, pin_v, hash_b);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    return enki_store_write(store, er_store_root_key_b, hash_b, sizeof(hash_b));
}

enki_error er_store_load_root(enki_store* store, const enki_allocator* heap_a,
    const enki_allocator* work_a, er_val* out_v)
{
    uint8_t hash_b[32];
    size_t len_s = 0;
    enki_error err = enki_store_read(store, er_store_root_key_b, hash_b, sizeof(hash_b), &len_s);
    if (err != ENKI_ERROR_OK) {
        return err;
    }
    if (len_s != sizeof(hash_b)) {
        return ENKI_ERROR_BAD_PIN;
    }
    return er_store_load_pin(store, heap_a, work_a, hash_b, out_v);
}
