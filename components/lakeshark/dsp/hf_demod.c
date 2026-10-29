/* HF demodulators: AM, SAM, USB, LSB, CW. See hf_demod.h for the chain.
   LakeShark original; written from the textbook forms (Weaver-style SSB by
   complex filtering, a second-order Costas-free carrier PLL for SAM, Kaiser
   window design), not from any other receiver's source. */
#include "hf_demod.h"

#include <math.h>
#include <string.h>
#include "ls_cold.h"

#define TWO_PI_D 6.283185307179586

/* Kaiser beta for about 70 dB (decimators) and 60 dB (channel). */
#define HF_BETA_DECIM   6.8
#define HF_BETA_CHANNEL 5.65

/* AGC timing: hang, then a steady decay in dB per second. */
#define HF_AGC_FAST_HANG_MS  80
#define HF_AGC_FAST_DB_S     30.0
#define HF_AGC_SLOW_HANG_MS 400
#define HF_AGC_SLOW_DB_S     10.0
/* At most this much gain: the 8-bit noise floor in a CW filter is ~-75 dBFS. */
#define HF_AGC_MAX_DB        80.0

/* Blanker: power over the running average that counts as an impulse, and
   how long a gate is held after it (input samples at 256k, ~62 us). */
#define HF_NB_HOLD           16
#define HF_NB_LOW_X          20.0f
#define HF_NB_HIGH_X          8.0f

LS_COLD static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    const double y = x * x / 4.0;
    for (int k = 1; k < 40; ++k) {
        term *= y / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

/* Windowed-sinc lowpass with unity gain at DC. */
LS_COLD static void design_lowpass(float *h, int n, double cutoff_hz, double fs_hz,
                           double beta)
{
    const double m = (n - 1) / 2.0, fc = cutoff_hz / fs_hz;
    const double i0b = bessel_i0(beta);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = i - m;
        const double sinc = t == 0.0 ? 2.0 * fc
                                     : sin(TWO_PI_D * fc * t) / (M_PI * t);
        const double r = m > 0.0 ? t / m : 0.0;
        const double w = bessel_i0(beta * sqrt(1.0 - r * r)) / i0b;
        h[i] = (float)(sinc * w);
        sum += h[i];
    }
    for (int i = 0; i < n; ++i) h[i] = (float)(h[i] / sum);
}

static int channel_taps(hf_demod_mode_t mode)
{
    if (mode == HF_DEMOD_CW) return 255;
    if (mode == HF_DEMOD_USB || mode == HF_DEMOD_LSB) return 191;
    return 127;
}

static void phasor_step(float hz, float rate, float *dre, float *dim)
{
    const double a = TWO_PI_D * (double)hz / (double)rate;
    *dre = (float)cos(a);
    *dim = (float)sin(a);
}

static void agc_timing(hf_demod_t *s)
{
    const bool fast = s->cfg.agc == HF_AGC_FAST;
    const double db_s = fast ? HF_AGC_FAST_DB_S : HF_AGC_SLOW_DB_S;
    s->agc_hang_n = (fast ? HF_AGC_FAST_HANG_MS : HF_AGC_SLOW_HANG_MS) *
                    (int)HF_OUT_RATE / 1000;
    s->agc_decay = (float)pow(10.0, -db_s / 20.0 / HF_OUT_RATE);
}

void hf_demod_reset(hf_demod_t *s)
{
    memset(s->s1_i, 0, sizeof(s->s1_i)); memset(s->s1_q, 0, sizeof(s->s1_q));
    memset(s->s2_i, 0, sizeof(s->s2_i)); memset(s->s2_q, 0, sizeof(s->s2_q));
    memset(s->ch_i, 0, sizeof(s->ch_i)); memset(s->ch_q, 0, sizeof(s->ch_q));
    memset(s->nb_di, 0, sizeof(s->nb_di)); memset(s->nb_dq, 0, sizeof(s->nb_dq));
    s->s1_pos = s->s2_pos = s->ch_pos = 0;
    s->s1_cnt = s->s2_cnt = 0;
    s->nco_re = 1.0f; s->nco_im = 0.0f; s->nco_norm = 0;
    s->post_re = 1.0f; s->post_im = 0.0f; s->post_norm = 0;
    s->nb_avg = 0.0f; s->nb_pos = 0; s->nb_left = 0;
    s->pll_re = 1.0f; s->pll_im = 0.0f; s->pll_freq = 0.0f; s->pll_norm = 0;
    s->dc = 0.0f;
    s->power = 0.0f;
    /* Keep the gain; let the next envelope set the peak from scratch. */
    s->agc_peak = s->agc_gain > 0.0f ? (HF_PCM_TARGET / 32767.0f) / s->agc_gain
                                     : s->agc_floor;
    s->agc_hang = 0;
}

LS_COLD static void apply_tuning(hf_demod_t *s)
{
    const hf_demod_cfg_t *c = &s->cfg;
    int32_t lo, hi;
    hf_demod_passband(c->mode, c->filter_hz, &lo, &hi);
    float center = 0.0f, post = 0.0f;
    if (c->mode == HF_DEMOD_USB || c->mode == HF_DEMOD_LSB) {
        center = 0.5f * (float)(lo + hi);
        post = center;
    } else if (c->mode == HF_DEMOD_CW) {
        post = (float)c->cw_pitch_hz;
    }
    s->pb_center_hz = center;
    /* Mixing down by the dial, the clarifier and the passband centre puts the
       wanted passband on 0 Hz for the lowpass. */
    phasor_step(-((float)c->tune_hz + (float)c->rit_hz + center),
                (float)HF_IN_RATE, &s->nco_dre, &s->nco_dim);
    phasor_step(post, (float)HF_OUT_RATE, &s->post_dre, &s->post_dim);
}

LS_COLD void hf_demod_set(hf_demod_t *s, const hf_demod_cfg_t *cfg)
{
    s->cfg = *cfg;
    if ((unsigned)s->cfg.mode >= HF_DEMOD_COUNT) s->cfg.mode = HF_DEMOD_AM;
    if ((unsigned)s->cfg.agc >= HF_AGC_COUNT) s->cfg.agc = HF_AGC_SLOW;
    if ((unsigned)s->cfg.nb >= HF_NB_COUNT) s->cfg.nb = HF_NB_OFF;
    s->cfg.filter_hz = hf_demod_clamp_width(s->cfg.mode, s->cfg.filter_hz);
    if (s->cfg.cw_pitch_hz < 300 || s->cfg.cw_pitch_hz > 1200)
        s->cfg.cw_pitch_hz = 700;
    if (s->cfg.mode != s->ch_mode || s->cfg.filter_hz != s->ch_width ||
        s->ch_n == 0) {
        const int n = channel_taps(s->cfg.mode);
        design_lowpass(s->ch_h, n, s->cfg.filter_hz / 2.0, HF_OUT_RATE,
                       HF_BETA_CHANNEL);
        if (n != s->ch_n) {
            memset(s->ch_i, 0, sizeof(s->ch_i));
            memset(s->ch_q, 0, sizeof(s->ch_q));
            s->ch_pos = 0;
        }
        s->ch_n = n;
        s->ch_mode = s->cfg.mode;
        s->ch_width = s->cfg.filter_hz;
        s->dc = 0.0f;
        s->pll_freq = 0.0f;
    }
    s->nb_thresh = s->cfg.nb == HF_NB_HIGH ? HF_NB_HIGH_X : HF_NB_LOW_X;
    agc_timing(s);
    apply_tuning(s);
}

void hf_demod_init(hf_demod_t *s)
{
    memset(s, 0, sizeof(*s));
    design_lowpass(s->s1_h, HF_S1_TAPS, 16000.0, HF_IN_RATE, HF_BETA_DECIM);
    design_lowpass(s->s2_h, HF_S2_TAPS, 8000.0, HF_MID_RATE, HF_BETA_DECIM);
    /* SAM loop: natural frequency 25 Hz, damping 0.707, pull-in +-250 Hz. */
    const double wn = TWO_PI_D * 25.0 / HF_OUT_RATE;
    s->pll_alpha = (float)(2.0 * 0.707 * wn);
    s->pll_beta = (float)(wn * wn);
    s->pll_limit = (float)(TWO_PI_D * 250.0 / HF_OUT_RATE);
    /* The AM carrier tracker: about 20 Hz. */
    s->dc_k = (float)(1.0 - exp(-TWO_PI_D * 20.0 / HF_OUT_RATE));
    const float target = HF_PCM_TARGET / 32767.0f;
    s->agc_floor = target / (float)pow(10.0, HF_AGC_MAX_DB / 20.0);
    s->agc_gain = (float)pow(10.0, 30.0 / 20.0);
    const hf_demod_cfg_t cfg = {
        .mode = HF_DEMOD_AM, .filter_hz = 6000, .tune_hz = 0, .rit_hz = 0,
        .cw_pitch_hz = 700, .agc = HF_AGC_SLOW, .nb = HF_NB_OFF,
    };
    s->ch_mode = HF_DEMOD_COUNT;
    hf_demod_set(s, &cfg);
    hf_demod_reset(s);
}

static inline void ring_push(float *ri, float *rq, int n, int *pos,
                             float i, float q)
{
    int p = *pos + 1;
    if (p >= n) p = 0;
    *pos = p;
    ri[p] = ri[p + n] = i;
    rq[p] = rq[p + n] = q;
}

/* Symmetric taps, so the window's order does not matter. */
static inline void ring_fir(const float *h, const float *ri, const float *rq,
                            int n, int pos, float *oi, float *oq)
{
    const float *a = ri + pos + 1, *b = rq + pos + 1;
    float si = 0.0f, sq = 0.0f;
    for (int k = 0; k < n; ++k) {
        si += h[k] * a[k];
        sq += h[k] * b[k];
    }
    *oi = si;
    *oq = sq;
}

static inline void renorm(float *re, float *im, int *count)
{
    if (++*count < 128) return;
    *count = 0;
    const float g = 1.5f - 0.5f * (*re * *re + *im * *im);
    *re *= g;
    *im *= g;
}

/* One channel sample at 16k: demodulate, level, return int16 audio. */
static int16_t demod_one(hf_demod_t *s, float yi, float yq)
{
    const float p = yi * yi + yq * yq;
    const float env = sqrtf(p);
    s->power += (p - s->power) * (1.0f / 1600.0f);
    float a;
    switch (s->cfg.mode) {
    case HF_DEMOD_AM:
        s->dc += (env - s->dc) * s->dc_k;
        a = env - s->dc;
        break;
    case HF_DEMOD_SAM: {
        /* Rotate onto the loop's carrier phase; the quadrature part over the
           envelope is the sine of the phase error. */
        const float zi = yi * s->pll_re + yq * s->pll_im;
        const float zq = yq * s->pll_re - yi * s->pll_im;
        const float err = env > 1e-9f ? zq / env : 0.0f;
        s->pll_freq += s->pll_beta * err;
        if (s->pll_freq > s->pll_limit) s->pll_freq = s->pll_limit;
        if (s->pll_freq < -s->pll_limit) s->pll_freq = -s->pll_limit;
        const float d = s->pll_freq + s->pll_alpha * err;
        /* Small-angle rotation; renormalised below. */
        const float c = 1.0f - 0.5f * d * d, sn = d - d * d * d * (1.0f / 6.0f);
        const float re = s->pll_re * c - s->pll_im * sn;
        s->pll_im = s->pll_re * sn + s->pll_im * c;
        s->pll_re = re;
        renorm(&s->pll_re, &s->pll_im, &s->pll_norm);
        s->dc += (zi - s->dc) * s->dc_k;
        a = zi - s->dc;
        break;
    }
    default: {
        /* SSB and CW: turn the centred passband back up and take the real
           part. The other sideband was outside the lowpass. */
        a = yi * s->post_re - yq * s->post_im;
        const float re = s->post_re * s->post_dre - s->post_im * s->post_dim;
        s->post_im = s->post_re * s->post_dim + s->post_im * s->post_dre;
        s->post_re = re;
        renorm(&s->post_re, &s->post_im, &s->post_norm);
        break;
    }
    }

    if (s->cfg.agc != HF_AGC_OFF) {
        if (env > s->agc_peak) {
            s->agc_peak = env;
            s->agc_hang = s->agc_hang_n;
        } else if (s->agc_hang > 0) {
            --s->agc_hang;
        } else {
            s->agc_peak *= s->agc_decay;
        }
        if (s->agc_peak < s->agc_floor) s->agc_peak = s->agc_floor;
        s->agc_gain = (HF_PCM_TARGET / 32767.0f) / s->agc_peak;
    }
    float v = a * s->agc_gain * 32767.0f;
    if (v > HF_PCM_LIMIT) v = HF_PCM_LIMIT;
    if (v < -HF_PCM_LIMIT) v = -HF_PCM_LIMIT;
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* One input pair at 256k. Returns true with *out set when a 16k sample is out. */
static inline bool push_one(hf_demod_t *s, float i, float q, int16_t *out)
{
    if (s->cfg.nb != HF_NB_OFF) {
        const float pw = i * i + q * q;
        if (s->nb_avg <= 0.0f) s->nb_avg = pw;
        const float lim = s->nb_thresh * s->nb_avg;
        if (pw > lim && s->nb_avg > 1e-12f) s->nb_left = HF_NB_DELAY + HF_NB_HOLD;
        /* A clipped contribution: an impulse barely moves the average, a
           carrier that has really arrived lifts it within a fraction of a
           millisecond and stops being blanked. */
        s->nb_avg += ((pw < lim ? pw : lim) - s->nb_avg) * (1.0f / 512.0f);
        const float di = s->nb_di[s->nb_pos], dq = s->nb_dq[s->nb_pos];
        s->nb_di[s->nb_pos] = i;
        s->nb_dq[s->nb_pos] = q;
        if (++s->nb_pos >= HF_NB_DELAY) s->nb_pos = 0;
        if (s->nb_left > 0) {
            --s->nb_left;
            ++s->nb_blanked;
            i = q = 0.0f;
        } else {
            i = di;
            q = dq;
        }
    }
    const float mi = i * s->nco_re - q * s->nco_im;
    const float mq = i * s->nco_im + q * s->nco_re;
    const float re = s->nco_re * s->nco_dre - s->nco_im * s->nco_dim;
    s->nco_im = s->nco_re * s->nco_dim + s->nco_im * s->nco_dre;
    s->nco_re = re;
    renorm(&s->nco_re, &s->nco_im, &s->nco_norm);

    ring_push(s->s1_i, s->s1_q, HF_S1_TAPS, &s->s1_pos, mi, mq);
    if (++s->s1_cnt < (int)(HF_IN_RATE / HF_MID_RATE)) return false;
    s->s1_cnt = 0;
    float ai, aq;
    ring_fir(s->s1_h, s->s1_i, s->s1_q, HF_S1_TAPS, s->s1_pos, &ai, &aq);
    ring_push(s->s2_i, s->s2_q, HF_S2_TAPS, &s->s2_pos, ai, aq);
    if (++s->s2_cnt < (int)(HF_MID_RATE / HF_OUT_RATE)) return false;
    s->s2_cnt = 0;
    float bi, bq;
    ring_fir(s->s2_h, s->s2_i, s->s2_q, HF_S2_TAPS, s->s2_pos, &bi, &bq);
    ring_push(s->ch_i, s->ch_q, s->ch_n, &s->ch_pos, bi, bq);
    float yi, yq;
    ring_fir(s->ch_h, s->ch_i, s->ch_q, s->ch_n, s->ch_pos, &yi, &yq);
    *out = demod_one(s, yi, yq);
    return true;
}

int hf_demod_u8(hf_demod_t *s, const uint8_t *iq, size_t bytes,
                int16_t *pcm, int max)
{
    int n = 0;
    const size_t pairs = bytes / 2;
    for (size_t k = 0; k < pairs; ++k) {
        int16_t o;
        const float i = ((float)iq[2 * k] - 127.5f) * (1.0f / 128.0f);
        const float q = ((float)iq[2 * k + 1] - 127.5f) * (1.0f / 128.0f);
        if (push_one(s, i, q, &o) && n < max) pcm[n++] = o;
    }
    return n;
}

int hf_demod_cf32(hf_demod_t *s, const float *iq, size_t pairs,
                  int16_t *pcm, int max)
{
    int n = 0;
    for (size_t k = 0; k < pairs; ++k) {
        int16_t o;
        if (push_one(s, iq[2 * k], iq[2 * k + 1], &o) && n < max) pcm[n++] = o;
    }
    return n;
}

float hf_demod_level_dbfs(const hf_demod_t *s)
{
    return s->power > 1e-15f ? 10.0f * log10f(s->power) : -150.0f;
}

float hf_demod_gain_db(const hf_demod_t *s)
{
    return s->agc_gain > 0.0f ? 20.0f * log10f(s->agc_gain) : 0.0f;
}

float hf_demod_sam_offset_hz(const hf_demod_t *s)
{
    if (s->cfg.mode != HF_DEMOD_SAM) return 0.0f;
    return s->pll_freq * (float)(HF_OUT_RATE / TWO_PI_D);
}

uint32_t hf_demod_blanked(const hf_demod_t *s) { return s->nb_blanked; }
