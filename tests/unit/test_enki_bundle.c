#ifdef ENKI_BUNDLE_CRITERION
#include <criterion/criterion.h>
#define TEST(suite, name)    Test(suite, name)
#define ASSERT(...)          cr_assert(__VA_ARGS__)
#define ASSERT_EQ(...)       cr_assert_eq(__VA_ARGS__)
#define ASSERT_NOT_NULL(...) cr_assert_not_null(__VA_ARGS__)
#else
#include "test.h"
#endif
#include "test_bundle.h"

TEST(bundle, closure_round_trip_and_read_only) {
  uint8_t* blob = NULL;
  size_t len = 0;
  ASSERT(test_bundle_make(&blob, &len));
  en_bundle b;
  char err[256];
  ASSERT(en_bundle_validate(blob, len, &b, err, sizeof(err)), "%s", err);
  en_bundle invalid;
  /* The trailer and its CRC remain intact; only the available payload shrinks.
   */
  ASSERT(!en_bundle_validate(blob + 1, len - 1, &invalid, err, sizeof(err)));
  ASSERT_EQ(strcmp(err, "bad bundle payload length"), 0);
  pl_val entry;
  pl_store* s = pl_store_new_bundle(b.payload, b.payload_len, b.entry_hash,
                                    &entry, err, sizeof(err));
  ASSERT_NOT_NULL(s, "%s", err);
  uint8_t hash[32];
  ASSERT(!pl_store_save_pin(s, entry, hash, err, sizeof(err)));
  ASSERT(!pl_store_put_root(s, b.entry_hash));
  ASSERT(s->be.has(s->be.ctx, b.entry_hash));
  uint8_t* encoded;
  size_t encoded_len;
  ASSERT(
      pl_store_bundle_encode(entry, &encoded, &encoded_len, err, sizeof(err)));
  ASSERT_EQ(encoded_len, b.payload_len);
  ASSERT_EQ(memcmp(encoded, b.payload, encoded_len), 0);
  free(encoded);
  pl_store_free(s);
  free(blob);
}

TEST(bundle, every_single_bit_corruption_and_truncation) {
  uint8_t* blob = NULL;
  size_t len = 0;
  ASSERT(test_bundle_make(&blob, &len));
  ASSERT_EQ(test_bundle_boot(blob, len), 0);
  for (size_t i = 0; i < len; i++) {
    ASSERT_EQ(test_bundle_boot(blob, i), 126, "truncation at %zu", i);
    for (unsigned bit = 0; bit < 8; bit++) {
      blob[i] ^= (uint8_t)(1U << bit);
      ASSERT_EQ(test_bundle_boot(blob, len), 126, "byte %zu bit %u", i, bit);
      blob[i] ^= (uint8_t)(1U << bit);
    }
  }
  free(blob);
}

TEST(bundle, valid_crc_with_bad_entry_hash) {
  uint8_t* blob = NULL;
  size_t len = 0;
  ASSERT(test_bundle_make(&blob, &len));
  en_bundle b;
  char err[256];
  ASSERT(en_bundle_validate(blob, len, &b, err, sizeof(err)));
  b.entry_hash[0] ^= 1;
  uint8_t* bad;
  size_t bad_len;
  ASSERT(en_bundle_blob(b.payload, b.payload_len, b.entry_hash, &bad, &bad_len,
                        err, sizeof(err)));
  ASSERT_EQ(test_bundle_boot(bad, bad_len), 126);
  free(bad);
  free(blob);
}

#ifdef __APPLE__
#include "../../pkg/enki/src/bundle_internal.h"
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <unistd.h>

TEST(bundle, rejects_fat_and_unknown_macho_commands) {
  char source[] = "/tmp/enki-macho-src-XXXXXX";
  char output[] = "/tmp/enki-macho-dst-XXXXXX";
  int src = mkstemp(source);
  int dst = mkstemp(output);
  ASSERT(src >= 0 && dst >= 0);
  (void)unlink(source);
  (void)unlink(output);
  struct mach_header_64 h = {.magic = FAT_CIGAM};
  ASSERT_EQ(write(src, &h, sizeof(h)), (ssize_t)sizeof(h));
  char err[256];
  uint8_t blob = 0;
  ASSERT(!en_bundle_platform_write(src, dst, &blob, 1, err, sizeof(err)));
  ASSERT(strstr(err, "fat Mach-O") != NULL);

  struct {
    struct mach_header_64 header;
    struct segment_command_64 enki;
    struct section_64 payload;
    struct segment_command_64 linkedit;
    struct load_command unknown;
  } image = {
      .header = {.magic = MH_MAGIC_64,
                 .cputype = CPU_TYPE_ARM64,
                 .filetype = MH_EXECUTE,
                 .ncmds = 3,
                 .sizeofcmds = sizeof(image) - sizeof(h)},
      .enki = {.cmd = LC_SEGMENT_64,
               .cmdsize = sizeof(image.enki) + sizeof(image.payload),
               .segname = "__ENKI",
               .vmaddr = 0x100004000,
               .vmsize = 16384,
               .fileoff = 16384,
               .filesize = 16384,
               .nsects = 1},
      .payload = {.sectname = "__payload",
                  .segname = "__ENKI",
                  .addr = 0x100004000,
                  .size = 8,
                  .offset = 16384},
      .linkedit = {.cmd = LC_SEGMENT_64,
                   .cmdsize = sizeof(image.linkedit),
                   .segname = "__LINKEDIT",
                   .vmaddr = 0x100008000,
                   .vmsize = 16384,
                   .fileoff = 32768,
                   .filesize = 8},
      .unknown = {.cmd = 0x12345678, .cmdsize = sizeof(struct load_command)}};
  ASSERT_EQ(pwrite(src, &image, sizeof(image), 0), (ssize_t)sizeof(image));
  ASSERT_EQ(ftruncate(src, 32776), 0);
  ASSERT(!en_bundle_platform_write(src, dst, &blob, 1, err, sizeof(err)));
  ASSERT(strstr(err, "unrecognized Mach-O load command") != NULL);
  close(dst);
  close(src);
}
#endif
