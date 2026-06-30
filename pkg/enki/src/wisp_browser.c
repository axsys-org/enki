#ifdef ENKI_WASM

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "axsys/allocator.h"
#include "enki/actor.h"
#include "plan/wasm_io.h"

#define ENKI_WISP_EMBEDDED 1
#include "../app/wisp.c"

#define WISP_EXPORT __attribute__((visibility("default")))

WISP_EXPORT void* wisp_alloc(size_t size);
WISP_EXPORT void wisp_free(void* ptr, size_t size);
WISP_EXPORT void wisp_reset(void);
WISP_EXPORT void wisp_clear_files(void);
WISP_EXPORT int wisp_mount_file(const char* path, size_t path_len,
                                const uint8_t* bytes, size_t bytes_len,
                                uint64_t mtime);
WISP_EXPORT void wisp_set_input(const uint8_t* bytes, size_t len);
WISP_EXPORT void wisp_set_file_root(const char* path, size_t len);
WISP_EXPORT void wisp_set_now(uint64_t now_s);
WISP_EXPORT void wisp_set_emit_top_level(int enabled);
WISP_EXPORT int wisp_run(const char* src_dir_c, const char* mod_c,
                         const char* fn_c, int argc, char** argv);
WISP_EXPORT const uint8_t* wisp_output_ptr(int channel);
WISP_EXPORT size_t wisp_output_len(int channel);
WISP_EXPORT const uint8_t* wisp_error_ptr(void);
WISP_EXPORT size_t wisp_error_len(void);
WISP_EXPORT size_t wisp_file_count(void);
WISP_EXPORT const char* wisp_file_path_ptr(size_t i);
WISP_EXPORT size_t wisp_file_path_len(size_t i);
WISP_EXPORT const uint8_t* wisp_file_data_ptr(size_t i);
WISP_EXPORT size_t wisp_file_data_len(size_t i);
WISP_EXPORT int wisp_file_written(size_t i);
WISP_EXPORT void wisp_dispose(void);

typedef struct browser_buf {
  uint8_t* data;
  size_t len;
  size_t cap;
} browser_buf;

typedef struct browser_file {
  char* path;
  uint8_t* data;
  size_t len;
  uint64_t mtime;
  bool written;
} browser_file;

typedef struct browser_state {
  browser_file* files;
  size_t file_len;
  size_t file_cap;
  browser_buf input;
  size_t input_pos;
  browser_buf stdout_buf;
  browser_buf stderr_buf;
  browser_buf error_buf;
  char* file_root;
  uint64_t now_s;
  bool emit_top_level_f;
} browser_state;

static browser_state g_browser = {.now_s = 0, .emit_top_level_f = true};

static void buf_clear(browser_buf* b) {
  b->len = 0;
  if (b->data != NULL)
    b->data[0] = 0;
}

static void buf_free(browser_buf* b) {
  free(b->data);
  *b = (browser_buf){0};
}

static void buf_reserve(browser_buf* b, size_t add) {
  size_t need = b->len + add + 1;
  if (need <= b->cap)
    return;
  size_t cap = b->cap == 0 ? 256 : b->cap;
  while (cap < need)
    cap *= 2;
  b->data = realloc(b->data, cap);
  ax_assume(b->data != NULL, "oom");
  b->cap = cap;
}

static void buf_append(browser_buf* b, const uint8_t* data, size_t len) {
  buf_reserve(b, len);
  if (len > 0)
    memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = 0;
}

static void buf_set(browser_buf* b, const uint8_t* data, size_t len) {
  b->len = 0;
  buf_append(b, data, len);
}

static char* browser_strndup(const char* s, size_t n) {
  char* out = malloc(n + 1);
  ax_assume(out != NULL, "oom");
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static char* normalize_path(const char* path, bool* escaped) {
  *escaped = false;
  if (path == NULL)
    path = "";
  size_t n = strlen(path);
  char* tmp = browser_strndup(path, n);
  for (size_t i = 0; i < n; i++)
    if (tmp[i] == '\\')
      tmp[i] = '/';

  char** segs = malloc((n + 1) * sizeof(*segs));
  ax_assume(segs != NULL, "oom");
  size_t seg_n = 0;
  char* cur = tmp;
  while (*cur == '/')
    cur++;
  while (*cur != '\0') {
    char* start = cur;
    while (*cur != '\0' && *cur != '/')
      cur++;
    if (*cur == '/') {
      *cur = '\0';
      cur++;
    }
    while (*cur == '/')
      cur++;
    if (start[0] == '\0' || strcmp(start, ".") == 0)
      continue;
    if (strcmp(start, "..") == 0) {
      if (seg_n == 0)
        *escaped = true;
      else
        seg_n--;
      continue;
    }
    segs[seg_n++] = start;
  }

  size_t out_n = 0;
  for (size_t i = 0; i < seg_n; i++)
    out_n += strlen(segs[i]) + (i == 0 ? 0 : 1);
  char* out = malloc(out_n + 1);
  ax_assume(out != NULL, "oom");
  char* dst = out;
  for (size_t i = 0; i < seg_n; i++) {
    if (i != 0)
      *dst++ = '/';
    size_t len = strlen(segs[i]);
    memcpy(dst, segs[i], len);
    dst += len;
  }
  *dst = '\0';
  free(segs);
  free(tmp);
  return out;
}

static bool path_has_prefix(const char* root, const char* path) {
  if (root == NULL || root[0] == '\0')
    return true;
  size_t n = strlen(root);
  return strcmp(root, path) == 0 ||
         (strncmp(root, path, n) == 0 && path[n] == '/');
}

static char* resolve_virtual_path(const char* root, const char* path,
                                  bool* ok) {
  bool root_esc = false;
  bool path_esc = false;
  char* root_norm = normalize_path(root == NULL ? "" : root, &root_esc);
  size_t rn = strlen(root_norm);
  size_t pn = strlen(path == NULL ? "" : path);
  char* joined = malloc(rn + (rn == 0 ? 0 : 1) + pn + 1);
  ax_assume(joined != NULL, "oom");
  if (rn == 0)
    memcpy(joined, path == NULL ? "" : path, pn + 1);
  else
    (void)sprintf(joined, "%s/%s", root_norm, path == NULL ? "" : path);
  char* full = normalize_path(joined, &path_esc);
  *ok = !root_esc && !path_esc && path_has_prefix(root_norm, full);
  free(root_norm);
  free(joined);
  if (!*ok) {
    free(full);
    return NULL;
  }
  return full;
}

static browser_file* find_file(const char* path) {
  for (size_t i = 0; i < g_browser.file_len; i++)
    if (strcmp(g_browser.files[i].path, path) == 0)
      return &g_browser.files[i];
  return NULL;
}

static browser_file* upsert_file(const char* path) {
  browser_file* f = find_file(path);
  if (f != NULL)
    return f;
  if (g_browser.file_len == g_browser.file_cap) {
    size_t cap = g_browser.file_cap == 0 ? 32 : g_browser.file_cap * 2;
    g_browser.files = realloc(g_browser.files, cap * sizeof(*g_browser.files));
    ax_assume(g_browser.files != NULL, "oom");
    g_browser.file_cap = cap;
  }
  f = &g_browser.files[g_browser.file_len++];
  *f = (browser_file){.path = browser_strndup(path, strlen(path))};
  return f;
}

static void browser_emit(void* ctx, int channel, const char* bytes,
                         size_t len) {
  AX_UNUSED(ctx);
  browser_buf* out =
      channel == 1 ? &g_browser.stdout_buf : &g_browser.stderr_buf;
  buf_append(out, (const uint8_t*)bytes, len);
}

static char* browser_read_boot_file(void* ctx, const ax_allocator* a,
                                    const char* path_c) {
  AX_UNUSED(ctx);
  bool esc = false;
  char* norm = normalize_path(path_c, &esc);
  browser_file* f = esc ? NULL : find_file(norm);
  free(norm);
  if (f == NULL) {
    boot_emitf(2, "wisp: failed to open %s\n", path_c);
    return NULL;
  }
  char* out = ax_calloc(a, char, f->len + 1);
  if (out == NULL)
    return NULL;
  memcpy(out, f->data, f->len);
  out[f->len] = '\0';
  return out;
}

static size_t wasm_input(void* ctx, uint8_t* out, size_t max) {
  AX_UNUSED(ctx);
  size_t avail = g_browser.input.len - g_browser.input_pos;
  if (avail > max)
    avail = max;
  if (avail > 0) {
    memcpy(out, g_browser.input.data + g_browser.input_pos, avail);
    g_browser.input_pos += avail;
  }
  return avail;
}

static void wasm_output(void* ctx, pl_wasm_io_channel channel,
                        const uint8_t* bytes, size_t len) {
  AX_UNUSED(ctx);
  browser_buf* out = channel == PL_WASM_IO_STDERR ? &g_browser.stderr_buf
                                                  : &g_browser.stdout_buf;
  buf_append(out, bytes, len);
}

static bool wasm_read_file(void* ctx, const char* root, const char* path,
                           uint8_t** out_bytes, size_t* out_len) {
  AX_UNUSED(ctx);
  bool ok = false;
  char* full = resolve_virtual_path(root, path, &ok);
  if (!ok)
    return false;
  browser_file* f = find_file(full);
  free(full);
  if (f == NULL)
    return false;
  uint8_t* out = malloc(f->len ? f->len : 1);
  ax_assume(out != NULL, "oom");
  if (f->len > 0)
    memcpy(out, f->data, f->len);
  *out_bytes = out;
  *out_len = f->len;
  return true;
}

static bool wasm_write_file(void* ctx, const char* root, const char* path,
                            const uint8_t* bytes, size_t len) {
  AX_UNUSED(ctx);
  bool ok = false;
  char* full = resolve_virtual_path(root, path, &ok);
  if (!ok)
    return false;
  browser_file* f = upsert_file(full);
  free(full);
  free(f->data);
  f->data = malloc(len ? len : 1);
  ax_assume(f->data != NULL, "oom");
  if (len > 0)
    memcpy(f->data, bytes, len);
  f->len = len;
  f->mtime = g_browser.now_s;
  f->written = true;
  return true;
}

static bool wasm_stamp(void* ctx, const char* root, const char* path,
                       uint64_t* out_mtime) {
  AX_UNUSED(ctx);
  bool ok = false;
  char* full = resolve_virtual_path(root, path, &ok);
  if (!ok)
    return false;
  browser_file* f = find_file(full);
  free(full);
  if (f == NULL)
    return false;
  *out_mtime = f->mtime;
  return true;
}

static uint64_t wasm_now(void* ctx) {
  AX_UNUSED(ctx);
  return g_browser.now_s;
}

static const pl_wasm_io browser_io = {
    .ctx = &g_browser,
    .input = wasm_input,
    .output = wasm_output,
    .read_file = wasm_read_file,
    .write_file = wasm_write_file,
    .stamp = wasm_stamp,
    .now = wasm_now,
};

WISP_EXPORT void* wisp_alloc(size_t size) {
  return malloc(size ? size : 1);
}

WISP_EXPORT void wisp_free(void* ptr, size_t size) {
  AX_UNUSED(size);
  free(ptr);
}

WISP_EXPORT void wisp_reset(void) {
  buf_clear(&g_browser.stdout_buf);
  buf_clear(&g_browser.stderr_buf);
  buf_clear(&g_browser.error_buf);
  g_browser.input_pos = 0;
}

WISP_EXPORT void wisp_clear_files(void) {
  for (size_t i = 0; i < g_browser.file_len; i++) {
    free(g_browser.files[i].path);
    free(g_browser.files[i].data);
  }
  free(g_browser.files);
  g_browser.files = NULL;
  g_browser.file_len = 0;
  g_browser.file_cap = 0;
}

WISP_EXPORT int wisp_mount_file(const char* path, size_t path_len,
                                const uint8_t* bytes, size_t bytes_len,
                                uint64_t mtime) {
  char* raw = browser_strndup(path, path_len);
  bool esc = false;
  char* norm = normalize_path(raw, &esc);
  free(raw);
  if (esc) {
    free(norm);
    return 1;
  }
  browser_file* f = upsert_file(norm);
  free(norm);
  free(f->data);
  f->data = malloc(bytes_len ? bytes_len : 1);
  ax_assume(f->data != NULL, "oom");
  if (bytes_len > 0)
    memcpy(f->data, bytes, bytes_len);
  f->len = bytes_len;
  f->mtime = mtime;
  f->written = false;
  return 0;
}

WISP_EXPORT void wisp_set_input(const uint8_t* bytes, size_t len) {
  buf_set(&g_browser.input, bytes, len);
  g_browser.input_pos = 0;
}

WISP_EXPORT void wisp_set_file_root(const char* path, size_t len) {
  free(g_browser.file_root);
  char* raw = browser_strndup(path, len);
  bool esc = false;
  g_browser.file_root = normalize_path(raw, &esc);
  free(raw);
  if (esc) {
    free(g_browser.file_root);
    g_browser.file_root = browser_strndup("", 0);
  }
}

WISP_EXPORT void wisp_set_now(uint64_t now_s) {
  g_browser.now_s = now_s;
}

WISP_EXPORT void wisp_set_emit_top_level(int enabled) {
  g_browser.emit_top_level_f = enabled != 0;
}

WISP_EXPORT int wisp_run(const char* src_dir_c, const char* mod_c,
                         const char* fn_c, int argc, char** argv) {
  wisp_reset();
  pl_wasm_io_set(&browser_io);
  boot_io_set(&g_browser, browser_read_boot_file, browser_emit);

  int rc = 1;
  pl_store* store = NULL;
  pl_heap* heap = NULL;
  en_wisp* w = NULL;
  er_scheduler* sched = NULL;
  boot_ctx ctx = {0};
  bool roots_added = false;
  bool boot_roots_added = false;

  store = pl_store_new_mem();
  heap = pl_heap_new(BOOT_HEAP_CELLS, store);
  w = en_wisp_new(heap);
  if (w == NULL) {
    buf_append(&g_browser.error_buf, (const uint8_t*)"wisp: oom\n", 10);
    goto cleanup;
  }
  w->t->rplan_file_root_c = g_browser.file_root;
  sched =
      er_scheduler_new(store, (er_config){.file_root_c = g_browser.file_root});
  w->sched = sched;
  w->self = er_scheduler_adopt(sched, w->t);
  w->exec = NULL;

  ctx = (boot_ctx){
      .loc_a = ax_allocator_system(),
      .w = w,
      .src_dir_c = src_dir_c,
      .mod_v = NULL,
      .emit_top_level_f = g_browser.emit_top_level_f,
  };
  pl_gc_add_root_source(heap, boot_roots, &ctx);
  boot_roots_added = true;

  w->err_f = true;
  if (setjmp(w->errjmp) != 0) {
    const char* msg = w->msg_c == NULL ? "unknown error" : w->msg_c;
    buf_append(&g_browser.error_buf, (const uint8_t*)"wisp: ", 6);
    buf_append(&g_browser.error_buf, (const uint8_t*)msg, strlen(msg));
    buf_append(&g_browser.error_buf, (const uint8_t*)"\n", 1);
    goto cleanup;
  }

  const char* run_fn_c = fn_c != NULL && fn_c[0] != '\0' ? fn_c : NULL;
  bool ok = boot_load_assembly(&ctx, mod_c, run_fn_c, argc, argv);
  rc = ok ? 0 : 1;
  if (!ok && g_browser.error_buf.len == 0)
    buf_append(&g_browser.error_buf, g_browser.stderr_buf.data,
               g_browser.stderr_buf.len);

cleanup:
  if (boot_roots_added)
    pl_gc_del_root_source(heap, boot_roots, &ctx);
  for (boot_module* mod = ctx.mod_v; mod != NULL;) {
    boot_module* next = mod->next;
    boot_env_free(&ctx, mod->env);
    ax_free(ctx.loc_a, mod);
    mod = next;
  }
  AX_UNUSED(roots_added);
  er_scheduler_free(sched);
  en_wisp_free(w);
  pl_heap_free(heap);
  pl_store_free(store);
  return rc;
}

static browser_buf* output_for_channel(int channel) {
  if (channel == 1)
    return &g_browser.stdout_buf;
  if (channel == 2)
    return &g_browser.stderr_buf;
  return &g_browser.error_buf;
}

WISP_EXPORT const uint8_t* wisp_output_ptr(int channel) {
  return output_for_channel(channel)->data;
}

WISP_EXPORT size_t wisp_output_len(int channel) {
  return output_for_channel(channel)->len;
}

WISP_EXPORT const uint8_t* wisp_error_ptr(void) {
  return g_browser.error_buf.data;
}

WISP_EXPORT size_t wisp_error_len(void) {
  return g_browser.error_buf.len;
}

WISP_EXPORT size_t wisp_file_count(void) {
  return g_browser.file_len;
}

WISP_EXPORT const char* wisp_file_path_ptr(size_t i) {
  return i < g_browser.file_len ? g_browser.files[i].path : NULL;
}

WISP_EXPORT size_t wisp_file_path_len(size_t i) {
  return i < g_browser.file_len ? strlen(g_browser.files[i].path) : 0;
}

WISP_EXPORT const uint8_t* wisp_file_data_ptr(size_t i) {
  return i < g_browser.file_len ? g_browser.files[i].data : NULL;
}

WISP_EXPORT size_t wisp_file_data_len(size_t i) {
  return i < g_browser.file_len ? g_browser.files[i].len : 0;
}

WISP_EXPORT int wisp_file_written(size_t i) {
  return i < g_browser.file_len && g_browser.files[i].written ? 1 : 0;
}

WISP_EXPORT void wisp_dispose(void) {
  wisp_clear_files();
  buf_free(&g_browser.input);
  buf_free(&g_browser.stdout_buf);
  buf_free(&g_browser.stderr_buf);
  buf_free(&g_browser.error_buf);
  free(g_browser.file_root);
  g_browser.file_root = NULL;
}

#endif
