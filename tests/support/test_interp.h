#pragma once

#include "enki/allocator.h"
#include "enki/interp.h"
#include "enki/store.h"
#include "enki/value.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENKI_TEST_STORE_SIZE_S   ((size_t)1024 * 1024)
#define ENKI_TEST_SCRATCH_SIZE_S ((size_t)1024 * 1024)

static char enki_test_store_path_s[256];
static unsigned long enki_test_store_seq_s;

static void enki_test_remove_store(void) {
  if (enki_test_store_path_s[0] == '\0')
    return;

  char path_s[320];
  (void)snprintf(path_s, sizeof(path_s), "%s/data.mdb", enki_test_store_path_s);
  (void)unlink(path_s);
  (void)snprintf(path_s, sizeof(path_s), "%s/lock.mdb", enki_test_store_path_s);
  (void)unlink(path_s);
  (void)rmdir(enki_test_store_path_s);
  enki_test_store_path_s[0] = '\0';
}

static void enki_test_make_store(void) {
  for (size_t k = 0; k < 1000; k++) {
    int n = snprintf(enki_test_store_path_s, sizeof(enki_test_store_path_s),
                     "/tmp/enki-test-%lu-%lu", (unsigned long)getpid(),
                     enki_test_store_seq_s++);
    if (n < 0 || (size_t)n >= sizeof(enki_test_store_path_s))
      abort();
    if (mkdir(enki_test_store_path_s, 0700) == 0)
      return;
    if (errno != EEXIST)
      abort();
  }
  abort();
}

static enki_interpreter* enki_test_interp_create(size_t heap_s,
                                                 enki_value law) {
  (void)law;
  enki_test_make_store();
  return enki_interp_create(enki_allocator_system(), heap_s,
                            enki_test_store_path_s, ENKI_TEST_STORE_SIZE_S,
                            ENKI_TEST_SCRATCH_SIZE_S);
}

static void enki_test_interp_destroy(enki_interpreter* i) {
  if (i != NULL) {
    enki_store_close(&i->store);
    enki_interp_destroy(i);
  }
  enki_test_remove_store();
}
