#include "bundle_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t bundle_magic[8] = {'E', 'N', 'K', 'I', 'B', 'N', 'D', 'L'};

bool en_bundle_error(char* err, size_t cap, const char* fmt, ...) {
  if (err != NULL && cap != 0) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(err, cap, fmt, args);
    va_end(args);
  }
  return false;
}

static uint64_t bundle_le(const uint8_t* bytes, unsigned n) {
  uint64_t value = 0;
  for (unsigned i = 0; i < n; i++) {
    value |= (uint64_t)bytes[i] << (8U * i);
  }
  return value;
}
static void bundle_put(uint8_t* bytes, uint64_t value, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    bytes[i] = (uint8_t)(value >> (8U * i));
  }
}
static uint32_t bundle_crc(const uint8_t* bytes, size_t len) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < len; i++) {
    crc ^= bytes[i];
    for (unsigned j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

bool en_bundle_validate(const uint8_t* blob, size_t len, en_bundle* out,
                        char* err, size_t cap) {
  *out = (en_bundle){0};
  if (blob == NULL || len <= EN_BUNDLE_TRAILER_SIZE) {
    return en_bundle_error(err, cap, "truncated bundle");
  }
  const uint8_t* trailer = blob + len - EN_BUNDLE_TRAILER_SIZE;
  if (memcmp(trailer, bundle_magic, 8) != 0) {
    return en_bundle_error(err, cap, "bad bundle magic");
  }
  if (bundle_le(trailer + 8, 4) != 1 || bundle_le(trailer + 12, 4) != 0) {
    return en_bundle_error(err, cap, "unsupported bundle version or flags");
  }
  if (bundle_crc(trailer, 56) != bundle_le(trailer + 56, 4)) {
    return en_bundle_error(err, cap, "bad bundle trailer CRC");
  }
  if (bundle_le(trailer + 16, 8) != len - EN_BUNDLE_TRAILER_SIZE) {
    return en_bundle_error(err, cap, "bad bundle payload length");
  }
  out->payload = blob;
  out->payload_len = len - EN_BUNDLE_TRAILER_SIZE;
  memcpy(out->entry_hash, trailer + 24, 32);
  return true;
}

bool en_bundle_blob(const uint8_t* payload, size_t len, const uint8_t hash[32],
                    uint8_t** out, size_t* out_len, char* err, size_t cap) {
  *out = NULL;
  *out_len = 0;
  if (len == 0 || len > SIZE_MAX - EN_BUNDLE_TRAILER_SIZE) {
    return en_bundle_error(err, cap, "invalid bundle payload length");
  }
  uint8_t* blob = malloc(len + EN_BUNDLE_TRAILER_SIZE);
  if (blob == NULL) {
    return en_bundle_error(err, cap, "out of memory encoding bundle");
  }
  memcpy(blob, payload, len);
  uint8_t* trailer = blob + len;
  memcpy(trailer, bundle_magic, 8);
  bundle_put(trailer + 8, 1, 4);
  bundle_put(trailer + 12, 0, 4);
  bundle_put(trailer + 16, len, 8);
  memcpy(trailer + 24, hash, 32);
  bundle_put(trailer + 56, bundle_crc(trailer, 56), 4);
  *out = blob;
  *out_len = len + EN_BUNDLE_TRAILER_SIZE;
  return true;
}

void en_bundle_close(en_bundle* bundle) {
  if (bundle->mapping != NULL) {
    (void)munmap(bundle->mapping, bundle->mapping_len);
  }
  *bundle = (en_bundle){0};
}

bool en_bundle_write_all(int fd, const void* bytes, size_t len) {
  const uint8_t* p = bytes;
  while (len != 0) {
    ssize_t n = write(fd, p, len > 1048576 ? 1048576 : len);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return true;
}

bool en_bundle_copy(int src, int dst, uint64_t off, uint64_t len) {
  uint8_t bytes[65536];
  while (len != 0) {
    size_t n = len > sizeof(bytes) ? sizeof(bytes) : (size_t)len;
    ssize_t got = pread(src, bytes, n, (off_t)off);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0 || !en_bundle_write_all(dst, bytes, (size_t)got)) {
      return false;
    }
    off += (uint64_t)got;
    len -= (uint64_t)got;
  }
  return true;
}

bool en_bundle_write(const char* path, const uint8_t* blob, size_t len,
                     char* err, size_t cap) {
  en_bundle checked;
  if (!en_bundle_validate(blob, len, &checked, err, cap)) {
    return false;
  }
  int src = en_bundle_open_self(err, cap);
  if (src < 0) {
    return false;
  }
  size_t n = strlen(path);
  char* tmp = malloc(n + 24);
  if (tmp == NULL) {
    close(src);
    return en_bundle_error(err, cap, "out of memory creating temporary name");
  }
  (void)snprintf(tmp, n + 24, "%s.enki-XXXXXX", path);
  int dst = mkstemp(tmp);
  bool ok = dst >= 0;
  if (!ok) {
    (void)en_bundle_error(err, cap, "cannot create temporary output: %s",
                          strerror(errno));
  }
  if (ok) {
    ok = en_bundle_platform_write(src, dst, blob, len, err, cap);
  }
  close(src);
  if (ok && (fsync(dst) != 0 || fchmod(dst, 0755) != 0)) {
    ok = en_bundle_error(err, cap, "cannot synchronize executable: %s",
                         strerror(errno));
  }
  if (dst >= 0 && close(dst) != 0 && ok) {
    ok = en_bundle_error(err, cap, "cannot close executable: %s",
                         strerror(errno));
  }
  if (ok) {
    ok = en_bundle_sign(tmp, err, cap);
  }
  /* Signing changes the inode contents; flush them again before publication. */
  if (ok) {
    dst = open(tmp, O_RDONLY | O_CLOEXEC);
    if (dst < 0 || fsync(dst) != 0) {
      ok = en_bundle_error(err, cap, "cannot synchronize signed executable: %s",
                           strerror(errno));
    }
    if (dst >= 0) {
      close(dst);
    }
  }
  if (ok && rename(tmp, path) != 0) {
    ok = en_bundle_error(err, cap, "cannot publish executable: %s",
                         strerror(errno));
  }
  if (!ok) {
    (void)unlink(tmp);
  }
  free(tmp);
  return ok;
}
