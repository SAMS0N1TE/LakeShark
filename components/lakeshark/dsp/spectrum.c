/**/
/* Power spectrum over the IQ the receiver is already delivering.
   See spectrum.h for why this exists and why it does not use esp-dsp. */

#include "spectrum.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_cold.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Independent callers keep their context in PSRAM. */
static EXT_RAM_BSS_ATTR spectrum_t s_default;

LS_COLD void spectrum_context_init(spectrum_t *s)
{
    if (s->ready) return;

    for (int k = 0; k < SPEC_FFT_N / 2; k++) {
        double a = -2.0 * M_PI * (double)k / (double)SPEC_FFT_N;
        s->tw_re[k] = (float)cos(a);
        s->tw_im[k] = (float)sin(a);
    }
    /* Hann. A rectangular window smears a strong carrier across the whole
       span in sidelobes, which on a finder reads as signal everywhere. */
    for (int i = 0; i < SPEC_FFT_N; i++)
        s->win[i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * (double)i /
                                            (double)(SPEC_FFT_N - 1));

    memset(s->acc, 0, sizeof(s->acc));
    s->navg  = 0;
    s->ready = true;
}

static void fft512(spectrum_t *s, float *re, float *im)
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
                float cr = s->tw_re[t], ci = s->tw_im[t];
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
    spectrum_t *s = &s_default;
    if (!s->ready || !iq) return;

    int nsamp = len / 2;
    if (nsamp < SPEC_FFT_N) return;

    /* u8 IQ from the RTL is offset binary centred on 127.5. */
    for (int i = 0; i < SPEC_FFT_N; i++) {
        float w = s->win[i];
        s->re[i] = ((float)iq[2 * i]     - 127.5f) * (1.0f / 127.5f) * w;
        s->im[i] = ((float)iq[2 * i + 1] - 127.5f) * (1.0f / 127.5f) * w;
    }

    spectrum_context_accum(s);
}

void spectrum_context_accum(spectrum_t *s)
{
    fft512(s, s->re, s->im);

    for (int i = 0; i < SPEC_FFT_N; i++)
        s->acc[i] += s->re[i] * s->re[i] + s->im[i] * s->im[i];

    s->navg++;
}

void spectrum_context_reset(spectrum_t *s)
{
    if (!s->ready) return;
    memset(s->acc, 0, sizeof(s->acc));
    s->navg = 0;
}

int spectrum_navg(void) { return s_default.navg; }

bool spectrum_context_read_db(spectrum_t *s, float *out, int n)
{
    if (!s->ready || !out || n < 1 || s->navg <= 0) return false;

    const int   N     = SPEC_FFT_N;
    const float inv   = 1.0f / (float)s->navg;
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
            float p = s->acc[raw];
            if (p > pk) pk = p;
        }

        float p = pk * scale;
        if (p < 1e-12f) p = 1e-12f;
        out[o] = 10.0f * log10f(p);      /* negative dBFS */
    }
    return true;
}

LS_COLD void spectrum_init(void) { spectrum_context_init(&s_default); }
void spectrum_reset(void) { spectrum_context_reset(&s_default); }
bool spectrum_read_db(float *out, int n)
{ return spectrum_context_read_db(&s_default, out, n); }
