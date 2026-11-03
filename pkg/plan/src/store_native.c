/* Native image format v1, layout 2 (reads layout 1). Immutable raw nodes may
 * be shared from earlier indexed extents. PLAN values and hashes are unchanged.
 * All offsets and addresses are 64-bit native endian.
 * Never mutate a sealed granule. The two superblocks are the only overwrites.
 * save_mu serializes in-process access; flock separates writer lifetime from
 * checkpoint acquisition by independent inspectors. */
#include "store_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include "axsys/ds.h"

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#define N_START  (2 * PL_NATIVE_GRANULE)
#define N_MAGIC  UINT64_C(0x31414e494b4e45) /* ENKINA1 */
#define N_ENDIAN UINT64_C(0x0102030405060708)
#if defined(__aarch64__) || defined(__arm64__)
#define N_ARCH 1u
#elif defined(__x86_64__)
#define N_ARCH 2u
#else
#define N_ARCH 0u
#endif
#define N_LAYOUT     2u
#define N_SLOT_COUNT (PL_NATIVE_BYTES / (PL_PIN_CELLS(0) * 8) + 1)
#define N_SLOT_BYTES (N_SLOT_COUNT * 8)

enum { N_OBJECT = 1, N_INDEX, N_JOURNAL, N_CACHE };
typedef struct native_record {
  uint8_t checksum[32];
  uint64_t len, type;
} native_record;
typedef struct native_index {
  native_record r;
  uint64_t bit;    /* 0..255 branch, 256 leaf */
  uint8_t key[32]; /* leaf key; zero on branches */
  uint64_t child[2];
  uint64_t value; /* object record offset or cache record offset */
} native_index;
typedef struct native_journal {
  native_record r;
  uint64_t previous;
  pl_store_root_entry entry;
  pl_val root;
} native_journal;
typedef struct native_super {
  uint8_t checksum[32];
  uint64_t magic, version, layout, arch, endian, base, capacity;
  uint64_t generation, end, objects, cache, journal, sequence, next_id;
  pl_val root;
  uint8_t root_hash[32];
} native_super;
static_assert(sizeof(native_record) == 48, "native record layout");
static_assert(sizeof(native_index) == 112, "native index layout");
static_assert(sizeof(native_super) == 184, "native superblock layout");

/* Odd references address this process-local overlay; aligned references are
 * immutable committed file offsets. No overlay reference reaches disk. */
typedef struct native_work_node {
  native_index node;
  uint64_t emitted; /* immutable staged record, reusable after a failed Save */
} native_work_node;

typedef struct native_store {
  pl_store* store;
  int fd, writer_fd, commit_fd;
  bool readonly, poisoned, reserved, legacy_slot;
  uint64_t mapped, sealed, next;
  native_super committed, work;
  native_work_node* nodes;
  /* All native operations hold save_mu (or are in construction/destruction).
   * Inspectors own separate contexts; nothing mutable is shared across stores.
   */
  EVP_MD_CTX* digest;
  unsigned slot;
  pl_cell* slots;
  pl_native_fault_fn fault;
  void* fault_ctx;
} native_store;

pl_cell* pl_native_code_slots;
static pthread_mutex_t native_open_mu = PTHREAD_MUTEX_INITIALIZER;

static bool nerror(char* err, size_t cap, const char* fmt, ...) {
  if (err != NULL && cap != 0) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, cap, fmt, ap);
    va_end(ap);
  }
  return false;
}
static uint64_t nround(uint64_t n) {
  return (n + PL_NATIVE_GRANULE - 1) & ~(PL_NATIVE_GRANULE - 1);
}
static void* nptr(uint64_t off) {
  return (void*)(uintptr_t)(PL_NATIVE_BASE + off);
}
static bool nbound(uint64_t off, uint64_t bytes, uint64_t end) {
  return off >= N_START && (off & 7) == 0 && off <= end && bytes <= end - off;
}
static bool npoint(native_store* n, const char* point, char* err, size_t cap) {
  return n->fault == NULL || n->fault(n->fault_ctx, point) ||
         nerror(err, cap, "injected native failure at %s", point);
}
void pl_native_set_fault(pl_store* s, pl_native_fault_fn fn, void* ctx) {
  native_store* n = s->be.ctx;
  n->fault = fn;
  n->fault_ctx = ctx;
}
static bool nsync(int fd) {
#ifdef __APPLE__
  int rc;
  do {
    rc = fcntl(fd, F_FULLFSYNC);
  } while (rc < 0 && errno == EINTR);
#else
  int rc;
  do {
    rc = fsync(fd);
  } while (rc < 0 && errno == EINTR);
#endif
  return rc == 0;
}
static bool nwrite(int fd, const void* src, size_t len, uint64_t off) {
  const uint8_t* b = src;
  while (len != 0) {
    ssize_t got = pwrite(fd, b, len, (off_t)off);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return false;
    b += got;
    off += (uint64_t)got;
    len -= (size_t)got;
  }
  return true;
}
static bool nread(int fd, void* dst, size_t len, uint64_t off) {
  uint8_t* b = dst;
  while (len != 0) {
    ssize_t got = pread(fd, b, len, (off_t)off);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return false;
    b += got;
    off += (uint64_t)got;
    len -= (size_t)got;
  }
  return true;
}
static bool nhash(native_store* n, const void* bytes, size_t len,
                  uint8_t out[32]) {
  unsigned size = 0;
  return EVP_DigestInit_ex(n->digest, NULL, NULL) == 1 &&
         EVP_DigestUpdate(n->digest, bytes, len) == 1 &&
         EVP_DigestFinal_ex(n->digest, out, &size) == 1 && size == 32;
}
static void ndigest(native_store* n, void* p, size_t len) {
  ax_assume(nhash(n, (uint8_t*)p + 32, len - 32, p), "native SHA-256 failed");
}
static bool nchecksum(native_store* n, const void* p, size_t len) {
  uint8_t sum[32];
  return nhash(n, (const uint8_t*)p + 32, len - 32, sum) &&
         memcmp(sum, p, 32) == 0;
}
static native_record* nheader(uint64_t off, uint64_t end, uint64_t type) {
  if (!nbound(off, sizeof(native_record), end))
    return NULL;
  native_record* r = nptr(off);
  if (r->type != type || r->len < sizeof(*r) || (r->len & 7) != 0 ||
      !nbound(off, r->len, end) ||
      (type == N_INDEX && r->len != sizeof(native_index)) ||
      (type == N_JOURNAL && r->len != sizeof(native_journal)) ||
      (type == N_CACHE && r->len != sizeof(*r) + 32))
    return NULL;
  return r;
}
static native_record* nrecord(native_store* n, uint64_t off, uint64_t end,
                              uint64_t type) {
  native_record* r = nheader(off, end, type);
  return r != NULL && nchecksum(n, r, (size_t)r->len) ? r : NULL;
}
static native_index* nnode(native_store* n, uint64_t off, uint64_t end) {
  native_record* r = nrecord(n, off, end, N_INDEX);
  if (r == NULL || r->len != sizeof(native_index))
    return NULL;
  native_index* p = (native_index*)r;
  if (p->bit > 256 || (p->bit < 256 && (!nbound(p->child[0], sizeof(*p), end) ||
                                        !nbound(p->child[1], sizeof(*p), end))))
    return NULL;
  return p;
}
static unsigned nbit(const uint8_t key[32], uint64_t bit) {
  return (key[bit / 8] >> (7 - bit % 8)) & 1u;
}
static native_index* nworking_node(native_store* n, uint64_t ref) {
  if ((ref & 1) == 0)
    return nnode(n, ref, n->committed.end);
  uint64_t slot = ref >> 1;
  if (slot >= (uint64_t)ax_arrlen(n->nodes))
    return NULL;
  /* These nodes were created by this writer, under save_mu, and have no
   * serialized checksum yet. Disk nodes always take the validating path. */
  return &n->nodes[slot].node;
}
static bool nlookup_inner(native_store* n, uint64_t root, const uint8_t key[32],
                          uint64_t end, bool working, uint64_t* value,
                          char* err, size_t cap) {
  *value = 0;
  uint64_t prior = 0;
  bool first = true;
  for (unsigned depth = 0; root != 0 && depth <= 256; depth++) {
    native_index* p = working ? nworking_node(n, root) : nnode(n, root, end);
    if (p == NULL || p->bit > 256 || (!first && p->bit <= prior))
      return nerror(err, cap, "invalid native hash index");
    if (p->bit == 256) {
      if (memcmp(p->key, key, 32) == 0)
        *value = p->value;
      return true;
    }
    first = false;
    prior = p->bit;
    root = p->child[nbit(key, p->bit)];
    if (root == 0)
      return nerror(err, cap, "empty native index branch");
  }
  return root == 0 || nerror(err, cap, "cyclic native index");
}
static bool nlookup(native_store* n, uint64_t root, const uint8_t key[32],
                    uint64_t end, uint64_t* value, char* err, size_t cap) {
  return nlookup_inner(n, root, key, end, false, value, err, cap);
}
static bool nworking_lookup(native_store* n, uint64_t root,
                            const uint8_t key[32], uint64_t* value, char* err,
                            size_t cap) {
  return nlookup_inner(n, root, key, n->next, true, value, err, cap);
}

static bool nreserve(native_store* n, char* err, size_t cap) {
  if (__atomic_load_n(&pl_native_code_slots, __ATOMIC_ACQUIRE) != NULL)
    return nerror(err, cap, "only one native store may be open per process");
#ifdef __APPLE__
  mach_vm_address_t addr = PL_NATIVE_BASE;
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, PL_NATIVE_BYTES,
                                      VM_FLAGS_FIXED);
  if (kr != KERN_SUCCESS)
    return nerror(err, cap,
                  "native address window 0x%llx is unavailable (Mach %d); "
                  "close conflicting mappings or use a fresh process",
                  (unsigned long long)PL_NATIVE_BASE, kr);
  if (mprotect((void*)(uintptr_t)addr, PL_NATIVE_BYTES, PROT_NONE) != 0) {
    (void)munmap((void*)(uintptr_t)addr, PL_NATIVE_BYTES);
    return nerror(err, cap, "cannot protect native reservation");
  }
#else
  /* A hint is non-destructive even on kernels lacking NOREPLACE. Accept only
   * the exact address, then MAP_FIXED only inside our owned reservation. */
  void* p = mmap(nptr(0), PL_NATIVE_BYTES, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED || p != nptr(0)) {
    if (p != MAP_FAILED)
      (void)munmap(p, PL_NATIVE_BYTES);
    return nerror(err, cap,
                  "native address window 0x%llx is unavailable; close "
                  "conflicting mappings or use a fresh process",
                  (unsigned long long)PL_NATIVE_BASE);
  }
#endif
  n->reserved = true;
  /* Reserve address space for all possible PINs, but back only slots actually
   * touched. Linux must not charge the entire array against its commit limit.
   */
  n->slots = mmap(NULL, N_SLOT_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (n->slots == MAP_FAILED) {
    n->slots = NULL;
    return nerror(err, cap, "cannot reserve native code slots: %s",
                  strerror(errno));
  }
  return true;
}
static bool nmap(native_store* n, uint64_t end, bool writable, char* err,
                 size_t cap) {
  end = nround(end);
  if (end > PL_NATIVE_BYTES)
    return nerror(err, cap, "native arena capacity exhausted");
  if (end <= n->mapped)
    return true;
  int prot = PROT_READ | (writable ? PROT_WRITE : 0);
  uint64_t start = n->mapped;
  void* p = mmap(nptr(start), (size_t)(end - start), prot,
                 MAP_SHARED | MAP_FIXED, n->fd, (off_t)start);
  if (p == MAP_FAILED)
    return nerror(err, cap, "cannot map native extent: %s", strerror(errno));
  n->mapped = end;
  return true;
}
static bool nwritable(native_store* n, char* err, size_t cap) {
  if (n->readonly)
    return nerror(err, cap, "native store is read-only");
  if (n->poisoned)
    return nerror(err, cap,
                  "native publication failed; close and reopen the store");
  return true;
}
static native_record* nalloc(native_store* n, uint64_t type, uint64_t len,
                             uint64_t* off, char* err, size_t cap) {
  if (!nwritable(n, err, cap))
    return NULL;
  if (len > PL_NATIVE_BYTES || len < sizeof(native_record)) {
    nerror(err, cap, "native extent exceeds arena capacity");
    return NULL;
  }
  len = (len + 7) & ~UINT64_C(7);
  uint64_t at = n->next;
  if (len >= PL_NATIVE_GRANULE ||
      at % PL_NATIVE_GRANULE + len > PL_NATIVE_GRANULE)
    at = nround(at);
  if (at > PL_NATIVE_BYTES || len > PL_NATIVE_BYTES - at) {
    nerror(err, cap, "native arena capacity exhausted");
    return NULL;
  }
  uint64_t end = nround(at + len);
  if (end > n->mapped) {
    if (!npoint(n, "grow", err, cap))
      return NULL;
    if (ftruncate(n->fd, (off_t)end) != 0) {
      nerror(err, cap, "cannot extend native arena: %s", strerror(errno));
      return NULL;
    }
    if (!nmap(n, end, true, err, cap))
      return NULL;
  }
  native_record* r = nptr(at);
  memset(r, 0, (size_t)len);
  r->len = len;
  r->type = type;
  n->next = at + len;
  *off = at;
  return r;
}
static uint64_t nworking_new(native_store* n, const native_index* src) {
  native_work_node node = {.node = *src};
  ax_arrpush(n->nodes, node);
  return ((uint64_t)ax_arrlen(n->nodes) - 1) * 2 + 1;
}
static bool ninsert(native_store* n, uint64_t* root, const uint8_t key[32],
                    uint64_t value, char* err, size_t cap) {
  /* Record one descent. Validate before mutating the overlay, so a corrupt
   * committed path cannot leave a partially changed working index. */
  uint64_t path[256];
  native_index ancestors[256];
  unsigned directions[256];
  size_t count = 0;
  uint64_t cur = *root;
  native_index leaf = {.bit = 256, .value = value};
  memcpy(leaf.key, key, 32);
  if (cur == 0) {
    *root = nworking_new(n, &leaf);
    return true;
  }
  native_index* p;
  for (;;) {
    p = nworking_node(n, cur);
    if (p == NULL || p->bit > 256 ||
        (count != 0 && p->bit <= ancestors[count - 1].bit))
      return nerror(err, cap, "invalid native index while inserting");
    if (p->bit == 256)
      break;
    if (count >= 256)
      return nerror(err, cap, "invalid native index path");
    path[count] = cur;
    ancestors[count] = *p;
    directions[count] = nbit(key, p->bit);
    cur = p->child[directions[count++]];
  }
  uint64_t split = 0;
  while (split < 256 && nbit(key, split) == nbit(p->key, split))
    split++;
  if (split == 256 && p->value == value)
    return true;
  while (count != 0 && ancestors[count - 1].bit >= split)
    cur = path[--count];
  uint64_t changed;
  if (split == 256 && (cur & 1)) {
    n->nodes[cur >> 1] = (native_work_node){.node = leaf};
    changed = cur;
  } else {
    changed = nworking_new(n, &leaf);
  }
  if (split != 256) {
    native_index branch = {.bit = split};
    unsigned dir = nbit(key, split);
    branch.child[dir] = changed;
    branch.child[1 - dir] = cur;
    changed = nworking_new(n, &branch);
  }
  while (count != 0) {
    count--;
    native_index branch = ancestors[count];
    branch.child[directions[count]] = changed;
    if (path[count] & 1) {
      n->nodes[path[count] >> 1] = (native_work_node){.node = branch};
      changed = path[count];
    } else {
      changed = nworking_new(n, &branch);
    }
  }
  *root = changed;
  return true;
}
/* Serialize only the final overlay, in dependency order, retaining immutable
 * committed subtrees. Emitted nodes are never rewritten: after a failed Save
 * a changed path loses its memoized offset, while unchanged nodes are reused.
 * The recursion is bounded by the 256-bit Patricia key. */
static bool nemit(native_store* n, uint64_t ref, uint64_t minimum,
                  uint64_t* out, char* err, size_t cap) {
  if ((ref & 1) == 0) {
    if (ref != 0 && !nbound(ref, sizeof(native_index), n->committed.end))
      return nerror(err, cap, "invalid committed native index reference");
    *out = ref;
    return true;
  }
  native_index* src = nworking_node(n, ref);
  if (src == NULL || src->bit < minimum || src->bit > 256)
    return nerror(err, cap, "invalid working native index");
  native_work_node* node = &n->nodes[ref >> 1];
  if (node->emitted != 0) {
    *out = node->emitted;
    return true;
  }
  native_index record = *src;
  if (record.bit < 256) {
    if (record.child[0] == 0 || record.child[1] == 0)
      return nerror(err, cap, "missing working native index child");
    if (!nemit(n, record.child[0], record.bit + 1, &record.child[0], err,
               cap) ||
        !nemit(n, record.child[1], record.bit + 1, &record.child[1], err, cap))
      return false;
  }
  uint64_t off;
  native_index* dst =
      (native_index*)nalloc(n, N_INDEX, sizeof(*dst), &off, err, cap);
  if (dst == NULL)
    return false;
  *dst = record;
  dst->r.len = sizeof(*dst);
  dst->r.type = N_INDEX;
  ndigest(n, dst, sizeof(*dst));
  node->emitted = *out = off;
  return npoint(n, "index", err, cap);
}

bool pl_native_seal(pl_store* s, char* err, size_t cap) {
  native_store* n = s->be.ctx;
  if (!nwritable(n, err, cap))
    return false;
  uint64_t end = nround(n->next);
  /* Even a failed protection call may have protected part of the range.
   * Retry from a fresh granule, never write back into that range. */
  n->next = end;
  if (end > n->sealed &&
      mprotect(nptr(n->sealed), (size_t)(end - n->sealed), PROT_READ) != 0)
    return nerror(err, cap, "cannot seal native pages");
  n->sealed = n->next = end;
  return true;
}

/* Store new nodes in one extent; keep earlier native raw nodes and PINs.
 * The map stores byte offsets only for nodes not yet in the native arena. */
typedef struct native_copy {
  pl_val key;
  uint64_t value;
} native_copy;
static pl_val ntranslate(native_copy* map, pl_val v, uint64_t off) {
  if (pl_is_nat63(v))
    return v;
  if (pl_tag(v) == PL_TAG_PIN) {
    pl_cell* p = pl_pin_resolved(pl_ptr(v));
    return pl_make(PL_TAG_PIN, p);
  }
  /* Earlier native nodes keep their address and PLAN identity. */
  if (pl_addr(v) >= PL_NATIVE_BASE + N_START &&
      pl_addr(v) < PL_NATIVE_BASE + off)
    return v;
  ptrdiff_t at = ax_hmgeti(map, v);
  ax_assume(at >= 0, "native body translation missing node");
  return pl_make(pl_tag(v), nptr(off + map[at].value));
}
bool pl_native_stage(pl_store* s, const uint8_t hash[32], pl_val body,
                     const pl_val* subpins, uint32_t nsub, pl_val* out,
                     char* err, size_t cap) {
  native_store* n = s->be.ctx;
  if (!nwritable(n, err, cap))
    return false;
  uint64_t found;
  if (!nworking_lookup(n, n->work.objects, hash, &found, err, cap))
    return false;
  if (found != 0)
    return pl_native_load(s, hash, out, err, cap);
  if (nsub > PL_HDR_META_MAX || n->work.next_id >= N_SLOT_COUNT)
    return nerror(err, cap, "native PIN table/identifier capacity exhausted");
  for (uint32_t i = 0; i < nsub; i++)
    if (pl_tag(subpins[i]) != PL_TAG_PIN ||
        pl_addr(subpins[i]) < s->native_lo ||
        pl_addr(subpins[i]) >= s->native_hi)
      return nerror(err, cap, "native PIN dependency is not native");
  native_copy* map = NULL;
  pl_val* todo = NULL;
  uint64_t size = sizeof(native_record) + (uint64_t)PL_PIN_CELLS(nsub) * 8;
  ax_arrpush(todo, body);
  bool ok = false;
  while (ax_arrlen(todo) != 0) {
    pl_val v = stbds_arrpop(todo);
    if (pl_is_nat63(v))
      continue;
    if (pl_tag(v) == PL_TAG_PIN) {
      uintptr_t p = (uintptr_t)pl_pin_resolved(pl_ptr(v)) & PL_ADDR_MASK;
      if (p < s->native_lo || p >= s->native_hi) {
        nerror(err, cap, "native body references a transient PIN");
        goto done;
      }
      continue;
    }
    if (pl_addr(v) >= PL_NATIVE_BASE + N_START &&
        pl_addr(v) < PL_NATIVE_BASE + n->next)
      continue;
    if (ax_hmgeti(map, v) >= 0)
      continue;
    pl_cell* p = pl_ptr(v);
    uint64_t bytes = (uint64_t)pl_hdr_cells(p[0]) * 8;
    if (bytes < 8 || size > PL_NATIVE_BYTES - bytes) {
      nerror(err, cap, "native body exceeds arena capacity");
      goto done;
    }
    ax_hmput(map, v, size);
    size += bytes;
    switch (pl_tag(v)) {
    case PL_TAG_NAT:
      break;
    case PL_TAG_LAW:
      ax_arrpush(todo, p[2]);
      ax_arrpush(todo, p[3]);
      break;
    case PL_TAG_APP:
      for (uint32_t i = 1; i < pl_hdr_cells(p[0]); i++)
        ax_arrpush(todo, p[i]);
      break;
    default:
      nerror(err, cap, "native body is not normal");
      goto done;
    }
  }
  uint64_t off;
  native_record* r = nalloc(n, N_OBJECT, size, &off, err, cap);
  if (r == NULL)
    goto done;
  pl_cell* pin = (pl_cell*)(r + 1);
  pin[0] = pl_hdr_make(PL_K_PIN, PL_F_NORMAL | PL_F_PIN_HASHED, nsub,
                       PL_PIN_CELLS(nsub));
  memcpy(pin + 1, hash, 32);
  pin[5] = ntranslate(map, body, off);
  pin[6] = n->work.next_id++;
  if (nsub != 0)
    memcpy(pin + 7, subpins, (size_t)nsub * 8);
  for (ptrdiff_t i = 0; i < ax_hmlen(map); i++) {
    pl_cell* src = pl_ptr(map[i].key);
    pl_cell* dst = nptr(off + map[i].value);
    uint32_t cells = pl_hdr_cells(src[0]);
    memcpy(dst, src, (size_t)cells * 8);
    dst[0] = pl_hdr_set_flag(dst[0], PL_F_NORMAL);
    unsigned start = pl_tag(map[i].key) == PL_TAG_APP ? 1 : 2;
    if (pl_tag(map[i].key) != PL_TAG_NAT)
      for (uint32_t j = start; j < cells; j++)
        dst[j] = ntranslate(map, src[j], off);
  }
  ndigest(n, r, (size_t)r->len);
  if (!npoint(n, "object", err, cap) ||
      !ninsert(n, &n->work.objects, hash, off, err, cap))
    goto done;
  *out = pl_make(PL_TAG_PIN, pin);
  ok = true;
done:
  ax_hmfree(map);
  ax_arrfree(todo);
  return ok;
}

static pl_cell* npin(uint64_t off, uint64_t end, uint64_t ids) {
  /* Opening/loading faults in headers, not whole body extents. Object
   * checksums and all graph edges are checked by the explicit full scrub. */
  native_record* r = nheader(off, end, N_OBJECT);
  if (r == NULL || r->len < sizeof(*r) + PL_PIN_CELLS(0) * 8)
    return NULL;
  pl_cell* p = (pl_cell*)(r + 1);
  uint32_t count = pl_hdr_meta(p[0]);
  if (pl_hdr_kind(p[0]) != PL_K_PIN ||
      pl_hdr_flags(p[0]) != (PL_F_NORMAL | PL_F_PIN_HASHED) ||
      pl_hdr_cells(p[0]) != PL_PIN_CELLS(count) ||
      (uint64_t)PL_PIN_CELLS(count) * 8 > r->len - sizeof(*r) || p[6] == 0 ||
      p[6] >= ids || ids > N_SLOT_COUNT)
    return NULL;
  pl_val body = p[5];
  if (!pl_is_nat63(body)) {
    uint64_t addr = pl_addr(body);
    if ((addr & 7) != 0)
      return NULL;
    if (pl_tag(body) == PL_TAG_PIN) {
      if (addr < PL_NATIVE_BASE + N_START + sizeof(*r) ||
          addr >= PL_NATIVE_BASE + off)
        return NULL;
    } else {
      if (pl_tag(body) != PL_TAG_NAT && pl_tag(body) != PL_TAG_APP &&
          pl_tag(body) != PL_TAG_LAW)
        return NULL;
      if (addr < PL_NATIVE_BASE + N_START + sizeof(*r) ||
          addr > PL_NATIVE_BASE + off + r->len - 8)
        return NULL;
    }
  }
  return p;
}
static bool nsb_valid(native_store* n, native_super* sb, uint64_t file_size) {
  if (!nchecksum(n, sb, sizeof(*sb)) || sb->magic != N_MAGIC ||
      sb->version != 1 || (sb->layout != 1 && sb->layout != N_LAYOUT) ||
      sb->arch != N_ARCH || sb->endian != N_ENDIAN ||
      sb->base != PL_NATIVE_BASE || sb->capacity != PL_NATIVE_BYTES ||
      sb->end < N_START || sb->end > file_size || sb->end > PL_NATIVE_BYTES ||
      sb->end % PL_NATIVE_GRANULE != 0 || sb->next_id == 0 ||
      sb->next_id > N_SLOT_COUNT)
    return false;
  if (!nmap(n, sb->end, false, NULL, 0))
    return false;
  if (sb->objects != 0 && nnode(n, sb->objects, sb->end) == NULL)
    return false;
  if (sb->cache != 0 && nnode(n, sb->cache, sb->end) == NULL)
    return false;
  if (sb->root != 0) {
    if (pl_tag(sb->root) != PL_TAG_PIN ||
        pl_addr(sb->root) < PL_NATIVE_BASE + N_START + sizeof(native_record))
      return false;
    uint64_t off = pl_addr(sb->root) - PL_NATIVE_BASE - sizeof(native_record);
    pl_cell* p = npin(off, sb->end, sb->next_id);
    uint64_t indexed;
    if (p == NULL || memcmp(p + 1, sb->root_hash, 32) != 0 ||
        !nlookup(n, sb->objects, sb->root_hash, sb->end, &indexed, NULL, 0) ||
        indexed != off)
      return false;
  }
  if (sb->journal != 0) {
    native_record* r = nrecord(n, sb->journal, sb->end, N_JOURNAL);
    if (r == NULL || r->len != sizeof(native_journal))
      return false;
    native_journal* j = (native_journal*)r;
    if (j->entry.seq != sb->sequence || j->root != sb->root ||
        memcmp(j->entry.hash, sb->root_hash, 32) != 0 ||
        j->entry.pack_bytes > sb->end ||
        j->entry.pack_bytes < sb->journal + sizeof(*j) || sb->sequence == 0)
      return false;
  } else if (sb->sequence != 0 || sb->root != 0)
    return false;
  return true;
}
static bool nrefresh(native_store* n, char* err, size_t cap) {
  if (!n->readonly)
    return true;
  if (flock(n->commit_fd, LOCK_SH) != 0)
    return nerror(err, cap, "cannot lock native checkpoint");
  native_super a = {0}, b = {0};
  struct stat st;
  bool ok = fstat(n->fd, &st) == 0;
  bool va = ok && nread(n->fd, &a, sizeof(a), 0) &&
            nsb_valid(n, &a, (uint64_t)st.st_size);
  bool vb = ok && nread(n->fd, &b, sizeof(b), PL_NATIVE_GRANULE) &&
            nsb_valid(n, &b, (uint64_t)st.st_size);
  (void)flock(n->commit_fd, LOCK_UN);
  if (!va && !vb)
    return nerror(err, cap,
                  "no valid native checkpoint (corrupt or incompatible image)");
  unsigned slot = vb && (!va || b.generation > a.generation) ? 1u : 0u;
  native_super chosen = slot ? b : a;
  /* Existing cursors remain valid because committed records are immutable. */
  if (chosen.generation >= n->committed.generation) {
    n->slot = slot;
    n->committed = n->work = chosen;
    n->next = n->sealed = chosen.end;
  }
  return true;
}
bool pl_native_load(pl_store* s, const uint8_t hash[32], pl_val* out, char* err,
                    size_t cap) {
  native_store* n = s->be.ctx;
  if (!nrefresh(n, err, cap))
    return false;
  uint64_t off;
  if (!nworking_lookup(n, n->work.objects, hash, &off, err, cap))
    return false;
  if (off == 0)
    return nerror(err, cap, "missing native pin");
  pl_cell* p = npin(off, n->next, n->work.next_id);
  if (p == NULL || memcmp(p + 1, hash, 32) != 0)
    return nerror(err, cap, "invalid native PIN record");
  *out = pl_make(PL_TAG_PIN, p);
  return true;
}

bool pl_native_checkpoint(pl_store* s, const uint8_t hash[32], char* err,
                          size_t cap) {
  native_store* n = s->be.ctx;
  if (!nwritable(n, err, cap))
    return false;
  pl_val root;
  if (!pl_native_load(s, hash, &root, err, cap))
    return false;
  bool changed =
      n->committed.root == 0 || memcmp(hash, n->committed.root_hash, 32) != 0;
  if (!changed && n->next == n->committed.end &&
      n->work.objects == n->committed.objects &&
      n->work.cache == n->committed.cache && !n->legacy_slot)
    return true;
  native_super candidate = n->work;
  if (candidate.generation == UINT64_MAX)
    return nerror(err, cap, "native generation exhausted");
  if (!nemit(n, n->work.objects, 0, &candidate.objects, err, cap) ||
      !nemit(n, n->work.cache, 0, &candidate.cache, err, cap))
    return false;
  candidate.generation++;
  candidate.layout = N_LAYOUT;
  candidate.root = root;
  memcpy(candidate.root_hash, hash, 32);
  if (changed) {
    if (candidate.sequence == UINT64_MAX)
      return nerror(err, cap, "native history exhausted");
    uint64_t off;
    native_journal* j =
        (native_journal*)nalloc(n, N_JOURNAL, sizeof(*j), &off, err, cap);
    if (j == NULL)
      return false;
    j->previous = n->committed.journal;
    j->entry.seq = ++candidate.sequence;
    memcpy(j->entry.hash, hash, 32);
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0)
      j->entry.unix_ns =
          (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    j->entry.pack_bytes = nround(n->next);
    j->root = root;
    ndigest(n, j, sizeof(*j));
    candidate.journal = off;
  }
  if (!pl_native_seal(s, err, cap))
    return false;
  candidate.end = n->next;
  ndigest(n, &candidate, sizeof(candidate));
  if (!npoint(n, "before_data_sync", err, cap) ||
      msync(nptr(n->committed.end), (size_t)(candidate.end - n->committed.end),
            MS_SYNC) != 0 ||
      !nsync(n->fd))
    return nerror(err, cap, "cannot synchronize native data");
  if (!npoint(n, "after_data_sync", err, cap))
    return false;
  if (flock(n->commit_fd, LOCK_EX) != 0)
    return nerror(err, cap, "cannot lock native publication");
  unsigned slot = 1u - n->slot;
  bool ok = npoint(n, "before_superblock", err, cap);
  if (ok) {
    /* From this point onward failure has an ambiguous durable outcome. */
    n->poisoned = true;
    ok = nwrite(n->fd, &candidate, sizeof(candidate),
                slot * PL_NATIVE_GRANULE) &&
         npoint(n, "after_superblock", err, cap) && nsync(n->fd) &&
         npoint(n, "after_superblock_sync", err, cap);
    /* A layout-1 reader must not silently select the older superblock after
     * an acknowledged upgrade. Publish the same durable root in both slots. */
    if (ok && n->legacy_slot)
      ok = nwrite(n->fd, &candidate, sizeof(candidate),
                  n->slot * PL_NATIVE_GRANULE) &&
           nsync(n->fd);
    if (ok) {
      n->slot = slot;
      n->legacy_slot = false;
      n->committed = n->work = candidate;
      ax_arrfree(n->nodes);
      n->nodes = NULL;
      n->poisoned = false;
    }
  }
  (void)flock(n->commit_fd, LOCK_UN);
  return ok ||
         nerror(err, cap,
                n->poisoned ? "ambiguous native publication; reopen required"
                            : "native publication failed");
}

static bool nget(void* ctx, const uint8_t key[32], uint8_t** out, size_t* len) {
  native_store* n = ctx;
  if (!nrefresh(n, NULL, 0))
    return false;
  uint64_t off;
  if (!nworking_lookup(n, n->work.cache, key, &off, NULL, 0) || off == 0)
    return false;
  native_record* r = nrecord(n, off, n->next, N_CACHE);
  if (r == NULL || r->len != sizeof(*r) + 32)
    return false;
  *out = malloc(32);
  if (*out == NULL)
    return false;
  memcpy(*out, r + 1, 32);
  *len = 32;
  return true;
}
static bool nput(void* ctx, const uint8_t key[32], const uint8_t* bytes,
                 size_t len) {
  native_store* n = ctx;
  if (len != 32 || !nwritable(n, NULL, 0))
    return false;
  uint64_t target;
  if (!nworking_lookup(n, n->work.objects, bytes, &target, NULL, 0) ||
      target == 0)
    return false;
  uint64_t off;
  native_record* r = nalloc(n, N_CACHE, sizeof(*r) + 32, &off, NULL, 0);
  if (r == NULL)
    return false;
  memcpy(r + 1, bytes, 32);
  ndigest(n, r, (size_t)r->len);
  return ninsert(n, &n->work.cache, key, off, NULL, 0);
}
static bool nhas(void* ctx, const uint8_t key[32]) {
  native_store* n = ctx;
  uint64_t off;
  return nrefresh(n, NULL, 0) &&
         nworking_lookup(n, n->work.objects, key, &off, NULL, 0) && off != 0;
}
static bool nput_root(void* ctx, const uint8_t hash[32]) {
  native_store* n = ctx;
  return pl_native_checkpoint(n->store, hash, NULL, 0);
}
static bool nget_root(void* ctx, uint8_t hash[32]) {
  native_store* n = ctx;
  if (!nrefresh(n, NULL, 0) || n->committed.root == 0)
    return false;
  memcpy(hash, n->committed.root_hash, 32);
  return true;
}
static void nclose(void* ctx) {
  native_store* n = ctx;
  pthread_mutex_lock(&native_open_mu);
  if (n->slots != NULL) {
    if (__atomic_load_n(&pl_native_code_slots, __ATOMIC_ACQUIRE) == n->slots)
      __atomic_store_n(&pl_native_code_slots, NULL, __ATOMIC_RELEASE);
    (void)munmap(n->slots, N_SLOT_BYTES);
  }
  if (n->reserved)
    (void)munmap(nptr(0), PL_NATIVE_BYTES);
  if (n->fd >= 0)
    (void)close(n->fd);
  if (n->commit_fd >= 0)
    (void)close(n->commit_fd);
  if (n->writer_fd >= 0)
    (void)close(n->writer_fd);
  pthread_mutex_unlock(&native_open_mu);
  ax_arrfree(n->nodes);
  EVP_MD_CTX_free(n->digest);
  free(n);
}
static pl_store* nopen(const char* path, bool readonly, char* err, size_t cap) {
  if (err != NULL && cap)
    err[0] = 0;
  native_store* n = calloc(1, sizeof(*n));
  if (n == NULL) {
    nerror(err, cap, "out of memory");
    return NULL;
  }
  n->fd = n->writer_fd = n->commit_fd = -1;
  long page = sysconf(_SC_PAGESIZE);
  if (N_ARCH == 0 || (page != 4096 && page != 16384)) {
    free(n);
    nerror(err, cap,
           "native arena requires a supported 64-bit architecture and 4 KiB or "
           "16 KiB pages");
    return NULL;
  }
  n->digest = EVP_MD_CTX_new();
  if (n->digest == NULL ||
      EVP_DigestInit_ex(n->digest, EVP_sha256(), NULL) != 1) {
    nerror(err, cap, "cannot initialize native SHA-256 context");
    nclose(n);
    return NULL;
  }
  n->readonly = readonly;
  pthread_mutex_lock(&native_open_mu);
  bool reserved = nreserve(n, err, cap);
  /* Claim the process slot before dropping the open mutex. */
  if (reserved)
    __atomic_store_n(&pl_native_code_slots, n->slots, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&native_open_mu);
  if (!reserved) {
    nclose(n);
    return NULL;
  }
  int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) {
    nerror(err, cap, "cannot open native store directory: %s", strerror(errno));
    goto fail;
  }
  struct stat st;
  if (fstatat(dir, "arena.dat", &st, 0) != 0) {
    if (errno != ENOENT) {
      nerror(err, cap, "cannot inspect arena.dat");
      goto fail_dir;
    }
    const char* old[] = {"pins.pack", "data.mdb", "root.plan"};
    for (size_t i = 0; i < sizeof(old) / sizeof(*old); i++) {
      if (fstatat(dir, old[i], &st, 0) == 0) {
        nerror(err, cap,
               "existing snapshot format; use a fresh directory (migration is "
               "not supported)");
        goto fail_dir;
      }
    }
  }
  if (!readonly) {
    n->writer_fd =
        openat(dir, "arena.writer.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (n->writer_fd < 0 || flock(n->writer_fd, LOCK_EX | LOCK_NB) != 0) {
      nerror(err, cap,
             "native store already has a writer or cannot acquire writer lock");
      goto fail_dir;
    }
  }
  n->commit_fd = openat(
      dir, "arena.commit.lock",
      readonly ? O_RDONLY | O_CLOEXEC : O_RDWR | O_CREAT | O_CLOEXEC, 0664);
  if (n->commit_fd < 0 ||
      flock(n->commit_fd, readonly ? LOCK_SH : LOCK_EX) != 0) {
    nerror(err, cap, "cannot acquire native commit lock");
    goto fail_dir;
  }
  bool created = false;
  if (!readonly) {
    n->fd =
        openat(dir, "arena.dat", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
    created = n->fd >= 0;
    if (n->fd < 0 && errno == EEXIST)
      n->fd = openat(dir, "arena.dat", O_RDWR | O_CLOEXEC);
  } else
    n->fd = openat(dir, "arena.dat", O_RDONLY | O_CLOEXEC);
  if (n->fd < 0 || fstat(n->fd, &st) != 0) {
    nerror(err, cap, "cannot open native arena: %s", strerror(errno));
    goto fail_dir;
  }
  if (created) {
    n->committed = (native_super){.magic = N_MAGIC,
                                  .version = 1,
                                  .layout = N_LAYOUT,
                                  .arch = N_ARCH,
                                  .endian = N_ENDIAN,
                                  .base = PL_NATIVE_BASE,
                                  .capacity = PL_NATIVE_BYTES,
                                  .generation = 1,
                                  .end = N_START,
                                  .next_id = 1};
    ndigest(n, &n->committed, sizeof(n->committed));
    if (ftruncate(n->fd, N_START) != 0 ||
        !nwrite(n->fd, &n->committed, sizeof(n->committed), 0) ||
        !nsync(n->fd)) {
      nerror(err, cap, "cannot initialize native arena");
      goto fail_dir;
    }
  } else {
    native_super a = {0}, b = {0};
    bool va = nread(n->fd, &a, sizeof(a), 0) &&
              nsb_valid(n, &a, (uint64_t)st.st_size);
    bool vb = nread(n->fd, &b, sizeof(b), PL_NATIVE_GRANULE) &&
              nsb_valid(n, &b, (uint64_t)st.st_size);
    if (!va && !vb) {
      nerror(err, cap,
             "no valid native checkpoint (corrupt or incompatible "
             "layout/address profile)");
      goto fail_dir;
    }
    n->slot = vb && (!va || b.generation > a.generation) ? 1u : 0u;
    n->committed = n->slot ? b : a;
    n->legacy_slot = (va && a.layout == 1) || (vb && b.layout == 1);
  }
  n->work = n->committed;
  n->next = n->sealed = n->committed.end;
  /* A failed newer checkpoint may have extended our read mapping. Keep the
   * reservation but remove those mappings before reusing abandoned staging. */
  if (n->mapped > n->next) {
    void* p = mmap(nptr(n->next), (size_t)(n->mapped - n->next), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
      nerror(err, cap, "cannot discard abandoned mappings");
      goto fail_dir;
    }
    n->mapped = n->next;
  }
  if (!nmap(n, n->next, false, err, cap))
    goto fail_dir;
  if (!readonly && ftruncate(n->fd, (off_t)n->next) != 0) {
    nerror(err, cap, "cannot discard abandoned native staging");
    goto fail_dir;
  }
  /* Also do this on reopen: a previous creation attempt may have written a
   * valid empty superblock but failed its directory durability barrier. */
  if (!readonly && fsync(dir) != 0) {
    nerror(err, cap, "cannot synchronize native store directory: %s",
           strerror(errno));
    goto fail_dir;
  }
  (void)flock(n->commit_fd, LOCK_UN);
  (void)close(dir);
  pl_store* s = pl_store_new((pl_store_backend){.ctx = n,
                                                .get = nget,
                                                .put = nput,
                                                .has = nhas,
                                                .get_root = nget_root,
                                                .put_root = nput_root,
                                                .close = nclose});
  n->store = s;
  s->format = PL_STORE_FORMAT_NATIVE_V1;
  s->native_lo = PL_NATIVE_BASE + N_START;
  s->native_hi = PL_NATIVE_BASE + PL_NATIVE_BYTES;
  return s;
fail_dir:
  (void)close(dir);
fail:
  nclose(n);
  return NULL;
}
pl_store* pl_store_new_native(const char* path, char* err, size_t cap) {
  return nopen(path, false, err, cap);
}
pl_store* pl_store_new_native_ro(const char* path, char* err, size_t cap) {
  return nopen(path, true, err, cap);
}

static bool nwalk_inner(native_store* n, uint64_t off, uint64_t end,
                        uint64_t minimum,
                        bool (*visit)(native_store*, const native_index*, void*,
                                      char*, size_t),
                        void* ctx, native_copy** seen, char* err, size_t cap) {
  if (off == 0)
    return true;
  if (ax_hmgeti(*seen, off) >= 0)
    return nerror(err, cap, "shared or cyclic native index node");
  ax_hmput(*seen, off, 1);
  native_index* p = nnode(n, off, end);
  if (p == NULL || p->bit < minimum)
    return nerror(err, cap, "invalid native index tree");
  if (p->bit == 256)
    return visit == NULL || visit(n, p, ctx, err, cap);
  if (p->child[0] == 0 || p->child[1] == 0)
    return nerror(err, cap, "missing native index child");
  return nwalk_inner(n, p->child[0], end, p->bit + 1, visit, ctx, seen, err,
                     cap) &&
         nwalk_inner(n, p->child[1], end, p->bit + 1, visit, ctx, seen, err,
                     cap);
}
static bool nwalk(native_store* n, uint64_t off, uint64_t end, uint64_t minimum,
                  bool (*visit)(native_store*, const native_index*, void*,
                                char*, size_t),
                  void* ctx, char* err, size_t cap) {
  native_copy* seen = NULL;
  bool ok = nwalk_inner(n, off, end, minimum, visit, ctx, &seen, err, cap);
  ax_hmfree(seen);
  return ok;
}
uint64_t pl_native_log_head(pl_store* s) {
  pl_store_save_lock(s);
  native_store* n = s->be.ctx;
  uint64_t out = nrefresh(n, NULL, 0) ? n->committed.sequence : 0;
  pl_store_save_unlock(s);
  return out;
}
size_t pl_native_log(pl_store* s, uint64_t from, pl_store_root_entry* out,
                     size_t cap) {
  if (out == NULL || cap == 0)
    return 0;
  pl_store_save_lock(s);
  native_store* n = s->be.ctx;
  size_t count = 0;
  if (!nrefresh(n, NULL, 0))
    goto done;
  if (from == 0)
    from = 1;
  uint64_t off = n->committed.journal;
  uint64_t seq = n->committed.sequence;
  while (off != 0 && seq >= from) {
    native_record* r = nrecord(n, off, n->committed.end, N_JOURNAL);
    if (r == NULL || r->len != sizeof(native_journal)) {
      count = 0;
      break;
    }
    native_journal* j = (native_journal*)r;
    if (j->entry.seq != seq || (j->previous != 0 && j->previous >= off)) {
      count = 0;
      break;
    }
    if (seq - from < cap) {
      out[seq - from] = j->entry;
      count++;
    }
    off = j->previous;
    seq--;
  }
done:
  pl_store_save_unlock(s);
  return count;
}
typedef struct native_visit {
  pl_store_silo_object_fn fn;
  void* ctx;
  size_t count;
} native_visit;
static bool nvisit_object(native_store* n, const native_index* leaf, void* ctx,
                          char* err, size_t cap) {
  native_visit* v = ctx;
  pl_cell* p = npin(leaf->value, n->committed.end, n->committed.next_id);
  if (p == NULL || memcmp(p + 1, leaf->key, 32) != 0)
    return nerror(err, cap, "invalid native object index leaf");
  native_record* r = nptr(leaf->value);
  v->fn(v->ctx, leaf->key, leaf->value, r->len);
  v->count++;
  return true;
}
size_t pl_store_objects(pl_store* s, pl_store_silo_object_fn fn, void* ctx) {
  if (s == NULL || fn == NULL)
    return 0;
  if (s->format != PL_STORE_FORMAT_NATIVE_V1)
    return pl_store_silo_objects(s, fn, ctx);
  pl_store_save_lock(s);
  native_store* n = s->be.ctx;
  native_visit v = {.fn = fn, .ctx = ctx};
  bool ok =
      nrefresh(n, NULL, 0) && nwalk(n, n->committed.objects, n->committed.end,
                                    0, nvisit_object, &v, NULL, 0);
  pl_store_save_unlock(s);
  return ok ? v.count : 0;
}
bool pl_store_object_info(pl_store* s, const uint8_t hash[32], uint64_t* len,
                          pl_hash** subs, size_t* count, char* err,
                          size_t cap) {
  if (s == NULL || len == NULL || subs == NULL || count == NULL)
    return nerror(err, cap, "invalid object query");
  if (s->format != PL_STORE_FORMAT_NATIVE_V1)
    return pl_store_silo_object_info(s, hash, len, subs, count, err, cap);
  *subs = NULL;
  *count = 0;
  *len = 0;
  pl_store_save_lock(s);
  pl_val pin;
  bool ok = pl_native_load(s, hash, &pin, err, cap);
  if (ok) {
    pl_cell* p = pl_ptr(pin);
    native_record* r = (native_record*)((uint8_t*)p - sizeof(native_record));
    *len = r->len;
    *count = pl_pin_npins(p);
    if (*count != 0) {
      *subs = malloc(*count * sizeof(pl_hash));
      if (*subs == NULL)
        ok = nerror(err, cap, "out of memory reading native dependencies");
      else
        for (size_t i = 0; i < *count; i++)
          memcpy((*subs)[i].b, pl_pin_hash(pl_pin_subpins(p)[i]), 32);
    }
  }
  pl_store_save_unlock(s);
  return ok;
}

/* Validate node boundaries before dereferencing any edge. Native files are
 * local ABI images, not an input wire format. Full scrub is explicit so
 * opening a terabyte graph never faults in every page. */
typedef struct native_boundary {
  pl_val key;
  uint64_t value;
} native_boundary;
/* Full scrub indexes object ranges, not every historical node. Cross-extent
 * edges must name an exact normal-node boundary in an earlier indexed object.
 * Memoize only the boundaries actually referenced across extents. */
typedef struct native_extent {
  uint64_t off, first, end;
} native_extent;
typedef struct native_verify {
  native_extent* extents;
  native_boundary* shared;
} native_verify;
static bool nverify_extent(native_store* n, const native_index* leaf, void* ctx,
                           char* err, size_t cap) {
  native_verify* v = ctx;
  pl_cell* pin = npin(leaf->value, n->committed.end, n->committed.next_id);
  if (pin == NULL)
    return nerror(err, cap, "invalid indexed native extent");
  native_record* r = nptr(leaf->value);
  native_extent extent = {leaf->value,
                          leaf->value + sizeof(*r) +
                              (uint64_t)pl_hdr_cells(pin[0]) * 8,
                          leaf->value + r->len};
  ax_arrpush(v->extents, extent);
  return true;
}
static int nextent_order(const void* a, const void* b) {
  uint64_t x = ((const native_extent*)a)->off;
  uint64_t y = ((const native_extent*)b)->off;
  return (x > y) - (x < y);
}
static bool nshared_edge(native_verify* v, pl_val value, uint64_t owner) {
  uint64_t addr = pl_addr(value);
  if (addr < PL_NATIVE_BASE + N_START || addr >= PL_NATIVE_BASE + owner ||
      (addr & 7) != 0)
    return false;
  uint64_t target = addr - PL_NATIVE_BASE;
  ptrdiff_t cached = ax_hmgeti(v->shared, value);
  if (cached >= 0)
    return v->shared[cached].value < owner;
  size_t lo = 0, hi = (size_t)ax_arrlen(v->extents);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (v->extents[mid].off <= target)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0)
    return false;
  native_extent* e = &v->extents[lo - 1];
  if (e->off >= owner || target < e->first || target >= e->end)
    return false;
  uint64_t at = e->first;
  while (at <= target) {
    pl_cell* p = nptr(at);
    uint32_t cells = pl_hdr_cells(p[0]);
    pl_kind kind = pl_hdr_kind(p[0]);
    if (cells == 0 || (uint64_t)cells * 8 > e->end - at ||
        pl_hdr_flags(p[0]) != PL_F_NORMAL ||
        (kind != PL_K_NAT && kind != PL_K_LAW && kind != PL_K_APP))
      return false;
    if (at == target) {
      if (pl_tag(value) != pl_tag_for_kind(kind))
        return false;
      ax_hmput(v->shared, value, e->off);
      return true;
    }
    at += (uint64_t)cells * 8;
  }
  return false;
}
static bool nedge(native_store* n, pl_val v, uint64_t owner,
                  native_boundary* nodes, native_verify* verify) {
  if (pl_is_nat63(v))
    return true;
  if (pl_tag(v) != PL_TAG_PIN)
    return ax_hmgeti(nodes, v) >= 0 || nshared_edge(verify, v, owner);
  uintptr_t addr = pl_addr(v);
  if (addr < PL_NATIVE_BASE + N_START + sizeof(native_record))
    return false;
  uint64_t off = addr - PL_NATIVE_BASE - sizeof(native_record);
  /* Dependency order forbids inter-PIN cycles. */
  pl_cell* pin =
      off < owner ? npin(off, n->committed.end, n->committed.next_id) : NULL;
  uint64_t indexed;
  return pin != NULL &&
         nlookup(n, n->committed.objects, (uint8_t*)(pin + 1), n->committed.end,
                 &indexed, NULL, 0) &&
         indexed == off;
}
static bool nverify_object(native_store* n, const native_index* leaf, void* ctx,
                           char* err, size_t cap) {
  native_verify* verify = ctx;
  uint64_t indexed;
  if (!nlookup(n, n->committed.objects, leaf->key, n->committed.end, &indexed,
               err, cap) ||
      indexed != leaf->value)
    return nerror(err, cap, "native index key is on the wrong branch");
  pl_cell* pin = npin(leaf->value, n->committed.end, n->committed.next_id);
  if (pin == NULL || memcmp(pin + 1, leaf->key, 32) != 0)
    return nerror(err, cap, "invalid native PIN extent");
  native_record* r = nptr(leaf->value);
  native_boundary* nodes = NULL;
  uint64_t at = sizeof(*r) + (uint64_t)pl_hdr_cells(pin[0]) * 8;
  bool ok = false;
  while (at < r->len) {
    pl_cell* p = nptr(leaf->value + at);
    uint32_t cells = pl_hdr_cells(p[0]);
    pl_kind kind = pl_hdr_kind(p[0]);
    if (cells == 0 || (uint64_t)cells * 8 > r->len - at ||
        pl_hdr_flags(p[0]) != PL_F_NORMAL)
      goto bad;
    if ((kind == PL_K_NAT &&
         (cells < 2 || pl_hdr_meta(p[0]) != cells - 1 || p[cells - 1] == 0 ||
          (cells == 2 && p[1] <= PL_NAT63_MAX))) ||
        (kind == PL_K_LAW &&
         (cells != PL_LAW_CELLS || pl_hdr_meta(p[0]) != 0)) ||
        (kind == PL_K_APP && cells < 3) ||
        (kind != PL_K_NAT && kind != PL_K_LAW && kind != PL_K_APP))
      goto bad;
    ax_hmput(nodes, pl_make(pl_tag_for_kind(kind), p), 0);
    at += (uint64_t)cells * 8;
  }
  if (!nedge(n, pin[5], leaf->value, nodes, verify))
    goto bad;
  for (uint32_t i = 0; i < pl_pin_npins(pin); i++)
    if (pl_tag(pin[7 + i]) != PL_TAG_PIN ||
        !nedge(n, pin[7 + i], leaf->value, nodes, verify))
      goto bad;
  for (ptrdiff_t i = 0; i < ax_hmlen(nodes); i++) {
    pl_cell* p = pl_ptr(nodes[i].key);
    if (pl_tag(nodes[i].key) == PL_TAG_NAT)
      continue;
    unsigned start = pl_tag(nodes[i].key) == PL_TAG_LAW ? 2 : 1;
    for (uint32_t j = start; j < pl_hdr_cells(p[0]); j++)
      if (!nedge(n, p[j], leaf->value, nodes, verify))
        goto bad;
  }
  /* Iterative DFS rejects local cycles before the canonical encoder runs. */
  typedef struct frame {
    pl_val v;
    uint32_t field;
  } frame;
  frame* stack = NULL;
  for (ptrdiff_t i = 0; i < ax_hmlen(nodes); i++) {
    if (nodes[i].value != 0)
      continue;
    frame f = {.v = nodes[i].key, .field = 0};
    ax_arrpush(stack, f);
    while (ax_arrlen(stack)) {
      frame* top = &stbds_arrlast(stack);
      ptrdiff_t pos = ax_hmgeti(nodes, top->v);
      pl_cell* p = pl_ptr(top->v);
      if (top->field == 0) {
        nodes[pos].value = 1;
        top->field = pl_tag(top->v) == PL_TAG_LAW ? 2 : 1;
      }
      if (pl_tag(top->v) == PL_TAG_NAT || top->field >= pl_hdr_cells(p[0])) {
        nodes[pos].value = 2;
        (void)stbds_arrpop(stack);
        continue;
      }
      pl_val child = p[top->field++];
      if (pl_is_nat63(child) || pl_tag(child) == PL_TAG_PIN)
        continue;
      ptrdiff_t child_at = ax_hmgeti(nodes, child);
      /* Cross-extent edges are strictly backward, hence cannot make cycles. */
      if (child_at < 0)
        continue;
      if (nodes[child_at].value == 1) {
        ax_arrfree(stack);
        goto bad;
      }
      if (nodes[child_at].value == 0) {
        frame next = {.v = child};
        ax_arrpush(stack, next);
      }
    }
  }
  ax_arrfree(stack);
  /* Arity caches affect evaluation but are deliberately absent from the
   * canonical hash. Validate them independently of content identity. */
  for (ptrdiff_t i = 0; i < ax_hmlen(nodes); i++) {
    if (pl_tag(nodes[i].key) != PL_TAG_APP)
      continue;
    pl_cell* app = pl_ptr(nodes[i].key);
    pl_val head = app[1];
    uint64_t arity = 0;
    if (pl_tag(head) == PL_TAG_PIN) {
      pl_cell* dep = pl_ptr(head);
      pl_val body = dep[5];
      arity = 1;
      if (pl_tag(body) == PL_TAG_LAW) {
        uint64_t off = pl_addr(head) - PL_NATIVE_BASE - sizeof(native_record);
        native_record* record = nptr(off);
        uint64_t addr = pl_addr(body);
        bool local =
            addr >= pl_addr(head) + (uint64_t)pl_hdr_cells(dep[0]) * 8 &&
            addr <= PL_NATIVE_BASE + off + record->len - PL_LAW_CELLS * 8 &&
            (addr & 7) == 0;
        if (!local && !nshared_edge(verify, body, off))
          goto bad;
        pl_cell* law = pl_ptr(body);
        if (pl_hdr_kind(law[0]) != PL_K_LAW ||
            pl_hdr_cells(law[0]) != PL_LAW_CELLS)
          goto bad;
        arity = law[1];
      }
    } else if (pl_tag(head) == PL_TAG_LAW)
      arity = pl_law_arity(pl_ptr(head));
    else if (pl_tag(head) == PL_TAG_APP)
      arity = pl_app_need(pl_ptr(head));
    uint64_t args = pl_hdr_cells(app[0]) - 2;
    uint64_t need = arity <= args ? 0 : arity - args;
    if (need != pl_hdr_meta(app[0]))
      goto bad;
  }
  ok = true;
  goto done;
bad:
  nerror(err, cap, "invalid native object graph at offset %" PRIu64,
         leaf->value);
done:
  ax_hmfree(nodes);
  return ok;
}
/* Validate every edge in the image before hashing through borrowed raw
 * subgraphs. Index traversal is by hash, not by dependency order. */
static bool nverify_hash(native_store* n, const native_index* leaf, void* ctx,
                         char* err, size_t cap) {
  (void)ctx;
  pl_cell* pin = npin(leaf->value, n->committed.end, n->committed.next_id);
  uint8_t hash[32];
  return (pin != NULL &&
          pl_silo_hash(pin[5], pl_pin_subpins(pin), pl_pin_npins(pin), hash,
                       err, cap) &&
          memcmp(hash, leaf->key, 32) == 0) ||
         nerror(err, cap, "invalid native object hash at offset %" PRIu64,
                leaf->value);
}
static bool nverify_cache(native_store* n, const native_index* leaf, void* ctx,
                          char* err, size_t cap) {
  (void)ctx;
  native_record* r = nrecord(n, leaf->value, n->committed.end, N_CACHE);
  uint64_t off, indexed;
  return (r != NULL && r->len == sizeof(*r) + 32 &&
          nlookup(n, n->committed.cache, leaf->key, n->committed.end, &indexed,
                  err, cap) &&
          indexed == leaf->value &&
          nlookup(n, n->committed.objects, (uint8_t*)(r + 1), n->committed.end,
                  &off, err, cap) &&
          off != 0) ||
         nerror(err, cap, "invalid native compiler-cache reference");
}
/* Scrub every committed record, including obsolete COW nodes and records
 * left by a retry. Padding is zero; no scan ever crosses committed.end. */
static bool nscrub_records(native_store* n, char* err, size_t cap) {
  native_copy* ids = NULL;
  bool ok = true;
  uint64_t at = N_START;
  while (at < n->committed.end) {
    uint64_t boundary = nround(at + 1);
    if (*(uint64_t*)nptr(at) == 0) {
      /* SHA-256 may start with zero, so only an entirely zero tail is padding.
       */
      bool padding = true;
      for (uint64_t p = at; p < boundary; p += 8)
        if (*(uint64_t*)nptr(p) != 0) {
          padding = false;
          break;
        }
      if (padding) {
        at = boundary;
        continue;
      }
    }
    if (!nbound(at, sizeof(native_record), n->committed.end)) {
      ok = false;
      break;
    }
    native_record* raw = nptr(at);
    native_record* r = nrecord(n, at, n->committed.end, raw->type);
    if (r == NULL || r->type < N_OBJECT || r->type > N_CACHE ||
        (r->len >= PL_NATIVE_GRANULE && at % PL_NATIVE_GRANULE != 0) ||
        (r->len < PL_NATIVE_GRANULE && r->len > boundary - at)) {
      ok = false;
      break;
    }
    if (r->type == N_OBJECT) {
      pl_cell* pin = npin(at, n->committed.end, n->committed.next_id);
      if (pin == NULL || ax_hmgeti(ids, pin[6]) >= 0) {
        ok = false;
        break;
      }
      ax_hmput(ids, pin[6], 1);
    } else if ((r->type == N_INDEX && r->len != sizeof(native_index)) ||
               (r->type == N_JOURNAL && r->len != sizeof(native_journal)) ||
               (r->type == N_CACHE && r->len != sizeof(*r) + 32)) {
      ok = false;
      break;
    }
    at += r->len;
  }
  ax_hmfree(ids);
  return ok || nerror(err, cap, "invalid native record at offset %" PRIu64, at);
}
bool pl_store_verify(pl_store* s, char* err, size_t cap) {
  if (s == NULL || s->format != PL_STORE_FORMAT_NATIVE_V1)
    return nerror(err, cap, "verification requires a native store");
  pl_store_save_lock(s);
  native_store* n = s->be.ctx;
  native_verify verify = {0};
  bool ok = nrefresh(n, err, cap) && nscrub_records(n, err, cap) &&
            nwalk(n, n->committed.objects, n->committed.end, 0, nverify_extent,
                  &verify, err, cap);
  if (ok)
    qsort(verify.extents, (size_t)ax_arrlen(verify.extents),
          sizeof(native_extent), nextent_order);
  ok = ok &&
       nwalk(n, n->committed.objects, n->committed.end, 0, nverify_object,
             &verify, err, cap) &&
       nwalk(n, n->committed.objects, n->committed.end, 0, nverify_hash, NULL,
             err, cap) &&
       nwalk(n, n->committed.cache, n->committed.end, 0, nverify_cache, NULL,
             err, cap);
  uint64_t off = n->committed.journal, seq = n->committed.sequence;
  uint64_t later_end = n->committed.end;
  while (ok && off != 0) {
    native_record* r = nrecord(n, off, n->committed.end, N_JOURNAL);
    if (r == NULL || r->len != sizeof(native_journal)) {
      ok = false;
      break;
    }
    native_journal* j = (native_journal*)r;
    uint64_t object;
    ok = j->entry.seq == seq && seq != 0 && j->entry.pack_bytes <= later_end &&
         j->entry.pack_bytes >= off + sizeof(*j) &&
         (j->previous == 0 || j->previous < off) &&
         nlookup(n, n->committed.objects, j->entry.hash, n->committed.end,
                 &object, err, cap) &&
         object != 0 && j->entry.pack_bytes % PL_NATIVE_GRANULE == 0 &&
         npin(object, j->entry.pack_bytes, n->committed.next_id) != NULL &&
         j->root == pl_make(PL_TAG_PIN, nptr(object + sizeof(native_record)));
    later_end = j->entry.pack_bytes;
    off = j->previous;
    seq--;
  }
  ok = ok && seq == 0;
  ax_arrfree(verify.extents);
  ax_hmfree(verify.shared);
  pl_store_save_unlock(s);
  return ok || nerror(err, cap, "native verification failed");
}
static bool nusage_leaf(native_store* n, const native_index* leaf, void* ctx,
                        char* err, size_t cap) {
  pl_store_usage* out = ctx;
  native_record* r = nheader(leaf->value, n->committed.end, N_OBJECT);
  if (r == NULL)
    return nerror(err, cap, "invalid native object while accounting");
  out->object_bytes += r->len;
  return true;
}
static bool nindex_size(native_store* n, uint64_t off, uint64_t end,
                        uint64_t minimum, uint64_t* size) {
  native_copy* seen = NULL;
  bool ok = nwalk_inner(n, off, end, minimum, NULL, NULL, &seen, NULL, 0);
  if (ok)
    *size += (uint64_t)ax_hmlen(seen) * sizeof(native_index);
  ax_hmfree(seen);
  return ok;
}
bool pl_store_get_usage(pl_store* s, pl_store_usage* out, char* err,
                        size_t cap) {
  if (s == NULL || s->format != PL_STORE_FORMAT_NATIVE_V1 || out == NULL)
    return nerror(err, cap,
                  "native storage accounting requires a native store");
  memset(out, 0, sizeof(*out));
  pl_store_save_lock(s);
  native_store* n = s->be.ctx;
  bool ok = nrefresh(n, err, cap) &&
            nwalk(n, n->committed.objects, n->committed.end, 0, nusage_leaf,
                  out, err, cap) &&
            nindex_size(n, n->committed.objects, n->committed.end, 0,
                        &out->index_bytes) &&
            nindex_size(n, n->committed.cache, n->committed.end, 0,
                        &out->index_bytes);
  if (ok) {
    out->committed_bytes = n->committed.end;
    out->journal_bytes = n->committed.sequence * sizeof(native_journal);
    uint64_t used = out->object_bytes + out->index_bytes + out->journal_bytes;
    ok = used <= out->committed_bytes;
    if (ok)
      out->overhead_bytes = out->committed_bytes - used;
  }
  pl_store_save_unlock(s);
  return ok || nerror(err, cap, "invalid native storage accounting");
}
