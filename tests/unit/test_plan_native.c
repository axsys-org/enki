/* Child modes use exec, not inherited mappings, to test genuine reopen. */
int native_test_main(int argc, char** argv);
#define main native_test_main
#include "test.h"
#undef main
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include "test_plan.h"
#include "../../pkg/plan/src/store_internal.h"
#include "axsys/ds.h"
#include "axsys/sha256.h"

extern char** environ;
static char native_binary[PATH_MAX];
static char native_dir[128];
static test_rt rt;

static void native_setup(void) {
  snprintf(native_dir, sizeof(native_dir), "/tmp/enki-native-XXXXXX");
  ASSERT_NOT_NULL(mkdtemp(native_dir));
  char err[256];
  rt.store = pl_store_new_native(native_dir, err, sizeof(err));
  ASSERT_NOT_NULL(rt.store, "%s", err);
  rt.heap = pl_heap_new(1 << 16, rt.store);
  rt.t = pl_thread_new(rt.heap);
}
static void native_teardown(void) {
  if (rt.store != NULL)
    test_rt_free(&rt);
  rt = (test_rt){0};
  const char* files[] = {"arena.dat", "arena.writer.lock", "arena.commit.lock"};
  for (size_t i = 0; i < sizeof(files) / sizeof(*files); i++) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", native_dir, files[i]);
    (void)unlink(path);
  }
  (void)rmdir(native_dir);
}
TEST_FIXTURE(native, native_setup, native_teardown)

static void close_rt(void) {
  test_rt_free(&rt);
  rt = (test_rt){0};
}
static void reopen_rt(void) {
  char err[256];
  rt.store = pl_store_new_native(native_dir, err, sizeof(err));
  ASSERT_NOT_NULL(rt.store, "%s", err);
  rt.heap = pl_heap_new(1 << 16, rt.store);
  rt.t = pl_thread_new(rt.heap);
}
static pl_val save_nat(uint64_t value, uint8_t hash[32], bool root) {
  size_t slot = rt.t->vsp;
  pl_vpush(rt.t, pl_pin(rt.t, value));
  char err[256];
  bool ok = root ? pl_store_save_root(rt.store, rt.t->vstack[slot], hash, err,
                                      sizeof(err))
                 : pl_store_save_pin(rt.store, rt.t->vstack[slot], hash, err,
                                     sizeof(err));
  ASSERT(ok, "%s", err);
  pl_val pin = pl_make(PL_TAG_PIN, pl_pin_resolved(pl_ptr(rt.t->vstack[slot])));
  rt.t->vsp = slot;
  return pin;
}
static int child(const char* mode, const char* a, const char* b) {
  char* args[] = {native_binary, "--native-child",    (char*)mode,
                  native_dir,    (char*)(a ? a : ""), (char*)(b ? b : ""),
                  NULL};
  pid_t pid;
  ASSERT_EQ(posix_spawn(&pid, native_binary, NULL, NULL, args, environ), 0);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT(WIFEXITED(status), "child %s status %d", mode, status);
  return WEXITSTATUS(status);
}
static bool fault_return(void* ctx, const char* point) {
  return strcmp(ctx, point) != 0;
}
static bool fault_crash(void* ctx, const char* point) {
  if (strcmp(ctx, point) == 0)
    _exit(77);
  return true;
}

#ifdef __linux__
TEST(native, code_slots_reserve_without_commit_or_population) {
  /* RSS alone cannot catch removal of MAP_NORESERVE: an ordinary anonymous
   * mapping is also demand-paged, but charges the entire table to commit.
   * Inspect this VMA rather than noisy, system-wide Committed_AS deltas. */
  FILE* policy = fopen("/proc/sys/vm/overcommit_memory", "r");
  ASSERT_NOT_NULL(policy);
  int overcommit = -1;
  int scanned = fscanf(policy, "%d", &overcommit);
  ASSERT_EQ(fclose(policy), 0);
  ASSERT_EQ(scanned, 1);
  /* Linux ignores MAP_NORESERVE in strict mode. This checks the reservation
   * flags without changing the host policy; it does not simulate mode 2. */
  if (overcommit == 2)
    SKIP_TEST("strict overcommit ignores MAP_NORESERVE");

  uintptr_t slots = (uintptr_t)pl_native_code_slots;
  ASSERT_NEQ(slots, 0);
  uint64_t bytes = (PL_NATIVE_BYTES / (PL_PIN_CELLS(0) * sizeof(pl_cell)) + 1) *
                   sizeof(pl_cell);
  FILE* smaps = fopen("/proc/self/smaps", "r");
  ASSERT_NOT_NULL(smaps);
  char line[1024];
  bool found = false, flags = false, noreserve = false, accounted = false;
  bool have_rss = false, have_swap = false;
  unsigned long rss = 0, swap = 0;
  while (fgets(line, sizeof(line), smaps) != NULL) {
    unsigned long long start, end;
    if (sscanf(line, "%llx-%llx", &start, &end) == 2) {
      if (found)
        break;
      if (slots < start || slots >= end)
        continue;
      found = true;
      ASSERT_GTE(end - slots, bytes);
    } else if (found) {
      if (sscanf(line, "Rss: %lu kB", &rss) == 1)
        have_rss = true;
      else if (sscanf(line, "Swap: %lu kB", &swap) == 1)
        have_swap = true;
      else if (strncmp(line, "VmFlags:", 8) == 0) {
        flags = true;
        for (char* flag = strtok(line + 8, " \t\n"); flag != NULL;
             flag = strtok(NULL, " \t\n")) {
          noreserve |= strcmp(flag, "nr") == 0;
          accounted |= strcmp(flag, "ac") == 0;
        }
      }
    }
  }
  ASSERT_FALSE(ferror(smaps));
  ASSERT_EQ(fclose(smaps), 0);
  ASSERT(found);
  ASSERT(flags);
  ASSERT(noreserve, "code-slot VMA must have VmFlags nr (MAP_NORESERVE)");
  ASSERT_FALSE(accounted, "code-slot VMA must not have VmFlags ac");
  ASSERT(have_rss && have_swap);
  /* Allow a little startup bookkeeping and page-size variation, but never
   * eager population of the multi-GiB table. Include swapped-out pages. */
  ASSERT_LTE(rss + swap, 1024);
}
#endif

TEST(native, exact_address_staging_and_reopen) {
  uint8_t hash[32], root[32];
  pl_val p = save_nat(42, hash, false);
  ASSERT_GTE(pl_addr(p), PL_NATIVE_BASE);
  ASSERT_LT(pl_addr(p), PL_NATIVE_BASE + PL_NATIVE_BYTES);
  ASSERT_FALSE(pl_store_get_root(rt.store, root));
  ASSERT_EQ(child("empty", NULL, NULL), 0);
  ASSERT_EQ(child("writer-blocked", NULL, NULL), 0);
  char err[256];
  ASSERT(pl_store_save_root(rt.store, p, root, err, sizeof(err)), "%s", err);
  ASSERT_MEM_EQ(root, hash, 32);
  char addr[64];
  snprintf(addr, sizeof(addr), "%llu", (unsigned long long)p);
  ASSERT_EQ(child("read", "42", addr), 0);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  reopen_rt();
  ASSERT_EQ(ax_arrlen(rt.store->pins), 0); /* no eager registration */
  ASSERT_EQ(ax_hmlen(rt.store->intern), 0);
  ASSERT_EQ(pl_store_load(rt.t, hash), p);
}
TEST(native, same_root_checkpoints_staging_and_close_discards_ice) {
  uint8_t a[32], b[32], c[32];
  pl_val root = save_nat(10, a, true);
  (void)save_nat(20, b, false);
  char err[256];
  ASSERT(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)), "%s", err);
  ASSERT_EQ(pl_store_root_log_head(rt.store), 1);
  (void)save_nat(30, c, false);
  close_rt();
  reopen_rt();
  pl_val found;
  ASSERT(pl_native_load(rt.store, b, &found, err, sizeof(err)), "%s", err);
  ASSERT_FALSE(pl_native_load(rt.store, c, &found, err, sizeof(err)));
  ASSERT_EQ(pl_pin_body(pl_ptr(found)), 20);
}
TEST(native, checkpoints_share_unpinned_native_nodes_and_keep_history) {
  size_t count = 1024 * 1024;
  uint8_t* data = malloc(count);
  ASSERT_NOT_NULL(data);
  memset(data, 0xa7, count);
  pl_vpush(rt.t, pl_nat_from_bytes(rt.t, data, count));
  free(data);
  pl_val fields[2] = {0, test_app1(rt.t, 0, rt.t->vstack[0])};
  pl_vpush(rt.t, pl_pin(rt.t, test_app(rt.t, 0, 2, fields)));
  uint8_t hashes[21][32], expected[32];
  char err[256], path[256];
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[1], hashes[0], err,
                            sizeof(err)),
         "%s", err);
  pl_val first = pl_store_load(rt.t, hashes[0]);
  pl_val shared = pl_app_args(pl_ptr(pl_pin_body(pl_ptr(first))))[1];
  rt.t->vsp = 0;
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  struct stat before, after;
  ASSERT_EQ(stat(path, &before), 0);
  for (unsigned i = 1; i <= 20; i++) {
    pl_val next[2] = {i, shared};
    pl_vpush(rt.t, pl_pin(rt.t, test_app(rt.t, 0, 2, next)));
    ASSERT(pl_store_save_root(rt.store, rt.t->vstack[0], hashes[i], err,
                              sizeof(err)),
           "%s", err);
    pl_val root = pl_store_load(rt.t, hashes[i]);
    ASSERT_EQ(pl_app_args(pl_ptr(pl_pin_body(pl_ptr(root))))[1], shared);
    rt.t->vsp = 0;
  }
  ASSERT_EQ(stat(path, &after), 0);
  ASSERT_LT((uint64_t)(after.st_size - before.st_size), 2 * 1024 * 1024);
  /* Pin an existing raw child, not previously a PIN body. Its whole body
   * is borrowed from an earlier extent without changing its content hash. */
  ASSERT(pl_silo_hash(shared, NULL, 0, expected, err, sizeof(err)), "%s", err);
  pl_vpush(rt.t, pl_pin(rt.t, shared));
  uint8_t raw_hash[32];
  ASSERT(
      pl_store_save_root(rt.store, rt.t->vstack[0], raw_hash, err, sizeof(err)),
      "%s", err);
  ASSERT_MEM_EQ(raw_hash, expected, 32);
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, raw_hash))), shared);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  reopen_rt();
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, raw_hash))), shared);
  for (unsigned i = 0; i <= 20; i++) {
    pl_val body = pl_pin_body(pl_ptr(pl_store_load(rt.t, hashes[i])));
    ASSERT_EQ(pl_app_args(pl_ptr(body))[0], i);
    ASSERT_EQ(pl_app_args(pl_ptr(body))[1], shared);
    pl_val payload = pl_app_args(pl_ptr(shared))[0];
    ASSERT_EQ(pl_nat_byte_len(payload), count);
    ASSERT_EQ(pl_nat_byte_at(payload, count - 1), 0xa7);
  }
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}
TEST(native, shared_edges_require_real_earlier_node_boundaries) {
  uint8_t data[128] = {0};
  /* Place a plausible APP header inside NAT payload. It is not a node. */
  uint64_t fake[3] = {pl_hdr_make(PL_K_APP, PL_F_NORMAL, 0, 3), 0, 99};
  memcpy(data + 8, fake, sizeof(fake));
  data[sizeof(data) - 1] = 1;
  pl_vpush(rt.t, pl_pin(rt.t, pl_nat_from_bytes(rt.t, data, sizeof(data))));
  uint8_t hash[32];
  char err[256], path[256];
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[0], hash, err, sizeof(err)),
         "%s", err);
  pl_val prior = pl_pin_body(pl_ptr(pl_store_load(rt.t, hash)));
  pl_vpush(rt.t, pl_pin(rt.t, test_app1(rt.t, 0, prior)));
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[1], hash, err, sizeof(err)),
         "%s", err);
  pl_val root = pl_store_load(rt.t, hash);
  uint64_t off = pl_addr(root) - PL_NATIVE_BASE - 48;
  uint64_t app_offset =
      pl_addr(pl_pin_body(pl_ptr(root))) - PL_NATIVE_BASE - off;
  pl_val forged = pl_make(PL_TAG_APP, (void*)(uintptr_t)(pl_addr(prior) + 16));
  close_rt();
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  uint64_t len;
  ASSERT_EQ(pread(fd, &len, 8, (off_t)(off + 32)), 8);
  uint8_t* bytes = malloc((size_t)len);
  ASSERT_NOT_NULL(bytes);
  ASSERT_EQ(pread(fd, bytes, (size_t)len, (off_t)off), (ssize_t)len);
  memcpy(bytes + app_offset + 16, &forged, 8);
  ax_sha256(bytes + 32, (size_t)len - 32, bytes);
  ASSERT_EQ(pwrite(fd, bytes, (size_t)len, (off_t)off), (ssize_t)len);
  close(fd);
  free(bytes);
  reopen_rt();
  ASSERT_FALSE(pl_store_verify(rt.store, err, sizeof(err)));
  close_rt();
  ASSERT_FALSE(
      pl_store_native_compact(native_dir, NULL, NULL, err, sizeof(err)));
}
TEST(native, layout_one_upgrade_keeps_roots_and_retires_old_readers) {
  uint8_t before[32], after[32];
  (void)save_nat(42, before, true);
  close_rt();
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  /* The object and journal layout is unchanged. Emulate the old writer's
   * superblocks, including valid checksums, before opening with layout 2. */
  uint8_t sb[184];
  for (unsigned i = 0; i < 2; i++) {
    ASSERT_EQ(pread(fd, sb, sizeof(sb), (off_t)(i * PL_NATIVE_GRANULE)),
              sizeof(sb));
    uint64_t one = 1;
    memcpy(sb + 48, &one, 8);
    ax_sha256(sb + 32, sizeof(sb) - 32, sb);
    ASSERT_EQ(pwrite(fd, sb, sizeof(sb), (off_t)(i * PL_NATIVE_GRANULE)),
              sizeof(sb));
  }
  close(fd);
  reopen_rt();
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, before))), 42);
  pl_native_set_fault(rt.store, fault_return, "after_superblock_sync");
  pl_vpush(rt.t, pl_pin(rt.t, 43));
  char err[256];
  ASSERT_FALSE(
      pl_store_save_root(rt.store, rt.t->vstack[0], after, err, sizeof(err)));
  close_rt();
  reopen_rt();
  /* Reopening may observe the new root before an earlier layout-1 slot was
   * retired. An unchanged-root retry must still complete that retirement. */
  ASSERT(pl_store_get_root(rt.store, after));
  pl_val retained = pl_store_load(rt.t, after);
  ASSERT_EQ(pl_pin_body(pl_ptr(retained)), 43);
  ASSERT(pl_store_save_root(rt.store, retained, NULL, err, sizeof(err)), "%s",
         err);
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, before))), 42);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  fd = open(path, O_RDONLY);
  ASSERT_GTE(fd, 0);
  for (unsigned i = 0; i < 2; i++) {
    ASSERT_EQ(pread(fd, sb, sizeof(sb), (off_t)(i * PL_NATIVE_GRANULE)),
              sizeof(sb));
    uint64_t layout;
    memcpy(&layout, sb + 48, 8);
    ASSERT_EQ(layout, 2);
  }
  close(fd);
  reopen_rt();
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, before))), 42);
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, after))), 43);
}
TEST(native, nested_graph_sharing_hash_and_code_slots) {
  uint8_t child_hash[32];
  pl_val leaf = save_nat(73, child_hash, false);
  pl_vpush(rt.t, leaf);
  pl_vpush(rt.t, test_app1(rt.t, 0, 99));
  pl_val fields[3] = {rt.t->vstack[0], rt.t->vstack[1], rt.t->vstack[1]};
  pl_vpush(rt.t, test_app(rt.t, 0, 3, fields));
  pl_nf(rt.t, rt.t->vstack[2]);
  uint8_t expected[32], actual[32];
  char err[256];
  ASSERT(pl_silo_hash(rt.t->vstack[2], &rt.t->vstack[0], 1, expected, err,
                      sizeof(err)),
         "%s", err);
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[2]));
  ASSERT(
      pl_store_save_root(rt.store, rt.t->vstack[3], actual, err, sizeof(err)),
      "%s", err);
  ASSERT_MEM_EQ(expected, actual, 32);
  pl_val root = pl_store_load(rt.t, actual);
  pl_cell* app = pl_ptr(pl_pin_body(pl_ptr(root)));
  ASSERT_EQ(pl_app_args(app)[1], pl_app_args(app)[2]);
  ASSERT_EQ(pl_app_args(app)[0], leaf);
  pl_code dummy = {0};
  uint64_t id = pl_ptr(root)[6];
  pl_pin_set_code(pl_ptr(root), &dummy);
  ASSERT_EQ(pl_pin_code(pl_ptr(root)), &dummy);
  ASSERT_EQ(pl_ptr(root)[6], id);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  reopen_rt();
  ASSERT_EQ(pl_store_load(rt.t, actual), root);
  ASSERT_NULL(pl_pin_code(pl_ptr(root)));
}
TEST(native, readonly_rejects_mutations) {
  uint8_t hash[32];
  (void)save_nat(5, hash, true);
  close_rt();
  char err[256];
  rt.store = pl_store_new_native_ro(native_dir, err, sizeof(err));
  ASSERT_NOT_NULL(rt.store, "%s", err);
  rt.heap = pl_heap_new(1 << 16, rt.store);
  rt.t = pl_thread_new(rt.heap);
  pl_val pin = pl_store_load(rt.t, hash);
  ASSERT_FALSE(pl_store_save_root(rt.store, pin, NULL, err, sizeof(err)));
  ASSERT_FALSE(pl_store_save_pin(rt.store, pin, NULL, err, sizeof(err)));
  ASSERT_FALSE(pl_store_backend_put(rt.store, hash, hash, 32));
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}
TEST(native, publication_failures_are_retryable_or_poisoned) {
  uint8_t old[32], hash[32];
  (void)save_nat(42, old, true);
  pl_vpush(rt.t, pl_pin(rt.t, 43));
  char err[256];
  pl_native_set_fault(rt.store, fault_return, "after_data_sync");
  ASSERT_FALSE(
      pl_store_save_root(rt.store, rt.t->vstack[0], hash, err, sizeof(err)));
  ASSERT_FALSE(pl_pin_is_hashed(rt.t->vstack[0]));
  ASSERT(pl_store_get_root(rt.store, hash));
  ASSERT_MEM_EQ(hash, old, 32);
  pl_native_set_fault(rt.store, NULL, NULL);
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[0], hash, err, sizeof(err)),
         "%s", err);
  pl_vpush(rt.t, pl_pin(rt.t, 44));
  pl_native_set_fault(rt.store, fault_return, "after_superblock");
  ASSERT_FALSE(
      pl_store_save_root(rt.store, rt.t->vstack[1], NULL, err, sizeof(err)));
  pl_native_set_fault(rt.store, NULL, NULL);
  ASSERT_FALSE(
      pl_store_save_root(rt.store, rt.t->vstack[1], NULL, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "reopen"));
  close_rt();
  reopen_rt();
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}
TEST(native, crash_boundaries) {
  const char* points[] = {"grow",
                          "object",
                          "index",
                          "before_data_sync",
                          "after_data_sync",
                          "before_superblock",
                          "after_superblock",
                          "after_superblock_sync"};
  for (size_t i = 0; i < sizeof(points) / sizeof(*points); i++) {
    uint8_t hash[32];
    (void)save_nat(42, hash, true);
    close_rt();
    ASSERT_EQ(child("crash", points[i], NULL), 77);
    reopen_rt();
    ASSERT(pl_store_get_root(rt.store, hash));
    pl_val pin = pl_store_load(rt.t, hash);
    pl_val value = pl_pin_body(pl_ptr(pin));
    ASSERT(value == 42 || value == 43);
    if (i < 6)
      ASSERT_EQ(value, 42);
    if (i == 7)
      ASSERT_EQ(value, 43);
    char err[256];
    ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
    /* Make the next crash attempt allocate a genuinely new object. */
    close_rt();
    char path[256];
    snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
    ASSERT_EQ(unlink(path), 0);
    reopen_rt();
  }
}
TEST(native, history_and_pat_routing) {
  uint8_t hashes[80][32];
  for (uint64_t i = 0; i < 80; i++)
    (void)save_nat(i, hashes[i], true);
  pl_store_root_entry entries[7];
  ASSERT_EQ(pl_store_root_log(rt.store, 13, entries, 7), 7);
  for (size_t i = 0; i < 7; i++) {
    ASSERT_EQ(entries[i].seq, 13 + i);
    ASSERT_MEM_EQ(entries[i].hash, hashes[12 + i], 32);
  }
  close_rt();
  reopen_rt();
  for (uint64_t i = 0; i < 80; i++)
    ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, hashes[i]))), i);
  char err[256];
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  pl_store_usage u;
  ASSERT(pl_store_get_usage(rt.store, &u, err, sizeof(err)), "%s", err);
  ASSERT_EQ(u.committed_bytes, u.object_bytes + u.index_bytes +
                                   u.journal_bytes + u.overhead_bytes);
  ASSERT_GT(u.object_bytes, 0);
  ASSERT_GT(u.index_bytes, 0);
  ASSERT_GT(u.journal_bytes, 0);
}
TEST(native, single_store_and_transient_snapshot) {
  char err[256];
  ASSERT_NULL(pl_store_new_native(native_dir, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "one native"));
  pl_store* mem = pl_store_new_mem();
  ASSERT_NOT_NULL(mem);
  pl_store_free(mem);
  pl_vpush(rt.t, test_app1(rt.t, 0, 7));
  pl_nf(rt.t, rt.t->vstack[0]);
  pl_val snapshot = pl_store_snapshot_normal(rt.t, rt.t->vstack[0]);
  ASSERT(pl_store_owns(rt.store, snapshot));
  ASSERT(pl_addr(snapshot) < PL_NATIVE_BASE ||
         pl_addr(snapshot) >= PL_NATIVE_BASE + PL_NATIVE_BYTES);
  uint8_t hash[32];
  ASSERT_FALSE(pl_store_get_root(rt.store, hash));
}
TEST(native, reject_legacy_and_invalid_existing_file) {
  close_rt();
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  ASSERT_EQ(unlink(path), 0);
  char old[256];
  snprintf(old, sizeof(old), "%s/pins.pack", native_dir);
  int fd = open(old, O_WRONLY | O_CREAT, 0600);
  ASSERT_GTE(fd, 0);
  close(fd);
  char err[256];
  ASSERT_NULL(pl_store_new_native(native_dir, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "fresh directory"));
  ASSERT_EQ(unlink(old), 0);
  fd = open(path, O_WRONLY | O_CREAT, 0600);
  ASSERT_GTE(fd, 0);
  close(fd);
  ASSERT_NULL(pl_store_new_native(native_dir, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "no valid"));
}
TEST(native, torn_superblock_falls_back) {
  uint8_t hash[32];
  (void)save_nat(42, hash, true);
  (void)save_nat(43, hash, true);
  close_rt();
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  uint8_t bad = 0xff;
  ASSERT_EQ(pwrite(fd, &bad, 1, 0), 1);
  close(fd);
  reopen_rt();
  ASSERT(pl_store_get_root(rt.store, hash));
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, hash))), 42);
}

TEST(native, collision_never_overwrites_existing_mapping) {
  close_rt();
  void* wanted = (void*)(uintptr_t)(PL_NATIVE_BASE + PL_NATIVE_GRANULE * 9);
#ifdef __APPLE__
  mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)wanted;
  ASSERT_EQ(mach_vm_allocate(mach_task_self(), &addr, PL_NATIVE_GRANULE,
                             VM_FLAGS_FIXED),
            KERN_SUCCESS);
  void* p = (void*)(uintptr_t)addr;
#else
  void* p = mmap(wanted, PL_NATIVE_GRANULE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
  ASSERT_EQ(p, wanted);
  *(volatile uint64_t*)p = UINT64_C(0x12345678abcdef);
  char err[256];
  ASSERT_NULL(pl_store_new_native(native_dir, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "unavailable"));
  ASSERT_EQ(*(volatile uint64_t*)p, UINT64_C(0x12345678abcdef));
  /* A non-native allocator may occupy this range while no native store is
   * open. Address alone must not turn its inline code field into an ID. */
  pl_cell* ordinary = p;
  ordinary[0] =
      pl_hdr_make(PL_K_PIN, PL_F_NORMAL | PL_F_PIN_HASHED, 0, PL_PIN_CELLS(0));
  ordinary[6] = 0;
  pl_code code = {0};
  pl_pin_set_code(ordinary, &code);
  ASSERT_EQ(pl_pin_code_raw(ordinary), &code);
  ASSERT_EQ(pl_pin_code_slot(ordinary), ordinary + 6);
  ASSERT_EQ(munmap(p, PL_NATIVE_GRANULE), 0);
  reopen_rt();
}
TEST(native, large_extent_stream_import_and_cache_mapping) {
  size_t bytes = 3 * PL_NATIVE_GRANULE + 19;
  uint8_t* data = malloc(bytes);
  ASSERT_NOT_NULL(data);
  memset(data, 0xa7, bytes);
  pl_vpush(rt.t, pl_nat_from_bytes(rt.t, data, bytes));
  free(data);
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[0]));
  char err[256];
  uint8_t hash[32];
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[1], hash, err, sizeof(err)),
         "%s", err);
  pl_val pin = pl_store_load(rt.t, hash);
  ASSERT_EQ((pl_addr(pin) - 48) % PL_NATIVE_GRANULE, 0);
  uint8_t* stream = NULL;
  size_t stream_len = 0;
  ASSERT(pl_silo_encode_buffer(pl_pin_body(pl_ptr(pin)), NULL, 0, NULL, NULL,
                               &stream, &stream_len, hash, err, sizeof(err)),
         "%s", err);
  pl_val duplicate;
  ASSERT(pl_store_load_silo_stream(rt.t, rt.store, stream, stream_len,
                                   &duplicate, err, sizeof(err)),
         "%s", err);
  ASSERT_EQ(duplicate, pin);
  uint8_t key[32] = {5};
  ASSERT(pl_store_backend_put(rt.store, key, hash, 32));
  ASSERT(pl_store_save_root(rt.store, pin, NULL, err, sizeof(err)), "%s", err);
  struct stat st;
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  ASSERT_EQ(stat(path, &st), 0);
  ASSERT_LT((uint64_t)st.st_size, 16 * PL_NATIVE_GRANULE);
  close_rt();
  reopen_rt();
  uint8_t* cached;
  size_t len;
  ASSERT(pl_store_backend_get(rt.store, key, &cached, &len));
  ASSERT_EQ(len, 32);
  ASSERT_MEM_EQ(cached, hash, 32);
  free(cached);
  ASSERT_EQ(pl_store_load(rt.t, hash), pin);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  ASSERT_EQ(unlink(path), 0);
  reopen_rt();
  ASSERT(pl_store_load_silo_stream(rt.t, rt.store, stream, stream_len,
                                   &duplicate, err, sizeof(err)),
         "%s", err);
  ax_arrfree(stream);
  ASSERT_GTE(pl_addr(duplicate), PL_NATIVE_BASE);
  ASSERT_MEM_EQ(pl_pin_hash(duplicate), hash, 32);
  ASSERT(pl_store_save_root(rt.store, duplicate, NULL, err, sizeof(err)), "%s",
         err);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}

typedef struct native_demand {
  pl_store* s;
  pl_val pin;
  void* result;
} native_demand;
static void* demand_worker(void* ctx) {
  native_demand* w = ctx;
  w->result = pl_store_code_demand(w->s, w->pin);
  return NULL;
}
TEST(native, lazy_law_registration_concurrent_demand_and_invalidation) {
  pl_vpush(rt.t, test_law(rt.t, 1, 7, 1));
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[0]));
  uint8_t hash[32];
  char err[256];
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[1], hash, err, sizeof(err)),
         "%s", err);
  pl_val pin = pl_store_load(rt.t, hash);
  ASSERT_EQ(pl_apply(rt.t, pin, 123), 123);
  uint8_t compiler[32] = {1, 2}, rowhash[32], pre[68], key[32];
  pl_val ops[] = {OP_PUSH_VAR, 1, OP_RET};
  pl_vpush(rt.t, test_app(rt.t, 0, 3, ops));
  pl_nf(rt.t, rt.t->vstack[2]);
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[2]));
  ASSERT(
      pl_store_save_pin(rt.store, rt.t->vstack[3], rowhash, err, sizeof(err)),
      "%s", err);
  memcpy(pre, "plgc", 4);
  memcpy(pre + 4, compiler, 32);
  memcpy(pre + 36, hash, 32);
  ax_sha256(pre, sizeof(pre), key);
  ASSERT(pl_store_backend_put(rt.store, key, rowhash, 32));
  ASSERT(pl_store_save_root(rt.store, pin, NULL, err, sizeof(err)), "%s", err);
  close_rt();
  reopen_rt();
  ASSERT_EQ(ax_arrlen(rt.store->pins), 0);
  ASSERT(pl_store_put_compiler(rt.store, compiler));
  ASSERT_NULL(pl_pin_code_raw(pl_ptr(pin)));
  uint64_t id = pl_ptr(pin)[6];
  native_demand workers[8];
  pthread_t threads[8];
  for (size_t i = 0; i < 8; i++) {
    workers[i] = (native_demand){.s = rt.store, .pin = pin};
    ASSERT_EQ(pthread_create(&threads[i], NULL, demand_worker, &workers[i]), 0);
  }
  for (size_t i = 0; i < 8; i++) {
    ASSERT_EQ(pthread_join(threads[i], NULL), 0);
  }
  pl_code* code = pl_pin_code(pl_ptr(pin));
  ASSERT_NOT_NULL(code);
  for (size_t i = 0; i < 8; i++)
    ASSERT(workers[i].result == NULL || workers[i].result == code);
  ASSERT_EQ(ax_arrlen(rt.store->codes), 1);
  ASSERT_EQ(pl_apply(rt.t, pin, 234), 234);
  ASSERT_EQ(pl_ptr(pin)[6], id);
  ASSERT_EQ(ax_arrlen(rt.store->pins), 2);
  memset(compiler, 0xff, 32);
  ASSERT(pl_store_put_compiler(rt.store, compiler));
  ASSERT_NULL(pl_pin_code(pl_ptr(pin)));
  ASSERT_EQ(rt.store->codes[0], code); /* suspended frames retain old code */
  ASSERT_EQ(pl_apply(rt.t, pin, 456), 456);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}

TEST(native, global_cache_export_import_and_local_checkpoint) {
  char cache[256];
  snprintf(cache, sizeof(cache), "%s/cache", native_dir);
  ASSERT_EQ(mkdir(cache, 0700), 0);
  char* saved_dir =
      getenv("PL_CODECACHE_DIR") ? strdup(getenv("PL_CODECACHE_DIR")) : NULL;
  char* saved_flag =
      getenv("PL_CODECACHE") ? strdup(getenv("PL_CODECACHE")) : NULL;
  setenv("PL_CODECACHE_DIR", cache, 1);
  unsetenv("PL_CODECACHE");
  pl_codecache_reset();
  pl_val ops[3] = {OP_PUSH_VAR, 1, OP_RET};
  pl_vpush(rt.t, test_app(rt.t, 0, 3, ops));
  pl_nf(rt.t, rt.t->vstack[0]);
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[0]));
  uint8_t rowhash[32], lawhash[32], compiler[32] = {1, 2};
  char err[256];
  ASSERT(
      pl_store_save_pin(rt.store, rt.t->vstack[1], rowhash, err, sizeof(err)),
      "%s", err);
  pl_vpush(rt.t, test_law(rt.t, 1, 7, 1));
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[2]));
  ASSERT(
      pl_store_save_root(rt.store, rt.t->vstack[3], lawhash, err, sizeof(err)),
      "%s", err);
  uint8_t pre[68], key[32];
  memcpy(pre, "plgc", 4);
  memcpy(pre + 4, compiler, 32);
  memcpy(pre + 36, lawhash, 32);
  ax_sha256(pre, sizeof(pre), key);
  ASSERT(pl_store_backend_put(rt.store, key, rowhash, 32));
  ASSERT(pl_store_put_compiler(rt.store, compiler));
  ASSERT_NOT_NULL(pl_store_code_demand(rt.store, rt.t->vstack[3]));
  ASSERT_EQ(pl_apply(rt.t, rt.t->vstack[3], 321), 321);
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[3], NULL, err, sizeof(err)),
         "%s", err);
  close_rt();
  pl_codecache_reset();
  uint8_t* encoded;
  size_t len;
  ASSERT(pl_codecache_get(key, &encoded, &len));
  ASSERT_GT(len, 0);
  free(encoded);
  char arena[256];
  snprintf(arena, sizeof(arena), "%s/arena.dat", native_dir);
  ASSERT_EQ(unlink(arena), 0);
  reopen_rt();
  pl_vpush(rt.t, test_law(rt.t, 1, 7, 1));
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[0]));
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[1], NULL, err, sizeof(err)),
         "%s", err);
  ASSERT_FALSE(pl_store_backend_get(rt.store, key, &encoded, &len));
  ASSERT(pl_store_put_compiler(rt.store, compiler));
  /* Compiler hash is deliberately absent: only the global Silo row can supply
   * code. */
  ASSERT_NOT_NULL(pl_store_code_demand(rt.store, rt.t->vstack[1]));
  ASSERT_EQ(pl_apply(rt.t, rt.t->vstack[1], 654), 654);
  pl_val row;
  ASSERT(pl_native_load(rt.store, rowhash, &row, err, sizeof(err)), "%s", err);
  ASSERT_GTE(pl_addr(row), PL_NATIVE_BASE);
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[1], NULL, err, sizeof(err)),
         "%s", err);
  close_rt();
  pl_codecache_reset();
  setenv("PL_CODECACHE", "0", 1);
  reopen_rt();
  pl_val law = pl_store_load(rt.t, lawhash);
  ASSERT(pl_store_put_compiler(rt.store, compiler));
  ASSERT_NOT_NULL(pl_store_code_demand(rt.store, law));
  ASSERT_EQ(pl_apply(rt.t, law, 987), 987);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  pl_codecache_reset();
  if (saved_dir) {
    setenv("PL_CODECACHE_DIR", saved_dir, 1);
    free(saved_dir);
  } else
    unsetenv("PL_CODECACHE_DIR");
  if (saved_flag) {
    setenv("PL_CODECACHE", saved_flag, 1);
    free(saved_flag);
  } else
    unsetenv("PL_CODECACHE");
  const char* files[] = {"plgc.mdb", "lock.mdb", "plgc.lock"};
  for (size_t i = 0; i < 3; i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", cache, files[i]);
    (void)unlink(path);
  }
  (void)rmdir(cache);
}

/* Offset assertions deliberately encode the v1 disk ABI. Mutations below
 * recompute checksums so validation must inspect the contents too. */
static void rewrite_super(int fd, unsigned slot, size_t field, uint64_t value) {
  uint8_t sb[184];
  off_t at = (off_t)(slot * PL_NATIVE_GRANULE);
  ASSERT_EQ(pread(fd, sb, sizeof(sb), at), sizeof(sb));
  memcpy(sb + field, &value, 8);
  ax_sha256(sb + 32, sizeof(sb) - 32, sb);
  ASSERT_EQ(pwrite(fd, sb, sizeof(sb), at), sizeof(sb));
}
TEST(native, incompatible_profile_and_capacity_rejection) {
  close_rt();
  char path[256], err[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  rewrite_super(fd, 0, 72, PL_NATIVE_BASE + PL_NATIVE_GRANULE);
  ASSERT_NULL(pl_store_new_native(native_dir, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "incompatible"));
  rewrite_super(fd, 0, 72, PL_NATIVE_BASE);
  /* A valid empty sparse image at its capacity has no allocation room. */
  ASSERT_EQ(ftruncate(fd, (off_t)PL_NATIVE_BYTES), 0);
  rewrite_super(fd, 0, 96, PL_NATIVE_BYTES);
  close(fd);
  reopen_rt();
  pl_vpush(rt.t, pl_pin(rt.t, 42));
  ASSERT_FALSE(
      pl_store_save_pin(rt.store, rt.t->vstack[0], NULL, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "capacity"));
  ASSERT_FALSE(pl_pin_is_hashed(rt.t->vstack[0]));
}
TEST(native, verifier_detects_damaged_dependency_and_truncation_recovers) {
  uint8_t child_hash[32], root_hash[32];
  pl_val leaf = save_nat(77, child_hash, false);
  pl_vpush(rt.t, pl_pin(rt.t, leaf));
  char err[256];
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[0], root_hash, err,
                            sizeof(err)),
         "%s", err);
  close_rt();
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  uint8_t byte;
  off_t at = (off_t)(pl_addr(leaf) - PL_NATIVE_BASE + 40);
  ASSERT_EQ(pread(fd, &byte, 1, at), 1);
  byte ^= 1;
  ASSERT_EQ(pwrite(fd, &byte, 1, at), 1);
  close(fd);
  reopen_rt(); /* normal startup does not traverse dependencies */
  ASSERT_FALSE(pl_store_verify(rt.store, err, sizeof(err)));
  close_rt();
  fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  ASSERT_EQ(ftruncate(fd, (off_t)(2 * PL_NATIVE_GRANULE)), 0);
  close(fd);
  reopen_rt();
  ASSERT_FALSE(pl_store_get_root(rt.store, root_hash));
}
TEST(native, live_inspector_refreshes_without_observing_ice) {
  uint8_t hash[32];
  (void)save_nat(42, hash, true);
  int to_child[2], from_child[2];
  ASSERT_EQ(pipe(to_child), 0);
  ASSERT_EQ(pipe(from_child), 0);
  posix_spawn_file_actions_t actions;
  ASSERT_EQ(posix_spawn_file_actions_init(&actions), 0);
  ASSERT_EQ(
      posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO), 0);
  ASSERT_EQ(
      posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO),
      0);
  char* args[] = {native_binary, "--native-child", "live", native_dir, "", "",
                  NULL};
  ASSERT_EQ(posix_spawn_file_actions_addclose(&actions, to_child[0]), 0);
  ASSERT_EQ(posix_spawn_file_actions_addclose(&actions, to_child[1]), 0);
  ASSERT_EQ(posix_spawn_file_actions_addclose(&actions, from_child[0]), 0);
  ASSERT_EQ(posix_spawn_file_actions_addclose(&actions, from_child[1]), 0);
  pid_t pid;
  ASSERT_EQ(posix_spawn(&pid, native_binary, &actions, NULL, args, environ), 0);
  posix_spawn_file_actions_destroy(&actions);
  close(to_child[0]);
  close(from_child[1]);
  uint64_t value;
  ASSERT_EQ(read(from_child[0], &value, 8), 8);
  ASSERT_EQ(value, 42);
  pl_val next = save_nat(43, hash, false);
  ASSERT_EQ(write(to_child[1], "x", 1), 1);
  ASSERT_EQ(read(from_child[0], &value, 8), 8);
  ASSERT_EQ(value, 42);
  char err[256];
  ASSERT(pl_store_save_root(rt.store, next, NULL, err, sizeof(err)), "%s", err);
  ASSERT_EQ(write(to_child[1], "x", 1), 1);
  ASSERT_EQ(read(from_child[0], &value, 8), 8);
  ASSERT_EQ(value, 43);
  close(to_child[1]);
  close(from_child[0]);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_EQ(status, 0);
}

static void readonly_fault(int signum) {
  _exit(128 + signum);
}
TEST(native, published_pages_are_readonly) {
  uint8_t hash[32];
  (void)save_nat(42, hash, true);
  int result = child("write", NULL, NULL);
  ASSERT(result == 128 + SIGBUS || result == 128 + SIGSEGV);
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, hash))), 42);
}
TEST(native, invalid_index_reference_is_not_a_checkpoint) {
  uint8_t hash[32];
  (void)save_nat(42, hash, true);
  close_rt();
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  uint64_t index;
  ASSERT_EQ(pread(fd, &index, 8, (off_t)(PL_NATIVE_GRANULE + 104)), 8);
  uint8_t node[112];
  ASSERT_EQ(pread(fd, node, sizeof(node), (off_t)index), sizeof(node));
  uint64_t invalid = PL_NATIVE_BYTES;
  memcpy(node + 104, &invalid, 8);
  ax_sha256(node + 32, sizeof(node) - 32, node);
  ASSERT_EQ(pwrite(fd, node, sizeof(node), (off_t)index), sizeof(node));
  close(fd);
  reopen_rt();
  ASSERT_FALSE(pl_store_get_root(rt.store, hash));
}

TEST(native, verifier_checks_layout_and_edges_even_with_valid_checksum) {
  pl_vpush(rt.t, test_app1(rt.t, 0, 99));
  pl_nf(rt.t, rt.t->vstack[0]);
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[0]));
  uint8_t hash[32];
  char err[256];
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[1], hash, err, sizeof(err)),
         "%s", err);
  pl_val pin = pl_store_load(rt.t, hash);
  uint64_t off = pl_addr(pin) - PL_NATIVE_BASE - 48;
  uint64_t app_offset =
      pl_addr(pl_pin_body(pl_ptr(pin))) - PL_NATIVE_BASE - off;
  close_rt();
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  uint64_t len;
  ASSERT_EQ(pread(fd, &len, 8, (off_t)(off + 32)), 8);
  uint8_t* bytes = malloc((size_t)len);
  ASSERT_NOT_NULL(bytes);
  ASSERT_EQ(pread(fd, bytes, (size_t)len, (off_t)off), (ssize_t)len);
  pl_cell* header = (pl_cell*)(bytes + app_offset);
  *header = pl_hdr_set_meta(
      *header, 1); /* changes evaluation, but not the canonical hash */
  ax_sha256(bytes + 32, (size_t)len - 32, bytes);
  ASSERT_EQ(pwrite(fd, bytes, (size_t)len, (off_t)off), (ssize_t)len);
  close(fd);
  reopen_rt();
  ASSERT_FALSE(pl_store_verify(rt.store, err, sizeof(err)));
  close_rt();
  *header = pl_hdr_set_meta(*header, 0);
  pl_val invalid = pl_make(
      PL_TAG_APP, (void*)(uintptr_t)(PL_NATIVE_BASE + PL_NATIVE_BYTES - 8));
  memcpy(bytes + app_offset + 16, &invalid, 8);
  ax_sha256(bytes + 32, (size_t)len - 32, bytes);
  fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  ASSERT_EQ(pwrite(fd, bytes, (size_t)len, (off_t)off), (ssize_t)len);
  close(fd);
  free(bytes);
  reopen_rt();
  ASSERT_FALSE(pl_store_verify(rt.store, err, sizeof(err)));
}

/* Count the actual v1 records, including obsolete records and abandoned
 * staging. This catches write amplification that live-index accounting hides.
 */
static uint64_t index_records(void) {
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDONLY);
  ASSERT_GTE(fd, 0);
  struct stat st;
  ASSERT_EQ(fstat(fd, &st), 0);
  uint64_t count = 0, at = 2 * PL_NATIVE_GRANULE;
  while (at < (uint64_t)st.st_size) {
    uint64_t boundary = (at / PL_NATIVE_GRANULE + 1) * PL_NATIVE_GRANULE;
    if (boundary - at < 48) {
      at = boundary;
      continue;
    }
    uint64_t header[6];
    ASSERT_EQ(pread(fd, header, sizeof(header), (off_t)at), sizeof(header));
    uint64_t len = header[4], type = header[5];
    if (len == 0) {
      at = boundary;
      continue;
    }
    ASSERT_GTE(len, 48);
    ASSERT_LTE(at + len, (uint64_t)st.st_size);
    if (type == 2) {
      ASSERT_EQ(len, 112);
      uint64_t fields[8];
      ASSERT_EQ(pread(fd, fields, sizeof(fields), (off_t)(at + 48)),
                sizeof(fields));
      if (fields[0] < 256) {
        /* Overlay handles are odd; only aligned disk offsets may escape. */
        ASSERT_EQ(fields[5] & 7, 0);
        ASSERT_EQ(fields[6] & 7, 0);
        ASSERT_GT(fields[5], 0);
        ASSERT_GT(fields[6], 0);
      }
      count++;
    }
    at += len;
  }
  close(fd);
  return count;
}
static pl_val stage_batch(size_t count, uint8_t hash[32]) {
  size_t slot = rt.t->vsp;
  for (size_t i = 0; i < count; i++)
    pl_vpush(rt.t, pl_pin(rt.t, (pl_val)(1000 + i)));
  pl_gc_reserve(rt.t, PL_APP_CELLS(count));
  pl_vpush(rt.t, pl_mk_app_from(rt.t, 0, (uint32_t)count, &rt.t->vstack[slot]));
  rt.t->vstack[slot + count] = pl_nf(rt.t, rt.t->vstack[slot + count]);
  pl_vpush(rt.t, pl_pin(rt.t, rt.t->vstack[slot + count]));
  char err[256];
  ASSERT(pl_store_save_pin(rt.store, rt.t->vstack[slot + count + 1], hash, err,
                           sizeof(err)),
         "%s", err);
  pl_val out = pl_make(PL_TAG_PIN,
                       pl_pin_resolved(pl_ptr(rt.t->vstack[slot + count + 1])));
  rt.t->vsp = slot;
  return out;
}
TEST(native, checkpoint_serializes_only_final_indexes_and_reuses_subtrees) {
  uint8_t hash[32];
  pl_val root = stage_batch(256, hash);
  ASSERT_EQ(index_records(), 0); /* Ice publishes objects, not index records. */
  uint8_t key[32] = {0};
  for (unsigned i = 0; i < 32; i++) {
    key[0] = (uint8_t)i;
    ASSERT(pl_store_backend_put(rt.store, key, hash, 32));
  }
  ASSERT_EQ(index_records(), 0);
  char err[256];
  ASSERT(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)), "%s", err);
  uint64_t first = 2 * 257 - 1 + 2 * 32 - 1;
  ASSERT_EQ(index_records(), first);
  pl_store_usage usage;
  ASSERT(pl_store_get_usage(rt.store, &usage, err, sizeof(err)), "%s", err);
  ASSERT_EQ(usage.index_bytes, first * 112);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  /* A changed leaf copies only its path; the rest of the committed tree and
   * cache are retained. Reopen also proves no overlay handles reached disk. */
  uint8_t next[32];
  (void)save_nat(9000, next, true);
  ASSERT_GT(index_records(), first);
  ASSERT_LTE(index_records() - first, 258);
  ASSERT_EQ(pl_store_root_log_head(rt.store), 2);
  close_rt();
  reopen_rt();
  ASSERT_EQ(pl_store_load(rt.t, hash), root);
  pl_cell* app = pl_ptr(pl_pin_body(pl_ptr(root)));
  for (unsigned i = 0; i < 256; i++)
    ASSERT_EQ(pl_pin_body(pl_ptr(pl_app_args(app)[i])), 1000 + i);
  for (unsigned i = 0; i < 32; i++) {
    key[0] = (uint8_t)i;
    uint8_t* bytes;
    size_t len;
    ASSERT(pl_store_backend_get(rt.store, key, &bytes, &len));
    ASSERT_EQ(len, 32);
    ASSERT_MEM_EQ(bytes, hash, 32);
    free(bytes);
  }
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}
typedef struct index_fault {
  unsigned remaining;
} index_fault;
static bool fail_index(void* ctx, const char* point) {
  index_fault* fault = ctx;
  return strcmp(point, "index") != 0 || --fault->remaining != 0;
}
TEST(native, partial_index_emission_is_reused_on_retry) {
  uint8_t hash[32];
  pl_val root = stage_batch(64, hash);
  char err[256];
  index_fault fault = {.remaining = 7};
  pl_native_set_fault(rt.store, fail_index, &fault);
  ASSERT_FALSE(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)));
  ASSERT_EQ(index_records(), 7);
  ASSERT_FALSE(pl_store_get_root(rt.store, hash));
  ASSERT_EQ(child("empty", NULL, NULL), 0);
  pl_native_set_fault(rt.store, NULL, NULL);
  ASSERT(pl_store_save_root(rt.store, root, hash, err, sizeof(err)), "%s", err);
  ASSERT_EQ(index_records(), 2 * 65 - 1);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  reopen_rt();
  ASSERT_EQ(pl_store_load(rt.t, hash), root);
}
TEST(native, failed_checkpoint_allows_index_and_cache_changes_before_retry) {
  uint8_t old[32], row[32], added[32];
  (void)save_nat(42, old, true);
  pl_val root = stage_batch(64, row);
  uint8_t key[32] = {7};
  ASSERT(pl_store_backend_put(rt.store, key, old, 32));
  char err[256];
  pl_native_set_fault(rt.store, fault_return, "after_data_sync");
  ASSERT_FALSE(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)));
  uint64_t emitted = index_records();
  pl_native_set_fault(rt.store, NULL, NULL);
  (void)save_nat(9000, added, false);
  /* Replace a memoized cache leaf twice, and add another branch. Old sealed
   * records must stay valid while all changed ancestor offsets are forgotten.
   */
  ASSERT(pl_store_backend_put(rt.store, key, row, 32));
  ASSERT(pl_store_backend_put(rt.store, key, added, 32));
  uint8_t other[32] = {8};
  ASSERT(pl_store_backend_put(rt.store, other, old, 32));
  ASSERT(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)), "%s", err);
  ASSERT_GT(index_records(), emitted);
  ASSERT_LT(index_records() - emitted, 32);
  ASSERT_EQ(pl_store_root_log_head(rt.store), 2);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  reopen_rt();
  uint8_t actual[32], *bytes;
  size_t len;
  ASSERT(pl_store_get_root(rt.store, actual));
  ASSERT_MEM_EQ(actual, row, 32);
  ASSERT(pl_store_backend_get(rt.store, key, &bytes, &len));
  ASSERT_EQ(len, 32);
  ASSERT_MEM_EQ(bytes, added, 32);
  free(bytes);
  ASSERT(pl_store_backend_get(rt.store, other, &bytes, &len));
  ASSERT_MEM_EQ(bytes, old, 32);
  free(bytes);
  ASSERT_EQ(pl_pin_body(pl_ptr(pl_store_load(rt.t, added))), 9000);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}

TEST(native, committed_index_checksums_are_not_trusted_by_working_overlay) {
  uint8_t hash[32], added[32];
  pl_val root = stage_batch(64, hash);
  char err[256];
  ASSERT(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)), "%s", err);
  (void)save_nat(9000, added, false);
  char path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  uint64_t off;
  ASSERT_EQ(pread(fd, &off, 8, (off_t)(PL_NATIVE_GRANULE + 104)), 8);
  /* Locate the immutable leaf for the old root; insertion reuses this leaf
   * even when its ancestors have been copied into the working overlay. */
  uint64_t node[14];
  for (unsigned depth = 0;; depth++) {
    ASSERT_LTE(depth, 256);
    ASSERT_EQ(pread(fd, node, sizeof(node), (off_t)off), sizeof(node));
    uint64_t bit = node[6];
    if (bit == 256)
      break;
    ASSERT_LT(bit, 256);
    unsigned direction = (hash[bit / 8] >> (7 - bit % 8)) & 1u;
    off = node[11 + direction];
  }
  ASSERT_MEM_EQ((uint8_t*)node + 56, hash, 32);
  uint8_t bad = (uint8_t)(node[0] ^ 1);
  ASSERT_EQ(pwrite(fd, &bad, 1, (off_t)off), 1);
  close(fd);
  pl_val pin;
  ASSERT_FALSE(pl_native_load(rt.store, hash, &pin, err, sizeof(err)));
  ASSERT_NOT_NULL(strstr(err, "index"));
  ASSERT_FALSE(pl_store_verify(rt.store, err, sizeof(err)));
}

typedef struct native_reader {
  pl_store* store;
  uint8_t key[32], hash[32];
  bool ok;
} native_reader;
static void* native_read_worker(void* ctx) {
  native_reader* reader = ctx;
  reader->ok = true;
  for (unsigned i = 0; i < 100; i++) {
    uint8_t* bytes = NULL;
    size_t len = 0;
    if (!pl_store_backend_get(reader->store, reader->key, &bytes, &len) ||
        len != 32 || memcmp(bytes, reader->hash, 32) != 0) {
      free(bytes);
      reader->ok = false;
      break;
    }
    free(bytes);
    uint64_t object_len;
    pl_hash* subs = NULL;
    size_t count;
    if (!pl_store_object_info(reader->store, reader->hash, &object_len, &subs,
                              &count, NULL, 0) ||
        count != 0)
      reader->ok = false;
    free(subs);
  }
  return NULL;
}
TEST(native, digest_context_is_serialized_for_writer_and_inspector_threads) {
  uint8_t hash[32], key[32] = {4};
  pl_val root = save_nat(42, hash, true);
  ASSERT(pl_store_backend_put(rt.store, key, hash, 32));
  char err[256];
  ASSERT(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)), "%s", err);
  for (unsigned readonly = 0; readonly < 2; readonly++) {
    native_reader readers[8];
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++) {
      readers[i] = (native_reader){.store = rt.store};
      memcpy(readers[i].key, key, 32);
      memcpy(readers[i].hash, hash, 32);
      ASSERT_EQ(
          pthread_create(&threads[i], NULL, native_read_worker, &readers[i]),
          0);
    }
    if (!readonly) {
      uint8_t staged[32];
      for (unsigned i = 0; i < 16; i++)
        (void)save_nat(100 + i, staged, false);
      ASSERT(pl_store_save_root(rt.store, root, NULL, err, sizeof(err)), "%s",
             err);
    }
    for (unsigned i = 0; i < 8; i++) {
      ASSERT_EQ(pthread_join(threads[i], NULL), 0);
      ASSERT(readers[i].ok);
    }
    ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
    if (!readonly) {
      close_rt();
      rt.store = pl_store_new_native_ro(native_dir, err, sizeof(err));
      ASSERT_NOT_NULL(rt.store, "%s", err);
      rt.heap = pl_heap_new(1 << 16, rt.store);
      rt.t = pl_thread_new(rt.heap);
    }
  }
}

/* A history value is reachable even when its original containing PIN is not.
 * This is the important distinction between native raw edges and subpin-only
 * reachability. The extra half of the old extent must actually be collected. */
TEST(native, compaction_retains_live_history_and_borrowed_raw_graphs) {
  char err[256];
  uint8_t obsolete[32], root_hash[32], old_hash[32], restored[32];
  size_t count = 1024 * 1024;
  uint8_t* data = malloc(count);
  ASSERT_NOT_NULL(data);
  memset(data, 0xad, count);
  pl_vpush(rt.t, pl_nat_from_bytes(rt.t, data, count));
  memset(data, 0xbd, count);
  pl_vpush(rt.t, pl_nat_from_bytes(rt.t, data, count));
  free(data);
  pl_val fields[2] = {rt.t->vstack[0], rt.t->vstack[1]};
  pl_vpush(rt.t, pl_pin(rt.t, test_app(rt.t, 0, 2, fields)));
  ASSERT(
      pl_store_save_root(rt.store, rt.t->vstack[2], obsolete, err, sizeof(err)),
      "%s", err);
  pl_val raw = pl_app_args(
      pl_ptr(pl_pin_body(pl_ptr(pl_store_load(rt.t, obsolete)))))[0];
  pl_val old = save_nat(17, old_hash, false);
  pl_val history[3] = {old, raw, raw};
  pl_vpush(rt.t, pl_pin(rt.t, test_app(rt.t, 0, 3, history)));
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[rt.t->vsp - 1], root_hash,
                            err, sizeof(err)),
         "%s", err);
  close_rt();
  uint64_t before, after;
  ASSERT(pl_store_native_compact(native_dir, &before, &after, err, sizeof(err)),
         "%s", err);
  ASSERT_GT(before - after, count);
  reopen_rt();
  ASSERT(pl_store_get_root(rt.store, restored));
  ASSERT_MEM_EQ(root_hash, restored, 32);
  pl_val body = pl_pin_body(pl_ptr(pl_store_load(rt.t, restored)));
  const pl_val* args = pl_app_args(pl_ptr(body));
  ASSERT_EQ(pl_pin_body(pl_ptr(args[0])), 17);
  ASSERT_EQ(args[1], args[2]);
  ASSERT_EQ(pl_nat_byte_len(args[1]), count);
  ASSERT_EQ(pl_nat_byte_at(args[1], count - 1), 0xad);
  pl_val missing;
  ASSERT_FALSE(pl_native_load(rt.store, obsolete, &missing, err, sizeof(err)));
  ASSERT(pl_native_load(rt.store, old_hash, &missing, err, sizeof(err)), "%s",
         err);
  ASSERT_EQ(pl_store_root_log_head(rt.store), 1);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  (void)save_nat(43, restored, true);
  ASSERT_EQ(pl_store_root_log_head(rt.store), 2);
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
  close_rt();
  reopen_rt();
  ASSERT(pl_native_load(rt.store, root_hash, &missing, err, sizeof(err)), "%s",
         err);
}
TEST(native, compaction_is_exclusive_and_failure_keeps_original) {
  uint8_t hash[32], retained[32];
  (void)save_nat(42, hash, true);
  ASSERT_EQ(child("compact-blocked", NULL, NULL), 0);
  close_rt();
  char err[256], path[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  struct stat original, current;
  ASSERT_EQ(stat(path, &original), 0);
  const char* points[] = {"compact_after_copy", "compact_before_replace"};
  for (size_t i = 0; i < 2; i++) {
    ASSERT_FALSE(pl_native_compact(native_dir, NULL, NULL, fault_return,
                                   (void*)points[i], err, sizeof(err)));
    ASSERT_EQ(stat(path, &current), 0);
    ASSERT_EQ(original.st_ino, current.st_ino);
    ASSERT_EQ(original.st_size, current.st_size);
    reopen_rt();
    ASSERT(pl_store_get_root(rt.store, retained));
    ASSERT_MEM_EQ(hash, retained, 32);
    ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
    close_rt();
  }
}
TEST(native, compaction_crashes_leave_a_complete_native_world) {
  uint8_t hash[32], retained[32];
  (void)save_nat(42, hash, true);
  close_rt();
  const char* points[] = {"compact_after_copy", "compact_before_replace",
                          "compact_after_replace"};
  char err[256];
  for (size_t i = 0; i < 3; i++) {
    ASSERT_EQ(child("compact-crash", points[i], NULL), 77);
    reopen_rt();
    ASSERT(pl_store_get_root(rt.store, retained));
    ASSERT_MEM_EQ(hash, retained, 32);
    ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
    close_rt();
  }
  /* Killed collectors can leave an uninstalled native candidate, never a
   * second authoritative world. Remove this test's candidates. */
  DIR* dir = opendir(native_dir);
  ASSERT_NOT_NULL(dir);
  struct dirent* item;
  while ((item = readdir(dir))) {
    if (strncmp(item->d_name, ".compact-", 9))
      continue;
    char path[512];
    const char* files[] = {"arena.dat", "arena.commit.lock"};
    for (size_t i = 0; i < 2; i++) {
      snprintf(path, sizeof(path), "%s/%s/%s", native_dir, item->d_name,
               files[i]);
      (void)unlink(path);
    }
    snprintf(path, sizeof(path), "%s/%s", native_dir, item->d_name);
    ASSERT_EQ(rmdir(path), 0);
  }
  closedir(dir);
}
TEST(native, compaction_verifies_content_before_replacing) {
  pl_vpush(rt.t, pl_pin(rt.t, test_app1(rt.t, 0, 99)));
  uint8_t hash[32];
  char err[256], path[256];
  ASSERT(pl_store_save_root(rt.store, rt.t->vstack[0], hash, err, sizeof(err)),
         "%s", err);
  pl_val pin = pl_store_load(rt.t, hash);
  uint64_t off = pl_addr(pin) - PL_NATIVE_BASE - 48;
  uint64_t body = pl_addr(pl_pin_body(pl_ptr(pin))) - PL_NATIVE_BASE - off;
  close_rt();
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  int fd = open(path, O_RDWR);
  ASSERT_GTE(fd, 0);
  struct stat before, after;
  ASSERT_EQ(fstat(fd, &before), 0);
  uint64_t len;
  ASSERT_EQ(pread(fd, &len, 8, (off_t)(off + 32)), 8);
  uint8_t* bytes = malloc((size_t)len);
  ASSERT_NOT_NULL(bytes);
  ASSERT_EQ(pread(fd, bytes, (size_t)len, (off_t)off), (ssize_t)len);
  /* Plausible graph with a valid record checksum, but the wrong content. */
  ((pl_cell*)(bytes + body))[2] = 100;
  ax_sha256(bytes + 32, (size_t)len - 32, bytes);
  ASSERT_EQ(pwrite(fd, bytes, (size_t)len, (off_t)off), (ssize_t)len);
  close(fd);
  free(bytes);
  ASSERT_FALSE(
      pl_store_native_compact(native_dir, NULL, NULL, err, sizeof(err)));
  ASSERT_EQ(stat(path, &after), 0);
  ASSERT_EQ(before.st_ino, after.st_ino);
  ASSERT_EQ(before.st_size, after.st_size);
}
TEST(native, compaction_of_empty_native_store_is_valid) {
  close_rt();
  char err[256];
  ASSERT(pl_store_native_compact(native_dir, NULL, NULL, err, sizeof(err)),
         "%s", err);
  reopen_rt();
  uint8_t hash[32];
  ASSERT_FALSE(pl_store_get_root(rt.store, hash));
  ASSERT(pl_store_verify(rt.store, err, sizeof(err)), "%s", err);
}
TEST(native, compaction_does_not_create_missing_arena) {
  close_rt();
  char path[256], err[256];
  snprintf(path, sizeof(path), "%s/arena.dat", native_dir);
  ASSERT_EQ(unlink(path), 0);
  ASSERT_FALSE(
      pl_store_native_compact(native_dir, NULL, NULL, err, sizeof(err)));
  ASSERT_EQ(access(path, F_OK), -1);
}
static int child_main(int argc, char** argv) {
  if (argc < 6)
    return 90;
  const char* mode = argv[2];
  char err[256];
  if (!strcmp(mode, "compact-crash")) {
    (void)pl_native_compact(argv[3], NULL, NULL, fault_crash, argv[4], err,
                            sizeof(err));
    return 98;
  }
  if (!strcmp(mode, "compact-blocked"))
    return !pl_store_native_compact(argv[3], NULL, NULL, err, sizeof(err)) &&
                   strstr(err, "writer")
               ? 0
               : 91;
  bool writer =
      strcmp(mode, "crash") == 0 || strcmp(mode, "writer-blocked") == 0;
  pl_store* s = writer ? pl_store_new_native(argv[3], err, sizeof(err))
                       : pl_store_new_native_ro(argv[3], err, sizeof(err));
  if (strcmp(mode, "writer-blocked") == 0)
    return s == NULL && strstr(err, "writer") != NULL ? 0 : 91;
  if (s == NULL) {
    fprintf(stderr, "%s\n", err);
    return 92;
  }
  pl_heap* h = pl_heap_new(1 << 16, s);
  pl_thread* t = pl_thread_new(h);
  uint8_t hash[32];
  int result = 0;
  if (strcmp(mode, "live") == 0) {
    pl_val first = 0;
    for (unsigned i = 0; i < 3; i++) {
      if (i != 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
          return 80;
      }
      if (!pl_store_get_root(s, hash))
        return 81;
      pl_val p = pl_store_load(t, hash);
      if (i == 0)
        first = p;
      if (pl_pin_body(pl_ptr(first)) != 42)
        return 82;
      uint64_t value = pl_pin_body(pl_ptr(p));
      if (write(STDOUT_FILENO, &value, 8) != 8)
        return 83;
    }
  } else if (strcmp(mode, "empty") == 0)
    result = pl_store_get_root(s, hash) ? 93 : 0;
  else if (strcmp(mode, "read") == 0) {
    if (!pl_store_get_root(s, hash))
      result = 94;
    else {
      if (ax_arrlen(s->pins) != 0)
        result = 95;
      pl_val p = pl_store_load(t, hash);
      if (pl_pin_body(pl_ptr(p)) != strtoull(argv[4], NULL, 10) ||
          p != strtoull(argv[5], NULL, 10))
        result = 96;
      if (!pl_store_verify(s, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        result = 97;
      }
    }
  } else if (strcmp(mode, "write") == 0) {
    if (!pl_store_get_root(s, hash))
      return 84;
    pl_val pin = pl_store_load(t, hash);
    signal(SIGSEGV, readonly_fault);
    signal(SIGBUS, readonly_fault);
    ((volatile pl_cell*)pl_ptr(pin))[5] = 99;
    result = 85;
  } else if (strcmp(mode, "crash") == 0) {
    pl_native_set_fault(s, fault_crash, argv[4]);
    pl_vpush(t, pl_pin(t, 43));
    if (!pl_store_save_root(s, t->vstack[0], hash, err, sizeof(err)))
      fprintf(stderr, "%s\n", err);
    result = 98; /* every configured crash point must be reached */
  }
  pl_thread_free(t);
  pl_heap_free(h);
  pl_store_free(s);
  return result;
}
int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--native-child") == 0)
    return child_main(argc, argv);
  if (realpath(argv[0], native_binary) == NULL)
    return 99;
  return native_test_main(argc, argv);
}
