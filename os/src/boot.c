#include "os/boot.h"

#include <stdio.h>
#include <string.h>

#include "os/platform.h"

#define MULTIBOOT2_BOOT_MAGIC UINT32_C(0x36d76289)
#define MULTIBOOT1_BOOT_MAGIC UINT32_C(0x2badb002)
#define XEN_HVM_START_MAGIC UINT32_C(0x336ec578)
#define MULTIBOOT_TAG_END 0
#define MULTIBOOT_TAG_CMDLINE 1
#define MULTIBOOT_TAG_MMAP 6
#define FOUR_GIB UINT64_C(0x100000000)

typedef struct mb_tag {
  uint32_t type;
  uint32_t size;
} mb_tag;

typedef struct mb_mmap_entry {
  uint64_t addr;
  uint64_t len;
  uint32_t type;
  uint32_t zero;
} mb_mmap_entry;

typedef struct mb_mmap_tag {
  mb_tag tag;
  uint32_t entry_size;
  uint32_t version;
  mb_mmap_entry entries[];
} mb_mmap_tag;

typedef struct mb1_info {
  uint32_t flags;
  uint32_t mem_lower;
  uint32_t mem_upper;
  uint32_t boot_device;
  uint32_t cmdline;
  uint32_t mods_count;
  uint32_t mods_addr;
  uint32_t syms[4];
  uint32_t mmap_length;
  uint32_t mmap_addr;
} mb1_info;

typedef struct mb1_mmap_entry {
  uint32_t size;
  uint64_t addr;
  uint64_t len;
  uint32_t type;
} __attribute__((packed)) mb1_mmap_entry;

typedef struct hvm_start_info {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t nr_modules;
  uint64_t modlist_paddr;
  uint64_t cmdline_paddr;
  uint64_t rsdp_paddr;
  uint64_t memmap_paddr;
  uint32_t memmap_entries;
  uint32_t reserved;
} hvm_start_info;

typedef struct hvm_memmap_entry {
  uint64_t addr;
  uint64_t size;
  uint32_t type;
  uint32_t reserved;
} hvm_memmap_entry;

extern char __kernel_end[];

static uintptr_t align_up(uintptr_t value, uintptr_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static void consider_region(uint64_t address, uint64_t length,
                            uintptr_t kernel_end, uintptr_t* best_begin,
                            uintptr_t* best_end) {
  if (length == 0)
    return;
  uint64_t raw_end = address + length;
  if (raw_end < address)
    raw_end = UINT64_MAX;
  uint64_t lo64 = address > kernel_end ? address : kernel_end;
  uint64_t hi64 = raw_end < FOUR_GIB ? raw_end : FOUR_GIB;
  uintptr_t lo = align_up((uintptr_t)lo64, 16);
  uintptr_t hi = (uintptr_t)(hi64 & ~UINT64_C(15));
  if (hi > lo && hi - lo > *best_end - *best_begin) {
    *best_begin = lo;
    *best_end = hi;
  }
}

static bool parse_multiboot1(uintptr_t address, uintptr_t kernel_end,
                             uintptr_t* best_begin, uintptr_t* best_end,
                             const char** command_line) {
  const mb1_info* info = (const mb1_info*)address;
  if ((info->flags & (UINT32_C(1) << 2)) != 0 && info->cmdline != 0)
    *command_line = (const char*)(uintptr_t)info->cmdline;
  if ((info->flags & (UINT32_C(1) << 6)) != 0) {
    uintptr_t cursor = info->mmap_addr;
    uintptr_t limit = cursor + info->mmap_length;
    if (limit < cursor)
      return false;
    while (cursor + sizeof(uint32_t) <= limit) {
      const mb1_mmap_entry* entry = (const mb1_mmap_entry*)cursor;
      uintptr_t next = cursor + sizeof(entry->size) + entry->size;
      if (entry->size < sizeof(*entry) - sizeof(entry->size) || next > limit)
        return false;
      if (entry->type == 1)
        consider_region(entry->addr, entry->len, kernel_end,
                        best_begin, best_end);
      cursor = next;
    }
  } else if ((info->flags & 1) != 0) {
    uint64_t end = ((uint64_t)info->mem_upper + 1024) * 1024;
    consider_region(0, end, kernel_end, best_begin, best_end);
  } else {
    return false;
  }
  return true;
}

static bool parse_pvh(uintptr_t address, uintptr_t kernel_end,
                      uintptr_t* best_begin, uintptr_t* best_end,
                      const char** command_line) {
  const hvm_start_info* info = (const hvm_start_info*)address;
  if (info->magic != XEN_HVM_START_MAGIC || info->version < 1)
    return false;
  if (info->cmdline_paddr != 0)
    *command_line = (const char*)(uintptr_t)info->cmdline_paddr;
  const hvm_memmap_entry* entries =
      (const hvm_memmap_entry*)(uintptr_t)info->memmap_paddr;
  if (entries == NULL || info->memmap_entries == 0)
    return false;
  for (uint32_t i = 0; i < info->memmap_entries; i++)
    if (entries[i].type == 1)
      consider_region(entries[i].addr, entries[i].size, kernel_end,
                      best_begin, best_end);
  return true;
}

bool os_multiboot_parse(uint32_t magic, uintptr_t address, os_boot_info* out) {
  if (address == 0 || out == NULL)
    return false;

  uintptr_t kernel_end = align_up((uintptr_t)__kernel_end, 4096);
  uintptr_t best_begin = 0;
  uintptr_t best_end = 0;
  const char* command_line = "";

  if (*(const uint32_t*)address == XEN_HVM_START_MAGIC) {
    if (!parse_pvh(address, kernel_end, &best_begin, &best_end,
                   &command_line))
      return false;
  } else if (magic == MULTIBOOT1_BOOT_MAGIC) {
    if (!parse_multiboot1(address, kernel_end, &best_begin, &best_end,
                          &command_line))
      return false;
  } else if (magic == MULTIBOOT2_BOOT_MAGIC) {
    uint32_t total_size = *(const uint32_t*)address;
    if (total_size < 16)
      return false;

    uintptr_t cursor = address + 8;
    uintptr_t limit = address + total_size;
    while (cursor + sizeof(mb_tag) <= limit) {
      const mb_tag* tag = (const mb_tag*)cursor;
      if (tag->size < sizeof(*tag) || cursor + tag->size > limit)
        return false;
      if (tag->type == MULTIBOOT_TAG_END)
        break;
      if (tag->type == MULTIBOOT_TAG_CMDLINE) {
        command_line = (const char*)(tag + 1);
      } else if (tag->type == MULTIBOOT_TAG_MMAP) {
        const mb_mmap_tag* map = (const mb_mmap_tag*)tag;
        if (map->entry_size < sizeof(mb_mmap_entry))
          return false;
        uintptr_t pos = (uintptr_t)map->entries;
        uintptr_t map_end = cursor + tag->size;
        while (pos + map->entry_size <= map_end) {
          const mb_mmap_entry* entry = (const mb_mmap_entry*)pos;
          if (entry->type == 1)
            consider_region(entry->addr, entry->len, kernel_end,
                            &best_begin, &best_end);
          pos += map->entry_size;
        }
      }
      cursor = align_up(cursor + tag->size, 8);
    }
  } else return false;

  if (best_end <= best_begin || best_end - best_begin < (size_t)1 << 28)
    return false;
  *out = (os_boot_info){
      .heap_begin = best_begin,
      .heap_end = best_end,
      .command_line = command_line,
  };
  return true;
}

typedef struct idt_gate {
  uint16_t offset0;
  uint16_t selector;
  uint8_t ist;
  uint8_t attributes;
  uint16_t offset1;
  uint32_t offset2;
  uint32_t reserved;
} __attribute__((packed)) idt_gate;

typedef struct idt_pointer {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed)) idt_pointer;

typedef struct exception_frame {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
  uint64_t vector, error, rip, cs, flags;
} exception_frame;

extern const uintptr_t os_isr_stub_table[32];
static idt_gate idt[256];

static void idt_set(size_t vector, uintptr_t handler) {
  idt[vector] = (idt_gate){
      .offset0 = (uint16_t)handler,
      .selector = 0x08,
      .ist = 0,
      .attributes = 0x8e,
      .offset1 = (uint16_t)(handler >> 16),
      .offset2 = (uint32_t)(handler >> 32),
      .reserved = 0,
  };
}

void os_idt_init(void) {
  memset(idt, 0, sizeof(idt));
  for (size_t i = 0; i < 32; i++)
    idt_set(i, os_isr_stub_table[i]);
  idt_pointer pointer = {.limit = sizeof(idt) - 1, .base = (uintptr_t)idt};
  __asm__ volatile("lidt %0" : : "m"(pointer));
}

[[noreturn]] void os_exception(exception_frame* frame) {
  fprintf(stderr,
          "\nFATAL: x86 exception %lu error=0x%lx rip=0x%lx cr2=0x%lx\n",
          frame->vector, frame->error, frame->rip,
          ({ unsigned long cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2)); cr2; }));
  os_halt();
}
