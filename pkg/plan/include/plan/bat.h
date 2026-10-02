#ifndef PL_BAT_H
#define PL_BAT_H

#include "plan/heap.h"

/* Byte trees: existing CordTree nodes plus ["bat" length tree] and
 * ["range" tree offset length]. Slice sources may be nats or pins of nats.
 * Ranges must be in bounds; nat slices retain CordTree's zero padding.
 * A cursor validates lengths, stores O(leaf occurrences) descriptors, and
 * never flattens payloads. Borrowed values must not move during its lifetime.
 * For asynchronous use, open on a rooted store snapshot. */
typedef struct pl_bat_span {
  pl_val source; /* nat, or zero for a repeated byte */
  uint64_t offset, length;
  uint8_t byte;
} pl_bat_span;
typedef struct pl_bat_cursor {
  pl_bat_span* spans;
  size_t count, capacity, index;
  uint64_t length, position, within;
} pl_bat_cursor;

bool pl_bat_open(pl_bat_cursor* c, pl_val tree);
void pl_bat_close(pl_bat_cursor* c);
size_t pl_bat_read(pl_bat_cursor* c, uint8_t* out, size_t size);
bool pl_bat_seek(pl_bat_cursor* c, uint64_t offset);
/* Constructors root their arguments before allocating. File/HTTP producers
 * use bounded chunks; the resulting tree is balanced. */
#define PL_BAT_CHUNK_BYTES ((size_t)1 << 20)
pl_val pl_bat_chunk(pl_thread* t, const uint8_t* bytes, size_t size);
pl_val pl_bat_join(pl_thread* t, pl_val left, pl_val right);
pl_val pl_bat_from_bytes(pl_thread* t, const uint8_t* bytes, size_t size);

#endif
