#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "enki/app.h"
#include "enki/eval.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/pin.h"
#include "enki/profile.h"
#include "enki/value.h"

void op66_inc(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_inc(i->gc, a);
}
void op66_dec(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_dec(i->gc, a);
}
void op66_add(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_add(i->gc, a, b);
}
void op66_sub(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_sub(i->gc, a, b);
}
void op66_mul(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_mul(i->gc, a, b);
}
void op66_div(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_div(i->gc, a, b);
}
void op66_mod(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_mod(i->gc, a, b);
}
enki_value op66_structural_eq(enki_interpreter* i, enki_value a, enki_value b) {
  a = enki_value_unind(a);
  b = enki_value_unind(b);
  if (!IS_PTR(a) && !IS_PTR(b)) {
    return (a == b ? (enki_value)1 : (enki_value)0);
  }
  size_t root_s = i->sp;
  i->stack_v[i->sp++] = a;
  i->stack_v[i->sp++] = b;
  enki_value res_v = 0;

  a = i->stack_v[root_s];
  b = i->stack_v[root_s + 1];
  enki_value_header* h_a = IS_PTR(a) ? ENKI_AS(enki_value_header, a) : NULL;
  enki_value_header* h_b = IS_PTR(b) ? ENKI_AS(enki_value_header, b) : NULL;

  if (IS_PTR(a) && !IS_PTR(b)) {
    if (h_a->kind_b == ENKI_NAT)
      res_v = enki_nat_eq(a, b);
    goto done;
  } else if (!IS_PTR(a) && IS_PTR(b)) {
    if (h_b->kind_b == ENKI_NAT)
      res_v = enki_nat_eq(a, b);
    goto done;
  } else if (IS_PTR(a) && IS_PTR(b)) {
    if (h_a->kind_b == ENKI_NAT && h_b->kind_b == ENKI_NAT) {
      res_v = enki_nat_eq(a, b);
      goto done;
    }
  }
  if (h_b->kind_b != h_a->kind_b)
    goto done;
  if (h_a->kind_b == ENKI_PIN) {
    enki_pin* pin_a = ENKI_AS(enki_pin, a);
    enki_pin* pin_b = ENKI_AS(enki_pin, b);
    if (pin_a->n_subpins_s != pin_b->n_subpins_s)
      goto done;
    if (pin_a->h.state_b == NF && pin_b->h.state_b == NF) {
      if (memcmp(pin_a->hash_b, pin_b->hash_b, 32) != 0) {
        goto done;
      }
    } else {
      i->stack_v[root_s] = enki_eval_nf(i, i->stack_v[root_s]);
      i->stack_v[root_s + 1] = enki_eval_nf(i, i->stack_v[root_s + 1]);
      a = i->stack_v[root_s];
      b = i->stack_v[root_s + 1];
      pin_a = ENKI_AS(enki_pin, a);
      pin_b = ENKI_AS(enki_pin, b);
      if (memcmp(pin_a->hash_b, pin_b->hash_b, 32) != 0) {
        goto done;
      }
    }
    res_v = 1;
    goto done;
  } else if (h_a->kind_b == ENKI_LAW) {
    enki_law* law_a = ENKI_AS(enki_law, a);
    enki_law* law_b = ENKI_AS(enki_law, b);
    if (law_b->arity_s != law_a->arity_s)
      goto done;
    if (op66_structural_eq(i, law_a->name_v, law_b->name_v) == 0)
      goto done;
    a = i->stack_v[root_s];
    b = i->stack_v[root_s + 1];
    law_a = ENKI_AS(enki_law, a);
    law_b = ENKI_AS(enki_law, b);
    if (op66_structural_eq(i, law_a->body_v, law_b->body_v) == 0)
      goto done;
    res_v = 1;
    goto done;
  } else if (h_a->kind_b == ENKI_APP) {
    enki_app* app_a = ENKI_AS(enki_app, a);
    enki_app* app_b = ENKI_AS(enki_app, b);
    if (app_a->n_args_s != app_b->n_args_s)
      goto done;
    for (size_t k = 0; k < app_a->n_args_s; k++) {
      if (!op66_structural_eq(i, app_a->args_v[k], app_b->args_v[k])) {
        goto done;
      }
      a = i->stack_v[root_s];
      b = i->stack_v[root_s + 1];
      app_a = ENKI_AS(enki_app, a);
      app_b = ENKI_AS(enki_app, b);
    }
    res_v = (op66_structural_eq(i, app_a->fn_v, app_b->fn_v)) ? (enki_value)1
                                                              : (enki_value)0;
    goto done;
  }

done:
  i->sp = root_s;
  return res_v;
}
void op66_eq(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_eq(a, b);
}
void op66_equal(enki_interpreter* i) {
  ENKI_PROFILE_ZONE("op66_equal");
  i->stack_v[i->sp - 2] = enki_eval_nf(i, i->stack_v[i->sp - 2]);
  i->stack_v[i->sp - 1] = enki_eval_nf(i, i->stack_v[i->sp - 1]);
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  if (!IS_PTR(a) && !IS_PTR(b)) {
    i->stack_v[i->sp - 1] = a == b ? 1 : 0;
  }
  i->stack_v[i->sp - 1] = op66_structural_eq(i, a, b);
}
void op66_ne(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_ne(a, b);
}
void op66_gt(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_gt(a, b);
}
void op66_ge(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_ge(a, b);
}
void op66_lt(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_lt(a, b);
}
void op66_le(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_le(a, b);
}
void op66_cmp(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value b = i->stack_v[i->sp - 1];
  i->sp--;
  int cmp = enki_nat_cmp(a, b);
  i->stack_v[i->sp - 1] =
      (cmp < 0) ? (enki_value)0 : (cmp == 0 ? (enki_value)1 : (enki_value)2);
}
void op66_lsh(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value bits = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_lsh(i->gc, a, bits);
}
void op66_rsh(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 2];
  enki_value bits = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_rsh(i->gc, a, bits);
}
void op66_test(enki_interpreter* i) {
  enki_value bit = i->stack_v[i->sp - 2];
  enki_value a = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_test(i->gc, bit, a);
}
void op66_set(enki_interpreter* i) {
  enki_value bit = i->stack_v[i->sp - 2];
  enki_value a = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_set(i->gc, bit, a);
}
void op66_clear(enki_interpreter* i) {
  enki_value bit = i->stack_v[i->sp - 2];
  enki_value a = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_clear(i->gc, bit, a);
}
void op66_bex(enki_interpreter* i) {
  enki_value bit = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_bex(i->gc, bit);
}
void op66_bits(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_bits(i->gc, a);
}
void op66_bytes(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_bytes(i->gc, a);
}
void op66_nib(enki_interpreter* i) {
  enki_value index_i = i->stack_v[i->sp - 2];
  enki_value a = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_nib(i->gc, index_i, a);
}
void op66_load8(enki_interpreter* i) {
  enki_value index_i = i->stack_v[i->sp - 2];
  enki_value a = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_load8(i->gc, index_i, a);
}
void op66_store8(enki_interpreter* i) {
  enki_value index_i = i->stack_v[i->sp - 3];
  enki_value byte = i->stack_v[i->sp - 2];
  enki_value a = i->stack_v[i->sp - 1];
  i->sp -= 2;
  i->stack_v[i->sp - 1] = enki_nat_store8(i->gc, index_i, byte, a);
}
void op66_trunc(enki_interpreter* i) {
  enki_value width = i->stack_v[i->sp - 2];
  enki_value a = i->stack_v[i->sp - 1];
  i->sp--;
  i->stack_v[i->sp - 1] = enki_nat_trunc(i->gc, width, a);
}
void op66_trunc8(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_trunc8(i->gc, a);
}
void op66_trunc16(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_trunc16(i->gc, a);
}
void op66_trunc32(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_trunc32(i->gc, a);
}
void op66_trunc64(enki_interpreter* i) {
  enki_value a = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = enki_nat_trunc64(i->gc, a);
}

void op66_type(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  uint8_t res_v = 0;
  if (!IS_PTR(x)) {
    res_v = 0;
  } else {
    enki_value_header* h = ENKI_AS(enki_value_header, x);
    switch (h->kind_b) {
    case ENKI_NAT:
      res_v = 0;
      break;
    case ENKI_PIN:
      res_v = 1;
      break;
    case ENKI_LAW:
      res_v = 2;
      break;
    case ENKI_APP:
      res_v = 3;
      break;
    default:
      enki_interp_throw(i, ENKI_ERROR_BAD_TAG, x);
      ;
      break;
    }
  }
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_pin(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  size_t res_v = 0;
  if (h->kind_b == ENKI_PIN)
    res_v = 1;
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_law(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  size_t res_v = 0;
  if (h->kind_b == ENKI_LAW)
    res_v = 1;
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_app(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  size_t res_v = 0;
  if (h->kind_b == ENKI_APP)
    res_v = 1;
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_is_nat(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)1;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  size_t res_v = 0;
  if (h->kind_b == ENKI_NAT)
    res_v = 1;
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_nat(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)x;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = 0;
  if (h->kind_b == ENKI_NAT)
    res_v = x;
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_unpin(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = 0;
  if (h->kind_b == ENKI_PIN) {
    enki_pin* pin = ENKI_AS(enki_pin, x);
    res_v = pin->inner_v;
  }
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
void op66_name(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = 0;
  if (h->kind_b == ENKI_LAW) {
    enki_law* law = ENKI_AS(enki_law, x);
    res_v = law->name_v;
  }
  i->stack_v[i->sp - 1] = res_v;
}
void op66_body(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = 0;
  if (h->kind_b == ENKI_LAW) {
    enki_law* law = ENKI_AS(enki_law, x);
    res_v = law->body_v;
  }
  i->stack_v[i->sp - 1] = res_v;
}
void op66_arity(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = 0;
  if (h->kind_b == ENKI_LAW) {
    enki_law* law = ENKI_AS(enki_law, x);
    res_v = (enki_value)law->arity_s;
  }
  i->stack_v[i->sp - 1] = res_v;
}
void op66_hd(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = x;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = x;
  if (h->kind_b == ENKI_APP) {
    enki_app* app = ENKI_AS(enki_app, x);
    res_v = app->fn_v;
  }
  i->stack_v[i->sp - 1] = res_v;
}
void op66_last(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = (enki_value)0;
  if (h->kind_b == ENKI_APP) {
    enki_app* app = ENKI_AS(enki_app, x);
    if (app->n_args_s == 0)
      res_v = 0;
    else
      res_v = app->args_v[app->n_args_s - 1];
  }
  i->stack_v[i->sp - 1] = res_v;
}
void op66_init(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = (enki_value)0;
  if (h->kind_b == ENKI_APP) {
    enki_app* app = ENKI_AS(enki_app, x);
    if (app->n_args_s == 1) {
      res_v = app->fn_v;
    } else if (app->n_args_s > 1) {
      size_t n_args_s = app->n_args_s - 1;
      enki_value fn_v = app->fn_v;
      enki_value new = enki_app_alloc(i->gc, fn_v, n_args_s);
      x = enki_value_unind(i->stack_v[i->sp - 1]);
      app = ENKI_AS(enki_app, x);
      enki_app* new_app = ENKI_AS(enki_app, new);
      new_app->fn_v = app->fn_v;
      memcpy(new_app->args_v, app->args_v, n_args_s * sizeof(enki_value));
      res_v = new;
    }
  }
  i->stack_v[i->sp - 1] = res_v;
}
void op66_sz(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = (enki_value)0;
  if (h->kind_b == ENKI_APP) {
    enki_app* app = ENKI_AS(enki_app, x);
    res_v = app->n_args_s;
  }
  i->stack_v[i->sp - 1] = res_v;
}

enki_value ix_at(enki_value i, enki_value x) {
  x = enki_value_unind(x);
  if (!IS_PTR(x) || IS_PTR(i)) {
    return (enki_value)0;
  }
  enki_value res_v = (enki_value)0;
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  if (h->kind_b == ENKI_APP) {
    enki_app* app = ENKI_AS(enki_app, x);
    if (i >= app->n_args_s)
      res_v = 0;
    else
      res_v = app->args_v[i];
  }
  return res_v;
}
void op66_ix(enki_interpreter* interp) {
  enki_value idx_i = interp->stack_v[interp->sp - 2];
  enki_value x = interp->stack_v[interp->sp - 1];
  interp->sp--;
  interp->stack_v[interp->sp - 1] = ix_at(idx_i, x);
}
void op66_ix0(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)0, x);
}

void op66_ix1(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)1, x);
}

void op66_ix2(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)2, x);
}

void op66_ix3(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)3, x);
}

void op66_ix4(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)4, x);
}

void op66_ix5(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)5, x);
}

void op66_ix6(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)6, x);
}

void op66_ix7(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = ix_at((enki_value)7, x);
}
void op66_row(enki_interpreter* i) {
  enki_value h = i->stack_v[i->sp - 3];
  enki_value n = i->stack_v[i->sp - 2];
  enki_value xs = i->stack_v[i->sp - 1];
  if (IS_PTR(n))
    enki_interp_throw(i, ENKI_ERROR_TYPE, n);
  enki_value app = enki_app_alloc(i->gc, h, n);
  enki_app* ptr = ENKI_AS(enki_app, app);
  h = i->stack_v[i->sp - 3];
  xs = i->stack_v[i->sp - 1];
  ptr->fn_v = h;
  enki_value curr = xs;
  for (size_t k = 0; k < n; k++) {
    ptr->args_v[k] = ix_at((enki_value)0, curr);
    curr = ix_at((enki_value)1, curr);
  }
  i->sp -= 2;
  i->stack_v[i->sp - 1] = app;
}
void op66_rep(enki_interpreter* i) {
  enki_value h = i->stack_v[i->sp - 3];
  enki_value x = i->stack_v[i->sp - 2];
  enki_value n = i->stack_v[i->sp - 1];
  if (IS_PTR(n))
    enki_interp_throw(i, ENKI_ERROR_TYPE, n);
  enki_value app = enki_app_alloc(i->gc, h, n);
  enki_app* ptr = ENKI_AS(enki_app, app);
  h = i->stack_v[i->sp - 3];
  x = i->stack_v[i->sp - 2];
  ptr->fn_v = h;
  for (size_t k = 0; k < n; k++) {
    ptr->args_v[k] = x;
  }
  i->sp -= 2;
  i->stack_v[i->sp - 1] = app;
}
void op66_slice(enki_interpreter* i) {
  enki_value o = i->stack_v[i->sp - 3];
  enki_value n = i->stack_v[i->sp - 2];
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  size_t scratch_s = i->sp;
  if (IS_PTR(o) || IS_PTR(n) || !IS_PTR(x)) {
    i->sp -= 2;
    i->stack_v[i->sp - 1] = (enki_value)0;
    return;
  }
  enki_value_header* h = ENKI_AS(enki_value_header, x);
  enki_value res_v = 0;
  if (h->kind_b == ENKI_APP) {
    enki_app* app = ENKI_AS(enki_app, x);
    if (o < app->n_args_s) {
      size_t rsz = app->n_args_s - o;
      if (n < rsz)
        rsz = n;
      if (rsz != 0) {
        for (size_t k = 0; k < rsz; k++) {
          i->stack_v[i->sp] = app->args_v[o + k];
          i->sp++;
        }
        enki_value new = enki_app_alloc(i->gc, (enki_value)0, rsz);
        enki_app* new_app = ENKI_AS(enki_app, new);
        for (size_t k = 0; k < rsz; k++) {
          new_app->args_v[k] = i->stack_v[scratch_s + k];
        }
        i->sp = scratch_s;
        res_v = new;
      }
    }
  }
  i->sp -= 2;
  i->stack_v[i->sp - 1] = (enki_value)res_v;
}
static enki_value* args_v(enki_value a) {
  a = enki_value_unind(a);
  if (!IS_PTR(a))
    return NULL;
  enki_value_header* h = ENKI_AS(enki_value_header, a);
  if (h->kind_b == ENKI_APP) {
    enki_app* app = ENKI_AS(enki_app, a);
    return app->args_v;
  }
  return NULL;
}

void op66_weld(enki_interpreter* i) {
  enki_value x = enki_value_unind(i->stack_v[i->sp - 2]);
  enki_value y = enki_value_unind(i->stack_v[i->sp - 1]);
  enki_value* x_args = args_v(x);
  enki_value* y_args = args_v(y);
  enki_value res_v;
  if (x_args == NULL && y_args == NULL)
    res_v = (enki_value)0;
  else {
    size_t x_c = 0;
    if (x_args != NULL)
      x_c = (ENKI_AS(enki_app, x))->n_args_s;
    size_t y_c = 0;
    if (y_args != NULL)
      y_c = (ENKI_AS(enki_app, y))->n_args_s;
    size_t n_args_s = (x_c + y_c);
    res_v = enki_app_alloc(i->gc, (enki_value)0, n_args_s);
    enki_app* ptr = ENKI_AS(enki_app, res_v);
    x = enki_value_unind(i->stack_v[i->sp - 2]);
    y = enki_value_unind(i->stack_v[i->sp - 1]);
    x_args = args_v(x);
    y_args = args_v(y);
    if (x_args != NULL) {
      for (size_t k = 0; k < x_c; k++)
        ptr->args_v[k] = x_args[k];
    }
    if (y_args != NULL) {
      for (size_t k = 0; k < y_c; k++)
        ptr->args_v[x_c + k] = y_args[k];
    }
  }
  i->sp--;
  i->stack_v[i->sp - 1] = res_v;
}
void op66_up(enki_interpreter* i) {
  enki_value idx_i = i->stack_v[i->sp - 3];
  enki_value v = i->stack_v[i->sp - 2];
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  enki_value res_v = x;
  if (IS_PTR(x) && !IS_PTR(idx_i)) {
    enki_value_header* h = ENKI_AS(enki_value_header, x);
    if (h->kind_b == ENKI_APP) {
      enki_app* app = ENKI_AS(enki_app, x);
      if (idx_i < app->n_args_s) {
        size_t n_args_s = app->n_args_s;
        enki_value fn_v = app->fn_v;
        size_t scratch_s = i->sp;
        for (size_t k = 0; k < n_args_s; k++) {
          i->stack_v[i->sp] = app->args_v[k];
          i->sp++;
        }
        res_v = enki_app_alloc(i->gc, fn_v, n_args_s);
        enki_app* new = ENKI_AS(enki_app, res_v);
        idx_i = i->stack_v[scratch_s - 3];
        v = i->stack_v[scratch_s - 2];
        x = enki_value_unind(i->stack_v[scratch_s - 1]);
        app = ENKI_AS(enki_app, x);
        new->fn_v = app->fn_v;
        for (size_t k = 0; k < n_args_s; k++) {
          new->args_v[k] = i->stack_v[scratch_s + k];
        }
        new->args_v[idx_i] = v;
        i->sp = scratch_s;
      }
    }
  }
  i->sp -= 2;
  i->stack_v[i->sp - 1] = res_v;
}
void op66_up_uniq(enki_interpreter* i) {
  op66_up(i); // same as UP for now
}
void op66_coup(enki_interpreter* i) {
  enki_value h = i->stack_v[i->sp - 2];
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  if (!IS_PTR(x)) {
    i->sp--;
    i->stack_v[i->sp - 1] = h;
    return;
  }
  enki_value_header* xh = ENKI_AS(enki_value_header, x);
  if (xh->kind_b != ENKI_APP) {
    i->sp--;
    i->stack_v[i->sp - 1] = h;
    return;
  }
  enki_app* app = ENKI_AS(enki_app, x);
  i->sp--; // remove x leaving h at sp - 1
  for (size_t k = 0; k < app->n_args_s; k++) {
    i->stack_v[i->sp] = app->args_v[k];
    i->sp++;
  }
  enki_app_apply(i, app->n_args_s);
  return;
}

void op66_case(enki_interpreter* i) {
  enki_value ix = i->stack_v[i->sp - 3];
  enki_value cs = enki_value_unind(i->stack_v[i->sp - 2]);
  enki_value f = i->stack_v[i->sp - 1];
  enki_value res_v = f;

  if (!IS_PTR(ix) && IS_PTR(cs)) {
    enki_value_header* h = ENKI_AS(enki_value_header, cs);
    if (h->kind_b == ENKI_APP) {
      enki_app* app = ENKI_AS(enki_app, cs);
      if (ix < app->n_args_s) {
        res_v = app->args_v[ix];
      }
    }
  }

  i->sp -= 2;
  i->stack_v[i->sp - 1] = res_v;
}

void op66_case_n(enki_interpreter* i, size_t n) {
  enki_value ix = i->stack_v[i->sp - (n + 2)];
  enki_value fallback_v = i->stack_v[i->sp - 1];
  enki_value res_v = fallback_v;

  if (!IS_PTR(ix) && ix < n) {
    res_v = i->stack_v[i->sp - (n + 1) + ix];
  }

  i->sp -= (n + 1);
  i->stack_v[i->sp - 1] = res_v;
}

void op66_case2(enki_interpreter* i) {
  op66_case_n(i, 2);
}

void op66_case3(enki_interpreter* i) {
  op66_case_n(i, 3);
}

void op66_case4(enki_interpreter* i) {
  op66_case_n(i, 4);
}

void op66_case5(enki_interpreter* i) {
  op66_case_n(i, 5);
}

void op66_case6(enki_interpreter* i) {
  op66_case_n(i, 6);
}

void op66_case7(enki_interpreter* i) {
  op66_case_n(i, 7);
}

void op66_case8(enki_interpreter* i) {
  op66_case_n(i, 8);
}

void op66_case9(enki_interpreter* i) {
  op66_case_n(i, 9);
}

void op66_case10(enki_interpreter* i) {
  op66_case_n(i, 10);
}

void op66_case11(enki_interpreter* i) {
  op66_case_n(i, 11);
}

void op66_case12(enki_interpreter* i) {
  op66_case_n(i, 12);
}

void op66_case13(enki_interpreter* i) {
  op66_case_n(i, 13);
}

void op66_case14(enki_interpreter* i) {
  op66_case_n(i, 14);
}

void op66_case15(enki_interpreter* i) {
  op66_case_n(i, 15);
}

void op66_case16(enki_interpreter* i) {
  op66_case_n(i, 16);
}

void op66_nil(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = (x == 0) ? (enki_value)1 : (enki_value)0;
}

void op66_truth(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  i->stack_v[i->sp - 1] = (x == 0) ? (enki_value)0 : (enki_value)1;
}

void op66_or(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 2];
  enki_value y = i->stack_v[i->sp - 1];
  enki_value res_v = (x == 0) ? y : x;

  i->sp--;
  i->stack_v[i->sp - 1] = res_v;
}

void op66_nor(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 2];
  enki_value y = i->stack_v[i->sp - 1];
  enki_value res_v =
      (x != 0) ? (enki_value)0 : ((y == 0) ? (enki_value)1 : (enki_value)0);

  i->sp--;
  i->stack_v[i->sp - 1] = res_v;
}

void op66_and(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 2];
  enki_value y = i->stack_v[i->sp - 1];
  enki_value res_v = (x == 0) ? (enki_value)0 : y;

  i->sp--;
  i->stack_v[i->sp - 1] = res_v;
}

void op66_if(enki_interpreter* i) {
  enki_value c = i->stack_v[i->sp - 3];
  enki_value t = i->stack_v[i->sp - 2];
  enki_value e = i->stack_v[i->sp - 1];
  enki_value res_v = (c != 0) ? t : e;

  i->sp -= 2;
  i->stack_v[i->sp - 1] = res_v;
}

void op66_ifz(enki_interpreter* i) {
  enki_value c = i->stack_v[i->sp - 3];
  enki_value t = i->stack_v[i->sp - 2];
  enki_value e = i->stack_v[i->sp - 1];
  enki_value res_v = (c == 0) ? t : e;

  i->sp -= 2;
  i->stack_v[i->sp - 1] = res_v;
}

void op66_seq(enki_interpreter* i) {
  (void)enki_eval_whnf(i, i->stack_v[i->sp - 2]);
  enki_value y = i->stack_v[i->sp - 1];

  i->sp--;
  i->stack_v[i->sp - 1] = y;
}

void op66_seq2(enki_interpreter* i) {
  enki_value z = i->stack_v[i->sp - 1];

  (void)enki_eval_whnf(i, i->stack_v[i->sp - 3]);
  z = i->stack_v[i->sp - 1];
  (void)enki_eval_whnf(i, i->stack_v[i->sp - 2]);
  z = i->stack_v[i->sp - 1];

  i->sp -= 2;
  i->stack_v[i->sp - 1] = z;
}

void op66_seq3(enki_interpreter* i) {
  enki_value d = i->stack_v[i->sp - 1];

  (void)enki_eval_whnf(i, i->stack_v[i->sp - 4]);
  d = i->stack_v[i->sp - 1];
  (void)enki_eval_whnf(i, i->stack_v[i->sp - 3]);
  d = i->stack_v[i->sp - 1];
  (void)enki_eval_whnf(i, i->stack_v[i->sp - 2]);
  d = i->stack_v[i->sp - 1];

  i->sp -= 3;
  i->stack_v[i->sp - 1] = d;
}

static enki_value op66_apply_whnf(enki_interpreter* i, size_t n_args_s) {
  size_t base_cp_s = i->cp;
  size_t res_base_s = i->sp - (n_args_s + 1);

  enki_app_apply(i, n_args_s);
  while (i->cp > base_cp_s && !i->halted) {
    enki_interp_step(i);
  }

  enki_value result_v = i->stack_v[res_base_s];
  i->sp = res_base_s + 1;
  result_v = enki_eval_whnf(i, result_v);
  i->stack_v[res_base_s] = result_v;
  i->sp = res_base_s + 1;
  return result_v;
}

void op66_sap(enki_interpreter* i) {
  i->stack_v[i->sp - 1] = enki_eval_whnf(i, i->stack_v[i->sp - 1]);
  enki_value f = i->stack_v[i->sp - 2];
  enki_value x = i->stack_v[i->sp - 1];

  i->stack_v[i->sp - 2] = f;
  i->stack_v[i->sp - 1] = x;

  op66_apply_whnf(i, 1);
}

void op66_sap2(enki_interpreter* i) {
  size_t base_s = i->sp - 3;
  i->stack_v[i->sp - 2] = enki_eval_whnf(i, i->stack_v[i->sp - 2]);
  i->stack_v[i->sp - 1] = enki_eval_whnf(i, i->stack_v[i->sp - 1]);
  enki_value f = i->stack_v[i->sp - 3];
  enki_value x = i->stack_v[i->sp - 2];
  enki_value y = i->stack_v[i->sp - 1];

  i->stack_v[base_s] = y;
  i->stack_v[base_s + 1] = f;
  i->stack_v[base_s + 2] = x;
  op66_apply_whnf(i, 1);

  y = i->stack_v[base_s];
  enki_value fx = i->stack_v[base_s + 1];
  i->stack_v[base_s] = fx;
  i->stack_v[base_s + 1] = y;
  i->sp = base_s + 2;

  op66_apply_whnf(i, 1);
}

void op66_force(enki_interpreter* i) {
  ENKI_PROFILE_ZONE("op66_force");
  enki_value x = enki_value_unind(i->stack_v[i->sp - 1]);
  i->stack_v[i->sp - 1] = x;
  if (!IS_PTR(x))
    return;

  enki_value_header* h = ENKI_AS(enki_value_header, x);
  if (h->kind_b == ENKI_NAT) {
    h->state_b = NF;
    return;
  }

  x = enki_eval_nf(i, x);
  i->stack_v[i->sp - 1] = x;
}

void op66_deepseq(enki_interpreter* i) {
  ENKI_PROFILE_ZONE("op66_deepseq");
  (void)enki_eval_nf(i, i->stack_v[i->sp - 2]);
  enki_value y = i->stack_v[i->sp - 1];

  i->sp--;
  i->stack_v[i->sp - 1] = y;
}

void op66_save(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  enki_pin_save_root(i, x);
  i->stack_v[i->sp - 1] = (enki_value)0;
}

void op66_load(enki_interpreter* i) {
  enki_value x = enki_pin_load_root(i);
  i->stack_v[i->sp - 1] = x;
}

void op66_try(enki_interpreter* i) {
  enki_handler hdlr;
  hdlr.sp = i->sp - 1;
  hdlr.cp = i->cp;
  hdlr.res_base_s = i->sp - 2;
  i->handler_v[i->hp++] = hdlr;
  enki_app_apply(i, 1);
}

void op66_throw(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  x = enki_eval_nf(i, x);
  enki_interp_throw(i, ENKI_ERROR_THROW, x);
}
