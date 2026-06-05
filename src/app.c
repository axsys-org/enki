
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "enki/app.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/value.h"
#include "enki/util.h"
#include "enki/print.h"
#include "enki/profile.h"

size_t enki_app_arity(enki_value val_v) {
  if (!IS_PTR(val_v))
    return 0;
  enki_value_header* h = ENKI_AS(enki_value_header, val_v);
  switch (h->kind_b) {
  case LAW: {
    enki_law* law = ENKI_AS(enki_law, val_v);
    return law->arity_s;
  }
  case PIN: {
    enki_pin* pin = ENKI_AS(enki_pin, val_v);
    enki_value inner_v = pin->inner_v;
    if (!IS_PTR(inner_v))
      return 1;
    enki_value_header* inner_h = ENKI_AS(enki_value_header, inner_v);
    if (inner_h->kind_b == LAW) {
      enki_law* law = ENKI_AS(enki_law, inner_v);
      return law->arity_s;
    }
    return 0;
  }
  case APP: {
    enki_app* app = ENKI_AS(enki_app, val_v);
    size_t fn_arity_s = enki_app_arity(app->fn_v);
    if (fn_arity_s <= app->n_args_s)
      return 0;
    return fn_arity_s - app->n_args_s;
  }
  case IND: {
    enki_app* app = ENKI_AS(enki_app, val_v);
    return enki_app_arity(app->fn_v);
  }
  default:
    return 0;
  }
}

static void enki_app_open_spine(enki_value head_v, enki_value* fn_v,
                                size_t* arity_s, size_t* old_args_s,
                                enki_value** old_args_v) {
  *arity_s = 0;
  *old_args_s = 0;
  *old_args_v = NULL;
  *fn_v = head_v;
  if (!IS_PTR(head_v))
    return;
  enki_value_header* h = ENKI_AS(enki_value_header, head_v);
  switch (h->kind_b) {
  case LAW: {
    enki_law* law = ENKI_AS(enki_law, head_v);
    *arity_s = law->arity_s;
    return;
  }
  case APP: {
    enki_app* app = ENKI_AS(enki_app, head_v);
    size_t fn_arity = enki_app_arity(app->fn_v);
    *old_args_s = app->n_args_s;
    *old_args_v = app->args_v;
    *fn_v = app->fn_v;
    if (fn_arity <= app->n_args_s) {
      *arity_s = 0;
      return;
    }
    *arity_s = fn_arity - app->n_args_s;
    return;
  }
  case IND: {
    enki_app* app = ENKI_AS(enki_app, head_v);
    enki_app_open_spine(app->fn_v, fn_v, arity_s, old_args_s, old_args_v);
    return;
  }
  case PIN: {
    enki_pin* pin = ENKI_AS(enki_pin, head_v);
    enki_value inner = pin->inner_v;
    *fn_v = inner;
    if (IS_PTR(inner)) {
      enki_value_header* inner_h = ENKI_AS(enki_value_header, inner);
      if (inner_h->kind_b == ENKI_LAW) {
        enki_law* law = ENKI_AS(enki_law, inner);
        *arity_s = law->arity_s;
        return;
      }
      return;
    }
    *arity_s = 1;
    return;
  }
  case NAT:
    return;
  default:
    return;
  }
}

static void enki_app_fold(enki_interpreter* i, enki_value val_v,
                          size_t fn_index_i) {
  i->stack_v[fn_index_i] = val_v;
  i->sp = fn_index_i + 1;
}

static enki_value
enki_app_build_flat(enki_interpreter* i, enki_value fn_v, size_t old_args_s,
                    const enki_value* old_args_v, size_t new_args_s,
                    const enki_value* new_args_v, uint8_t state_b) {
  size_t total_s = old_args_s + new_args_s;
  enki_app* new_app = enki_alloc_app_bare(i->gc, fn_v, total_s);
  new_app->h.state_b = state_b;
  if (old_args_s > 0) {
    memcpy(new_app->args_v, old_args_v, old_args_s * sizeof(enki_value));
  }
  if (new_args_s > 0) {
    memcpy(new_app->args_v + old_args_s, new_args_v,
           new_args_s * sizeof(enki_value));
  }
  return PTR_TO_ENKI(new_app);
}

static void enki_app_fold_law_app(enki_interpreter* i, enki_value fn_v,
                                  size_t fn_index_i, size_t arg_index_i,
                                  size_t n_args_s, uint8_t state_b) {
  size_t n = sizeof(enki_app) + (n_args_s * sizeof(enki_value));
  enki_app* app = (enki_app*)i->gc->alloc(i->gc, n, _Alignof(enki_app));
  if (!app)
    return;
  app->h.size_s = n;
  app->h.kind_b = ENKI_APP;
  app->h.state_b = state_b;
  app->fn_v = fn_v;
  app->n_args_s = n_args_s;
  switch (n_args_s) {
  case 4:
    app->args_v[3] = i->stack_v[arg_index_i + 3];
    [[fallthrough]];
  case 3:
    app->args_v[2] = i->stack_v[arg_index_i + 2];
    [[fallthrough]];
  case 2:
    app->args_v[1] = i->stack_v[arg_index_i + 1];
    [[fallthrough]];
  case 1:
    app->args_v[0] = i->stack_v[arg_index_i];
    break;
  case 0:
    break;
  default:
    memcpy(app->args_v, &i->stack_v[arg_index_i],
           n_args_s * sizeof(enki_value));
    break;
  }
  enki_app_fold(i, PTR_TO_ENKI(app), fn_index_i);
}

void enki_app_apply(enki_interpreter* i, size_t n_args_s) {
  ENKI_PROFILE_ZONE("enki_app_apply");
  i->stats.apply_s++;
  size_t fn_index_i = i->sp - (n_args_s + 1);
  size_t arg_index_i = fn_index_i + 1;
  enki_value head_v = i->stack_v[fn_index_i];
  if (!IS_PTR(head_v)) {
    i->stats.apply_row_s++;
    enki_value row =
        enki_alloc_row(i->gc, head_v, n_args_s, &i->stack_v[arg_index_i]);
    enki_app_fold(i, row, fn_index_i);
    return;
  }
  enki_value_header* head_h = ENKI_AS(enki_value_header, head_v);
  if (head_h->kind_b == ENKI_LAW) {
    enki_law* law = ENKI_AS(enki_law, head_v);
    if (law->arity_s == n_args_s) {
      i->stats.apply_exact_s++;
      enki_law_enter(n_args_s, head_v, i);
      return;
    }
    if (law->arity_s > n_args_s) {
      i->stats.apply_under_s++;
      enki_app_fold_law_app(i, head_v, fn_index_i, arg_index_i, n_args_s, WHNF);
      return;
    }
    i->stats.apply_over_s++;
    enki_app_fold_law_app(i, head_v, fn_index_i, arg_index_i, n_args_s, THUNK);
    return;
  } else if (head_h->kind_b == ENKI_PIN) {
    enki_pin* pin = ENKI_AS(enki_pin, head_v);
    enki_value inner_v = pin->inner_v;
    if (IS_PTR(inner_v)) {
      enki_value_header* inner_h = ENKI_AS(enki_value_header, inner_v);
      if (inner_h->kind_b == ENKI_LAW) {
        enki_law* law = ENKI_AS(enki_law, inner_v);
        if (law->arity_s == n_args_s) {
          i->stats.apply_exact_s++;
          enki_law_enter(n_args_s, inner_v, i);
          return;
        }
        if (law->arity_s > n_args_s) {
          i->stats.apply_under_s++;
          enki_app_fold_law_app(i, head_v, fn_index_i, arg_index_i, n_args_s,
                                WHNF);
          return;
        }
        i->stats.apply_over_s++;
        enki_app_fold_law_app(i, head_v, fn_index_i, arg_index_i, n_args_s,
                              THUNK);
        return;
      }
    }
  }
  size_t arity_s;
  size_t old_args_s;
  enki_value* old_args_v = NULL;
  enki_value fn_v;
  enki_app_open_spine(head_v, &fn_v, &arity_s, &old_args_s, &old_args_v);
  if (arity_s == n_args_s) {
    i->stats.apply_exact_s++;
    if (!IS_PTR(fn_v)) {
      i->stats.apply_op_s++;
      enki_interp_dispatch_op(i, (uint8_t)fn_v);
      return;
    }
    enki_value_header* h = ENKI_AS(enki_value_header, fn_v);
    switch (h->kind_b) {
    case NAT:
      i->stats.apply_op_s++;
      enki_interp_dispatch_op(i, (uint8_t)fn_v);
      return;
    case LAW:
      size_t call_arity_s = n_args_s + old_args_s;
      if (old_args_s > 0) {
        for (size_t k = n_args_s; k > 0; k--) {
          size_t idx_i = k - 1;
          i->stack_v[arg_index_i + old_args_s + idx_i] =
              i->stack_v[arg_index_i + idx_i];
        }
        for (size_t k = 0; k < old_args_s; k++) {
          i->stack_v[arg_index_i + k] = old_args_v[k];
        }
      }
      i->sp = arg_index_i + call_arity_s;
      i->stack_v[fn_index_i] =
          head_v; // self stays as original head/pin/partial
      enki_law_enter(call_arity_s, fn_v, i);
      return;
    default:
      enki_interp_throw(i, ENKI_ERROR_BAD_TAG, fn_v);
    }
    return;
  }
  if (arity_s > n_args_s) {
    i->stats.apply_under_s++;
  } else {
    i->stats.apply_over_s++;
  }
  enki_value app_v = enki_app_build_flat(
      i, fn_v, old_args_s, old_args_v, n_args_s, &i->stack_v[arg_index_i],
      (arity_s == 0 || arity_s > n_args_s) ? WHNF : THUNK);
  enki_app_fold(i, app_v, fn_index_i);
}

enki_value enki_app_alloc(enki_gc* gc, enki_value fn_v, size_t n_args_s) {
  size_t n = sizeof(enki_app) + (n_args_s * sizeof(enki_value));
  enki_app* new = (enki_app*)enki_gc_alloc_locked(gc, n, _Alignof(enki_app));
  new->h.size_s = n;
  new->h.kind_b = ENKI_APP;
  new->h.state_b = WHNF;
  new->fn_v = fn_v;
  new->n_args_s = n_args_s;
  // if(n_args_s > 0 && args_v != NULL) memcpy(new->args_v, args_v, n_args_s *
  // sizeof(enki_value));
  return PTR_TO_ENKI(new);
}

enki_value enki_app_cont_alloc(enki_gc* gc, size_t n_args_s,
                               enki_value* bas_v) {
  size_t n = sizeof(enki_cont) + (n_args_s * sizeof(enki_value));
  enki_cont* new = (enki_cont*)enki_gc_alloc_locked(gc, n, _Alignof(enki_cont));
  new->h.size_s = n;
  new->h.kind_b = ENKI_CONT;
  new->h.state_b = WHNF;
  new->n_args_s = n_args_s;
  if (n_args_s > 0) {
    memcpy(new->args_v, bas_v, n_args_s * sizeof(enki_value));
  }
  return PTR_TO_ENKI(new);
}

enki_value enki_alloc_cont(enki_gc* gc, size_t n_args_s, enki_value* bas_v) {
  return enki_app_cont_alloc(gc, n_args_s, bas_v);
}

enki_value enki_alloc_pair(enki_gc* gc, enki_value l_v, enki_value r_v) {
  enki_app* app = enki_alloc_app_bare(gc, l_v, 1);
  app->args_v[0] = r_v;
  return PTR_TO_ENKI(app);
}

enki_value enki_alloc_trel(enki_gc* gc, enki_value fn_v, enki_value one_v,
                           enki_value two_v) {
  enki_app* app = enki_alloc_app_bare(gc, fn_v, 2);
  app->args_v[0] = one_v;
  app->args_v[1] = two_v;
  return PTR_TO_ENKI(app);
}
enki_value enki_alloc_quad(enki_gc* gc, enki_value fn_v, enki_value one_v,
                           enki_value two_v, enki_value tri_v) {
  enki_app* app = enki_alloc_app_bare(gc, fn_v, 3);
  app->args_v[0] = one_v;
  app->args_v[1] = two_v;
  app->args_v[2] = tri_v;
  return PTR_TO_ENKI(app);
}

enki_value enki_alloc_row(enki_gc* gc, enki_value fn_v, size_t arg_s,
                          enki_value* arg_v) {
  enki_app* app = enki_alloc_app_bare(gc, fn_v, arg_s);
  memcpy(app->args_v, arg_v, sizeof(enki_value) * arg_s);
  return PTR_TO_ENKI(app);
}

enki_value enki_app_hd(enki_value app_v) {
  enki_app* app = (enki_app*)ENKI_TO_PTR(app_v);
  char kin_b = app->h.kind_b;
  ea_assertf(kin_b == ENKI_APP, "not app %u\n", kin_b);
  return app->fn_v;
}
/// XX: this should be discouraged in hot loops,
/// avoid unmasking and remasking
enki_value enki_app_idx(enki_value app_v, size_t idx_s) {
  enki_app* app = (enki_app*)ENKI_TO_PTR(app_v);
  char kin_b = app->h.kind_b;
  ea_assertf(kin_b == ENKI_APP, "not app %u\n", kin_b);
  return app->args_v[idx_s];
}

enki_value enki_alloc_quin(enki_gc* gc, enki_value fn_v, enki_value one_v,
                           enki_value two_v, enki_value tri_v,
                           enki_value qua_v) {
  enki_app* app = enki_alloc_app_bare(gc, fn_v, 4);
  app->args_v[0] = one_v;
  app->args_v[1] = two_v;
  app->args_v[2] = tri_v;
  app->args_v[3] = qua_v;
  return PTR_TO_ENKI(app);
}

enki_app* enki_alloc_app_bare(enki_gc* gc, enki_value fn_v, size_t n_args_s) {
  enki_value_header* hed = IS_PTR(fn_v) ? ENKI_TO_PTR(fn_v) : NULL;

  ea_assertf(hed == NULL || hed->kind_b != ENKI_APP,
             "cannot put app as hd of another app %s",
             enki_print_value(&sys_a, fn_v, NULL));

  size_t n = sizeof(enki_app) + (n_args_s * sizeof(enki_value));
  enki_app* new = (enki_app*)gc->alloc(gc, n, _Alignof(enki_app));
  if (!new)
    return 0;
  new->h.size_s = n;
  new->h.kind_b = ENKI_APP;
  new->h.state_b = WHNF;
  new->fn_v = fn_v;
  new->n_args_s = n_args_s;
  // if(n_args_s > 0 && args_v != NULL) memcpy(new->args_v, args_v, n_args_s *
  // sizeof(enki_value));
  return new;
}
