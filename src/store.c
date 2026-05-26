#include <string.h>

#include "enki/store.h"
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
enki_error enki_store_size(enki_store* store, uint8_t* key_b, size_t* len_s) {
    if(store == NULL || store->env == NULL || key_b == NULL || len_s == NULL) {
        return ENKI_STORE_ERROR;
    }
    MDB_txn* txn;
    int rc = mdb_txn_begin(store->env, NULL, MDB_RDONLY, &txn);
    if(rc != 0) {
        *len_s = 0;
        return ENKI_STORE_ERROR;
    }
    MDB_val key = {.mv_size = 32, .mv_data = key_b };
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
enki_error enki_store_read(enki_store* store, uint8_t* key_b, uint8_t* bytes, size_t cap_s, size_t* len_s) {
    if(store == NULL || store->env == NULL || key_b == NULL || len_s == NULL) {
        return ENKI_STORE_ERROR;
    }
    MDB_txn* txn;
    int rc = mdb_txn_begin(store->env, NULL, MDB_RDONLY, &txn);
    if(rc != 0) {
        *len_s = 0;
        return ENKI_STORE_ERROR;
    }
    MDB_val key = {.mv_size = 32, .mv_data = key_b };
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
enki_error enki_store_write(enki_store* store, uint8_t* key_b, uint8_t* bytes, size_t len) {
    if(store == NULL || store->env == NULL || key_b == NULL || bytes == NULL) {
        return ENKI_STORE_ERROR;
    }
    MDB_txn* txn;
    int rc = mdb_txn_begin(store->env, NULL, 0, &txn);
    if(rc != 0) return ENKI_STORE_ERROR;
    MDB_val key = {.mv_size = 32, .mv_data = key_b };
    MDB_val val = {.mv_size = len, .mv_data = bytes };
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
