#ifndef ENKI_OS_UNISTD_H
#define ENKI_OS_UNISTD_H
#include <stddef.h>
#include <stdint.h>
typedef long ssize_t;
typedef long off_t;
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
int access(const char* path, int mode);
int isatty(int fd);
unsigned sleep(unsigned seconds);
#endif
