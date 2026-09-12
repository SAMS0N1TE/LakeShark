/* Deterministic P25 LSM / CQPSK transmitter and channel for host tests.
 *
 * generate the unsigned 8-bit interleaved IQ handed to the live
 * CQPSK pipeline: 240 kHz pi/4-DQPSK, root-raised-cosine shaped at the P25
 * excess bandwidth of 0.2, with optional noise, carrier offset and echo.
 */
#ifndef LS_LSM_GEN_H
#define LS_LSM_GEN_H

#include <stdint.h>

#include "ls_test.h"

#define LSM_SYNC_LEN 24
extern const int LSM_SYNC[LSM_SYNC_LEN];

typedef struct {
    float echo_amp;
    float echo_delay_sym;
    float echo_rot_rad;
    float noise_sd;
    float freq_off_hz;
} lsm_channel_t;

void lsm_channel_clean(lsm_channel_t *ch);

/* Render symbols from {-3,-1,+1,+3} as pi/4-DQPSK. Returns bytes written,
 * or zero when iq_max is too small or a fixture allocation fails. */
int lsm_render_iq(const int *syms, int n_syms, const lsm_channel_t *ch,
                  ls_rng_t *rng, uint8_t *iq, int iq_max);

/* The CQPSK path repeats every recovered symbol DSP_SPS times for DSD. */
int lsm_slice(const int16_t *audio, int n_audio, float demod_gain,
              int *syms_out, int max_out);

#endif
