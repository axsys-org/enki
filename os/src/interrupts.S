.section .text,"ax"
.code64

.macro ISR_NOERR n
.global os_isr_\n
os_isr_\n:
    pushq $0
    pushq $\n
    jmp os_isr_common
.endm

.macro ISR_ERR n
.global os_isr_\n
os_isr_\n:
    pushq $\n
    jmp os_isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

os_isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    andq $-16, %rsp
    call os_exception
1:  hlt
    jmp 1b

.section .rodata,"a"
.global os_isr_stub_table
os_isr_stub_table:
    .quad os_isr_0, os_isr_1, os_isr_2, os_isr_3
    .quad os_isr_4, os_isr_5, os_isr_6, os_isr_7
    .quad os_isr_8, os_isr_9, os_isr_10, os_isr_11
    .quad os_isr_12, os_isr_13, os_isr_14, os_isr_15
    .quad os_isr_16, os_isr_17, os_isr_18, os_isr_19
    .quad os_isr_20, os_isr_21, os_isr_22, os_isr_23
    .quad os_isr_24, os_isr_25, os_isr_26, os_isr_27
    .quad os_isr_28, os_isr_29, os_isr_30, os_isr_31
