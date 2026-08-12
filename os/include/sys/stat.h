#ifndef ENKI_OS_SYS_STAT_H
#define ENKI_OS_SYS_STAT_H
#include <sys/types.h>
struct stat { off_t st_size; time_t st_mtime; mode_t st_mode; };
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & 0170000) == S_IFDIR)
int mkdir(const char* path, mode_t mode);
int stat(const char* path, struct stat* st);
int lstat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);
#endif
