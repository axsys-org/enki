#include "store_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <lmdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SILO_INDEX_BYTES 24u

typedef struct silo_backend {
  MDB_env* env;
  MDB_dbi objects;
  MDB_dbi meta;
  int pack_fd;
} silo_backend;

typedef struct pack_writer {
  int fd;
  uint64_t start;
  uint64_t written;
} pack_writer;

typedef struct pack_reader {
  int fd;
  uint64_t start;
  uint64_t len;
  uint64_t pos;
} pack_reader;

static const uint8_t index_magic[8] = {'p', 'i', 'n', 'p', 'a', 'c', 'k', 1};
static const uint8_t format_key[] = "format";
/* Persistence revision 2 changes object identity from canonical text to the
 * SHA-256 of the canonical Silo stream.  Revision-1 stores are rejected. */
static const uint8_t format_value[] = {'S', 'I', 'L', 'O', 2};
static const uint8_t root_key[] = "root";

static bool pack_error(char* err, size_t cap, const char* fmt, ...) {
  if (err != NULL && cap != 0) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, cap, fmt, ap);
    va_end(ap);
  }
  return false;
}

static void index_put64(uint8_t out[8], uint64_t v) {
  for (unsigned i = 0; i < 8; i++)
    out[i] = (uint8_t)(v >> (8u * i));
}

static uint64_t index_get64(const uint8_t in[8]) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; i++)
    v |= (uint64_t)in[i] << (8u * i);
  return v;
}

static void index_encode(uint8_t out[SILO_INDEX_BYTES], uint64_t off,
                         uint64_t len) {
  memcpy(out, index_magic, sizeof(index_magic));
  index_put64(out + 8, off);
  index_put64(out + 16, len);
}

static bool index_decode(const MDB_val* value, uint64_t* off, uint64_t* len) {
  if (value->mv_size != SILO_INDEX_BYTES ||
      memcmp(value->mv_data, index_magic, sizeof(index_magic)) != 0)
    return false;
  const uint8_t* b = value->mv_data;
  *off = index_get64(b + 8);
  *len = index_get64(b + 16);
  return true;
}

static bool pack_writer_write(void* ctx, const uint8_t* bytes, size_t len) {
  pack_writer* w = ctx;
  uint64_t max_off = (uint64_t)INT64_MAX;
  if (w->start > max_off || w->written > max_off - w->start ||
      len > max_off - w->start - w->written)
    return false;
  size_t done = 0;
  while (done < len) {
    off_t off = (off_t)(w->start + w->written + done);
    ssize_t n = pwrite(w->fd, bytes + done, len - done, off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    done += (size_t)n;
  }
  w->written += len;
  return true;
}

static bool pack_reader_read(void* ctx, uint8_t* bytes, size_t len) {
  pack_reader* r = ctx;
  if (r->pos > r->len || len > r->len - r->pos)
    return false;
  size_t done = 0;
  while (done < len) {
    off_t off = (off_t)(r->start + r->pos + done);
    ssize_t n = pread(r->fd, bytes + done, len - done, off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    done += (size_t)n;
  }
  r->pos += len;
  return true;
}

static bool pack_reader_rewind(void* ctx) {
  ((pack_reader*)ctx)->pos = 0;
  return true;
}

static bool silo_has(void* ctx, const uint8_t hash[32]) {
  silo_backend* b = ctx;
  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val value;
  int rc = mdb_get(txn, b->objects, &key, &value);
  uint64_t off = 0, len = 0;
  bool valid = rc == 0 && index_decode(&value, &off, &len) && len != 0;
  mdb_txn_abort(txn);
  if (!valid)
    return false;
  struct stat st;
  return fstat(b->pack_fd, &st) == 0 && st.st_size >= 0 &&
         off <= (uint64_t)st.st_size && len <= (uint64_t)st.st_size - off &&
         off <= (uint64_t)INT64_MAX && len <= (uint64_t)INT64_MAX - off;
}

static bool silo_meta_put(silo_backend* b, const void* key_bytes,
                          size_t key_len, const void* value_bytes,
                          size_t value_len) {
  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, 0, &txn) != 0)
    return false;
  MDB_val key = {.mv_size = key_len, .mv_data = (void*)key_bytes};
  MDB_val value = {.mv_size = value_len, .mv_data = (void*)value_bytes};
  if (mdb_put(txn, b->meta, &key, &value, 0) != 0) {
    mdb_txn_abort(txn);
    return false;
  }
  return mdb_txn_commit(txn) == 0;
}

static bool silo_meta_get(silo_backend* b, const void* key_bytes,
                          size_t key_len, void* out, size_t out_len) {
  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;
  MDB_val key = {.mv_size = key_len, .mv_data = (void*)key_bytes};
  MDB_val value;
  int rc = mdb_get(txn, b->meta, &key, &value);
  bool ok = rc == 0 && value.mv_size == out_len;
  if (ok)
    memcpy(out, value.mv_data, out_len);
  mdb_txn_abort(txn);
  return ok;
}

static bool silo_put_root(void* ctx, const uint8_t hash[32]) {
  return silo_meta_put(ctx, root_key, sizeof(root_key) - 1, hash, 32);
}

static bool silo_get_root(void* ctx, uint8_t hash[32]) {
  return silo_meta_get(ctx, root_key, sizeof(root_key) - 1, hash, 32);
}

static bool silo_unused_get(void* ctx, const uint8_t hash[32], uint8_t** out_b,
                            size_t* out_s) {
  (void)ctx;
  (void)hash;
  (void)out_b;
  (void)out_s;
  return false;
}

static bool silo_unused_put(void* ctx, const uint8_t hash[32],
                            const uint8_t* bytes, size_t len) {
  (void)ctx;
  (void)hash;
  (void)bytes;
  (void)len;
  return false;
}

static void silo_close(void* ctx) {
  silo_backend* b = ctx;
  if (b->pack_fd >= 0)
    (void)close(b->pack_fd);
  mdb_dbi_close(b->env, b->objects);
  mdb_dbi_close(b->env, b->meta);
  mdb_env_close(b->env);
  free(b);
}

bool pl_store_silo_put(pl_store* store, const uint8_t hash[32], pl_val root,
                       const pl_val* subpins, size_t nsub, char* err,
                       size_t err_cap) {
  if (store == NULL || store->format != PL_STORE_FORMAT_SILO_V1)
    return pack_error(err, err_cap, "store is not a Silo backend");
  silo_backend* b = store->be.ctx;
  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, 0, &txn) != 0)
    return pack_error(err, err_cap, "cannot begin Silo index transaction");
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val old;
  int rc = mdb_get(txn, b->objects, &key, &old);
  if (rc == 0) {
    uint64_t off = 0, len = 0;
    struct stat indexed;
    bool valid = index_decode(&old, &off, &len) && len != 0 &&
                 fstat(b->pack_fd, &indexed) == 0 && indexed.st_size >= 0 &&
                 off <= (uint64_t)indexed.st_size &&
                 len <= (uint64_t)indexed.st_size - off &&
                 off <= (uint64_t)INT64_MAX && len <= (uint64_t)INT64_MAX - off;
    mdb_txn_abort(txn);
    return valid ? true
                 : pack_error(err, err_cap,
                              "existing Silo object index is invalid");
  }
  if (rc != MDB_NOTFOUND) {
    mdb_txn_abort(txn);
    return pack_error(err, err_cap, "cannot query Silo object index");
  }

  struct stat st;
  if (fstat(b->pack_fd, &st) != 0 || st.st_size < 0) {
    mdb_txn_abort(txn);
    return pack_error(err, err_cap, "cannot determine pins.pack length");
  }
  pack_writer pw = {.fd = b->pack_fd, .start = (uint64_t)st.st_size};
  pl_silo_writer writer = {.ctx = &pw, .write = pack_writer_write};
  if (!pl_silo_encode(&writer, root, subpins, nsub, err, err_cap)) {
    mdb_txn_abort(txn);
    return false;
  }
  if (writer.pos != pw.written) {
    mdb_txn_abort(txn);
    return pack_error(err, err_cap, "Silo pack writer length mismatch");
  }
#ifdef F_FULLFSYNC
  if (fcntl(b->pack_fd, F_FULLFSYNC, 0) != 0) {
#else
  if (fsync(b->pack_fd) != 0) {
#endif
    mdb_txn_abort(txn);
    return pack_error(err, err_cap, "cannot sync pins.pack");
  }

  uint8_t index[SILO_INDEX_BYTES];
  index_encode(index, pw.start, writer.pos);
  MDB_val value = {.mv_size = sizeof(index), .mv_data = index};
  if (mdb_put(txn, b->objects, &key, &value, MDB_NOOVERWRITE) != 0) {
    mdb_txn_abort(txn);
    return pack_error(err, err_cap, "cannot publish Silo object index");
  }
  if (mdb_txn_commit(txn) != 0)
    return pack_error(err, err_cap, "cannot commit Silo object index");
  return true;
}

bool pl_store_silo_open(pl_store* store, const uint8_t hash[32],
                        pl_silo_reader* out, char* err, size_t err_cap) {
  if (store == NULL || store->format != PL_STORE_FORMAT_SILO_V1 || out == NULL)
    return pack_error(err, err_cap, "store is not a Silo backend");
  silo_backend* b = store->be.ctx;
  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, MDB_RDONLY, &txn) != 0)
    return pack_error(err, err_cap, "cannot begin Silo index read");
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val value;
  int rc = mdb_get(txn, b->objects, &key, &value);
  uint64_t off = 0, len = 0;
  bool valid = rc == 0 && index_decode(&value, &off, &len) && len != 0;
  mdb_txn_abort(txn);
  if (rc == MDB_NOTFOUND)
    return pack_error(err, err_cap, "missing Silo pin");
  if (!valid)
    return pack_error(err, err_cap, "invalid Silo object index");
  struct stat st;
  if (fstat(b->pack_fd, &st) != 0 || st.st_size < 0 ||
      off > (uint64_t)st.st_size || len > (uint64_t)st.st_size - off ||
      off > (uint64_t)INT64_MAX || len > (uint64_t)INT64_MAX - off)
    return pack_error(err, err_cap, "Silo object index is outside pins.pack");
  pack_reader* pr = calloc(1, sizeof(*pr));
  if (pr == NULL)
    return pack_error(err, err_cap, "out of memory for Silo reader");
  *pr = (pack_reader){.fd = b->pack_fd, .start = off, .len = len};
  *out = (pl_silo_reader){.ctx = pr,
                          .read = pack_reader_read,
                          .rewind = pack_reader_rewind,
                          .len = len};
  return true;
}

void pl_store_silo_close_reader(pl_silo_reader* r) {
  if (r == NULL)
    return;
  free(r->ctx);
  memset(r, 0, sizeof(*r));
}

pl_store* pl_store_new_silo(const char* path, size_t map_size) {
  silo_backend* b = calloc(1, sizeof(*b));
  if (b == NULL)
    return NULL;
  b->pack_fd = -1;
  if (mdb_env_create(&b->env) != 0 || mdb_env_set_maxdbs(b->env, 2) != 0 ||
      mdb_env_set_mapsize(b->env, map_size) != 0 ||
      mdb_env_open(b->env, path, 0, 0664) != 0)
    goto fail_env;

  char pack_path[4096];
  int path_len = snprintf(pack_path, sizeof(pack_path), "%s/pins.pack", path);
  if (path_len < 0 || (size_t)path_len >= sizeof(pack_path))
    goto fail_env;
  b->pack_fd = open(pack_path, O_RDWR | O_CREAT, 0664);
  if (b->pack_fd < 0)
    goto fail_env;

  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, 0, &txn) != 0)
    goto fail_pack;
  if (mdb_dbi_open(txn, "objects", MDB_CREATE, &b->objects) != 0 ||
      mdb_dbi_open(txn, "meta", MDB_CREATE, &b->meta) != 0) {
    mdb_txn_abort(txn);
    goto fail_pack;
  }
  MDB_val fkey = {.mv_size = sizeof(format_key) - 1,
                  .mv_data = (void*)format_key};
  MDB_val found;
  int rc = mdb_get(txn, b->meta, &fkey, &found);
  if (rc == MDB_NOTFOUND) {
    MDB_val fval = {.mv_size = sizeof(format_value),
                    .mv_data = (void*)format_value};
    rc = mdb_put(txn, b->meta, &fkey, &fval, 0);
  } else if (rc == 0 &&
             (found.mv_size != sizeof(format_value) ||
              memcmp(found.mv_data, format_value, sizeof(format_value)) != 0)) {
    rc = EINVAL;
  }
  if (rc != 0) {
    mdb_txn_abort(txn);
    goto fail_pack;
  }
  if (mdb_txn_commit(txn) != 0)
    goto fail_pack;

  pl_store* store = pl_store_new((pl_store_backend){
      .ctx = b,
      .get = silo_unused_get,
      .put = silo_unused_put,
      .has = silo_has,
      .put_root = silo_put_root,
      .get_root = silo_get_root,
      .close = silo_close,
  });
  store->format = PL_STORE_FORMAT_SILO_V1;
  return store;

fail_pack:
  if (b->pack_fd >= 0)
    (void)close(b->pack_fd);
fail_env:
  if (b->env != NULL)
    mdb_env_close(b->env);
  free(b);
  return NULL;
}
