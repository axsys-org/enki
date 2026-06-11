#include <criterion/criterion.h>
#include <string.h>

#include "axsys/allocator.h"
#include "plan/canon.h"
#include "test_plan.h"

/*
 * Canonical-text rendering, checked against hand-derived reference
 * (Print.hs) output.  These bytes are normative: a pin's content hash is
 * SHA-256 of pl_canonize, and Save writes pl_canonize to disk.
 */

static char* show(pl_thread* t, pl_val v) {
  v = pl_nf(t, v);
  return pl_show_val(ax_allocator_system(), v, NULL);
}

Test(canon, nat_and_string) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  char* a = show(t, 42);
  cr_assert_str_eq(a, "42");
  ax_free(ax_allocator_system(), a);

  /* "foo" little-endian = 'f' | 'o'<<8 | 'o'<<16 */
  pl_val foo = (pl_val)'f' | ((pl_val)'o' << 8) | ((pl_val)'o' << 16);
  char* b = show(t, foo);
  cr_assert_str_eq(b, "\"foo\"");
  ax_free(ax_allocator_system(), b);

  /* a single non-alpha byte is a number, not a string */
  char* c = show(t, (pl_val)'0');
  cr_assert_str_eq(c, "48");
  ax_free(ax_allocator_system(), c);

  test_rt_free(&rt);
}

Test(canon, application_row) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_app2(t, 0, 1, 2)); /* (0 1 2) */
  char* s = show(t, t->vstack[base]);
  cr_assert_str_eq(s, "(0 1 2)");
  ax_free(ax_allocator_system(), s);
  test_rt_free(&rt);
}

Test(canon, law_self_and_vars) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* L 2 "f" (0 1 2): a 2-ary law tagged "f", body applies arg1 to arg2.
   * extract: self<-"f", args 1,2 -> a,b; body (var1 var2) -> (a b). */
  pl_vpush(t, test_law(t, 2, (pl_val)'f', test_app2(t, 0, 1, 2)));
  char* s = show(t, t->vstack[base]);
  cr_assert_str_eq(s, "(#law \"f\" (f a b) (a b))");
  ax_free(ax_allocator_system(), s);
  test_rt_free(&rt);
}

Test(canon, law_with_let) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* L 1 "g" (1 7 2): one let binding = 7, body = var2 (the let var).
   * self<-"g", arg1<-a, let var2<-b; the rhs is a raw nat 7, which
   * extractExpr renders as an escaped literal #(7) (then wrap parens). */
  pl_vpush(t, test_law(t, 1, (pl_val)'g', test_app2(t, 1, 7, 2)));
  char* s = show(t, t->vstack[base]);
  cr_assert_str_eq(s, "(#law \"g\" (g a) b(#(7)) b)");
  ax_free(ax_allocator_system(), s);
  test_rt_free(&rt);
}

Test(canon, pin_of_nat) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, 42);
  pl_val pin = pl_pin(t, t->vstack[base]);
  char* s = pl_show_val(ax_allocator_system(), pin, NULL);
  cr_assert_str_eq(s, "(#pin 42)");
  ax_free(ax_allocator_system(), s);
  test_rt_free(&rt);
}

Test(canon, canonize_module_text) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  /* pin a law with no sub-pins: the snapshot has no imports. */
  pl_vpush(t, test_law(t, 1, (pl_val)'h', test_app2(t, 0, 1, 1)));
  pl_val pin = pl_pin(t, t->vstack[base]);
  char* s = pl_canonize(ax_allocator_system(), pin, NULL);
  cr_assert_str_eq(s,
                   "(#bind _ (#pin (#law \"h\" (h a) (a a))))\n(#export _)\n");
  ax_free(ax_allocator_system(), s);
  test_rt_free(&rt);
}

Test(canon, pin_hash_is_canonical_text) {
  /* The pin hash is SHA-256 of the canonical text: re-pinning a
   * structurally identical graph interns to the same pin. */
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  size_t base = t->vsp;
  pl_vpush(t, test_law(t, 1, (pl_val)'h', test_app2(t, 0, 1, 1)));
  pl_vpush(t, test_law(t, 1, (pl_val)'h', test_app2(t, 0, 1, 1)));
  pl_val p1 = pl_pin(t, t->vstack[base]);
  pl_val p2 = pl_pin(t, t->vstack[base + 1]);
  cr_assert_eq(p1, p2);
  test_rt_free(&rt);
}
