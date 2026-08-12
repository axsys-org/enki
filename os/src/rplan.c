#include <stdint.h>
#include <stdlib.h>

#include "internal.h"
#include "os/platform.h"
#include "plan/build.h"
#include "plan/nat.h"

#define ARG(i) (t->vstack[ab + (i)])

static const uint8_t* input_data;
static size_t input_size;
static size_t input_offset;
static const uint8_t empty_input;
static bool output_enabled = true;

void os_rplan_set_input(const uint8_t* data, size_t size) {
  input_data = data;
  input_size = size;
  input_offset = 0;
}

void os_rplan_set_output_enabled(bool enabled) {
  output_enabled = enabled;
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
  size_t remaining = input_size - input_offset;
  size_t take = requested < remaining ? (size_t)requested : remaining;
  const uint8_t* bytes = take ? input_data + input_offset : &empty_input;
  pl_val out = bytes_bar(t, bytes, take);
  input_offset += take;
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
pl_val pl_op82_now(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_closefd(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_connect(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_listen(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_accept(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_read(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_write(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_spawn(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_send(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_send_caps(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_recv(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op82_close_handle(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op83_read_folder(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op83_fetch(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
pl_val pl_op83_sleep(pl_thread* t, size_t ab) { (void)ab; unsupported(t); }
