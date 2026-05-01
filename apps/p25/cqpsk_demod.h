#ifndef CQPSK_DEMOD_H
#define CQPSK_DEMOD_H

#include <stdint.h>

#define CQPSK_IF_RATE       24000
#define CQPSK_SYMBOL_RATE   4800
#define CQPSK_SPS           5
#define CQPSK_RRC_TAPS      41

typedef struct {
    /* Stage 1 anti-alias LPF + decimate x5 (240kHz -> 48kHz) */
    float lpf1_buf_i[31];
    float lpf1_buf_q[31];
    int   lpf1_idx;
    int   decim1_count;

    /* Stage 2 decimate x2 (48kHz -> 24kHz) */
    int   decim2_count;

    /* RRC matched filter (I and Q) at 24kHz */
    float rrc_buf_i[CQPSK_RRC_TAPS];
    float rrc_buf_q[CQPSK_RRC_TAPS];
    int   rrc_idx;

    /* Costas loop carrier recovery */
    float costas_freq;
    float costas_phase;
    float costas_alpha;
    float costas_beta;
    float tracking_feedback;
    float tracking_threshold;

    /* Gardner symbol timing recovery */
    float sym_period;
    float sym_clock;
    float sym_gain;
    float prev_i;
    float prev_q;
    float mid_i;
    float mid_q;
    int   sym_count;

    /* Differential decode */
    float last_sym_i;
    float last_sym_q;
    int   constellation_rot;  /* 0=0°, 1=90°, 2=180°, 3=270° */

    /* Output */
    int16_t *out_buf;
    int      out_write;
    int      out_size;
} cqpsk_state_t;

void cqpsk_init(cqpsk_state_t *s);
void cqpsk_set_tracking(cqpsk_state_t *s, float feedback, float threshold);
int  cqpsk_process_iq(cqpsk_state_t *s, const uint8_t *iq_data, int iq_len,
                      int16_t *audio_out, int audio_max);

#endif
