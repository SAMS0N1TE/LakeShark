/* Mixed-radix decimation-in-time FFT (Cooley-Tukey, any factorization). */
#include "digi_fft.h"
#include <math.h>
#include <string.h>

static inline digi_cpx cmul(digi_cpx a, digi_cpx b)
{
    return (digi_cpx){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

bool digi_fft_init(digi_fft_t *f, int n)
{
    memset(f, 0, sizeof(*f));
    if (n < 2) return false;
    f->n = n;
    int rem = n, k = 0;
    static const int pref[] = {4, 2, 3, 5};
    for (int i = 0; i < 4 && rem > 1; ++i)
        while (rem % pref[i] == 0 && k < DIGI_FFT_MAX_FACTORS) {
            rem /= pref[i];
            f->fac[2 * k] = pref[i]; f->fac[2 * k + 1] = rem; ++k;
        }
    for (int p = 7; rem > 1 && p <= 16 && k < DIGI_FFT_MAX_FACTORS; p += 2)
        while (rem % p == 0 && k < DIGI_FFT_MAX_FACTORS) {
            rem /= p;
            f->fac[2 * k] = p; f->fac[2 * k + 1] = rem; ++k;
        }
    if (rem != 1) return false;
    f->tw = digi_alloc(sizeof(digi_cpx) * (size_t)n);
    if (!f->tw) return false;
    for (int i = 0; i < n; ++i) {
        double a = -6.283185307179586 * (double)i / (double)n;
        f->tw[i] = (digi_cpx){(float)cos(a), (float)sin(a)};
    }
    return true;
}

void digi_fft_free(digi_fft_t *f)
{
    if (f->tw) digi_free(f->tw);
    memset(f, 0, sizeof(*f));
}

/* Combine p sub-transforms of length m held at out[q*m .. q*m+m-1]. */
static void butterfly(const digi_fft_t *f, digi_cpx *out, int fstride, int p, int m)
{
    const digi_cpx *tw = f->tw;
    if (p == 2) {
        for (int k = 0; k < m; ++k) {
            digi_cpx a = out[k], b = cmul(out[k + m], tw[k * fstride]);
            out[k] = (digi_cpx){a.re + b.re, a.im + b.im};
            out[k + m] = (digi_cpx){a.re - b.re, a.im - b.im};
        }
        return;
    }
    if (p == 4) {
        for (int k = 0; k < m; ++k) {
            digi_cpx t0 = out[k];
            digi_cpx t1 = cmul(out[k + m], tw[k * fstride]);
            digi_cpx t2 = cmul(out[k + 2 * m], tw[2 * k * fstride]);
            digi_cpx t3 = cmul(out[k + 3 * m], tw[3 * k * fstride]);
            digi_cpx s02 = {t0.re + t2.re, t0.im + t2.im}, d02 = {t0.re - t2.re, t0.im - t2.im};
            digi_cpx s13 = {t1.re + t3.re, t1.im + t3.im}, d13 = {t1.re - t3.re, t1.im - t3.im};
            out[k]         = (digi_cpx){s02.re + s13.re, s02.im + s13.im};
            out[k + 2 * m] = (digi_cpx){s02.re - s13.re, s02.im - s13.im};
            /* -i * d13 = (d13.im, -d13.re) */
            out[k + m]     = (digi_cpx){d02.re + d13.im, d02.im - d13.re};
            out[k + 3 * m] = (digi_cpx){d02.re - d13.im, d02.im + d13.re};
        }
        return;
    }
    /* Generic radix: a direct p-point DFT per output column. */
    digi_cpx t[16];
    const int n = f->n;
    for (int k = 0; k < m; ++k) {
        for (int q = 0; q < p; ++q) t[q] = cmul(out[k + q * m], tw[(q * k * fstride) % n]);
        for (int u = 0; u < p; ++u) {
            digi_cpx acc = t[0];
            for (int q = 1; q < p; ++q) {
                digi_cpx w = tw[(((u * q) % p) * fstride * m) % n];
                digi_cpx v = cmul(t[q], w);
                acc.re += v.re; acc.im += v.im;
            }
            out[k + u * m] = acc;
        }
    }
}

static void work(const digi_fft_t *f, digi_cpx *out, const digi_cpx *in,
                 int fstride, const int *fac)
{
    const int p = fac[0], m = fac[1];
    if (m == 1) {
        for (int q = 0; q < p; ++q) out[q] = in[q * fstride];
    } else {
        for (int q = 0; q < p; ++q)
            work(f, out + q * m, in + q * fstride, fstride * p, fac + 2);
    }
    butterfly(f, out, fstride, p, m);
}

void digi_fft_run(const digi_fft_t *f, const digi_cpx *in, digi_cpx *out)
{
    if (!f || !f->tw) return;
    work(f, out, in, 1, f->fac);
}
