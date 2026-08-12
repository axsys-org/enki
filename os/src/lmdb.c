#include <lmdb.h>
#include <stdlib.h>

struct MDB_env { int unused; };
struct MDB_txn { int unused; };

int mdb_env_create(MDB_env** env) { *env = calloc(1, sizeof(**env)); return *env ? 0 : -1; }
int mdb_env_set_maxdbs(MDB_env* env, MDB_dbi dbs) { (void)env; (void)dbs; return 0; }
int mdb_env_set_mapsize(MDB_env* env, size_t size) { (void)env; (void)size; return 0; }
int mdb_env_open(MDB_env* env, const char* path, unsigned flags, unsigned mode) {
  (void)env; (void)path; (void)flags; (void)mode; return -1;
}
void mdb_env_close(MDB_env* env) { free(env); }
int mdb_txn_begin(MDB_env* env, MDB_txn* parent, unsigned flags, MDB_txn** txn) {
  (void)env; (void)parent; (void)flags; *txn = NULL; return -1;
}
int mdb_txn_commit(MDB_txn* txn) { (void)txn; return -1; }
void mdb_txn_abort(MDB_txn* txn) { (void)txn; }
int mdb_dbi_open(MDB_txn* txn, const char* name, unsigned flags, MDB_dbi* dbi) {
  (void)txn; (void)name; (void)flags; (void)dbi; return -1;
}
void mdb_dbi_close(MDB_env* env, MDB_dbi dbi) { (void)env; (void)dbi; }
int mdb_put(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data, unsigned flags) {
  (void)txn; (void)dbi; (void)key; (void)data; (void)flags; return -1;
}
int mdb_get(MDB_txn* txn, MDB_dbi dbi, MDB_val* key, MDB_val* data) {
  (void)txn; (void)dbi; (void)key; (void)data; return MDB_NOTFOUND;
}
