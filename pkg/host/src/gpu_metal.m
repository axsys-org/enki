#include "host/gpu_metal.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "axsys/sha256.h"
#include "plan/eval.h"
#include "plan/hostcall.h"
#include "plan/nat.h"
#include "plan/value.h"

/*
 * Direct-Metal host thunks (NoApiMetalBackend discipline, no_api-informed).
 *
 * Sessions own the device, queue, arena heap, shared event, and every
 * object they materialize; closing the session releases all of them.
 * Carrier records arrive as under-applied pinned laws (foil/shrine/
 * no-api-carriers.plan): the record kind is the law name, the fields
 * are the app args.  Native never interprets payload bytes — the root
 * payload is assembled by the instantiate-root law and bound blindly.
 *
 * Pointer complements are the allocation authority: gpu.alloc returns
 * (alloc, offset, length, generation) witnesses; gpu.free bumps the
 * slot generation, and any use of an outdated complement refuses with
 * 950 — the ACL linear no-contraction rule, not defensive coding.
 *
 * Sync model: gpu.command-graph signals the session's MTLSharedEvent
 * and returns the signal value in its witness; gpu.readback waits that
 * event value.  waitUntilCompleted appears nowhere.  Declared hazard
 * facts (Stage, Stage, Hazard) lower to pass splits between draw steps
 * — the TBDR-native barrier (Metal serializes draws to the attachment
 * within one pass; intra-pass memory barriers are restricted on Apple
 * GPUs); absent facts mean no split, never a conservative wait.
 *
 * Descriptors are bytes at index x stride: an argument buffer of
 * MTLResourceIDs at index x 8 (no_api's vkGetDescriptorEXT mapping).
 * Sampling state is constexpr in the artifact — no API sampler objects
 * (no_api recreates and leaks VkSamplers per write; we don't copy that).
 */

#define GM_SESSIONS 4
#define GM_OBJECTS 64
#define GM_ALLOCS 64
#define GM_DHEAPS 8
#define GM_DHEAP_TEXTURES 16
#define GM_MAX_EXTENT 4096u
#define GM_CACHE_SLOTS 16

enum {
  GM_OBJ_NONE = 0,
  GM_OBJ_LIBRARY,
  GM_OBJ_PIPELINE,
  GM_OBJ_PIPELINE_COMPUTE,
  GM_OBJ_TEXTURE,
};

/* Command-step payload kinds (carrier convention).  5 (draw-indexed)
 * and 6 (draw-indexed-indirect) are reserved for Slice 3 Gap 13/15. */
enum {
  GM_STEP_DRAW = 1,
  GM_STEP_CLEAR = 2,
  GM_STEP_DISPATCH = 3,
  GM_STEP_DISPATCH_INDIRECT = 4,
  GM_STEP_COPY = 7,              /* {src-ptr dst-ptr 0 0} buffer→buffer */
  GM_STEP_COPY_TO_TEXTURE = 8,   /* {src-ptr texture w h} */
  GM_STEP_COPY_FROM_TEXTURE = 9, /* {texture dst-ptr w h} */
};

/* Refusal kinds, continuing the no-api backend's 9xx space. */
enum {
  GM_REFUSAL_BAD_SESSION = 941,
  GM_REFUSAL_BAD_HANDLE = 942,
  GM_REFUSAL_NO_DEVICE = 943,
  GM_REFUSAL_LIBRARY = 944,
  GM_REFUSAL_PIPELINE = 945,
  GM_REFUSAL_ALLOC = 946,
  GM_REFUSAL_ENCODE = 947,
  GM_REFUSAL_BOUNDS = 948,
  GM_REFUSAL_CAPACITY = 949,
  GM_REFUSAL_STALE_GENERATION = 950,
  GM_REFUSAL_CARRIER_SHAPE = 951,
  GM_REFUSAL_GRAPH_SHAPE = 952,
};

typedef struct gm_alloc {
  const void* buffer_ref; /* retained MTLBuffer; NULL = freed slot */
  uint32_t generation;    /* bumps on free; 0 = slot never used */
  size_t length;
} gm_alloc;

typedef struct gm_dheap {
  const void* buffer_ref; /* retained MTLBuffer of MTLResourceIDs */
  uint32_t count;
  uint32_t generation;
  uint64_t textures[GM_DHEAP_TEXTURES]; /* written handles, for residency */
  uint32_t texture_count;
} gm_dheap;

typedef struct gm_session {
  bool used;
  const void* device_ref; /* CFBridgingRetain'd id<MTLDevice> */
  const void* queue_ref;  /* CFBridgingRetain'd id<MTLCommandQueue> */
  const void* arena_ref;  /* CFBridgingRetain'd id<MTLHeap>; NULL until
                             gpu.arena */
  const void* event_ref;  /* CFBridgingRetain'd id<MTLSharedEvent> */
  uint64_t event_value;   /* last signaled submission */
  size_t arena_capacity;
  gm_alloc allocs[GM_ALLOCS];
  uint32_t alloc_count;
  gm_dheap dheaps[GM_DHEAPS];
  uint32_t dheap_count;
  const void* object_refs[GM_OBJECTS];
  uint8_t object_kinds[GM_OBJECTS];
  uint8_t object_modes[GM_OBJECTS]; /* pipelines: root lowering mode */
  uint32_t object_count;
} gm_session;

/* Root lowering modes (root-layout carrier, field 3). */
enum {
  GM_ROOT_BUFFER = 1, /* root bound as a vertex/compute buffer */
  GM_ROOT_INLINE = 2, /* root bytes inlined via setBytes (<= 4 KiB) */
};

static gm_session gm_sessions[GM_SESSIONS];

/* Pin-keyed library/pipeline cache (Gap 6): keyed by sha-256 of the
 * artifact payload bytes — the same bytes the artifact pin commits to.
 * Hit counts return as cache evidence in the witness. */
typedef struct gm_cache_entry {
  uint8_t key[32];
  const void* object_ref; /* retained library or pipeline */
  uint32_t hits;
  bool used;
} gm_cache_entry;

static gm_cache_entry gm_library_cache[GM_CACHE_SLOTS];
static gm_cache_entry gm_pipeline_cache[GM_CACHE_SLOTS];
static gm_cache_entry gm_compute_cache[GM_CACHE_SLOTS];

#define ARG(i) (t->vstack[ab + (i)])

/* ── Records, handles, payload bytes ───────────────────────────────────── */

static bool gm_nat_is(pl_val v, const char* s) {
  if (!pl_is_nat(v))
    return false;
  size_t n = strlen(s);
  if (pl_nat_byte_len(v) != n)
    return false;
  for (size_t i = 0; i < n; i++) {
    if (pl_nat_byte_at(v, i) != (uint8_t)s[i])
      return false;
  }
  return true;
}

/* Resolve a carrier record: an APP of a (pinned) law named kind_c with
 * exactly nfields supplied fields.  The caller must have normalized v
 * (pl_nf) so fields are values, not thunks.  Returns the field array or
 * NULL on shape mismatch. */
static pl_val* gm_record(pl_val v, const char* kind_c, uint32_t nfields) {
  pl_cell* p = pl_as(PL_TAG_APP, v);
  if (p == NULL || pl_app_n(p) != nfields)
    return NULL;
  pl_val head = pl_app_head(p);
  pl_cell* lp = NULL;
  if (!pl_is_nat63(head)) {
    if (pl_tag(head) == PL_TAG_PIN) {
      pl_val body = pl_pin_body(pl_ptr(head));
      if (!pl_is_nat63(body) && pl_tag(body) == PL_TAG_LAW)
        lp = pl_ptr(body);
    } else if (pl_tag(head) == PL_TAG_LAW) {
      lp = pl_ptr(head);
    }
  }
  if (lp == NULL || !gm_nat_is(pl_law_name(lp), kind_c))
    return NULL;
  return pl_app_args(p);
}

static gm_session* gm_session_at(pl_val v) {
  if (!pl_is_nat(v))
    return NULL;
  uint64_t h = pl_nat_u64_clamp(v);
  if (h == 0 || h > GM_SESSIONS)
    return NULL;
  gm_session* s = &gm_sessions[h - 1];
  return s->used ? s : NULL;
}

static id gm_object(gm_session* s, uint64_t h, uint8_t kind) {
  if (s == NULL || h == 0 || h > s->object_count ||
      s->object_kinds[h - 1] != kind)
    return nil;
  return (__bridge id)s->object_refs[h - 1];
}

static id gm_object_v(gm_session* s, pl_val v, uint8_t kind) {
  if (!pl_is_nat(v))
    return nil;
  return gm_object(s, pl_nat_u64_clamp(v), kind);
}

static uint64_t gm_intern(gm_session* s, id obj, uint8_t kind) {
  if (s->object_count >= GM_OBJECTS)
    return 0;
  s->object_refs[s->object_count] = CFBridgingRetain(obj);
  s->object_kinds[s->object_count] = kind;
  s->object_modes[s->object_count] = GM_ROOT_BUFFER;
  s->object_count++;
  return s->object_count;
}

static uint8_t* gm_bar_bytes(pl_val v, size_t* out_n);

/* Pipeline jets accept either a bare artifact bar or a pipeline-request
 * record (artifact, root-layout).  Resolves the artifact bytes and the
 * declared root layout; NULL = carrier shape refusal. */
static uint8_t* gm_pipeline_source(pl_thread* t, size_t ab, size_t* out_n,
                                   uint64_t* out_stride, uint64_t* out_mode) {
  ARG(1) = pl_nf(t, ARG(1));
  pl_val src = ARG(1);
  *out_stride = 0;
  *out_mode = GM_ROOT_BUFFER;
  pl_val* req = gm_record(src, "pipeline-request", 2);
  if (req != NULL) {
    pl_val* art = gm_record(req[0], "artifact", 2);
    pl_val* layout = gm_record(req[1], "root-layout", 4);
    if (art == NULL || layout == NULL)
      return NULL;
    *out_stride = pl_nat_u64_clamp(pl_nat_coerce(layout[0]));
    *out_mode = pl_nat_u64_clamp(pl_nat_coerce(layout[3]));
    if (*out_mode != GM_ROOT_BUFFER && *out_mode != GM_ROOT_INLINE)
      return NULL;
    src = art[0];
  } else if (!pl_is_nat(src)) {
    return NULL;
  }
  return gm_bar_bytes(pl_nat_coerce(src), out_n);
}

/* Pipeline cache key: sha of (artifact content address || layout).
 * One artifact under two root layouts is two pipelines — the CB10
 * second relation, keyed and witnessed separately from the library. */
static void gm_pipeline_key(const uint8_t lib_key[32], uint64_t stride,
                            uint64_t mode, uint8_t out_key[32]) {
  uint8_t seed[48];
  memcpy(seed, lib_key, 32);
  memcpy(seed + 32, &stride, 8);
  memcpy(seed + 40, &mode, 8);
  ax_sha256(seed, sizeof(seed), out_key);
}

static gm_dheap* gm_dheap_at(gm_session* s, pl_val v) {
  if (!pl_is_nat(v))
    return NULL;
  uint64_t h = pl_nat_u64_clamp(v);
  if (h == 0 || h > s->dheap_count)
    return NULL;
  return &s->dheaps[h - 1];
}

/* Resolve a heap-pointer record (alloc offset length byte-tag
 * generation) against the session's allocation table.  Validates
 * liveness, generation, and window bounds.  On success returns the
 * buffer and writes the window offset; on failure returns nil with
 * *out_refusal set. */
static id<MTLBuffer> gm_pointer(gm_session* s, pl_val* f, size_t* out_offset,
                                size_t* out_length, uint64_t* out_refusal) {
  uint64_t idx = pl_nat_u64_clamp(pl_nat_coerce(f[0]));
  uint64_t offset = pl_nat_u64_clamp(pl_nat_coerce(f[1]));
  uint64_t length = pl_nat_u64_clamp(pl_nat_coerce(f[2]));
  uint64_t generation = pl_nat_u64_clamp(pl_nat_coerce(f[4]));
  if (idx == 0 || idx > s->alloc_count) {
    *out_refusal = GM_REFUSAL_BAD_HANDLE;
    return nil;
  }
  gm_alloc* a = &s->allocs[idx - 1];
  if (a->buffer_ref == NULL || generation != a->generation) {
    *out_refusal = GM_REFUSAL_STALE_GENERATION;
    return nil;
  }
  if (length == 0 || offset + length > a->length) {
    *out_refusal = GM_REFUSAL_BOUNDS;
    return nil;
  }
  *out_offset = (size_t)offset;
  *out_length = (size_t)length;
  return (__bridge id<MTLBuffer>)a->buffer_ref;
}

/* Bar payload bytes (op-82 convention): nat bytes minus the top byte. */
static uint8_t* gm_bar_bytes(pl_val v, size_t* out_n) {
  size_t n = pl_nat_byte_len(v);
  if (n > 0)
    n -= 1;
  uint8_t* b = malloc(n ? n : 1);
  if (b == NULL)
    return NULL;
  for (size_t i = 0; i < n; i++)
    b[i] = pl_nat_byte_at(v, i);
  *out_n = n;
  return b;
}

static gm_cache_entry* gm_cache_find(gm_cache_entry* cache,
                                     const uint8_t key[32]) {
  for (int i = 0; i < GM_CACHE_SLOTS; i++) {
    if (cache[i].used && memcmp(cache[i].key, key, 32) == 0)
      return &cache[i];
  }
  return NULL;
}

static gm_cache_entry* gm_cache_insert(gm_cache_entry* cache,
                                       const uint8_t key[32], id obj) {
  for (int i = 0; i < GM_CACHE_SLOTS; i++) {
    if (!cache[i].used) {
      memcpy(cache[i].key, key, 32);
      cache[i].object_ref = CFBridgingRetain(obj);
      cache[i].hits = 0;
      cache[i].used = true;
      return &cache[i];
    }
  }
  return NULL; /* cache full: caller proceeds uncached */
}

/* ── Session lifecycle ─────────────────────────────────────────────────── */

static pl_val gm_session_open(pl_thread* t, size_t ab) {
  (void)ab; /* ARG(0): profile row, reserved for target selection */
  @autoreleasepool {
    int slot = -1;
    for (int i = 0; i < GM_SESSIONS; i++) {
      if (!gm_sessions[i].used) {
        slot = i;
        break;
      }
    }
    if (slot < 0)
      return pl_hostcall_refusal(t, "gpu.session-open", GM_REFUSAL_CAPACITY,
                                 "session table full");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil)
      return pl_hostcall_refusal(t, "gpu.session-open", GM_REFUSAL_NO_DEVICE,
                                 "no metal device");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLSharedEvent> event = [device newSharedEvent];
    if (queue == nil || event == nil)
      return pl_hostcall_refusal(t, "gpu.session-open", GM_REFUSAL_ALLOC,
                                 "command queue/event");
    gm_session* s = &gm_sessions[slot];
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->device_ref = CFBridgingRetain(device);
    s->queue_ref = CFBridgingRetain(queue);
    s->event_ref = CFBridgingRetain(event);
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)(slot + 1));
    return pl_hostcall_witness(t, "gpu.session-open", base);
  }
}

static pl_val gm_session_close(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.session-close", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  @autoreleasepool {
    for (uint32_t i = 0; i < s->alloc_count; i++) {
      if (s->allocs[i].buffer_ref != NULL)
        CFRelease(s->allocs[i].buffer_ref);
    }
    for (uint32_t i = 0; i < s->dheap_count; i++)
      CFRelease(s->dheaps[i].buffer_ref);
    for (uint32_t i = 0; i < s->object_count; i++)
      CFRelease(s->object_refs[i]);
    if (s->arena_ref != NULL)
      CFRelease(s->arena_ref);
    CFRelease(s->event_ref);
    CFRelease(s->queue_ref);
    CFRelease(s->device_ref);
    memset(s, 0, sizeof(*s));
  }
  return pl_hostcall_witness(t, "gpu.session-close", t->vsp);
}

/* ── Arena and pointer complements (Gap 2) ─────────────────────────────── */

static pl_val gm_arena(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.arena", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  ARG(1) = pl_nf(t, ARG(1));
  pl_val* f = gm_record(ARG(1), "memory-arena", 3);
  if (f == NULL)
    return pl_hostcall_refusal(t, "gpu.arena", GM_REFUSAL_CARRIER_SHAPE,
                               "expected memory-arena record");
  uint64_t capacity = pl_nat_u64_clamp(pl_nat_coerce(f[1]));
  if (capacity == 0 || capacity > ((uint64_t)1 << 30))
    return pl_hostcall_refusal(t, "gpu.arena", GM_REFUSAL_BOUNDS,
                               "arena capacity out of range");
  if (s->arena_ref != NULL)
    return pl_hostcall_refusal(t, "gpu.arena", GM_REFUSAL_CAPACITY,
                               "session already has an arena");
  @autoreleasepool {
    MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    descriptor.size = (NSUInteger)((capacity + 4095u) & ~(uint64_t)4095u);
    id<MTLDevice> device = (__bridge id<MTLDevice>)s->device_ref;
    id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
    if (heap == nil)
      return pl_hostcall_refusal(t, "gpu.arena", GM_REFUSAL_ALLOC,
                                 "heap create failed");
    s->arena_ref = CFBridgingRetain(heap);
    s->arena_capacity = (size_t)capacity;
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)capacity);
    return pl_hostcall_witness(t, "gpu.arena", base);
  }
}

static pl_val gm_alloc_body(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.alloc", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  if (s->arena_ref == NULL)
    return pl_hostcall_refusal(t, "gpu.alloc", GM_REFUSAL_CARRIER_SHAPE,
                               "session has no admitted arena");
  size_t n = 0;
  uint8_t* bytes = gm_bar_bytes(pl_nat_coerce(ARG(1)), &n);
  if (bytes == NULL || n == 0) {
    free(bytes);
    return pl_hostcall_refusal(t, "gpu.alloc", GM_REFUSAL_BOUNDS,
                               "empty payload");
  }
  @autoreleasepool {
    /* reuse a freed slot (its generation is already bumped) or take a
     * fresh one */
    int slot = -1;
    for (uint32_t i = 0; i < s->alloc_count; i++) {
      if (s->allocs[i].buffer_ref == NULL) {
        slot = (int)i;
        break;
      }
    }
    if (slot < 0) {
      if (s->alloc_count >= GM_ALLOCS) {
        free(bytes);
        return pl_hostcall_refusal(t, "gpu.alloc", GM_REFUSAL_CAPACITY,
                                   "allocation table full");
      }
      slot = (int)s->alloc_count++;
      s->allocs[slot].generation = 0;
    }
    id<MTLHeap> heap = (__bridge id<MTLHeap>)s->arena_ref;
    id<MTLBuffer> buffer =
        [heap newBufferWithLength:(NSUInteger)n
                          options:MTLResourceStorageModeShared];
    if (buffer == nil) {
      free(bytes);
      return pl_hostcall_refusal(t, "gpu.alloc", GM_REFUSAL_ALLOC,
                                 "arena exhausted");
    }
    memcpy([buffer contents], bytes, n);
    free(bytes);
    gm_alloc* a = &s->allocs[slot];
    a->buffer_ref = CFBridgingRetain(buffer);
    a->generation += 1;
    a->length = n;
    /* pointer-complement witness payload: alloc, offset, length,
     * generation */
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)(slot + 1));
    pl_vpush(t, 0);
    pl_vpush(t, (pl_val)n);
    pl_vpush(t, (pl_val)a->generation);
    return pl_hostcall_witness(t, "gpu.alloc", base);
  }
}

static pl_val gm_free_body(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.free", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  ARG(1) = pl_nf(t, ARG(1));
  pl_val* f = gm_record(ARG(1), "heap-pointer", 5);
  if (f == NULL)
    return pl_hostcall_refusal(t, "gpu.free", GM_REFUSAL_CARRIER_SHAPE,
                               "expected heap-pointer record");
  uint64_t refusal = 0;
  size_t offset = 0, length = 0;
  id<MTLBuffer> buffer = gm_pointer(s, f, &offset, &length, &refusal);
  if (buffer == nil)
    return pl_hostcall_refusal(t, "gpu.free", refusal,
                               "free of unknown or stale pointer");
  @autoreleasepool {
    uint64_t idx = pl_nat_u64_clamp(pl_nat_coerce(f[0]));
    gm_alloc* a = &s->allocs[idx - 1];
    CFRelease(a->buffer_ref);
    a->buffer_ref = NULL;
    a->generation += 1; /* consume the linear resource */
    a->length = 0;
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)idx);
    pl_vpush(t, (pl_val)a->generation);
    return pl_hostcall_witness(t, "gpu.free", base);
  }
}

/* The 64-bit GPU virtual address of an admitted complement window —
 * "all pointers in GPU data structures must use GPU addresses"
 * (NoGraphicsApi).  Substrate-side laws embed these in root payloads;
 * native never chases them, the shader does. */
static pl_val gm_device_address(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.device-address",
                               GM_REFUSAL_BAD_SESSION, "unknown session");
  ARG(1) = pl_nf(t, ARG(1));
  pl_val* f = gm_record(ARG(1), "heap-pointer", 5);
  if (f == NULL)
    return pl_hostcall_refusal(t, "gpu.device-address",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "expected heap-pointer record");
  uint64_t refusal = 0;
  size_t offset = 0, length = 0;
  id<MTLBuffer> buffer = gm_pointer(s, f, &offset, &length, &refusal);
  if (buffer == nil)
    return pl_hostcall_refusal(t, "gpu.device-address", refusal,
                               "pointer rejected");
  @autoreleasepool {
    uint64_t address = [buffer gpuAddress] + (uint64_t)offset;
    size_t base = t->vsp;
    pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)&address, 8));
    return pl_hostcall_witness(t, "gpu.device-address", base);
  }
}

/* Buffer readback through the complement: the Consumer leg for
 * non-pixel evidence.  Waits the session event (like gpu.readback)
 * and returns the window bytes as a bar. */
static pl_val gm_read(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.read", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  ARG(1) = pl_nf(t, ARG(1));
  pl_val* f = gm_record(ARG(1), "heap-pointer", 5);
  if (f == NULL)
    return pl_hostcall_refusal(t, "gpu.read", GM_REFUSAL_CARRIER_SHAPE,
                               "expected heap-pointer record");
  uint64_t refusal = 0;
  size_t offset = 0, length = 0;
  id<MTLBuffer> buffer = gm_pointer(s, f, &offset, &length, &refusal);
  if (buffer == nil)
    return pl_hostcall_refusal(t, "gpu.read", refusal, "pointer rejected");
  @autoreleasepool {
    if (s->event_value > 0) {
      id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
      if (![event waitUntilSignaledValue:s->event_value timeoutMS:5000])
        return pl_hostcall_refusal(t, "gpu.read", GM_REFUSAL_ENCODE,
                                   "submission event timeout");
    }
    uint8_t* bar = malloc(length + 1);
    if (bar == NULL)
      return pl_hostcall_refusal(t, "gpu.read", GM_REFUSAL_ALLOC,
                                 "readback alloc failed");
    memcpy(bar, (const uint8_t*)[buffer contents] + offset, length);
    bar[length] = 0x01;
    pl_val bytes = pl_nat_from_bytes(t, bar, length + 1);
    free(bar);
    size_t base = t->vsp;
    pl_vpush(t, bytes);
    return pl_hostcall_witness(t, "gpu.read", base);
  }
}

/* CPU-side wait on the session timeline without reading bytes (Gap
 * 11): the frames-in-flight primitive.  Waiting a value the session
 * never signaled is a caller bug, refused up front. */
static pl_val gm_wait(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.wait", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  uint64_t value = pl_nat_u64_clamp(pl_nat_coerce(ARG(1)));
  if (value > s->event_value)
    return pl_hostcall_refusal(t, "gpu.wait", GM_REFUSAL_BOUNDS,
                               "wait value was never signaled");
  @autoreleasepool {
    if (value > 0) {
      id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
      if (![event waitUntilSignaledValue:value timeoutMS:5000])
        return pl_hostcall_refusal(t, "gpu.wait", GM_REFUSAL_ENCODE,
                                   "submission event timeout");
    }
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)value);
    return pl_hostcall_witness(t, "gpu.wait", base);
  }
}

/* Write admitted bytes into a live allocation window (Gap 11).  A
 * pointer complement is a linear residual (ACL6.2): the write consumes
 * it and mints the successor — wait the session timeline (the
 * Producer-side dual of gpu.read: the write happens-after all
 * submitted GPU work), memcpy, bump the generation.  Every prior
 * complement over the allocation goes stale (950 on reuse).
 * Frames-in-flight is a pattern (N allocations round-robin), never
 * native cleverness. */
static pl_val gm_write(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.write", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  ARG(1) = pl_nf(t, ARG(1));
  pl_val* f = gm_record(ARG(1), "heap-pointer", 5);
  if (f == NULL)
    return pl_hostcall_refusal(t, "gpu.write", GM_REFUSAL_CARRIER_SHAPE,
                               "expected heap-pointer record");
  uint64_t refusal = 0;
  size_t offset = 0, length = 0;
  id<MTLBuffer> buffer = gm_pointer(s, f, &offset, &length, &refusal);
  if (buffer == nil)
    return pl_hostcall_refusal(t, "gpu.write", refusal, "pointer rejected");
  size_t n = 0;
  uint8_t* bytes = gm_bar_bytes(pl_nat_coerce(ARG(2)), &n);
  if (bytes == NULL || n != length) {
    free(bytes);
    return pl_hostcall_refusal(t, "gpu.write", GM_REFUSAL_BOUNDS,
                               "write bytes do not fill the window");
  }
  @autoreleasepool {
    if (s->event_value > 0) {
      id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
      if (![event waitUntilSignaledValue:s->event_value timeoutMS:5000]) {
        free(bytes);
        return pl_hostcall_refusal(t, "gpu.write", GM_REFUSAL_ENCODE,
                                   "submission event timeout");
      }
    }
    memcpy((uint8_t*)[buffer contents] + offset, bytes, n);
    free(bytes);
    uint64_t idx = pl_nat_u64_clamp(pl_nat_coerce(f[0]));
    gm_alloc* a = &s->allocs[idx - 1];
    a->generation += 1; /* consume the residual, mint the successor */
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)idx);
    pl_vpush(t, (pl_val)a->generation);
    return pl_hostcall_witness(t, "gpu.write", base);
  }
}

/* ── Artifacts and pipelines (Gap 6 cache) ─────────────────────────────── */

static pl_val gm_library(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.library", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  size_t n = 0;
  uint8_t* bytes = gm_bar_bytes(pl_nat_coerce(ARG(1)), &n);
  if (bytes == NULL || n == 0) {
    free(bytes);
    return pl_hostcall_refusal(t, "gpu.library", GM_REFUSAL_BOUNDS,
                               "empty source artifact");
  }
  uint8_t key[32];
  ax_sha256(bytes, n, key);
  @autoreleasepool {
    uint32_t hits = 0;
    id<MTLLibrary> library = nil;
    gm_cache_entry* hit = gm_cache_find(gm_library_cache, key);
    if (hit != NULL) {
      hit->hits += 1;
      hits = hit->hits;
      library = (__bridge id<MTLLibrary>)hit->object_ref;
      free(bytes);
    } else {
      NSString* source = [[NSString alloc] initWithBytes:bytes
                                                  length:n
                                                encoding:NSUTF8StringEncoding];
      free(bytes);
      if (source == nil)
        return pl_hostcall_refusal(t, "gpu.library", GM_REFUSAL_BOUNDS,
                                   "artifact is not utf-8");
      id<MTLDevice> device = (__bridge id<MTLDevice>)s->device_ref;
      /* safe math: witness comparisons against CPU table-walkers need
       * deterministic op order, not fast-math reassociation */
      MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
      options.mathMode = MTLMathModeSafe;
      NSError* error = nil;
      library = [device newLibraryWithSource:source
                                     options:options
                                       error:&error];
      if (library == nil)
        return pl_hostcall_refusal(t, "gpu.library", GM_REFUSAL_LIBRARY,
                                   "msl compile failed");
      gm_cache_insert(gm_library_cache, key, library);
    }
    uint64_t h = gm_intern(s, library, GM_OBJ_LIBRARY);
    if (h == 0)
      return pl_hostcall_refusal(t, "gpu.library", GM_REFUSAL_CAPACITY,
                                 "object table full");
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)h);
    pl_vpush(t, (pl_val)hits);
    return pl_hostcall_witness(t, "gpu.library", base);
  }
}

/* The no-api function discipline: the pipeline takes the library's
 * unique vertex and unique fragment function, selected by type, never
 * by name (names are labels, not authority). */
static id<MTLFunction> gm_unique_function(id<MTLLibrary> library,
                                          MTLFunctionType type) {
  id<MTLFunction> found = nil;
  for (NSString* name in [library functionNames]) {
    id<MTLFunction> fn = [library newFunctionWithName:name];
    if (fn == nil || fn.functionType != type)
      continue;
    if (found != nil)
      return nil; /* ambiguous */
    found = fn;
  }
  return found;
}

static pl_val gm_pipeline_render(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.pipeline-render",
                               GM_REFUSAL_BAD_SESSION, "unknown session");
  size_t n = 0;
  uint64_t stride = 0, mode = 0;
  uint8_t* bytes = gm_pipeline_source(t, ab, &n, &stride, &mode);
  if (bytes == NULL)
    return pl_hostcall_refusal(t, "gpu.pipeline-render",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "expected artifact bar or pipeline-request");
  if (n == 0) {
    free(bytes);
    return pl_hostcall_refusal(t, "gpu.pipeline-render", GM_REFUSAL_BOUNDS,
                               "empty source artifact");
  }
  uint8_t lib_key[32], key[32];
  ax_sha256(bytes, n, lib_key);
  gm_pipeline_key(lib_key, stride, mode, key);
  free(bytes);
  @autoreleasepool {
    uint32_t hits = 0;
    id<MTLRenderPipelineState> pipeline = nil;
    gm_cache_entry* hit = gm_cache_find(gm_pipeline_cache, key);
    if (hit != NULL) {
      hit->hits += 1;
      hits = hit->hits;
      pipeline = (__bridge id<MTLRenderPipelineState>)hit->object_ref;
    } else {
      gm_cache_entry* lib_hit = gm_cache_find(gm_library_cache, lib_key);
      if (lib_hit == NULL)
        return pl_hostcall_refusal(t, "gpu.pipeline-render",
                                   GM_REFUSAL_BAD_HANDLE,
                                   "artifact has no compiled library");
      id<MTLLibrary> library = (__bridge id<MTLLibrary>)lib_hit->object_ref;
      MTLRenderPipelineDescriptor* descriptor =
          [[MTLRenderPipelineDescriptor alloc] init];
      descriptor.vertexFunction =
          gm_unique_function(library, MTLFunctionTypeVertex);
      descriptor.fragmentFunction =
          gm_unique_function(library, MTLFunctionTypeFragment);
      descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
      if (descriptor.vertexFunction == nil ||
          descriptor.fragmentFunction == nil)
        return pl_hostcall_refusal(
            t, "gpu.pipeline-render", GM_REFUSAL_PIPELINE,
            "library must expose one vertex and one fragment function");
      id<MTLDevice> device = (__bridge id<MTLDevice>)s->device_ref;
      NSError* error = nil;
      id<MTLRenderPipelineState> fresh =
          [device newRenderPipelineStateWithDescriptor:descriptor
                                                 error:&error];
      if (fresh == nil)
        return pl_hostcall_refusal(t, "gpu.pipeline-render",
                                   GM_REFUSAL_PIPELINE,
                                   "pipeline create failed");
      gm_cache_insert(gm_pipeline_cache, key, fresh);
      pipeline = fresh;
    }
    uint64_t h = gm_intern(s, pipeline, GM_OBJ_PIPELINE);
    if (h == 0)
      return pl_hostcall_refusal(t, "gpu.pipeline-render", GM_REFUSAL_CAPACITY,
                                 "object table full");
    s->object_modes[h - 1] = (uint8_t)mode;
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)h);
    pl_vpush(t, (pl_val)hits);
    return pl_hostcall_witness(t, "gpu.pipeline-render", base);
  }
}

/* Compute pipeline from the same artifact key: the library's unique
 * kernel function (selected by type, never name).  One entry point —
 * the CUDA/no-api discipline; the 16-entry-point framework stays out. */
static pl_val gm_pipeline_compute(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.pipeline-compute",
                               GM_REFUSAL_BAD_SESSION, "unknown session");
  size_t n = 0;
  uint64_t stride = 0, mode = 0;
  uint8_t* bytes = gm_pipeline_source(t, ab, &n, &stride, &mode);
  if (bytes == NULL)
    return pl_hostcall_refusal(t, "gpu.pipeline-compute",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "expected artifact bar or pipeline-request");
  if (n == 0) {
    free(bytes);
    return pl_hostcall_refusal(t, "gpu.pipeline-compute", GM_REFUSAL_BOUNDS,
                               "empty source artifact");
  }
  uint8_t lib_key[32], key[32];
  ax_sha256(bytes, n, lib_key);
  gm_pipeline_key(lib_key, stride, mode, key);
  free(bytes);
  @autoreleasepool {
    uint32_t hits = 0;
    id<MTLComputePipelineState> pipeline = nil;
    gm_cache_entry* hit = gm_cache_find(gm_compute_cache, key);
    if (hit != NULL) {
      hit->hits += 1;
      hits = hit->hits;
      pipeline = (__bridge id<MTLComputePipelineState>)hit->object_ref;
    } else {
      gm_cache_entry* lib_hit = gm_cache_find(gm_library_cache, lib_key);
      if (lib_hit == NULL)
        return pl_hostcall_refusal(t, "gpu.pipeline-compute",
                                   GM_REFUSAL_BAD_HANDLE,
                                   "artifact has no compiled library");
      id<MTLLibrary> library = (__bridge id<MTLLibrary>)lib_hit->object_ref;
      id<MTLFunction> kernel =
          gm_unique_function(library, MTLFunctionTypeKernel);
      if (kernel == nil)
        return pl_hostcall_refusal(t, "gpu.pipeline-compute",
                                   GM_REFUSAL_PIPELINE,
                                   "library must expose one kernel function");
      id<MTLDevice> device = (__bridge id<MTLDevice>)s->device_ref;
      NSError* error = nil;
      id<MTLComputePipelineState> fresh =
          [device newComputePipelineStateWithFunction:kernel error:&error];
      if (fresh == nil)
        return pl_hostcall_refusal(t, "gpu.pipeline-compute",
                                   GM_REFUSAL_PIPELINE,
                                   "compute pipeline create failed");
      gm_cache_insert(gm_compute_cache, key, fresh);
      pipeline = fresh;
    }
    uint64_t h = gm_intern(s, pipeline, GM_OBJ_PIPELINE_COMPUTE);
    if (h == 0)
      return pl_hostcall_refusal(t, "gpu.pipeline-compute",
                                 GM_REFUSAL_CAPACITY, "object table full");
    s->object_modes[h - 1] = (uint8_t)mode;
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)h);
    pl_vpush(t, (pl_val)hits);
    return pl_hostcall_witness(t, "gpu.pipeline-compute", base);
  }
}

/* ── Targets and sampled textures ──────────────────────────────────────── */

static pl_val gm_target(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.target", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  uint64_t w = pl_nat_u64_clamp(pl_nat_coerce(ARG(1)));
  uint64_t h = pl_nat_u64_clamp(pl_nat_coerce(ARG(2)));
  if (w == 0 || h == 0 || w > GM_MAX_EXTENT || h > GM_MAX_EXTENT)
    return pl_hostcall_refusal(t, "gpu.target", GM_REFUSAL_BOUNDS,
                               "target extent out of range");
  @autoreleasepool {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:(NSUInteger)w
                                    height:(NSUInteger)h
                                 mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLDevice> device = (__bridge id<MTLDevice>)s->device_ref;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (texture == nil)
      return pl_hostcall_refusal(t, "gpu.target", GM_REFUSAL_ALLOC,
                                 "texture alloc failed");
    uint64_t handle = gm_intern(s, texture, GM_OBJ_TEXTURE);
    if (handle == 0)
      return pl_hostcall_refusal(t, "gpu.target", GM_REFUSAL_CAPACITY,
                                 "object table full");
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)handle);
    return pl_hostcall_witness(t, "gpu.target", base);
  }
}

/* Sampled 2D texture from admitted BGRA8 bytes. */
static pl_val gm_texture_2d(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.texture-2d", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  uint64_t w = pl_nat_u64_clamp(pl_nat_coerce(ARG(1)));
  uint64_t h = pl_nat_u64_clamp(pl_nat_coerce(ARG(2)));
  if (w == 0 || h == 0 || w > GM_MAX_EXTENT || h > GM_MAX_EXTENT)
    return pl_hostcall_refusal(t, "gpu.texture-2d", GM_REFUSAL_BOUNDS,
                               "texture extent out of range");
  size_t n = 0;
  uint8_t* bytes = gm_bar_bytes(pl_nat_coerce(ARG(3)), &n);
  if (bytes == NULL || n != (size_t)(w * h * 4)) {
    free(bytes);
    return pl_hostcall_refusal(t, "gpu.texture-2d", GM_REFUSAL_BOUNDS,
                               "pixel payload is not w*h*4 bytes");
  }
  @autoreleasepool {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:(NSUInteger)w
                                    height:(NSUInteger)h
                                 mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLDevice> device = (__bridge id<MTLDevice>)s->device_ref;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (texture == nil) {
      free(bytes);
      return pl_hostcall_refusal(t, "gpu.texture-2d", GM_REFUSAL_ALLOC,
                                 "texture alloc failed");
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
               mipmapLevel:0
                 withBytes:bytes
               bytesPerRow:(NSUInteger)(w * 4)];
    free(bytes);
    uint64_t handle = gm_intern(s, texture, GM_OBJ_TEXTURE);
    if (handle == 0)
      return pl_hostcall_refusal(t, "gpu.texture-2d", GM_REFUSAL_CAPACITY,
                                 "object table full");
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)handle);
    return pl_hostcall_witness(t, "gpu.texture-2d", base);
  }
}

/* ── Descriptor heap (Gap 4) ───────────────────────────────────────────── */

static pl_val gm_descriptor_heap(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.descriptor-heap",
                               GM_REFUSAL_BAD_SESSION, "unknown session");
  ARG(1) = pl_nf(t, ARG(1));
  pl_val* f = gm_record(ARG(1), "descriptor-heap", 2);
  if (f == NULL)
    return pl_hostcall_refusal(t, "gpu.descriptor-heap",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "expected descriptor-heap record");
  uint64_t count = pl_nat_u64_clamp(pl_nat_coerce(f[0]));
  if (count == 0 || count > 1024)
    return pl_hostcall_refusal(t, "gpu.descriptor-heap", GM_REFUSAL_BOUNDS,
                               "descriptor count out of range");
  if (s->dheap_count >= GM_DHEAPS)
    return pl_hostcall_refusal(t, "gpu.descriptor-heap", GM_REFUSAL_CAPACITY,
                               "descriptor heap table full");
  @autoreleasepool {
    /* descriptors are bytes at index x stride: MTLResourceIDs at
     * index x 8 (the stride-class field is recorded by the carrier;
     * Metal collapses sampled/storage to one stride, MoltenVK will not) */
    id<MTLDevice> device = (__bridge id<MTLDevice>)s->device_ref;
    id<MTLBuffer> buffer =
        [device newBufferWithLength:(NSUInteger)(count * 8)
                            options:MTLResourceStorageModeShared];
    if (buffer == nil)
      return pl_hostcall_refusal(t, "gpu.descriptor-heap", GM_REFUSAL_ALLOC,
                                 "argument buffer alloc failed");
    memset([buffer contents], 0, (size_t)(count * 8));
    gm_dheap* dh = &s->dheaps[s->dheap_count++];
    memset(dh, 0, sizeof(*dh));
    dh->buffer_ref = CFBridgingRetain(buffer);
    dh->count = (uint32_t)count;
    dh->generation = 1;
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)s->dheap_count);
    pl_vpush(t, (pl_val)count);
    return pl_hostcall_witness(t, "gpu.descriptor-heap", base);
  }
}

static pl_val gm_descriptor_write(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.descriptor-write",
                               GM_REFUSAL_BAD_SESSION, "unknown session");
  gm_dheap* dh = gm_dheap_at(s, ARG(1));
  if (dh == NULL)
    return pl_hostcall_refusal(t, "gpu.descriptor-write",
                               GM_REFUSAL_BAD_HANDLE,
                               "unknown descriptor heap");
  ARG(2) = pl_nf(t, ARG(2));
  pl_val* f = gm_record(ARG(2), "descriptor-window", 4);
  if (f == NULL)
    return pl_hostcall_refusal(t, "gpu.descriptor-write",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "expected descriptor-window record");
  uint64_t heap = pl_nat_u64_clamp(pl_nat_coerce(f[0]));
  uint64_t index = pl_nat_u64_clamp(pl_nat_coerce(f[1]));
  uint64_t generation = pl_nat_u64_clamp(pl_nat_coerce(f[3]));
  if (heap != pl_nat_u64_clamp(pl_nat_coerce(ARG(1))))
    return pl_hostcall_refusal(t, "gpu.descriptor-write",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "window names a different heap");
  if (generation != dh->generation)
    return pl_hostcall_refusal(t, "gpu.descriptor-write",
                               GM_REFUSAL_STALE_GENERATION,
                               "outdated descriptor window");
  if (index >= dh->count)
    return pl_hostcall_refusal(t, "gpu.descriptor-write", GM_REFUSAL_BOUNDS,
                               "window index outside heap");
  @autoreleasepool {
    id<MTLTexture> texture = gm_object_v(s, ARG(3), GM_OBJ_TEXTURE);
    if (texture == nil)
      return pl_hostcall_refusal(t, "gpu.descriptor-write",
                                 GM_REFUSAL_BAD_HANDLE, "unknown texture");
    if (dh->texture_count >= GM_DHEAP_TEXTURES)
      return pl_hostcall_refusal(t, "gpu.descriptor-write",
                                 GM_REFUSAL_CAPACITY, "residency table full");
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)dh->buffer_ref;
    MTLResourceID rid = texture.gpuResourceID;
    memcpy((uint8_t*)[buffer contents] + index * 8, &rid, 8);
    dh->textures[dh->texture_count++] = pl_nat_u64_clamp(ARG(3));
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)index);
    pl_vpush(t, (pl_val)dh->generation);
    return pl_hostcall_witness(t, "gpu.descriptor-write", base);
  }
}

/* ── Generic command graph (Gap 3 + Gap 5 lowering) ────────────────────── */

/* (Re)open a render pass with the graph's standing bindings: pipeline,
 * root payload at vertex 0, descriptor heap at fragment 1 with its
 * written textures made resident. */
static id<MTLRenderCommandEncoder> gm_open_pass(
    id<MTLCommandBuffer> commands, id<MTLTexture> target, MTLLoadAction load,
    MTLClearColor clear, id<MTLRenderPipelineState> pipeline,
    id<MTLBuffer> root, NSUInteger root_offset, NSUInteger root_length,
    uint8_t root_mode, gm_session* s, gm_dheap* dh) {
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = load;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = clear;
  id<MTLRenderCommandEncoder> encoder =
      [commands renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil)
    return nil;
  [encoder setRenderPipelineState:pipeline];
  if (root_mode == GM_ROOT_INLINE) {
    /* the push-constant lowering: root bytes snapshot from the admitted
     * complement window at encode time */
    [encoder setVertexBytes:(const uint8_t*)[root contents] + root_offset
                     length:root_length
                    atIndex:0];
  } else {
    [encoder setVertexBuffer:root offset:root_offset atIndex:0];
  }
  /* every arena allocation is resident: root payloads may carry device
   * addresses into sibling allocations that native never inspects */
  if (s->arena_ref != NULL)
    [encoder useHeap:(__bridge id<MTLHeap>)s->arena_ref
              stages:MTLRenderStageVertex | MTLRenderStageFragment];
  if (dh != NULL) {
    id<MTLBuffer> heap = (__bridge id<MTLBuffer>)dh->buffer_ref;
    [encoder setFragmentBuffer:heap offset:0 atIndex:1];
    for (uint32_t i = 0; i < dh->texture_count; i++) {
      id<MTLTexture> texture = gm_object(s, dh->textures[i], GM_OBJ_TEXTURE);
      if (texture != nil)
        [encoder useResource:texture
                       usage:MTLResourceUsageRead
                      stages:MTLRenderStageFragment];
    }
  }
  return encoder;
}

/* Walk a pair-list of command-step records, validating shape.  Native
 * reads only the step kind and its generic args — payload meaning lives
 * in the root layout and the artifact, never here.  Declared hazard
 * facts lower to pass splits between draw steps; the witness carries
 * (steps-executed, splits, event-value). */
/* A blit graph (Gap 11): pipeline 0 selects copy-only encoding — the
 * lawful byte-movement legs (buffer→buffer, buffer↔texture) behind the
 * same graph carrier, wait-value, and signal discipline.  A copy
 * destination is consumed and re-minted like gpu.write: its slot
 * generation bumps at encode, appended to the witness in step order,
 * and visibility of the new bytes is ordered by this graph's signal
 * value.  Copies within one blit encoder are hardware-ordered, so a
 * blit graph carries no hazard facts. */
#define GM_BLIT_MAX_STEPS 32

static bool gm_is_zero(pl_val v) {
  return pl_is_nat(v) && pl_nat_is_zero(pl_nat_coerce(v));
}

static pl_val gm_blit_graph(pl_thread* t, size_t ab, gm_session* s,
                            pl_val* graph) {
  if (!gm_is_zero(ARG(3)) || !gm_is_zero(ARG(4)) || !gm_is_zero(ARG(5)))
    return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_GRAPH_SHAPE,
                               "blit graph takes no target, root or heap");
  if (!gm_is_zero(graph[2]))
    return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_GRAPH_SHAPE,
                               "blit copies are encoder-ordered; no facts");
  @autoreleasepool {
    uint64_t declared = pl_nat_u64_clamp(pl_nat_coerce(graph[1]));
    uint64_t kinds[GM_BLIT_MAX_STEPS];
    id<MTLBuffer> srcs[GM_BLIT_MAX_STEPS] = {nil};
    id<MTLBuffer> dsts[GM_BLIT_MAX_STEPS] = {nil};
    id<MTLTexture> texs[GM_BLIT_MAX_STEPS] = {nil};
    size_t srcoff[GM_BLIT_MAX_STEPS], dstoff[GM_BLIT_MAX_STEPS];
    size_t lens[GM_BLIT_MAX_STEPS];
    uint64_t ws[GM_BLIT_MAX_STEPS], hs[GM_BLIT_MAX_STEPS];
    uint64_t bump[GM_BLIT_MAX_STEPS] = {0};
    uint64_t walked = 0;
    pl_val cursor = graph[0];
    while (!gm_is_zero(cursor)) {
      pl_val* cell = gm_record(cursor, "pair", 2);
      if (cell == NULL)
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "steps must be a pair list");
      pl_val* step = gm_record(cell[0], "command-step", 5);
      if (step == NULL)
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "expected command-step record");
      if (walked >= GM_BLIT_MAX_STEPS)
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "blit graph exceeds 32 steps");
      uint64_t kind = pl_nat_u64_clamp(pl_nat_coerce(step[0]));
      kinds[walked] = kind;
      uint64_t rk = 0;
      if (kind == GM_STEP_COPY) {
        pl_val* sp = gm_record(step[1], "heap-pointer", 5);
        pl_val* dp = gm_record(step[2], "heap-pointer", 5);
        if (sp == NULL || dp == NULL)
          return pl_hostcall_refusal(t, "gpu.command-graph",
                                     GM_REFUSAL_CARRIER_SHAPE,
                                     "copy takes two heap-pointer records");
        srcs[walked] = gm_pointer(s, sp, &srcoff[walked], &lens[walked], &rk);
        if (srcs[walked] == nil)
          return pl_hostcall_refusal(t, "gpu.command-graph", rk,
                                     "copy source rejected");
        size_t dlen = 0;
        dsts[walked] = gm_pointer(s, dp, &dstoff[walked], &dlen, &rk);
        if (dsts[walked] == nil)
          return pl_hostcall_refusal(t, "gpu.command-graph", rk,
                                     "copy destination rejected");
        if (dlen != lens[walked])
          return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_BOUNDS,
                                     "copy window lengths differ");
        bump[walked] = pl_nat_u64_clamp(pl_nat_coerce(dp[0]));
      } else if (kind == GM_STEP_COPY_TO_TEXTURE ||
                 kind == GM_STEP_COPY_FROM_TEXTURE) {
        bool to_tex = kind == GM_STEP_COPY_TO_TEXTURE;
        pl_val* pp = gm_record(step[to_tex ? 1 : 2], "heap-pointer", 5);
        if (pp == NULL)
          return pl_hostcall_refusal(t, "gpu.command-graph",
                                     GM_REFUSAL_CARRIER_SHAPE,
                                     "texture copy takes a heap-pointer");
        texs[walked] =
            gm_object_v(s, step[to_tex ? 2 : 1], GM_OBJ_TEXTURE);
        if (texs[walked] == nil)
          return pl_hostcall_refusal(t, "gpu.command-graph",
                                     GM_REFUSAL_BAD_HANDLE, "unknown texture");
        id<MTLBuffer> b;
        if (to_tex) {
          b = gm_pointer(s, pp, &srcoff[walked], &lens[walked], &rk);
          srcs[walked] = b;
        } else {
          b = gm_pointer(s, pp, &dstoff[walked], &lens[walked], &rk);
          dsts[walked] = b;
          bump[walked] = pl_nat_u64_clamp(pl_nat_coerce(pp[0]));
        }
        if (b == nil)
          return pl_hostcall_refusal(t, "gpu.command-graph", rk,
                                     "texture copy window rejected");
        ws[walked] = pl_nat_u64_clamp(pl_nat_coerce(step[3]));
        hs[walked] = pl_nat_u64_clamp(pl_nat_coerce(step[4]));
        if (ws[walked] == 0 || hs[walked] == 0 ||
            lens[walked] != (size_t)(ws[walked] * hs[walked] * 4))
          return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_BOUNDS,
                                     "texture copy window is not w*h*4");
      } else {
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "blit graph steps must be copies");
      }
      walked++;
      cursor = cell[1];
    }
    if (walked != declared)
      return pl_hostcall_refusal(t, "gpu.command-graph",
                                 GM_REFUSAL_GRAPH_SHAPE,
                                 "step count does not match graph row");

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)s->queue_ref;
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    if (commands == nil)
      return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_ENCODE,
                                 "command buffer unavailable");
    uint64_t wait_value = pl_nat_u64_clamp(pl_nat_coerce(graph[3]));
    if (wait_value > 0) {
      id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
      [commands encodeWaitForEvent:event value:wait_value];
    }
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (blit == nil)
      return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_ENCODE,
                                 "encoder unavailable");
    for (uint64_t i = 0; i < walked; i++) {
      if (kinds[i] == GM_STEP_COPY) {
        [blit copyFromBuffer:srcs[i]
                sourceOffset:(NSUInteger)srcoff[i]
                    toBuffer:dsts[i]
           destinationOffset:(NSUInteger)dstoff[i]
                        size:(NSUInteger)lens[i]];
      } else if (kinds[i] == GM_STEP_COPY_TO_TEXTURE) {
        [blit copyFromBuffer:srcs[i]
                   sourceOffset:(NSUInteger)srcoff[i]
              sourceBytesPerRow:(NSUInteger)(ws[i] * 4)
            sourceBytesPerImage:(NSUInteger)(ws[i] * hs[i] * 4)
                     sourceSize:MTLSizeMake((NSUInteger)ws[i],
                                            (NSUInteger)hs[i], 1)
                      toTexture:texs[i]
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
      } else {
        [blit copyFromTexture:texs[i]
                        sourceSlice:0
                        sourceLevel:0
                       sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake((NSUInteger)ws[i],
                                                (NSUInteger)hs[i], 1)
                           toBuffer:dsts[i]
                  destinationOffset:(NSUInteger)dstoff[i]
             destinationBytesPerRow:(NSUInteger)(ws[i] * 4)
           destinationBytesPerImage:(NSUInteger)(ws[i] * hs[i] * 4)];
      }
    }
    [blit endEncoding];
    id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
    s->event_value += 1;
    [commands encodeSignalEvent:event value:s->event_value];
    [commands commit];
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)walked);
    pl_vpush(t, 0); /* nothing lowered: copies are encoder-ordered */
    pl_vpush(t, (pl_val)s->event_value);
    for (uint64_t i = 0; i < walked; i++) {
      if (bump[i] != 0) {
        gm_alloc* a = &s->allocs[bump[i] - 1];
        a->generation += 1; /* consume the destination, mint the successor */
        pl_vpush(t, (pl_val)a->generation);
      }
    }
    return pl_hostcall_witness(t, "gpu.command-graph", base);
  }
}

static pl_val gm_command_graph(pl_thread* t, size_t ab) {
  ARG(0) = pl_nf(t, ARG(0));
  gm_session* s = gm_session_at(ARG(1));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  pl_val* graph = gm_record(ARG(0), "command-graph", 4);
  if (graph == NULL)
    return pl_hostcall_refusal(t, "gpu.command-graph",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "expected command-graph record");
  /* pipeline 0 selects the blit graph: copy steps only (Gap 11) */
  if (gm_is_zero(ARG(2)))
    return gm_blit_graph(t, ab, s, graph);
  ARG(4) = pl_nf(t, ARG(4));
  pl_val* root = gm_record(ARG(4), "heap-pointer", 5);
  if (root == NULL)
    return pl_hostcall_refusal(t, "gpu.command-graph",
                               GM_REFUSAL_CARRIER_SHAPE,
                               "expected heap-pointer root record");
  gm_dheap* dh = NULL;
  if (!(pl_is_nat(ARG(5)) && pl_nat_is_zero(pl_nat_coerce(ARG(5))))) {
    dh = gm_dheap_at(s, ARG(5));
    if (dh == NULL)
      return pl_hostcall_refusal(t, "gpu.command-graph",
                                 GM_REFUSAL_BAD_HANDLE,
                                 "unknown descriptor heap");
  }
  @autoreleasepool {
    id<MTLRenderPipelineState> pipeline =
        gm_object_v(s, ARG(2), GM_OBJ_PIPELINE);
    id<MTLComputePipelineState> kernel =
        gm_object_v(s, ARG(2), GM_OBJ_PIPELINE_COMPUTE);
    if (pipeline == nil && kernel == nil)
      return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_BAD_HANDLE,
                                 "unknown pipeline");
    id<MTLTexture> target = nil;
    if (kernel != nil) {
      if (!(pl_is_nat(ARG(3)) && pl_nat_is_zero(pl_nat_coerce(ARG(3)))))
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "compute graph takes no target");
    } else {
      target = gm_object_v(s, ARG(3), GM_OBJ_TEXTURE);
      if (target == nil)
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_BAD_HANDLE, "unknown target");
    }
    uint64_t refusal = 0;
    size_t root_offset = 0, root_length = 0;
    id<MTLBuffer> root_buffer =
        gm_pointer(s, root, &root_offset, &root_length, &refusal);
    if (root_buffer == nil)
      return pl_hostcall_refusal(t, "gpu.command-graph", refusal,
                                 "root pointer rejected");
    uint8_t root_mode = s->object_modes[pl_nat_u64_clamp(ARG(2)) - 1];
    if (root_mode == GM_ROOT_INLINE && root_length > 4096)
      return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_BOUNDS,
                                 "inline root exceeds 4 KiB");

    uint64_t declared = pl_nat_u64_clamp(pl_nat_coerce(graph[1]));
    MTLClearColor clear = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    MTLLoadAction load = MTLLoadActionLoad;

    /* declared hazard facts: validate shape, count them.  Each fact is
     * the no_api (Stage, Stage, Hazard) triple; on TBDR the lowering
     * between draw steps is a pass split. */
    uint64_t hazards = 0;
    pl_val cursor = graph[2];
    while (!(pl_is_nat(cursor) && pl_nat_is_zero(pl_nat_coerce(cursor)))) {
      pl_val* cell = gm_record(cursor, "pair", 2);
      if (cell == NULL || gm_record(cell[0], "hazard-fact", 3) == NULL)
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "hazards must be hazard-fact rows");
      hazards++;
      cursor = cell[1];
    }

    /* first pass: validate every step record and find the leading
     * clear before anything encodes */
    uint64_t walked = 0;
    cursor = graph[0];
    while (!(pl_is_nat(cursor) && pl_nat_is_zero(pl_nat_coerce(cursor)))) {
      pl_val* cell = gm_record(cursor, "pair", 2);
      if (cell == NULL)
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "steps must be a pair list");
      pl_val* step = gm_record(cell[0], "command-step", 5);
      if (step == NULL)
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "expected command-step record");
      uint64_t kind = pl_nat_u64_clamp(pl_nat_coerce(step[0]));
      if (kernel != nil) {
        if (kind == GM_STEP_DISPATCH) {
          uint64_t gx = pl_nat_u64_clamp(pl_nat_coerce(step[1]));
          uint64_t gy = pl_nat_u64_clamp(pl_nat_coerce(step[2]));
          uint64_t gz = pl_nat_u64_clamp(pl_nat_coerce(step[3]));
          uint64_t tw = pl_nat_u64_clamp(pl_nat_coerce(step[4]));
          if (gx == 0 || gy == 0 || gz == 0 || tw == 0)
            return pl_hostcall_refusal(t, "gpu.command-graph",
                                       GM_REFUSAL_GRAPH_SHAPE,
                                       "dispatch with zero extent");
          if (tw > [kernel maxTotalThreadsPerThreadgroup])
            return pl_hostcall_refusal(t, "gpu.command-graph",
                                       GM_REFUSAL_BOUNDS,
                                       "threadgroup width exceeds pipeline");
        } else if (kind == GM_STEP_DISPATCH_INDIRECT) {
          /* the grid comes from 12 admitted bytes — typically written
           * by a prior admissibility stage; the window is validated
           * like any complement, never read by native */
          uint64_t idx = pl_nat_u64_clamp(pl_nat_coerce(step[1]));
          uint64_t off = pl_nat_u64_clamp(pl_nat_coerce(step[2]));
          uint64_t gen = pl_nat_u64_clamp(pl_nat_coerce(step[3]));
          uint64_t tw = pl_nat_u64_clamp(pl_nat_coerce(step[4]));
          if (idx == 0 || idx > s->alloc_count)
            return pl_hostcall_refusal(t, "gpu.command-graph",
                                       GM_REFUSAL_BAD_HANDLE,
                                       "indirect args allocation unknown");
          gm_alloc* a = &s->allocs[idx - 1];
          if (a->buffer_ref == NULL || gen != a->generation)
            return pl_hostcall_refusal(t, "gpu.command-graph",
                                       GM_REFUSAL_STALE_GENERATION,
                                       "indirect args window stale");
          if ((off & 3) != 0 || off + 12 > a->length)
            return pl_hostcall_refusal(t, "gpu.command-graph",
                                       GM_REFUSAL_BOUNDS,
                                       "indirect args window out of bounds");
          if (tw == 0 || tw > [kernel maxTotalThreadsPerThreadgroup])
            return pl_hostcall_refusal(t, "gpu.command-graph",
                                       GM_REFUSAL_BOUNDS,
                                       "threadgroup width exceeds pipeline");
        } else {
          return pl_hostcall_refusal(t, "gpu.command-graph",
                                     GM_REFUSAL_GRAPH_SHAPE,
                                     "compute graph steps must be dispatch");
        }
      } else if (kind == GM_STEP_CLEAR) {
        if (walked != 0)
          return pl_hostcall_refusal(t, "gpu.command-graph",
                                     GM_REFUSAL_GRAPH_SHAPE,
                                     "clear must be the first step");
        uint64_t bgra = pl_nat_u64_clamp(pl_nat_coerce(step[1]));
        clear = MTLClearColorMake((double)((bgra >> 16) & 0xff) / 255.0,
                                  (double)((bgra >> 8) & 0xff) / 255.0,
                                  (double)(bgra & 0xff) / 255.0,
                                  (double)((bgra >> 24) & 0xff) / 255.0);
        load = MTLLoadActionClear;
      } else if (kind != GM_STEP_DRAW) {
        return pl_hostcall_refusal(t, "gpu.command-graph",
                                   GM_REFUSAL_GRAPH_SHAPE,
                                   "unsupported step kind");
      }
      walked++;
      cursor = cell[1];
    }
    if (walked != declared)
      return pl_hostcall_refusal(t, "gpu.command-graph",
                                 GM_REFUSAL_GRAPH_SHAPE,
                                 "step count does not match graph row");

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)s->queue_ref;
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    if (commands == nil)
      return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_ENCODE,
                                 "command buffer unavailable");
    /* the split-barrier consumer half: a declared inter-graph dependency
     * waits the session timeline event before any of this graph's work;
     * independent graphs submitted between a signal and its wait overlap
     * freely.  Absent facts mean submission order stays free — the
     * scheduler's freedom, exercised adversarially by the negative
     * fixture. */
    uint64_t wait_value = pl_nat_u64_clamp(pl_nat_coerce(graph[3]));
    if (wait_value > 0) {
      id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
      [commands encodeWaitForEvent:event value:wait_value];
    }

    uint64_t executed = 0, lowered = 0;
    if (kernel != nil) {
      /* dispatches run CONCURRENT by default — absent facts mean no
       * barrier; a declared fact lowers to a real memory barrier
       * between dispatches (compute encoders support it; only render
       * passes restricted it to pass splits) */
      id<MTLComputeCommandEncoder> compute = [commands
          computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
      if (compute == nil)
        return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_ENCODE,
                                   "encoder unavailable");
      [compute setComputePipelineState:kernel];
      if (root_mode == GM_ROOT_INLINE) {
        [compute setBytes:(const uint8_t*)[root_buffer contents] + root_offset
                   length:(NSUInteger)root_length
                  atIndex:0];
      } else {
        [compute setBuffer:root_buffer
                    offset:(NSUInteger)root_offset
                   atIndex:0];
      }
      if (s->arena_ref != NULL)
        [compute useHeap:(__bridge id<MTLHeap>)s->arena_ref];
      if (dh != NULL) {
        [compute setBuffer:(__bridge id<MTLBuffer>)dh->buffer_ref
                    offset:0
                   atIndex:2];
        for (uint32_t i = 0; i < dh->texture_count; i++) {
          id<MTLTexture> texture =
              gm_object(s, dh->textures[i], GM_OBJ_TEXTURE);
          if (texture != nil)
            [compute useResource:texture usage:MTLResourceUsageRead];
        }
      }
      cursor = graph[0];
      while (!(pl_is_nat(cursor) && pl_nat_is_zero(pl_nat_coerce(cursor)))) {
        pl_val* cell = gm_record(cursor, "pair", 2);
        pl_val* step = gm_record(cell[0], "command-step", 5);
        if (executed > 0 && hazards > 0) {
          [compute memoryBarrierWithScope:MTLBarrierScopeBuffers];
          lowered++;
        }
        uint64_t step_kind = pl_nat_u64_clamp(pl_nat_coerce(step[0]));
        if (step_kind == GM_STEP_DISPATCH_INDIRECT) {
          gm_alloc* a =
              &s->allocs[pl_nat_u64_clamp(pl_nat_coerce(step[1])) - 1];
          [compute
              dispatchThreadgroupsWithIndirectBuffer:(__bridge id<MTLBuffer>)
                                                         a->buffer_ref
                                indirectBufferOffset:
                                    (NSUInteger)pl_nat_u64_clamp(
                                        pl_nat_coerce(step[2]))
                               threadsPerThreadgroup:
                                   MTLSizeMake((NSUInteger)pl_nat_u64_clamp(
                                                   pl_nat_coerce(step[4])),
                                               1, 1)];
        } else {
          [compute
              dispatchThreadgroups:MTLSizeMake(
                                       (NSUInteger)pl_nat_u64_clamp(
                                           pl_nat_coerce(step[1])),
                                       (NSUInteger)pl_nat_u64_clamp(
                                           pl_nat_coerce(step[2])),
                                       (NSUInteger)pl_nat_u64_clamp(
                                           pl_nat_coerce(step[3])))
             threadsPerThreadgroup:MTLSizeMake((NSUInteger)pl_nat_u64_clamp(
                                                   pl_nat_coerce(step[4])),
                                               1, 1)];
        }
        executed++;
        cursor = cell[1];
      }
      [compute endEncoding];
    } else {
      id<MTLRenderCommandEncoder> encoder =
          gm_open_pass(commands, target, load, clear, pipeline, root_buffer,
                       (NSUInteger)root_offset, (NSUInteger)root_length,
                       root_mode, s, dh);
      if (encoder == nil)
        return pl_hostcall_refusal(t, "gpu.command-graph", GM_REFUSAL_ENCODE,
                                   "encoder unavailable");
      uint64_t draws = 0;
      cursor = graph[0];
      while (!(pl_is_nat(cursor) && pl_nat_is_zero(pl_nat_coerce(cursor)))) {
        pl_val* cell = gm_record(cursor, "pair", 2);
        pl_val* step = gm_record(cell[0], "command-step", 5);
        uint64_t kind = pl_nat_u64_clamp(pl_nat_coerce(step[0]));
        if (kind == GM_STEP_DRAW) {
          uint64_t vertices = pl_nat_u64_clamp(pl_nat_coerce(step[1]));
          uint64_t instances = pl_nat_u64_clamp(pl_nat_coerce(step[2]));
          if (vertices == 0 || instances == 0) {
            [encoder endEncoding];
            return pl_hostcall_refusal(t, "gpu.command-graph",
                                       GM_REFUSAL_GRAPH_SHAPE,
                                       "draw with zero extent");
          }
          if (draws > 0 && hazards > 0) {
            [encoder endEncoding];
            encoder = gm_open_pass(commands, target, MTLLoadActionLoad, clear,
                                   pipeline, root_buffer,
                                   (NSUInteger)root_offset,
                                   (NSUInteger)root_length, root_mode, s, dh);
            if (encoder == nil)
              return pl_hostcall_refusal(t, "gpu.command-graph",
                                         GM_REFUSAL_ENCODE,
                                         "encoder unavailable");
            lowered++;
          }
          [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                      vertexStart:0
                      vertexCount:(NSUInteger)vertices
                    instanceCount:(NSUInteger)instances];
          draws++;
        }
        executed++;
        cursor = cell[1];
      }
      [encoder endEncoding];
    }
    id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
    s->event_value += 1;
    [commands encodeSignalEvent:event value:s->event_value];
    [commands commit];
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)executed);
    pl_vpush(t, (pl_val)lowered);
    pl_vpush(t, (pl_val)s->event_value);
    return pl_hostcall_witness(t, "gpu.command-graph", base);
  }
}

static pl_val gm_readback(pl_thread* t, size_t ab) {
  gm_session* s = gm_session_at(ARG(0));
  if (s == NULL)
    return pl_hostcall_refusal(t, "gpu.readback", GM_REFUSAL_BAD_SESSION,
                               "unknown session");
  @autoreleasepool {
    id<MTLTexture> target = gm_object_v(s, ARG(1), GM_OBJ_TEXTURE);
    if (target == nil)
      return pl_hostcall_refusal(t, "gpu.readback", GM_REFUSAL_BAD_HANDLE,
                                 "unknown target");
    uint64_t x = pl_nat_u64_clamp(pl_nat_coerce(ARG(2)));
    uint64_t y = pl_nat_u64_clamp(pl_nat_coerce(ARG(3)));
    if (x >= [target width] || y >= [target height])
      return pl_hostcall_refusal(t, "gpu.readback", GM_REFUSAL_BOUNDS,
                                 "readback outside target");
    /* the only wait in the backend: the session's submission event,
     * never waitUntilCompleted on the world */
    if (s->event_value > 0) {
      id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)s->event_ref;
      if (![event waitUntilSignaledValue:s->event_value timeoutMS:5000])
        return pl_hostcall_refusal(t, "gpu.readback", GM_REFUSAL_ENCODE,
                                   "submission event timeout");
    }
    uint32_t pixel = 0;
    [target getBytes:&pixel
         bytesPerRow:4
          fromRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, 1, 1)
         mipmapLevel:0];
    size_t base = t->vsp;
    pl_vpush(t, (pl_val)pixel);
    return pl_hostcall_witness(t, "gpu.readback", base);
  }
}

/* ── Registration ──────────────────────────────────────────────────────── */

bool pl_host_gpu_metal_register(void) {
  bool ok = true;
  ok = ok && pl_hostcall_register("gpu.session-open", 1, 0b1, gm_session_open);
  ok = ok && pl_hostcall_register("gpu.arena", 2, 0b11, gm_arena);
  ok = ok && pl_hostcall_register("gpu.alloc", 2, 0b11, gm_alloc_body);
  ok = ok && pl_hostcall_register("gpu.free", 2, 0b11, gm_free_body);
  ok = ok &&
       pl_hostcall_register("gpu.device-address", 2, 0b11, gm_device_address);
  ok = ok && pl_hostcall_register("gpu.read", 2, 0b11, gm_read);
  ok = ok && pl_hostcall_register("gpu.wait", 2, 0b11, gm_wait);
  ok = ok && pl_hostcall_register("gpu.write", 3, 0b111, gm_write);
  ok = ok && pl_hostcall_register("gpu.library", 2, 0b11, gm_library);
  ok = ok && pl_hostcall_register("gpu.pipeline-render", 2, 0b11,
                                  gm_pipeline_render);
  ok = ok && pl_hostcall_register("gpu.pipeline-compute", 2, 0b11,
                                  gm_pipeline_compute);
  ok = ok && pl_hostcall_register("gpu.target", 3, 0b111, gm_target);
  ok = ok && pl_hostcall_register("gpu.texture-2d", 4, 0b1111, gm_texture_2d);
  ok = ok && pl_hostcall_register("gpu.descriptor-heap", 2, 0b11,
                                  gm_descriptor_heap);
  ok = ok && pl_hostcall_register("gpu.descriptor-write", 4, 0b1111,
                                  gm_descriptor_write);
  ok = ok && pl_hostcall_register("gpu.command-graph", 6, 0b111111,
                                  gm_command_graph);
  ok = ok && pl_hostcall_register("gpu.readback", 4, 0b1111, gm_readback);
  ok = ok &&
       pl_hostcall_register("gpu.session-close", 1, 0b1, gm_session_close);
  return ok;
}
