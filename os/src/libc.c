#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "os/platform.h"

int errno;

static inline void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
  return value;
}

void os_serial_init(void) {
  outb(0x3f9, 0x00);
  outb(0x3fb, 0x80);
  outb(0x3f8, 0x01);
  outb(0x3f9, 0x00);
  outb(0x3fb, 0x03);
  outb(0x3fa, 0xc7);
  outb(0x3fc, 0x0b);
}

void os_serial_putc(char c) {
  if (c == '\n')
    os_serial_putc('\r');
  while ((inb(0x3fd) & 0x20) == 0)
    __asm__ volatile("pause");
  outb(0x3f8, (uint8_t)c);
}

int os_serial_getc(void) {
  while ((inb(0x3fd) & 1) == 0)
    __asm__ volatile("pause");
  return inb(0x3f8);
}

void os_serial_write(const void* data, size_t size) {
  const char* bytes = data;
  for (size_t i = 0; i < size; i++)
    os_serial_putc(bytes[i]);
}

void os_qemu_exit(uint32_t code) {
  __asm__ volatile("outl %0,%1" : : "a"(code), "Nd"((uint16_t)0xf4));
}

[[noreturn]] void os_halt(void) {
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

/* ── General-purpose heap ─────────────────────────────────────────────── */

#define HEAP_MAGIC UINT64_C(0x656e6b6968656170)

typedef struct heap_block {
  uint64_t magic;
  size_t size;
  struct heap_block* previous;
  struct heap_block* next;
  bool free;
  uint8_t padding[15];
} heap_block;

static_assert(sizeof(heap_block) % 16 == 0,
              "heap metadata must preserve max_align_t alignment");

static heap_block* heap_first;
static size_t heap_free_bytes;

static size_t align16(size_t size) {
  return (size + 15) & ~(size_t)15;
}

void os_memory_init(uintptr_t begin, uintptr_t end) {
  begin = (begin + 15) & ~(uintptr_t)15;
  end &= ~(uintptr_t)15;
  if (end <= begin + sizeof(heap_block))
    os_halt();
  heap_first = (heap_block*)begin;
  *heap_first = (heap_block){
      .magic = HEAP_MAGIC,
      .size = end - begin - sizeof(heap_block),
      .previous = NULL,
      .next = NULL,
      .free = true,
  };
  heap_free_bytes = heap_first->size;
}

size_t os_memory_available(void) {
  return heap_free_bytes;
}

static void split_block(heap_block* block, size_t size) {
  if (block->size < size + sizeof(heap_block) + 32)
    return;
  heap_block* tail = (heap_block*)((uint8_t*)(block + 1) + size);
  *tail = (heap_block){
      .magic = HEAP_MAGIC,
      .size = block->size - size - sizeof(heap_block),
      .previous = block,
      .next = block->next,
      .free = true,
  };
  if (tail->next != NULL)
    tail->next->previous = tail;
  block->next = tail;
  block->size = size;
  heap_free_bytes -= sizeof(heap_block);
}

void* malloc(size_t size) {
  if (size == 0)
    size = 1;
  if (size > SIZE_MAX - 15)
    return NULL;
  size = align16(size);
  for (heap_block* block = heap_first; block != NULL; block = block->next) {
    if (!block->free || block->size < size)
      continue;
    size_t before = block->size;
    split_block(block, size);
    block->free = false;
    heap_free_bytes -= block->size;
    (void)before;
    return block + 1;
  }
  return NULL;
}

void* calloc(size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count)
    return NULL;
  size_t total = count * size;
  void* out = malloc(total);
  if (out != NULL)
    memset(out, 0, total);
  return out;
}

static void merge_next(heap_block* block) {
  heap_block* next = block->next;
  if (next == NULL || !next->free)
    return;
  block->size += sizeof(heap_block) + next->size;
  block->next = next->next;
  if (block->next != NULL)
    block->next->previous = block;
  heap_free_bytes += sizeof(heap_block);
}

void free(void* pointer) {
  if (pointer == NULL)
    return;
  heap_block* block = (heap_block*)pointer - 1;
  if (block->magic != HEAP_MAGIC || block->free) {
    fprintf(stderr, "free: invalid pointer %p\n", pointer);
    abort();
  }
  block->free = true;
  heap_free_bytes += block->size;
  merge_next(block);
  if (block->previous != NULL && block->previous->free) {
    block = block->previous;
    merge_next(block);
  }
}

void* realloc(void* pointer, size_t size) {
  if (pointer == NULL)
    return malloc(size);
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  if (size > SIZE_MAX - 15)
    return NULL;
  size = align16(size);
  heap_block* block = (heap_block*)pointer - 1;
  if (block->magic != HEAP_MAGIC || block->free)
    abort();
  if (block->size >= size) {
    size_t old = block->size;
    split_block(block, size);
    heap_free_bytes += old - block->size;
    return pointer;
  }
  if (block->next != NULL && block->next->free &&
      block->size + sizeof(heap_block) + block->next->size >= size) {
    heap_block* next = block->next;
    heap_free_bytes -= next->size;
    block->size += sizeof(heap_block) + next->size;
    block->next = next->next;
    if (block->next != NULL)
      block->next->previous = block;
    size_t old = block->size;
    split_block(block, size);
    heap_free_bytes += old - block->size;
    return pointer;
  }
  void* out = malloc(size);
  if (out == NULL)
    return NULL;
  memcpy(out, pointer, block->size);
  free(pointer);
  return out;
}

/* ── Memory and strings ───────────────────────────────────────────────── */

void* memcpy(void* dst, const void* src, size_t n) {
  uint8_t* d = dst;
  const uint8_t* s = src;
  for (size_t i = 0; i < n; i++)
    d[i] = s[i];
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  uint8_t* d = dst;
  const uint8_t* s = src;
  if (d <= s) {
    for (size_t i = 0; i < n; i++)
      d[i] = s[i];
  } else {
    while (n != 0) {
      n--;
      d[n] = s[n];
    }
  }
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  uint8_t* d = dst;
  for (size_t i = 0; i < n; i++)
    d[i] = (uint8_t)c;
  return dst;
}

int memcmp(const void* av, const void* bv, size_t n) {
  const uint8_t* a = av;
  const uint8_t* b = bv;
  for (size_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return a[i] < b[i] ? -1 : 1;
  return 0;
}

size_t strlen(const char* s) {
  size_t n = 0;
  while (s[n] != '\0')
    n++;
  return n;
}

size_t strnlen(const char* s, size_t maxlen) {
  size_t n = 0;
  while (n < maxlen && s[n] != '\0')
    n++;
  return n;
}

int strcmp(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i] || a[i] == '\0')
      return (unsigned char)a[i] - (unsigned char)b[i];
  }
  return 0;
}

char* strcpy(char* dst, const char* src) {
  char* out = dst;
  while ((*dst++ = *src++) != '\0') {}
  return out;
}

char* strncpy(char* dst, const char* src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i] != '\0'; i++)
    dst[i] = src[i];
  for (; i < n; i++)
    dst[i] = '\0';
  return dst;
}

char* strchr(const char* s, int c) {
  do {
    if (*s == (char)c)
      return (char*)s;
  } while (*s++ != '\0');
  return NULL;
}

char* strrchr(const char* s, int c) {
  const char* found = NULL;
  do {
    if (*s == (char)c)
      found = s;
  } while (*s++ != '\0');
  return (char*)found;
}

char* strstr(const char* haystack, const char* needle) {
  size_t n = strlen(needle);
  if (n == 0)
    return (char*)haystack;
  for (; *haystack != '\0'; haystack++)
    if (strncmp(haystack, needle, n) == 0)
      return (char*)haystack;
  return NULL;
}

char* strdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* out = malloc(n);
  if (out != NULL)
    memcpy(out, s, n);
  return out;
}

char* strerror(int error) {
  switch (error) {
  case 0: return "success";
  case ENOENT: return "not found";
  case ENOMEM: return "out of memory";
  case EINVAL: return "invalid argument";
  case ENOSYS: return "unsupported";
  default: return "enki-os error";
  }
}

/* ── Character classes ────────────────────────────────────────────────── */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c) { return c >= 0x20 && c <= 0x7e; }
int isxdigit(int c) {
  return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
int toupper(int c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }

/* ── Formatting and FILE sink ─────────────────────────────────────────── */

static FILE stdin_file = {.kind = 0};
static FILE stdout_file = {.kind = 1};
static FILE stderr_file = {.kind = 2};
static FILE sink_file = {.kind = 3};
FILE* stdin = &stdin_file;
FILE* stdout = &stdout_file;
FILE* stderr = &stderr_file;

typedef struct format_out {
  char* dst;
  size_t cap;
  size_t used;
  FILE* stream;
} format_out;

static void emit_char(format_out* out, char c) {
  if (out->dst != NULL && out->used + 1 < out->cap)
    out->dst[out->used] = c;
  if (out->stream != NULL && out->stream->kind != 3)
    os_serial_putc(c);
  out->used++;
}

static void emit_repeat(format_out* out, char c, size_t n) {
  while (n-- != 0)
    emit_char(out, c);
}

static size_t unsigned_text(char buf[32], uint64_t value, unsigned base,
                            bool upper) {
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  size_t n = 0;
  do {
    buf[n++] = digits[value % base];
    value /= base;
  } while (value != 0);
  for (size_t i = 0; i < n / 2; i++) {
    char tmp = buf[i];
    buf[i] = buf[n - i - 1];
    buf[n - i - 1] = tmp;
  }
  return n;
}

static int format_core(format_out* out, const char* fmt, va_list ap) {
  while (*fmt != '\0') {
    if (*fmt != '%') {
      emit_char(out, *fmt++);
      continue;
    }
    fmt++;
    if (*fmt == '%') {
      emit_char(out, *fmt++);
      continue;
    }
    bool left = false;
    bool zero = false;
    bool alternate = false;
    for (;;) {
      if (*fmt == '-') left = true;
      else if (*fmt == '0') zero = true;
      else if (*fmt == '#') alternate = true;
      else if (*fmt == '+' || *fmt == ' ') {}
      else break;
      fmt++;
    }
    int width = 0;
    if (*fmt == '*') {
      width = va_arg(ap, int);
      fmt++;
    } else {
      while (isdigit((unsigned char)*fmt))
        width = width * 10 + (*fmt++ - '0');
    }
    int precision = -1;
    if (*fmt == '.') {
      fmt++;
      precision = 0;
      if (*fmt == '*') {
        precision = va_arg(ap, int);
        fmt++;
      } else {
        while (isdigit((unsigned char)*fmt))
          precision = precision * 10 + (*fmt++ - '0');
      }
    }
    enum { LEN_INT, LEN_LONG, LEN_LLONG, LEN_SIZE, LEN_PTRDIFF } length = LEN_INT;
    if (*fmt == 'z') { length = LEN_SIZE; fmt++; }
    else if (*fmt == 't') { length = LEN_PTRDIFF; fmt++; }
    else if (*fmt == 'l') {
      fmt++;
      if (*fmt == 'l') { length = LEN_LLONG; fmt++; }
      else length = LEN_LONG;
    }
    char conv = *fmt != '\0' ? *fmt++ : '\0';
    if (conv == 's') {
      const char* s = va_arg(ap, const char*);
      if (s == NULL) s = "(null)";
      size_t n = strlen(s);
      if (precision >= 0 && n > (size_t)precision) n = (size_t)precision;
      if (!left && width > (int)n) emit_repeat(out, ' ', (size_t)width - n);
      for (size_t i = 0; i < n; i++) emit_char(out, s[i]);
      if (left && width > (int)n) emit_repeat(out, ' ', (size_t)width - n);
      continue;
    }
    if (conv == 'c') {
      emit_char(out, (char)va_arg(ap, int));
      continue;
    }
    bool signed_value = conv == 'd' || conv == 'i';
    unsigned base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16u : 10u;
    uint64_t value;
    bool negative = false;
    if (conv == 'p') {
      value = (uintptr_t)va_arg(ap, void*);
      alternate = true;
    } else if (signed_value) {
      int64_t v;
      if (length == LEN_LLONG) v = va_arg(ap, long long);
      else if (length == LEN_LONG) v = va_arg(ap, long);
      else if (length == LEN_SIZE || length == LEN_PTRDIFF) v = va_arg(ap, ptrdiff_t);
      else v = va_arg(ap, int);
      negative = v < 0;
      value = negative ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    } else if (conv == 'u' || conv == 'x' || conv == 'X') {
      if (length == LEN_LLONG) value = va_arg(ap, unsigned long long);
      else if (length == LEN_LONG) value = va_arg(ap, unsigned long);
      else if (length == LEN_SIZE || length == LEN_PTRDIFF) value = va_arg(ap, size_t);
      else value = va_arg(ap, unsigned);
    } else {
      emit_char(out, '%');
      if (conv != '\0') emit_char(out, conv);
      continue;
    }
    char digits[32];
    size_t n = unsigned_text(digits, value, base, conv == 'X');
    size_t prefix = negative ? 1 : (alternate && base == 16 ? 2 : 0);
    size_t zeroes = precision > (int)n ? (size_t)precision - n : 0;
    size_t total = prefix + zeroes + n;
    if (!left && width > (int)total && !zero)
      emit_repeat(out, ' ', (size_t)width - total);
    if (negative) emit_char(out, '-');
    else if (prefix == 2) { emit_char(out, '0'); emit_char(out, 'x'); }
    if (!left && width > (int)total && zero && precision < 0)
      emit_repeat(out, '0', (size_t)width - total);
    emit_repeat(out, '0', zeroes);
    for (size_t i = 0; i < n; i++) emit_char(out, digits[i]);
    if (left && width > (int)total)
      emit_repeat(out, ' ', (size_t)width - total);
  }
  if (out->dst != NULL && out->cap != 0) {
    size_t at = out->used < out->cap ? out->used : out->cap - 1;
    out->dst[at] = '\0';
  }
  return out->used > INT32_MAX ? INT32_MAX : (int)out->used;
}

int vsnprintf(char* dst, size_t cap, const char* fmt, va_list ap) {
  format_out out = {.dst = dst, .cap = cap};
  va_list copy;
  va_copy(copy, ap);
  int rc = format_core(&out, fmt, copy);
  va_end(copy);
  return rc;
}

int snprintf(char* dst, size_t cap, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vsnprintf(dst, cap, fmt, ap);
  va_end(ap);
  return rc;
}

int vfprintf(FILE* stream, const char* fmt, va_list ap) {
  format_out out = {.stream = stream};
  va_list copy;
  va_copy(copy, ap);
  int rc = format_core(&out, fmt, copy);
  va_end(copy);
  return rc;
}

int fprintf(FILE* stream, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vfprintf(stream, fmt, ap);
  va_end(ap);
  return rc;
}

int printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return rc;
}

int fputc(int c, FILE* stream) {
  if (stream->kind != 3)
    os_serial_putc((char)c);
  return (unsigned char)c;
}
int putchar(int c) { return fputc(c, stdout); }
int puts(const char* s) { fprintf(stdout, "%s\n", s); return 0; }
int fflush(FILE* stream) { (void)stream; return 0; }

FILE* fopen(const char* path, const char* mode) {
  (void)path;
  (void)mode;
  return &sink_file;
}
int fclose(FILE* stream) { (void)stream; return 0; }
size_t fread(void* ptr, size_t size, size_t count, FILE* stream) {
  (void)ptr; (void)size; (void)count; (void)stream; return 0;
}
size_t fwrite(const void* ptr, size_t size, size_t count, FILE* stream) {
  if (size != 0 && count > SIZE_MAX / size) return 0;
  size_t bytes = size * count;
  if (stream->kind != 3) os_serial_write(ptr, bytes);
  return count;
}
int fseek(FILE* stream, long offset, int whence) {
  (void)stream; (void)offset; (void)whence; errno = ENOSYS; return -1;
}
long ftell(FILE* stream) { (void)stream; errno = ENOSYS; return -1; }
int feof(FILE* stream) { (void)stream; return 1; }
int ferror(FILE* stream) { return stream->error; }
void clearerr(FILE* stream) { stream->error = 0; }
void perror(const char* prefix) { fprintf(stderr, "%s: %s\n", prefix, strerror(errno)); }

/* ── Miscellaneous hosted compatibility ───────────────────────────────── */

[[noreturn]] void abort(void) {
  fprintf(stderr, "abort\n");
  os_qemu_exit(127);
  os_halt();
}

[[noreturn]] void exit(int status) {
  os_qemu_exit((uint32_t)status);
  os_halt();
}

[[noreturn]] void __assert_fail(const char* expr, const char* file,
                                unsigned line, const char* function) {
  fprintf(stderr, "%s:%u: %s: assertion `%s' failed\n", file, line,
          function, expr);
  abort();
}

int atexit(void (*fn)(void)) { (void)fn; return 0; }
char* getenv(const char* name) { (void)name; return NULL; }

long strtol(const char* s, char** end, int base) {
  bool neg = false;
  while (isspace((unsigned char)*s)) s++;
  if (*s == '-' || *s == '+') neg = *s++ == '-';
  unsigned long value = strtoul(s, end, base);
  return neg ? -(long)value : (long)value;
}

unsigned long strtoul(const char* s, char** end, int base) {
  while (isspace((unsigned char)*s)) s++;
  if (base == 0) base = 10;
  unsigned long value = 0;
  const char* first = s;
  for (;;) {
    int digit;
    if (isdigit((unsigned char)*s)) digit = *s - '0';
    else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
    else break;
    if (digit >= base) break;
    value = value * (unsigned)base + (unsigned)digit;
    s++;
  }
  if (end != NULL) *end = (char*)(s == first ? first : s);
  return value;
}

double strtod(const char* s, char** end) {
  while (isspace((unsigned char)*s)) s++;
  if (*s == '-' || *s == '+') s++;
  while (isdigit((unsigned char)*s)) s++;
  if (*s == '.') {
    s++;
    while (isdigit((unsigned char)*s)) s++;
  }
  if (end != NULL) *end = (char*)s;
  /* Only profiling/host-option code asks for doubles; it is disabled here. */
  return 0.0;
}

static void byte_swap(uint8_t* a, uint8_t* b, size_t size) {
  for (size_t i = 0; i < size; i++) { uint8_t t = a[i]; a[i] = b[i]; b[i] = t; }
}

static void qsort_range(uint8_t* base, size_t count, size_t size,
                        int (*compare)(const void*, const void*)) {
  while (count > 16) {
    size_t pivot = count / 2;
    byte_swap(base + pivot * size, base + (count - 1) * size, size);
    size_t split = 0;
    for (size_t i = 0; i + 1 < count; i++)
      if (compare(base + i * size, base + (count - 1) * size) < 0) {
        byte_swap(base + i * size, base + split * size, size);
        split++;
      }
    byte_swap(base + split * size, base + (count - 1) * size, size);
    if (split < count - split - 1) {
      qsort_range(base, split, size, compare);
      base += (split + 1) * size;
      count -= split + 1;
    } else {
      qsort_range(base + (split + 1) * size, count - split - 1, size, compare);
      count = split;
    }
  }
  for (size_t i = 1; i < count; i++) {
    size_t j = i;
    while (j > 0 && compare(base + j * size, base + (j - 1) * size) < 0) {
      byte_swap(base + j * size, base + (j - 1) * size, size);
      j--;
    }
  }
}

void qsort(void* base, size_t count, size_t size,
           int (*compare)(const void*, const void*)) {
  if (size != 0 && count > 1) qsort_range(base, count, size, compare);
}

int mkdir(const char* path, mode_t mode) { (void)path; (void)mode; return 0; }
int stat(const char* path, struct stat* st) { (void)path; (void)st; errno = ENOENT; return -1; }
int lstat(const char* path, struct stat* st) { return stat(path, st); }
int fstat(int fd, struct stat* st) { (void)fd; (void)st; errno = EBADF; return -1; }
int access(const char* path, int mode) {
  (void)mode;
  /* Save's host-side canonical mirror is an intentional write sink.  Report
   * those objects as already mirrored so every serial resume does not spend
   * time regenerating bytes that the guest cannot persist. */
  if (path != NULL &&
      (strncmp(path, "snap/", 5) == 0 || strncmp(path, "./snap/", 7) == 0))
    return 0;
  errno = ENOENT;
  return -1;
}
int open(const char* path, int flags, ...) { (void)path; (void)flags; errno = ENOSYS; return -1; }
int close(int fd) { (void)fd; errno = EBADF; return -1; }
ssize_t read(int fd, void* buf, size_t count) {
  if (fd != 0) { errno = EBADF; return -1; }
  uint8_t* bytes = buf;
  for (size_t i = 0; i < count; i++) bytes[i] = (uint8_t)os_serial_getc();
  return (ssize_t)count;
}
ssize_t write(int fd, const void* buf, size_t count) {
  if (fd != 1 && fd != 2) { errno = EBADF; return -1; }
  os_serial_write(buf, count);
  return (ssize_t)count;
}
int isatty(int fd) { return fd >= 0 && fd <= 2; }
unsigned sleep(unsigned seconds) { (void)seconds; return 0; }

static uint64_t fake_clock;
int clock_gettime(int clock_id, struct timespec* ts) {
  (void)clock_id;
  if (ts == NULL) { errno = EINVAL; return -1; }
  fake_clock += 1000;
  ts->tv_sec = (time_t)(fake_clock / UINT64_C(1000000000));
  ts->tv_nsec = (long)(fake_clock % UINT64_C(1000000000));
  return 0;
}
int nanosleep(const struct timespec* req, struct timespec* rem) {
  (void)req; (void)rem; return 0;
}

uint64_t os_fnv1a64(const void* data, size_t size) {
  const uint8_t* bytes = data;
  uint64_t value = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < size; i++) {
    value ^= bytes[i];
    value *= UINT64_C(1099511628211);
  }
  return value;
}
