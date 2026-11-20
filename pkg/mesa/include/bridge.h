#ifndef ENKI_MESA_BRIDGE_H
#define ENKI_MESA_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
typedef struct em_handle em_handle;
typedef struct em_reply em_reply;
/* Internal ABI, not a representation exposed to PLAN. Handles must be live,
 * rooted resource allocations from this bridge. All calls are serialized per
 * handle; repeated handles in a single call are rejected. */
em_reply* em_call(const char* op, em_handle* const* handles, size_t count,
                  const uint8_t* data, size_t len);
uint32_t em_reply_status(const em_reply* r);
const uint8_t* em_reply_bytes(const em_reply* r, size_t* len);
size_t em_reply_count(const em_reply* r);
em_handle* em_reply_take(em_reply* r, size_t i);
void em_reply_free(em_reply* r);
void em_handle_free(void* h);
size_t em_handle_charge(const em_handle* h);
#endif
