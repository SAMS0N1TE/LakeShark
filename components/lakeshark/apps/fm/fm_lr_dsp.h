#ifndef FM_LR_DSP_H
#define FM_LR_DSP_H
#include "fm_dsp.h"
#include <stddef.h>
/* 125 kS/s signed LE IQ -> channel FIR /4 -> polar discriminator -> 32k.
 * Then use the existing FM squelch, tone and 16k de-emphasis/audio stages.
 * Caller owns this state in PSRAM. No allocation or waits in process. */
#define FM_LR_TAPS 81
typedef struct {
    float taps[FM_LR_TAPS], noise_scale;
    float i[FM_LR_TAPS], q[FM_LR_TAPS];
    float noise_power;
    float prev_i, prev_q, prev_d;
    unsigned pos, decim, phase;
    bool have;
} fm_lr_dsp_t;
static inline bool fm_lr_supported(uint32_t hz, int mode) {
    return mode == FM_MODE_LISTEN && hz >= 150000000u && hz <= 960000000u;
}
void fm_lr_reset(fm_lr_dsp_t *s);
void fm_lr_init(fm_lr_dsp_t *s, unsigned channel_hz);
int fm_lr_demod(fm_lr_dsp_t *s, fm_dsp_t *fm, const uint8_t *iq,
                size_t bytes, float *out, int capacity);
#endif
