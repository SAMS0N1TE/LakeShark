#include "dmr.h"

#include <string.h>

/* Full Link Control PDU - ETSI TS 102 361-2 §7.1.1 (Voice LC Header).
 *
 * 72-bit LC content packed MSB-first:
 *   [0]     PF (protect flag)                       1 bit
 *   [1]     Reserved                                1 bit
 *   [2..7]  FLCO (Full Link Control Opcode)         6 bits
 *   [8..15] FID (Feature set ID)                    8 bits
 *   [16..23] Service options                        8 bits
 *   [24..47] Destination address (TG or radio ID)  24 bits
 *   [48..71] Source address (radio ID)             24 bits
 *
 * The 96-bit BPTC block wraps these 72 bits followed by 24 bits of RS(12,9)
 * parity; RS decoding of that trailer is left as a follow-up. */

static uint32_t get_bits_msb(const uint8_t *buf, unsigned first, unsigned count)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned bit = first + i;
        v = (v << 1) | ((buf[bit >> 3] >> (7u - (bit & 7u))) & 1u);
    }
    return v;
}

void dmr_lc_parse(const uint8_t bits96[12], dmr_lc_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!bits96) return;
    out->protect_flag    = (uint8_t)get_bits_msb(bits96, 0, 1);
    /* bit 1 reserved, skipped */
    out->flco            = (uint8_t)get_bits_msb(bits96, 2, 6);
    out->fid             = (uint8_t)get_bits_msb(bits96, 8, 8);
    out->service_options = (uint8_t)get_bits_msb(bits96, 16, 8);
    out->destination     = get_bits_msb(bits96, 24, 24);
    out->source          = get_bits_msb(bits96, 48, 24);
}
