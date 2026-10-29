/* A DMR burst as the RTL would hand it over: unsigned 8-bit interleaved IQ.
 *
 * DMR is 4FSK at 4800 symbols per second with deviations of +/-1944 Hz for
 * the outer symbols and +/-648 Hz for the inner ones (ETSI TS 102 361-1
 * 5.1.1), which is the same modulation and rate the C4FM P25 path already
 * demodulates. Rendering a burst this way drives the real receive chain -
 * discriminator, timing, slicer - instead of handing the decoder symbols it
 * was never going to receive that cleanly.
 */
#ifndef LS_DMR_IQ_GEN_H
#define LS_DMR_IQ_GEN_H

#include <stdint.h>

#include "dmr.h"
#include "ls_test.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DMR_IQ_RAMP = 0,     /* raised-cosine step confined to 30% of a symbol */
    DMR_IQ_RRC,          /* root raised cosine, alpha 0.2, as ETSI 5.1.1   */
} dmr_iq_shape_t;

typedef struct {
    float noise_sd;      /* per-component, in LSB of the 8-bit sample */
    float freq_off_hz;   /* carrier error the receiver has to live with */
    /* Only read by dmr_iq_render_stream(). */
    dmr_iq_shape_t shape;
    int   invert;        /* transmit the spectrum the other way round     */
    float clock_ppm;     /* transmitter symbol clock error                 */
    float cn_db;         /* carrier to noise in 12.5 kHz; overrides noise_sd
                            when non-zero                                  */
    float start_sym;     /* first symbol's offset from sample 0, symbols   */
} dmr_iq_channel_t;

/* Render `bits` (MSB first in bits[0]) as 4FSK and append the IQ to `iq`.
 * Returns bytes written, or 0 if the buffer is too small. */
int dmr_iq_render_bits(const uint8_t *bits, int n_bits,
                       const dmr_iq_channel_t *ch, ls_rng_t *rng,
                       uint8_t *iq, int iq_max);

/* Idle carrier, for the gap between bursts and to let the receiver's level
 * trackers settle before a burst arrives. */
int dmr_iq_render_idle(int symbols, const dmr_iq_channel_t *ch,
                       ls_rng_t *rng, uint8_t *iq, int iq_max);

/* Symbol-stream building. Each appends deviations in Hz to `dev` at `at` and
 * returns the new length, or -1 if `max` would be exceeded. */
int dmr_iq_dev_bits(float *dev, int at, int max, const uint8_t *bits, int n_bits);
int dmr_iq_dev_idle(float *dev, int at, int max, int symbols);
/* Random dibits, what a payload looks like to the timing loop. */
int dmr_iq_dev_random(float *dev, int at, int max, int symbols, ls_rng_t *rng);
/* No carrier at all - the gap a mobile leaves between its bursts, where
 * the discriminator sees only noise. */
#define DMR_IQ_OFF 1.0e9f
int dmr_iq_dev_off(float *dev, int at, int max, int symbols);

/* Render a whole symbol stream as float IQ at 240 kHz, interleaved, nominal
 * amplitude 0.7 - what dsp_process_float_iq() takes, which is the LR2021
 * path. Shaping is filtered across the whole stream, so the filter tails of
 * one burst run into the next the way they do on air. Returns complex
 * samples written, or 0 if `max_pairs` is too small. */
int dmr_iq_render_stream(const float *dev, int n_sym,
                         const dmr_iq_channel_t *ch, ls_rng_t *rng,
                         float *iq, int max_pairs);

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_IQ_GEN_H */
