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

static er_val eo_tank(const enki_allocator* loc_a, er_val val_v, char* msg_c)
{
    return er_tank_make(loc_a, val_v, msg_c);
}

static er_val eo_bad_arity(const enki_allocator* loc_a, size_t have_s, size_t want_s)
{
    (void)want_s;
    return eo_tank(loc_a, (er_val)have_s, "bad primitive arity");
}

static bool eo_is_nat(er_val v)
{
    return er_is_cat(v) || er_is_tag(er_tag_bat, v);
}

static bool eo_callable_arity(er_val val_v, uint32_t* out_d)
{
    er_pin* pin;
    er_app* app;
    er_law* law;
    switch (er_get_tag(val_v)) {
    case er_tag_pin:
        pin = er_outa(val_v);
        if (er_is_cat(pin->val_v)) {
            *out_d = 1;
            return true;
        }
        law = er_outt(er_tag_law, pin->val_v);
        if (law == NULL) {
            return false;
        }
        *out_d = law->ari_d;
        return true;
    case er_tag_app: {
        app = er_outa(val_v);
        uint32_t fun_ari_d = 0;
        if (!eo_callable_arity(app->fn_v, &fun_ari_d) || fun_ari_d <= app->arg_s) {
            return false;
        }
        *out_d = fun_ari_d - (uint32_t)app->arg_s;
        return true;
    }
    case er_tag_law:
        law = er_outa(val_v);
        *out_d = law->ari_d;
        return true;
    default:
        return false;
    }
}

static er_val eo_limbs_to_nat(const enki_allocator* loc_a, size_t limb_s,
                              const uint64_t limb_q[])
{
    while (limb_s > 0 && limb_q[limb_s - 1] == 0) {
        limb_s--;
    }
    if (limb_s == 0) {
        return 0;
    }
    if (limb_s == 1 && limb_q[0] <= EO_SMALL_MAX) {
        return limb_q[0];
    }

    er_bat* bat = er_bat_alloc(loc_a, limb_s);
    if (bat == NULL) {
        return er_bad;
    }
    er_val out_v = er_bat_init(bat, limb_s, limb_q);
    return out_v == 0 ? er_bad : out_v;
}

static er_val eo_u64_to_nat(const enki_allocator* loc_a, uint64_t n_q)
{
    return eo_limbs_to_nat(loc_a, 1, &n_q);
}

static void eo_mul_u64(uint64_t a_q, uint64_t b_q, uint64_t* lo_q, uint64_t* hi_q)
{
    const uint64_t mask_q = UINT64_C(0xffffffff);
    uint64_t a_lo_q = a_q & mask_q;
    uint64_t a_hi_q = a_q >> 32u;
    uint64_t b_lo_q = b_q & mask_q;
    uint64_t b_hi_q = b_q >> 32u;

    uint64_t p0_q = a_lo_q * b_lo_q;
    uint64_t p1_q = a_lo_q * b_hi_q;
    uint64_t p2_q = a_hi_q * b_lo_q;
    uint64_t p3_q = a_hi_q * b_hi_q;
    uint64_t mid_q = (p0_q >> 32u) + (p1_q & mask_q) + (p2_q & mask_q);

    *lo_q = (p0_q & mask_q) | (mid_q << 32u);
    *hi_q = p3_q + (p1_q >> 32u) + (p2_q >> 32u) + (mid_q >> 32u);
}

static er_val eo_mul_cats(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    if (a_v == 0 || b_v == 0) {
        return 0;
    }
    if (b_v <= EO_SMALL_MAX / a_v) {
        return a_v * b_v;
    }

    uint64_t limb_q[2] = {0, 0};
    eo_mul_u64(a_v, b_v, &limb_q[0], &limb_q[1]);
    return eo_limbs_to_nat(loc_a, 2, limb_q);
}

static er_val eo_lsh_cat(const enki_allocator* loc_a, er_val a_v, size_t shift_s)
{
    if (a_v == 0) {
        return 0;
    }
    if (shift_s < 63 && a_v <= (EO_SMALL_MAX >> shift_s)) {
        return a_v << shift_s;
    }

    size_t word_shift_s = shift_s / 64u;
    unsigned int bit_shift_s = (unsigned int)(shift_s % 64u);
    uint64_t low_q = a_v;
    uint64_t high_q = 0;
    size_t limb_s = word_shift_s + 1u;

    if (bit_shift_s != 0) {
        low_q = a_v << bit_shift_s;
        high_q = a_v >> (64u - bit_shift_s);
        if (high_q != 0) {
            limb_s++;
        }
    }

    er_bat* bat = er_bat_alloc(loc_a, limb_s);
    if (bat == NULL) {
        return er_bad;
    }
    er_val out_v = er_bat_init(bat, limb_s, NULL);
    if (out_v == 0) {
        return er_bad;
    }
    bat->lim_q[word_shift_s] = low_q;
    if (high_q != 0) {
        bat->lim_q[word_shift_s + 1u] = high_q;
    }
    return out_v;
}

static size_t eo_bits_cat(er_val n_v)
{
    return n_v == 0 ? 0 : 64u - (size_t)__builtin_clzll((unsigned long long)n_v);
}

static uint64_t eo_mask_below(size_t bit_s)
{
    if (bit_s == 0) {
        return 0;
    }
    if (bit_s >= 64) {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << bit_s) - 1u;
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
    er_val out_v = eo_limbs_to_nat(loc_a, limb_s, limb_q);
    eo_gmp_free(limb_q, limb_s * sizeof(uint64_t));
    return out_v;
}

bool eo_nat_to_size(er_val v, size_t* out_s)
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

static size_t eo_nat_size_or(er_val v, size_t nonnat_s, size_t overflow_s)
{
    size_t out_s = 0;
    if (!eo_is_nat(v)) {
        return nonnat_s;
    }
    return eo_nat_to_size(v, &out_s) ? out_s : overflow_s;
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
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        if (a_v <= EO_SMALL_MAX - b_v) {
            return a_v + b_v;
        }
        return eo_u64_to_nat(loc_a, a_v + b_v);
    }
    return eo_binary_nat(loc_a, a_v, b_v, eo_mpz_add);
}

er_val eo_sub(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_sub");
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        return a_v >= b_v ? a_v - b_v : 0;
    }
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
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        return eo_mul_cats(loc_a, a_v, b_v);
    }
    return eo_binary_nat(loc_a, a_v, b_v, eo_mpz_mul);
}

er_val eo_div(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_div");
    a_v = eo_nat(a_v);
    b_v = eo_nat(b_v);
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        return b_v == 0 ? eo_tank(loc_a, b_v, "division by zero") : a_v / b_v;
    }
    er_val out_v = er_bad;
    mpz_t a;
    mpz_t b;
    mpz_t out;
    mpz_inits(a, b, out, NULL);
    bool ok_a_f = eo_nat_to_mpz(a_v, a);
    bool ok_b_f = eo_nat_to_mpz(b_v, b);
    if (!ok_a_f || !ok_b_f) {
        out_v = eo_tank(loc_a, !ok_a_f ? a_v : b_v, "bad numeric argument");
    } else if (mpz_sgn(b) == 0) {
        out_v = eo_tank(loc_a, b_v, "division by zero");
    } else {
        mpz_fdiv_q(out, a, b);
        out_v = eo_mpz_to_nat(loc_a, out);
    }
    mpz_clears(a, b, out, NULL);
    return out_v;
}

er_val eo_mod(const enki_allocator* loc_a, er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_mod");
    a_v = eo_nat(a_v);
    b_v = eo_nat(b_v);
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        return b_v == 0 ? eo_tank(loc_a, b_v, "division by zero") : a_v % b_v;
    }
    er_val out_v = er_bad;
    mpz_t a;
    mpz_t b;
    mpz_t out;
    mpz_inits(a, b, out, NULL);
    bool ok_a_f = eo_nat_to_mpz(a_v, a);
    bool ok_b_f = eo_nat_to_mpz(b_v, b);
    if (!ok_a_f || !ok_b_f) {
        out_v = eo_tank(loc_a, !ok_a_f ? a_v : b_v, "bad numeric argument");
    } else if (mpz_sgn(b) == 0) {
        out_v = eo_tank(loc_a, b_v, "division by zero");
    } else {
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
    if (er_is_cat(a_v)) {
        return eo_lsh_cat(loc_a, a_v, shift_s);
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
    if (er_is_cat(a_v)) {
        return shift_s >= 63 ? 0 : a_v >> shift_s;
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
    a_v = eo_nat(a_v);
    b_v = eo_nat(b_v);
    if (a_v == b_v) {
        return 1;
    }
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        return a_v < b_v ? 0 : 2;
    }
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
    a_v = eo_nat(a_v);
    b_v = eo_nat(b_v);
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        return a_v == b_v ? 1 : 0;
    }
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
    a_v = eo_nat(a_v);
    b_v = eo_nat(b_v);
    if (er_is_cat(a_v) && er_is_cat(b_v)) {
        return a_v <= b_v ? 1 : 0;
    }
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
    if (er_is_cat(n_v)) {
        return bit_s < 63 && ((n_v >> bit_s) & 1u) != 0 ? 1 : 0;
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
    if (er_is_cat(n_v)) {
        if (off_s >= 63) {
            return 0;
        }
        er_val shifted_v = n_v >> off_s;
        return width_s >= 63 ? shifted_v : shifted_v & eo_mask_below(width_s);
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
    if (er_is_cat(n_v) && er_is_cat(val_v)) {
        if (width_s == 0) {
            return n_v;
        }
        if (top_s <= 64) {
            uint64_t low_q = n_v & eo_mask_below(off_s);
            uint64_t mid_q = (val_v & eo_mask_below(width_s)) << off_s;
            uint64_t high_q = n_v & ~eo_mask_below(top_s);
            return eo_u64_to_nat(loc_a, high_q | mid_q | low_q);
        }
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
    if (er_is_cat(n_v)) {
        return width_s >= 63 ? n_v : n_v & eo_mask_below(width_s);
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
    if (er_is_cat(n_v)) {
        size_t bit_s = eo_bits_cat(n_v);
        return bit_s == 0 ? 0 : (er_val)(((bit_s - 1u) / width_s) + 1u);
    }

    mpz_t n;
    mpz_init(n);
    if (!eo_nat_to_mpz(n_v, n) || mpz_sgn(n) == 0) {
        mpz_clear(n);
        return 0;
    }
    size_t bit_s = mpz_sizeinbase(n, 2);
    mpz_clear(n);
    return (er_val)(((bit_s - 1u) / width_s) + 1u);
}

er_val eo_inc(const enki_allocator* loc_a, er_val v)
{
    ENKI_PROFILE_ZONE("eo_inc");
    if (er_is_cat(v)) {
        return v < EO_SMALL_MAX ? v + 1u : eo_u64_to_nat(loc_a, v + 1u);
    }
    return eo_add(loc_a, v, 1);
}

er_val eo_dec(const enki_allocator* loc_a, er_val v)
{
    ENKI_PROFILE_ZONE("eo_dec");
    if (er_is_cat(v)) {
        return v == 0 ? 0 : v - 1u;
    }
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
    if (app->arg_s == 1) {
        return app->fn_v;
    }
    return eo_app_make(loc_a, app->fn_v, app->arg_s - 1, app->arg_v);
}

er_val eo_rep(const enki_allocator* loc_a, er_val hd_v, er_val item_v, er_val count_v)
{
    ENKI_PROFILE_ZONE("eo_rep");
    size_t count_s = 0;
    hd_v = eo_nat(hd_v);
    (void)eo_nat_to_size(count_v, &count_s);
    if (count_s == 0) {
        return hd_v;
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

er_val eo_row(const enki_allocator* loc_a, er_val hd_v, er_val count_v, er_val xs_v)
{
    ENKI_PROFILE_ZONE("eo_row");
    size_t count_s = 0;
    hd_v = eo_nat(hd_v);
    (void)eo_nat_to_size(count_v, &count_s);
    if (count_s == 0) {
        return hd_v;
    }
    er_app* app = er_app_alloc(loc_a, count_s);
    if (app == NULL) {
        return er_bad;
    }
    er_val out_v = er_app_init(app, hd_v, count_s, NULL);
    if (out_v == 0) {
        return er_bad;
    }
    er_val cur_v = xs_v;
    for (size_t k = 0; k < count_s; k++) {
        app->arg_v[k] = eo_ix(0, cur_v);
        cur_v = eo_ix(1, cur_v);
    }
    return out_v;
}

er_val eo_slice(const enki_allocator* loc_a, er_val off_v, er_val count_v, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_slice");
    size_t off_s = eo_nat_size_or(off_v, 0, SIZE_MAX);
    size_t count_s = eo_nat_size_or(count_v, 0, SIZE_MAX);
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL || off_s > row->arg_s) {
        return 0;
    }
    size_t keep_s = row->arg_s - off_s;
    if (keep_s > count_s) {
        keep_s = count_s;
    }
    if (keep_s == 0) {
        return 0;
    }
    return eo_app_make(loc_a, 0, keep_s, row->arg_v + off_s);
}

er_val eo_weld(const enki_allocator* loc_a, er_val x_v, er_val y_v)
{
    ENKI_PROFILE_ZONE("eo_weld");
    er_app* x = er_outt(er_tag_app, x_v);
    er_app* y = er_outt(er_tag_app, y_v);
    size_t x_s = x == NULL ? 0 : x->arg_s;
    size_t y_s = y == NULL ? 0 : y->arg_s;
    size_t total_s = 0;
    if (!eo_add_size(x_s, y_s, &total_s)) {
        return 0;
    }
    er_app* out = er_app_alloc(loc_a, total_s);
    if (out == NULL) {
        return er_bad;
    }
    er_val out_v = er_app_init(out, 0, total_s, NULL);
    if (out_v == 0) {
        return er_bad;
    }
    if (x_s > 0) {
        memcpy(out->arg_v, x->arg_v, x_s * sizeof(er_val));
    }
    if (y_s > 0) {
        memcpy(out->arg_v + x_s, y->arg_v, y_s * sizeof(er_val));
    }
    return out_v;
}

er_val eo_up(const enki_allocator* loc_a, er_val idx_v, er_val val_v, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_up");
    size_t idx_s = eo_nat_size_or(idx_v, 0, SIZE_MAX);
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL || idx_s >= row->arg_s) {
        return row_v;
    }
    er_val out_v = eo_app_make(loc_a, row->fn_v, row->arg_s, row->arg_v);
    er_app* out = er_outt(er_tag_app, out_v);
    if (out == NULL) {
        return out_v;
    }
    out->arg_v[idx_s] = val_v;
    return out_v;
}

er_val eo_coup(const enki_allocator* loc_a, er_val hd_v, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_coup");
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL) {
        return hd_v;
    }
    uint32_t arity_d = 0;
    if (eo_callable_arity(hd_v, &arity_d) && arity_d > row->arg_s) {
        er_app* app = er_app_alloc(loc_a, row->arg_s);
        if (app == NULL) {
            return er_bad;
        }
        er_val app_v = er_app_init(app, hd_v, row->arg_s, row->arg_v);
        return app_v == 0 ? er_bad : app_v;
    }
    return eo_thk_app(loc_a, hd_v, row->arg_s, row->arg_v);
}

er_val eo_hd(er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_hd");
    er_app* row = er_outt(er_tag_app, row_v);
    return row == NULL ? row_v : row->fn_v;
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
    return a_v == 0 ? b_v : a_v;
}

er_val eo_and(er_val a_v, er_val b_v)
{
    ENKI_PROFILE_ZONE("eo_and");
    return a_v == 0 ? 0 : b_v;
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
        return eo_tank(loc_a, ari_v, "bad law arity");
    }
    er_val out_v = er_law_make(loc_a, nam_v, bod_v, (uint32_t)(ari_s + 1));
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
        er_val init_v = eo_init(loc_a, val_v);
        if (!er_is_good(init_v)) {
            return init_v;
        }
        er_val args_v[] = {init_v, eo_last(val_v)};
        return eo_thk_app(loc_a, app_f_v, 2, args_v);
    }

    if (!eo_is_nat(val_v)) {
        return 0;
    }
    if (eo_eq(val_v, 0) != 0) {
        return zero_v;
    }
    er_val dec_v = eo_dec(loc_a, val_v);
    return !er_is_good(dec_v) ? dec_v : eo_thk_app(loc_a, nat_f_v, 1, &dec_v);
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
    case PLAN_S2('N', 'e'):
        *out_op = OP66_NE;
        return true;
    case PLAN_S2('L', 't'):
        *out_op = OP66_LT;
        return true;
    case PLAN_S2('L', 'e'):
        *out_op = OP66_LE;
        return true;
    case PLAN_S2('G', 't'):
        *out_op = OP66_GT;
        return true;
    case PLAN_S2('G', 'e'):
        *out_op = OP66_GE;
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
    case PLAN_S3('S', 'e', 't'):
        *out_op = OP66_SET;
        return true;
    case PLAN_S5('C', 'l', 'e', 'a', 'r'):
        *out_op = OP66_CLEAR;
        return true;
    case PLAN_S3('B', 'e', 'x'):
        *out_op = OP66_BEX;
        return true;
    case PLAN_S4('B', 'i', 't', 's'):
        *out_op = OP66_BITS;
        return true;
    case PLAN_S5('B', 'y', 't', 'e', 's'):
        *out_op = OP66_BYTES;
        return true;
    case PLAN_S3('N', 'i', 'b'):
        *out_op = OP66_NIB;
        return true;
    case PLAN_S5('L', 'o', 'a', 'd', '8'):
        *out_op = OP66_LOAD8;
        return true;
    case PLAN_S6('S', 't', 'o', 'r', 'e', '8'):
        *out_op = OP66_STORE8;
        return true;
    case PLAN_S5('T', 'r', 'u', 'n', 'c'):
        *out_op = OP66_TRUNC;
        return true;
    case PLAN_S6('T', 'r', 'u', 'n', 'c', '8'):
        *out_op = OP66_TRUNC8;
        return true;
    case PLAN_S7('T', 'r', 'u', 'n', 'c', '1', '6'):
        *out_op = OP66_TRUNC16;
        return true;
    case PLAN_S7('T', 'r', 'u', 'n', 'c', '3', '2'):
        *out_op = OP66_TRUNC32;
        return true;
    case PLAN_S7('T', 'r', 'u', 'n', 'c', '6', '4'):
        *out_op = OP66_TRUNC64;
        return true;
    case PLAN_S4('T', 'y', 'p', 'e'):
        *out_op = OP66_TYPE;
        return true;
    case PLAN_S5('I', 's', 'P', 'i', 'n'):
        *out_op = OP66_IS_PIN;
        return true;
    case PLAN_S5('I', 's', 'L', 'a', 'w'):
        *out_op = OP66_IS_LAW;
        return true;
    case PLAN_S5('I', 's', 'A', 'p', 'p'):
        *out_op = OP66_IS_APP;
        return true;
    case PLAN_S5('I', 's', 'N', 'a', 't'):
        *out_op = OP66_IS_NAT;
        return true;
    case PLAN_S5('A', 'r', 'i', 't', 'y'):
        *out_op = OP66_ARITY;
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
    case PLAN_S3('R', 'o', 'w'):
        *out_op = OP66_ROW;
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
    case PLAN_S6('U', 'p', 'U', 'n', 'i', 'q'):
        *out_op = OP66_UP_UNIQ;
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
    case PLAN_S3('I', 'x', '0'):
        *out_op = OP66_IX0;
        return true;
    case PLAN_S3('I', 'x', '1'):
        *out_op = OP66_IX1;
        return true;
    case PLAN_S3('I', 'x', '2'):
        *out_op = OP66_IX2;
        return true;
    case PLAN_S3('I', 'x', '3'):
        *out_op = OP66_IX3;
        return true;
    case PLAN_S3('I', 'x', '4'):
        *out_op = OP66_IX4;
        return true;
    case PLAN_S3('I', 'x', '5'):
        *out_op = OP66_IX5;
        return true;
    case PLAN_S3('I', 'x', '6'):
        *out_op = OP66_IX6;
        return true;
    case PLAN_S3('I', 'x', '7'):
        *out_op = OP66_IX7;
        return true;
    case PLAN_S4('C', 'a', 's', 'e'):
        *out_op = OP66_CASE;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '2'):
        *out_op = OP66_CASE2;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '3'):
        *out_op = OP66_CASE3;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '4'):
        *out_op = OP66_CASE4;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '5'):
        *out_op = OP66_CASE5;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '6'):
        *out_op = OP66_CASE6;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '7'):
        *out_op = OP66_CASE7;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '8'):
        *out_op = OP66_CASE8;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '9'):
        *out_op = OP66_CASE9;
        return true;
    case PLAN_S6('C', 'a', 's', 'e', '1', '0'):
        *out_op = OP66_CASE10;
        return true;
    case PLAN_S6('C', 'a', 's', 'e', '1', '1'):
        *out_op = OP66_CASE11;
        return true;
    case PLAN_S6('C', 'a', 's', 'e', '1', '2'):
        *out_op = OP66_CASE12;
        return true;
    case PLAN_S6('C', 'a', 's', 'e', '1', '3'):
        *out_op = OP66_CASE13;
        return true;
    case PLAN_S6('C', 'a', 's', 'e', '1', '4'):
        *out_op = OP66_CASE14;
        return true;
    case PLAN_S6('C', 'a', 's', 'e', '1', '5'):
        *out_op = OP66_CASE15;
        return true;
    case PLAN_S6('C', 'a', 's', 'e', '1', '6'):
        *out_op = OP66_CASE16;
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
    case PLAN_S3('N', 'o', 'r'):
        *out_op = OP66_NOR;
        return true;
    case PLAN_S3('A', 'n', 'd'):
        *out_op = OP66_AND;
        return true;
    case PLAN_S2('I', 'f'):
        *out_op = OP66_IF;
        return true;
    case PLAN_S3('I', 'f', 'z'):
        *out_op = OP66_IFZ;
        return true;
    case PLAN_S3('S', 'e', 'q'):
        *out_op = OP66_SEQ;
        return true;
    case PLAN_S4('S', 'e', 'q', '2'):
        *out_op = OP66_SEQ2;
        return true;
    case PLAN_S4('S', 'e', 'q', '3'):
        *out_op = OP66_SEQ3;
        return true;
    case PLAN_S3('S', 'a', 'p'):
        *out_op = OP66_SAP;
        return true;
    case PLAN_S4('S', 'a', 'p', '2'):
        *out_op = OP66_SAP2;
        return true;
    case PLAN_S5('F', 'o', 'r', 'c', 'e'):
        *out_op = OP66_FORCE;
        return true;
    case PLAN_S7('D', 'e', 'e', 'p', 's', 'e', 'q'):
    case PLAN_S7('D', 'e', 'e', 'p', 'S', 'e', 'q'):
        *out_op = OP66_DEEPSEQ;
        return true;
    case PLAN_S3('T', 'r', 'y'):
        *out_op = OP66_TRY;
        return true;
    case PLAN_S5('T', 'h', 'r', 'o', 'w'):
        *out_op = OP66_THROW;
        return true;
    case PLAN_S4('S', 'a', 'v', 'e'):
        *out_op = OP66_SAVE;
        return true;
    case PLAN_S4('L', 'o', 'a', 'd'):
        *out_op = OP66_LOAD;
        return true;
    case PLAN_S5('T', 'r', 'a', 'c', 'e'):
        *out_op = OP66_TRACE;
        return true;
    case PLAN_S5('E', 'q', 'u', 'a', 'l'):
        *out_op = OP66_EQUAL;
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
#define EO_ARITY(_want) eo_bad_arity(loc_a, arg_s, (_want))
    switch (op) {
    case OP66_NAME:
        return arg_s == 1 ? eo_nam(arg_v[0]) : EO_ARITY(1);
    case OP66_BODY:
        return arg_s == 1 ? eo_body(arg_v[0]) : EO_ARITY(1);
    case OP66_UNPIN:
        return arg_s == 1 ? eo_unpin(arg_v[0]) : EO_ARITY(1);
    case OP66_SZ:
        return arg_s == 1 ? eo_sz(arg_v[0]) : EO_ARITY(1);
    case OP66_LAST:
        return arg_s == 1 ? eo_last(arg_v[0]) : EO_ARITY(1);
    case OP66_INIT:
        return arg_s == 1 ? eo_init(loc_a, arg_v[0]) : EO_ARITY(1);

    case OP66_ADD:
        return arg_s == 2 ? eo_add(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_SUB:
        return arg_s == 2 ? eo_sub(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_RSH:
        return arg_s == 2 ? eo_rsh(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_LSH:
        return arg_s == 2 ? eo_lsh(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_DIV:
        return arg_s == 2 ? eo_div(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_MUL:
        return arg_s == 2 ? eo_mul(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_MOD:
        return arg_s == 2 ? eo_mod(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_TEST:
        return arg_s == 2 ? eo_test(arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_LOAD:
        return arg_s == 3 ? eo_load(loc_a, arg_v[0], arg_v[1], arg_v[2]) : EO_ARITY(3);
    case OP66_LOAD8:
        return arg_s == 2 ? eo_load8(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case EO_OP66_STORE:
        return arg_s == 4 ? eo_store(loc_a, arg_v[0], arg_v[2], arg_v[1], arg_v[3])
                          : EO_ARITY(4);
    case OP66_STORE8:
        return arg_s == 3 ? eo_store8(loc_a, arg_v[0], arg_v[1], arg_v[2]) : EO_ARITY(3);
    case OP66_TRUNC:
        return arg_s == 2 ? eo_trunc(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case EO_OP66_MET:
        return arg_s == 2 ? eo_met(arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_TRUNC8:
        return arg_s == 1 ? eo_trunc8(loc_a, arg_v[0]) : EO_ARITY(1);
    case OP66_TRUNC16:
        return arg_s == 1 ? eo_trunc16(loc_a, arg_v[0]) : EO_ARITY(1);
    case OP66_TRUNC32:
        return arg_s == 1 ? eo_trunc32(loc_a, arg_v[0]) : EO_ARITY(1);
    case OP66_TRUNC64:
        return arg_s == 1 ? eo_trunc64(loc_a, arg_v[0]) : EO_ARITY(1);
    case OP66_BITS:
        return arg_s == 1 ? eo_bits(arg_v[0]) : EO_ARITY(1);
    case OP66_BYTES:
        return arg_s == 1 ? eo_bytes(arg_v[0]) : EO_ARITY(1);

    case OP66_REP:
        return arg_s == 3 ? eo_rep(loc_a, arg_v[0], arg_v[1], arg_v[2]) : EO_ARITY(3);
    case OP66_SLICE:
        return arg_s == 3 ? eo_slice(loc_a, arg_v[0], arg_v[1], arg_v[2]) : EO_ARITY(3);
    case OP66_WELD:
        return arg_s == 2 ? eo_weld(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_UP:
    case OP66_UP_UNIQ:
        return arg_s == 3 ? eo_up(loc_a, arg_v[0], arg_v[1], arg_v[2]) : EO_ARITY(3);
    case OP66_COUP:
        return arg_s == 2 ? eo_coup(loc_a, arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_HD:
        return arg_s == 1 ? eo_hd(arg_v[0]) : EO_ARITY(1);
    case OP66_IX:
        return arg_s == 2 ? eo_ix(arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_NIL:
        return arg_s == 1 ? eo_not(arg_v[0]) : EO_ARITY(1);
    case OP66_TRUTH:
        return arg_s == 1 ? eo_tru(arg_v[0]) : EO_ARITY(1);
    case OP66_OR:
        return arg_s == 2 ? eo_or(arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_AND:
        return arg_s == 2 ? eo_and(arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_EQ:
        return arg_s == 2 ? eo_eq(arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_LE:
        return arg_s == 2 ? eo_le(arg_v[0], arg_v[1]) : EO_ARITY(2);
    case OP66_CMP:
        return arg_s == 2 ? eo_cmp(arg_v[0], arg_v[1]) : EO_ARITY(2);

    case OP66_INC:
        return arg_s == 1 ? eo_inc(loc_a, arg_v[0]) : EO_ARITY(1);
    case OP66_DEC:
        return arg_s == 1 ? eo_dec(loc_a, arg_v[0]) : EO_ARITY(1);
    case OP66_BEX:
        return arg_s == 1 ? eo_bex(loc_a, arg_v[0]) : EO_ARITY(1);
    default:
        return eo_tank(loc_a, (er_val)op, "bad primitive op");
    }
#undef EO_ARITY
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
            return arg_s == 3 ? eo_load(loc_a, arg_v[0], arg_v[1], arg_v[2])
                              : eo_bad_arity(loc_a, arg_s, 3);
        }
        return arg_s == 2 ? eo_loadn(loc_a, width_v, arg_v[0], arg_v[1])
                          : eo_bad_arity(loc_a, arg_s, 2);
    case EO_S8('O', 'P', '_', 'S', 'T', 'O', 'R', 'E'):
        if (width_v == 0) {
            return arg_s == 4 ? eo_store(loc_a, arg_v[0], arg_v[2], arg_v[1], arg_v[3])
                              : eo_bad_arity(loc_a, arg_s, 4);
        }
        return arg_s == 3 ? eo_storen(loc_a, width_v, arg_v[0], arg_v[1], arg_v[2])
                          : eo_bad_arity(loc_a, arg_s, 3);
    case EO_S8('O', 'P', '_', 'T', 'R', 'U', 'N', 'C'):
        if (width_v == 0) {
            return arg_s == 2 ? eo_trunc(loc_a, arg_v[0], arg_v[1])
                              : eo_bad_arity(loc_a, arg_s, 2);
        }
        return arg_s == 1 ? eo_truncn(loc_a, width_v, arg_v[0])
                          : eo_bad_arity(loc_a, arg_s, 1);
    case PLAN_S6('O', 'P', '_', 'M', 'E', 'T'):
        return arg_s == 1 ? eo_met(width_v, arg_v[0]) : eo_bad_arity(loc_a, arg_s, 1);
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
                return eo_exec_op66(loc_a, nested, row->arg_s - 1, row->arg_v + 1);
            }
            return eo_tank(loc_a, row->arg_v[0], "bad primitive tag");
        }
    }

    bool handled_f = false;
    er_val out_v = eo_exec_op66_descriptor(loc_a, tag_v, arg_s, arg_v, &handled_f);
    if (handled_f) {
        return out_v;
    }

    int op = 0;
    return eo_op66_from_tag(tag_v, &op) ? eo_exec_op66(loc_a, op, arg_s, arg_v)
                                        : eo_tank(loc_a, tag_v, "bad primitive tag");
}

er_val eo_exec_op0(const enki_allocator* loc_a, int op, size_t arg_s, const er_val arg_v[])
{
    ENKI_PROFILE_ZONE("eo_exec_op0");
#define EO_ARITY(_want) eo_bad_arity(loc_a, arg_s, (_want))
    switch (op) {
    case OP0_PIN:
        return arg_s == 1 ? eo_pin(loc_a, arg_v[0]) : EO_ARITY(1);
    case OP0_LAW:
        return arg_s == 3 ? eo_law(loc_a, arg_v[1], arg_v[2], arg_v[0]) : EO_ARITY(3);
    case OP0_ELIM:
        return arg_s == 6
                   ? eo_elim(loc_a, arg_v[0], arg_v[1], arg_v[2], arg_v[3], arg_v[4],
                             arg_v[5])
                   : EO_ARITY(6);
    default:
        return eo_tank(loc_a, (er_val)op, "bad primitive op");
    }
#undef EO_ARITY
}

er_val eo_exec_op0_app(const enki_allocator* loc_a, er_val row_v)
{
    ENKI_PROFILE_ZONE("eo_exec_op0_app");
    er_app* row = er_outt(er_tag_app, row_v);
    if (row == NULL) {
        return eo_tank(loc_a, row_v, "expected primitive row");
    }

    er_val tag_v = row->fn_v;
    const er_val* arg_v = row->arg_v;
    size_t arg_s = row->arg_s;
    if (tag_v == 0 && row->arg_s > 0) {
        size_t nested_s = 0;
        if (eo_nat_to_size(row->arg_v[0], &nested_s) && nested_s <= OP0_ELIM) {
            return eo_exec_op0(loc_a, (int)nested_s, row->arg_s - 1, row->arg_v + 1);
        }
        return eo_tank(loc_a, row->arg_v[0], "bad primitive tag");
    }
    size_t op_s = 0;
    return eo_nat_to_size(tag_v, &op_s) && op_s <= OP0_ELIM
               ? eo_exec_op0(loc_a, (int)op_s, arg_s, arg_v)
               : eo_tank(loc_a, tag_v, "bad primitive tag");
}
