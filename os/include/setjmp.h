#ifndef ENKI_OS_SETJMP_H
#define ENKI_OS_SETJMP_H

typedef struct os_jmp_buf {
  unsigned long rbx, rbp, r12, r13, r14, r15, rsp, rip;
} jmp_buf[1];

int setjmp(jmp_buf env) __attribute__((returns_twice));
[[noreturn]] void longjmp(jmp_buf env, int value);

#endif
