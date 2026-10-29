#include "dmr_iq_gen.h"

#include <math.h>
#include <stdbool.h>
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

/* ------------------------------------------------------ symbol streams --- */

int dmr_iq_dev_bits(float *dev, int at, int max, const uint8_t *bits, int n_bits)
{
    if (!dev || !bits || at < 0 || at + n_bits / 2 > max) return -1;
    for (int i = 0; i + 1 < n_bits; i += 2) {
        const int hi = (bits[i >> 3] >> (7u - (i & 7u))) & 1u;
        const int lo = (bits[(i + 1) >> 3] >> (7u - ((i + 1) & 7u))) & 1u;
        dev[at++] = deviation_for(hi, lo);
    }
    return at;
}

int dmr_iq_dev_idle(float *dev, int at, int max, int symbols)
{
    if (!dev || at < 0 || at + symbols > max) return -1;
    for (int s = 0; s < symbols; s++)
        dev[at++] = (s & 1) ? 1944.0f : -1944.0f;
    return at;
}

int dmr_iq_dev_random(float *dev, int at, int max, int symbols, ls_rng_t *rng)
{
    if (!dev || !rng || at < 0 || at + symbols > max) return -1;
    for (int s = 0; s < symbols; s++) {
        const unsigned d = ls_rng_u32(rng) >> 30;
        dev[at++] = deviation_for((int)(d >> 1), (int)(d & 1u));
    }
    return at;
}

int dmr_iq_dev_off(float *dev, int at, int max, int symbols)
{
    if (!dev || at < 0 || at + symbols > max) return -1;
    for (int s = 0; s < symbols; s++) dev[at++] = DMR_IQ_OFF;
    return at;
}

/* Root raised cosine, alpha 0.2 (ETSI TS 102 361-1 5.1.1 gives the
   transmitter's shaping filter as a root raised cosine with this roll-off),
   tabulated across +/-RRC_SPAN symbols. The receiver's own RRC then makes the
   pair a raised cosine, which is what puts zero ISI at the symbol centre. */
#define RRC_ALPHA   0.2
#define RRC_SPAN    8
#define RRC_RES     200                      /* table points per symbol */
#define RRC_N       (2 * RRC_SPAN * RRC_RES + 1)

static float s_rrc[RRC_N];
static bool  s_rrc_ready;

static double rrc_at(double t)
{
    const double a = RRC_ALPHA;
    if (fabs(t) < 1e-9) return 1.0 - a + 4.0 * a / M_PI;
    if (fabs(fabs(t) - 1.0 / (4.0 * a)) < 1e-9)
        return a / sqrt(2.0) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * a)) +
                                (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * a)));
    const double num = sin(M_PI * t * (1.0 - a)) +
                       4.0 * a * t * cos(M_PI * t * (1.0 + a));
    const double den = M_PI * t * (1.0 - (4.0 * a * t) * (4.0 * a * t));
    return num / den;
}

static void rrc_build(void)
{
    if (s_rrc_ready) return;
    /* Unit gain at DC: a run of one symbol value deviates by that value. */
    double dc = 0.0;
    for (int k = -RRC_SPAN; k <= RRC_SPAN; k++) dc += rrc_at((double)k);
    for (int i = 0; i < RRC_N; i++)
        s_rrc[i] = (float)(rrc_at((double)(i - RRC_SPAN * RRC_RES) / RRC_RES) / dc);
    s_rrc_ready = true;
}

static float rrc_lookup(double t)
{
    const double x = (t + RRC_SPAN) * RRC_RES;
    if (x < 0.0 || x >= RRC_N - 1) return 0.0f;
    const int i = (int)x;
    const float f = (float)(x - i);
    return s_rrc[i] + (s_rrc[i + 1] - s_rrc[i]) * f;
}

/* The ramp render() uses, evaluated at symbol time u. */
static float ramp_at(const float *dev, int n_sym, double u)
{
    int s = (int)floor(u);
    if (s < 0) s = 0;
    if (s >= n_sym) s = n_sym - 1;
    const double frac = u - floor(u);
    const double RAMP = 0.3;
    const float to   = dev[s] == DMR_IQ_OFF ? 0.0f : dev[s];
    const float from = s == 0 ? to : dev[s - 1] == DMR_IQ_OFF ? 0.0f : dev[s - 1];
    const double v = frac < RAMP ? frac / RAMP : 1.0;
    return from + (to - from) * (float)(0.5 - 0.5 * cos(M_PI * v));
}

/* Box-Muller. The sum-of-uniforms ls_rng_noise() has no tail past about
   seven sigma and a thin one well before, and the tail is where the error
   rate near a threshold comes from. */
static double gauss(ls_rng_t *rng)
{
    const double u1 = ((double)ls_rng_u32(rng) + 1.0) / 4294967297.0;
    const double u2 = (double)ls_rng_u32(rng) / 4294967296.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

int dmr_iq_render_stream(const float *dev, int n_sym,
                         const dmr_iq_channel_t *ch, ls_rng_t *rng,
                         float *iq, int max_pairs)
{
    const double start = ch ? (double)ch->start_sym : 0.0;
    const double tsym = (double)SPS * (1.0 + (ch ? ch->clock_ppm : 0.0f) * 1e-6);
    const int pairs = (int)((n_sym + start) * tsym) + 1;
    if (!dev || !iq || n_sym <= 0 || pairs > max_pairs) return 0;
    rrc_build();

    const double off  = ch ? (double)ch->freq_off_hz : 0.0;
    const double sign = ch && ch->invert ? -1.0 : 1.0;
    const double A    = 0.7;
    double sd = 0.0;
    if (ch && ch->cn_db != 0.0f)
        /* C/N in 12.5 kHz of a channel sampled at 240 kHz: the noise is
           spread across the whole sample rate, so only 12.5/240 of it is
           in-channel, split equally over I and Q. */
        sd = sqrt(A * A / pow(10.0, ch->cn_db / 10.0) / (2.0 * 12500.0 / IQ_RATE));
    else if (ch)
        sd = ch->noise_sd * 4.0 * 0.2887 / 127.5;   /* render()'s scale */

    double phase = 0.0;
    for (int n = 0; n < pairs; n++) {
        const double u = n / tsym - start;           /* time in symbols */
        const int s = (int)floor(u);
        double f = 0.0;
        if (ch && ch->shape == DMR_IQ_RRC) {
            for (int k = s - RRC_SPAN; k <= s + RRC_SPAN + 1; k++)
                if (k >= 0 && k < n_sym && dev[k] != DMR_IQ_OFF)
                    f += dev[k] * rrc_lookup(u - k - 0.5);
        } else if (u >= 0.0) {
            f = ramp_at(dev, n_sym, u);
        }
        const bool keyed = s >= 0 && s < n_sym && dev[s] != DMR_IQ_OFF;
        phase += 2.0 * M_PI * (sign * f + off) / IQ_RATE;
        if (phase >  M_PI) phase -= 2.0 * M_PI;
        if (phase < -M_PI) phase += 2.0 * M_PI;
        double i = keyed ? cos(phase) * A : 0.0;
        double q = keyed ? sin(phase) * A : 0.0;
        if (sd > 0.0 && rng) {
            i += gauss(rng) * sd;
            q += gauss(rng) * sd;
        }
        iq[2 * n]     = (float)i;
        iq[2 * n + 1] = (float)q;
    }
    return pairs;
}
