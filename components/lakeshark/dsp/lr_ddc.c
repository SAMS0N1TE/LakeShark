#include "lr_ddc.h"
#include <math.h>
#include <string.h>
#include "ls_cold.h"

#define PI_D 3.14159265358979323846
/* Channel filter design target. The Kaiser estimate lands within a few dB,
   so 70 keeps the 50 dB alias requirement with margin to spare. */
#define ATTEN_DB 70.0
#define FPASS 0.16 /* channel filter flat edge, fraction of out_hz */
#define FSTOP 0.50 /* channel filter stop edge: the output Nyquist */
#define NCO_SPAN 32u  /* oscillator rebuilt from the exact phase at every 32nd input */
#define DC_Q 14       /* DC estimate fraction bits */
#define DC_SHIFT 12   /* one-pole k = 2^-12: 19 Hz at 500k, 39 Hz at 1M */
_Static_assert(LR_DDC_CIC_N == 5, "the fronts unroll five CIC stages");
_Static_assert(NCO_SPAN % 4 == 0 && (NCO_SPAN & (NCO_SPAN - 1)) == 0,
               "re-anchor points fall on 4-sample group starts");

#define INLINE static inline __attribute__((always_inline))

/* Explicit fused multiply-add where the target has one, so the same sum is
   rounded the same way wherever it is computed (block-cut independence).
   Elsewhere a plain multiply and add, which this file never contracts. */
#ifdef __FP_FAST_FMAF
#define FMA(a, b, c) __builtin_fmaf((a), (b), (c))
#else
#define FMA(a, b, c) ((a) * (b) + (c))
#endif

/* Mixing modes: offset +in/4, -in/4, oscillator, oscillator plus DC block. */
enum { M_QP, M_QM, M_NCO, M_NDC };

LS_COLD static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 64 && term > 1e-12 * sum; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

static float bessel_i0f(float x)
{
    const float q = 0.25f * x * x;
    float sum = 1, term = 1;
    for (int k = 1; k < 48; ++k) {
        term *= q / (float)(k * k);
        sum += term;
        if (term < 1e-9f * sum) break;
    }
    return sum;
}

/* CIC magnitude at f (fraction of the input rate). */
static double cic_gain(double f, unsigned r)
{
    if (r < 2 || f == 0) return 1;
    return pow(fabs(sin(PI_D * f * r) / (r * sin(PI_D * f))), LR_DDC_CIC_N);
}

static uint64_t gcd64(uint64_t a, uint64_t b)
{
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

/* e^(j*2*pi*ph/2^32) without libm: fold to +-45 degrees, then Taylor. Error
   about 3e-8, below float resolution. Runs once per NCO_SPAN samples. */
INLINE void expj(uint32_t ph, float *re, float *im)
{
    uint32_t q = (ph + 0x20000000u) >> 30;
    float a = (float)(int32_t)(ph - (q << 30)) * (float)(2 * PI_D / 4294967296.0);
    float a2 = a * a;
    float c = 1 + a2 * (-1.f/2 + a2 * (1.f/24 + a2 * (-1.f/720 + a2 * (1.f/40320))));
    float s = a * (1 + a2 * (-1.f/6 + a2 * (1.f/120 + a2 * (-1.f/5040 + a2 * (1.f/362880)))));
    switch (q & 3) {
    case 0: *re = c; *im = s; break;
    case 1: *re = -s; *im = c; break;
    case 2: *re = -c; *im = -s; break;
    default: *re = s; *im = -c; break;
    }
}

/* Kaiser lowpass at the CIC output rate, in CIC-sample units tau. Single
   precision: the taps are stored as floats anyway, and the P4 has no double
   FPU, so this keeps init to a few ms with one row per step phase. */
static float proto(float tau, float fc2, float inv_half, float beta, float inv_i0b)
{
    const float x = tau * inv_half, r = 1 - x * x;
    if (r < 0) return 0;
    const float a = (float)PI_D * fc2 * tau;
    const float sc = a == 0 ? 1.f : sinf(a) / a;
    return fc2 * sc * bessel_i0f(beta * sqrtf(r)) * inv_i0b;
}

/* Channel filter at the CIC output rate fm. Row r is the prototype delayed
   by r/step_den of a CIC sample (exact: every phase the rational step can
   land on), or by r/PHASES when that table would not fit (blended). Taps are
   oldest first, after `pad` zeros and followed by zeros up to `stride`, so
   the filter loops run whole groups of four and two outputs can share one
   pass over the data. Each row has unity DC gain times the CIC's 1/R^5. The
   prototype is convolved with [-a, 1+2a, -a], a 3-tap lift sized to cancel
   the CIC droop across the passband. */
LS_COLD static bool design_fir(lr_ddc_t *s, double fin, double fm)
{
    double tw = (FSTOP - FPASS) * s->out_hz / fm;
    unsigned core = (unsigned)ceil((ATTEN_DB - 8) / (2.285 * 2 * PI_D * tw)) + 1;
    unsigned nt = core + 2;
    if (nt > LR_DDC_NT_MAX || s->step_int > 3) return false;
    const unsigned pad = s->step_int + 1; /* the furthest a second output starts */
    const unsigned stride = pad + ((nt + pad + 3) & ~3u);
    const bool exact = (uint64_t)s->step_den * stride <= LR_DDC_FIR_MAX;
    const unsigned rows = exact ? s->step_den : LR_DDC_PHASES + 1;
    if (rows * stride > LR_DDC_FIR_MAX) return false;
    s->nt = (uint8_t)nt; s->pad = (uint8_t)pad;
    s->stride = (uint16_t)stride; s->rows = (uint16_t)rows; s->exact = exact;
    memset(s->fir, 0, sizeof(s->fir));
    const double fc = 0.5 * (FPASS + FSTOP) * s->out_hz / fm;
    const double beta = 0.1102 * (ATTEN_DB - 8.7), half = core / 2.0;
    const double f0 = 0.75 * FPASS * s->out_hz;
    const double lift = 2 * (1 - cos(2 * PI_D * f0 / fm));
    const double a = (1 / cic_gain(f0 / fin, s->decim) - 1) / lift;
    const float fc2 = (float)(2 * fc), inv_half = (float)(1 / half), fb = (float)beta;
    const float inv_i0b = (float)(1 / bessel_i0(beta));
    const float ka = (float)a, k1 = (float)(1 + 2 * a);
    const double phases = exact ? (double)rows : LR_DDC_PHASES;
    for (unsigned r = 0; r < rows; ++r) {
        /* p[k] is the prototype at tap k-1's instant: tap j sees p[j+1], its
           neighbours one CIC sample either side are p[j] and p[j+2]. */
        float p[LR_DDC_NT_MAX + 2], tmp[LR_DDC_NT_MAX];
        const double delta = r / phases;
        for (unsigned k = 0; k < nt + 2; ++k)
            p[k] = proto((float)(delta + (double)nt - k - nt / 2.0), fc2, inv_half, fb, inv_i0b);
        double sum = 0;
        for (unsigned j = 0; j < nt; ++j) {
            tmp[j] = k1 * p[j + 1] - ka * (p[j] + p[j + 2]);
            sum += tmp[j];
        }
        float *row = s->fir + r * stride + pad;
        const float scale = (float)(s->cic_gain / sum);
        for (unsigned j = 0; j < nt; ++j) row[j] = tmp[j] * scale;
    }
    return true;
}

static bool offset_ok(const lr_ddc_t *s, int32_t hz)
{
    /* The channel's +-out/2 must sit inside the input's +-Fin/2. */
    uint64_t mag = (uint64_t)(hz < 0 ? -(int64_t)hz : hz);
    return (2 * mag + s->out_hz) * s->in_den <= s->in_num;
}

/* Sign-and-swap mixing for +-in/4, when it continues the oscillator's phase
   exactly: the phase is a whole number of quarter turns from the absolute
   sample count. Always true from init or reset; a retune into in/4 from
   another offset mid-stream keeps the oscillator, so the phase never jumps. */
static void pick_path(lr_ddc_t *s)
{
    const uint32_t st = s->nco_step;
    s->quarter = 0;
    if (!s->dc_on && (st == 0xC0000000u || st == 0x40000000u) && s->nco_phase == s->n_abs * st)
        s->quarter = st == 0xC0000000u ? 1 : -1;
}

LS_COLD bool lr_ddc_set_offset(lr_ddc_t *s, int32_t hz)
{
    if (!s || !s->in_num || !offset_ok(s, hz)) return false;
    /* Mix by e^(-j*2*pi*offset*t): the channel moves down to 0 Hz. */
    double turns = -(double)hz * s->in_den / s->in_num;
    s->offset_hz = hz;
    s->nco_step = (uint32_t)(int64_t)llround(turns * 4294967296.0);
    expj(s->nco_step, &s->nco_wr, &s->nco_wi);
    /* DC lands at -offset. Past out it is deep in the stopband; nearer, the
       input DC blocker takes it out first. */
    s->dc_on = (uint64_t)(hz < 0 ? -(int64_t)hz : hz) < s->out_hz;
    expj(s->nco_phase, &s->nco_zr, &s->nco_zi); /* phase unchanged */
    pick_path(s);
    return true;
}

LS_COLD void lr_ddc_reset(lr_ddc_t *s)
{
    memset(&s->nco_phase, 0, sizeof(*s) - offsetof(lr_ddc_t, nco_phase));
    s->cic_left = s->decim;
    pick_path(s);
}

LS_COLD bool lr_ddc_init(lr_ddc_t *s, uint32_t in_hz, int32_t offset_hz, uint32_t out_hz)
{
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    if (!in_hz || !out_hz) return false;
    /* 32 MHz / 48: the driver rounds it either way. Exact keeps the output
       count from drifting by 0.5 ppm. */
    if (in_hz == 666666u || in_hz == 666667u) { s->in_num = 2000000u; s->in_den = 3; }
    else { s->in_num = in_hz; s->in_den = 1; }
    s->out_hz = out_hz;
    const uint64_t two_out = 2ull * out_hz * s->in_den;
    /* R <= 7 keeps the CIC's growth (R^5) inside 32 bits for any input. */
    if (s->in_num < two_out || s->in_num >= 8 * two_out) goto fail;
    s->decim = (uint32_t)(s->in_num / two_out);
    s->cic_gain = (float)(1.0 / pow(s->decim, LR_DDC_CIC_N));
    /* Final step Fm/out as an exact fraction: no drift over any run. */
    uint64_t num = s->in_num, den = (uint64_t)s->in_den * out_hz * s->decim;
    uint64_t g = gcd64(num, den);
    num /= g; den /= g;
    if (den > UINT32_MAX) goto fail;
    s->step_int = (uint32_t)(num / den);
    s->step_num = (uint32_t)(num % den);
    s->step_den = (uint32_t)den;
    const double fin = (double)s->in_num / s->in_den;
    if (!design_fir(s, fin, fin / s->decim)) goto fail;
    if (!lr_ddc_set_offset(s, offset_hz)) goto fail;
    lr_ddc_reset(s);
    return true;
fail:
    memset(s, 0, sizeof(*s));
    return false;
}

uint32_t lr_ddc_out_hz(const lr_ddc_t *s) { return s->out_hz; }

size_t lr_ddc_out_max(const lr_ddc_t *s, size_t in_pairs)
{
    if (!s->in_num) return 0;
    return (size_t)((uint64_t)in_pairs * s->out_hz * s->in_den / s->in_num) + 2;
}

/* ---- Per-sample pieces shared by the scalar steps and the unrolled loops,
   so both round identically. ---- */

INLINE void dc_step(int32_t *di, int32_t *dq, int32_t *ei, int32_t *eq)
{
    *ei -= *di >> DC_Q; *eq -= *dq >> DC_Q;
    *di += *ei * (1 << (DC_Q - DC_SHIFT));
    *dq += *eq * (1 << (DC_Q - DC_SHIFT));
}

/* x * z, then z *= w. */
INLINE void nco_mix(float xi, float xq, float *zr, float *zi, float wr, float wi,
                    float *mi, float *mq)
{
    const float r = *zr, i = *zi;
    *mi = FMA(xi, r, -(xq * i));
    *mq = FMA(xi, i, xq * r);
    *zr = FMA(r, wr, -(i * wi));
    *zi = FMA(r, wi, i * wr);
}

/* ---- Front for R = 2 ----
   CIC5 /2 is the 6-tap FIR [1 5 10 10 5 1] at every odd absolute sample n:
   y = a + 5(b + 2c) with a = x(n)+x(n-5), b = x(n-1)+x(n-4), c = x(n-2)+x(n-3).
   No integrators, so it can run in float: with the in/4 mix every value is
   an integer below 2^21 and exact; with the oscillator it is an FIR, nothing
   to drift. The unrolled loops take groups of four inputs starting at
   n = 0 mod 4 (two outputs each); a few scalar steps align the ends. */

INLINE float cic6(float a, float b, float c) { return FMA(5.f, FMA(2.f, c, b), a); }

/* Mix input sample s->n_abs. */
INLINE void mix_one(lr_ddc_t *s, const int16_t *in, float *mi, float *mq, const int mode)
{
    if (mode == M_QP || mode == M_QM) {
        const float sg = mode == M_QP ? 1.f : -1.f, I = in[0], Q = in[1];
        switch (s->n_abs & 3) {
        case 0: *mi = I; *mq = Q; break;
        case 1: *mi = sg * Q; *mq = -sg * I; break;
        case 2: *mi = -I; *mq = -Q; break;
        default: *mi = -sg * Q; *mq = sg * I; break;
        }
    } else {
        int32_t ei = in[0], eq = in[1];
        if (mode == M_NDC) dc_step(&s->dc_i, &s->dc_q, &ei, &eq);
        if (!(s->n_abs & (NCO_SPAN - 1))) expj(s->nco_phase, &s->nco_zr, &s->nco_zi);
        nco_mix((float)ei, (float)eq, &s->nco_zr, &s->nco_zi, s->nco_wr, s->nco_wi, mi, mq);
    }
    s->nco_phase += s->nco_step;
}

/* One mixed sample into the R = 2 CIC; an output at odd n. */
INLINE void r2_take(lr_ddc_t *s, float mi, float mq, lr_ddc_cf **dst)
{
    float *hi = s->hist[0], *hq = s->hist[1];
    if (s->n_abs & 1) {
        (*dst)->i = cic6(mi + hi[4], hi[0] + hi[3], hi[1] + hi[2]);
        (*dst)->q = cic6(mq + hq[4], hq[0] + hq[3], hq[1] + hq[2]);
        ++*dst;
    }
    hi[4] = hi[3]; hi[3] = hi[2]; hi[2] = hi[1]; hi[1] = hi[0]; hi[0] = mi;
    hq[4] = hq[3]; hq[3] = hq[2]; hq[2] = hq[1]; hq[1] = hq[0]; hq[0] = mq;
    ++s->n_abs;
}

/* In/4 mix, sg = +1 for offset +in/4 (multiply by (-j)^n), -1 for -in/4.
   Mixed sample n = 0,1,2,3 mod 4 is (I,Q), (sg Q,-sg I), (-I,-Q), (-sg Q,sg I).
   Raw values of the previous group (P) and this one (C) stay in registers;
   the two sets swap roles each group, so nothing is copied. */
#define QLOAD(C) do { \
    C##I0 = in[0]; C##Q0 = in[1]; C##I1 = in[2]; C##Q1 = in[3]; \
    C##I2 = in[4]; C##Q2 = in[5]; C##I3 = in[6]; C##Q3 = in[7]; in += 8; } while (0)
#define QOUT(P, C) do { \
    d[0].i = cic6(sg * C##Q1 + P##I0, C##I0 + sg * P##Q1, -sg * P##Q3 - P##I2); \
    d[0].q = cic6(P##Q0 - sg * C##I1, C##Q0 - sg * P##I1, sg * P##I3 - P##Q2); \
    d[1].i = cic6(-sg * C##Q3 - P##I2, -C##I2 - sg * P##Q3, sg * C##Q1 + C##I0); \
    d[1].q = cic6(sg * C##I3 - P##Q2, sg * P##I3 - C##Q2, C##Q0 - sg * C##I1); \
    d += 2; } while (0)
#define QSAVE(P, C) do { \
    hi[0] = -sg * C##Q3; hq[0] = sg * C##I3; hi[1] = -C##I2;     hq[1] = -C##Q2; \
    hi[2] = sg * C##Q1;  hq[2] = -sg * C##I1; hi[3] = C##I0;     hq[3] = C##Q0; \
    hi[4] = -sg * P##Q3; hq[4] = sg * P##I3; } while (0)

INLINE lr_ddc_cf *k_quarter(lr_ddc_t *s, const int16_t *in, unsigned groups,
                            lr_ddc_cf *d, const float sg)
{
    float *hi = s->hist[0], *hq = s->hist[1];
    /* history (newest first) back to raw: x(n-1) is a 3 mod 4 sample, etc. */
    float AI0 = hi[3], AQ0 = hq[3], AQ1 = sg * hi[2], AI1 = -sg * hq[2];
    float AI2 = -hi[1], AQ2 = -hq[1], AQ3 = -sg * hi[0], AI3 = sg * hq[0];
    float BI0 = 0, BQ0 = 0, BI1 = 0, BQ1 = 0, BI2 = 0, BQ2 = 0, BI3 = 0, BQ3 = 0;
    const unsigned total = groups;
    for (; groups >= 2; groups -= 2) {
        QLOAD(B); QOUT(A, B);
        QLOAD(A); QOUT(B, A);
    }
    if (groups) { QLOAD(B); QOUT(A, B); QSAVE(A, B); }
    else QSAVE(B, A);
    s->n_abs += 4 * total;
    s->nco_phase += 4 * total * s->nco_step;
    return d;
}

/* Oscillator mix. History is carried mixed: G0..G3 = x(n0-1)..x(n0-4). */
#define NSAMPLE(k, oi, oq) do { \
    int32_t ei = in[2 * (k)], eq = in[2 * (k) + 1]; \
    if (dc) dc_step(&di, &dq, &ei, &eq); \
    nco_mix((float)ei, (float)eq, &zr, &zi, wr, wi, &oi, &oq); } while (0)
#define NGROUP(G, N) do { \
    if (!(n & (NCO_SPAN - 1))) expj(ph, &zr, &zi); \
    float m0i, m0q, m1i, m1q, m2i, m2q, m3i, m3q; \
    NSAMPLE(0, m0i, m0q); NSAMPLE(1, m1i, m1q); \
    NSAMPLE(2, m2i, m2q); NSAMPLE(3, m3i, m3q); \
    d[0].i = cic6(m1i + G##3i, m0i + G##2i, G##0i + G##1i); \
    d[0].q = cic6(m1q + G##3q, m0q + G##2q, G##0q + G##1q); \
    d[1].i = cic6(m3i + G##1i, m2i + G##0i, m1i + m0i); \
    d[1].q = cic6(m3q + G##1q, m2q + G##0q, m1q + m0q); \
    N##0i = m3i; N##0q = m3q; N##1i = m2i; N##1q = m2q; \
    N##2i = m1i; N##2q = m1q; N##3i = m0i; N##3q = m0q; \
    d += 2; in += 8; n += 4; ph += step4; } while (0)
#define NSAVE(P, C) do { \
    hi[0] = C##0i; hq[0] = C##0q; hi[1] = C##1i; hq[1] = C##1q; \
    hi[2] = C##2i; hq[2] = C##2q; hi[3] = C##3i; hq[3] = C##3q; \
    hi[4] = P##0i; hq[4] = P##0q; } while (0)

INLINE lr_ddc_cf *k_nco(lr_ddc_t *s, const int16_t *in, unsigned groups,
                        lr_ddc_cf *d, const bool dc)
{
    float *hi = s->hist[0], *hq = s->hist[1];
    float A0i = hi[0], A0q = hq[0], A1i = hi[1], A1q = hq[1];
    float A2i = hi[2], A2q = hq[2], A3i = hi[3], A3q = hq[3];
    float B0i = 0, B0q = 0, B1i = 0, B1q = 0, B2i = 0, B2q = 0, B3i = 0, B3q = 0;
    float zr = s->nco_zr, zi = s->nco_zi;
    const float wr = s->nco_wr, wi = s->nco_wi;
    const uint32_t step4 = 4 * s->nco_step;
    uint32_t ph = s->nco_phase, n = s->n_abs;
    int32_t di = s->dc_i, dq = s->dc_q;
    for (; groups >= 2; groups -= 2) {
        NGROUP(A, B);
        NGROUP(B, A);
    }
    if (groups) { NGROUP(A, B); NSAVE(A, B); }
    else NSAVE(B, A);
    s->nco_zr = zr; s->nco_zi = zi; s->nco_phase = ph; s->n_abs = n;
    s->dc_i = di; s->dc_q = dq;
    return d;
}

INLINE unsigned front2(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *dst,
                       const int mode)
{
    lr_ddc_cf *d = dst;
    float mi, mq;
    for (; n && (s->n_abs & 3); --n, in += 2) { mix_one(s, in, &mi, &mq, mode); r2_take(s, mi, mq, &d); }
    const unsigned groups = n >> 2;
    if (groups) {
        if (mode == M_QP) d = k_quarter(s, in, groups, d, 1.f);
        else if (mode == M_QM) d = k_quarter(s, in, groups, d, -1.f);
        else d = k_nco(s, in, groups, d, mode == M_NDC);
        in += 8 * groups; n -= 4 * groups;
    }
    for (; n; --n, in += 2) { mix_one(s, in, &mi, &mq, mode); r2_take(s, mi, mq, &d); }
    return (unsigned)(d - dst);
}

/* ---- Front for R > 2: integer CIC5, wraparound arithmetic on purpose (the
   combs undo the wrap; R^5 * 2^16 fits 32 bits for R <= 7). Decimates on the
   absolute sample count, so block cuts do not matter. ---- */
INLINE unsigned front_rec(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *dst,
                          const int mode)
{
    int32_t di = s->dc_i, dq = s->dc_q;
    float zr = s->nco_zr, zi = s->nco_zi;
    const float wr = s->nco_wr, wi = s->nco_wi;
    const uint32_t step = s->nco_step, r = s->decim;
    uint32_t ph = s->nco_phase, nabs = s->n_abs, cl = s->cic_left;
    uint32_t i0 = s->integ[0][0], i1 = s->integ[0][1], i2 = s->integ[0][2],
             i3 = s->integ[0][3], i4 = s->integ[0][4];
    uint32_t q0 = s->integ[1][0], q1 = s->integ[1][1], q2 = s->integ[1][2],
             q3 = s->integ[1][3], q4 = s->integ[1][4];
    unsigned made = 0;
    for (; n; --n, in += 2) {
        int32_t ei = in[0], eq = in[1];
        if (mode == M_QP || mode == M_QM) {
            const int32_t sg = mode == M_QP ? 1 : -1, I = ei, Q = eq;
            switch (nabs & 3) {
            case 0: break;
            case 1: ei = sg * Q; eq = -sg * I; break;
            case 2: ei = -I; eq = -Q; break;
            default: ei = -sg * Q; eq = sg * I; break;
            }
        } else {
            if (mode == M_NDC) dc_step(&di, &dq, &ei, &eq);
            if (!(nabs & (NCO_SPAN - 1))) expj(ph, &zr, &zi);
            float mi, mq;
            nco_mix((float)ei, (float)eq, &zr, &zi, wr, wi, &mi, &mq);
            ei = (int32_t)mi; eq = (int32_t)mq;
        }
        ph += step; ++nabs;
        i0 += (uint32_t)ei; i1 += i0; i2 += i1; i3 += i2; i4 += i3;
        q0 += (uint32_t)eq; q1 += q0; q2 += q1; q3 += q2; q4 += q3;
        if (!--cl) {
            cl = r;
            uint32_t a = i4, b = q4;
            for (unsigned k = 0; k < LR_DDC_CIC_N; ++k) {
                uint32_t ta = a - s->comb[0][k], tb = b - s->comb[1][k];
                s->comb[0][k] = a; s->comb[1][k] = b;
                a = ta; b = tb;
            }
            dst[made].i = (float)(int32_t)a;
            dst[made].q = (float)(int32_t)b;
            ++made;
        }
    }
    s->integ[0][0] = i0; s->integ[0][1] = i1; s->integ[0][2] = i2;
    s->integ[0][3] = i3; s->integ[0][4] = i4;
    s->integ[1][0] = q0; s->integ[1][1] = q1; s->integ[1][2] = q2;
    s->integ[1][3] = q3; s->integ[1][4] = q4;
    s->dc_i = di; s->dc_q = dq; s->nco_zr = zr; s->nco_zi = zi;
    s->nco_phase = ph; s->n_abs = nabs; s->cic_left = cl;
    return made;
}

/* One out-of-line instance per mode keeps each loop's registers its own. */
#define NOINLINE static __attribute__((noinline))
typedef unsigned (*front_fn)(lr_ddc_t *, const int16_t *, unsigned, lr_ddc_cf *);
NOINLINE unsigned f2_qp(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front2(s, in, n, d, M_QP); }
NOINLINE unsigned f2_qm(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front2(s, in, n, d, M_QM); }
NOINLINE unsigned f2_nco(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front2(s, in, n, d, M_NCO); }
NOINLINE unsigned f2_ndc(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front2(s, in, n, d, M_NDC); }
NOINLINE unsigned fr_qp(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front_rec(s, in, n, d, M_QP); }
NOINLINE unsigned fr_qm(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front_rec(s, in, n, d, M_QM); }
NOINLINE unsigned fr_nco(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front_rec(s, in, n, d, M_NCO); }
NOINLINE unsigned fr_ndc(lr_ddc_t *s, const int16_t *in, unsigned n, lr_ddc_cf *d) { return front_rec(s, in, n, d, M_NDC); }

static front_fn pick_front(const lr_ddc_t *s)
{
    if (s->decim == 2) {
        if (s->quarter) return s->quarter > 0 ? f2_qp : f2_qm;
        return s->dc_on ? f2_ndc : f2_nco;
    }
    if (s->quarter) return s->quarter > 0 ? fr_qp : fr_qm;
    return s->dc_on ? fr_ndc : fr_nco;
}

/* ---- Channel filter and rational step ---- */

INLINE int16_t sat16(float v)
{
#if defined(__riscv) && defined(__riscv_flen)
    int32_t r; /* clamp, then round half away from zero: no branches */
    v = __builtin_fminf(__builtin_fmaxf(v, -32768.f), 32767.f);
    __asm__("fcvt.w.s %0, %1, rmm" : "=r"(r) : "f"(v));
    return (int16_t)r;
#else
    if (v >= 32767.f) return 32767;
    if (v <= -32768.f) return -32768;
    return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
#endif
}

/* Two outputs from one pass: c0 from w[0], c1 from w[d] (its row pointer is
   already moved back by d; the row's leading zeros cover the gap). Each
   sample load feeds four multiply-adds. One accumulator per rail and output
   in tap order, as dot1, so an output is bit-identical whichever ran it:
   the padding only adds exact zeros. */
#define TAP2(k) do { \
    const float xi = w[k].i, xq = w[k].q, a = c0[k], b = c1[k]; \
    i0 = FMA(a, xi, i0); q0 = FMA(a, xq, q0); \
    i1 = FMA(b, xi, i1); q1 = FMA(b, xq, q1); } while (0)
INLINE void dot2(const lr_ddc_cf *w, const float *c0, const float *c1, unsigned quads,
                 float r[4])
{
    float i0 = 0, q0 = 0, i1 = 0, q1 = 0;
    const lr_ddc_cf *const end = w + 4 * quads;
    do {
        TAP2(0); TAP2(1); TAP2(2); TAP2(3);
        w += 4; c0 += 4; c1 += 4;
    } while (w != end);
    r[0] = i0; r[1] = q0; r[2] = i1; r[3] = q1;
}

#define TAP1(k) do { \
    const float a = c0[k]; \
    i0 = FMA(a, w[k].i, i0); q0 = FMA(a, w[k].q, q0); } while (0)
INLINE void dot1(const lr_ddc_cf *w, const float *c0, unsigned quads, float r[2])
{
    float i0 = 0, q0 = 0;
    const lr_ddc_cf *const end = w + 4 * quads;
    do {
        TAP1(0); TAP1(1); TAP1(2); TAP1(3);
        w += 4; c0 += 4;
    } while (w != end);
    r[0] = i0; r[1] = q0;
}

/* fpos is the next output's newest input, relative to this pass's first new
   sample; facc/step_den is how far the output instant sits past it, which
   picks the row (exact) or two rows to blend (fallback). The window may read
   up to three samples past the new data: always finite (zeroed at reset,
   stale after), and only ever against zero taps. */
NOINLINE size_t fin_run(lr_ddc_t *s, unsigned n, int16_t *out, size_t cap)
{
    const unsigned nt = s->nt, stride = s->stride, pad = s->pad;
    const uint32_t si = s->step_int, sn = s->step_num, sd = s->step_den;
    uint32_t j = s->fpos, acc = s->facc;
    size_t made = 0;
#define EMIT(vi, vq) do { \
    if (made < cap) { out[2 * made] = sat16(vi); out[2 * made + 1] = sat16(vq); ++made; } \
    else ++s->dropped; } while (0)
    if (s->exact) {
        const float *const fir = s->fir + pad;
        const unsigned quads1 = (nt + 3) >> 2;
        while (j < n) {
            uint32_t j1 = j + si, a1 = acc + sn;
            if (a1 >= sd) { a1 -= sd; ++j1; }
            const lr_ddc_cf *w = s->fin_buf + j;
            const float *c0 = fir + acc * stride;
            if (j1 < n) {
                const unsigned dd = j1 - j;
                float r[4];
                dot2(w, c0, fir + a1 * stride - dd, (nt + dd + 3) >> 2, r);
                EMIT(r[0], r[1]);
                EMIT(r[2], r[3]);
                j = j1 + si; acc = a1 + sn;
                if (acc >= sd) { acc -= sd; ++j; }
            } else {
                float r[2];
                dot1(w, c0, quads1, r);
                EMIT(r[0], r[1]);
                j = j1; acc = a1;
            }
        }
    } else {
        const float scale = (float)LR_DDC_PHASES / (float)sd;
        while (j < n) {
            const lr_ddc_cf *w = s->fin_buf + j;
            const float x = (float)acc * scale;
            unsigned p = (unsigned)x;
            if (p >= LR_DDC_PHASES) p = LR_DDC_PHASES - 1;
            const float fr = x - (float)p;
            const float *c = s->fir + p * stride + pad, *e = c + stride;
            float i0 = 0, q0 = 0;
            for (unsigned t = 0; t < nt; ++t) {
                const float k = FMA(fr, e[t] - c[t], c[t]);
                i0 = FMA(k, w[t].i, i0); q0 = FMA(k, w[t].q, q0);
            }
            EMIT(i0, q0);
            j += si; acc += sn;
            if (acc >= sd) { acc -= sd; ++j; }
        }
    }
#undef EMIT
    s->fpos = j - n; s->facc = acc;
    if (n) memmove(s->fin_buf, s->fin_buf + n, (nt - 1) * sizeof(*s->fin_buf));
    return made;
}

size_t lr_ddc_process(lr_ddc_t *s, const int16_t *in, size_t pairs,
                      int16_t *out, size_t cap)
{
    if (!s || !s->in_num || !in) return 0;
    size_t made = 0;
    lr_ddc_cf *const fin_new = s->fin_buf + s->nt - 1;
    const front_fn front = pick_front(s);
    while (pairs) {
        unsigned n = pairs < LR_DDC_CHUNK ? (unsigned)pairs : LR_DDC_CHUNK;
        unsigned m = front(s, in, n, fin_new);
        in += 2 * n; pairs -= n;
        made += fin_run(s, m, out ? out + 2 * made : NULL, out ? cap - made : 0);
    }
    return made;
}
