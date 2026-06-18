
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
} fm_dsp_t;

void fm_dsp_init(fm_dsp_t *s);

int  fm_demod_iq(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                 float *demod_out, int max);

int  fm_demod_to_audio(fm_dsp_t *s, const float *demod, int n,
                       int16_t *pcm16k, int max);

int  fm_demod_wide(fm_dsp_t *s, const uint8_t *iq, int iq_len,
                   int16_t *pcm16k, int max);

float fm_iq_rms(const uint8_t *iq, int iq_len);

#ifdef __cplusplus
}
#endif

#endif
