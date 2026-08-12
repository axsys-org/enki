#ifndef ENKI_OS_BOOT_H
#define ENKI_OS_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os_boot_info {
  uintptr_t heap_begin;
  uintptr_t heap_end;
  const char* command_line;
} os_boot_info;

bool os_multiboot_parse(uint32_t magic, uintptr_t address, os_boot_info* out);
void os_idt_init(void);

#endif
