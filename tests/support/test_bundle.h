#ifndef TEST_BUNDLE_H
#define TEST_BUNDLE_H
#include "test_plan.h"
#include "enki/bundle.h"
#include "../../pkg/plan/src/store_internal.h"
#include "axsys/ds.h"

static bool test_bundle_make(uint8_t** blob, size_t* len) {
  *blob = NULL;
  *len = 0;
  test_rt rt = test_rt_new();
  uint8_t *stream = NULL, *payload = NULL;
  size_t stream_len = 0, payload_len = 0;
  uint8_t hash[32];
  char err[256];
  pl_val leaf, entry;
  bool ok = pl_silo_encode_buffer(42, NULL, 0, NULL, NULL, &stream, &stream_len,
                                  hash, err, sizeof(err)) &&
            pl_store_load_silo_stream(rt.t, rt.store, stream, stream_len, &leaf,
                                      err, sizeof(err));
  ax_arrfree(stream);
  stream = NULL;
  if (ok) {
    pl_val args[] = {leaf, leaf};
    pl_val body = test_app(rt.t, 0, 2, args);
    ok =
        pl_silo_encode_buffer(body, &leaf, 1, NULL, NULL, &stream, &stream_len,
                              hash, err, sizeof(err)) &&
        pl_store_load_silo_stream(rt.t, rt.store, stream, stream_len, &entry,
                                  err, sizeof(err)) &&
        pl_store_bundle_encode(entry, &payload, &payload_len, err,
                               sizeof(err)) &&
        en_bundle_blob(payload, payload_len, hash, blob, len, err, sizeof(err));
  }
  free(payload);
  ax_arrfree(stream);
  test_rt_free(&rt);
  return ok;
}

static int test_bundle_boot(const uint8_t* blob, size_t len) {
  char err[256];
  en_bundle b;
  if (!en_bundle_validate(blob, len, &b, err, sizeof(err)))
    return 126;
  pl_val entry;
  pl_store* s = pl_store_new_bundle(b.payload, b.payload_len, b.entry_hash,
                                    &entry, err, sizeof(err));
  if (s == NULL)
    return 126;
  pl_store_free(s);
  return 0;
}
#endif
