#include "dmr_gen.h"

#include <string.h>

static void wr_bit(uint8_t *buf, unsigned int idx, int v)
{
    uint8_t mask = (uint8_t)(1u << (7u - (idx & 7u)));
    if (v) buf[idx >> 3] |= mask;
    else   buf[idx >> 3] &= (uint8_t)~mask;
}

static int rd_bit(const uint8_t *buf, unsigned int idx)
{
    return (buf[idx >> 3] >> (7u - (idx & 7u))) & 1u;
}

static void set_bits_msb(uint8_t *buf, unsigned first, unsigned count, uint32_t value)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned bit = first + i;
        uint8_t mask = (uint8_t)(1u << (7u - (bit & 7u)));
        if ((value >> (count - 1 - i)) & 1u) buf[bit >> 3] |= mask;
        else                                  buf[bit >> 3] &= (uint8_t)~mask;
    }
}

void dmr_gen_lc_bits(const dmr_lc_t *lc, uint8_t bits96[12])
{
    memset(bits96, 0, 12);
    if (!lc) return;
    set_bits_msb(bits96,  0, 1, lc->protect_flag & 1u);
    /* bit 1 reserved */
    set_bits_msb(bits96,  2, 6, lc->flco & 0x3fu);
    set_bits_msb(bits96,  8, 8, lc->fid);
    set_bits_msb(bits96, 16, 8, lc->service_options);
    set_bits_msb(bits96, 24, 24, lc->destination & 0xffffffu);
    set_bits_msb(bits96, 48, 24, lc->source      & 0xffffffu);
    /* Bits 72..95 (RS parity) intentionally left zero. */
}

void dmr_gen_burst(uint8_t out_burst[33],
                   const uint8_t sync_pattern[6],
                   uint8_t colour_code,
                   uint8_t data_type,
                   const dmr_lc_t *lc)
{
    uint8_t lc_bits[12];
    uint8_t coded[DMR_BPTC_BITS / 8 + 1];

    memset(out_burst, 0, 33);

    dmr_gen_lc_bits(lc, lc_bits);
    dmr_bptc_encode(lc_bits, coded);

    /* Split the 196-bit BPTC block back into info1 (98 bits) + info2. */
    for (unsigned i = 0; i < 98; i++)
        wr_bit(out_burst, i, rd_bit(coded, i));
    for (unsigned i = 0; i < 98; i++)
        wr_bit(out_burst, 166u + i, rd_bit(coded, 98u + i));

    /* Slot Type: encode the 8-bit data field {colour code, data type} as a
     * Golay(20,8,7) codeword per ETSI TS 102 361-1 §B.3.4. The 20-bit
     * codeword is split around the sync: the top 10 bits go to burst
     * positions 98..107, the bottom 10 bits to 156..165. */
    uint8_t data8 = (uint8_t)(((colour_code & 0xfu) << 4) | (data_type & 0xfu));
    uint32_t st_cw = dmr_slot_type_encode(data8);
    for (unsigned i = 0; i < 10; i++)
        wr_bit(out_burst, 98u + i, (int)((st_cw >> (19u - i)) & 1u));
    for (unsigned i = 0; i < 10; i++)
        wr_bit(out_burst, 156u + i, (int)((st_cw >> (9u - i)) & 1u));

    /* Sync (48 bits) starting at 108. */
    for (unsigned i = 0; i < 48; i++) {
        int bit = (sync_pattern[i / 8] >> (7 - (i % 8))) & 1;
        wr_bit(out_burst, 108u + i, bit);
    }
}
