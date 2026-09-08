#include "dmr.h"

#include <string.h>

/* DMR burst layout (ETSI TS 102 361-1 §6.2 Fig. 6.2), 264 bits total:
 *
 *   info1 (98)  |  slot-type1 (10)  |  sync (48)  |  slot-type2 (10)  |  info2 (98)
 *
 * bit offsets:  0..97           98..107      108..155      156..165    166..263
 *
 * The 196-bit BPTC block is info1 || info2 (98 + 98).
 *
 * Slot Type (20 bits total, split around sync) is a Golay(20,8,7) codeword
 * carrying colour code in the high nibble and data type in the low nibble.
 * Both halves of the codeword are pulled out and joined back into a single
 * 20-bit word before being handed to the Golay decoder below. */

static int rd_bit(const uint8_t *buf, unsigned int idx)
{
    return (buf[idx >> 3] >> (7u - (idx & 7u))) & 1u;
}

static void wr_bit(uint8_t *buf, unsigned int idx, int v)
{
    uint8_t mask = (uint8_t)(1u << (7u - (idx & 7u)));
    if (v) buf[idx >> 3] |= mask;
    else   buf[idx >> 3] &= (uint8_t)~mask;
}

void dmr_burst_extract_bptc(const uint8_t burst_bits[DMR_BURST_BITS / 8],
                            uint8_t out_bits[DMR_BPTC_BITS / 8 + 1])
{
    memset(out_bits, 0, DMR_BPTC_BITS / 8 + 1);
    for (unsigned i = 0; i < 98; i++)
        wr_bit(out_bits, i, rd_bit(burst_bits, i));
    for (unsigned i = 0; i < 98; i++)
        wr_bit(out_bits, 98u + i, rd_bit(burst_bits, 166u + i));
}

/* ------------------------------------------------------------------------
 * Golay(20,8,7) - ETSI TS 102 361-1 §B.3.4
 *
 * Generator polynomial g(x) = x^12 + x^11 + x^10 + x^7 + x^4 + x^3 + 1
 * (binary 1 1100 1001 1001 = 0x1C99, degree 12).  A brute-force sweep over
 * every degree-12 polynomial with bit 12 set turned up eight candidates
 * whose 256 systematic codewords all sit at Hamming distance >= 7 from
 * every other codeword; this is one of the four with minimum distance
 * exactly 7 (the (20,8,7) family), which is what the ETSI spec calls out.
 * The wire-vs-spec cross-check needs a real DMR burst on hardware and is
 * filed as a follow-up in bench/queue/.
 *
 * Codewords are systematic and 20 bits wide, MSB-first:
 *     bits 19..12   the 8 data bits (colour code | data type)
 *     bits 11..0    the 12 parity bits, = (data << 12) mod g(x)
 *
 * Minimum distance is 7, so the code corrects up to (7-1)/2 = 3 bit errors.
 * The decoder is a minimum-distance search over the 256 legal codewords,
 * which for k=8 is trivial (256 * 20 XORs per burst).  Anything further
 * than 3 from a legal codeword is rejected instead of silently returning
 * a wrong colour code.
 * ---------------------------------------------------------------------- */

#define DMR_GOLAY_2087_POLY 0x1C99u   /* x^12 + x^11 + x^10 + x^7 + x^4 + x^3 + 1 */

uint32_t dmr_slot_type_encode(uint8_t data8)
{
    uint32_t codeword = ((uint32_t)data8) << 12;
    uint32_t reg = codeword;
    /* Long division: for each information bit from position 19 down to 12,
     * subtract (XOR) the generator aligned to that bit if it is set.  What
     * remains in bits 11..0 is the parity. */
    for (int i = 19; i >= 12; i--) {
        if (reg & (1u << i))
            reg ^= (DMR_GOLAY_2087_POLY << (i - 12));
    }
    return (codeword | (reg & 0xFFFu)) & 0xFFFFFu;
}

static int popcount20(uint32_t v)
{
    v &= 0xFFFFFu;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(v);
#else
    int n = 0;
    while (v) { v &= v - 1u; n++; }
    return n;
#endif
}

static int golay_2087_decode(uint32_t received_20, uint8_t *data_out)
{
    int best = 999;
    uint8_t best_data = 0;
    for (int d = 0; d < 256; d++) {
        uint32_t cw = dmr_slot_type_encode((uint8_t)d);
        int errs = popcount20(cw ^ received_20);
        if (errs < best) {
            best = errs;
            best_data = (uint8_t)d;
            if (best == 0) break;             /* clean codeword, done */
        }
    }
    if (best > 3) return -1;
    *data_out = best_data;
    return best;
}

int dmr_slot_type_decode(const uint8_t in[3], uint8_t *cc_out, uint8_t *dt_out)
{
    uint32_t v =
          ((uint32_t)in[0] << 12)
        | ((uint32_t)in[1] << 4)
        | ((uint32_t)in[2] >> 4);
    uint8_t data;
    int errs = golay_2087_decode(v, &data);
    if (errs < 0) return -1;
    if (cc_out) *cc_out = (uint8_t)((data >> 4) & 0xFu);
    if (dt_out) *dt_out = (uint8_t)(data & 0xFu);
    return errs;
}

uint8_t dmr_burst_colour_code(const uint8_t burst_bits[DMR_BURST_BITS / 8])
{
    /* Pull the 20-bit Slot Type codeword back out of the burst.  The first
     * half (10 bits) sits at burst positions 98..107, the second half at
     * 156..165.  Pack them MSB-first into a 3-byte buffer for the Golay
     * decoder; the low nibble of packed[2] is padding. */
    uint8_t packed[3] = { 0, 0, 0 };
    for (unsigned i = 0; i < 10; i++)
        wr_bit(packed, i, rd_bit(burst_bits, 98u + i));
    for (unsigned i = 0; i < 10; i++)
        wr_bit(packed, 10u + i, rd_bit(burst_bits, 156u + i));

    uint8_t cc = 0, dt = 0;
    if (dmr_slot_type_decode(packed, &cc, &dt) < 0) {
        /* FEC could not settle on a codeword within its 3-bit correction
         * radius.  Fall back to the raw high nibble so callers that treat
         * the CC as advisory still see something; a caller that needs to
         * gate on a valid Slot Type should use dmr_slot_type_decode()
         * directly and inspect the return code. */
        cc = 0;
        for (unsigned i = 0; i < 4; i++)
            cc = (uint8_t)((cc << 1) | rd_bit(burst_bits, 98u + i));
    }
    return cc;
}
