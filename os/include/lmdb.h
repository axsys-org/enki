#ifndef ENKI_OS_LMDB_H
#define ENKI_OS_LMDB_H
#include <stddef.h>
typedef struct MDB_env MDB_env;
typedef struct MDB_txn MDB_txn;
typedef unsigned MDB_dbi;
typedef struct MDB_val { size_t mv_size; void* mv_data; } MDB_val;
#define MDB_RDONLY 0x20000u
#define MDB_CREATE 0x40000u
#define MDB_NOTFOUND (-30798)
int mdb_env_create(MDB_env** env);
int mdb_env_set_maxdbs(MDB_env* env, MDB_dbi dbs);
int mdb_env_set_mapsize(MDB_env* env, size_t size);
int mdb_env_open(MDB_env* env, const char* path, unsigned flags, unsigned mode);
void mdb_env_close(MDB_env* env);
int mdb_txn_begin(MDB_env* env, MDB_txn* parent, unsigned flags, MDB_txn** txn);
int mdb_txn_commit(MDB_txn* txn);
void mdb_txn_abort(MDB_txn* txn);
int mdb_dbi_open(MDB_txn* txn, const char* name, unsigned flags, MDB_dbi* dbi);
void mdb_dbi_close(MDB_env* env, MDB_dbi dbi);
int mdb_put(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data,
            unsigned flags);
int mdb_get(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data);
#endif
