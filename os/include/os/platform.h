#ifndef ENKI_OS_PLATFORM_H
#define ENKI_OS_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os_rom_file {
  const char* path;
  const uint8_t* data;
  size_t size;
  uint64_t stamp;
} os_rom_file;

typedef struct os_rom_child {
  const char* name;
  size_t name_size;
  bool folder;
} os_rom_child;

void os_serial_init(void);
void os_serial_putc(char c);
int os_serial_getc(void);
void os_serial_write(const void* data, size_t size);
void os_qemu_exit(uint32_t code);
[[noreturn]] void os_halt(void);

void os_memory_init(uintptr_t begin, uintptr_t end);
size_t os_memory_available(void);
void os_arena_set_store_limit(size_t size);

const os_rom_file* os_rom_find(const char* path);
const os_rom_file* os_rom_files(size_t* count);
size_t os_rom_children(const char* path, os_rom_child* children,
                       size_t capacity);

void os_rplan_set_input(const uint8_t* data, size_t size);
void os_rplan_set_input_then_serial(const uint8_t* data, size_t size);
void os_rplan_set_output_enabled(bool enabled);
bool os_rplan_output_enabled(void);

uint64_t os_fnv1a64(const void* data, size_t size);

#endif
