#include "bundle_internal.h"

#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

int en_bundle_open_self(char* err, size_t cap) {
  uint32_t size = 0;
  (void)_NSGetExecutablePath(NULL, &size);
  char* name = malloc(size);
  if (name == NULL) {
    (void)en_bundle_error(err, cap, "out of memory locating executable");
    return -1;
  }
  char resolved[PATH_MAX];
  int fd = -1;
  if (_NSGetExecutablePath(name, &size) == 0 &&
      realpath(name, resolved) != NULL) {
    fd = open(resolved, O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    (void)en_bundle_error(err, cap, "cannot open own executable: %s",
                          strerror(errno));
  }
  free(name);
  return fd;
}

int en_bundle_locate(en_bundle* out, char* err, size_t cap) {
  *out = (en_bundle){0};
  unsigned long size = 0;
  const uint8_t* bytes =
      getsectiondata(&_mh_execute_header, "__ENKI", "__payload", &size);
  if (bytes == NULL) {
    return 0;
  }
  unsigned long segment_size = 0;
  const uint8_t* segment =
      getsegmentdata(&_mh_execute_header, "__ENKI", &segment_size);
  uintptr_t offset = (uintptr_t)bytes - (uintptr_t)segment;
  if (segment == NULL || (uintptr_t)bytes < (uintptr_t)segment ||
      offset > segment_size || size > segment_size - offset) {
    (void)en_bundle_error(err, cap, "bundle section exceeds mapped segment");
    return -1;
  }
  if (size == 8 && memcmp(bytes, "ENKI-NO\n", 8) == 0) {
    return 0;
  }
  return en_bundle_validate(bytes, size, out, err, cap) ? 1 : -1;
}

static const char enki_segment[16] = "__ENKI";
static const char payload_section[16] = "__payload";
static const char linkedit_segment[16] = "__LINKEDIT";

static uint64_t page_round(uint64_t n, uint64_t page) {
  return (n + page - 1) & ~(page - 1);
}

/* Every offset-bearing command is explicit. A new linker command is an
 * actionable error, never an assumption that its offsets can be ignored. */
static bool macho_patch(struct load_command* lc, uint64_t lo, uint64_t hi,
                        int64_t delta, char* err, size_t cap) {
#define REQUIRE(type)                                                          \
  do {                                                                         \
    if (lc->cmdsize < sizeof(type)) {                                          \
      return en_bundle_error(err, cap, "short Mach-O load command 0x%x",       \
                             lc->cmd);                                         \
    }                                                                          \
  } while (0)
#define OFFSET(field, size)                                                    \
  do {                                                                         \
    uint64_t off_ = (field);                                                   \
    uint64_t len_ = (size);                                                    \
    if (len_ != 0 || off_ != 0) {                                              \
      if (off_ < lo || off_ > hi || len_ > hi - off_ ||                        \
          (int64_t)off_ + delta < 0 ||                                         \
          (uint64_t)((int64_t)off_ + delta) > UINT32_MAX) {                    \
        return en_bundle_error(                                                \
            err, cap, "invalid __LINKEDIT offset in command 0x%x", lc->cmd);   \
      }                                                                        \
      (field) = (uint32_t)((int64_t)off_ + delta);                             \
    }                                                                          \
  } while (0)
  switch (lc->cmd) {
  case LC_SYMTAB: {
    REQUIRE(struct symtab_command);
    struct symtab_command* c = (void*)lc;
    OFFSET(c->symoff, (uint64_t)c->nsyms * 16);
    OFFSET(c->stroff, c->strsize);
    break;
  }
  case LC_DYSYMTAB: {
    REQUIRE(struct dysymtab_command);
    struct dysymtab_command* c = (void*)lc;
    OFFSET(c->tocoff, (uint64_t)c->ntoc * 8);
    OFFSET(c->modtaboff, (uint64_t)c->nmodtab * 56);
    OFFSET(c->extrefsymoff, (uint64_t)c->nextrefsyms * 4);
    OFFSET(c->indirectsymoff, (uint64_t)c->nindirectsyms * 4);
    OFFSET(c->extreloff, (uint64_t)c->nextrel * 8);
    OFFSET(c->locreloff, (uint64_t)c->nlocrel * 8);
    break;
  }
  case LC_DYLD_INFO:
  case LC_DYLD_INFO_ONLY: {
    REQUIRE(struct dyld_info_command);
    struct dyld_info_command* c = (void*)lc;
    OFFSET(c->rebase_off, c->rebase_size);
    OFFSET(c->bind_off, c->bind_size);
    OFFSET(c->weak_bind_off, c->weak_bind_size);
    OFFSET(c->lazy_bind_off, c->lazy_bind_size);
    OFFSET(c->export_off, c->export_size);
    break;
  }
  case LC_FUNCTION_STARTS:
  case LC_DATA_IN_CODE:
  case LC_DYLD_CHAINED_FIXUPS:
  case LC_DYLD_EXPORTS_TRIE:
  case LC_SEGMENT_SPLIT_INFO:
  case LC_DYLIB_CODE_SIGN_DRS:
  case LC_LINKER_OPTIMIZATION_HINT: {
    REQUIRE(struct linkedit_data_command);
    struct linkedit_data_command* c = (void*)lc;
    OFFSET(c->dataoff, c->datasize);
    break;
  }
  case LC_TWOLEVEL_HINTS: {
    REQUIRE(struct twolevel_hints_command);
    struct twolevel_hints_command* c = (void*)lc;
    OFFSET(c->offset, (uint64_t)c->nhints * 4);
    break;
  }
  case LC_SEGMENT_64: {
    REQUIRE(struct segment_command_64);
    struct segment_command_64* c = (void*)lc;
    if (c->nsects > (lc->cmdsize - sizeof(*c)) / sizeof(struct section_64)) {
      return en_bundle_error(err, cap, "short Mach-O section table");
    }
    struct section_64* sections = (void*)(c + 1);
    for (uint32_t i = 0; i < c->nsects; i++) {
      OFFSET(sections[i].reloff, (uint64_t)sections[i].nreloc * 8);
    }
    break;
  }
  /* These commands have no absolute file offsets into __LINKEDIT. String
   * offsets are relative to the command; LC_MAIN points into __TEXT. */
  case LC_LOAD_DYLINKER:
  case LC_LOAD_DYLIB:
  case LC_LOAD_WEAK_DYLIB:
  case LC_REEXPORT_DYLIB:
  case LC_LOAD_UPWARD_DYLIB:
  case LC_LAZY_LOAD_DYLIB:
  case LC_RPATH:
  case LC_UUID:
  case LC_BUILD_VERSION:
  case LC_VERSION_MIN_MACOSX:
  case LC_SOURCE_VERSION:
  case LC_MAIN:
  case LC_ROUTINES_64:
    break;
  default:
    return en_bundle_error(
        err, cap,
        "unrecognized Mach-O load command 0x%x; inspect with otool -l",
        lc->cmd);
  }
#undef OFFSET
#undef REQUIRE
  return true;
}

bool en_bundle_platform_write(int src, int dst, const uint8_t* blob, size_t len,
                              char* err, size_t cap) {
  struct stat st;
  struct mach_header_64 header;
  if (fstat(src, &st) != 0 || st.st_size < (off_t)sizeof(header) ||
      pread(src, &header, sizeof(header), 0) != (ssize_t)sizeof(header)) {
    return en_bundle_error(err, cap, "cannot read Mach-O header");
  }
  if (header.magic == FAT_MAGIC || header.magic == FAT_CIGAM ||
      header.magic == FAT_MAGIC_64 || header.magic == FAT_CIGAM_64) {
    return en_bundle_error(
        err, cap,
        "fat Mach-O binaries are unsupported; use thin arm64 or x86_64");
  }
  if (header.magic != MH_MAGIC_64 || header.filetype != MH_EXECUTE ||
      (header.cputype != CPU_TYPE_ARM64 && header.cputype != CPU_TYPE_X86_64)) {
    return en_bundle_error(err, cap,
                           "expected a thin arm64 or x86_64 executable");
  }
  uint64_t size = (uint64_t)st.st_size;
  if (header.sizeofcmds > size - sizeof(header)) {
    return en_bundle_error(err, cap, "truncated Mach-O load commands");
  }
  size_t head_len = sizeof(header) + header.sizeofcmds;
  uint8_t* head = malloc(head_len);
  if (head == NULL) {
    return en_bundle_error(err, cap, "out of memory reading Mach-O");
  }
  bool ok = false;
  struct segment_command_64* enki = NULL;
  struct segment_command_64* linkedit = NULL;
  struct segment_command_64* previous = NULL;
  struct section_64* payload = NULL;
  struct linkedit_data_command* signature = NULL;
  if (pread(src, head, head_len, 0) != (ssize_t)head_len) {
    (void)en_bundle_error(err, cap, "cannot read Mach-O load commands");
    goto done;
  }
  size_t off = sizeof(header);
  for (uint32_t i = 0; i < header.ncmds; i++) {
    if (head_len - off < sizeof(struct load_command)) {
      goto malformed;
    }
    struct load_command* lc = (void*)(head + off);
    if (lc->cmdsize < sizeof(*lc) || lc->cmdsize % 8 != 0 ||
        lc->cmdsize > head_len - off) {
      goto malformed;
    }
    if (lc->cmd == LC_SEGMENT_64) {
      if (lc->cmdsize < sizeof(struct segment_command_64)) {
        goto malformed;
      }
      struct segment_command_64* seg = (void*)lc;
      if (seg->fileoff > size || seg->filesize > size - seg->fileoff) {
        goto malformed;
      }
      if (linkedit != NULL) {
        goto malformed; /* __LINKEDIT must be the last segment */
      }
      if (memcmp(seg->segname, enki_segment, 16) == 0) {
        if (enki != NULL || seg->nsects != 1 ||
            lc->cmdsize != sizeof(*seg) + sizeof(*payload)) {
          goto malformed;
        }
        enki = seg;
        payload = (void*)(seg + 1);
        if (memcmp(payload->sectname, payload_section, 16) != 0 ||
            memcmp(payload->segname, enki_segment, 16) != 0 ||
            payload->offset != seg->fileoff || payload->addr != seg->vmaddr ||
            payload->size > seg->filesize || payload->nreloc != 0) {
          goto malformed;
        }
      }
      if (memcmp(seg->segname, linkedit_segment, 16) == 0) {
        if (enki == NULL || previous != enki || seg->nsects != 0) {
          goto malformed;
        }
        linkedit = seg;
      }
      previous = seg;
    } else if (lc->cmd == LC_CODE_SIGNATURE) {
      if (signature != NULL || lc->cmdsize != sizeof(*signature)) {
        goto malformed;
      }
      signature = (void*)lc;
    }
    off += lc->cmdsize;
  }
  if (off != head_len || enki == NULL || linkedit == NULL ||
      enki->fileoff < head_len ||
      enki->fileoff + enki->filesize != linkedit->fileoff ||
      enki->vmaddr + enki->vmsize != linkedit->vmaddr ||
      linkedit->fileoff + linkedit->filesize != size) {
    goto malformed;
  }
  uint64_t old_link = linkedit->fileoff;
  uint64_t unsigned_end = size;
  if (signature != NULL) {
    if (signature->dataoff < old_link || signature->dataoff > size ||
        signature->datasize != size - signature->dataoff) {
      goto malformed;
    }
    unsigned_end = signature->dataoff;
  }
  uint64_t page = header.cputype == CPU_TYPE_ARM64 ? 16384 : 4096;
  if (len > UINT32_MAX - page || enki->fileoff % page != 0 ||
      enki->filesize % page != 0 || enki->vmsize != enki->filesize) {
    goto malformed;
  }
  uint64_t payload_size = page_round(len, page);
  int64_t delta = (int64_t)payload_size - (int64_t)enki->filesize;
  if (enki->fileoff + payload_size + unsigned_end - old_link > UINT32_MAX) {
    goto malformed;
  }
  /* Patch before compacting commands so pointers into head remain valid. */
  off = sizeof(header);
  for (uint32_t i = 0; i < header.ncmds; i++) {
    struct load_command* lc = (void*)(head + off);
    if (lc->cmd != LC_CODE_SIGNATURE &&
        !macho_patch(lc, old_link, unsigned_end, delta, err, cap)) {
      goto done;
    }
    off += lc->cmdsize;
  }
  uint64_t payload_off = enki->fileoff;
  payload->size = len;
  enki->filesize = payload_size;
  enki->vmsize = payload_size;
  linkedit->fileoff = payload_off + payload_size;
  linkedit->vmaddr = enki->vmaddr + payload_size;
  linkedit->filesize = unsigned_end - old_link;
  linkedit->vmsize = page_round(linkedit->filesize, page);
  if (signature != NULL) {
    size_t sigoff = (size_t)((uint8_t*)signature - head);
    size_t sigsize = signature->cmdsize;
    memmove(head + sigoff, head + sigoff + sigsize,
            head_len - sigoff - sigsize);
    memset(head + head_len - sigsize, 0, sigsize);
    struct mach_header_64* h = (void*)head;
    h->ncmds--;
    h->sizeofcmds -= (uint32_t)sigsize;
  }
  if (!en_bundle_write_all(dst, head, head_len) ||
      !en_bundle_copy(src, dst, head_len, payload_off - head_len) ||
      !en_bundle_write_all(dst, blob, len)) {
    goto io_error;
  }
  /* The unwritten tail is zero-filled by the following write. */
  if (lseek(dst, (off_t)(payload_off + payload_size), SEEK_SET) < 0 ||
      !en_bundle_copy(src, dst, old_link, unsigned_end - old_link)) {
    goto io_error;
  }
  ok = true;
  goto done;
malformed:
  (void)en_bundle_error(err, cap,
                        "unsupported or malformed Mach-O layout (__ENKI must "
                        "immediately precede __LINKEDIT)");
  goto done;
io_error:
  (void)en_bundle_error(err, cap, "cannot write Mach-O bundle: %s",
                        strerror(errno));
done:
  free(head);
  return ok;
}

bool en_bundle_sign(const char* path, char* err, size_t cap) {
  char* const argv[] = {"/usr/bin/codesign", "--force", "--sign", "-",
                        (char*)path,         NULL};
  pid_t pid;
  int rc = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
  if (rc != 0) {
    return en_bundle_error(err, cap, "cannot launch codesign: %s",
                           strerror(rc));
  }
  int status;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return en_bundle_error(err, cap, "ad-hoc codesign failed");
  }
  return true;
}
#endif
