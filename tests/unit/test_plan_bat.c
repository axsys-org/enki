#include "test.h"
#include "test_plan.h"
#include "plan/bat.h"
#include "plan/rplan.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

TEST(bat, binary_chunks_ranges_seek_and_gc) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t n = 9 * PL_BAT_CHUNK_BYTES + 3;
  uint8_t* bytes = malloc(n);
  ASSERT_NOT_NULL(bytes);
  for (size_t i = 0; i < n; i++)
    bytes[i] = (uint8_t)(i * 13);
  bytes[n - 1] = bytes[n - 2] = 0;
  pl_vpush(t, pl_bat_from_bytes(t, bytes, n));
  pl_gc_collect_now(t);
  pl_bat_cursor c;
  ASSERT(pl_bat_open(&c, t->vstack[0]));
  ASSERT_EQ(c.length, n);
  uint8_t buf[8191];
  size_t off = 0, got;
  while ((got = pl_bat_read(&c, buf, sizeof(buf))) != 0) {
    ASSERT_MEM_EQ(buf, bytes + off, got);
    off += got;
  }
  ASSERT_EQ(off, n);
  ASSERT(pl_bat_seek(&c, PL_BAT_CHUNK_BYTES - 2));
  ASSERT_EQ(pl_bat_read(&c, buf, 9), 9);
  ASSERT_MEM_EQ(buf, bytes + PL_BAT_CHUNK_BYTES - 2, 9);
  ASSERT_FALSE(pl_bat_seek(&c, n + 1));
  pl_bat_close(&c);
  pl_val fields[] = {ax_s5('r', 'a', 'n', 'g', 'e'), t->vstack[0],
                     PL_BAT_CHUNK_BYTES - 2, 9};
  pl_vpush(t, test_app(t, 0, 4, fields));
  ASSERT(pl_bat_open(&c, t->vstack[1]));
  ASSERT_EQ(c.length, 9);
  ASSERT_EQ(pl_bat_read(&c, buf, sizeof(buf)), 9);
  ASSERT_MEM_EQ(buf, bytes + PL_BAT_CHUNK_BYTES - 2, 9);
  pl_bat_close(&c);
  free(bytes);
  test_rt_free(&rt);
}

TEST(bat, validation_and_virtual_runs) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_val repeat[] = {ax_s6('r', 'e', 'p', 'e', 'a', 't'), 42,
                     UINT64_C(1) << 40};
  pl_vpush(t, test_app(t, 0, 3, repeat));
  pl_bat_cursor c;
  ASSERT(pl_bat_open(&c, t->vstack[0]));
  ASSERT_EQ(c.count, 1);
  ASSERT_EQ(c.length, UINT64_C(1) << 40);
  uint8_t b[4];
  ASSERT(pl_bat_seek(&c, c.length - 3));
  ASSERT_EQ(pl_bat_read(&c, b, 4), 3);
  ASSERT_EQ(b[2], 42);
  pl_bat_close(&c);
  pl_val wrong[] = {ax_s3('b', 'a', 't'), 1, t->vstack[0]};
  pl_vpush(t, test_app(t, 0, 3, wrong));
  ASSERT_FALSE(pl_bat_open(&c, t->vstack[1]));
  pl_val range[] = {ax_s5('r', 'a', 'n', 'g', 'e'), t->vstack[0],
                    UINT64_C(1) << 40, 1};
  pl_vpush(t, test_app(t, 0, 4, range));
  ASSERT_FALSE(pl_bat_open(&c, t->vstack[2]));
  test_rt_free(&rt);
}

TEST(bat, read_file_above_nat_limit_and_jail) {
  char dir[] = "/tmp/enki-bat-XXXXXX";
  ASSERT_NOT_NULL(mkdtemp(dir));
  char path[256];
  snprintf(path, sizeof(path), "%s/data", dir);
  FILE* f = fopen(path, "wb");
  ASSERT_NOT_NULL(f);
  uint8_t b[65536];
  memset(b, 0xa5, sizeof(b));
  for (size_t i = 0; i < 144; i++)
    ASSERT_EQ(fwrite(b, 1, sizeof(b), f), sizeof(b));
  ASSERT_EQ(fputc(0, f), 0);
  fclose(f);
  test_rt rt = test_rt_new();
  rt.t->rplan_file_root_c = dir;
  pl_vpush(rt.t, pl_rplan_read_bat(rt.t, ax_s4('d', 'a', 't', 'a')));
  pl_bat_cursor c;
  ASSERT(pl_bat_open(&c, rt.t->vstack[0]));
  ASSERT_EQ(c.length, 9 * PL_BAT_CHUNK_BYTES + 1);
  ASSERT(pl_bat_seek(&c, c.length - 2));
  ASSERT_EQ(pl_bat_read(&c, b, 2), 2);
  ASSERT_EQ(b[0], 0xa5);
  ASSERT_EQ(b[1], 0);
  pl_bat_close(&c);
  pl_val escape = pl_nat_from_bytes(rt.t, (const uint8_t*)"../outside", 10);
  ASSERT_EQ(pl_rplan_read_bat(rt.t, escape), 0);
  unlink(path);
  rmdir(dir);
  test_rt_free(&rt);
}
