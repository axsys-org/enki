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

static bool folder_path(const char* input, const char** path, size_t* size) {
  if (input == NULL)
    return false;
  while (*input == '/')
    input++;
  while (input[0] == '.' && input[1] == '/')
    input += 2;
  size_t n = strlen(input);
  while (n != 0 && input[n - 1] == '/')
    n--;
  if (n == 1 && input[0] == '.')
    n = 0;
  for (size_t begin = 0; begin < n;) {
    size_t end = begin;
    while (end < n && input[end] != '/')
      end++;
    size_t segment = end - begin;
    if (segment == 0 ||
        (segment == 1 && input[begin] == '.') ||
        (segment == 2 && input[begin] == '.' && input[begin + 1] == '.'))
      return false;
    begin = end + 1;
  }
  *path = input;
  *size = n;
  return true;
}

size_t os_rom_children(const char* input, os_rom_child* children,
                       size_t capacity) {
  const char* path;
  size_t path_size;
  if (!folder_path(input, &path, &path_size))
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < os_generated_rom_count; i++) {
    const char* candidate = os_generated_rom[i].path;
    const char* rest = candidate;
    if (path_size != 0) {
      if (strncmp(candidate, path, path_size) != 0 ||
          candidate[path_size] != '/')
        continue;
      rest = candidate + path_size + 1;
    }
    if (*rest == '\0')
      continue;
    const char* slash = strchr(rest, '/');
    size_t name_size = slash == NULL ? strlen(rest) : (size_t)(slash - rest);
    bool is_folder = slash != NULL;
    if (count != 0) {
      if (children != NULL && count - 1 < capacity) {
        os_rom_child* previous = &children[count - 1];
        if (
          previous->name_size == name_size &&
          memcmp(previous->name, rest, name_size) == 0) {
          previous->folder |= is_folder;
          continue;
        }
      }
      /* Counting calls have no output row to compare.  Sorted ROM paths make
       * the immediately preceding candidate sufficient for deduplication. */
      const char* prior = os_generated_rom[i - 1].path;
      const char* prior_rest = prior;
      if (path_size != 0 && strncmp(prior, path, path_size) == 0 &&
          prior[path_size] == '/')
        prior_rest = prior + path_size + 1;
      const char* prior_slash = strchr(prior_rest, '/');
      size_t prior_size = prior_slash == NULL
                              ? strlen(prior_rest)
                              : (size_t)(prior_slash - prior_rest);
      if (prior_size == name_size &&
          memcmp(prior_rest, rest, name_size) == 0)
        continue;
    }
    if (children != NULL && count < capacity)
      children[count] = (os_rom_child){.name = rest,
                                      .name_size = name_size,
                                      .folder = is_folder};
    count++;
  }
  return count;
}
