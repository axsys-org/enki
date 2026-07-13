#include "internal.h"

#include "plan/build.h"
#include "plan/nat.h"

#define ARG(i) (t->vstack[ab + (i)])

static void rp_want_nat(pl_thread* t, pl_val v) {
  if (!pl_is_nat(v))
    pl_raise_msg(t, "op82 expected nat");
}

/* Coordination requests remain in the core runtime; hosts never see them. */
static pl_val rp_request(pl_thread* t, size_t ab, uint32_t argc) {
  pl_gc_reserve(t, PL_APP_CELLS(argc));
  PL_GC_FORBID(t);
  pl_val r = pl_mk_app_from(t, t->vstack[ab - 1], argc, &t->vstack[ab]);
  PL_GC_ALLOW(t);
  return r;
}

pl_val pl_op82_spawn(pl_thread* t, size_t ab) {
  return rp_request(t, ab, 1);
}

pl_val pl_op82_send(pl_thread* t, size_t ab) {
  rp_want_nat(t, ARG(0));
  return rp_request(t, ab, 2);
}

pl_val pl_op82_send_caps(pl_thread* t, size_t ab) {
  rp_want_nat(t, ARG(0));
  return rp_request(t, ab, 3);
}

pl_val pl_op82_recv(pl_thread* t, size_t ab) {
  if (ARG(0) != 0)
    pl_raise_msg(t, "unknown actor/net op");
  return rp_request(t, ab, 1);
}

pl_val pl_op82_close_handle(pl_thread* t, size_t ab) {
  rp_want_nat(t, ARG(0));
  return rp_request(t, ab, 1);
}

pl_val pl_op83_read_folder(pl_thread* t, size_t ab) {
  rp_want_nat(t, ARG(0));
  return rp_request(t, ab, 1);
}

pl_val pl_op83_fetch(pl_thread* t, size_t ab) {
  return rp_request(t, ab, 2);
}
