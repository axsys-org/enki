
#include "enki/value.h"
#include "enki/gc.h"
#include "enki/print.h"
#include "enki/util.h"


void enki_trace_value(enki_gc* gc, void* obj)
{
    enki_value_header* h = obj;
    switch (h->kind_b) {
    case ENKI_PIN: {
        enki_pin* pin = obj;
        pin->inner_v = gc->copy(gc, pin->inner_v);
        for (size_t k = 0; k < pin->n_subpins_s; k++) {
            pin->subpins_v[k] = gc->copy(gc, pin->subpins_v[k]);
        }
        break;
    }
    case ENKI_LAW: {
        enki_law* law = obj;
        law->body_v = gc->copy(gc, law->body_v);
        law->name_v = gc->copy(gc, law->name_v);
        for (size_t k = 0; k < law->n_const_s; k++) {
            ENKI_LAW_CONSTS(law)[k] = gc->copy(gc, ENKI_LAW_CONSTS(law)[k]);
        }
        break;
    }
    case ENKI_APP: {
        enki_app* app = obj;
        app->fn_v = gc->copy(gc, app->fn_v);
        for (size_t k = 0; k < app->n_args_s; k++) {
            app->args_v[k] = gc->copy(gc, app->args_v[k]);
        }
        break;
    }
    case ENKI_CONT: {
        enki_cont* cont_v = obj;
        for (size_t k = 0; k < cont_v->n_args_s; k++) {
            cont_v->args_v[k] = gc->copy(gc, cont_v->args_v[k]);
        }
        break;
    }
    default:
        return;
    }
}

static enki_nat* _enki_alloc_big_nat_empty(enki_gc* gc, size_t n_limbs_s)
{
    size_t n = sizeof(enki_nat) + (n_limbs_s * sizeof(mp_limb_t));
    enki_nat* new = (enki_nat*)gc->alloc(gc, n);
    if (!new)
        return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_BIG_NAT;
    new->h.state_b = NF;
    new->n_limbs_s = n_limbs_s;
    return new;
}

enki_value enki_alloc_big_nat_bytes(enki_gc* gc, size_t byt_s, char* byt_b)
{
    size_t lim_s = shr_ceil(byt_s, 3);

    enki_nat* nat = _enki_alloc_big_nat_empty(gc, lim_s);
    memcpy((char*)nat->limbs, byt_b, byt_s);
    return PTR_TO_ENKI(nat);
}

enki_value enki_alloc_big_nat(enki_gc* gc, size_t n_limbs_s, mp_limb_t limbs[])
{
    enki_nat* nat = _enki_alloc_big_nat_empty(gc, n_limbs_s);
    memcpy(nat->limbs, limbs, (n_limbs_s * sizeof(mp_limb_t)));
    return PTR_TO_ENKI(nat);
}
enki_value enki_alloc_law(enki_gc* gc, size_t arity_s, enki_value name_v, enki_value body_v,
                          size_t bc_len_s, size_t n_const_s, uint8_t* bc_b,
                          enki_value* const_table_v)
{
    size_t n = sizeof(enki_law) + bc_len_s + (n_const_s * sizeof(enki_value));
    enki_law* new = (enki_law*)gc->alloc(gc, n);
    if (!new)
        return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_LAW;
    new->h.state_b = NF;
    new->body_v = body_v;
    new->name_v = name_v;
    new->arity_s = (uint32_t)arity_s;
    new->bc_len_s = bc_len_s;
    new->n_const_s = n_const_s;
    new->let_ss = 0;
    new->let_s = NULL;
    new->bc_b = (uint8_t*)(new->const_v + n_const_s);
    new->let_b = NULL;
    size_t const_off_o = n_const_s * sizeof(enki_value);
    if (n_const_s > 0)
        memcpy(new->const_v, const_table_v, const_off_o);
    if (bc_len_s > 0)
        memcpy(new->bc_b, bc_b, bc_len_s);
    return PTR_TO_ENKI(new);
}

enki_value enki_alloc_nat(enki_gc* gc, mp_limb_t* out, size_t n_limbs_s)
{
    size_t n = n_limbs_s;
    while (n > 0 && out[n - 1] == 0) {
        n--;
    }
    if (n == 0) {
        gc->sys_a.free(gc->sys_a.ctx, out);
        return (enki_value)0;
    }
    if (n == 1 && out[n - 1] < (1ULL << 63)) {
        enki_value res_v = (enki_value)out[0];
        gc->sys_a.free(gc->sys_a.ctx, out);
        return res_v;
    }
    enki_value res_v = enki_alloc_big_nat(gc, n, out);
    gc->sys_a.free(gc->sys_a.ctx, out);
    return res_v;
}

enki_value enki_alloc_pin(enki_gc* gc, const uint8_t hash_b[32], enki_value inner_v,
                          size_t n_subpins_s, enki_value subpins_v[])
{
    size_t n = sizeof(enki_pin) + (n_subpins_s * sizeof(enki_value));
    enki_pin* new = (enki_pin*)gc->alloc(gc, n);
    if (!new)
        return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_PIN;
    new->h.state_b = WHNF;
    new->inner_v = inner_v;
    new->n_subpins_s = n_subpins_s;
    memcpy(new->hash_b, hash_b, 32);
    if (n_subpins_s > 0)
        memcpy(new->subpins_v, subpins_v, n_subpins_s * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}
enki_app* enki_alloc_app_bare(enki_gc* gc, enki_value fn_v, size_t n_args_s)
{
  enki_value_header* hed = IS_PTR(fn_v) ? ENKI_TO_PTR(fn_v) : NULL;

  ea_assertf(
      hed == NULL || hed->kind_b != ENKI_APP,
      "cannot put app as hd of another app %s",
      enki_print_value(EA_TMP_ALLOC, fn_v, NULL));

    size_t n = sizeof(enki_app) + (n_args_s * sizeof(enki_value));
    enki_app* new = (enki_app*)gc->alloc(gc, n);
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

enki_value enki_alloc_app(enki_gc* gc, enki_value fn_v, size_t n_args_s)
{
    return PTR_TO_ENKI(enki_alloc_app_bare(gc, fn_v, n_args_s));
}

enki_value enki_alloc_cont(enki_gc* gc, size_t n_args_s, enki_value* bas_v)
{
    size_t n = sizeof(enki_cont) + (n_args_s * sizeof(enki_value));
    enki_cont* new = (enki_cont*)gc->alloc(gc, n);
    if (!new)
        return 0;
    new->h.size_s = n;
    new->h.kind_b = ENKI_CONT;
    new->h.state_b = WHNF;
    new->n_args_s = n_args_s;
    memcpy(new->args_v, bas_v, n_args_s * sizeof(enki_value));
    return PTR_TO_ENKI(new);
}

static uint64_t _enki_strnat_direct(char* str_c, size_t str_s)
{
    uint64_t ret_q = 0;
    ea_assertf(str_s <= 8, "strnat bloat %lu", str_s);
    for (size_t i = 0; i < str_s; i++) {
        ret_q |= ((uint64_t)str_c[i] << (8 * i));
    }
    return ret_q;
}

enki_value enki_alloc_strnat(enki_gc* gc, char* str_c, size_t str_s)
{
    if (str_s < 8) {
        return _enki_strnat_direct(str_c, str_s);
    } else {
        enki_value nat = enki_alloc_big_nat_bytes(gc, str_s, str_c);
        return nat;
    }
}

enki_value enki_alloc_cstrnat(enki_gc* gc, char* str_c)
{
    size_t str_s = strlen(str_c);
    return enki_alloc_strnat(gc, str_c, str_s);
}

enki_value enki_alloc_pair(enki_gc* gc, enki_value l_v, enki_value r_v)
{
    enki_app* app = enki_alloc_app_bare(gc, l_v, 1);
    app->args_v[0] = r_v;
    return PTR_TO_ENKI(app);
}

enki_value enki_alloc_trel(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, 2);
    app->args_v[0] = one_v;
    app->args_v[1] = two_v;
    return PTR_TO_ENKI(app);
}
enki_value enki_alloc_quad(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v,
                           enki_value tri_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, 3);
    app->args_v[0] = one_v;
    app->args_v[1] = two_v;
    app->args_v[2] = tri_v;
    return PTR_TO_ENKI(app);
}

enki_value enki_alloc_row(enki_gc* gc, enki_value fn_v, size_t arg_s, enki_value* arg_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, arg_s);
    memcpy(app->args_v, arg_v, sizeof(enki_value) * arg_s);
    return PTR_TO_ENKI(app);
}

enki_value enki_app_hd(enki_value app_v)
{
    enki_app* app = (enki_app*)ENKI_TO_PTR(app_v);
    char kin_b = app->h.kind_b;
    ea_assertf(kin_b == ENKI_APP, "not app %u\n", kin_b);
    return app->fn_v;
}
/// XX: this should be discouraged in hot loops,
/// avoid unmasking and remasking
enki_value enki_app_idx(enki_value app_v, size_t idx_s)
{
    enki_app* app = (enki_app*)ENKI_TO_PTR(app_v);
    char kin_b = app->h.kind_b;
    ea_assertf(kin_b == ENKI_APP, "not app %u\n", kin_b);
    return app->args_v[idx_s];
}

size_t enki_bat_met_bytes(enki_nat* nat)
{
    ea_assertf(nat->n_limbs_s > 0, "empty big nat");
    size_t ret_s = nat->n_limbs_s * sizeof(mp_limb_t);
    uint64_t last_q = nat->limbs[nat->n_limbs_s - 1];
    ea_assertf(last_q > 0, "found unnormalised limb %llu", last_q);
    int trim_i = __builtin_clzll(last_q) >> 3;
    return ret_s - (uint64_t)trim_i;
}

enki_value enki_alloc_quin(enki_gc* gc, enki_value fn_v, enki_value one_v, enki_value two_v,
                           enki_value tri_v, enki_value qua_v)
{
    enki_app* app = enki_alloc_app_bare(gc, fn_v, 4);
    app->args_v[0] = one_v;
    app->args_v[1] = two_v;
    app->args_v[2] = tri_v;
    app->args_v[3] = qua_v;
    return PTR_TO_ENKI(app);
}

static unsigned char empty_pinhash[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

enki_value enki_make_pin(enki_gc* gc, enki_value val_v)
{
  return enki_alloc_pin(gc, empty_pinhash, val_v, 0, NULL);
}


// enki_value enki_make_law(
//   enki_gc* gc,
//   uint64_t ari_q,
//   enki_value nam_v
//   enki_value bod_v
// ) {
//
//
// }


enki_value enki_make_hole(enki_gc* gc)
{
  return PTR_TO_ENKI(enki_alloc_app_bare(gc, 0, 0));
}

uint8_t* enki_law_bc(enki_law* law)
{
  return (uint8_t*)(law->const_v + law->n_const_s);
}


enki_value* enki_law_consts(enki_law* law)
{
  return law->const_v;
}

static uint32_t _pin_arity(enki_value val_v)
{

  if (!IS_PTR(val_v)) { return 1; }
  obj_header* h = (obj_header*)ENKI_TO_PTR(val_v);
  switch (h->kind_b) {
    case ENKI_BIG_NAT:
      return 1;
    case ENKI_LAW:
      return enki_arity(val_v);
    default:
      return 0;
  }
}

uint32_t enki_arity(enki_value val_v) {
  if (!IS_PTR(val_v)) { return 0; }
  obj_header* h = (obj_header*)ENKI_TO_PTR(val_v);
  switch ( h->kind_b ) {
    case ENKI_PIN:
      return _pin_arity(((enki_pin*)h)->inner_v);
    case ENKI_APP:
      enki_app* app = (enki_app*)h;
      return (uint32_t)(enki_arity(app->fn_v) - (uint32_t)app->n_args_s);
    case ENKI_BIG_NAT:
      return 0;
    case ENKI_LAW:
      enki_law* law = (enki_law*)h;
      return law->arity_s;
    default:
      die("weird kind");
      return 0;
  }
}

enki_value enki_app_weld(enki_gc* gc, enki_app* old, size_t add_s, enki_value* add_v)
{
  if (add_s == 0) {
    return PTR_TO_ENKI(old);
  }
  enki_app* app = enki_alloc_app_bare(gc,old->fn_v, add_s + old->n_args_s);
  memcpy(app->args_v, old->args_v, sizeof(enki_value) * old->n_args_s);
  memcpy(&app->args_v[old->n_args_s], add_v, sizeof(enki_value) * add_s);
  return PTR_TO_ENKI(app);
}

enki_value enki_unpin(enki_value pin_v)
{
  enki_pin* pin = (enki_pin*)pin_v;
  return pin->inner_v;
}
