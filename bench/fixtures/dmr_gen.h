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

/* The 72 LC bits in nine octets with RS(12,9) parity masked for data type
 * 1 or 2 - a full LC as it sits in the BPTC block. */
void dmr_gen_lc_signed(const uint8_t lc9[9], uint8_t data_type, uint8_t bits96[12]);

/* A data-sync burst carrying any 96-bit block: BPTC, Slot Type and sync. */
void dmr_gen_data_burst(uint8_t out_burst[33], const uint8_t sync_pattern[6],
                        uint8_t colour_code, uint8_t data_type,
                        const uint8_t block[12]);

/* A voice burst. 216 payload bits stand in for AMBE (a fixed pseudo-random
 * fill, seeded by `fill`); the middle 48 bits are the sync for burst A
 * (`sync_pattern` non-NULL) or EMB and one embedded fragment for B-F. */
void dmr_gen_voice_burst(uint8_t out_burst[33], const uint8_t *sync_pattern,
                         uint16_t emb, uint32_t fragment, unsigned fill);

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_GEN_H */
