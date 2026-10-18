
#ifndef FM_DSP_H
#define FM_DSP_H

#include <stdint.h>
#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FM_DSP_CHUNK   4096

typedef struct {

    int16_t lowpassed[FM_DSP_CHUNK];
    int16_t result[FM_DSP_CHUNK / 2 + 4];
    int     lp_len;
    int     result_len;

    int16_t lp_i_hist[6][6];
    int16_t lp_q_hist[6][6];

    int     pre_r, pre_j;

    int     deemph_a;
    int     deemph_avg;

    int     now_lpr;
    int     prev_lpr_index;
    int     rate_out;
    int     rate_out2;

    float   f_deemph;
    float   f_acc;
    int     f_n;

    float   iq_peak;
    float   iq_block_peak;
    /* RMS of the demodulated stream above the voice band, which is what an
       FM noise squelch actually measures: with no carrier the discriminator
       output is loud hiss, and a carrier quietens it. Carrier level alone
       cannot do this job on a wideband IQ block, where a 12.5 kHz channel
       barely moves the peak. */
    float   demod_noise;
    float   noise_hp;
    int     squelch_samples;
    int     squelch_settle_samples;
    /* Hysteresis. Opening and closing on the same number makes one noisy
       block shut the audio and cost a whole qualify window to get back, and
       every one of those splices a silence into the speaker. */
    bool    squelch_is_open;
    int     squelch_release_samples;
    float   am_dc;
    /* fm_demod_env's own channel filter: the 256k -> 32k binomial chain the
       other paths use droops unevenly around an off-tune carrier, which bends
       ACARS's AM envelope enough to break frames. */
#define FM_ENV_TAPS 64
    float   env_i[FM_ENV_TAPS], env_q[FM_ENV_TAPS];
    int     env_pos, env_dec;
} fm_dsp_t;

void fm_dsp_init(fm_dsp_t *s);
int fm_nfm_squelch(fm_dsp_t *s, int threshold_pct, int samples);
/* The demod-noise level a squelch setting opens below (0 = always open).
   Inline so screens can draw the gate mark without linking the DSP. */
static inline float fm_nfm_squelch_gate(int threshold_pct)
{
    if (threshold_pct <= 0)  return 99.0f;
    if (threshold_pct > 100) threshold_pct = 100;
    if (threshold_pct <= 30) return 1.25f - 0.55f * (float)threshold_pct / 30.0f;
    return (float)(100 - threshold_pct) * 0.010f;
}

int  fm_demod_iq(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                 float *demod_out, int max);

int  fm_demod_to_audio(fm_dsp_t *s, const float *demod, int n,
                       int16_t *pcm16k, int max);

int  fm_demod_wide(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                   int16_t *pcm16k, int max);

/* The AM envelope at FM_DEMOD_RATE as a float, carrier removed and divided
   by it (100% modulation swings +-1). ACARS's MSK tones ride an AM carrier,
   so this, not the discriminator, is what its decoder reads. */
int fm_demod_env(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                 float *out, int max);
int fm_demod_am(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                int16_t *pcm16k, int max);

float fm_iq_rms(const uint8_t *iq, int iq_len);

#ifdef __cplusplus
}
#endif

#endif
