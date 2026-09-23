#include "dmr_iq_gen.h"

#include <math.h>
#include <string.h>

#define IQ_RATE      240000            /* what the pipeline is fed */
#define BAUD         4800
#define SPS          (IQ_RATE / BAUD)  /* 50 samples per symbol */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ETSI TS 102 361-1 5.1.1, in the dibit order dmr_gen uses: the symbol for
   bits 01 is +1944 Hz, 00 is +648, 10 is -648, 11 is -1944. */
static float deviation_for(int hi, int lo)
{
    if (!hi && lo) return  1944.0f;
    if (!hi)       return   648.0f;
    if (!lo)       return  -648.0f;
    return              -1944.0f;
}

/* Carrier phase is kept across calls so a burst appended after an idle run
   does not start with a discontinuity the discriminator would read as a
   large frequency spike. */
static double s_phase;

static int render(const float *dev, int n_sym, const dmr_iq_channel_t *ch,
                  ls_rng_t *rng, uint8_t *iq, int iq_max)
{
    const int need = n_sym * SPS * 2;
    if (!iq || need > iq_max) return 0;

    const double off = ch ? (double)ch->freq_off_hz : 0.0;
    const double sd  = ch ? (double)ch->noise_sd : 0.0;
    int w = 0;

    for (int s = 0; s < n_sym; s++) {
        /* A raised-cosine transition rather than a step - a square frequency
           profile has infinite bandwidth and the receiver's filters ring on
           it - but confined to the first RAMP of the symbol, so the middle is
           flat and a sample taken there is not still moving. Spreading the
           ramp across the whole symbol smears one symbol into the next and
           cost 7% of the bits. */
        const double RAMP = 0.3;
        const double from = (s == 0) ? dev[0] : dev[s - 1];
        const double to   = dev[s];
        for (int k = 0; k < SPS; k++) {
            const double t = (double)k / SPS;
            const double u = t < RAMP ? t / RAMP : 1.0;
            const double blend = 0.5 - 0.5 * cos(M_PI * u);
            const double f = from + (to - from) * blend + off;
            s_phase += 2.0 * M_PI * f / IQ_RATE;
            if (s_phase >  M_PI) s_phase -= 2.0 * M_PI;
            if (s_phase < -M_PI) s_phase += 2.0 * M_PI;

            double i = cos(s_phase) * 90.0;
            double q = sin(s_phase) * 90.0;
            if (sd > 0.0 && rng) {
                i += ls_rng_noise(rng) * sd * 4.0;
                q += ls_rng_noise(rng) * sd * 4.0;
            }
            /* Centre at 127.5, which is what an RTL hands over. */
            int vi = (int)lround(i + 127.5), vq = (int)lround(q + 127.5);
            iq[w++] = (uint8_t)(vi < 0 ? 0 : vi > 255 ? 255 : vi);
            iq[w++] = (uint8_t)(vq < 0 ? 0 : vq > 255 ? 255 : vq);
        }
    }
    return w;
}

int dmr_iq_render_bits(const uint8_t *bits, int n_bits,
                       const dmr_iq_channel_t *ch, ls_rng_t *rng,
                       uint8_t *iq, int iq_max)
{
    if (!bits || n_bits < 2) return 0;
    const int n_sym = n_bits / 2;
    static float dev[DMR_BURST_BITS];
    if (n_sym > (int)(sizeof(dev) / sizeof(dev[0]))) return 0;

    for (int s = 0; s < n_sym; s++) {
        const int i = s * 2;
        const int hi = (bits[i >> 3] >> (7u - (i & 7u))) & 1u;
        const int lo = (bits[(i + 1) >> 3] >> (7u - ((i + 1) & 7u))) & 1u;
        dev[s] = deviation_for(hi, lo);
    }
    return render(dev, n_sym, ch, rng, iq, iq_max);
}

int dmr_iq_render_idle(int symbols, const dmr_iq_channel_t *ch,
                       ls_rng_t *rng, uint8_t *iq, int iq_max)
{
    if (symbols <= 0) return 0;
    static float dev[4096];
    if (symbols > (int)(sizeof(dev) / sizeof(dev[0]))) return 0;
    /* Alternating outer symbols: carries no data, but gives the level
       trackers the full deviation range to measure, which is what they need
       before any slicing is meaningful. */
    for (int s = 0; s < symbols; s++)
        dev[s] = (s & 1) ? 1944.0f : -1944.0f;
    return render(dev, symbols, ch, rng, iq, iq_max);
}
