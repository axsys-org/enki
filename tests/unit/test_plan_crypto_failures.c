#include <stdbool.h>
#include <sodium.h>

/* Compile the real primops into this test executable so backend failures can
 * be injected without adding mutable test hooks to the runtime API.  The
 * archive's rplan.o is not needed: these definitions satisfy its references.
 * Reuse the op83 suite to exercise successful calls through the same code.
 */
static bool test_sign_fails;

static int test_sign_detached(unsigned char* signature,
                              unsigned long long* signature_len,
                              const unsigned char* message,
                              unsigned long long message_len,
                              const unsigned char* key) {
  if (test_sign_fails)
    return -1;
  return crypto_sign_ed25519_detached(signature, signature_len, message,
                                       message_len, key);
}

#define crypto_sign_ed25519_detached test_sign_detached
#include "../../pkg/plan/src/rplan.c"
#undef crypto_sign_ed25519_detached

#include "test_plan_op83.c"

static void test_crypto_backend_restore(void) {
  rp_sodium_status = sodium_init();
  test_sign_fails = false;
}

TEST_FIXTURE(crypto_failures, NULL, test_crypto_backend_restore)

TEST(crypto_failures, initialization_failure_is_catchable) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;

  /* Complete pthread_once before simulating its cached failure result. */
  rp_crypto_init(t);
  rp_sodium_status = -1;
  pl_vpush(t, 1);
  test_crypto_rejection(t, "Sha256", 1, base,
                         "crypto: sodium initialization failed");
  t->vsp = base;
  test_rt_free(&rt);
}

TEST(crypto_failures, signing_failure_is_catchable) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->rplan_f = true;
  size_t base = t->vsp;
  uint8_t seed[33] = {0};
  seed[32] = 1;
  pl_vpush(t, pl_nat_from_bytes(t, seed, sizeof(seed)));
  pl_vpush(t, 1);

  test_sign_fails = true;
  test_crypto_rejection(t, "Ed25519Sign", 2, base, "Ed25519Sign: signing failed");
  test_sign_fails = false;
  /* Catching the failure leaves this thread usable for another signature. */
  ASSERT_EQ(pl_nat_byte_len(test_crypto(t, "Ed25519Sign", 2, base)), 65);
  t->vsp = base;
  test_rt_free(&rt);
}
