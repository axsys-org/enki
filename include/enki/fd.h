#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FD_MAX 1024 
#define ENKI_FD_INVALID FD_MAX

typedef struct {
  bool live;
  int fd;
  size_t refs;
} enki_fd_slot;

typedef struct {
  size_t next;
  enki_fd_slot slots[FD_MAX];
} enki_fd_table;

extern enki_fd_table enki_global_fd_table;

size_t enki_fd_add(int fd);
int enki_fd_get(size_t hdl);
int enki_fd_close(size_t hdl);
int enki_fd_retain(size_t hdl);
int enki_fd_release(size_t hdl);
