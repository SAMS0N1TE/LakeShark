/*LS-748*/
/* Power spectrum over the IQ the receiver is already delivering.
   See spectrum.h for why this exists and why it does not use esp-dsp. */

#include "spectrum.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Working buffers stay in INTERNAL ram: the butterfly is the hot loop and
   PSRAM is materially slower for scattered access. 512 floats x2 = 4 KB. */
static float s_re[SPEC_FFT_N];
static float s_im[SPEC_FFT_N];

/* Read-mostly tables and the accumulator go to PSRAM - they are touched
   linearly and there is 32 MB of it, while internal free is around 50 KB
   after the C6/BLE bring-up. */
static EXT_RAM_BSS_ATTR float s_tw_re[SPEC_FFT_N / 2];
static EXT_RAM_BSS_ATTR float s_tw_im[SPEC_FFT_N / 2];
static EXT_RAM_BSS_ATTR float s_win[SPEC_FFT_N];
static EXT_RAM_BSS_ATTR float s_acc[SPEC_FFT_N];

static int  s_navg  = 0;
static bool s_ready = false;

void spectrum_init(void)
{
    if (s_ready) return;

    for (int k = 0; k < SPEC_FFT_N / 2; k++) {
        double a = -2.0 * M_PI * (double)k / (double)SPEC_FFT_N;
        s_tw_re[k] = (float)cos(a);
        s_tw_im[k] = (float)sin(a);
    }
    /* Hann. A rectangular window smears a strong carrier across the whole
       span in sidelobes, which on a finder reads as signal everywhere. */
    for (int i = 0; i < SPEC_FFT_N; i++)
        s_win[i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * (double)i /
                                            (double)(SPEC_FFT_N - 1));

    memset(s_acc, 0, sizeof(s_acc));
    s_navg  = 0;
    s_ready = true;
}

/* Iterative radix-2 decimation-in-time, twiddles from the table rather than a
   running rotation - the recurrence drifts measurably by 512 points and the
   whole value of this module is that a peak sits where it claims to. */
static void fft512(float *re, float *im)
{
    const int n = SPEC_FFT_N;

    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                int   t  = k * step;
                float cr = s_tw_re[t], ci = s_tw_im[t];
                int   a  = i + k, b = i + k + half;
                float br = re[b] * cr - im[b] * ci;
                float bi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - br; im[b] = im[a] - bi;
                re[a] = re[a] + br; im[a] = im[a] + bi;
            }
        }
    }
}

void spectrum_accum(const uint8_t *iq, int len)
{
    if (!s_ready || !iq) return;

    int nsamp = len / 2;
    if (nsamp < SPEC_FFT_N) return;

    /* u8 IQ from the RTL is offset binary centred on 127.5. */
    for (int i = 0; i < SPEC_FFT_N; i++) {
        float w = s_win[i];
        s_re[i] = ((float)iq[2 * i]     - 127.5f) * (1.0f / 127.5f) * w;
        s_im[i] = ((float)iq[2 * i + 1] - 127.5f) * (1.0f / 127.5f) * w;
    }

    fft512(s_re, s_im);

    for (int i = 0; i < SPEC_FFT_N; i++)
        s_acc[i] += s_re[i] * s_re[i] + s_im[i] * s_im[i];

    s_navg++;
}

void spectrum_reset(void)
{
    if (!s_ready) return;
    memset(s_acc, 0, sizeof(s_acc));
    s_navg = 0;
}

int spectrum_navg(void) { return s_navg; }

bool spectrum_read_db(float *out, int n)
{
    if (!s_ready || !out || n < 1 || s_navg <= 0) return false;

    const int   N     = SPEC_FFT_N;
    const float inv   = 1.0f / (float)s_navg;
    /* Normalise by N so the numbers do not move when SPEC_FFT_N changes, and
       by the Hann coherent gain (0.5) so a full-scale tone reads near 0 dB. */
    const float scale = inv / ((float)N * 0.5f) / ((float)N * 0.5f);

    for (int o = 0; o < n; o++) {
        /* Group o covers this slice of the SHIFTED spectrum. */
        int lo = (int)(((int64_t)o       * N) / n);
        int hi = (int)(((int64_t)(o + 1) * N) / n);
        if (hi <= lo) hi = lo + 1;
        if (hi > N)   hi = N;

        float pk = 0.0f;
        for (int i = lo; i < hi; i++) {
            /* fftshift: shifted index i maps to raw bin i+N/2 (mod N), so
               out[0] is centre-rate/2 and out[n-1] is centre+rate/2. */
            int raw = (i + N / 2) & (N - 1);
            float p = s_acc[raw];
            if (p > pk) pk = p;
        }

        float p = pk * scale;
        if (p < 1e-12f) p = 1e-12f;
        out[o] = 10.0f * log10f(p);      /* negative dBFS */
    }
    return true;
}
