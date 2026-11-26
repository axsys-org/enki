#ifndef EN_BUNDLE_H
#define EN_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EN_BUNDLE_TRAILER_SIZE 60u

typedef struct en_bundle {
  const uint8_t* payload;
  size_t payload_len;
  uint8_t entry_hash[32];
  void* mapping;
  size_t mapping_len;
} en_bundle;

/* Strict validation of exactly [payload][trailer], including CRC32C. */
bool en_bundle_validate(const uint8_t* blob, size_t len, en_bundle* out,
                        char* err, size_t cap);
bool en_bundle_blob(const uint8_t* payload, size_t len, const uint8_t hash[32],
                    uint8_t** out, size_t* out_len, char* err, size_t cap);
/* 0 absent, 1 present, -1 malformed or unreadable. */
int en_bundle_locate(en_bundle* out, char* err, size_t cap);
void en_bundle_close(en_bundle* bundle);
/* Copies self into a same-directory temporary, replaces payload, and publishes.
 */
bool en_bundle_write(const char* path, const uint8_t* blob, size_t len,
                     char* err, size_t cap);

#endif
