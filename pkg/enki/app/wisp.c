#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "axsys/allocator.h"
#include "axsys/assume.h"
#include "axsys/profile.h"
#include "axsys/util.h"
#include "enki/actor.h"
#include "enki/wisp.h"
#include "plan/build.h"
#include "plan/eval.h"
#include "plan/nat.h"
#include "plan/heap.h"
#include "plan/store.h"

/*
 * wisp [--text-hash] [--file-root DIR] [--profile-json FILE]
 *      [--profile-pins USECS]
 *      DIR MODULE [FUNCTION ARGS...]
 *
 * Loads MODULE (and its @includes) from DIR, then optionally applies the
 * binding FUNCTION to a row of the remaining arguments.  Pins and the
 * saved root use the Silo pack by default; --text-hash selects the
 * reference canonical-text snapshots.  Mirrors the reference
 * loadAssembly / runRepl drivers.
 */

#define BOOT_HEAP_CELLS                ((size_t)1 << 26) /* 512 MiB per semispace, grows */
#define BOOT_GC_ALLOCATION_FLOOR_CELLS ((size_t)1 << 20) /* 8 MiB */
// ./build/release/bin/wisp --file-root ../reaver/src ../reaver/src/plan
// main  35.43s user 0.55s system 99% cpu 36.136 total
#define BOOT_DEFAULT_FILE_ROOT         "./reaver/src"
#define BOOT_DEFAULT_STORE_DIR         "./snap"
#define BOOT_STORE_MAP_SIZE            ((size_t)1 << 30)

typedef struct boot_module {
  pl_val key_v;
  en_env_entry* env;
  struct boot_module* next;
} boot_module;

#define BOOT_TMP_ENV_CAP 256

typedef struct boot_ctx {
  const ax_allocator* loc_a;
  en_wisp* w;
  const char* src_dir_c;
  boot_module* mod_v;
  /* rooted scratch for env lists under construction */
  en_env_entry* tmp_env_v[BOOT_TMP_ENV_CAP];
  size_t tmp_env_s;
} boot_ctx;

static void boot_trace_env(pl_root_visit visit, void* gc_ctx,
                           en_env_entry* env) {
  for (en_env_entry* e = env; e != NULL; e = e->next) {
    visit(&e->key_v, gc_ctx);
    visit(&e->val_v, gc_ctx);
  }
}

static void boot_roots(pl_root_visit visit, void* gc_ctx, void* src_ctx) {
  boot_ctx* ctx = src_ctx;
  for (boot_module* mod = ctx->mod_v; mod != NULL; mod = mod->next) {
    visit(&mod->key_v, gc_ctx);
    boot_trace_env(visit, gc_ctx, mod->env);
  }
  for (size_t i = 0; i < ctx->tmp_env_s; i++)
    boot_trace_env(visit, gc_ctx, ctx->tmp_env_v[i]);
}

static void boot_env_root_push(boot_ctx* ctx, en_env_entry* env) {
  ax_assertf(ctx->tmp_env_s < BOOT_TMP_ENV_CAP, "env root overflow");
  ctx->tmp_env_v[ctx->tmp_env_s++] = env;
}

static void boot_collect_after_form(boot_ctx* ctx) {
  /*
   * A form boundary is a convenient safe point for dropping its temporary
   * graph, but copying the complete, growing module environment after every
   * form makes assembly quadratic.  Wait until new allocation can pay for
   * copying the prior live set (with a floor for small live sets).  Reserve
   * still collects on exhaustion, so this is a space/throughput policy only.
   */
  (void)pl_gc_collect_if_pressure(ctx->w->t, BOOT_GC_ALLOCATION_FLOOR_CELLS);
}

/* ── Environment list helpers (host-side, allocator-backed) ────────────── */

static void boot_env_free(boot_ctx* ctx, en_env_entry* env) {
  while (env != NULL) {
    en_env_entry* next = env->next;
    ax_free(ctx->loc_a, env);
    env = next;
  }
}

static en_env_entry* boot_env_find(en_env_entry* env, pl_val key) {
  for (en_env_entry* e = env; e != NULL; e = e->next) {
    if (pl_nat_eq(e->key_v, key))
      return e;
  }
  return NULL;
}

static en_env_entry* boot_env_clone(boot_ctx* ctx, en_env_entry* env) {
  en_env_entry* head = NULL;
  en_env_entry** tail = &head;
  for (en_env_entry* e = env; e != NULL; e = e->next) {
    en_env_entry* copy = ax_calloc(ctx->loc_a, en_env_entry, 1);
    if (copy == NULL)
      return NULL;
    *copy = (en_env_entry){
        .key_v = e->key_v, .val_v = e->val_v, .mac_f = e->mac_f, .next = NULL};
    *tail = copy;
    tail = &copy->next;
  }
  return head;
}

static bool boot_env_put(boot_ctx* ctx, en_env_entry** env, en_env_entry* src) {
  en_env_entry* old = boot_env_find(*env, src->key_v);
  if (old != NULL) {
    old->val_v = src->val_v;
    old->mac_f = src->mac_f;
    return true;
  }
  en_env_entry* copy = ax_calloc(ctx->loc_a, en_env_entry, 1);
  if (copy == NULL)
    return false;
  *copy = (en_env_entry){.key_v = src->key_v,
                         .val_v = src->val_v,
                         .mac_f = src->mac_f,
                         .next = *env};
  *env = copy;
  return true;
}

static en_env_entry* boot_env_merge(boot_ctx* ctx, en_env_entry* old,
                                    en_env_entry* new_env) {
  en_env_entry* out = boot_env_clone(ctx, old);
  if (old != NULL && out == NULL)
    return NULL;
  for (en_env_entry* e = new_env; e != NULL; e = e->next) {
    if (!boot_env_put(ctx, &out, e))
      return NULL;
  }
  return out;
}

static boot_module* boot_module_find(boot_ctx* ctx, pl_val key) {
  for (boot_module* mod = ctx->mod_v; mod != NULL; mod = mod->next) {
    if (pl_nat_eq(mod->key_v, key))
      return mod;
  }
  return NULL;
}

static bool boot_module_put(boot_ctx* ctx, pl_val key, en_env_entry* env) {
  boot_module* mod = ax_calloc(ctx->loc_a, boot_module, 1);
  if (mod == NULL)
    return false;
  *mod = (boot_module){.key_v = key, .env = env, .next = ctx->mod_v};
  ctx->mod_v = mod;
  return true;
}

/* ── Module file handling ──────────────────────────────────────────────── */

static bool boot_eat(char** str_c) {
  for (;;) {
    if (**str_c == ';') {
      while (**str_c != '\n' && **str_c != 0)
        (*str_c)++;
    }
    if (**str_c == ' ' || **str_c == '\n') {
      (*str_c)++;
      continue;
    }
    return **str_c == 0;
  }
}

static bool boot_ok_file_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static bool boot_ok_module_name(const char* mod_c) {
  if (mod_c == NULL || *mod_c == '\0')
    return false;
  for (const char* c = mod_c; *c != '\0'; c++) {
    if (!boot_ok_file_char(*c))
      return false;
  }
  return true;
}

/* @module include forms */
static char* boot_read_include(boot_ctx* ctx, pl_val form) {
  if (!pl_is_nat(form))
    return NULL;
  size_t n = pl_nat_byte_len(form);
  if (n < 2 || pl_nat_byte_at(form, 0) != '@')
    return NULL;
  char* mod_c = ax_calloc(ctx->loc_a, char, n);
  if (mod_c == NULL)
    return NULL;
  for (size_t i = 1; i < n; i++) {
    char c = (char)pl_nat_byte_at(form, i);
    if (!boot_ok_file_char(c)) {
      ax_free(ctx->loc_a, mod_c);
      return NULL;
    }
    mod_c[i - 1] = c;
  }
  mod_c[n - 1] = '\0';
  return mod_c;
}

static char* boot_module_path(boot_ctx* ctx, const char* mod_c) {
  size_t n = strlen(ctx->src_dir_c) + 1 + strlen(mod_c) + 6;
  char* path_c = ax_calloc(ctx->loc_a, char, n + 1);
  if (path_c == NULL)
    return NULL;
  (void)snprintf(path_c, n + 1, "%s/%s.plan", ctx->src_dir_c, mod_c);
  return path_c;
}

static char* boot_read_file(boot_ctx* ctx, const char* path_c) {
  FILE* file = fopen(path_c, "rb");
  if (file == NULL) {
    fprintf(stderr, "wisp: failed to open %s\n", path_c);
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long len_l = ftell(file);
  if (len_l < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  size_t len = (size_t)len_l;
  char* text_c = ax_calloc(ctx->loc_a, char, len + 1);
  if (text_c == NULL) {
    fclose(file);
    return NULL;
  }
  size_t got = fread(text_c, 1, len, file);
  fclose(file);
  if (got != len) {
    ax_free(ctx->loc_a, text_c);
    return NULL;
  }
  text_c[len] = '\0';
  return text_c;
}

static bool boot_process_file(boot_ctx* ctx, const char* mod_c);

static bool boot_process_form(boot_ctx* ctx, pl_val form) {
  char* inc_c = boot_read_include(ctx, form);
  if (inc_c != NULL) {
    bool ok = boot_process_file(ctx, inc_c);
    ax_free(ctx->loc_a, inc_c);
    return ok;
  }

  en_wisp* w = ctx->w;
  pl_val exp = en_wisp_macroexpand(w, form);
  if (exp == 0) {
    boot_collect_after_form(ctx);
    return true;
  }
  /* Preserve the reference driver's deep forcing without dumping every
   * top-level assembly result to stderr. */
  (void)en_run_nf(w, en_wisp_thunk(w, exp));
  boot_collect_after_form(ctx);
  return true;
}

static en_env_entry* boot_load_module(boot_ctx* ctx, const char* mod_c) {
  en_wisp* w = ctx->w;
  if (!boot_ok_module_name(mod_c)) {
    fprintf(stderr, "wisp: bad path: %s\n", mod_c);
    return NULL;
  }

  size_t mark = en_root_mark(w);
  en_root_push(w, en_string_nat(w, mod_c));
  boot_module* cached = boot_module_find(ctx, w->tmp_v[mark]);
  if (cached != NULL) {
    en_env_entry* out = boot_env_clone(ctx, cached->env);
    en_root_pop(w, mark);
    return out;
  }

  char* path_c = boot_module_path(ctx, mod_c);
  if (path_c == NULL) {
    en_root_pop(w, mark);
    return NULL;
  }
  char* text_c = boot_read_file(ctx, path_c);
  ax_free(ctx->loc_a, path_c);
  if (text_c == NULL) {
    en_root_pop(w, mark);
    return NULL;
  }

  boot_env_free(ctx, w->env);
  w->env = NULL; /* modules load in an empty environment */
  char* cur_c = text_c;
  while (!boot_eat(&cur_c)) {
    pl_val form = en_wisp_parse(w, &cur_c);
    if (!boot_process_form(ctx, form)) {
      ax_free(ctx->loc_a, text_c);
      en_root_pop(w, mark);
      return NULL;
    }
  }
  ax_free(ctx->loc_a, text_c);

  en_env_entry* module_env = boot_env_clone(ctx, w->env);
  if (w->env != NULL && module_env == NULL) {
    en_root_pop(w, mark);
    return NULL;
  }
  en_env_entry* cached_env = boot_env_clone(ctx, module_env);
  if (module_env != NULL && cached_env == NULL) {
    en_root_pop(w, mark);
    return NULL;
  }
  if (!boot_module_put(ctx, w->tmp_v[mark], cached_env)) {
    en_root_pop(w, mark);
    return NULL;
  }
  en_root_pop(w, mark);
  return module_env;
}

static bool boot_process_file(boot_ctx* ctx, const char* mod_c) {
  en_wisp* w = ctx->w;
  size_t mark = en_root_mark(w);
  size_t env_mark = ctx->tmp_env_s;
  en_root_push(w, en_string_nat(w, mod_c));

  en_env_entry* old_env = boot_env_clone(ctx, w->env);
  if (w->env != NULL && old_env == NULL) {
    en_root_pop(w, mark);
    return false;
  }
  boot_env_root_push(ctx, old_env);

  en_env_entry* mod_env = boot_load_module(ctx, mod_c);
  bool found = mod_env != NULL || boot_module_find(ctx, w->tmp_v[mark]) != NULL;
  if (!found) {
    ctx->tmp_env_s = env_mark;
    en_root_pop(w, mark);
    return false;
  }
  boot_env_root_push(ctx, mod_env);

  en_env_entry* merged = boot_env_merge(ctx, old_env, mod_env);
  if ((old_env != NULL || mod_env != NULL) && merged == NULL) {
    ctx->tmp_env_s = env_mark;
    en_root_pop(w, mark);
    return false;
  }
  boot_env_free(ctx, w->env);
  boot_env_free(ctx, old_env);
  boot_env_free(ctx, mod_env);
  w->env = merged;
  ctx->tmp_env_s = env_mark;
  en_root_pop(w, mark);
  return true;
}

/* ── Running a bound function ──────────────────────────────────────────── */

static pl_val boot_make_row(boot_ctx* ctx, int argc, char** argv) {
  en_wisp* w = ctx->w;
  if (argc == 0)
    return 0;
  size_t mark = en_root_mark(w);
  for (int i = 0; i < argc; i++)
    en_root_push(w, en_string_nat(w, argv[i]));
  pl_val row = en_app_make(w, 0, (size_t)argc, &w->tmp_v[mark]);
  en_root_pop(w, mark);
  return row;
}

static pl_val boot_repl_fun(pl_val fun) {
  for (;;) {
    pl_cell* pin = pl_as(PL_TAG_PIN, fun);
    if (pin == NULL || pl_as(PL_TAG_LAW, pl_pin_body(pin)) != NULL)
      return fun;
    fun = pl_pin_body(pin);
  }
}

static bool boot_run_function(boot_ctx* ctx, pl_val fun, int argc,
                              char** argv) {
  en_wisp* w = ctx->w;
  en_env_entry* old_env = w->env;
  size_t env_mark = ctx->tmp_env_s;
  boot_env_root_push(ctx, old_env);
  w->env = NULL;
  w->t->rplan_f = true; /* runRepl runs the program in RPLAN mode */

  size_t mark = en_root_mark(w);
  en_root_push(w, boot_repl_fun(fun));
  en_root_push(w, 0);
  w->tmp_v[mark + 1] = boot_make_row(ctx, argc, argv);

  /* the reference runRepl: force (fun % args), in the root actor so
   * the program may spawn, send, and block on Recv.  Assembly deliberately
   * leaves w->exec NULL and uses the deterministic scheduler; create the
   * persistent worker pool only at this final-program boundary. */
  ax_assume(w->exec == NULL, "boot assembly must be single-threaded");
  er_mt_executor* exec = er_mt_executor_new(w->sched, (er_mt_config){0});
  pl_thread_start_call_nf(w->t, w->tmp_v[mark], w->tmp_v[mark + 1]);
  er_drive_status ds = er_mt_executor_drive(exec, w->self);
  er_mt_executor_free(exec);
  en_root_pop(w, mark);
  w->env = old_env;
  ctx->tmp_env_s = env_mark;
  if (ds != ER_DRIVE_DONE) {
    fprintf(stderr, "wisp: runtime error: %s\n",
            ds == ER_DRIVE_DEADLOCK ? "deadlock: every actor is blocked on Recv"
            : w->t->exn_msg != NULL ? w->t->exn_msg
                                    : "PLAN exception");
    return false;
  }
  return true;
}

static bool boot_load_assembly(boot_ctx* ctx, const char* mod_c,
                               const char* fn_c, int argc, char** argv) {
  en_wisp* w = ctx->w;
  w->env = NULL;

  pl_store* store = pl_heap_store(w->t->heap);
  bool silo_root_f = store->format == PL_STORE_FORMAT_SILO_V1 &&
                     (strcmp(ctx->src_dir_c, "snap") == 0 ||
                      strcmp(ctx->src_dir_c, "./snap") == 0) &&
                     strcmp(mod_c, "root") == 0;
  if (silo_root_f) {
    uint8_t hash[32];
    if (!pl_store_get_root(store, hash)) {
      fprintf(stderr, "wisp: Silo store has no root\n");
      return false;
    }
    pl_val root = pl_store_load(w->t, hash);
    if (fn_c == NULL)
      return true;
    if (strcmp(fn_c, "_") != 0) {
      fprintf(stderr, "wisp: Silo root only binds _\n");
      return false;
    }
    return boot_run_function(ctx, root, argc, argv);
  }

  /* the reference loadAssembly: snapshots load in RPLAN mode, plan
   * sources in BPLAN mode (op 82 is gated on this) */
  w->t->rplan_f = strcmp(ctx->src_dir_c, "snap") == 0;
  if (!boot_process_file(ctx, mod_c))
    return false;
  if (fn_c == NULL)
    return true;

  size_t mark = en_root_mark(w);
  en_root_push(w, en_string_nat(w, fn_c));
  en_env_entry* ent = en_wisp_getenv(w, w->tmp_v[mark]);
  en_root_pop(w, mark);
  if (ent == NULL) {
    fprintf(stderr, "wisp: program unbound: %s\n", fn_c);
    return false;
  }
  return boot_run_function(ctx, ent->val_v, argc, argv);
}

static bool boot_parse_double(const char* s, double* out) {
  char* end = NULL;
  double v = strtod(s, &end);
  if (end == s || *end != '\0' || v < 0.0)
    return false;
  *out = v;
  return true;
}

static bool boot_parse_u64(const char* s, uint64_t* out) {
  if (s[0] == '\0' || s[0] == '-')
    return false;
  errno = 0;
  char* end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno == ERANGE || end == s || *end != '\0')
    return false;
  *out = (uint64_t)v;
  return true;
}

static void boot_usage(const char* argv0_c) {
  fprintf(stderr,
          "usage: %s [--text-hash] [--file-root DIR] "
          "[--wait-for-tracy[=SECONDS]] "
          "[--profile-json FILE] [--profile-locks FILE] "
          "[--profile-pins USECS] "
          "DIR MODULE [FUNCTION ARGS...]\n",
          argv0_c);
}

static const char* boot_env_file_root(void) {
  const char* env_c = getenv("ENKI_WISP_FILE_ROOT");
  return env_c != NULL && env_c[0] != '\0' ? env_c : BOOT_DEFAULT_FILE_ROOT;
}

int main(int argc, char** argv) {
  const char* file_root_c = boot_env_file_root();
  const char* profile_json_c = NULL;
  const char* profile_locks_c = NULL;
  uint64_t pin_profile_us = 0;
  double tracy_wait_s = 0.0;
  bool text_hash_f = false;
  bool pin_profile_f = false;
  volatile int argi = 1;
  while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
    if (strcmp(argv[argi], "--") == 0) {
      argi++;
      break;
    }
    if (strcmp(argv[argi], "--text-hash") == 0) {
      text_hash_f = true;
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "--file-root") == 0) {
      if (argi + 1 >= argc) {
        boot_usage(argv[0]);
        return 2;
      }
      file_root_c = argv[argi + 1];
      argi += 2;
      continue;
    }
    const char prefix_c[] = "--file-root=";
    size_t prefix_s = sizeof(prefix_c) - 1;
    if (strncmp(argv[argi], prefix_c, prefix_s) == 0) {
      file_root_c = argv[argi] + prefix_s;
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "--profile-json") == 0) {
      if (argi + 1 >= argc || argv[argi + 1][0] == '\0') {
        boot_usage(argv[0]);
        return 2;
      }
      profile_json_c = argv[argi + 1];
      argi += 2;
      continue;
    }
    const char json_prefix_c[] = "--profile-json=";
    size_t json_prefix_s = sizeof(json_prefix_c) - 1;
    if (strncmp(argv[argi], json_prefix_c, json_prefix_s) == 0) {
      profile_json_c = argv[argi] + json_prefix_s;
      if (profile_json_c[0] == '\0') {
        boot_usage(argv[0]);
        return 2;
      }
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "--profile-locks") == 0) {
      if (argi + 1 >= argc || argv[argi + 1][0] == '\0') {
        boot_usage(argv[0]);
        return 2;
      }
      profile_locks_c = argv[argi + 1];
      argi += 2;
      continue;
    }
    const char lock_prefix_c[] = "--profile-locks=";
    size_t lock_prefix_s = sizeof(lock_prefix_c) - 1;
    if (strncmp(argv[argi], lock_prefix_c, lock_prefix_s) == 0) {
      profile_locks_c = argv[argi] + lock_prefix_s;
      if (profile_locks_c[0] == '\0') {
        boot_usage(argv[0]);
        return 2;
      }
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "--profile-pins") == 0) {
      if (argi + 1 >= argc ||
          !boot_parse_u64(argv[argi + 1], &pin_profile_us)) {
        boot_usage(argv[0]);
        return 2;
      }
      pin_profile_f = true;
      argi += 2;
      continue;
    }
    const char pin_prefix_c[] = "--profile-pins=";
    size_t pin_prefix_s = sizeof(pin_prefix_c) - 1;
    if (strncmp(argv[argi], pin_prefix_c, pin_prefix_s) == 0) {
      if (!boot_parse_u64(argv[argi] + pin_prefix_s, &pin_profile_us)) {
        boot_usage(argv[0]);
        return 2;
      }
      pin_profile_f = true;
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "--wait-for-tracy") == 0) {
      tracy_wait_s = 10.0;
      if (argi + 1 < argc && strncmp(argv[argi + 1], "--", 2) != 0) {
        double parsed_s;
        if (boot_parse_double(argv[argi + 1], &parsed_s)) {
          tracy_wait_s = parsed_s;
          argi++;
        }
      }
      argi++;
      continue;
    }
    const char tracy_prefix_c[] = "--wait-for-tracy=";
    size_t tracy_prefix_s = sizeof(tracy_prefix_c) - 1;
    if (strncmp(argv[argi], tracy_prefix_c, tracy_prefix_s) == 0) {
      if (!boot_parse_double(argv[argi] + tracy_prefix_s, &tracy_wait_s)) {
        boot_usage(argv[0]);
        return 2;
      }
      argi++;
      continue;
    }
    boot_usage(argv[0]);
    return 2;
  }

  if (argc - argi < 2) {
    boot_usage(argv[0]);
    return 2;
  }
  if (file_root_c[0] == '\0') {
    boot_usage(argv[0]);
    return 2;
  }

  if (profile_json_c != NULL && !ax_profile_json_start(profile_json_c)) {
    fprintf(stderr, "wisp: cannot open profile JSON `%s`: %s\n", profile_json_c,
            strerror(errno));
    return 1;
  }

  ax_wait_for_tracy(tracy_wait_s);

  pl_store* store;
  if (text_hash_f) {
    store = pl_store_new_mem();
  } else {
    if (mkdir(BOOT_DEFAULT_STORE_DIR, 0777) != 0 && errno != EEXIST) {
      fprintf(stderr, "wisp: cannot create Silo store %s\n",
              BOOT_DEFAULT_STORE_DIR);
      (void)ax_profile_json_finish();
      return 1;
    }
    store = pl_store_new_silo(BOOT_DEFAULT_STORE_DIR, BOOT_STORE_MAP_SIZE);
    if (store == NULL) {
      fprintf(stderr, "wisp: cannot open Silo store %s\n",
              BOOT_DEFAULT_STORE_DIR);
      (void)ax_profile_json_finish();
      return 1;
    }
  }
  if (profile_locks_c != NULL &&
      !pl_store_profile_locks(store, profile_locks_c)) {
    fprintf(stderr, "wisp: cannot open lock profile `%s`: %s\n",
            profile_locks_c, strerror(errno));
    pl_store_free(store);
    (void)ax_profile_json_finish();
    return 1;
  }
  if (pin_profile_f)
    pl_store_profile_pins(store, pin_profile_us);
  pl_heap* heap = pl_heap_new(BOOT_HEAP_CELLS, store);
  en_wisp* w = en_wisp_new(heap);
  if (w == NULL) {
    fprintf(stderr, "wisp: oom\n");
    pl_heap_free(heap);
    (void)pl_store_profile_locks_finish(store);
    pl_store_free(store);
    if (!ax_profile_json_finish())
      fprintf(stderr, "wisp: failed to finalize profile JSON\n");
    return 1;
  }
  w->t->rplan_file_root_c = file_root_c;

  /* the boot thread is the root actor (the reference withNewRts);
   * spawned actors inherit the ReadFile jail */
  er_scheduler* sched =
      er_scheduler_new(store, (er_config){.file_root_c = file_root_c});
  w->sched = sched;
  w->self = er_scheduler_adopt(sched, w->t);

  boot_ctx* ctx = malloc(sizeof(*ctx));
  ax_assume(ctx != NULL, "oom");
  *ctx = (boot_ctx){
      .loc_a = ax_allocator_system(),
      .w = w,
      .src_dir_c = argv[argi],
      .mod_v = NULL,
  };
  pl_gc_add_root_source(heap, boot_roots, ctx);

  volatile int exit_code = 1;
  w->err_f = true;
  if (setjmp(w->errjmp) != 0) {
    fprintf(stderr, "wisp: %s\n",
            w->msg_c == NULL ? "unknown error" : w->msg_c);
  } else {
    const char* fn_c = argc - argi >= 3 ? argv[argi + 2] : NULL;
    int run_argc = argc - argi >= 4 ? argc - argi - 3 : 0;
    char** run_argv = argc - argi >= 4 ? argv + argi + 3 : NULL;
    bool ok = boot_load_assembly(ctx, argv[argi + 1], fn_c, run_argc, run_argv);
    exit_code = ok ? 0 : 1;
  }

  pl_gc_del_root_source(heap, boot_roots, ctx);
  for (boot_module* mod = ctx->mod_v; mod != NULL;) {
    boot_module* next = mod->next;
    boot_env_free(ctx, mod->env);
    ax_free(ctx->loc_a, mod);
    mod = next;
  }
  free(ctx);
  er_scheduler_free(sched); /* leftover actors die with the program */
  en_wisp_free(w);
  pl_heap_free(heap);
  if (!pl_store_profile_locks_finish(store)) {
    fprintf(stderr, "wisp: failed to finalize lock profile\n");
    exit_code = 1;
  }
  pl_store_free(store);
  if (!ax_profile_json_finish()) {
    fprintf(stderr, "wisp: failed to finalize profile JSON\n");
    exit_code = 1;
  }
  return exit_code;
}
