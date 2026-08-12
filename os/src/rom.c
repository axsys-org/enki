#include "os/platform.h"

#include <string.h>

extern const os_rom_file os_generated_rom[];
extern const size_t os_generated_rom_count;

static const char* normalize(const char* path) {
  while (*path == '/')
    path++;
  while (path[0] == '.' && path[1] == '/')
    path += 2;
  return path;
}

const os_rom_file* os_rom_find(const char* path) {
  if (path == NULL)
    return NULL;
  path = normalize(path);
  if (*path == '\0' || strstr(path, "../") != NULL ||
      strcmp(path, "..") == 0)
    return NULL;
  size_t lo = 0;
  size_t hi = os_generated_rom_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int order = strcmp(path, os_generated_rom[mid].path);
    if (order == 0)
      return &os_generated_rom[mid];
    if (order < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

const os_rom_file* os_rom_files(size_t* count) {
  if (count != NULL)
    *count = os_generated_rom_count;
  return os_generated_rom;
}
