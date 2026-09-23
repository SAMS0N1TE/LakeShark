
#include "fm_dsp.h"
#include "esp_attr.h"
#include <limits.h>
#include <math.h>
#include <string.h>

#define FM_NBFM_PASSES   3

static inline void cmultiply(int ar, int aj, int br, int bj, int *cr, int *cj)
{
    *cr = ar * br - aj * bj;
    *cj = aj * br + ar * bj;
}

/* The largest magnitude pi4 can still be multiplied by inside an int. */
#define FM_ATAN_LIMIT (INT_MAX / (1 << 12))

static inline int fast_atan2_i(int y, int x)
{
    const int pi4 = (1 << 12), pi34 = 3 * (1 << 12);
    if (x == 0 && y == 0) return 0;

    /* The whole function is one ratio, so form it first and in a width that
       cannot wrap.  pi4 * numerator is what used to leave an int, and on the
       narrowband path it did so on any signal worth listening to: three
       interpolation passes bring an IQ sample to about +/-1024, and the dot
       product of two of those passes FM_ATAN_LIMIT.  The wrap put the angle
       out by up to a fifth of full scale - an impulse in the audio, arriving
       exactly when the signal got strong.

       yabs is taken here too rather than in an int, because negating INT_MIN
       in one is undefined in its own right. */
    const int64_t yabs = y < 0 ? -(int64_t)y : (int64_t)y;
    int64_t num = (x >= 0) ? (int64_t)x - yabs : (int64_t)x + yabs;
    int64_t den = (x >= 0) ? (int64_t)x + yabs : yabs - (int64_t)x;

    /* Halve the ratio until both halves fit, which leaves its value alone and
       costs only resolution - of which there is a large surplus, a thirteen
       bit result being read off a divisor still twenty bits wide.

       The test is on the numerator and the divisor themselves, not on x and
       yabs: two large values close together make a small difference, and
       that case never needed shifting.  Testing the quantity that actually
       has to fit is what makes every input that already worked come back
       bit-identical rather than merely close.

       It cannot divide its way to a zero divisor, because den >= |num| holds
       throughout - for either sign of x, |x -/+ yabs| <= |x| + yabs - so the
       loop stops while den is still at least half the limit. */
    while (num > FM_ATAN_LIMIT || num < -FM_ATAN_LIMIT || den > INT_MAX) {
        num /= 2;
        den /= 2;
    }

    const int base = (x >= 0) ? pi4 : pi34;
    const int angle = base - pi4 * (int)num / (int)den;
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
    if (now > s->iq_block_peak) s->iq_block_peak = now;
    if (now > peak) peak = now;
    s->iq_peak = peak > 1.0f ? 1.0f : peak;
    return iq_len;
}

void fm_dsp_init(fm_dsp_t *s)
{
    memset(s, 0, sizeof(*s));
    s->squelch_settle_samples = FM_DEMOD_RATE * 64 / 1000;
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
    s->iq_block_peak = 0.0f;

    const float k = 3.14159265f / (float)(1 << 14);
    float noise_sum = 0.0f;
    int   noise_n = 0;
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
        for (int i = 0; i < s->result_len && out < max; i++) {
            const float d = (float)s->result[i] * k;
            demod_out[out++] = d;
            /* One-pole high pass, then power. The pole sits well above speech
               so voice contributes little and hiss contributes nearly all. */
            s->noise_hp += 0.25f * (d - s->noise_hp);
            const float hp = d - s->noise_hp;
            noise_sum += hp * hp;
            noise_n++;
        }
    }
    if (noise_n > 0) s->demod_noise = sqrtf(noise_sum / (float)noise_n);
    return out;
}

#define FM_NB_DEEMPH_A   0.30f
/* How much quieter than the gate the noise must get to open, versus how much
   louder to shut: 0.06 of full scale, against a gate that sits at 0.70 by
   default. The release window is about 250 ms at FM_DEMOD_RATE, long enough
   that a burst of noise inside a transmission rides through. */
#define FM_SQUELCH_HYSTERESIS       0.06f
#define FM_SQUELCH_RELEASE_SAMPLES  (FM_DEMOD_RATE * 250 / 1000)
/* Open on quiet, not on level.

   This used to gate on iq_block_peak, the carrier level across the whole IQ
   block, and that cannot do the job: a 12.5 kHz channel barely moves the peak
   of a wideband block. Measured on a T-Display-P4 with an RTL-SDR, the
   strongest broadcast in the area read 5% of full scale and dead air read 3%,
   against a default threshold of 15. The squelch could never open, which is
   what "NFM never picks anything up" was.

   Post-demod noise separates the same two cases cleanly, which is what an FM
   noise squelch has always measured: with no carrier the discriminator puts
   out loud hiss, and a carrier quietens it. Same bench, demod_noise x1000:
   broadcast 280..593, dead air 809..973. The gate sits between them.

   threshold_pct keeps its direction, higher is more squelch: 0 opens on
   anything, 100 never opens, and the default 30 puts the gate at 0.70. */
int fm_nfm_squelch(fm_dsp_t *s, int threshold_pct, int samples)
{
    const int qualify = FM_DEMOD_RATE * 64 / 1000;
    if (samples <= 0) return 0;
    if (s->squelch_settle_samples > 0) {
        s->squelch_settle_samples -= samples;
        s->squelch_samples = 0;
        return 0;
    }
    if (threshold_pct < 0)   threshold_pct = 0;
    if (threshold_pct > 100) threshold_pct = 100;
    const float gate = (float)(100 - threshold_pct) * 0.010f;

    /* Two thresholds and a release timer, which is what stops the chop.
       Closing on the same number it opens on means a single noisy block
       drops the audio and costs another 64 ms qualify window to recover,
       and the player splices a silence chunk into the speaker for every one
       of those. Measured on hardware: a solid broadcast reads 0.24 to 0.55
       against a 0.70 gate and still produced ~1.6 underruns a second, all
       of them audible. So once open it takes a clear rise and a sustained
       one to shut again. */
    const float release = gate + FM_SQUELCH_HYSTERESIS;

    if (s->squelch_is_open) {
        if (s->demod_noise > release) {
            s->squelch_release_samples += samples;
            if (s->squelch_release_samples >= FM_SQUELCH_RELEASE_SAMPLES) {
                s->squelch_is_open = false;
                s->squelch_samples = 0;
                s->squelch_release_samples = 0;
                return 0;
            }
        } else {
            s->squelch_release_samples = 0;
        }
        return 1;
    }

    if (s->demod_noise >= gate) {
        s->squelch_samples = 0;
        return 0;
    }
    if (s->squelch_samples < qualify) s->squelch_samples += samples;
    if (s->squelch_samples >= qualify) {
        s->squelch_is_open = true;
        s->squelch_release_samples = 0;
        return 1;
    }
    return 0;
}

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

int fm_demod_am(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                int16_t *pcm16k, int max)
{
    int out = 0;
    for (int off = 0; off < iq_len && out < max; off += FM_DSP_CHUNK) {
        int n = iq_len - off;
        if (n > FM_DSP_CHUNK) n = FM_DSP_CHUNK;
        n &= ~1;
        if (n < 16) break;
        load_iq(s, iq + off, n);
        for (int p = 0; p < FM_NBFM_PASSES; ++p) {
            fifth_order(s->lowpassed, s->lp_len >> p, s->lp_i_hist[p]);
            fifth_order(s->lowpassed + 1, (s->lp_len >> p) - 1, s->lp_q_hist[p]);
        }
        const int len = s->lp_len >> FM_NBFM_PASSES;
        for (int i = 0; i + 1 < len; i += 2) {
            const float re = s->lowpassed[i], im = s->lowpassed[i + 1];
            const float magnitude = sqrtf(re * re + im * im);
            if (s->am_dc <= 0.0f) s->am_dc = magnitude;
            s->am_dc += 0.002f * (magnitude - s->am_dc);
            s->f_acc += (magnitude - s->am_dc) * (10000.0f / (s->am_dc + 1.0f));
            if (++s->f_n < 2) continue;
            float sample = s->f_acc * 0.5f;
            s->f_acc = 0;
            s->f_n = 0;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            pcm16k[out++] = (int16_t)sample;
            if (out >= max) break;
        }
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
