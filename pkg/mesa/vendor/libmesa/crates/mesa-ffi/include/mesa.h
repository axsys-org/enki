#ifndef LIBMESA_WIRE_H
#define LIBMESA_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MESA_HOON_ATOM = 0, MESA_VERE_BYTES = 1 };
enum {
  MESA_OK = 0, MESA_ARGUMENT = 1, MESA_INVALID = 2,
  MESA_CAPACITY = 3, MESA_PANIC = 4
};

/* Historical v1-only ABI: datagrams <=1472 bytes, fragments <=1024 bytes.
 * Validation checks structure and checksum, not signatures or LSS proofs.
 * data must address len readable bytes and remain valid during the call.
 * No pointer is retained. Unknown checksum modes return MESA_ARGUMENT.
 */
int32_t mesa_wire_validate(const uint8_t *data, size_t len, uint32_t checksum);

/* Caller owns all buffers; data, out, and written must not overlap.
 * written must address an aligned, writable size_t. If out is non-NULL,
 * it must address capacity writable bytes. No allocation crosses the ABI.
 * MESA_CAPACITY sets *written to the required size and leaves out untouched;
 * out=NULL with capacity=0 is a size query. Other errors set *written=0
 * and leave out untouched. Allocation failure can terminate the process.
 */
int32_t mesa_wire_roundtrip(const uint8_t *data, size_t len, uint32_t checksum,
                           uint8_t *out, size_t capacity, size_t *written);

/* Experimental v2 only: fixed 128-bit names, explicit domain selectors,
 * length-prefixed signatures, checksum over exact body bytes. V1 is rejected;
 * no automatic version/checksum fallback. These functions use the same limits,
 * pointer ownership, capacity/error behavior, and unauthenticated-validation
 * boundary documented above. Signature algorithm/validity and domain authority
 * are not established by successful validation.
 */
int32_t mesa_wire_v2_validate(const uint8_t *data, size_t len);
int32_t mesa_wire_v2_roundtrip(const uint8_t *data, size_t len,
                              uint8_t *out, size_t capacity, size_t *written);

#ifdef __cplusplus
}
#endif
#endif
