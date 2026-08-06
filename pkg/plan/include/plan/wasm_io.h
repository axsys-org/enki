#ifndef PL_WASM_IO_H
#define PL_WASM_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  PL_WASM_IO_STDOUT = 1,
  PL_WASM_IO_STDERR = 2,
} pl_wasm_io_channel;

typedef struct pl_wasm_io {
  void* ctx;
  size_t (*input)(void* ctx, uint8_t* out, size_t max);
  void (*output)(void* ctx, pl_wasm_io_channel channel, const uint8_t* bytes,
                 size_t len);
  bool (*read_file)(void* ctx, const char* root, const char* path,
                    uint8_t** out_bytes, size_t* out_len);
  bool (*write_file)(void* ctx, const char* root, const char* path,
                     const uint8_t* bytes, size_t len);
  bool (*stamp)(void* ctx, const char* root, const char* path,
                uint64_t* out_mtime);
  uint64_t (*now)(void* ctx);
} pl_wasm_io;

#endif
