OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
  . = 1M;
  __kernel_start = .;

  .multiboot ALIGN(8) : {
    KEEP(*(.multiboot))
  }

  .note.xen ALIGN(4) : {
    KEEP(*(.note.Xen))
  }

  .text ALIGN(4K) : {
    *(.text.boot)
    *(.text .text.*)
  }

  .rodata ALIGN(4K) : {
    *(.rodata .rodata.*)
  }

  .data ALIGN(4K) : {
    *(.data .data.*)
  }

  .bss ALIGN(4K) (NOLOAD) : {
    __bss_start = .;
    *(COMMON)
    *(.bss .bss.*)
    __bss_end = .;
  }

  __kernel_end = ALIGN(4K);

  /DISCARD/ : {
    *(.comment)
    *(.eh_frame .eh_frame_hdr)
    *(.note .note.*)
  }
}
