#ifndef P25_TSBK_H
#define P25_TSBK_H

#include <stddef.h>
#include <stdint.h>

#include "dsd.h"

#define P25_TSBK_ENCODED_DIBITS 98
#define P25_TSBK_DECODED_DIBITS 48
#define P25_TSBK_BYTES 12

void p25_tsbk_deinterleave(const uint8_t input[P25_TSBK_ENCODED_DIBITS],
                           uint8_t output[P25_TSBK_ENCODED_DIBITS]);
int p25_tsbk_trellis_decode(
    const uint8_t encoded[P25_TSBK_ENCODED_DIBITS],
    uint8_t decoded[P25_TSBK_DECODED_DIBITS]);
uint16_t p25_tsbk_crc16(const uint8_t *data, size_t length);
int p25_tsbk_parse(dsd_state *state,
                   const uint8_t block[P25_TSBK_BYTES]);
/* Explicit control/system change; counters survive, site evidence does not. */
void p25_tsbk_reset_system(dsd_state *state);
unsigned int p25_tsbk_process_tsdu(dsd_state *state,
                                   const uint8_t *wire_dibits,
                                   size_t wire_count);

#endif
