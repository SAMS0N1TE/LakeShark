/* ge_scalarmult_base with its table built at run time.

   upstream/ed25519/ge.c multiplies by the base point B with a 30 KB constant
   table, base[i][j] = (j+1) * 256^i * B in (y+x, y-x, 2dxy) form. That table
   is 30 KB of the app image. Here the same table is computed once, the first
   time a key is derived or a message signed, into PSRAM (a few milliseconds),
   and the scalar multiplication below is ref10's (public domain, SUPERCOP
   crypto_sign/ed25519/ref10) unchanged: constant time, the same cmov walk.
   ge.c is built with its own ge_scalarmult_base renamed (CMakeLists.txt), so
   nothing references its table and the linker drops it.

   bench/tests/test_ed25519_base.c checks the computed table against
   upstream's, entry by entry, and RFC 8032 keys and signatures. */
#include "ls_ed25519_base.h"

#include <stdatomic.h>
#include <string.h>

#include "fe.h"
#include "ge.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define LS_ED_EXT EXT_RAM_BSS_ATTR
static void wait_turn(void) { vTaskDelay(1); }
#else
#define LS_ED_EXT
static void wait_turn(void) {}
#endif

static LS_ED_EXT ge_precomp s_base[32][8];
static atomic_int s_state;  /* 0 not built, 1 building, 2 ready */

/* The build's working set lives here, not on the caller's stack: the first
   signature can come from any task, and this keeps that first call within
   the stack the old constant-table code used. Only the task that wins
   s_state touches it. */
static LS_ED_EXT struct {
    fe prefix[8];
    fe d2, inv, zinv, x, y, t0, t1;
    unsigned char s[32];
    ge_p3 p, acc;
    ge_cached pc;
    ge_p1p1 r;
} w;

/* One representation per field element: the carried limbs fe_frombytes
   gives for the canonical encoding. */
static void canon(fe f)
{
    fe_tobytes(w.s, f);
    fe_frombytes(f, w.s);
}

static void small(fe f, int32_t v)
{
    fe_0(f);
    f[0] = v;
}

/* d2 = 2d, d = -121665/121666. */
static void make_d2(void)
{
    small(w.t0, 121665);
    small(w.t1, 121666);
    fe_invert(w.t1, w.t1);
    fe_mul(w.d2, w.t0, w.t1);
    fe_neg(w.d2, w.d2);
    fe_add(w.d2, w.d2, w.d2);
    canon(w.d2);
}

/* B = (x, 4/5) with x even. ge_frombytes_negate_vartime returns -B. */
static void make_b(void)
{
    small(w.t0, 4);
    small(w.t1, 5);
    fe_invert(w.t1, w.t1);
    fe_mul(w.t0, w.t0, w.t1);
    fe_tobytes(w.s, w.t0);
    ge_frombytes_negate_vartime(&w.p, w.s);
    fe_neg(w.p.X, w.p.X);
    fe_neg(w.p.T, w.p.T);
}

static void build(void)
{
    make_d2();
    make_b();
    for (int i = 0; i < 32; ++i) {
        /* Row i: k*P for k = 1..8, P = 256^i B, kept projective (X, Y, Z)
           in the row's own slots until one inversion converts all eight. */
        ge_p3_to_cached(&w.pc, &w.p);
        w.acc = w.p;
        for (int k = 0; k < 8; ++k) {
            ge_precomp *e = &s_base[i][k];
            fe_copy(e->yplusx, w.acc.X);
            fe_copy(e->yminusx, w.acc.Y);
            fe_copy(e->xy2d, w.acc.Z);
            if (k < 7) {
                ge_add(&w.r, &w.acc, &w.pc);
                ge_p1p1_to_p3(&w.acc, &w.r);
            }
        }
        /* Montgomery's trick: prefix products of the Zs, one inversion. */
        fe_copy(w.prefix[0], s_base[i][0].xy2d);
        for (int k = 1; k < 8; ++k)
            fe_mul(w.prefix[k], w.prefix[k - 1], s_base[i][k].xy2d);
        fe_invert(w.inv, w.prefix[7]);
        for (int k = 7; k >= 0; --k) {
            ge_precomp *e = &s_base[i][k];
            if (k) {
                fe_mul(w.zinv, w.inv, w.prefix[k - 1]);
                fe_mul(w.inv, w.inv, e->xy2d);
            } else {
                fe_copy(w.zinv, w.inv);
            }
            fe_mul(w.x, e->yplusx, w.zinv);
            fe_mul(w.y, e->yminusx, w.zinv);
            fe_add(e->yplusx, w.y, w.x);
            fe_sub(e->yminusx, w.y, w.x);
            fe_mul(e->xy2d, w.x, w.y);
            fe_mul(e->xy2d, e->xy2d, w.d2);
            canon(e->yplusx);
            canon(e->yminusx);
            canon(e->xy2d);
        }
        /* P = 256 P */
        for (int k = 0; k < 8; ++k) {
            ge_p3_dbl(&w.r, &w.p);
            ge_p1p1_to_p3(&w.p, &w.r);
        }
    }
}

const ge_precomp (*ls_ed25519_base_table(void))[8]
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != 2) {
        int expect = 0;
        if (atomic_compare_exchange_strong(&s_state, &expect, 1)) {
            build();
            atomic_store_explicit(&s_state, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&s_state, memory_order_acquire) != 2)
                wait_turn();
        }
    }
    return (const ge_precomp (*)[8])s_base;
}

/* ref10 from here: select() and ge_scalarmult_base() as in ge.c. */

static unsigned char equal(signed char b, signed char c)
{
    unsigned char ub = b;
    unsigned char uc = c;
    unsigned char x = ub ^ uc;
    uint64_t y = x;
    y -= 1;
    y >>= 63;
    return (unsigned char)y;
}

static unsigned char negative(signed char b)
{
    uint64_t x = b;
    x >>= 63;
    return (unsigned char)x;
}

static void cmov(ge_precomp *t, const ge_precomp *u, unsigned char b)
{
    fe_cmov(t->yplusx, u->yplusx, b);
    fe_cmov(t->yminusx, u->yminusx, b);
    fe_cmov(t->xy2d, u->xy2d, b);
}

static void select_entry(const ge_precomp (*base)[8], ge_precomp *t, int pos, signed char b)
{
    ge_precomp minust;
    unsigned char bnegative = negative(b);
    unsigned char babs = b - (((-bnegative) & b) << 1);
    fe_1(t->yplusx);
    fe_1(t->yminusx);
    fe_0(t->xy2d);
    for (int k = 0; k < 8; ++k)
        cmov(t, &base[pos][k], equal(babs, (signed char)(k + 1)));
    fe_copy(minust.yplusx, t->yminusx);
    fe_copy(minust.yminusx, t->yplusx);
    fe_neg(minust.xy2d, t->xy2d);
    cmov(t, &minust, bnegative);
}

/* h = a * B, a = a[0] + 256 a[1] + ... + 256^31 a[31], a[31] <= 127. */
void ge_scalarmult_base(ge_p3 *h, const unsigned char *a)
{
    const ge_precomp (*base)[8] = ls_ed25519_base_table();
    signed char e[64];
    signed char carry;
    ge_p1p1 r;
    ge_p2 s;
    ge_precomp t;
    int i;

    for (i = 0; i < 32; ++i) {
        e[2 * i + 0] = (a[i] >> 0) & 15;
        e[2 * i + 1] = (a[i] >> 4) & 15;
    }
    carry = 0;
    for (i = 0; i < 63; ++i) {
        e[i] += carry;
        carry = e[i] + 8;
        carry >>= 4;
        e[i] -= carry << 4;
    }
    e[63] += carry;
    ge_p3_0(h);
    for (i = 1; i < 64; i += 2) {
        select_entry(base, &t, i / 2, e[i]);
        ge_madd(&r, h, &t);
        ge_p1p1_to_p3(h, &r);
    }
    ge_p3_dbl(&r, h);
    ge_p1p1_to_p2(&s, &r);
    ge_p2_dbl(&r, &s);
    ge_p1p1_to_p2(&s, &r);
    ge_p2_dbl(&r, &s);
    ge_p1p1_to_p2(&s, &r);
    ge_p2_dbl(&r, &s);
    ge_p1p1_to_p3(h, &r);
    for (i = 0; i < 64; i += 2) {
        select_entry(base, &t, i / 2, e[i]);
        ge_madd(&r, h, &t);
        ge_p1p1_to_p3(h, &r);
    }
}
