#include "test_interp.h"
#include "enki/interp.h"
#include "enki/pin.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static enki_interpreter* fuzz_interp;

static void cleanup_interp(void)
{
    if(fuzz_interp != NULL) {
        enki_test_interp_destroy(fuzz_interp);
        fuzz_interp = NULL;
    }
}

static enki_interpreter* get_interp(void)
{
    if(fuzz_interp == NULL) {
        fuzz_interp = enki_test_interp_create(1024 * 1024, 0);
        if(fuzz_interp == NULL) abort();
        atexit(cleanup_interp);
    }
    return fuzz_interp;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    enki_interpreter* i = get_interp();
    uint8_t bytes_b[512];
    size_t n_bytes_s = size;
    size_t off_s = 0;

    if(n_bytes_s > sizeof(bytes_b)) {
        n_bytes_s = sizeof(bytes_b);
    }
    if(n_bytes_s > 0) {
        memcpy(bytes_b, data, n_bytes_s);
    }

    i->sp = 0;
    i->cp = 0;
    i->halted = false;
    i->has_error_jmp = true;
    if(setjmp(i->error_jmp) == 0) {
        (void)enki_pin_deserialize(i, bytes_b, n_bytes_s, &off_s);
    }
    i->has_error_jmp = false;
    i->sp = 0;
    enki_arena_reset(i->scratch_a);
    return 0;
}
