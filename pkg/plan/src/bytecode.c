#include "plan/bytecode.h"
#include "stdio.h"
#include "inttypes.h"
#include "stdlib.h"

/** mallocs (and leaks) */
pl_code* pl_bytecode_from_val(pl_val val) {
  char* msg = NULL;
  pl_code* out = calloc(1, sizeof(pl_code));
  pl_cell* a = pl_as(PL_TAG_APP, val);
#define FAIL(m)                                                                \
  {                                                                            \
    msg = m;                                                                   \
    goto failed;                                                               \
  }
  if (a == NULL)
    FAIL("no app inside pin")
  out->nops = pl_app_n(a);
  out->ops = calloc(out->nops, sizeof(pl_op_t));
  pl_val* args = pl_app_args(a);
  for (size_t i = 0; i < out->nops; i++) {
    // pl_val v = args[i];
    out->ops[i] = args[i];
    fprintf(stderr, "op[%zu] = %" PRIu64 "\n", i, args[i]);
  }
  return out;

failed:
  if (out->ops != NULL)
    free(out->ops);
  free(out);
  fprintf(stderr, "Failed to decode bytecode: %s\r\n", msg);
  return NULL;
}
