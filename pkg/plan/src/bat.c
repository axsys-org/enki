#include "plan/bat.h"

#include <stdlib.h>
#include <string.h>
#include "axsys/util.h"
#include "plan/build.h"
#include "plan/nat.h"
#include "plan/store.h"

static bool bat_size(pl_val v, uint64_t* n) {
  if (!pl_is_nat(v) || pl_nat_limb_len(v) > 1)
    return false;
  *n = pl_nat_limb_at(v, 0);
  return true;
}

static bool bat_grow(void** p, size_t* cap, size_t need, size_t width) {
  if (need <= *cap)
    return true;
  size_t next = *cap ? *cap : 16;
  while (next < need) {
    if (next > SIZE_MAX / 2)
      return false;
    next *= 2;
  }
  if (next > SIZE_MAX / width)
    return false;
  void* q = realloc(*p, next * width);
  if (!q)
    return false;
  *p = q;
  *cap = next;
  return true;
}

static bool bat_span(pl_bat_cursor* c, pl_val source, uint64_t off,
                     uint64_t len, uint8_t byte) {
  if (!len)
    return true;
  if (len > UINT64_MAX - c->length || c->count == SIZE_MAX ||
      !bat_grow((void**)&c->spans, &c->capacity, c->count + 1,
                sizeof(*c->spans)))
    return false;
  c->spans[c->count++] = (pl_bat_span){source, off, len, byte};
  c->length += len;
  return true;
}

typedef struct bat_task {
  pl_val node;
  uint64_t start, offset, length;
  size_t first;
  unsigned finish; /* 1: check wrapper length, 2: crop a range */
} bat_task;

bool pl_bat_open(pl_bat_cursor* c, pl_val tree) {
  memset(c, 0, sizeof(*c));
  bat_task* stack = NULL;
  size_t count = 0, cap = 0;
#define PUSH(task)                                                             \
  do {                                                                         \
    if (!bat_grow((void**)&stack, &cap, count + 1, sizeof(*stack)))            \
      goto bad;                                                                \
    stack[count++] = (task);                                                   \
  } while (0)
  PUSH(((bat_task){.node = tree}));
  while (count) {
    bat_task task = stack[--count];
    if (task.finish) {
      uint64_t size = c->length - task.start;
      if (task.finish == 1) {
        if (size != task.length)
          goto bad;
      } else {
        if (task.offset > size || task.length > size - task.offset)
          goto bad;
        size_t end = c->count;
        c->count = task.first;
        c->length = task.start;
        uint64_t skip = task.offset, remain = task.length;
        for (size_t i = task.first; i < end && remain; i++) {
          pl_bat_span s = c->spans[i];
          if (skip >= s.length) {
            skip -= s.length;
            continue;
          }
          s.offset += skip;
          s.length -= skip;
          skip = 0;
          if (s.length > remain)
            s.length = remain;
          c->spans[c->count++] = s;
          c->length += s.length;
          remain -= s.length;
        }
      }
      continue;
    }
    pl_cell* p = pl_as(PL_TAG_APP, task.node);
    if (!p || pl_app_head(p) != 0 || !pl_app_n(p))
      goto bad;
    pl_val* f = pl_app_args(p);
    uint32_t n = pl_app_n(p);
    uint64_t off = 0, len;
    if (n == 3 && f[0] == ax_s3('c', 'a', 't')) {
      PUSH(((bat_task){.node = f[2]}));
      PUSH(((bat_task){.node = f[1]}));
    } else if ((n == 3 && f[0] == ax_s3('b', 'a', 't')) ||
               (n == 4 && f[0] == ax_s5('r', 'a', 'n', 'g', 'e'))) {
      bool range = n == 4;
      if (!bat_size(f[range ? 3 : 1], &len) || (range && !bat_size(f[2], &off)))
        goto bad;
      PUSH(((bat_task){.start = c->length,
                       .first = c->count,
                       .length = len,
                       .offset = range ? off : 0,
                       .finish = range ? 2 : 1}));
      PUSH(((bat_task){.node = f[range ? 1 : 2]}));
    } else if (n == 3 && f[0] == ax_s6('r', 'e', 'p', 'e', 'a', 't')) {
      if (!pl_is_nat(f[1]) || !bat_size(f[2], &len) ||
          !bat_span(c, 0, 0, len, pl_nat_byte_at(f[1], 0)))
        goto bad;
    } else if ((n == 2 && f[0] == ax_s4('t', 'e', 'x', 't')) ||
               (n == 4 && f[0] == ax_s5('s', 'l', 'i', 'c', 'e'))) {
      pl_val src = f[1];
      if (pl_tag(src) == PL_TAG_PIN)
        src = pl_pin_body(pl_ptr(src));
      if (!pl_is_nat(src))
        goto bad;
      uint64_t available = pl_nat_byte_len(src);
      off = 0;
      len = available;
      if (n == 4 && (!bat_size(f[2], &off) || !bat_size(f[3], &len)))
        goto bad;
      available = off < available ? available - off : 0;
      if (available > len)
        available = len;
      if (!bat_span(c, src, off, available, 0) ||
          !bat_span(c, 0, 0, len - available, 0))
        goto bad;
    } else
      goto bad;
  }
  free(stack);
  return true;
bad:
  free(stack);
  pl_bat_close(c);
  return false;
#undef PUSH
}

void pl_bat_close(pl_bat_cursor* c) {
  free(c->spans);
  memset(c, 0, sizeof(*c));
}

bool pl_bat_seek(pl_bat_cursor* c, uint64_t offset) {
  if (offset > c->length)
    return false;
  c->position = offset;
  c->index = 0;
  while (c->index < c->count && offset >= c->spans[c->index].length)
    offset -= c->spans[c->index++].length;
  c->within = offset;
  return true;
}

size_t pl_bat_read(pl_bat_cursor* c, uint8_t* out, size_t size) {
  size_t done = 0;
  while (done < size && c->index < c->count) {
    pl_bat_span* s = &c->spans[c->index];
    uint64_t remain = s->length - c->within;
    size_t n = remain < size - done ? (size_t)remain : size - done;
    if (s->source == 0)
      memset(out + done, s->byte, n);
    else if (pl_is_nat63(s->source)) {
      for (size_t i = 0; i < n; i++)
        out[done + i] =
            pl_nat_byte_at(s->source, (size_t)(s->offset + c->within) + i);
    } else {
      memcpy(out + done,
             (uint8_t*)pl_nat_limb_ptr(pl_ptr(s->source)) + s->offset +
                 c->within,
             n);
    }
    done += n;
    c->within += n;
    c->position += n;
    if (c->within == s->length) {
      c->index++;
      c->within = 0;
    }
  }
  return done;
}

/* Build a row using rooted fields already on the stack. */
static pl_val bat_row(pl_thread* t, size_t base, uint32_t n) {
  pl_gc_reserve(t, PL_APP_CELLS(n));
  return pl_mk_app_from(t, 0, n, t->vstack + base);
}

pl_val pl_bat_chunk(pl_thread* t, const uint8_t* bytes, size_t size) {
  ax_assume(size <= PL_BAT_CHUNK_BYTES, "BAT chunk too large");
  size_t base = t->vsp;
  pl_vpush(t, ax_s3('b', 'a', 't'));
  pl_vpush(t, size);
  pl_vpush(t, ax_s5('s', 'l', 'i', 'c', 'e'));
  pl_vpush(t, pl_nat_from_bytes(t, bytes, size));
  t->vstack[base + 3] = pl_pin(t, t->vstack[base + 3]);
  pl_vpush(t, 0);
  pl_vpush(t, size);
  pl_val leaf = bat_row(t, base + 2, 4);
  t->vsp = base + 2;
  pl_vpush(t, leaf);
  pl_val out = bat_row(t, base, 3);
  t->vsp = base;
  return out;
}

pl_val pl_bat_join(pl_thread* t, pl_val left, pl_val right) {
  size_t base = t->vsp;
  pl_vpush(t, left);
  pl_vpush(t, right);
  uint64_t a, b;
  ax_assume(bat_size(pl_app_args(pl_ptr(left))[1], &a) &&
                bat_size(pl_app_args(pl_ptr(right))[1], &b) &&
                a <= UINT64_MAX - b,
            "BAT length overflow");
  pl_vpush(t, ax_s3('b', 'a', 't'));
  pl_gc_reserve(t, PL_NAT_CELLS(1));
  pl_vpush(t, pl_mk_nat_u64(t, a + b));
  pl_vpush(t, ax_s3('c', 'a', 't'));
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, t->vstack[base + 1]);
  pl_val cat = bat_row(t, base + 4, 3);
  t->vsp = base + 4;
  pl_vpush(t, cat);
  pl_val out = bat_row(t, base + 2, 3);
  t->vsp = base;
  return out;
}

pl_val pl_bat_from_bytes(pl_thread* t, const uint8_t* bytes, size_t size) {
  size_t base = t->vsp;
  do {
    size_t n = size < PL_BAT_CHUNK_BYTES ? size : PL_BAT_CHUNK_BYTES;
    pl_vpush(t, pl_bat_chunk(t, bytes, n));
    if (n)
      bytes += n;
    size -= n;
  } while (size);
  while (t->vsp - base > 1) {
    size_t end = t->vsp, dst = base;
    for (size_t i = base; i < end; i += 2) {
      pl_val v = t->vstack[i];
      if (i + 1 < end)
        v = pl_bat_join(t, v, t->vstack[i + 1]);
      t->vstack[dst++] = v;
    }
    t->vsp = dst;
  }
  pl_val out = t->vstack[base];
  t->vsp = base;
  return out;
}
