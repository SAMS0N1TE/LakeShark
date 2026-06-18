
#include "fm_dsp.h"
#include "esp_attr.h"
#include <math.h>
#include <string.h>

#define FM_NBFM_PASSES   3

static inline void cmultiply(int ar, int aj, int br, int bj, int *cr, int *cj)
{
    *cr = ar * br - aj * bj;
    *cj = aj * br + ar * bj;
}

static inline int fast_atan2_i(int y, int x)
{
    int yabs, angle;
    int pi4 = (1 << 12), pi34 = 3 * (1 << 12);
    if (x == 0 && y == 0) return 0;
    yabs = y < 0 ? -y : y;
    if (x >= 0) angle = pi4  - pi4 * (x - yabs) / (x + yabs);
    else        angle = pi34 - pi4 * (x + yabs) / (yabs - x);
    return (y < 0) ? -angle : angle;
}

static inline int polar_disc_fast(int ar, int aj, int br, int bj)
{
    int cr, cj;
    cmultiply(ar, aj, br, -bj, &cr, &cj);
    return fast_atan2_i(cj, cr);
}

static void IRAM_ATTR fifth_order(int16_t *data, int length, int16_t *hist)
{
    int i;
    int16_t a, b, c, d, e, f;
    a = hist[1]; b = hist[2]; c = hist[3]; d = hist[4]; e = hist[5];
    f = data[0];
    data[0] = (a + (b + e) * 5 + (c + d) * 10 + f) >> 4;
    for (i = 4; i < length; i += 4) {
        a = c; b = d; c = e; d = f;
        e = data[i - 2];
        f = data[i];
        data[i / 2] = (a + (b + e) * 5 + (c + d) * 10 + f) >> 4;
    }
    hist[0] = a; hist[1] = b; hist[2] = c; hist[3] = d; hist[4] = e; hist[5] = f;
}

static void IRAM_ATTR low_pass_real(fm_dsp_t *s)
{
    int i = 0, i2 = 0;
    int fast = s->rate_out;
    int slow = s->rate_out2;
    while (i < s->result_len) {
        s->now_lpr += s->result[i];
        i++;
        s->prev_lpr_index += slow;
        if (s->prev_lpr_index < fast) continue;
        s->result[i2] = (int16_t)(s->now_lpr / (fast / slow));
        s->prev_lpr_index -= fast;
        s->now_lpr = 0;
        i2 += 1;
    }
    s->result_len = i2;
}

static void IRAM_ATTR fm_demod(fm_dsp_t *d)
{
    int i, pcm;
    int16_t *lp = d->lowpassed;
    pcm = polar_disc_fast(lp[0], lp[1], d->pre_r, d->pre_j);
    d->result[0] = (int16_t)pcm;
    for (i = 2; i < (d->lp_len - 1); i += 2) {
        pcm = polar_disc_fast(lp[i], lp[i + 1], lp[i - 2], lp[i - 1]);
        d->result[i / 2] = (int16_t)pcm;
    }
    d->pre_r = lp[d->lp_len - 2];
    d->pre_j = lp[d->lp_len - 1];
    d->result_len = d->lp_len / 2;
}

static void IRAM_ATTR deemph_filter(fm_dsp_t *fm)
{
    int i, d;
    int avg = fm->deemph_avg;
    for (i = 0; i < fm->result_len; i++) {
        d = fm->result[i] - avg;
        if (d > 0) avg += (d + fm->deemph_a / 2) / fm->deemph_a;
        else       avg += (d - fm->deemph_a / 2) / fm->deemph_a;
        fm->result[i] = (int16_t)avg;
    }
    fm->deemph_avg = avg;
}

static int IRAM_ATTR load_iq(fm_dsp_t *s, const uint8_t *iq, int iq_len)
{
    if (iq_len > FM_DSP_CHUNK) iq_len = FM_DSP_CHUNK;
    int ipeak = 0;
    for (int i = 0; i < iq_len; i++) {
        int v = (int)iq[i] - 127;
        s->lowpassed[i] = (int16_t)v;
        int a = v < 0 ? -v : v;
        if (a > ipeak) ipeak = a;
    }
    s->lp_len = iq_len;

    float peak = s->iq_peak * 0.98f;
    float now  = (float)ipeak / 127.0f;
    if (now > peak) peak = now;
    s->iq_peak = peak > 1.0f ? 1.0f : peak;
    return iq_len;
}

void fm_dsp_init(fm_dsp_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rate_out  = FM_RTL_RATE;
    s->rate_out2 = FM_AUDIO_RATE;

    s->deemph_a = (int)lround(1.0 / (1.0 - exp(-1.0 / (FM_RTL_RATE * 75e-6))));
    if (s->deemph_a < 1) s->deemph_a = 1;
}

int IRAM_ATTR fm_demod_wide(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                            int16_t *pcm16k, int max)
{
    int out = 0;
    for (int off = 0; off < iq_len && out < max; off += FM_DSP_CHUNK) {
        int n = iq_len - off;
        if (n > FM_DSP_CHUNK) n = FM_DSP_CHUNK;
        load_iq(s, iq + off, n);

        fm_demod(s);
        deemph_filter(s);
        low_pass_real(s);
        for (int i = 0; i < s->result_len && out < max; i++)
            pcm16k[out++] = s->result[i];
    }
    return out;
}

int IRAM_ATTR fm_demod_iq(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                          float *demod_out, int max)
{

    const float k = 3.14159265f / (float)(1 << 14);
    int out = 0;
    for (int off = 0; off < iq_len && out < max; off += FM_DSP_CHUNK) {
        int n = iq_len - off;
        if (n > FM_DSP_CHUNK) n = FM_DSP_CHUNK;
        load_iq(s, iq + off, n);

        for (int p = 0; p < FM_NBFM_PASSES; p++) {
            fifth_order(s->lowpassed,     s->lp_len >> p,       s->lp_i_hist[p]);
            fifth_order(s->lowpassed + 1, (s->lp_len >> p) - 1, s->lp_q_hist[p]);
        }
        s->lp_len >>= FM_NBFM_PASSES;
        fm_demod(s);
        for (int i = 0; i < s->result_len && out < max; i++)
            demod_out[out++] = (float)s->result[i] * k;
    }
    return out;
}

#define FM_NB_DEEMPH_A   0.30f
#define FM_NB_SCALE      9000.0f
int fm_demod_to_audio(fm_dsp_t *s, const float *demod, int n,
                      int16_t *pcm16k, int max)
{
    int out = 0;
    for (int i = 0; i < n; i++) {
        s->f_deemph += FM_NB_DEEMPH_A * (demod[i] - s->f_deemph);

        s->f_acc += s->f_deemph;
        if (++s->f_n < 2) continue;
        float a = (s->f_acc * 0.5f) * FM_NB_SCALE;
        s->f_acc = 0.0f; s->f_n = 0;
        if (a > 32767.0f) a = 32767.0f;
        if (a < -32768.0f) a = -32768.0f;
        if (out < max) pcm16k[out++] = (int16_t)a;
        else break;
    }
    return out;
}

float fm_iq_rms(const uint8_t *iq, int iq_len)
{
    if (iq_len < 2) return 0.0f;
    uint64_t sumsq = 0;
    int n = 0;
    for (int p = 0; p + 1 < iq_len; p += 2) {
        int di = (int)iq[p]     - 128;
        int dq = (int)iq[p + 1] - 128;
        sumsq += (uint64_t)(di * di) + (uint64_t)(dq * dq);
        n++;
    }
    if (n == 0) return 0.0f;
    float ms = (float)sumsq / (float)(2 * n);
    float rms = sqrtf(ms) / 127.5f;
    return rms > 1.0f ? 1.0f : rms;
}
