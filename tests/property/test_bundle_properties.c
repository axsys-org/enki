#include "test_bundle.h"
#include "theft.h"
#include <sys/wait.h>
#include <unistd.h>

static void* generate_corruption(theft_rng* rng, void* ctx) {
  (void)ctx;
  uint64_t* seed = malloc(sizeof(*seed));
  if (seed != NULL) {
    *seed = theft_rng_next_u64(rng);
  }
  return seed;
}
static void free_corruption(void* generated, void* ctx) {
  (void)ctx;
  free(generated);
}
static bool corruption_exits_126(const void* generated, void* ctx,
                                 char* message, size_t cap) {
  (void)ctx;
  uint8_t* blob = NULL;
  size_t len = 0;
  if (!test_bundle_make(&blob, &len)) {
    return false;
  }
  theft_rng rng;
  theft_rng_seed(&rng, *(const uint64_t*)generated);
  size_t at = theft_rng_range(&rng, len);
  blob[at] ^= (uint8_t)(1U << theft_rng_range(&rng, 8));
  pid_t pid = fork();
  if (pid == 0) {
    _exit(test_bundle_boot(blob, len));
  }
  free(blob);
  int status = 0;
  if (pid <= 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 126) {
    (void)snprintf(
        message, cap,
        "corruption at %zu crashed or escaped validation (status %d)", at,
        status);
    return false;
  }
  return true;
}
int main(void) {
  theft_property property = {.name_v =
                                 "corrupted bundle exits 126 without crashing",
                             .trials = 250,
                             .generate = generate_corruption,
                             .check = corruption_exits_126,
                             .free = free_corruption};
  return theft_run_property(
      &property, theft_seed_from_env("THEFT_SEED", UINT64_C(0xb00d1e)));
}
