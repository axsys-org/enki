/*
    enki_string_builder.h - tiny deferred string builder / flat twine

    public domain-ish single header, in the style of stb libraries.
    no warranty, etc.

    usage:

        #define ENKI_STRING_BUILDER_IMPLEMENTATION
        #include "enki_string_builder.h"

    notes:

        - appended strings are referenced, not copied.
        - caller must keep referenced string bytes alive until materialization.
        - append_cstr() captures strlen() at append time.
        - materialized output is always NUL-terminated, but may contain embedded
   NULs.
        - do not memcpy/copy an initialized builder by value; it has internal
   storage.
*/

#ifndef ENKI_STRING_BUILDER_H
#define ENKI_STRING_BUILDER_H

#include <stddef.h>
#include <stdint.h>
#include "enki/allocator.h"

#ifndef ENKI_SB_INLINE_PIECES
#define ENKI_SB_INLINE_PIECES 16
#endif

#if ENKI_SB_INLINE_PIECES < 1
#error ENKI_SB_INLINE_PIECES must be at least 1
#endif

#ifndef ENKI_SB_API
#ifdef ENKI_SB_STATIC
#define ENKI_SB_API static
#else
#define ENKI_SB_API extern
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct enki_sb_ref {
  const char* data;
  size_t len;
} enki_sb_ref;

typedef enum enki_sb_piece_kind {
  ENKI_SB_PIECE_REF = 1,
  ENKI_SB_PIECE_CHAR = 2,
  ENKI_SB_PIECE_I64 = 3,
  ENKI_SB_PIECE_U64 = 4
} enki_sb_piece_kind;

typedef struct enki_sb_piece {
  unsigned char kind;
  unsigned char base;
  unsigned char uppercase;
  unsigned char _pad;

  union {
    enki_sb_ref ref;
    char ch;
    int64_t i64;
    uint64_t u64;
  } u;
} enki_sb_piece;

typedef struct enki_string_builder {
  const enki_allocator* allocator;

  enki_sb_piece* pieces;
  size_t count;
  size_t capacity;
  int failed;

  enki_sb_piece inline_pieces[ENKI_SB_INLINE_PIECES];
} enki_string_builder;

/* string-literal helper. intentionally only works with narrow string literals.
 */
#define enki_sb_append_lit(sb, literal)                                        \
  enki_sb_append_ref((sb), "" literal, sizeof(literal) - 1u)

/* lifecycle */
ENKI_SB_API void enki_sb_init(enki_string_builder* sb,
                              const enki_allocator* allocator);
ENKI_SB_API void enki_sb_reset(enki_string_builder* sb);
ENKI_SB_API void enki_sb_free(enki_string_builder* sb);

/* status */
ENKI_SB_API int enki_sb_failed(const enki_string_builder* sb);
ENKI_SB_API size_t enki_sb_piece_count(const enki_string_builder* sb);

/* optional preallocation for metadata pieces, not bytes */
ENKI_SB_API int enki_sb_reserve_pieces(enki_string_builder* sb,
                                       size_t additional_piece_count);

/* appending */
ENKI_SB_API int enki_sb_append_ref(enki_string_builder* sb, const char* data,
                                   size_t len);
ENKI_SB_API int enki_sb_append_cstr(enki_string_builder* sb, const char* cstr);
ENKI_SB_API int enki_sb_append_char(enki_string_builder* sb, char c);
ENKI_SB_API int enki_sb_append_i64(enki_string_builder* sb, int64_t value);
ENKI_SB_API int enki_sb_append_u64(enki_string_builder* sb, uint64_t value);
ENKI_SB_API int enki_sb_append_u64_base(enki_string_builder* sb, uint64_t value,
                                        unsigned base, int uppercase);

/* copies only metadata pieces, not referenced string bytes */
ENKI_SB_API int enki_sb_append_builder(enki_string_builder* sb,
                                       const enki_string_builder* other);

/* flattening */
ENKI_SB_API int enki_sb_measure(const enki_string_builder* sb, size_t* out_len);
ENKI_SB_API int enki_sb_write(const enki_string_builder* sb, char* dst,
                              size_t dst_size, size_t* out_len);
ENKI_SB_API char* enki_sb_build(const enki_string_builder* sb, size_t* out_len);
ENKI_SB_API char* enki_sb_build_with_allocator(const enki_string_builder* sb,
                                               const enki_allocator* allocator,
                                               size_t* out_len);

#if defined(ENKI_STRING_BUILDER_IMPLEMENTATION) ||                             \
    defined(ENKI_SB_IMPLEMENTATION)

#include <string.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

static int enki_sb__allocator_ok(const enki_allocator* a) {
  return a->alloc != 0 && a->free != 0;
}

static int enki_sb__grow(enki_string_builder* sb, size_t wanted_capacity) {
  size_t new_capacity;
  size_t bytes;
  enki_sb_piece* new_pieces;

  if (!sb || sb->failed)
    return 0;

  if (wanted_capacity <= sb->capacity)
    return 1;

  if (!enki_sb__allocator_ok(sb->allocator)) {
    sb->failed = 1;
    return 0;
  }

  new_capacity = sb->capacity ? sb->capacity : ENKI_SB_INLINE_PIECES;

  while (new_capacity < wanted_capacity) {
    if (new_capacity > SIZE_MAX / 2u) {
      new_capacity = wanted_capacity;
      break;
    }
    new_capacity *= 2u;
  }

  if (new_capacity > SIZE_MAX / sizeof(enki_sb_piece)) {
    sb->failed = 1;
    return 0;
  }

  bytes = new_capacity * sizeof(enki_sb_piece);

  if (sb->pieces == sb->inline_pieces) {
    new_pieces =
        (enki_sb_piece*)sb->allocator->alloc(sb->allocator->ctx, bytes);
    if (!new_pieces) {
      sb->failed = 1;
      return 0;
    }

    if (sb->count)
      memcpy(new_pieces, sb->inline_pieces, sb->count * sizeof(enki_sb_piece));
  } else if (sb->allocator->realloc) {
    new_pieces = (enki_sb_piece*)sb->allocator->realloc(sb->allocator->ctx,
                                                        sb->pieces, bytes);
    if (!new_pieces) {
      sb->failed = 1;
      return 0;
    }
  } else {
    new_pieces =
        (enki_sb_piece*)sb->allocator->alloc(sb->allocator->ctx, bytes);
    if (!new_pieces) {
      sb->failed = 1;
      return 0;
    }

    if (sb->count)
      memcpy(new_pieces, sb->pieces, sb->count * sizeof(enki_sb_piece));

    sb->allocator->free(sb->allocator->ctx, sb->pieces);
  }

  sb->pieces = new_pieces;
  sb->capacity = new_capacity;
  return 1;
}

static int enki_sb__push(enki_string_builder* sb, enki_sb_piece piece) {
  if (!sb || sb->failed)
    return 0;

  if (sb->count == sb->capacity) {
    if (!enki_sb__grow(sb, sb->count + 1u))
      return 0;
  }

  sb->pieces[sb->count++] = piece;
  return 1;
}

static size_t enki_sb__u64_len(uint64_t value, unsigned base) {
  size_t n = 1;

  while (value >= (uint64_t)base) {
    value /= (uint64_t)base;
    ++n;
  }

  return n;
}

static char* enki_sb__emit_u64(char* dst, uint64_t value, unsigned base,
                               int uppercase) {
  char tmp[64];
  size_t n = 0;
  const char* digits = uppercase ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 : "0123456789abcdefghijklmnopqrstuvwxyz";

  do {
    tmp[n++] = digits[value % (uint64_t)base];
    value /= (uint64_t)base;
  } while (value);

  while (n)
    *dst++ = tmp[--n];

  return dst;
}

static uint64_t enki_sb__abs_i64_to_u64(int64_t value) {
  if (value < 0)
    return (uint64_t)(-(value + 1)) + 1u;

  return (uint64_t)value;
}

static size_t enki_sb__i64_len(int64_t value) {
  size_t n = enki_sb__u64_len(enki_sb__abs_i64_to_u64(value), 10);
  return value < 0 ? n + 1u : n;
}

static char* enki_sb__emit_i64(char* dst, int64_t value) {
  if (value < 0)
    *dst++ = '-';

  return enki_sb__emit_u64(dst, enki_sb__abs_i64_to_u64(value), 10, 0);
}

ENKI_SB_API void enki_sb_init(enki_string_builder* sb,
                              const enki_allocator* allocator) {
  if (!sb)
    return;

  sb->allocator = allocator;
  sb->pieces = sb->inline_pieces;
  sb->count = 0;
  sb->capacity = ENKI_SB_INLINE_PIECES;
  sb->failed = !enki_sb__allocator_ok(allocator);
}

ENKI_SB_API void enki_sb_reset(enki_string_builder* sb) {
  if (!sb)
    return;

  sb->count = 0;
  sb->failed = !enki_sb__allocator_ok(sb->allocator);
}

ENKI_SB_API void enki_sb_free(enki_string_builder* sb) {
  const enki_allocator* allocator;

  if (!sb)
    return;

  allocator = sb->allocator;

  if (sb->pieces && sb->pieces != sb->inline_pieces && allocator->free)
    allocator->free(allocator->ctx, sb->pieces);

  sb->allocator = allocator;
  sb->pieces = sb->inline_pieces;
  sb->count = 0;
  sb->capacity = ENKI_SB_INLINE_PIECES;
  sb->failed = !enki_sb__allocator_ok(allocator);
}

ENKI_SB_API int enki_sb_failed(const enki_string_builder* sb) {
  return !sb || sb->failed;
}

ENKI_SB_API size_t enki_sb_piece_count(const enki_string_builder* sb) {
  return sb ? sb->count : 0u;
}

ENKI_SB_API int enki_sb_reserve_pieces(enki_string_builder* sb,
                                       size_t additional_piece_count) {
  if (!sb || sb->failed)
    return 0;

  if (additional_piece_count > SIZE_MAX - sb->count) {
    sb->failed = 1;
    return 0;
  }

  return enki_sb__grow(sb, sb->count + additional_piece_count);
}

ENKI_SB_API int enki_sb_append_ref(enki_string_builder* sb, const char* data,
                                   size_t len) {
  enki_sb_piece piece;

  if (!sb || sb->failed)
    return 0;

  if (len == 0)
    return 1;

  if (!data) {
    sb->failed = 1;
    return 0;
  }

  piece.kind = ENKI_SB_PIECE_REF;
  piece.base = 0;
  piece.uppercase = 0;
  piece._pad = 0;
  piece.u.ref.data = data;
  piece.u.ref.len = len;

  return enki_sb__push(sb, piece);
}

ENKI_SB_API int enki_sb_append_cstr(enki_string_builder* sb, const char* cstr) {
  if (!sb || sb->failed)
    return 0;

  if (!cstr) {
    sb->failed = 1;
    return 0;
  }

  return enki_sb_append_ref(sb, cstr, strlen(cstr));
}

ENKI_SB_API int enki_sb_append_char(enki_string_builder* sb, char c) {
  enki_sb_piece piece;

  if (!sb || sb->failed)
    return 0;

  piece.kind = ENKI_SB_PIECE_CHAR;
  piece.base = 0;
  piece.uppercase = 0;
  piece._pad = 0;
  piece.u.ch = c;

  return enki_sb__push(sb, piece);
}

ENKI_SB_API int enki_sb_append_i64(enki_string_builder* sb, int64_t value) {
  enki_sb_piece piece;

  if (!sb || sb->failed)
    return 0;

  piece.kind = ENKI_SB_PIECE_I64;
  piece.base = 10;
  piece.uppercase = 0;
  piece._pad = 0;
  piece.u.i64 = value;

  return enki_sb__push(sb, piece);
}

ENKI_SB_API int enki_sb_append_u64(enki_string_builder* sb, uint64_t value) {
  return enki_sb_append_u64_base(sb, value, 10, 0);
}

ENKI_SB_API int enki_sb_append_u64_base(enki_string_builder* sb, uint64_t value,
                                        unsigned base, int uppercase) {
  enki_sb_piece piece;

  if (!sb || sb->failed)
    return 0;

  if (base < 2 || base > 36) {
    sb->failed = 1;
    return 0;
  }

  piece.kind = ENKI_SB_PIECE_U64;
  piece.base = (unsigned char)base;
  piece.uppercase = uppercase ? 1u : 0u;
  piece._pad = 0;
  piece.u.u64 = value;

  return enki_sb__push(sb, piece);
}

ENKI_SB_API int enki_sb_append_builder(enki_string_builder* sb,
                                       const enki_string_builder* other) {
  size_t old_count;

  if (!sb || sb->failed)
    return 0;

  if (!other || other->failed) {
    sb->failed = 1;
    return 0;
  }

  if (other->count == 0)
    return 1;

  old_count = sb->count;

  if (!enki_sb_reserve_pieces(sb, other->count))
    return 0;

  /*
      memmove handles self-append:
          enki_sb_append_builder(&sb, &sb);
  */
  memmove(sb->pieces + old_count, other->pieces,
          other->count * sizeof(enki_sb_piece));

  sb->count = old_count + other->count;
  return 1;
}

ENKI_SB_API int enki_sb_measure(const enki_string_builder* sb,
                                size_t* out_len) {
  size_t total = 0;
  size_t i;

  if (out_len)
    *out_len = 0;

  if (!sb || sb->failed)
    return 0;

  for (i = 0; i < sb->count; ++i) {
    const enki_sb_piece* p = &sb->pieces[i];
    size_t len = 0;

    switch ((enki_sb_piece_kind)p->kind) {
    case ENKI_SB_PIECE_REF:
      len = p->u.ref.len;
      break;

    case ENKI_SB_PIECE_CHAR:
      len = 1;
      break;

    case ENKI_SB_PIECE_I64:
      len = enki_sb__i64_len(p->u.i64);
      break;

    case ENKI_SB_PIECE_U64:
      if (p->base < 2 || p->base > 36)
        return 0;
      len = enki_sb__u64_len(p->u.u64, p->base);
      break;

    default:
      return 0;
    }

    if (len > SIZE_MAX - total)
      return 0;

    total += len;
  }

  if (out_len)
    *out_len = total;

  return 1;
}

ENKI_SB_API int enki_sb_write(const enki_string_builder* sb, char* dst,
                              size_t dst_size, size_t* out_len) {
  size_t needed;
  size_t i;
  char* p;

  if (out_len)
    *out_len = 0;

  if (!enki_sb_measure(sb, &needed))
    return 0;

  if (out_len)
    *out_len = needed;

  if (needed == SIZE_MAX)
    return 0;

  if (!dst || dst_size < needed + 1u)
    return 0;

  p = dst;

  for (i = 0; i < sb->count; ++i) {
    const enki_sb_piece* piece = &sb->pieces[i];

    switch ((enki_sb_piece_kind)piece->kind) {
    case ENKI_SB_PIECE_REF:
      if (piece->u.ref.len) {
        memcpy(p, piece->u.ref.data, piece->u.ref.len);
        p += piece->u.ref.len;
      }
      break;

    case ENKI_SB_PIECE_CHAR:
      *p++ = piece->u.ch;
      break;

    case ENKI_SB_PIECE_I64:
      p = enki_sb__emit_i64(p, piece->u.i64);
      break;

    case ENKI_SB_PIECE_U64:
      p = enki_sb__emit_u64(p, piece->u.u64, piece->base, piece->uppercase);
      break;

    default:
      return 0;
    }
  }

  *p = '\0';
  return 1;
}

ENKI_SB_API char* enki_sb_build(const enki_string_builder* sb,
                                size_t* out_len) {
  if (!sb)
    return 0;

  return enki_sb_build_with_allocator(sb, sb->allocator, out_len);
}

ENKI_SB_API char* enki_sb_build_with_allocator(const enki_string_builder* sb,
                                               const enki_allocator* allocator,
                                               size_t* out_len) {
  size_t len;
  char* result;

  if (out_len)
    *out_len = 0;

  if (!enki_sb__allocator_ok(allocator))
    return 0;

  if (!enki_sb_measure(sb, &len))
    return 0;

  if (len == SIZE_MAX)
    return 0;

  result = (char*)allocator->alloc(allocator->ctx, len + 1u);
  if (!result)
    return 0;

  if (!enki_sb_write(sb, result, len + 1u, out_len)) {
    allocator->free(allocator->ctx, result);
    return 0;
  }

  return result;
}

#endif /* implementation */

#ifdef __cplusplus
}
#endif

#endif /* ENKI_STRING_BUILDER_H */
