#include <criterion/criterion.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axsys/profile.h"
#include "test_plan.h"

static char* store_test_read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  cr_assert_not_null(f);
  cr_assert_eq(fseek(f, 0, SEEK_END), 0);
  long end = ftell(f);
  cr_assert_geq(end, 0);
  cr_assert_eq(fseek(f, 0, SEEK_SET), 0);
  char* data = malloc((size_t)end + 1);
  cr_assert_not_null(data);
  cr_assert_eq(fread(data, 1, (size_t)end, f), (size_t)end);
  data[end] = '\0';
  cr_assert_eq(fclose(f), 0);
  return data;
}

static size_t store_test_count(const char* haystack, const char* needle) {
  size_t count = 0;
  size_t needle_n = strlen(needle);
  for (const char* p = haystack; (p = strstr(p, needle)) != NULL; p += needle_n)
    count++;
  return count;
}

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
    memcpy(hash, pl_pin_hash(pin), 32);
    cr_assert(pl_store_put_root(s, hash));
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
    cr_assert_eq(again, pin);

    pl_thread_free(t);
    pl_heap_free(h);
    pl_store_free(s);
  }
}

static pl_store* mk_lmdb(const char* dir) {
  return pl_store_new_lmdb(dir, (size_t)64 << 20);
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

Test(store, chrome_json_profiles_slow_store_operations) {
  char path[] = "/tmp/enki-profile-store-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  cr_assert_eq(close(fd), 0);
  cr_assert(ax_profile_json_start(path));

  pl_store* s = pl_store_new_mem();
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);
  size_t base = t->vsp;
  pl_vpush(t, 42);
  pl_val pin = pl_pin(t, t->vstack[base]);
  uint8_t hash[32];
  memcpy(hash, pl_pin_hash(pin), sizeof(hash));
  cr_assert(pl_store_put_root(s, hash));
  memset(hash, 0, sizeof(hash));
  cr_assert(pl_store_get_root(s, hash));
  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);

  cr_assert(ax_profile_json_finish());
  char* json = store_test_read_file(path);
  cr_assert_not_null(strstr(json, "\"cat\":\"splan.store\""));
  cr_assert_not_null(strstr(json, "\"name\":\"store.open\""));
  cr_assert_not_null(strstr(json, "\"name\":\"store.backend.put\""));
  cr_assert_not_null(strstr(json, "\"name\":\"store.root.put\""));
  cr_assert_not_null(strstr(json, "\"name\":\"store.root.get\""));
  cr_assert_not_null(strstr(json, "\"name\":\"store.close\""));
  cr_assert_not_null(strstr(json, "\"args\":{\"span\":"));
  cr_assert_not_null(strstr(json, "Store thread "));
  cr_assert_eq(store_test_count(json, "\"ph\":\"B\""),
               store_test_count(json, "\"ph\":\"E\""));
  free(json);
  cr_assert_eq(unlink(path), 0);
}
