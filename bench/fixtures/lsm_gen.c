/* see lsm_gen.h. */
#include "lsm_gen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp_pipeline.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR_IN   (DSP_SAMPLE_RATE * DSP_PRE_DECIM)
#define SPS_IN  (SR_IN / DSP_BAUD)
#define RRC_HALF_SYM 4
#define RRC_LEN      (2 * RRC_HALF_SYM * SPS_IN + 1)

/* 0x5575F5FF77FF, using the same dibit-to-level map as production DSD. */
const int LSM_SYNC[LSM_SYNC_LEN] = {
    3, 3, 3, 3, 3, -3, 3, 3, -3, -3, 3, 3,
    -3, -3, -3, -3, 3, -3, 3, -3, -3, -3, -3, -3
};

void lsm_channel_clean(lsm_channel_t *ch)
{
    memset(ch, 0, sizeof(*ch));
}

static void make_rrc(double *taps, double beta)
{
    for (int i = 0; i < RRC_LEN; i++) {
        double t = (double)(i - RRC_LEN / 2) / (double)SPS_IN;
        double v;
        if (fabs(t) < 1e-8) {
            v = 1.0 - beta + 4.0 * beta / M_PI;
        } else if (fabs(fabs(4.0 * beta * t) - 1.0) < 1e-6) {
            v = beta / sqrt(2.0)
              * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * beta))
               + (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * beta)));
        } else {
            double num = sin(M_PI * t * (1.0 - beta))
                       + 4.0 * beta * t * cos(M_PI * t * (1.0 + beta));
            double den = M_PI * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t));
            v = num / den;
        }
        taps[i] = v;
    }
    double energy = 0.0;
    for (int i = 0; i < RRC_LEN; i++) energy += taps[i] * taps[i];
    double gain = sqrt((double)SPS_IN / energy);
    for (int i = 0; i < RRC_LEN; i++) taps[i] *= gain;
}

int lsm_render_iq(const int *syms, int n_syms, const lsm_channel_t *ch,
                  ls_rng_t *rng, uint8_t *iq, int iq_max)
{
    int n_samp = n_syms * SPS_IN;
    if (n_syms <= 0 || iq_max < n_samp * 2) return 0;

    double *taps = (double *)malloc(sizeof(double) * RRC_LEN);
    float *si = (float *)malloc(sizeof(float) * (size_t)n_samp);
    float *sq = (float *)malloc(sizeof(float) * (size_t)n_samp);
    float *ai = (float *)malloc(sizeof(float) * (size_t)n_syms);
    float *aq = (float *)malloc(sizeof(float) * (size_t)n_syms);
    if (!taps || !si || !sq || !ai || !aq) {
        free(taps);
        free(si);
        free(sq);
        free(ai);
        free(aq);
        return 0;
    }
    make_rrc(taps, 0.2);

    double phase = 0.0;
    for (int k = 0; k < n_syms; k++) {
        phase += (double)syms[k] * M_PI / 4.0;
        ai[k] = (float)cos(phase);
        aq[k] = (float)sin(phase);
    }

    /* The upsampled symbol train has one nonzero sample per symbol, so only
     * the nine symbols in the RRC span can contribute to an output sample. */
    for (int t = 0; t < n_samp; t++) {
        int centre = t / SPS_IN;
        double acc_i = 0.0;
        double acc_q = 0.0;
        for (int d = -RRC_HALF_SYM; d <= RRC_HALF_SYM; d++) {
            int k = centre + d;
            if (k < 0 || k >= n_syms) continue;
            int tap = t - k * SPS_IN + RRC_LEN / 2;
            if (tap < 0 || tap >= RRC_LEN) continue;
            acc_i += taps[tap] * ai[k];
            acc_q += taps[tap] * aq[k];
        }
        si[t] = (float)acc_i;
        sq[t] = (float)acc_q;
    }

    if (ch && ch->echo_amp > 0.0f) {
        int delay = (int)(ch->echo_delay_sym * (float)SPS_IN + 0.5f);
        if (delay < 0) delay = 0;
        float cr = cosf(ch->echo_rot_rad);
        float sr = sinf(ch->echo_rot_rad);
        for (int t = n_samp - 1; t >= delay; t--) {
            float ei = si[t - delay];
            float eq = sq[t - delay];
            si[t] += ch->echo_amp * (ei * cr - eq * sr);
            sq[t] += ch->echo_amp * (ei * sr + eq * cr);
        }
    }

    if (ch && ch->freq_off_hz != 0.0f) {
        double step = 2.0 * M_PI * (double)ch->freq_off_hz / (double)SR_IN;
        double p = 0.0;
        for (int t = 0; t < n_samp; t++) {
            float c = (float)cos(p);
            float s = (float)sin(p);
            float x = si[t] * c - sq[t] * s;
            float y = si[t] * s + sq[t] * c;
            si[t] = x;
            sq[t] = y;
            p += step;
        }
    }

    if (ch && ch->noise_sd > 0.0f && rng) {
        float gain = ch->noise_sd / 0.289f;
        for (int t = 0; t < n_samp; t++) {
            si[t] += gain * ls_rng_noise(rng);
            sq[t] += gain * ls_rng_noise(rng);
        }
    }

    float peak = 1e-9f;
    for (int t = 0; t < n_samp; t++) {
        float m = fabsf(si[t]);
        if (m > peak) peak = m;
        m = fabsf(sq[t]);
        if (m > peak) peak = m;
    }
    float scale = 110.0f / peak;
    int w = 0;
    for (int t = 0; t < n_samp; t++) {
        int x = (int)(si[t] * scale) + 128;
        int y = (int)(sq[t] * scale) + 128;
        if (x < 0) x = 0; else if (x > 255) x = 255;
        if (y < 0) y = 0; else if (y > 255) y = 255;
        iq[w++] = (uint8_t)x;
        iq[w++] = (uint8_t)y;
    }

    free(taps);
    free(si);
    free(sq);
    free(ai);
    free(aq);
    return w;
}

int lsm_slice(const int16_t *audio, int n_audio, float demod_gain,
              int *syms_out, int max_out)
{
    int n = 0;
    float gain = demod_gain != 0.0f ? demod_gain : 1.0f;
    for (int i = 0; i < n_audio && n < max_out; i += DSP_SPS) {
        float v = (float)audio[i] / gain;
        syms_out[n++] = v > 2.0f ? 3 : v > 0.0f ? 1 : v > -2.0f ? -1 : -3;
    }
    return n;
}
