#include "bundle_internal.h"

#ifdef __linux__
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int en_bundle_open_self(char* err, size_t cap) {
  int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    (void)en_bundle_error(err, cap, "cannot open /proc/self/exe: %s",
                          strerror(errno));
  }
  return fd;
}

/* The ELF extent also distinguishes damaged/truncated trailers from absence.
 * A missing magic must never silently send a bundled tool into the CLI. */
static bool elf_extent(int fd, uint64_t* end, uint64_t* size, char* err,
                       size_t cap) {
  struct stat st;
  Elf64_Ehdr h;
  if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(h) ||
      pread(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
    return en_bundle_error(err, cap, "cannot read ELF header");
  }
  *size = (uint64_t)st.st_size;
  if (memcmp(h.e_ident, ELFMAG, SELFMAG) != 0 ||
      h.e_ident[EI_CLASS] != ELFCLASS64 || h.e_ident[EI_DATA] != ELFDATA2LSB ||
      h.e_shentsize != sizeof(Elf64_Shdr) ||
      h.e_phentsize != sizeof(Elf64_Phdr) || h.e_shnum == 0) {
    return en_bundle_error(err, cap, "unsupported ELF layout");
  }
  uint64_t shbytes = (uint64_t)h.e_shnum * sizeof(Elf64_Shdr);
  uint64_t phbytes = (uint64_t)h.e_phnum * sizeof(Elf64_Phdr);
  if (h.e_shoff > *size || shbytes > *size - h.e_shoff || h.e_phoff > *size ||
      phbytes > *size - h.e_phoff) {
    return en_bundle_error(err, cap, "truncated ELF tables");
  }
  *end = h.e_shoff + shbytes;
  if (h.e_phoff + phbytes > *end) {
    *end = h.e_phoff + phbytes;
  }
  for (unsigned i = 0; i < h.e_shnum; i++) {
    Elf64_Shdr sh;
    if (pread(fd, &sh, sizeof(sh), (off_t)(h.e_shoff + (i * sizeof(sh)))) !=
        (ssize_t)sizeof(sh)) {
      return en_bundle_error(err, cap, "cannot read ELF section");
    }
    if (sh.sh_type == SHT_NOBITS) {
      continue;
    }
    if (sh.sh_offset > *size || sh.sh_size > *size - sh.sh_offset) {
      return en_bundle_error(err, cap, "truncated ELF section");
    }
    if (sh.sh_offset + sh.sh_size > *end) {
      *end = sh.sh_offset + sh.sh_size;
    }
  }
  for (unsigned i = 0; i < h.e_phnum; i++) {
    Elf64_Phdr ph;
    if (pread(fd, &ph, sizeof(ph), (off_t)(h.e_phoff + (i * sizeof(ph)))) !=
        (ssize_t)sizeof(ph)) {
      return en_bundle_error(err, cap, "cannot read ELF segment");
    }
    if (ph.p_offset > *size || ph.p_filesz > *size - ph.p_offset) {
      return en_bundle_error(err, cap, "truncated ELF segment");
    }
    if (ph.p_offset + ph.p_filesz > *end) {
      *end = ph.p_offset + ph.p_filesz;
    }
  }
  return true;
}

int en_bundle_locate(en_bundle* out, char* err, size_t cap) {
  *out = (en_bundle){0};
  int fd = en_bundle_open_self(err, cap);
  if (fd < 0) {
    return -1;
  }
  uint64_t end = 0;
  uint64_t size = 0;
  bool ok = elf_extent(fd, &end, &size, err, cap);
  if (!ok || end == size) {
    close(fd);
    return ok ? 0 : -1;
  }
  uint8_t trailer[EN_BUNDLE_TRAILER_SIZE];
  if (size - end <= sizeof(trailer) ||
      pread(fd, trailer, sizeof(trailer), (off_t)(size - sizeof(trailer))) !=
          (ssize_t)sizeof(trailer)) {
    close(fd);
    (void)en_bundle_error(err, cap, "truncated bundle trailer");
    return -1;
  }
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0 || size > SIZE_MAX) {
    close(fd);
    (void)en_bundle_error(err, cap, "invalid bundle mapping size");
    return -1;
  }
  uint64_t off = end - (end % (uint64_t)page);
  size_t map_len = (size_t)(size - off);
  void* map = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, (off_t)off);
  close(fd);
  if (map == MAP_FAILED) {
    (void)en_bundle_error(err, cap, "cannot map bundle: %s", strerror(errno));
    return -1;
  }
  const uint8_t* blob = (const uint8_t*)map + (size_t)(end - off);
  ok = en_bundle_validate(blob, (size_t)(size - end), out, err, cap);
  if (!ok) {
    munmap(map, map_len);
    return -1;
  }
  out->mapping = map;
  out->mapping_len = map_len;
  return 1;
}

bool en_bundle_platform_write(int src, int dst, const uint8_t* blob, size_t len,
                              char* err, size_t cap) {
  uint64_t end = 0;
  uint64_t size = 0;
  if (!elf_extent(src, &end, &size, err, cap)) {
    return false;
  }
  if (!en_bundle_copy(src, dst, 0, end) ||
      !en_bundle_write_all(dst, blob, len)) {
    return en_bundle_error(err, cap, "cannot write bundle: %s",
                           strerror(errno));
  }
  return true;
}

/* Signature shared with the macOS signer, which writes errors. */
// NOLINTNEXTLINE(readability-non-const-parameter)
bool en_bundle_sign(const char* path, char* err, size_t cap) {
  (void)path;
  (void)err;
  (void)cap;
  return true;
}
#endif
