#ifndef LS_DMR_GEN_H
#define LS_DMR_GEN_H

#include <stddef.h>
#include <stdint.h>

#include "dmr.h"

#ifdef __cplusplus
extern "C" {
#endif

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
