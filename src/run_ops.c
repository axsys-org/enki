#include "enki/run_ops.h"

#include <gmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "enki/interp.h"
#include "enki/profile.h"

#define EO_SMALL_MAX UINT64_C(0x7fffffffffffffff)
#define EO_S8(a, b, c, d, e, f, g, h) \
    ((er_val)(PLAN_S7(a, b, c, d, e, f, g) | (PLAN_CH(h) << 56u)))

enum {
    EO_OP66_STORE = OP66_PRINT_REX + 1,
    EO_OP66_MET,
};

static bool eo_is_nat(er_val v)
{
    return er_is_cat(v) || er_is_tag(er_tag_bat, v);
}

static bool eo_nat_to_mpz(er_val v, mpz_t out)
{
    if (er_is_cat(v)) {
        uint64_t limb = v;
        mpz_import(out, 1, -1, sizeof(limb), 0, 0, &limb);
        return true;
    }

    er_bat* bat = er_outt(er_tag_bat, v);
    if (bat == NULL) {
        mpz_set_ui(out, 0);
        return false;
    }
    mpz_import(out, bat->lim_s, -1, sizeof(uint64_t), 0, 0, bat->lim_q);
    return true;
}

static void eo_gmp_free(void* ptr, size_t size_s)
{
    void (*free_fn)(void*, size_t) = NULL;
    mp_get_memory_functions(NULL, NULL, &free_fn);
    free_fn(ptr, size_s);
}

static er_val eo_mpz_to_nat(const enki_allocator* loc_a, const mpz_t z)
{
    ENKI_PROFILE_ZONE("eo_mpz_to_nat");
    if (mpz_sgn(z) <= 0) {
        return 0;
    }

    size_t limb_s = 0;
    uint64_t* limb_q = (uint64_t*)mpz_export(NULL, &limb_s, -1, sizeof(uint64_t), 0, 0, z);
    if (limb_s == 0) {
        return 0;
    }
    if (limb_s == 1 && limb_q[0] <= EO_SMALL_MAX) {
        er_val out_v = limb_q[0];
        eo_gmp_free(limb_q, limb_s * sizeof(uint64_t));
        return out_v;
    }

    er_bat* bat = er_bat_alloc(loc_a, limb_s);
    if (bat == NULL) {
        eo_gmp_free(limb_q, limb_s * sizeof(uint64_t));
        return er_bad;
    }
    er_val out_v = er_bat_init(bat, limb_s, limb_q);
    eo_gmp_free(limb_q, limb_s * sizeof(uint64_t));
    return out_v == 0 ? er_bad : out_v;
}

static bool eo_nat_to_size(er_val v, size_t* out_s)
{
    if (!eo_is_nat(v)) {
        return false;
    }
    if (er_is_cat(v)) {
        *out_s = (size_t)v;
        return (er_val)*out_s == v;
    }

    bool ok = false;
    mpz_t z;
    mpz_init(z);
    if (eo_nat_to_mpz(v, z) && mpz_fits_ulong_p(z)) {
        unsigned long x = mpz_get_ui(z);
        *out_s = (size_t)x;
        ok = (unsigned long)*out_s == x;
    }
    mpz_clear(z);
    return ok;
}

static bool eo_mul_size(size_t a_s, size_t b_s, size_t* out_s)
{
    if (b_s != 0 && a_s > SIZE_MAX / b_s) {
        return false;
    }
    *out_s = a_s * b_s;
    return true;
}

static bool eo_add_size(size_t a_s, size_t b_s, size_t* out_s)
{
    if (a_s > SIZE_MAX - b_s) {
        return false;
    }
    *out_s = a_s + b_s;
    return true;
}

static er_val eo_app_make(const enki_allocator* loc_a, er_val fn_v, size_t arg_s,
                          const er_val arg_v[])
{
    ENKI_PROFILE_ZONE("eo_app_make");
    er_app* app = er_app_alloc(loc_a, arg_s);
    if (app == NULL) {
        return er_bad;
    }
    er_val out_v = er_app_init(app, fn_v, arg_s, arg_v);
    return out_v == 0 ? er_bad : out_v;
}

static er_val eo_thk_app(const enki_allocator* loc_a, er_val fn_v, size_t arg_s,
                         const er_val arg_v[])
{
    ENKI_PROFILE_ZONE("eo_thk_app");
    if (arg_s == SIZE_MAX) {
        return er_bad;
    }
    er_thk* thk = er_thk_alloc(loc_a, arg_s + 1);
    if (thk == NULL) {
        return er_bad;
    }
    er_val out_v = er_thk_init(thk, ER_XUNK_APP, arg_s + 1, NULL);
    if (out_v == 0) {
        return er_bad;
    }
    thk->arg_v[0] = fn_v;
    if (arg_s > 0) {
        memcpy(thk->arg_v + 1, arg_v, arg_s * sizeof(er_val));
    }
    return out_v;
}

static er_val eo_binary_nat(const enki_allocator* loc_a, er_val a_v, er_val b_v,
                            void (*op)(mpz_t out, const mpz_t a, const mpz_t b))
{
    ENKI_PROFILE_ZONE("eo_binary_nat");
    er_val out_v = 0;
    mpz_t a;
    mpz_t b;
    mpz_t out;
    mpz_inits(a, b, out, NULL);
    if (eo_nat_to_mpz(a_v, a) && eo_nat_to_mpz(b_v, b)) {
        op(out, a, b);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(a, b, out, NULL);
    return out_v;
}

static void eo_mpz_add(mpz_t out, const mpz_t a, const mpz_t b)
{
    mpz_add(out, a, b);
}

static void eo_mpz_mul(mpz_t out, const mpz_t a, const mpz_t b)
{
    mpz_mul(out, a, b);
}

er_val eo_nat(er_val v)
{
    ENKI_PROFILE_ZONE("eo_nat");
    return eo_is_nat(v) ? v : 0;
}

er_val eo_add(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_add");
    return eo_binary_nat(loc_a, a_v, b_v, eo_mpz_add);
}

er_val eo_sub(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_sub");
    er_val out_v = 0;
    mpz_t a;
    mpz_t b;
    mpz_t out;
    mpz_inits(a, b, out, NULL);
    if (eo_nat_to_mpz(a_v, a) && eo_nat_to_mpz(b_v, b) && mpz_cmp(a, b) >= 0) {
        mpz_sub(out, a, b);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(a, b, out, NULL);
    return out_v;
}

er_val eo_mul(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_mul");
    return eo_binary_nat(loc_a, a_v, b_v, eo_mpz_mul);
}

er_val eo_div(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_div");
    er_val out_v = 0;
    mpz_t a;
    mpz_t b;
    mpz_t out;
    mpz_inits(a, b, out, NULL);
    if (eo_nat_to_mpz(a_v, a) && eo_nat_to_mpz(b_v, b) && mpz_sgn(b) != 0) {
        mpz_fdiv_q(out, a, b);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(a, b, out, NULL);
    return out_v;
}

er_val eo_mod(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_mod");
    er_val out_v = 0;
    mpz_t a;
    mpz_t b;
    mpz_t out;
    mpz_inits(a, b, out, NULL);
    if (eo_nat_to_mpz(a_v, a) && eo_nat_to_mpz(b_v, b) && mpz_sgn(b) != 0) {
        mpz_fdiv_r(out, a, b);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(a, b, out, NULL);
    return out_v;
}

er_val eo_lsh(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_lsh");
    size_t shift_s = 0;
    if (!eo_nat_to_size(b_v, &shift_s)) {
        return 0;
    }

    er_val out_v = 0;
    mpz_t a;
    mpz_t out;
    mpz_inits(a, out, NULL);
    if (eo_nat_to_mpz(a_v, a)) {
        mpz_mul_2exp(out, a, (mp_bitcnt_t)shift_s);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(a, out, NULL);
    return out_v;
}

er_val eo_rsh(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_rsh");
    (void)loc_a;
    size_t shift_s = 0;
    if (!eo_nat_to_size(b_v, &shift_s)) {
        return 0;
    }

    er_val out_v = 0;
    mpz_t a;
    mpz_t out;
    mpz_inits(a, out, NULL);
    if (eo_nat_to_mpz(a_v, a)) {
        mpz_fdiv_q_2exp(out, a, (mp_bitcnt_t)shift_s);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(a, out, NULL);
    return out_v;
}

er_val eo_cmp(er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_cmp");
    er_val out_v = 0;
    mpz_t a;
    mpz_t b;
    mpz_inits(a, b, NULL);
    if (eo_nat_to_mpz(a_v, a) && eo_nat_to_mpz(b_v, b)) {
        int cmp = mpz_cmp(a, b);
        out_v = cmp < 0 ? 0 : (cmp == 0 ? 1 : 2);
    }
    mpz_clears(a, b, NULL);
    return out_v;
}

er_val eo_eq(er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_eq");
    er_val out_v = 0;
    mpz_t a;
    mpz_t b;
    mpz_inits(a, b, NULL);
    if (eo_nat_to_mpz(a_v, a) && eo_nat_to_mpz(b_v, b)) {
        out_v = mpz_cmp(a, b) == 0 ? 1 : 0;
    }
    mpz_clears(a, b, NULL);
    return out_v;
}

er_val eo_le(er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_le");
    er_val out_v = 0;
    mpz_t a;
    mpz_t b;
    mpz_inits(a, b, NULL);
    if (eo_nat_to_mpz(a_v, a) && eo_nat_to_mpz(b_v, b)) {
        out_v = mpz_cmp(a, b) <= 0 ? 1 : 0;
    }
    mpz_clears(a, b, NULL);
    return out_v;
}

er_val eo_test(er_val bit_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_test");
    size_t bit_s = 0;
    if (!eo_nat_to_size(bit_v, &bit_s)) {
        return 0;
    }

    er_val out_v = 0;
    mpz_t n;
    mpz_init(n);
    if (eo_nat_to_mpz(n_v, n)) {
        out_v = mpz_tstbit(n, (mp_bitcnt_t)bit_s) != 0 ? 1 : 0;
    }
    mpz_clear(n);
    return out_v;
}

er_val eo_load(const enki_allocator* loc_a, er_val idx_v, er_val width_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_load");
    size_t idx_s = 0;
    size_t width_s = 0;
    size_t off_s = 0;
    if (!eo_nat_to_size(idx_v, &idx_s) || !eo_nat_to_size(width_v, &width_s) ||
        !eo_mul_size(idx_s, width_s, &off_s)) {
        return 0;
    }
    if (width_s == 0) {
        return 0;
    }

    er_val out_v = 0;
    mpz_t n;
    mpz_t out;
    mpz_inits(n, out, NULL);
    if (eo_nat_to_mpz(n_v, n)) {
        mpz_fdiv_q_2exp(out, n, (mp_bitcnt_t)off_s);
        mpz_fdiv_r_2exp(out, out, (mp_bitcnt_t)width_s);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(n, out, NULL);
    return out_v;
}

er_val eo_loadn(const enki_allocator* loc_a, er_val width_v, er_val idx_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_loadn");
    return eo_load(loc_a, idx_v, width_v, n_v);
}

er_val eo_store(const enki_allocator* loc_a, er_val idx_v, er_val val_v, er_val width_v,
                er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_store");
    size_t idx_s = 0;
    size_t width_s = 0;
    size_t off_s = 0;
    size_t top_s = 0;
    if (!eo_nat_to_size(idx_v, &idx_s) || !eo_nat_to_size(width_v, &width_s) ||
        !eo_mul_size(idx_s, width_s, &off_s) || !eo_add_size(off_s, width_s, &top_s)) {
        return 0;
    }

    er_val out_v = 0;
    mpz_t n;
    mpz_t v;
    mpz_t low;
    mpz_t mid;
    mpz_t high;
    mpz_t out;
    mpz_inits(n, v, low, mid, high, out, NULL);
    if (eo_nat_to_mpz(n_v, n) && eo_nat_to_mpz(val_v, v)) {
        if (width_s == 0) {
            mpz_set(out, n);
        } else {
            mpz_fdiv_r_2exp(low, n, (mp_bitcnt_t)off_s);
            mpz_fdiv_r_2exp(mid, v, (mp_bitcnt_t)width_s);
            mpz_fdiv_q_2exp(high, n, (mp_bitcnt_t)top_s);
            mpz_mul_2exp(high, high, (mp_bitcnt_t)width_s);
            mpz_ior(high, high, mid);
            mpz_mul_2exp(high, high, (mp_bitcnt_t)off_s);
            mpz_ior(out, high, low);
        }
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(n, v, low, mid, high, out, NULL);
    return out_v;
}

er_val eo_storen(const enki_allocator* loc_a, er_val width_v, er_val idx_v, er_val val_v,
                 er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_storen");
    return eo_store(loc_a, idx_v, val_v, width_v, n_v);
}

er_val eo_trunc(const enki_allocator* loc_a, er_val width_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_trunc");
    size_t width_s = 0;
    if (!eo_nat_to_size(width_v, &width_s)) {
        return 0;
    }

    er_val out_v = 0;
    mpz_t n;
    mpz_t out;
    mpz_inits(n, out, NULL);
    if (eo_nat_to_mpz(n_v, n)) {
        mpz_fdiv_r_2exp(out, n, (mp_bitcnt_t)width_s);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(n, out, NULL);
    return out_v;
}

er_val eo_truncn(const enki_allocator* loc_a, er_val width_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_truncn");
    return eo_trunc(loc_a, width_v, n_v);
}

er_val eo_met(er_val width_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_met");
    size_t width_s = 0;
    if (!eo_nat_to_size(width_v, &width_s) || width_s == 0 || !eo_is_nat(n_v)) {
        return 0;
    }

    mpz_t n;
    mpz_init(n);
    if (!eo_nat_to_mpz(n_v, n) || mpz_sgn(n) == 0) {
        mpz_clear(n);
        return 0;
    }
    size_t bit_s = mpz_sizeinbase(n, 2);
    mpz_clear(n);
    return (er_val)((bit_s + width_s - 1) / width_s);
}

er_val eo_inc(const enki_allocator* loc_a, er_val v)
{
    ENKI_PROFILE_ZONE("eo_inc");
    return eo_add(loc_a, v, 1);
}

er_val eo_dec(const enki_allocator* loc_a, er_val v)
{
    ENKI_PROFILE_ZONE("eo_dec");
    return eo_sub(loc_a, v, 1);
}

er_val eo_bex(const enki_allocator* loc_a, er_val bit_v)
{
    ENKI_PROFILE_ZONE("eo_bex");
    return eo_lsh(loc_a, 1, bit_v);
}

er_val eo_bits(er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_bits");
    return eo_met(1, n_v);
}

er_val eo_bytes(er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_bytes");
    return eo_met(8, n_v);
}

er_val eo_load8(const enki_allocator* loc_a, er_val idx_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_load8");
    return eo_load(loc_a, idx_v, 8, n_v);
}

er_val eo_store8(const enki_allocator* loc_a, er_val idx_v, er_val val_v, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_store8");
    return eo_store(loc_a, idx_v, val_v, 8, n_v);
}

er_val eo_trunc8(const enki_allocator* loc_a, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_trunc8");
    return eo_trunc(loc_a, 8, n_v);
}

er_val eo_trunc16(const enki_allocator* loc_a, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_trunc16");
    return eo_trunc(loc_a, 16, n_v);
}

er_val eo_trunc32(const enki_allocator* loc_a, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_trunc32");
    return eo_trunc(loc_a, 32, n_v);
}

er_val eo_trunc64(const enki_allocator* loc_a, er_val n_v)
{
    ENKI_PROFILE_ZONE("eo_trunc64");
    return eo_trunc(loc_a, 64, n_v);
}

er_val eo_nam(er_val law_v)
{
    ENKI_PROFILE_ZONE("eo_nam");
    er_law* law = er_outt(er_tag_law, law_v);
    return law == NULL ? 0 : law->name_v;
}

er_val eo_body(er_val law_v)
{
    ENKI_PROFILE_ZONE("eo_body");
    er_law* law = er_outt(er_tag_law, law_v);
    return law == NULL ? 0 : law->body_v;
}

er_val eo_unpin(er_val pin_v)
{
    ENKI_PROFILE_ZONE("eo_unpin");
    er_pin* pin = er_outt(er_tag_pin, pin_v);
    return pin == NULL ? 0 : pin->val_v;
}

er_val eo_sz(er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_sz");
    er_app* app = er_outt(er_tag_app, row_v);
    return app == NULL ? 0 : (er_val)app->arg_s;
}

er_val eo_last(er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_last");
    er_app* app = er_outt(er_tag_app, row_v);
    return app == NULL || app->arg_s == 0 ? 0 : app->arg_v[app->arg_s - 1];
}

er_val eo_init(const enki_allocator* loc_a, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_init");
    er_app* app = er_outt(er_tag_app, row_v);
    if (app == NULL || app->arg_s == 0) {
        return 0;
    }
    return eo_app_make(loc_a, app->fn_v, app->arg_s - 1, app->arg_v);
}

er_val eo_rep(const enki_allocator* loc_a, er_val hd_v, er_val item_v, er_val count_v)
{
    ENKI_PROFILE_ZONE("eo_rep");
    size_t count_s = 0;
    if (!eo_nat_to_size(count_v, &count_s)) {
        return 0;
    }
    er_app* app = er_app_alloc(loc_a, count_s);
    if (app == NULL) {
        return er_bad;
    }
    er_val out_v = er_app_init(app, hd_v, count_s, NULL);
    if (out_v == 0) {
        return er_bad;
    }
    for (size_t k = 0; k < count_s; k++) {
        app->arg_v[k] = item_v;
    }
    return out_v;
}

er_val eo_slice(const enki_allocator* loc_a, er_val off_v, er_val count_v, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_slice");
    size_t off_s = 0;
    size_t count_s = 0;
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL || !eo_nat_to_size(off_v, &off_s) || !eo_nat_to_size(count_v, &count_s) ||
        off_s > row->arg_s || count_s > row->arg_s - off_s) {
        return 0;
    }
    return eo_app_make(loc_a, 0, count_s, row->arg_v + off_s);
}

er_val eo_weld(const enki_allocator* loc_a, er_val x_v, er_val y_v)
{
    ENKI_PROFILE_ZONE("eo_weld");
    er_app* x = er_outt(er_tag_app, x_v);
    er_app* y = er_outt(er_tag_app, y_v);
    size_t total_s = 0;
    if (x == NULL || y == NULL || !eo_add_size(x->arg_s, y->arg_s, &total_s)) {
        return 0;
    }
    er_app* out = er_app_alloc(loc_a, total_s);
    if (out == NULL) {
        return er_bad;
    }
    er_val out_v = er_app_init(out, x->fn_v, total_s, NULL);
    if (out_v == 0) {
        return er_bad;
    }
    memcpy(out->arg_v, x->arg_v, x->arg_s * sizeof(er_val));
    memcpy(out->arg_v + x->arg_s, y->arg_v, y->arg_s * sizeof(er_val));
    return out_v;
}

er_val eo_up(const enki_allocator* loc_a, er_val idx_v, er_val val_v, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_up");
    size_t idx_s = 0;
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL || !eo_nat_to_size(idx_v, &idx_s)) {
        return 0;
    }
    er_val out_v = eo_app_make(loc_a, row->fn_v, row->arg_s, row->arg_v);
    er_app* out = er_outt(er_tag_app, out_v);
    if (out != NULL && idx_s < out->arg_s) {
        out->arg_v[idx_s] = val_v;
    }
    return out_v;
}

er_val eo_coup(const enki_allocator* loc_a, er_val hd_v, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_coup");
    er_app* row = er_outt(er_tag_app, row_v);
    return row == NULL ? 0 : eo_app_make(loc_a, hd_v, row->arg_s, row->arg_v);
}

er_val eo_hd(er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_hd");
    er_app* row = er_outt(er_tag_app, row_v);
    return row == NULL ? 0 : row->fn_v;
}

er_val eo_ix(er_val idx_v, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_ix");
    size_t idx_s = 0;
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL || !eo_nat_to_size(idx_v, &idx_s) || idx_s >= row->arg_s) {
        return 0;
    }
    return row->arg_v[idx_s];
}

er_val eo_not(er_val v)
{
    ENKI_PROFILE_ZONE("eo_not");
    return v == 0 ? 1 : 0;
}

er_val eo_tru(er_val v)
{
    ENKI_PROFILE_ZONE("eo_tru");
    return v == 0 ? 0 : 1;
}

er_val eo_or(er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_or");
    return a_v != 0 || b_v != 0 ? 1 : 0;
}

er_val eo_and(er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_and");
    return a_v != 0 && b_v != 0 ? 1 : 0;
}

er_val eo_pin(const enki_allocator* loc_a, er_val v)
{
    ENKI_PROFILE_ZONE("eo_pin");
    er_val out_v = er_pin_make(loc_a, v);
    return out_v == 0 ? er_bad : out_v;
}

er_val eo_law(const enki_allocator* loc_a, er_val nam_v, er_val bod_v, er_val ari_v)
{
    ENKI_PROFILE_ZONE("eo_law");
    size_t ari_s = 0;
    if (!eo_nat_to_size(ari_v, &ari_s) || ari_s > UINT32_MAX - 1) {
        return 0;
    }
    er_val out_v = er_law_make(loc_a, nam_v, bod_v, (uint32_t)ari_s);
    return out_v == 0 ? er_bad : out_v;
}

er_val eo_elim(const enki_allocator* loc_a, er_val pin_f_v, er_val law_f_v, er_val app_f_v,
               er_val zero_v, er_val nat_f_v, er_val val_v)
{
    ENKI_PROFILE_ZONE("eo_elim");
    er_pin* pin = er_outt(er_tag_pin, val_v);
    if (pin != NULL) {
        return eo_thk_app(loc_a, pin_f_v, 1, &pin->val_v);
    }

    er_law* law = er_outt(er_tag_law, val_v);
    if (law != NULL) {
        er_val args_v[] = {law->ari_d, law->name_v, law->body_v};
        return eo_thk_app(loc_a, law_f_v, 3, args_v);
    }

    er_app* app = er_outt(er_tag_app, val_v);
    if (app != NULL) {
        er_val args_v[] = {app->fn_v, val_v};
        return eo_thk_app(loc_a, app_f_v, 2, args_v);
    }

    if (!eo_is_nat(val_v)) {
        return 0;
    }
    if (eo_eq(val_v, 0) != 0) {
        return zero_v;
    }
    er_val dec_v = eo_dec(loc_a, val_v);
    return dec_v == er_bad ? er_bad : eo_thk_app(loc_a, nat_f_v, 1, &dec_v);
}

bool eo_op66_from_tag(er_val tag_v, int* out_op)
{
    ENKI_PROFILE_ZONE("eo_op66_from_tag");
    if (!er_is_cat(tag_v)) {
        return false;
    }
    if (tag_v <= (er_val)OP66_PRINT_REX) {
        *out_op = (int)tag_v;
        return true;
    }
    switch (tag_v) {
    case PLAN_S3('I', 'n', 'c'):
        *out_op = OP66_INC;
        return true;
    case PLAN_S3('D', 'e', 'c'):
        *out_op = OP66_DEC;
        return true;
    case PLAN_S3('A', 'd', 'd'):
        *out_op = OP66_ADD;
        return true;
    case PLAN_S3('S', 'u', 'b'):
        *out_op = OP66_SUB;
        return true;
    case PLAN_S3('M', 'u', 'l'):
        *out_op = OP66_MUL;
        return true;
    case PLAN_S3('D', 'i', 'v'):
        *out_op = OP66_DIV;
        return true;
    case PLAN_S3('M', 'o', 'd'):
        *out_op = OP66_MOD;
        return true;
    case PLAN_S2('E', 'q'):
        *out_op = OP66_EQ;
        return true;
    case PLAN_S2('L', 'e'):
        *out_op = OP66_LE;
        return true;
    case PLAN_S3('C', 'm', 'p'):
        *out_op = OP66_CMP;
        return true;
    case PLAN_S3('R', 's', 'h'):
        *out_op = OP66_RSH;
        return true;
    case PLAN_S3('L', 's', 'h'):
        *out_op = OP66_LSH;
        return true;
    case PLAN_S4('T', 'e', 's', 't'):
        *out_op = OP66_TEST;
        return true;
    case PLAN_S4('N', 'a', 'm', 'e'):
    case PLAN_S3('N', 'a', 'm'):
        *out_op = OP66_NAME;
        return true;
    case PLAN_S4('B', 'o', 'd', 'y'):
        *out_op = OP66_BODY;
        return true;
    case PLAN_S5('U', 'n', 'p', 'i', 'n'):
        *out_op = OP66_UNPIN;
        return true;
    case PLAN_S2('S', 'z'):
        *out_op = OP66_SZ;
        return true;
    case PLAN_S4('L', 'a', 's', 't'):
        *out_op = OP66_LAST;
        return true;
    case PLAN_S4('I', 'n', 'i', 't'):
        *out_op = OP66_INIT;
        return true;
    case PLAN_S3('R', 'e', 'p'):
        *out_op = OP66_REP;
        return true;
    case PLAN_S5('S', 'l', 'i', 'c', 'e'):
        *out_op = OP66_SLICE;
        return true;
    case PLAN_S4('W', 'e', 'l', 'd'):
        *out_op = OP66_WELD;
        return true;
    case PLAN_S2('U', 'p'):
        *out_op = OP66_UP;
        return true;
    case PLAN_S4('C', 'o', 'u', 'p'):
        *out_op = OP66_COUP;
        return true;
    case PLAN_S2('H', 'd'):
        *out_op = OP66_HD;
        return true;
    case PLAN_S2('I', 'x'):
        *out_op = OP66_IX;
        return true;
    case PLAN_S3('N', 'o', 't'):
        *out_op = OP66_NIL;
        return true;
    case PLAN_S3('T', 'r', 'u'):
    case PLAN_S5('T', 'r', 'u', 't', 'h'):
        *out_op = OP66_TRUTH;
        return true;
    case PLAN_S2('O', 'r'):
        *out_op = OP66_OR;
        return true;
    case PLAN_S3('A', 'n', 'd'):
        *out_op = OP66_AND;
        return true;
    case PLAN_S6('O', 'P', '_', 'N', 'A', 'M'):
        *out_op = OP66_NAME;
        return true;
    case PLAN_S7('O', 'P', '_', 'B', 'O', 'D', 'Y'):
        *out_op = OP66_BODY;
        return true;
    case EO_S8('O', 'P', '_', 'U', 'N', 'P', 'I', 'N'):
        *out_op = OP66_UNPIN;
        return true;
    case PLAN_S5('O', 'P', '_', 'S', 'Z'):
        *out_op = OP66_SZ;
        return true;
    case PLAN_S7('O', 'P', '_', 'L', 'A', 'S', 'T'):
        *out_op = OP66_LAST;
        return true;
    case PLAN_S7('O', 'P', '_', 'I', 'N', 'I', 'T'):
        *out_op = OP66_INIT;
        return true;
    case PLAN_S6('O', 'P', '_', 'A', 'D', 'D'):
        *out_op = OP66_ADD;
        return true;
    case PLAN_S6('O', 'P', '_', 'S', 'U', 'B'):
        *out_op = OP66_SUB;
        return true;
    case PLAN_S6('O', 'P', '_', 'R', 'S', 'H'):
        *out_op = OP66_RSH;
        return true;
    case PLAN_S6('O', 'P', '_', 'L', 'S', 'H'):
        *out_op = OP66_LSH;
        return true;
    case PLAN_S6('O', 'P', '_', 'D', 'I', 'V'):
        *out_op = OP66_DIV;
        return true;
    case PLAN_S6('O', 'P', '_', 'M', 'U', 'L'):
        *out_op = OP66_MUL;
        return true;
    case PLAN_S6('O', 'P', '_', 'M', 'O', 'D'):
        *out_op = OP66_MOD;
        return true;
    case PLAN_S7('O', 'P', '_', 'T', 'E', 'S', 'T'):
        *out_op = OP66_TEST;
        return true;
    case PLAN_S7('O', 'P', '_', 'L', 'O', 'A', 'D'):
        *out_op = OP66_LOAD;
        return true;
    case EO_S8('O', 'P', '_', 'S', 'T', 'O', 'R', 'E'):
        *out_op = EO_OP66_STORE;
        return true;
    case EO_S8('O', 'P', '_', 'T', 'R', 'U', 'N', 'C'):
        *out_op = OP66_TRUNC;
        return true;
    case PLAN_S6('O', 'P', '_', 'M', 'E', 'T'):
        *out_op = EO_OP66_MET;
        return true;
    case PLAN_S6('O', 'P', '_', 'R', 'E', 'P'):
        *out_op = OP66_REP;
        return true;
    case EO_S8('O', 'P', '_', 'S', 'L', 'I', 'C', 'E'):
        *out_op = OP66_SLICE;
        return true;
    case PLAN_S7('O', 'P', '_', 'W', 'E', 'L', 'D'):
        *out_op = OP66_WELD;
        return true;
    case PLAN_S5('O', 'P', '_', 'U', 'P'):
        *out_op = OP66_UP;
        return true;
    case PLAN_S7('O', 'P', '_', 'C', 'O', 'U', 'P'):
        *out_op = OP66_COUP;
        return true;
    case PLAN_S5('O', 'P', '_', 'H', 'D'):
        *out_op = OP66_HD;
        return true;
    case PLAN_S5('O', 'P', '_', 'I', 'X'):
        *out_op = OP66_IX;
        return true;
    case PLAN_S6('O', 'P', '_', 'N', 'O', 'T'):
        *out_op = OP66_NIL;
        return true;
    case PLAN_S6('O', 'P', '_', 'T', 'R', 'U'):
        *out_op = OP66_TRUTH;
        return true;
    case PLAN_S5('O', 'P', '_', 'O', 'R'):
        *out_op = OP66_OR;
        return true;
    case PLAN_S6('O', 'P', '_', 'A', 'N', 'D'):
        *out_op = OP66_AND;
        return true;
    case PLAN_S5('O', 'P', '_', 'E', 'Q'):
        *out_op = OP66_EQ;
        return true;
    case PLAN_S6('O', 'P', '_', 'C', 'M', 'P'):
        *out_op = OP66_CMP;
        return true;
    default:
        return false;
    }
}

er_val eo_exec_op66(const enki_allocator* loc_a, int op, size_t arg_s, const er_val arg_v[])
{
    ENKI_PROFILE_ZONE("eo_exec_op66");
    switch (op) {
    case OP66_NAME:
        return arg_s == 1 ? eo_nam(arg_v[0]) : 0;
    case OP66_BODY:
        return arg_s == 1 ? eo_body(arg_v[0]) : 0;
    case OP66_UNPIN:
        return arg_s == 1 ? eo_unpin(arg_v[0]) : 0;
    case OP66_SZ:
        return arg_s == 1 ? eo_sz(arg_v[0]) : 0;
    case OP66_LAST:
        return arg_s == 1 ? eo_last(arg_v[0]) : 0;
    case OP66_INIT:
        return arg_s == 1 ? eo_init(loc_a, arg_v[0]) : 0;

    case OP66_ADD:
        return arg_s == 2 ? eo_add(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_SUB:
        return arg_s == 2 ? eo_sub(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_RSH:
        return arg_s == 2 ? eo_rsh(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_LSH:
        return arg_s == 2 ? eo_lsh(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_DIV:
        return arg_s == 2 ? eo_div(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_MUL:
        return arg_s == 2 ? eo_mul(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_MOD:
        return arg_s == 2 ? eo_mod(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_TEST:
        return arg_s == 2 ? eo_test(arg_v[0], arg_v[1]) : 0;
    case OP66_LOAD:
        return arg_s == 3 ? eo_load(loc_a, arg_v[0], arg_v[1], arg_v[2]) : 0;
    case OP66_LOAD8:
        return arg_s == 2 ? eo_load8(loc_a, arg_v[0], arg_v[1]) : 0;
    case EO_OP66_STORE:
        return arg_s == 4 ? eo_store(loc_a, arg_v[0], arg_v[2], arg_v[1], arg_v[3]) : 0;
    case OP66_STORE8:
        return arg_s == 3 ? eo_store8(loc_a, arg_v[0], arg_v[1], arg_v[2]) : 0;
    case OP66_TRUNC:
        return arg_s == 2 ? eo_trunc(loc_a, arg_v[0], arg_v[1]) : 0;
    case EO_OP66_MET:
        return arg_s == 2 ? eo_met(arg_v[0], arg_v[1]) : 0;
    case OP66_TRUNC8:
        return arg_s == 1 ? eo_trunc8(loc_a, arg_v[0]) : 0;
    case OP66_TRUNC16:
        return arg_s == 1 ? eo_trunc16(loc_a, arg_v[0]) : 0;
    case OP66_TRUNC32:
        return arg_s == 1 ? eo_trunc32(loc_a, arg_v[0]) : 0;
    case OP66_TRUNC64:
        return arg_s == 1 ? eo_trunc64(loc_a, arg_v[0]) : 0;
    case OP66_BITS:
        return arg_s == 1 ? eo_bits(arg_v[0]) : 0;
    case OP66_BYTES:
        return arg_s == 1 ? eo_bytes(arg_v[0]) : 0;

    case OP66_REP:
        return arg_s == 3 ? eo_rep(loc_a, arg_v[0], arg_v[1], arg_v[2]) : 0;
    case OP66_SLICE:
        return arg_s == 3 ? eo_slice(loc_a, arg_v[0], arg_v[1], arg_v[2]) : 0;
    case OP66_WELD:
        return arg_s == 2 ? eo_weld(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_UP:
    case OP66_UP_UNIQ:
        return arg_s == 3 ? eo_up(loc_a, arg_v[0], arg_v[1], arg_v[2]) : 0;
    case OP66_COUP:
        return arg_s == 2 ? eo_coup(loc_a, arg_v[0], arg_v[1]) : 0;
    case OP66_HD:
        return arg_s == 1 ? eo_hd(arg_v[0]) : 0;
    case OP66_IX:
        return arg_s == 2 ? eo_ix(arg_v[0], arg_v[1]) : 0;
    case OP66_NIL:
        return arg_s == 1 ? eo_not(arg_v[0]) : 0;
    case OP66_TRUTH:
        return arg_s == 1 ? eo_tru(arg_v[0]) : 0;
    case OP66_OR:
        return arg_s == 2 ? eo_or(arg_v[0], arg_v[1]) : 0;
    case OP66_AND:
        return arg_s == 2 ? eo_and(arg_v[0], arg_v[1]) : 0;
    case OP66_EQ:
        return arg_s == 2 ? eo_eq(arg_v[0], arg_v[1]) : 0;
    case OP66_LE:
        return arg_s == 2 ? eo_le(arg_v[0], arg_v[1]) : 0;
    case OP66_CMP:
        return arg_s == 2 ? eo_cmp(arg_v[0], arg_v[1]) : 0;

    case OP66_INC:
        return arg_s == 1 ? eo_inc(loc_a, arg_v[0]) : 0;
    case OP66_DEC:
        return arg_s == 1 ? eo_dec(loc_a, arg_v[0]) : 0;
    case OP66_BEX:
        return arg_s == 1 ? eo_bex(loc_a, arg_v[0]) : 0;
    default:
        return er_bad;
    }
}

static er_val eo_exec_op66_descriptor(const enki_allocator* loc_a, er_val tag_v, size_t arg_s,
                                      const er_val arg_v[], bool* handled_f)
{
    ENKI_PROFILE_ZONE("eo_exec_op66_descriptor");
    er_app* tag = er_outt(er_tag_app, tag_v);
    if (tag == NULL || tag->arg_s != 1) {
        *handled_f = false;
        return er_bad;
    }

    er_val name_v = tag->fn_v;
    er_val width_v = tag->arg_v[0];
    *handled_f = true;
    switch (name_v) {
    case PLAN_S7('O', 'P', '_', 'L', 'O', 'A', 'D'):
        if (width_v == 0) {
            return arg_s == 3 ? eo_load(loc_a, arg_v[0], arg_v[1], arg_v[2]) : 0;
        }
        return arg_s == 2 ? eo_loadn(loc_a, width_v, arg_v[0], arg_v[1]) : 0;
    case EO_S8('O', 'P', '_', 'S', 'T', 'O', 'R', 'E'):
        if (width_v == 0) {
            return arg_s == 4 ? eo_store(loc_a, arg_v[0], arg_v[2], arg_v[1], arg_v[3]) : 0;
        }
        return arg_s == 3 ? eo_storen(loc_a, width_v, arg_v[0], arg_v[1], arg_v[2]) : 0;
    case EO_S8('O', 'P', '_', 'T', 'R', 'U', 'N', 'C'):
        if (width_v == 0) {
            return arg_s == 2 ? eo_trunc(loc_a, arg_v[0], arg_v[1]) : 0;
        }
        return arg_s == 1 ? eo_truncn(loc_a, width_v, arg_v[0]) : 0;
    case PLAN_S6('O', 'P', '_', 'M', 'E', 'T'):
        return arg_s == 1 ? eo_met(width_v, arg_v[0]) : 0;
    default:
        break;
    }

    int op = 0;
    if (eo_op66_from_tag(name_v, &op)) {
        return eo_exec_op66(loc_a, op, arg_s, arg_v);
    }

    *handled_f = false;
    return er_bad;
}

er_val eo_exec_op66_app(const enki_allocator* loc_a, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_exec_op66_app");
    er_app* row = er_outt(er_tag_app, row_v);
    er_val tag_v = row_v;
    const er_val* arg_v = NULL;
    size_t arg_s = 0;

    if (row != NULL) {
        tag_v = row->fn_v;
        arg_v = row->arg_v;
        arg_s = row->arg_s;
        if (tag_v == 0 && row->arg_s > 0) {
            bool handled_f = false;
            er_val out_v =
                eo_exec_op66_descriptor(loc_a, row->arg_v[0], row->arg_s - 1, row->arg_v + 1,
                                        &handled_f);
            if (handled_f) {
                return out_v;
            }
            int nested = 0;
            if (eo_op66_from_tag(row->arg_v[0], &nested)) {
                out_v = eo_exec_op66(loc_a, nested, row->arg_s - 1, row->arg_v + 1);
                if (out_v != er_bad) {
                    return out_v;
                }
            }
        }
    }

    bool handled_f = false;
    er_val out_v = eo_exec_op66_descriptor(loc_a, tag_v, arg_s, arg_v, &handled_f);
    if (handled_f) {
        return out_v;
    }

    int op = 0;
    return eo_op66_from_tag(tag_v, &op) ? eo_exec_op66(loc_a, op, arg_s, arg_v) : er_bad;
}

er_val eo_exec_op0(const enki_allocator* loc_a, int op, size_t arg_s, const er_val arg_v[])
{
    ENKI_PROFILE_ZONE("eo_exec_op0");
    switch (op) {
    case OP0_PIN:
        return arg_s == 1 ? eo_pin(loc_a, arg_v[0]) : 0;
    case OP0_LAW:
        return arg_s == 3 ? eo_law(loc_a, arg_v[1], arg_v[2], arg_v[0]) : 0;
    case OP0_ELIM:
        return arg_s == 6
                   ? eo_elim(loc_a, arg_v[0], arg_v[1], arg_v[2], arg_v[3], arg_v[4],
                             arg_v[5])
                   : 0;
    default:
        return er_bad;
    }
}

er_val eo_exec_op0_app(const enki_allocator* loc_a, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_exec_op0_app");
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL) {
        return er_bad;
    }

    er_val tag_v = row->fn_v;
    const er_val* arg_v = row->arg_v;
    size_t arg_s = row->arg_s;
    if (tag_v == 0 && row->arg_s > 0) {
        size_t nested_s = 0;
        if (eo_nat_to_size(row->arg_v[0], &nested_s) && nested_s <= OP0_ELIM) {
            er_val out_v = eo_exec_op0(loc_a, (int)nested_s, row->arg_s - 1, row->arg_v + 1);
            if (out_v != er_bad) {
                return out_v;
            }
        }
    }
    size_t op_s = 0;
    return eo_nat_to_size(tag_v, &op_s) && op_s <= OP0_ELIM
               ? eo_exec_op0(loc_a, (int)op_s, arg_s, arg_v)
               : er_bad;
}
