/* 
collect_subpins 
canonize
hash 
*/


/*
NAT:
offset  size                 field
0       1                    tag = 0x00
1       8                    byte_len, u64 little-endian
9       byte_len             nat bytes, little-endian, canonical trimmed


PINREF: 
offset  size                 field
0       1                    tag = 0x01
1       32                   hash bytes


LAW:
offset  size                 field
0       1                    tag = 0x02
1       8                    arity, u64 little-endian
9       size(name)           encoded name value
9+N     size(body)           encoded body value
9+N+B   8                    const_count, u64 little-endian
17+N+B  size(const[0])       encoded const 0
...     ...                  encoded const 1, const 2, ...
N = encoded byte size of name
B = encoded byte size of body

APP
offset  size                 field
0       1                    tag = 0x03
1       size(fn)             encoded fn value
1+F     8                    arg_count, u64 little-endian
9+F     size(arg[0])         encoded arg 0
...     ...                  encoded arg 1, arg 2, ...
F = encoded byte size of fn


PINFILE

offset  size                 field
0       4                    magic = "ENKP"
4       1                    version = 0x01
5       8                    subpin_count, u64 little-endian
13      32 * subpin_count    subpin hashes
13+...  size(inner)          encoded inner value



*/

/*

init:
    MDB_env* env;
    mdb_env_create(&env);
    mdb_env_set_mapsize(env, 1024ULL * 1024ULL * 1024ULL); // 1GB max map
    mdb_env_open(env, "./snap.lmdb", 0, 0664);
    MDB_txn* txn;
    MDB_dbi dbi;
    mdb_txn_begin(env, NULL, 0, &txn);
    mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi);
    /* alloc store object with txn and dbi * /
    mdb_txn_commit(txn);

write (for very save)
    MDB_txn* txn;
    mdb_txn_begin(store->env, NULL, 0, &txn);
    MDB_val key = {.mv_size = 32, .mv_data = hash_b };
    MDB_val key = {.mv_size = len, .mv_data = bytes };
    mdb_put(txn, dbi, &key, &val, 0);
    mdb_txn_commit_(txn);

read (for every load)
    MDB_txn* txn;
    mdb_txn_begin(store->env, NULL, 0, &txn);
    MDB_val key = {.mv_size = 32, .mv_data = hash_b };
    MDB_val val;
    int rc = mdb_get(txn, dbi, &key, &val);
    if(rc == 0) {
        uint8* bytes = val.mv_data;
        size_t len = val.mv_size;
    }
    mdb_txn_abort(txn);

close 
    mdb_dbi_close(store->env, store->dbi)
    mdb_env_close(store->env)
*/

enki_value enki_deserialize(uint8_t* buf, size_t* off) {

    /*
        caller must go back to base sp once this is doen to remove scratch stack 
        switch on buf[*off]
            NAT:
                uint8_t tag = ENKI_NAT;
                size_t byte_len = 0;
                *off += 1
                for i=*off to *off+7: 
                    byte_len |= ((uint64_t)buf[i]) << (8 * i)
                    *off += 8
                size_t n_limbs = (byte_len + 7) / 8
                alloc nat 
                push to stack 
                for i=0..byte_len:
                    size_t index_i = i / 64;
                    limbs[index_i] |= ((mp_limb_t)buf[*off + i]) << ((i % 8) * 8)
                *off += byte_len                
                
            PIN_REF:
                uint8_t tag = ENKI_PIN;
                *off += 1
                uint8_t[32] hash; 
                for i=off to off+32
                    hash[i] = buff[i]
                *off += 32;
                alloc 
                push to stack 
            LAW: 
                uint8_t tag = ENKI_LAW;
                off += 1
                size_t arity = 0
                for i=0 to 7
                    arity |= ((uint64_t)buff[i] << (8 * i))
                    *off += 8
                name = enki_deserialize(buf, &off)
                push to stack 
                body = enki_deserialize(buf, &off)
                push to stack 
                size_t n_const = 0
                for i=off to off+7:
                    n_const |= ((uint64_t)buff[i] << (8 * i))
                    *off += 8
                alloc law 
                push to stack 
                for i=0..n_const:
                    const[i] = enki_deserialize(buf, &off)
                    push to stack                 
            APP:
                uint8_t tag = ENKI_APP;
                off += 1
                fn = enki_deserialize(buf, &off)
                push to stack 
                size_t n_args = 0
                for i=off to off+7:
                    n_args |= ((uint64_t)buff[i] << (8 * i))
                    off += 8
                alloc app 
                 push to stack 
                for i=0..n_args:
                    args[i] = enki_deserialize(buf, &off)
                    push to stack                 
    */
}

void enki_serialize(enki_interpreter* i, enki_value val, enki_vector* out) {

    if(!IS_PTR(val)) {
        enki_vector_push_u8(out, ENKI_NAT);
        enki_vector_push_u8(out, (uint8_t)8);
        for(size_t k = 0; k < 8; k++) {
            enki_vector_push_u8(out, ((uint8_t)val >> 8 * k) & 0xFF);
        }
    }
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(val);
    enki_vector_push_u8(out, h->kind_b);
    switch (h->kind_b) {
        case ENKI_NAT:
            size_t bytes_len = enki_nat_bytes(i->gc, val);
            for(size_t k = 0; k < 8; k++) {
                enki_vector_push_u8(out, ((uint8_t)bytes_len >> 8 * k) & 0xFF);
            }
            for(size_tk = 0; k < byte_len; k++) {
                enki_vector_push_u8(out, (uint8_t)enki_nat_load8(i->gc, k, val));
            }
            break;
        case ENKI_PIN:
            enki_pin* pin = (enki_pin*)val;
            for(size_t k = 0; k < 32; k++) {
                enki_vector_push_u8(out, pin->hash_b[k]);
            } 
            break;
        case ENKI_LAW:
            enki_law* law = (enki_law*)val;
            for(size_t k = 0; k < 8; k++) {
                enki_vector_push_u8(out, (((uint8_t)law->arity_s >> (8 * k)) & 0xFF));
            }
            enki_serialize(i, law->name_v, out);
            enki_serialize(i, law->body_v, out);
            for(size_t k = 0; k < 8; k++) {
                enki_vector_push_u8(out, (((uint8_t)law->n_const_s >> (8 * k)) & 0xFF));
            }
            for(size_t k = 0; k < law->n_const_s; k++) {
                enki_serialize(i, law->const_table_v[k], out);
            }
            break;
        case ENKI_APP:
            enki_app* app = (enki_app*)val;
            enki_serialize(i, app->fn, out);
            for(size_t k = 0; k < 8; k++) {
                enki_vector_push_u8(out, (((uint8_t)app->n_args_s >> (8 * k)) & 0xFF));
            }
            for(size_t k = 0; k < app->n_args_s; k++) {
                enki_serialize(i, app->args[k], out);
            }
            break; 
        default: break;
    }
}
void enki_save_pin(enki_interpreter* i, enki_value val, uint8_t* hash_b) {
    if(!IS_PTR(val)) exit(1);
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(val);
    if(h->kind != ENKI_PIN) exit(1);
    enki_pin* pin = (enki_pin*)ENKI_TO_PTR(val);
    enki_vector* out_subpins_v = enki_vector_create_sized(i->sys_a, sizeof(enki_value));
    enki_collect_subpins(pin->inner, out_subpins_v);
    size_t n_subpins_s = enki_vector_len(out_subpins_v);
    enki_value* subpins_v = (enki_value*)enki_vector_data(out_subpins_v);
    uint8_t sub_hashes[n_subpins_s][32];
    for(size_t k = 0; k < n_subpins_s; k++) {
        uint8_t hash[32];
        enki_save_pin(i, subpins_v[k], hash);
        for(size_t j = 0; j < 32; j++) {
            sub_hashes[k][j] = hash[j];
        }
    }
    enki_vector* out_inner_v = enki_vector_create_sized(i->sys_a, sizeof(uint8_t));
    enki_serialize(pin->inner, out_inner_v);
    size_t n_inner_s = enki_vector_len(out_inner_v);
    uint8_t* inner_v = (uint8_t*)enki_vector_data(out_inner_v);
    size_t n_out = 1 + 8 + (32 * n_subpins_s) + n_inner_s;
    uint8_t out[n_out];
    out[0] = 0; // TEMPORARY for now 
    for (size_t k = 0; k < 8; k++) {
        out[k + 1] = (n_subpins_s >> (8 * k));
    }
    for (size_t k = 0; k < n_subpins_s; k++) {
        for (size_t j = 0; j < 32; j++) {
            out[(k * 32) + j + 1 + 8] = sub_hashes[k][j];
        }
    }
    for (size_t k = 0; k < n_inner_s; k++) {
        out[(n_out - n_inner_s) + k] = inner_v[k];
    }
    SHA256(out, n_out, hash_b);
    i->store->write(i->store, hash_b, out, n_out);
    enki_vector_destroy(out_inner_v);
    enki_vector_destroy(out_subpins_v);
}
enki_value enki_load_pin(enki_interpreter* i, uint8_t* hash) {
    size_t off = 0;
    uint8_t* out = i->store->read(i->store, hash);
    uint8_t ver = out[0]; // do something with later 
    off += 1;
    size_t n_subpins_s = 0;
    for(size_t k = 0; k < 8; k++) {
        n_subpins_s |= ((size_t)out[off] << (k * 8));
        off += 1;
    }
    size_t res_base = i->sp;
    size_t n = sizeof(enki_pin) + (n_subpins_s * sizeof(enki_value));
    enki_pin* pin = i->gc->alloc(n);
    pin->n_subpins_s = n_subpins_s;
    pin->h.size_s = n;
    pin->h.state_b = NF;
    pin->h.kind_b = ENKI_PIN;
    memcpy(pin->hash_b, hash, 32);
    i->stack[i->sp++] = PTR_TO_ENKI(pin);
    for(size_t k = 0; k < n_subpins_s; k++) {
        uint8_t hash_b[32];
        memcpy(hash_b, (out + off), 32);
        enki_value v = enki_load_pin(i, hash_b);
        pin = (enki_pin*)ENKI_TO_PTR(i->stack[res_base]);
        pin->subpins_v[k] = v;
        off += 32;
    }
    enki_value inner = enki_deserialize(i, out, &off);
    pin = (enki_pin*)ENKI_TO_PTR(i->stack[res_base]);
    pin->inner_v = inner;
    i->stack[res_base] = PTR_TO_ENKI(pin);
    i->sp = res_base;
    return PTR_TO_ENKI(pin);
}
void enki_collect_subpins(enki_value inner, enki_vector* subpins_v) {
    if(!IS_PTR(inner)) return;
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(inner);
    switch(h->kind_b) {
        case ENKI_NAT: return;
        case ENKI_LAW:
            enki_law* law = (enki_law*)ENKI_TO_PTR(inner);
            enki_collect_subpins(law->name_v, subpins_v);
            enki_collect_subpins(law->body_v, subpins_v);
            for(size_t k = 0; k < law->n_const_s; k++) {
                enki_collect_subpins(law->args_v[k], subpins_v);
            }
            return;
        case ENKI_PIN:
            enki_vector_push_copy(subpins_v, &inner);
            return;
        case ENKI_APP:
            enki_app* app = (enki_app*)ENKI_TO_PTR(inner);
            enki_collect_subpins(app->fn_v, subpins_v);
            for(size_t k = 0; k < app->n_args_s; k++) {
                enki_collect_subpins(law->args_v[k], subpins_v);
            }
            return;
        default:  break;
    }
}