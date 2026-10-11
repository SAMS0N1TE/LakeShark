#ifndef P25_LR_FRONTEND_H
#define P25_LR_FRONTEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define P25_LR_FRONTEND_TAPS 65
/* Experimental, allocation-free IQ16LE 125 kHz -> float IQ 240 kHz bridge.
 * Mixes center_hz to DC, optionally applies a 6.5 kHz channel filter, and
 * preserves sample phase/history across arbitrary whole-pair chunk boundaries.
 * Floats use IQ16 / 32768 units. No per-chunk normalization or U8 quantization.
 * Single-owner state. Call reset after EVERY discontinuity/retune/rejected block,
 * and reset downstream acquisition too; missing samples cannot be reconstructed.
 * The FIR has 32 input samples (256 us) group delay. No end padding is emitted.
 * Used by the queued live LR2021 receiver; filter is optional. */
typedef struct {
    float taps[P25_LR_FRONTEND_TAPS];
    float history[P25_LR_FRONTEND_TAPS][2];
    float previous[2];
    double angle, step;
    unsigned index, phase;
    bool have_previous, filtered;
} p25_lr_frontend_t;

bool p25_lr_frontend_init(p25_lr_frontend_t *s, float center_hz, bool filtered);
void p25_lr_frontend_reset(p25_lr_frontend_t *s);
/* Returns required output PAIRS, or SIZE_MAX for invalid input. */
size_t p25_lr_frontend_needed(const p25_lr_frontend_t *s, size_t input_bytes);
/* Returns output PAIRS. SIZE_MAX means invalid input/capacity; state and output
 * are unchanged on failure. Input/output must not overlap; capacity is pairs.
 * A zero return can mean an empty input. Max input per call is 125256 bytes. */
size_t p25_lr_frontend_process(p25_lr_frontend_t *s, const uint8_t *iq,
                             size_t bytes, float *out, size_t capacity_pairs);
#endif
