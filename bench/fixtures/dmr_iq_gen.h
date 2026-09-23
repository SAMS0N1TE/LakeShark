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

typedef struct {
    float noise_sd;      /* per-component, in LSB of the 8-bit sample */
    float freq_off_hz;   /* carrier error the receiver has to live with */
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

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_IQ_GEN_H */
