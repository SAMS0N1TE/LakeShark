#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H

#include <stdint.h>

#define DSP_SAMPLE_RATE     240000
#define DSP_PRE_DECIM       1
#define DSP_DECIMATION      5
#define DSP_AUDIO_RATE      (DSP_SAMPLE_RATE / DSP_PRE_DECIM / DSP_DECIMATION)
#define DSP_FIR_TAPS        31
#define DSP_SPS             10
#define DSP_BAUD            4800

typedef enum {
    DEMOD_C4FM          = 0,
    DEMOD_CQPSK         = 1,
    DEMOD_DIFF_4FSK     = 2,
    DEMOD_FSK4_TRACKING = 3,
} demod_mode_t;

typedef struct {

    float fir_coeffs[DSP_FIR_TAPS];
    float fir_buf_i[DSP_FIR_TAPS];
    float fir_buf_q[DSP_FIR_TAPS];
    int   fir_idx;
    int   decim_count;

    demod_mode_t mode;
    float demod_gain;

    float prev_i;
    float prev_q;

    float dc_avg_i;
    float dc_avg_q;
    float dc_alpha;

    float  g_clock;
    float  g_period;
    float  g_mu;
    float  g_gain_mu;
    float  g_gain_omega;
    float  g_omega_rel;
    float  g_di[3];
    float  g_dq[3];
    int    g_sample_idx;
    int    g_half;

    float  c_phase;
    float  c_freq;
    float  c_alpha;
    float  c_beta;

    float  diff_prev_i;
    float  diff_prev_q;

    int    cqpsk_polarity;

    float  agc_gain;
    float  agc_alpha;
    float  agc_ref;

#define RRC_SYM_TAPS 51
    float  rrc_buf[51];
    int    rrc_idx;

    float  rrc_i_buf[RRC_SYM_TAPS];
    float  rrc_q_buf[RRC_SYM_TAPS];
    int    rrc_iq_idx;

#define DIFF_DELAY DSP_SPS
    float  diff_ring_i[DIFF_DELAY];
    float  diff_ring_q[DIFF_DELAY];
    int    diff_ring_idx;
    int    diff_ring_filled;

    float  eq_pll;
    float  eq_gain;

    float  diff_output_scale;

    float  cic_acc_i;
    float  cic_acc_q;
    int    cic_count;

    float fsk4_input_scale;
    double ft_symbol_clock;
    double ft_symbol_time;
    double ft_symbol_spread;
    double ft_fine_freq;
    double ft_coarse_freq;
    float  ft_history[8];
    int    ft_history_idx;

    float  ft_prev_i;
    float  ft_prev_q;

    double nco_phase;
    double nco_step_rad;
    double nco_dc_avg;

#define RRC_FSK4_TAPS 21
    float  rrc_fsk4_buf[RRC_FSK4_TAPS];
    int    rrc_fsk4_idx;

    float  c4fm_dc_avg;

} dsp_state_t;

void dsp_init(dsp_state_t *s);
void dsp_set_mode(dsp_state_t *s, demod_mode_t mode);
void dsp_set_gain(dsp_state_t *s, float gain);
void dsp_set_costas_alpha(dsp_state_t *s, float alpha);
void dsp_flip_polarity(dsp_state_t *s);
int  dsp_process_iq(dsp_state_t *s, const uint8_t *iq_data, int iq_len,
                    int16_t *audio_out, int audio_max);

float dsp_fsk4_get_fine_freq_hz(const dsp_state_t *s);
void  dsp_fsk4_clear_fine_freq(dsp_state_t *s);
void  dsp_fsk4_reset_tracker(dsp_state_t *s);

#endif
