#include <criterion/criterion.h>
#include <errno.h>
#include <fcntl.h>
#include <lmdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axsys/allocator.h"
#include "axsys/sha256.h"
#include "plan/canon.h"
#include "test_plan.h"

/* ── Interning ─────────────────────────────────────────────────────────── */

Test(pin, dedup_is_semantic) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* two structurally equal but distinct graphs intern to the same pin */
  pl_vpush(t, test_app2(t, 0, 7, test_law(t, 1, 0, 1)));
  pl_vpush(t, test_app2(t, 0, 7, test_law(t, 1, 0, 1)));
  pl_val p1 = pl_pin(t, t->vstack[base]);
  pl_val p2 = pl_pin(t, t->vstack[base + 1]);
  cr_assert_eq(p1, p2);
  cr_assert_eq(memcmp(pl_pin_hash(p1), pl_pin_hash(p2), 32), 0);
  test_rt_free(&rt);
}

Test(pin, sub_pins_collected_shallow) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, 42);
  pl_val inner = pl_pin(t, t->vstack[base]);
  pl_vpush(t, test_app2(t, 0, inner, inner));
  pl_val outer = pl_pin(t, t->vstack[base + 1]);
  pl_cell* p = pl_as(PL_TAG_PIN, outer);
  cr_assert_not_null(p);
  cr_assert_eq(pl_pin_npins(p), 1); /* deduplicated, shallow */
  cr_assert_eq(pl_pin_subpins(p)[0], inner);
  test_rt_free(&rt);
}

Test(pin, pinning_normalizes) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* pin of (id 42) under-applied row containing a redex via op:
   * pin the value Add 1 2 lazily applied */
  pl_vpush(t, 1);
  pl_vpush(t, 2);
  pl_vpush(t, test_op66_2(t, ax_s3('A', 'd', 'd'), t->vstack[base],
                          t->vstack[base + 1]));
  pl_val pinned = pl_pin(t, t->vstack[base + 2]);
  cr_assert_eq(pl_pin_body(pl_ptr(pinned)), 3);
  test_rt_free(&rt);
}

/* ── Round trips through the backend ───────────────────────────────────── */

static void roundtrip_via(pl_store* (*mk)(const char* dir), const char* dir) {
  uint8_t hash[32];

  /* First session: pin a structured value, remember its hash. */
  {
    pl_store* s = mk(dir);
    cr_assert_not_null(s);
    pl_heap* h = pl_heap_new(1 << 16, s);
    pl_thread* t = pl_thread_new(h);
    size_t base = t->vsp;
    pl_vpush(t, 42);
    pl_val inner = pl_pin(t, t->vstack[base]);
    pl_vpush(t, test_law(t, 2, ax_s2('h', 'i'), 1));
    pl_vpush(t, test_app2(t, 0, inner, t->vstack[base + 1]));
    pl_val pin = pl_pin(t, t->vstack[base + 2]);
    if (s->format == PL_STORE_FORMAT_SILO_V1) {
      char err[192] = {0};
      cr_assert_null(pl_pin_hash(pin));
      cr_assert(pl_store_save_root(s, pin, hash, err, sizeof(err)), "%s", err);
    } else {
      memcpy(hash, pl_pin_hash(pin), 32);
      cr_assert(pl_store_put_root(s, hash));
    }
    pl_thread_free(t);
    pl_heap_free(h);
    pl_store_free(s);
  }

  /* Second session: load by hash, re-pin, expect the identical hash. */
  {
    pl_store* s = mk(dir);
    cr_assert_not_null(s);
    pl_heap* h = pl_heap_new(1 << 16, s);
    pl_thread* t = pl_thread_new(h);

    uint8_t root[32];
    cr_assert(pl_store_get_root(s, root));
    cr_assert_eq(memcmp(root, hash, 32), 0);

    pl_val pin = pl_store_load(t, hash);
    cr_assert_eq(memcmp(pl_pin_hash(pin), hash, 32), 0);

    pl_cell* p = pl_as(PL_TAG_PIN, pin);
    cr_assert_not_null(p);
    pl_cell* body = pl_as(PL_TAG_APP, pl_pin_body(p));
    cr_assert_not_null(body);
    cr_assert_eq(pl_app_n(body), 2);
    cr_assert_eq(pl_app_head(body), 0);
    pl_cell* ip = pl_as(PL_TAG_PIN, pl_app_args(body)[0]);
    cr_assert_not_null(ip);
    cr_assert_eq(pl_pin_body(ip), 42);
    pl_cell* lp = pl_as(PL_TAG_LAW, pl_app_args(body)[1]);
    cr_assert_not_null(lp);
    cr_assert_eq(pl_law_arity(lp), 2);
    cr_assert_eq(pl_law_name(lp), (pl_val)ax_s2('h', 'i'));

    /* re-pinning the rehydrated body yields the same hash */
    size_t base = t->vsp;
    pl_vpush(t, pl_pin_body(p));
    pl_val again = pl_pin(t, t->vstack[base]);
    if (s->format == PL_STORE_FORMAT_SILO_V1) {
      char err[192] = {0};
      uint8_t again_hash[32];
      cr_assert_neq(again, pin);
      cr_assert_null(pl_pin_hash(again));
      cr_assert(pl_store_save_root(s, again, again_hash, err, sizeof(err)),
                "%s", err);
      cr_assert_eq(memcmp(again_hash, hash, 32), 0);
    } else {
      cr_assert_eq(again, pin);
    }

    pl_thread_free(t);
    pl_heap_free(h);
    pl_store_free(s);
  }
}

static pl_store* mk_lmdb(const char* dir) {
  return pl_store_new_lmdb(dir, (size_t)64 << 20);
}

static pl_store* mk_silo(const char* dir) {
  return pl_store_new_silo(dir, (size_t)64 << 20);
}

static void cleanup_store_dir(const char* dir, bool pack) {
  char path[128];
  if (pack) {
    snprintf(path, sizeof(path), "%s/pins.pack", dir);
    (void)unlink(path);
  }
  snprintf(path, sizeof(path), "%s/data.mdb", dir);
  (void)unlink(path);
  snprintf(path, sizeof(path), "%s/lock.mdb", dir);
  (void)unlink(path);
  (void)rmdir(dir);
}

static void silo_pin_nat(const char* dir, uint64_t natural, uint8_t hash[32]) {
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);
  pl_vpush(t, natural);
  pl_val pin = pl_pin(t, t->vstack[t->vsp - 1]);
  char err[192] = {0};
  cr_assert_null(pl_pin_hash(pin));
  cr_assert(pl_store_save_root(s, pin, hash, err, sizeof(err)), "%s", err);
  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
}

static void pin_sample(pl_store* s, uint8_t hash[32]) {
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);
  pl_vpush(t, test_law(t, 2, ax_s2('h', 'i'), 1));
  pl_vpush(t, test_app2(t, 0, t->vstack[t->vsp - 1], 42));
  pl_val pin = pl_pin(t, t->vstack[t->vsp - 1]);
  if (s->format == PL_STORE_FORMAT_SILO_V1) {
    char err[192] = {0};
    cr_assert(pl_store_save_root(s, pin, hash, err, sizeof(err)), "%s", err);
  } else {
    memcpy(hash, pl_pin_hash(pin), 32);
  }
  pl_thread_free(t);
  pl_heap_free(h);
}

static bool fd_read_exact(int fd, uint8_t* out, size_t len) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = read(fd, out + done, len - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += (size_t)n;
  }
  return true;
}

static bool silo_load_raises(const char* dir, const uint8_t hash[32]) {
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);
  pl_catch c;
  bool raised = false;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)pl_store_load(t, hash);
  } else {
    raised = true;
  }
  pl_catch_unwind(t, &c);
  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  return raised;
}

static void check_object_record(const char* dir, const uint8_t hash[32]) {
  MDB_env* env;
  MDB_txn* txn;
  MDB_dbi objects;
  cr_assert_eq(mdb_env_create(&env), 0);
  cr_assert_eq(mdb_env_set_maxdbs(env, 2), 0);
  cr_assert_eq(mdb_env_open(env, dir, MDB_RDONLY, 0), 0);
  cr_assert_eq(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn), 0);
  cr_assert_eq(mdb_dbi_open(txn, "objects", 0, &objects), 0);
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val value;
  cr_assert_eq(mdb_get(txn, objects, &key, &value), 0);
  cr_assert_eq(value.mv_size, 24);
  cr_assert_eq(memcmp(value.mv_data, "pinpack\1", 8), 0);
  mdb_txn_abort(txn);
  mdb_env_close(env);
}

static void delete_object_record(const char* dir, const uint8_t hash[32]) {
  MDB_env* env;
  MDB_txn* txn;
  MDB_dbi objects;
  cr_assert_eq(mdb_env_create(&env), 0);
  cr_assert_eq(mdb_env_set_maxdbs(env, 2), 0);
  cr_assert_eq(mdb_env_open(env, dir, 0, 0600), 0);
  cr_assert_eq(mdb_txn_begin(env, NULL, 0, &txn), 0);
  cr_assert_eq(mdb_dbi_open(txn, "objects", 0, &objects), 0);
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  cr_assert_eq(mdb_del(txn, objects, &key, NULL), 0);
  cr_assert_eq(mdb_txn_commit(txn), 0);
  mdb_env_close(env);
}

static void put_object_record(const char* dir, const uint8_t hash[32],
                              const uint8_t* bytes, size_t len) {
  MDB_env* env;
  MDB_txn* txn;
  MDB_dbi objects;
  cr_assert_eq(mdb_env_create(&env), 0);
  cr_assert_eq(mdb_env_set_maxdbs(env, 2), 0);
  cr_assert_eq(mdb_env_open(env, dir, 0, 0600), 0);
  cr_assert_eq(mdb_txn_begin(env, NULL, 0, &txn), 0);
  cr_assert_eq(mdb_dbi_open(txn, "objects", 0, &objects), 0);
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val value = {.mv_size = len, .mv_data = (void*)bytes};
  cr_assert_eq(mdb_put(txn, objects, &key, &value, 0), 0);
  cr_assert_eq(mdb_txn_commit(txn), 0);
  mdb_env_close(env);
}

static void test_put64le(uint8_t bytes[8], uint64_t value) {
  for (unsigned i = 0; i < 8; i++)
    bytes[i] = (uint8_t)(value >> (8u * i));
}

static size_t silo_last_txnid(const char* dir) {
  MDB_env* env;
  MDB_envinfo info;
  cr_assert_eq(mdb_env_create(&env), 0);
  cr_assert_eq(mdb_env_open(env, dir, MDB_RDONLY, 0), 0);
  cr_assert_eq(mdb_env_info(env, &info), 0);
  mdb_env_close(env);
  return (size_t)info.me_last_txnid;
}

Test(store, lmdb_round_trip) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-store-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  roundtrip_via(mk_lmdb, dir);
  char path[96];
  snprintf(path, sizeof(path), "%s/data.mdb", dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/lock.mdb", dir);
  unlink(path);
  rmdir(dir);
}

Test(store, silo_pack_round_trip) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  roundtrip_via(mk_silo, dir);

  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  struct stat st;
  cr_assert_eq(stat(path, &st), 0);
  cr_assert_gt(st.st_size, 0);
  cleanup_store_dir(dir, true);
}

Test(store, silo_pin_defers_hash_and_io_until_save) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-defer-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);

  pl_vpush(t, 42);
  pl_val unreachable = pl_pin(t, t->vstack[t->vsp - 1]);
  pl_vpush(t, 43);
  pl_val root = pl_pin(t, t->vstack[t->vsp - 1]);
  cr_assert_null(pl_pin_hash(unreachable));
  cr_assert_null(pl_pin_hash(root));

  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  struct stat before, after;
  cr_assert_eq(stat(path, &before), 0);
  cr_assert_eq(before.st_size, 0);
  uint8_t absent[32];
  cr_assert_not(pl_store_get_root(s, absent));

  uint8_t hash[32];
  char err[192] = {0};
  cr_assert(pl_store_save_root(s, root, hash, err, sizeof(err)), "%s", err);
  cr_assert_not_null(pl_pin_hash(root));
  cr_assert_null(pl_pin_hash(unreachable));
  cr_assert_eq(stat(path, &after), 0);
  cr_assert_gt(after.st_size, 0);

  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  cleanup_store_dir(dir, true);
}

Test(store, silo_batches_closure_in_one_transaction) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-batch-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);

  pl_store* initialized = mk_silo(dir);
  cr_assert_not_null(initialized);
  pl_store_free(initialized);
  size_t before_txnid = silo_last_txnid(dir);

  uint8_t root_hash[32];
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);
  pl_vpush(t, 0);
  pl_val root = pl_pin(t, t->vstack[t->vsp - 1]);
  for (uint64_t i = 1; i <= 32; i++) {
    pl_vpush(t, test_app2(t, 0, root, i));
    root = pl_pin(t, t->vstack[t->vsp - 1]);
  }

  char err[192] = {0};
  cr_assert(pl_store_save_root(s, root, root_hash, err, sizeof(err)), "%s",
            err);
  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  struct stat first, second;
  cr_assert_eq(stat(path, &first), 0);
  cr_assert(pl_store_save_root(s, root, NULL, err, sizeof(err)), "%s", err);
  cr_assert_eq(stat(path, &second), 0);
  cr_assert_eq(second.st_size, first.st_size);

  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  size_t after_txnid = silo_last_txnid(dir);
  /* Reopening the backend may consume one no-op initialization transaction;
   * the 33-object Save itself must consume only one more. */
  cr_assert_geq(after_txnid, before_txnid + 1);
  cr_assert_leq(after_txnid, before_txnid + 2);
  cr_assert_not(silo_load_raises(dir, root_hash));
  cleanup_store_dir(dir, true);
}

Test(store, silo_failed_batch_keeps_provisional_pins_retryable) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-retry-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);

  pl_vpush(t, 42);
  pl_vpush(t, 42);
  pl_val first = pl_pin(t, t->vstack[t->vsp - 2]);
  pl_val second = pl_pin(t, t->vstack[t->vsp - 1]);
  pl_vpush(t, test_app2(t, 0, first, second));
  size_t valid_at = t->vsp - 1;
  pl_vpush(t, test_law(t, 0, 0, 1));
  pl_vpush(t, test_law(t, 1, t->vstack[t->vsp - 1], t->vstack[valid_at]));
  pl_val invalid = pl_pin(t, t->vstack[t->vsp - 1]);
  cr_assert_eq(pl_pin_npins(pl_ptr(invalid)), 2);

  char err[192] = {0};
  cr_assert_not(pl_store_save_root(s, invalid, NULL, err, sizeof(err)));
  cr_assert(strstr(err, "LAW arity") != NULL, "%s", err);
  cr_assert_not(pl_pin_is_hashed(first));
  cr_assert_not(pl_pin_is_hashed(second));
  cr_assert_not(pl_pin_is_hashed(invalid));
  cr_assert_eq(pl_pin_npins(pl_ptr(invalid)), 2);

  pl_val retry = pl_pin(t, t->vstack[valid_at]);
  cr_assert(pl_store_save_root(s, retry, NULL, err, sizeof(err)), "%s", err);
  cr_assert(pl_pin_is_hashed(first));
  cr_assert(pl_pin_is_hashed(second));
  cr_assert_eq(memcmp(pl_pin_hash(first), pl_pin_hash(second), 32), 0);
  cr_assert_eq(pl_pin_npins(pl_ptr(retry)), 1);

  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  cleanup_store_dir(dir, true);
}

Test(store, silo_failed_batch_truncates_flushed_pack_tail) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-rollback-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 18, s);
  pl_thread* t = pl_thread_new(h);

  const size_t byte_len = (size_t)128 * 1024 + 17;
  uint8_t* bytes = malloc(byte_len);
  cr_assert_not_null(bytes);
  for (size_t i = 0; i < byte_len; i++)
    bytes[i] = (uint8_t)(i * 29u + 5u);
  bytes[byte_len - 1] = 1;
  pl_vpush(t, pl_nat_from_bytes(t, bytes, byte_len));
  pl_val large = pl_pin(t, t->vstack[t->vsp - 1]);
  pl_vpush(t, test_law(t, 0, 0, large));
  pl_val invalid = pl_pin(t, t->vstack[t->vsp - 1]);

  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  struct stat before, after;
  cr_assert_eq(stat(path, &before), 0);
  char err[192] = {0};
  cr_assert_not(pl_store_save_root(s, invalid, NULL, err, sizeof(err)));
  cr_assert(strstr(err, "LAW arity") != NULL, "%s", err);
  cr_assert_eq(stat(path, &after), 0);
  cr_assert_eq(after.st_size, before.st_size);
  cr_assert_not(pl_pin_is_hashed(large));
  cr_assert_not(pl_pin_is_hashed(invalid));

  cr_assert(pl_store_save_root(s, large, NULL, err, sizeof(err)), "%s", err);
  cr_assert(pl_pin_is_hashed(large));

  free(bytes);
  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  cleanup_store_dir(dir, true);
}

Test(store, silo_buffered_write_and_mapped_read_cross_chunk_boundaries) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-large-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  const size_t byte_len = (size_t)128 * 1024 + 17;
  uint8_t* bytes = malloc(byte_len);
  cr_assert_not_null(bytes);
  for (size_t i = 0; i < byte_len; i++)
    bytes[i] = (uint8_t)(i * 17u + 3u);
  bytes[byte_len - 1] = 1;

  uint8_t large_hash[32];
  {
    pl_store* s = mk_silo(dir);
    cr_assert_not_null(s);
    pl_heap* h = pl_heap_new(1 << 18, s);
    pl_thread* t = pl_thread_new(h);

    pl_vpush(t, 7);
    pl_val prefix = pl_pin(t, t->vstack[t->vsp - 1]);
    char err[192] = {0};
    cr_assert(pl_store_save_root(s, prefix, NULL, err, sizeof(err)), "%s", err);

    pl_vpush(t, pl_nat_from_bytes(t, bytes, byte_len));
    pl_val large = pl_pin(t, t->vstack[t->vsp - 1]);
    cr_assert(pl_store_save_root(s, large, large_hash, err, sizeof(err)), "%s",
              err);
    pl_thread_free(t);
    pl_heap_free(h);
    pl_store_free(s);
  }

  {
    pl_store* s = mk_silo(dir);
    cr_assert_not_null(s);
    pl_heap* h = pl_heap_new(1 << 18, s);
    pl_thread* t = pl_thread_new(h);
    pl_val loaded = pl_store_load(t, large_hash);
    pl_val body = pl_pin_body(pl_ptr(loaded));
    cr_assert_eq(pl_nat_byte_len(body), byte_len);
    cr_assert_eq(pl_nat_byte_at(body, 0), bytes[0]);
    cr_assert_eq(pl_nat_byte_at(body, 65536), bytes[65536]);
    cr_assert_eq(pl_nat_byte_at(body, byte_len - 1), bytes[byte_len - 1]);
    pl_thread_free(t);
    pl_heap_free(h);
    pl_store_free(s);
  }

  free(bytes);
  cleanup_store_dir(dir, true);
}

Test(store, silo_equal_provisional_pins_share_wire_identity) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-equal-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);

  pl_vpush(t, 42);
  pl_vpush(t, 42);
  pl_val first = pl_pin(t, t->vstack[t->vsp - 2]);
  pl_val second = pl_pin(t, t->vstack[t->vsp - 1]);
  cr_assert_neq(first, second);
  cr_assert_eq(test_op66_2(t, ax_s5('E', 'q', 'u', 'a', 'l'), first, second),
               1);
  pl_vpush(t, test_app2(t, 0, first, second));
  pl_val outer = pl_pin(t, t->vstack[t->vsp - 1]);
  cr_assert_eq(pl_pin_npins(pl_ptr(outer)), 2);

  char err[192] = {0};
  cr_assert(pl_store_save_root(s, outer, NULL, err, sizeof(err)), "%s", err);
  cr_assert_eq(memcmp(pl_pin_hash(first), pl_pin_hash(second), 32), 0);
  cr_assert_eq(pl_pin_npins(pl_ptr(outer)), 1);

  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  cleanup_store_dir(dir, true);
}

Test(store, silo_prints_pending_pins_as_liquid) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-print-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  pl_store* s = mk_silo(dir);
  cr_assert_not_null(s);
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);

  pl_vpush(t, test_law(t, 1, (pl_val)'h', 1));
  pl_val inner = pl_pin(t, t->vstack[t->vsp - 1]);
  cr_assert_not(pl_pin_is_hashed(inner));
  char* shown = pl_show_val(ax_allocator_system(), inner, NULL);
  cr_assert_str_eq(shown, "liquid");
  ax_free(ax_allocator_system(), shown);

  pl_vpush(t, test_app2(t, 0, inner, 7));
  shown = pl_show_val(ax_allocator_system(), t->vstack[t->vsp - 1], NULL);
  cr_assert_str_eq(shown, "(0 liquid 7)");
  ax_free(ax_allocator_system(), shown);

  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  cleanup_store_dir(dir, true);
}

Test(store, silo_rejects_text_hash_store_revision) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-v1-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);

  MDB_env* env;
  MDB_txn* txn;
  MDB_dbi meta, objects;
  cr_assert_eq(mdb_env_create(&env), 0);
  cr_assert_eq(mdb_env_set_maxdbs(env, 2), 0);
  cr_assert_eq(mdb_env_open(env, dir, 0, 0600), 0);
  cr_assert_eq(mdb_txn_begin(env, NULL, 0, &txn), 0);
  cr_assert_eq(mdb_dbi_open(txn, "objects", MDB_CREATE, &objects), 0);
  cr_assert_eq(mdb_dbi_open(txn, "meta", MDB_CREATE, &meta), 0);
  static const char key[] = "format";
  static const uint8_t old_format[] = {'S', 'I', 'L', 'O', 1};
  MDB_val k = {.mv_size = sizeof(key) - 1, .mv_data = (void*)key};
  MDB_val v = {.mv_size = sizeof(old_format), .mv_data = (void*)old_format};
  cr_assert_eq(mdb_put(txn, meta, &k, &v, 0), 0);
  cr_assert_eq(mdb_txn_commit(txn), 0);
  mdb_env_close(env);

  cr_assert_null(mk_silo(dir));
  cleanup_store_dir(dir, true);
}

Test(store, silo_deduplicates_and_tolerates_orphan_pack_tails) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-tail-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  uint8_t first[32], again[32], second[32];
  silo_pin_nat(dir, 42, first);
  check_object_record(dir, first);

  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  struct stat before, after;
  cr_assert_eq(stat(path, &before), 0);
  silo_pin_nat(dir, 42, again);
  cr_assert_eq(memcmp(first, again, 32), 0);
  cr_assert_eq(stat(path, &after), 0);
  cr_assert_eq(after.st_size, before.st_size);

  int fd = open(path, O_WRONLY | O_APPEND);
  cr_assert_geq(fd, 0);
  static const uint8_t orphan[] = {0xde, 0xad, 0xbe, 0xef};
  cr_assert_eq(write(fd, orphan, sizeof(orphan)), (ssize_t)sizeof(orphan));
  cr_assert_eq(close(fd), 0);
  cr_assert_not(silo_load_raises(dir, first));

  silo_pin_nat(dir, 43, second);
  check_object_record(dir, second);
  cr_assert_not(silo_load_raises(dir, first));
  cr_assert_not(silo_load_raises(dir, second));
  cleanup_store_dir(dir, true);
}

Test(store, silo_rejects_indexed_pack_truncation) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-truncate-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  uint8_t hash[32];
  silo_pin_nat(dir, 99, hash);

  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  struct stat st;
  cr_assert_eq(stat(path, &st), 0);
  cr_assert_gt(st.st_size, 0);
  cr_assert_eq(truncate(path, st.st_size - 1), 0);
  cr_assert(silo_load_raises(dir, hash));
  cleanup_store_dir(dir, true);
}

Test(store, silo_rejects_corrupt_object_indices) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-index-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  uint8_t hash[32];
  silo_pin_nat(dir, 99, hash);
  put_object_record(dir, hash, (const uint8_t[]){0}, 1);
  cr_assert(silo_load_raises(dir, hash));
  cleanup_store_dir(dir, true);
}

Test(store, silo_rejects_semantic_hash_mismatches) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-hash-mismatch-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  uint8_t hash[32];
  silo_pin_nat(dir, 99, hash);

  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  int fd = open(path, O_RDWR);
  cr_assert_geq(fd, 0);
  struct stat st;
  cr_assert_eq(fstat(fd, &st), 0);
  cr_assert_gt(st.st_size, 0);
  uint8_t replacement = 100;
  cr_assert_eq(pwrite(fd, &replacement, 1, st.st_size - 1), 1);
  cr_assert_eq(close(fd), 0);

  /* The modified stream is well formed but denotes a different PLAN value. */
  cr_assert(silo_load_raises(dir, hash));
  cleanup_store_dir(dir, true);
}

Test(store, silo_reports_missing_referents) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-missing-ref-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  uint8_t inner_hash[32], outer_hash[32];
  {
    pl_store* s = mk_silo(dir);
    cr_assert_not_null(s);
    pl_heap* h = pl_heap_new(1 << 16, s);
    pl_thread* t = pl_thread_new(h);
    pl_vpush(t, 42);
    pl_val inner = pl_pin(t, t->vstack[t->vsp - 1]);
    pl_vpush(t, test_app2(t, 0, inner, 7));
    pl_val outer = pl_pin(t, t->vstack[t->vsp - 1]);
    char err[192] = {0};
    cr_assert(pl_store_save_root(s, outer, NULL, err, sizeof(err)), "%s", err);
    memcpy(inner_hash, pl_pin_hash(inner), 32);
    memcpy(outer_hash, pl_pin_hash(outer), 32);
    pl_thread_free(t);
    pl_heap_free(h);
    pl_store_free(s);
  }
  delete_object_record(dir, inner_hash);
  cr_assert(silo_load_raises(dir, outer_hash));
  cleanup_store_dir(dir, true);
}

Test(store, silo_rejects_cyclic_referent_loads) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-cycle-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  uint8_t inner_hash[32], outer_hash[32];
  {
    pl_store* s = mk_silo(dir);
    cr_assert_not_null(s);
    pl_heap* h = pl_heap_new(1 << 16, s);
    pl_thread* t = pl_thread_new(h);
    pl_vpush(t, 42);
    pl_val inner = pl_pin(t, t->vstack[t->vsp - 1]);
    pl_vpush(t, test_app2(t, 0, inner, 7));
    pl_val outer = pl_pin(t, t->vstack[t->vsp - 1]);
    char err[192] = {0};
    cr_assert(pl_store_save_root(s, outer, NULL, err, sizeof(err)), "%s", err);
    memcpy(inner_hash, pl_pin_hash(inner), 32);
    memcpy(outer_hash, pl_pin_hash(outer), 32);
    pl_thread_free(t);
    pl_heap_free(h);
    pl_store_free(s);
  }

  /* Replace the inner index with a valid Silo object whose direct PIN is
   * the already-loading outer object. The semantic mismatch is never reached:
   * dependency resolution must reject the cycle first. */
  uint8_t cycle[41] = {'S', 'I', 'L', 'O', 1, 1, 1};
  memcpy(cycle + 7, outer_hash, 32);
  cycle[39] = 0xf1;
  cycle[40] = 0;
  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  int fd = open(path, O_WRONLY | O_APPEND);
  cr_assert_geq(fd, 0);
  off_t offset = lseek(fd, 0, SEEK_END);
  cr_assert_geq(offset, 0);
  cr_assert_eq(write(fd, cycle, sizeof(cycle)), (ssize_t)sizeof(cycle));
  cr_assert_eq(close(fd), 0);
  uint8_t record[24] = {'p', 'i', 'n', 'p', 'a', 'c', 'k', 1};
  test_put64le(record + 8, (uint64_t)offset);
  test_put64le(record + 16, sizeof(cycle));
  put_object_record(dir, inner_hash, record, sizeof(record));

  cr_assert(silo_load_raises(dir, outer_hash));
  cleanup_store_dir(dir, true);
}

Test(store, silo_hashes_canonical_stream_not_canonical_text) {
  uint8_t memory_hash[32], silo_hash[32];
  pl_store* memory = pl_store_new_mem();
  pin_sample(memory, memory_hash);
  pl_store_free(memory);

  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-hash-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  pl_store* silo = mk_silo(dir);
  cr_assert_not_null(silo);
  pin_sample(silo, silo_hash);
  pl_store_free(silo);
  cr_assert_neq(memcmp(memory_hash, silo_hash, 32), 0);

  char path[96];
  snprintf(path, sizeof(path), "%s/pins.pack", dir);
  int fd = open(path, O_RDONLY);
  cr_assert_geq(fd, 0);
  struct stat st;
  cr_assert_eq(fstat(fd, &st), 0);
  cr_assert_gt(st.st_size, 0);
  uint8_t* bytes = malloc((size_t)st.st_size);
  cr_assert_not_null(bytes);
  cr_assert(fd_read_exact(fd, bytes, (size_t)st.st_size));
  cr_assert_eq(close(fd), 0);
  uint8_t expected[32];
  ax_sha256(bytes, (size_t)st.st_size, expected);
  free(bytes);
  cr_assert_eq(memcmp(expected, silo_hash, sizeof(expected)), 0);
  cleanup_store_dir(dir, true);
}

Test(store, silo_serializes_concurrent_process_writers) {
  char dir[64];
  snprintf(dir, sizeof(dir), "/tmp/enki-test-silo-writers-%lu",
           (unsigned long)getpid());
  cr_assert(mkdir(dir, 0700) == 0 || errno == EEXIST);
  pl_store* initialized = mk_silo(dir);
  cr_assert_not_null(initialized);
  pl_store_free(initialized);

  int result[2][2];
  cr_assert_eq(pipe(result[0]), 0);
  cr_assert_eq(pipe(result[1]), 0);
  pid_t children[2];
  for (unsigned i = 0; i < 2; i++) {
    children[i] = fork();
    cr_assert_neq(children[i], -1);
    if (children[i] == 0) {
      (void)close(result[i][0]);
      uint8_t hash[32];
      silo_pin_nat(dir, 100 + i, hash);
      bool ok =
          write(result[i][1], hash, sizeof(hash)) == (ssize_t)sizeof(hash);
      (void)close(result[i][1]);
      _exit(ok ? 0 : 1);
    }
    (void)close(result[i][1]);
  }

  uint8_t hash[2][32];
  for (unsigned i = 0; i < 2; i++) {
    cr_assert(fd_read_exact(result[i][0], hash[i], sizeof(hash[i])));
    (void)close(result[i][0]);
    int status;
    cr_assert_eq(waitpid(children[i], &status, 0), children[i]);
    cr_assert(WIFEXITED(status));
    cr_assert_eq(WEXITSTATUS(status), 0);
  }
  cr_assert_neq(memcmp(hash[0], hash[1], 32), 0);
  cr_assert_not(silo_load_raises(dir, hash[0]));
  cr_assert_not(silo_load_raises(dir, hash[1]));
  cleanup_store_dir(dir, true);
}

Test(store, missing_pin_raises) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  uint8_t nohash[32] = {1, 2, 3};
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) == 0) {
    (void)pl_store_load(t, nohash);
    cr_assert_fail("expected raise");
  }
  pl_catch_unwind(t, &c);
  cr_assert_not_null(t->exn_msg);
  test_rt_free(&rt);
}
