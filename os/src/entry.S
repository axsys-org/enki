.section .multiboot,"a"
.align 8
mb2_header:
    .long 0xe85250d6
    .long 0
    .long mb2_header_end - mb2_header
    .long -(0xe85250d6 + 0 + (mb2_header_end - mb2_header))
    .short 0
    .short 0
    .long 8
mb2_header_end:

/* QEMU's ELF64 -kernel loader enters through Xen's physical 32-bit entry
 * note.  Firmware/bootloaders can use the Multiboot2 header above. */
.section .note.Xen,"a",@note
.align 4
    .long 4
    .long 4
    .long 18
    .asciz "Xen"
    .long _start

.section .text.boot,"ax"
.code32
.global _start
.type _start,@function
_start:
    cli
    movl %eax, mb_magic
    movl %ebx, mb_info
    movl $boot_stack_top, %esp

    movl $pd0, %edi
    xorl %ebx, %ebx
    movl $2048, %ecx
1:
    movl %ebx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $8, %edi
    incl %ebx
    loop 1b

    movl $pd0, %eax
    orl $0x03, %eax
    movl %eax, pdpt + 0
    movl $pd1, %eax
    orl $0x03, %eax
    movl %eax, pdpt + 8
    movl $pd2, %eax
    orl $0x03, %eax
    movl %eax, pdpt + 16
    movl $pd3, %eax
    orl $0x03, %eax
    movl %eax, pdpt + 24
    movl $pdpt, %eax
    orl $0x03, %eax
    movl %eax, pml4

    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4
    movl $pml4, %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0
    lgdt gdt_ptr32
    ljmp $0x08, $long_mode

.code64
long_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movq $boot_stack_top, %rsp
    xorq %rbp, %rbp
    movl mb_magic(%rip), %edi
    movl mb_info(%rip), %esi
    call os_boot_entry
2:
    cli
    hlt
    jmp 2b

.section .rodata
.align 8
gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
gdt_end:
gdt_ptr32:
    .word gdt_end - gdt - 1
    .long gdt

.section .bss
.align 4096
pml4: .skip 4096
pdpt: .skip 4096
pd0:  .skip 4096
pd1:  .skip 4096
pd2:  .skip 4096
pd3:  .skip 4096
.align 16
boot_stack: .skip 65536
boot_stack_top:
.align 4
mb_magic: .skip 4
mb_info:  .skip 4
