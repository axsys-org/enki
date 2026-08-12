#ifndef ENKI_OS_STDIO_H
#define ENKI_OS_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct os_FILE {
  int kind;
  int error;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);
int vfprintf(FILE* stream, const char* fmt, va_list ap);
int snprintf(char* dst, size_t cap, const char* fmt, ...);
int vsnprintf(char* dst, size_t cap, const char* fmt, va_list ap);
int fputc(int c, FILE* stream);
int putchar(int c);
int puts(const char* s);
int fflush(FILE* stream);
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t count, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t count, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
void perror(const char* prefix);

#endif
