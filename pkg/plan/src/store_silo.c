#include "store_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <lmdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axsys/ds.h"

#define SILO_INDEX_BYTES  24u
#define PACK_BUFFER_BYTES (64u * 1024u)

typedef struct silo_backend {
  MDB_env* env;
  MDB_dbi objects;
  MDB_dbi meta;
  MDB_dbi codecache; /* side KV: (compiler, law) -> code-row pin hash */
  int pack_fd;
} silo_backend;

typedef struct pack_reader {
  void* mapping;
  size_t mapping_len;
  const uint8_t* bytes;
  size_t len;
  size_t pos;
} pack_reader;

typedef struct batch_hash_entry {
  pl_hash key;
  uint8_t value;
} batch_hash_entry;

struct pl_silo_batch {
  silo_backend* backend;
  MDB_txn* txn;
  uint64_t pack_start;
  uint64_t pack_flushed;
  uint64_t pack_end;
  size_t buffered;
  bool changed;
  bool pack_dirty;
  batch_hash_entry* pending;
  uint8_t buffer[PACK_BUFFER_BYTES];
};

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

static bool pack_pwrite_exact(int fd, const uint8_t* bytes, size_t len,
                              uint64_t start) {
  if (start > (uint64_t)INT64_MAX || len > (uint64_t)INT64_MAX - start)
    return false;
  size_t done = 0;
  while (done < len) {
    off_t off = (off_t)(start + done);
    ssize_t n = pwrite(fd, bytes + done, len - done, off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    done += (size_t)n;
  }
  return true;
}

static bool pack_reader_read(void* ctx, uint8_t* bytes, size_t len) {
  pack_reader* r = ctx;
  if (r->pos > r->len || len > r->len - r->pos)
    return false;
  memcpy(bytes, r->bytes + r->pos, len);
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

/* Generic KV over the side `codecache` DBI.  Silo's pin objects only
 * ever move through the validated batch path; this table serves small
 * derived artifacts (the bytecode compile cache) keyed by tagged
 * hashes that cannot collide with anything content-addressed. */
static bool silo_kv_get(void* ctx, const uint8_t hash[32], uint8_t** out_b,
                        size_t* out_s) {
  silo_backend* b = ctx;
  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;
  MDB_val k = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val v;
  if (mdb_get(txn, b->codecache, &k, &v) != 0) {
    mdb_txn_abort(txn);
    return false;
  }
  *out_b = malloc(v.mv_size);
  ax_assume(*out_b != NULL, "oom");
  memcpy(*out_b, v.mv_data, v.mv_size);
  *out_s = v.mv_size;
  mdb_txn_abort(txn);
  return true;
}

static bool silo_kv_put(void* ctx, const uint8_t hash[32], const uint8_t* bytes,
                        size_t len) {
  silo_backend* b = ctx;
  MDB_txn* txn;
  if (mdb_txn_begin(b->env, NULL, 0, &txn) != 0)
    return false;
  MDB_val k = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val v = {.mv_size = len, .mv_data = (void*)bytes};
  if (mdb_put(txn, b->codecache, &k, &v, 0) != 0) {
    mdb_txn_abort(txn);
    return false;
  }
  return mdb_txn_commit(txn) == 0;
}

static void silo_close(void* ctx) {
  silo_backend* b = ctx;
  if (b->pack_fd >= 0)
    (void)close(b->pack_fd);
  mdb_dbi_close(b->env, b->objects);
  mdb_dbi_close(b->env, b->meta);
  mdb_dbi_close(b->env, b->codecache);
  mdb_env_close(b->env);
  free(b);
}

static bool batch_flush(pl_silo_batch* batch, char* err, size_t err_cap) {
  if (batch->buffered == 0)
    return true;
  if (!pack_pwrite_exact(batch->backend->pack_fd, batch->buffer,
                         batch->buffered, batch->pack_flushed))
    return pack_error(err, err_cap, "cannot append pins.pack");
  batch->pack_flushed += batch->buffered;
  batch->buffered = 0;
  batch->pack_dirty = true;
  return true;
}

static bool batch_append(pl_silo_batch* batch, const uint8_t* bytes, size_t len,
                         char* err, size_t err_cap) {
  if (len > UINT64_MAX - batch->pack_end ||
      len > (uint64_t)INT64_MAX - batch->pack_end)
    return pack_error(err, err_cap, "pins.pack length overflow");
  size_t left = len;
  while (left != 0) {
    size_t room = sizeof(batch->buffer) - batch->buffered;
    if (room == 0) {
      if (!batch_flush(batch, err, err_cap))
        return false;
      room = sizeof(batch->buffer);
    }
    size_t take = left < room ? left : room;
    memcpy(batch->buffer + batch->buffered, bytes + (len - left), take);
    batch->buffered += take;
    batch->pack_end += take;
    left -= take;
  }
  return true;
}

bool pl_store_silo_batch_begin(pl_store* store, pl_silo_batch** out, char* err,
                               size_t err_cap) {
  if (out == NULL)
    return pack_error(err, err_cap, "invalid Silo batch output");
  *out = NULL;
  if (store == NULL || store->format != PL_STORE_FORMAT_SILO_V1)
    return pack_error(err, err_cap, "store is not a Silo backend");
  pl_silo_batch* batch = calloc(1, sizeof(*batch));
  if (batch == NULL)
    return pack_error(err, err_cap, "out of memory for Silo batch");
  batch->backend = store->be.ctx;
  if (mdb_txn_begin(batch->backend->env, NULL, 0, &batch->txn) != 0) {
    free(batch);
    return pack_error(err, err_cap, "cannot begin Silo save transaction");
  }

  /* The LMDB write transaction is also the cross-process append lock.  It
   * must be held before choosing the pack offset. */
  struct stat st;
  if (fstat(batch->backend->pack_fd, &st) != 0 || st.st_size < 0 ||
      (uint64_t)st.st_size > (uint64_t)INT64_MAX) {
    mdb_txn_abort(batch->txn);
    free(batch);
    return pack_error(err, err_cap, "cannot determine pins.pack length");
  }
  batch->pack_start = (uint64_t)st.st_size;
  batch->pack_flushed = batch->pack_start;
  batch->pack_end = batch->pack_start;
  *out = batch;
  return true;
}

bool pl_store_silo_batch_contains(pl_silo_batch* batch, const uint8_t hash[32],
                                  bool* out, char* err, size_t err_cap) {
  if (batch == NULL || batch->txn == NULL || hash == NULL || out == NULL)
    return pack_error(err, err_cap, "invalid Silo batch query");
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val value;
  int rc = mdb_get(batch->txn, batch->backend->objects, &key, &value);
  if (rc == MDB_NOTFOUND) {
    *out = false;
    return true;
  }
  if (rc != 0)
    return pack_error(err, err_cap, "cannot query Silo object index");
  pl_hash hash_key;
  memcpy(hash_key.b, hash, sizeof(hash_key.b));
  uint64_t extent = ax_hmgeti(batch->pending, hash_key) >= 0
                        ? batch->pack_end
                        : batch->pack_start;
  uint64_t off = 0, len = 0;
  if (!index_decode(&value, &off, &len) || len == 0 || off > extent ||
      len > extent - off || off > (uint64_t)INT64_MAX ||
      len > (uint64_t)INT64_MAX - off)
    return pack_error(err, err_cap, "existing Silo object index is invalid");
  *out = true;
  return true;
}

bool pl_store_silo_batch_put(pl_silo_batch* batch, const uint8_t hash[32],
                             const uint8_t* bytes, size_t len, char* err,
                             size_t err_cap) {
  if (batch == NULL || batch->txn == NULL || hash == NULL || bytes == NULL ||
      len == 0)
    return pack_error(err, err_cap, "invalid Silo batch object");
  bool present;
  if (!pl_store_silo_batch_contains(batch, hash, &present, err, err_cap))
    return false;
  if (present)
    return true;

  uint64_t off = batch->pack_end;
  if (!batch_append(batch, bytes, len, err, err_cap))
    return false;
  uint8_t index[SILO_INDEX_BYTES];
  index_encode(index, off, len);
  MDB_val key = {.mv_size = 32, .mv_data = (void*)hash};
  MDB_val value = {.mv_size = sizeof(index), .mv_data = index};
  if (mdb_put(batch->txn, batch->backend->objects, &key, &value,
              MDB_NOOVERWRITE) != 0)
    return pack_error(err, err_cap, "cannot publish Silo object index");
  pl_hash hash_key;
  memcpy(hash_key.b, hash, sizeof(hash_key.b));
  ax_hmput(batch->pending, hash_key, 1);
  batch->changed = true;
  return true;
}

void pl_store_silo_batch_abort(pl_silo_batch* batch) {
  if (batch == NULL)
    return;
  if (batch->txn != NULL) {
    /* The active LMDB write transaction still owns the cross-process append
     * lock, so no cooperating writer can have appended behind this batch.
     * Best-effort rollback prevents retryable failures from accumulating a
     * full orphan closure; a failed truncate remains safe as an orphan tail. */
    if (batch->pack_end > batch->pack_start &&
        ftruncate(batch->backend->pack_fd, (off_t)batch->pack_start) != 0) {
      /* glibc marks ftruncate warn-unused-result; the failure is
       * acceptable here per the comment above */
    }
    mdb_txn_abort(batch->txn);
  }
  ax_hmfree(batch->pending);
  free(batch);
}

bool pl_store_silo_batch_commit(pl_silo_batch* batch,
                                const uint8_t root_hash[32], char* err,
                                size_t err_cap) {
  /* root_hash == NULL: commit the batch's objects without touching the
   * store root (derived-artifact writes, e.g. the compile cache). */
  if (batch == NULL || batch->txn == NULL) {
    pl_store_silo_batch_abort(batch);
    return pack_error(err, err_cap, "invalid Silo batch commit");
  }
  bool root_changed = false;
  MDB_val root_k = {.mv_size = sizeof(root_key) - 1,
                    .mv_data = (void*)root_key};
  if (root_hash != NULL) {
    bool root_present = false;
    if (!pl_store_silo_batch_contains(batch, root_hash, &root_present, err,
                                      err_cap)) {
      pl_store_silo_batch_abort(batch);
      return false;
    }
    if (!root_present) {
      pl_store_silo_batch_abort(batch);
      return pack_error(err, err_cap, "Silo root object is not indexed");
    }

    MDB_val old_root;
    int root_rc = mdb_get(batch->txn, batch->backend->meta, &root_k, &old_root);
    root_changed =
        root_rc == MDB_NOTFOUND ||
        (root_rc == 0 && (old_root.mv_size != 32 ||
                          memcmp(old_root.mv_data, root_hash, 32) != 0));
    if (root_rc != 0 && root_rc != MDB_NOTFOUND) {
      pl_store_silo_batch_abort(batch);
      return pack_error(err, err_cap, "cannot query Silo root");
    }
  }

  if (!batch->changed && !root_changed) {
    pl_store_silo_batch_abort(batch);
    return true;
  }
  if (!batch_flush(batch, err, err_cap)) {
    pl_store_silo_batch_abort(batch);
    return false;
  }
  if (batch->pack_dirty) {
#ifdef F_FULLFSYNC
    int sync_rc = fcntl(batch->backend->pack_fd, F_FULLFSYNC, 0);
#else
    int sync_rc = fsync(batch->backend->pack_fd);
#endif
    if (sync_rc != 0) {
      pl_store_silo_batch_abort(batch);
      return pack_error(err, err_cap, "cannot sync pins.pack");
    }
  }
  if (root_changed) {
    MDB_val root_v = {.mv_size = 32, .mv_data = (void*)root_hash};
    if (mdb_put(batch->txn, batch->backend->meta, &root_k, &root_v, 0) != 0) {
      pl_store_silo_batch_abort(batch);
      return pack_error(err, err_cap, "cannot publish Silo root");
    }
  }

  MDB_txn* txn = batch->txn;
  batch->txn = NULL; /* mdb_txn_commit consumes the handle on every result. */
  int rc = mdb_txn_commit(txn);
  ax_hmfree(batch->pending);
  free(batch);
  return rc == 0
             ? true
             : pack_error(err, err_cap, "cannot commit Silo save transaction");
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
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return pack_error(err, err_cap, "cannot determine system page size");
  uint64_t page = (uint64_t)page_size;
  uint64_t map_off = off - off % page;
  uint64_t delta = off - map_off;
  if (len > SIZE_MAX || delta > SIZE_MAX - (size_t)len)
    return pack_error(err, err_cap, "Silo object is too large to map");
  size_t mapping_len = (size_t)delta + (size_t)len;
  pack_reader* pr = calloc(1, sizeof(*pr));
  if (pr == NULL)
    return pack_error(err, err_cap, "out of memory for Silo reader");
  void* mapping = mmap(NULL, mapping_len, PROT_READ, MAP_SHARED, b->pack_fd,
                       (off_t)map_off);
  if (mapping == MAP_FAILED) {
    free(pr);
    return pack_error(err, err_cap, "cannot map Silo object");
  }
  *pr = (pack_reader){.mapping = mapping,
                      .mapping_len = mapping_len,
                      .bytes = (const uint8_t*)mapping + (size_t)delta,
                      .len = (size_t)len};
  *out = (pl_silo_reader){.ctx = pr,
                          .read = pack_reader_read,
                          .rewind = pack_reader_rewind,
                          .len = len};
  return true;
}

void pl_store_silo_close_reader(pl_silo_reader* r) {
  if (r == NULL)
    return;
  pack_reader* pr = r->ctx;
  if (pr != NULL) {
    (void)munmap(pr->mapping, pr->mapping_len);
    free(pr);
  }
  memset(r, 0, sizeof(*r));
}

pl_store* pl_store_new_silo(const char* path, size_t map_size) {
  silo_backend* b = calloc(1, sizeof(*b));
  if (b == NULL)
    return NULL;
  b->pack_fd = -1;
  if (mdb_env_create(&b->env) != 0 || mdb_env_set_maxdbs(b->env, 3) != 0 ||
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
      mdb_dbi_open(txn, "meta", MDB_CREATE, &b->meta) != 0 ||
      mdb_dbi_open(txn, "codecache", MDB_CREATE, &b->codecache) != 0) {
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
      .get = silo_kv_get,
      .put = silo_kv_put,
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
