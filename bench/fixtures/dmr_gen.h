#ifndef LS_DMR_GEN_H
#define LS_DMR_GEN_H

#include <stddef.h>
#include <stdint.h>

#include "dmr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Assemble a synthetic DMR burst (264 bits, packed MSB-first, 33 bytes).
 *
 *   sync_pattern: 48-bit sync bytes (6 bytes) to place in the centre. Callers
 *                 in the test suite use the ETSI class patterns from
 *                 dmr_sync_detect() so the same test can round-trip through
 *                 both the sync detector and the burst extractor.
 *   colour_code:  0..15, placed in the top nibble of the slot-type field.
 *   data_type:    0..15, placed in the low nibble.
 *   lc:           Voice LC Header to encode into the BPTC(196,96) payload;
 *                 pass NULL for a zeroed payload.
 *
 * The generator emits a valid, error-free burst; tests inject errors
 * afterwards to exercise the correction paths. */
void dmr_gen_burst(uint8_t out_burst[33],
                   const uint8_t sync_pattern[6],
                   uint8_t colour_code,
                   uint8_t data_type,
                   const dmr_lc_t *lc);

/* Encode a Voice LC Header per ETSI TS 102 361-2 §7.1.1 into the top 72 bits
 * of the 96-bit LC block; the remaining 24 bits (RS parity) are zeroed. */
void dmr_gen_lc_bits(const dmr_lc_t *lc, uint8_t bits96[12]);

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_GEN_H */
