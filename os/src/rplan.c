#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"
#include "os/platform.h"
#include "plan/build.h"
#include "plan/nat.h"
#include "plan/rplan.h"

#define ARG(i) (t->vstack[ab + (i)])

static const uint8_t* input_data;
static size_t input_size;
static size_t input_offset;
static const uint8_t empty_input;
static bool output_enabled = true;
static bool input_boundary_sent;
static bool serial_after_boundary;
static bool serial_eof;
static uint8_t serial_line[4096];
static size_t serial_line_size;
static size_t serial_line_offset;

void os_rplan_set_input(const uint8_t* data, size_t size) {
  input_data = data;
  input_size = size;
  input_offset = 0;
  input_boundary_sent = false;
  serial_after_boundary = false;
  serial_eof = false;
  serial_line_size = 0;
  serial_line_offset = 0;
}

void os_rplan_set_input_then_serial(const uint8_t* data, size_t size) {
  os_rplan_set_input(data, size);
  serial_after_boundary = true;
}

void os_rplan_set_output_enabled(bool enabled) {
  output_enabled = enabled;
}

bool os_rplan_output_enabled(void) {
  return output_enabled;
}

static pl_val want_nat(pl_thread* thread, pl_val value) {
  if (!pl_is_nat(value))
    pl_raise_msg(thread, "unknown actor/net op");
  return value;
}

static pl_val bytes_bar(pl_thread* thread, const uint8_t* bytes, size_t size) {
  uint8_t* copy = malloc(size + 1);
  if (copy == NULL)
    pl_raise_msg(thread, "oom");
  for (size_t i = 0; i < size; i++)
    copy[i] = bytes[i];
  copy[size] = 1;
  pl_val out = pl_nat_from_bytes(thread, copy, size + 1);
  free(copy);
  return out;
}

pl_val pl_op82_input(pl_thread* t, size_t ab) {
  uint64_t requested = pl_nat_u64_clamp(pl_nat_coerce(ARG(0)));
  if (requested == 0)
    return bytes_bar(t, &empty_input, 0);

  if (input_offset < input_size) {
    size_t remaining = input_size - input_offset;
    size_t take = requested < remaining ? (size_t)requested : remaining;
    pl_val out = bytes_bar(t, input_data + input_offset, take);
    input_offset += take;
    return out;
  }

  /* The buffered Reaver form must observe one real EOF before the serial
   * actor takes ownership of subsequent Input calls. */
  if (!input_boundary_sent) {
    input_boundary_sent = true;
    return bytes_bar(t, &empty_input, 0);
  }
  if (!serial_after_boundary || serial_eof)
    return bytes_bar(t, &empty_input, 0);

  if (serial_line_offset == serial_line_size) {
    serial_line_offset = 0;
    serial_line_size = 0;
    for (;;) {
      int raw = os_serial_getc();
      char c = (char)raw;
      if (c == 4 && serial_line_size == 0) { /* Ctrl-D */
        os_serial_write("^D\n", 3);
        serial_eof = true;
        return bytes_bar(t, &empty_input, 0);
      }
      if (c == 3) { /* Ctrl-C: discard without submitting an empty command. */
        os_serial_write("^C\n", 3);
        serial_line_size = 0;
        continue;
      }
      if (c == 8 || c == 127) {
        if (serial_line_size != 0) {
          serial_line_size--;
          os_serial_write("\b \b", 3);
        }
        continue;
      }
      if (c == '\r')
        c = '\n';
      if (c == '\n') {
        os_serial_putc('\n');
        serial_line[serial_line_size++] = (uint8_t)c;
        break;
      }
      if ((unsigned char)c < 0x20)
        continue;
      if (serial_line_size + 1 >= sizeof(serial_line)) {
        static const char too_long[] =
            "\nenki-os: Shrine line exceeds 4095 bytes; discarded\n";
        os_serial_write(too_long, sizeof(too_long) - 1);
        serial_line_size = 0;
        continue;
      }
      os_serial_putc(c);
      serial_line[serial_line_size++] = (uint8_t)c;
    }
  }
  size_t remaining = serial_line_size - serial_line_offset;
  size_t take = requested < remaining ? (size_t)requested : remaining;
  pl_val out = bytes_bar(t, serial_line + serial_line_offset, take);
  serial_line_offset += take;
  return out;
}

static pl_val output_bar(pl_thread* t, size_t ab) {
  pl_val value = pl_nat_coerce(ARG(0));
  size_t size = pl_nat_byte_len(value);
  if (size != 0)
    size--; /* discard bytesBar terminator */
  if (output_enabled)
    for (size_t i = 0; i < size; i++)
      os_serial_putc((char)pl_nat_byte_at(value, i));
  return 0;
}

pl_val pl_op82_output(pl_thread* t, size_t ab) { return output_bar(t, ab); }
pl_val pl_op82_warn(pl_thread* t, size_t ab) { return output_bar(t, ab); }

pl_val pl_op82_print(pl_thread* t, size_t ab) {
  pl_val value = want_nat(t, ARG(0));
  if (output_enabled)
    for (size_t i = 0; i < pl_nat_byte_len(value); i++)
      os_serial_putc((char)pl_nat_byte_at(value, i));
  return 0;
}

static char* nat_path(pl_thread* t, pl_val value) {
  value = want_nat(t, value);
  size_t size = pl_nat_byte_len(value);
  char* path = malloc(size + 1);
  if (path == NULL)
    pl_raise_msg(t, "oom");
  for (size_t i = 0; i < size; i++)
    path[i] = (char)pl_nat_byte_at(value, i);
  path[size] = '\0';
  return path;
}

pl_val pl_op82_read_file(pl_thread* t, size_t ab) {
  char* path = nat_path(t, ARG(0));
  const os_rom_file* file = os_rom_find(path);
  free(path);
  if (file == NULL)
    return 0;
  return bytes_bar(t, file->data, file->size);
}

pl_val pl_rplan_read_folder(pl_thread* t, pl_val path_value) {
  char* path = nat_path(t, path_value);
  size_t child_count = os_rom_children(path, NULL, 0);
  os_rom_child* children =
      child_count == 0 ? NULL : malloc(child_count * sizeof(*children));
  if (child_count != 0 && children == NULL) {
    free(path);
    pl_raise_msg(t, "oom");
  }
  size_t listed = os_rom_children(path, children, child_count);
  free(path);
  if (listed != child_count) {
    free(children);
    pl_raise_msg(t, "ReadFolder: ROM changed");
  }
  if (child_count == 0) {
    free(children);
    return 0;
  }

  size_t base = t->vsp;
  for (size_t i = 0; i < child_count; i++) {
    pl_vpush(t, children[i].folder ? 1 : 0);
    pl_vpush(t, pl_nat_from_bytes(t, (const uint8_t*)children[i].name,
                                  children[i].name_size));
    pl_gc_reserve(t, PL_APP_CELLS(2));
    PL_GC_FORBID(t);
    pl_val entry = pl_mk_app_from(t, 0, 2, &t->vstack[t->vsp - 2]);
    PL_GC_ALLOW(t);
    t->vsp -= 2;
    pl_vpush(t, entry);
  }
  free(children);
  pl_gc_reserve(t, PL_APP_CELLS(child_count));
  PL_GC_FORBID(t);
  pl_val row = pl_mk_app_from(t, 0, (uint32_t)child_count,
                              &t->vstack[base]);
  PL_GC_ALLOW(t);
  t->vsp = base;
  return row;
}

pl_val pl_op82_stamp(pl_thread* t, size_t ab) {
  char* path = nat_path(t, ARG(0));
  const os_rom_file* file = os_rom_find(path);
  free(path);
  return file == NULL ? 0 : file->stamp;
}

[[noreturn]] static void unsupported(pl_thread* t) {
  pl_raise_msg(t, "unsupported on enki-os");
}

pl_val pl_op82_write_file(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_now(pl_thread* t, size_t ab) {
  (void)ab;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    pl_raise_msg(t, "Now: clock failed");
  uint64_t ticks = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
                   (uint64_t)now.tv_nsec;
  /* ROM stamps occupy [0, 2^63).  Keeping the synthetic immutable-world
   * clock above that range makes Reaver's mtime-era freshness check valid. */
  uint64_t instant = (UINT64_C(1) << 63) + ticks;
  pl_gc_reserve(t, PL_NAT_CELLS(1));
  return pl_mk_nat_u64(t, instant);
}
pl_val pl_op82_closefd(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_connect(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_listen(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_accept(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_read(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_write(pl_thread* t, size_t ab) {
  uint64_t fd = pl_nat_u64_clamp(pl_nat_coerce(ARG(0)));
  if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
    unsupported(t);
  pl_val value = pl_nat_coerce(ARG(1));
  size_t size = pl_nat_byte_len(value);
  if (size != 0)
    size--; /* bytes-bar terminator */
  if (!output_enabled) {
    static const char shrine_banner[] = "Shrine serial console";
    if (size < sizeof(shrine_banner) - 1)
      return 0;
    for (size_t i = 0; i < sizeof(shrine_banner) - 1; i++)
      if (pl_nat_byte_at(value, i) != (uint8_t)shrine_banner[i])
        return 0;
    output_enabled = true;
  }
  for (size_t i = 0; i < size; i++)
    os_serial_putc((char)pl_nat_byte_at(value, i));
  return 0;
}

static pl_val request(pl_thread* t, size_t ab, uint32_t argc) {
  pl_gc_reserve(t, PL_APP_CELLS(argc));
  PL_GC_FORBID(t);
  pl_val out = pl_mk_app_from(t, t->vstack[ab - 1], argc, &t->vstack[ab]);
  PL_GC_ALLOW(t);
  return out;
}

pl_val pl_op82_spawn(pl_thread* t, size_t ab) { return request(t, ab, 1); }
pl_val pl_op82_send(pl_thread* t, size_t ab) {
  (void)want_nat(t, ARG(0));
  return request(t, ab, 2);
}
pl_val pl_op82_send_caps(pl_thread* t, size_t ab) {
  (void)want_nat(t, ARG(0));
  return request(t, ab, 3);
}
pl_val pl_op82_recv(pl_thread* t, size_t ab) {
  if (ARG(0) != 0)
    pl_raise_msg(t, "unknown actor/net op");
  return request(t, ab, 1);
}
pl_val pl_op82_close_handle(pl_thread* t, size_t ab) {
  (void)want_nat(t, ARG(0));
  return request(t, ab, 1);
}
pl_val pl_op83_read_folder(pl_thread* t, size_t ab) {
  (void)want_nat(t, ARG(0));
  return request(t, ab, 1);
}
pl_val pl_op83_fetch(pl_thread* t, size_t ab) {
  return request(t, ab, 2);
}
pl_val pl_op83_sleep(pl_thread* t, size_t ab) {
  (void)want_nat(t, ARG(0));
  return request(t, ab, 1);
}
