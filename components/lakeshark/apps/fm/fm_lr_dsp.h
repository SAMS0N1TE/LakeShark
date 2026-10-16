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
    float am_dc, am_acc;          /* AM: carrier level and the 32k -> 16k pair */
    unsigned pos, decim, phase, am_n;
    bool have;
} fm_lr_dsp_t;
/* Modes the LR2021's I/Q carries: narrowband FM listening, AM voice (airband),
   and the decoders that read the FM discriminator: pagers, APRS, SAME and
   ACARS. (The chip's own FSK engine heard nothing of a +61 dB pager at
   152.6 MHz that this path decodes.) */
static inline bool fm_lr_mode(int mode) {
    return mode == FM_MODE_LISTEN || mode == FM_MODE_AM || mode == FM_MODE_APRS ||
           mode == FM_MODE_SAME || mode == FM_MODE_ACARS ||
           mode == FM_MODE_POCSAG || mode == FM_MODE_FLEX;
}
static inline bool fm_lr_supported(uint32_t hz, int mode) {
    /* The LR2021's measured receive range (LS_LORA_RX_EXT_*, ls_lora.h). */
    return fm_lr_mode(mode) && hz >= 104000000u && hz <= 2740000000u;
}
void fm_lr_reset(fm_lr_dsp_t *s);
void fm_lr_init(fm_lr_dsp_t *s, unsigned channel_hz);
int fm_lr_demod(fm_lr_dsp_t *s, fm_dsp_t *fm, const uint8_t *iq,
                size_t bytes, float *out, int capacity);
/* AM envelope through the same channel filter, levelled like fm_demod_am:
   16 kHz PCM straight out. Returns samples written. */
int fm_lr_demod_am(fm_lr_dsp_t *s, fm_dsp_t *fm, const uint8_t *iq,
                   size_t bytes, int16_t *pcm16k, int capacity);
/* The AM envelope at 32 kHz as a float, carrier removed and divided by it
   (100% modulation swings +-1): what ACARS's MSK tones ride on. */
int fm_lr_demod_env(fm_lr_dsp_t *s, fm_dsp_t *fm, const uint8_t *iq,
                    size_t bytes, float *out32k, int capacity);
#endif
