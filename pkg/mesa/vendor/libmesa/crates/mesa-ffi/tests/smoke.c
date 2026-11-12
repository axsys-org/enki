#include "mesa.h"
#include <assert.h>
#include <string.h>

int main(void) {
  /* Hoon-extracted peek-hop-saturate vector. */
  const uint8_t wire[] = {
    0x10,0x1f,0x8a,0x43,0x00,0x02,0xe0,0x67,
    0x10,0x00,0x00,0x01,0x0d,0x01,0x00,0x61
  };
  uint8_t out[sizeof wire], bad[sizeof wire];
  size_t written = 99;
  assert(mesa_wire_validate(wire, sizeof wire, MESA_HOON_ATOM) == MESA_OK);
  assert(mesa_wire_validate(wire, sizeof wire, MESA_VERE_BYTES) == MESA_OK);
  assert(mesa_wire_roundtrip(wire, sizeof wire, MESA_VERE_BYTES,
                            NULL, 0, &written) == MESA_CAPACITY);
  assert(written == sizeof wire);
  memset(out, 0xa5, sizeof out);
  assert(mesa_wire_roundtrip(wire, sizeof wire, MESA_VERE_BYTES,
                            out, sizeof out - 1, &written) == MESA_CAPACITY);
  for (size_t i = 0; i < sizeof out; ++i) assert(out[i] == 0xa5);
  assert(mesa_wire_roundtrip(wire, sizeof wire, MESA_VERE_BYTES,
                            out, sizeof out, &written) == MESA_OK);
  assert(written == sizeof wire && memcmp(out, wire, sizeof wire) == 0);
  memcpy(bad, wire, sizeof wire); bad[8] |= 0x20;
  assert(mesa_wire_validate(bad, sizeof bad, MESA_VERE_BYTES) == MESA_INVALID);
  assert(mesa_wire_validate(wire, 7, MESA_VERE_BYTES) == MESA_INVALID);
  assert(mesa_wire_validate(NULL, 0, MESA_VERE_BYTES) == MESA_ARGUMENT);
  assert(mesa_wire_validate(wire, sizeof wire, 2) == MESA_ARGUMENT);
  assert(mesa_wire_roundtrip(wire, sizeof wire, 2, out, sizeof out, &written) == MESA_ARGUMENT);
  assert(written == 0 && memcmp(out, wire, sizeof wire) == 0);
  assert(mesa_wire_roundtrip(wire, sizeof wire, MESA_VERE_BYTES,
                            out, sizeof out, NULL) == MESA_ARGUMENT);
  /* Independently specified v2 peek-zero fixture. */
  const uint8_t v2[] = {
    0x20,0x11,0xbe,0x61,0x00,0x02,0xe0,0x67,
    0x10,0x06,0x67,0x77,0x2d,0x62,0x74,0x63,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x0d,0x01,0x00,0x61,
  };
  uint8_t v2out[sizeof v2], v2bad[sizeof v2];
  assert(mesa_wire_v2_validate(v2, sizeof v2) == MESA_OK);
  assert(mesa_wire_v2_validate(wire, sizeof wire) == MESA_INVALID);
  assert(mesa_wire_validate(v2, sizeof v2, MESA_HOON_ATOM) == MESA_INVALID);
  assert(mesa_wire_validate(v2, sizeof v2, MESA_VERE_BYTES) == MESA_INVALID);
  assert(mesa_wire_v2_roundtrip(v2, sizeof v2, NULL, 0, &written) == MESA_CAPACITY);
  assert(written == sizeof v2);
  memset(v2out, 0xa5, sizeof v2out);
  assert(mesa_wire_v2_roundtrip(v2, sizeof v2, v2out, sizeof v2out - 1, &written) == MESA_CAPACITY);
  assert(written == sizeof v2);
  for (size_t i = 0; i < sizeof v2out; ++i) assert(v2out[i] == 0xa5);
  assert(mesa_wire_v2_roundtrip(v2, sizeof v2, v2out, sizeof v2out, &written) == MESA_OK);
  assert(written == sizeof v2 && memcmp(v2out, v2, sizeof v2) == 0);
  memcpy(v2bad, v2, sizeof v2); v2bad[8] |= 1;
  assert(mesa_wire_v2_validate(v2bad, sizeof v2bad) == MESA_INVALID);
  assert(mesa_wire_v2_roundtrip(v2bad, sizeof v2bad, v2out, sizeof v2out, &written) == MESA_INVALID);
  assert(written == 0 && memcmp(v2out, v2, sizeof v2) == 0);
  assert(mesa_wire_v2_validate(NULL, 0) == MESA_ARGUMENT);
  assert(mesa_wire_v2_validate(v2, 7) == MESA_INVALID);
  assert(mesa_wire_v2_roundtrip(v2, sizeof v2, NULL, sizeof v2, &written) == MESA_ARGUMENT);
  assert(written == 0);
  assert(mesa_wire_v2_roundtrip(v2, sizeof v2, v2out, sizeof v2out, NULL) == MESA_ARGUMENT);
  assert(mesa_wire_v2_roundtrip(wire, sizeof wire, v2out, sizeof v2out, &written) == MESA_INVALID);
  assert(written == 0 && memcmp(v2out, v2, sizeof v2) == 0);
  return 0;
}
