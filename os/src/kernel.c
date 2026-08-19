#include <gmp.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enki/actor.h"
#include "enki/wisp.h"
#include "os/boot.h"
#include "os/loader.h"
#include "os/platform.h"
#include "os/sha256.h"
#include "plan/eval.h"
#include "plan/heap.h"
#include "plan/store.h"

#define HEAP_CELLS ((size_t)1 << 23)
#define INPUT_LIMIT ((size_t)1 << 20)

typedef struct os_runtime {
  pl_store* store;
  pl_heap* heap;
  pl_thread* thread;
  er_scheduler* scheduler;
  er_actor* root_actor;
} os_runtime;

static pl_val runnable_pin(pl_val function) {
  for (;;) {
    pl_cell* pin = pl_as(PL_TAG_PIN, function);
    if (pin == NULL || pl_as(PL_TAG_LAW, pl_pin_body(pin)) != NULL)
      return function;
    function = pl_pin_body(pin);
  }
}

static bool drive_plain_call(pl_thread* thread, pl_val function,
                             pl_val argument) {
  pl_thread_start_call_nf(thread, runnable_pin(function), argument);
  for (;;) {
    pl_run_status status = pl_thread_run(thread, UINT64_MAX);
    if (status == PL_RUN_DONE)
      return true;
    if (status == PL_RUN_YIELDED)
      continue;
    if (status == PL_RUN_BLOCKED)
      pl_thread_abandon(thread);
    return false;
  }
}

static bool drive_runtime_call(os_runtime* runtime, pl_val function,
                               pl_val argument) {
  if (runtime->scheduler == NULL)
    return drive_plain_call(runtime->thread, function, argument);
  pl_thread_start_call_nf(runtime->thread, runnable_pin(function), argument);
  return er_scheduler_drive(runtime->scheduler, runtime->root_actor) ==
         ER_DRIVE_DONE;
}

static bool run_saved_session_mode(os_runtime* runtime, const uint8_t* input,
                                   size_t input_size, bool then_serial) {
  uint8_t previous_root[32];
  if (!pl_store_get_root(runtime->store, previous_root)) {
    fprintf(stderr, "enki-os: volatile store has no Reaver root\n");
    return false;
  }
  pl_val root = pl_store_load(runtime->thread, previous_root);
  runtime->thread->rplan_f = true;
  if (then_serial)
    os_rplan_set_input_then_serial(input, input_size);
  else
    os_rplan_set_input(input, input_size);
  if (!drive_runtime_call(runtime, root, 0)) {
    (void)pl_store_put_root(runtime->store, previous_root);
    os_rplan_set_output_enabled(true);
    fprintf(stderr, "enki-os: PLAN error: %s\n",
            runtime->thread->exn_msg != NULL
                ? runtime->thread->exn_msg
                : "uncaught PLAN exception");
    return false;
  }
  return true;
}

static bool run_saved_session(os_runtime* runtime, const uint8_t* input,
                              size_t input_size) {
  return run_saved_session_mode(runtime, input, input_size, false);
}

static bool enable_actors(os_runtime* runtime) {
  runtime->scheduler = er_scheduler_new(
      runtime->store,
      (er_config){.quantum = 4096,
                  .root_quantum = 4096,
                  .heap_cells = 8192,
                  .file_root_c = ""});
  if (runtime->scheduler == NULL)
    return false;
  runtime->root_actor =
      er_scheduler_adopt(runtime->scheduler, runtime->thread);
  return runtime->root_actor != NULL;
}

static bool bootstrap(os_runtime* runtime) {
  runtime->store = pl_store_new_mem();
  if (runtime->store == NULL)
    return false;
  pl_heap* boot_heap = pl_heap_new(HEAP_CELLS, runtime->store);
  en_wisp* wisp = en_wisp_new(boot_heap);
  if (boot_heap == NULL || wisp == NULL) {
    fprintf(stderr, "enki-os: unable to allocate bootstrap heap\n");
    return false;
  }
  os_loader* loader = os_loader_new(wisp);
  if (loader == NULL)
    return false;

  volatile bool ok = false;
  wisp->err_f = true;
  os_rplan_set_output_enabled(false);
  if (setjmp(wisp->errjmp) != 0) {
    fprintf(stderr, "enki-os: Wisp bootstrap failed: %s\n",
            wisp->msg_c != NULL ? wisp->msg_c : "unknown Wisp error");
  } else {
    printf("enki-os: assembling Reaver from ROM...\n");
    if (!os_loader_load(loader, "reaver")) {
      fprintf(stderr, "enki-os: plan/reaver.plan not found in ROM\n");
    } else {
      pl_val main_function = 0;
      if (!os_loader_binding(loader, "main", &main_function)) {
        fprintf(stderr, "enki-os: Reaver module does not export main\n");
      } else {
        wisp->t->rplan_f = true;
        os_rplan_set_input(NULL, 0);
        ok = drive_plain_call(wisp->t, main_function, 0);
        if (!ok)
          fprintf(stderr, "enki-os: Reaver main failed: %s\n",
                  wisp->t->exn_msg != NULL ? wisp->t->exn_msg : "PLAN exception");
      }
    }
  }

  os_loader_free(loader);
  en_wisp_free(wisp);
  pl_heap_free(boot_heap);
  if (!ok) {
    os_rplan_set_output_enabled(true);
    return false;
  }

  runtime->heap = pl_heap_new(HEAP_CELLS, runtime->store);
  runtime->thread = pl_thread_new(runtime->heap);
  static const uint8_t import_std[] =
      "(#bind std (#module std))\n(#import std)\n";
  printf("enki-os: importing std...\n");
  if (!run_saved_session(runtime, import_std, sizeof(import_std) - 1))
    return false;
  os_rplan_set_output_enabled(true);
  printf("enki-os: Reaver ready (volatile store, ROM modules, COM1)\n");
  return true;
}

typedef struct form_state {
  int parentheses;
  int brackets;
  int braces;
  bool string;
  bool comment;
  bool content;
  bool invalid_close;
} form_state;

static form_state scan_form(const char* text, size_t size) {
  form_state state = {0};
  for (size_t i = 0; i < size; i++) {
    char c = text[i];
    if (state.comment) {
      if (c == '\n')
        state.comment = false;
      continue;
    }
    if (state.string) {
      if (c == '"')
        state.string = false;
      continue;
    }
    if (c == ';') {
      state.comment = true;
      continue;
    }
    if (c == '"') {
      state.string = true;
      state.content = true;
      continue;
    }
    if (c == '(') state.parentheses++;
    else if (c == ')') { if (--state.parentheses < 0) state.invalid_close = true; }
    else if (c == '[') state.brackets++;
    else if (c == ']') { if (--state.brackets < 0) state.invalid_close = true; }
    else if (c == '{') state.braces++;
    else if (c == '}') { if (--state.braces < 0) state.invalid_close = true; }
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      state.content = true;
  }
  return state;
}

static bool command_is_quit(const char* text, size_t size) {
  while (size != 0 && (text[size - 1] == '\n' || text[size - 1] == '\r' ||
                       text[size - 1] == ' ' || text[size - 1] == '\t'))
    size--;
  while (size != 0 && (*text == ' ' || *text == '\t')) {
    text++;
    size--;
  }
  return size == 5 && memcmp(text, ":quit", 5) == 0;
}

static bool command_line_has(const char* command_line, const char* wanted) {
  size_t wanted_size = strlen(wanted);
  const char* cursor = command_line == NULL ? "" : command_line;
  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == '\t')
      cursor++;
    const char* begin = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
      cursor++;
    if ((size_t)(cursor - begin) == wanted_size &&
        memcmp(begin, wanted, wanted_size) == 0)
      return true;
  }
  return false;
}

static bool shrine(os_runtime* runtime) {
  static const uint8_t boot[] =
      "(#bind helm-repl (#module helm-repl))\n"
      "(helm-repl:run-repl-serial 0)\n";
  if (!enable_actors(runtime)) {
    fprintf(stderr, "enki-os: unable to initialize actor runtime\n");
    return false;
  }
  printf("enki-os: compiling Shrine core and bundled apps...\n");
  /* Shrine's user interface writes fd 1 directly.  Keep compiler Trace,
   * Output, Warn, and Print traffic off polling COM1: it is both internal
   * noise and orders of magnitude slower than the in-guest compilation. */
  os_rplan_set_output_enabled(false);
  return run_saved_session_mode(runtime, boot, sizeof(boot) - 1, true);
}

static void repl(os_runtime* runtime) {
  char* input = malloc(4096);
  size_t capacity = input == NULL ? 0 : 4096;
  size_t size = 0;
  bool continuation = false;
  if (input == NULL)
    abort();

  for (;;) {
    printf(continuation ? "... " : "enki> ");
    bool line_done = false;
    while (!line_done) {
      int raw = os_serial_getc();
      char c = (char)raw;
      if (c == 3) { /* Ctrl-C */
        printf("^C\n");
        size = 0;
        continuation = false;
        line_done = true;
        continue;
      }
      if (c == 4 && size == 0) { /* Ctrl-D */
        printf("^D\n");
        free(input);
        return;
      }
      if (c == 8 || c == 127) {
        if (size != 0) {
          size--;
          os_serial_write("\b \b", 3);
        }
        continue;
      }
      if (c == '\r')
        c = '\n';
      if (c == '\n') {
        os_serial_putc('\n');
        line_done = true;
      } else if ((unsigned char)c < 0x20) {
        continue;
      } else {
        os_serial_putc(c);
      }
      if (size + 2 > capacity) {
        if (capacity >= INPUT_LIMIT) {
          printf("\nenki-os: input exceeds 1 MiB; discarded\n");
          size = 0;
          continuation = false;
          line_done = true;
          continue;
        }
        size_t next = capacity * 2;
        if (next > INPUT_LIMIT) next = INPUT_LIMIT;
        char* grown = realloc(input, next);
        if (grown == NULL) abort();
        input = grown;
        capacity = next;
      }
      input[size++] = c;
      input[size] = '\0';
    }

    if (size == 0)
      continue;
    form_state state = scan_form(input, size);
    if (!state.content) {
      size = 0;
      continuation = false;
      continue;
    }
    bool complete = state.invalid_close ||
                    (!state.string && state.parentheses == 0 &&
                     state.brackets == 0 && state.braces == 0);
    if (!complete) {
      continuation = true;
      continue;
    }
    if (command_is_quit(input, size)) {
      free(input);
      return;
    }
    (void)run_saved_session(runtime, (const uint8_t*)input, size);
    size = 0;
    continuation = false;
  }
}

static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t n) {
  return memcmp(a, b, n) == 0;
}

static void selftest(void) {
  printf("enki-os: selftest\n");
  void* a = malloc(1024);
  void* b = malloc(2048);
  if (a == NULL || b == NULL) abort();
  free(a); free(b);
  size_t before = os_memory_available();
  void* c = malloc(3000);
  free(c);
  if (os_memory_available() != before) abort();
  void* grow = malloc(1000);
  void* neighbor = malloc(2000);
  free(neighbor);
  grow = realloc(grow, 2500);
  if (grow == NULL) abort();
  free(grow);
  if (os_memory_available() != before) abort();

  static const uint8_t expected[32] = {
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
  };
  uint8_t digest[32];
  os_sha256_ctx sha;
  os_sha256_init(&sha); os_sha256_update(&sha, "abc", 3); os_sha256_final(&sha, digest);
  if (!bytes_equal(digest, expected, sizeof(expected))) abort();

  mp_limb_t x[2] = {UINT64_MAX, 1};
  mp_limb_t y[1] = {3};
  mp_limb_t q[2], r[1];
  mpn_tdiv_qr(q, r, 0, x, 2, y, 1);
  if (r[0] != 1) abort();
  mp_limb_t wide_n[2] = {UINT64_MAX, UINT64_MAX};
  mp_limb_t wide_d[2] = {1, UINT64_C(0x8000000000000000)};
  mp_limb_t wide_q[1], wide_r[2];
  mpn_tdiv_qr(wide_q, wide_r, 0, wide_n, 2, wide_d, 2);
  if (wide_q[0] != 1 || wide_r[0] != UINT64_C(0xfffffffffffffffe) ||
      wide_r[1] != UINT64_C(0x7fffffffffffffff)) abort();
  mpz_t z;
  mpz_init(z);
  if (mpz_set_str(z, "340282366920938463463374607431768211456", 10) != 0 ||
      mpz_size(z) != 3) abort();
  char* decimal = mpz_get_str(NULL, 10, z);
  if (decimal == NULL || strcmp(decimal, "340282366920938463463374607431768211456") != 0) abort();
  free(decimal); mpz_clear(z);

  jmp_buf jump;
  volatile int jump_value = setjmp(jump);
  if (jump_value == 0) longjmp(jump, 7);
  if (jump_value != 7) abort();
  if (os_rom_find("plan/reaver.plan") == NULL) abort();

  os_rom_child apps[4];
  static const char* app_names[] = {"chat", "demo", "life", "nenex"};
  if (os_rom_children("foil/apps", apps, 4) != 4) abort();
  for (size_t i = 0; i < 4; i++)
    if (!apps[i].folder || strlen(app_names[i]) != apps[i].name_size ||
        memcmp(apps[i].name, app_names[i], apps[i].name_size) != 0)
      abort();
  if (os_rom_children("/", NULL, 0) == 0 ||
      os_rom_children("foil/apps/chat", NULL, 0) == 0 ||
      os_rom_children("foil/apps/missing", NULL, 0) != 0 ||
      os_rom_children("foil/apps/../plan", NULL, 0) != 0)
    abort();

  printf("ENKI_OS_SELFTEST_OK\n");
  os_qemu_exit(0);
  os_halt();
}

void os_boot_entry(uint32_t magic, uint32_t multiboot_address) {
  os_serial_init();
  os_idt_init();
  os_boot_info boot;
  if (!os_multiboot_parse(magic, multiboot_address, &boot)) {
    fprintf(stderr, "enki-os: invalid Multiboot2 information\n");
    os_qemu_exit(2);
    os_halt();
  }
  os_memory_init(boot.heap_begin, boot.heap_end);
  printf("enki-os: x86_64 bare metal, heap=%zu MiB\n",
         os_memory_available() >> 20);
  if (command_line_has(boot.command_line, "selftest"))
    selftest();

  bool shrine_mode = command_line_has(boot.command_line, "shrine");
  os_arena_set_store_limit(shrine_mode ? (size_t)1 << 30 : (size_t)1 << 27);

  os_runtime runtime = {0};
  if (!bootstrap(&runtime)) {
    fprintf(stderr, "enki-os: bootstrap failed\n");
    os_qemu_exit(3);
    os_halt();
  }
  if (shrine_mode) {
    printf("enki-os: Shrine mode (volatile, serial-only)\n");
    if (!shrine(&runtime)) {
      fprintf(stderr, "enki-os: Shrine failed\n");
      os_qemu_exit(4);
      os_halt();
    }
  } else {
    repl(&runtime);
  }
  printf("enki-os: halted\n");
  os_qemu_exit(0);
  os_halt();
}
