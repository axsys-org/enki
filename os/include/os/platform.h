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

void os_serial_init(void);
void os_serial_putc(char c);
int os_serial_getc(void);
void os_serial_write(const void* data, size_t size);
void os_qemu_exit(uint32_t code);
[[noreturn]] void os_halt(void);

void os_memory_init(uintptr_t begin, uintptr_t end);
size_t os_memory_available(void);

const os_rom_file* os_rom_find(const char* path);
const os_rom_file* os_rom_files(size_t* count);

void os_rplan_set_input(const uint8_t* data, size_t size);
void os_rplan_set_output_enabled(bool enabled);

uint64_t os_fnv1a64(const void* data, size_t size);

#endif
