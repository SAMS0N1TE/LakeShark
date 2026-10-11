/* Shared synthetic TSBK replay fixtures from test_p25_tsbk.c. */
#pragma once
static const uint8_t constellation[4][4] = {
    { 0x2, 0xc, 0x1, 0xf },
    { 0xe, 0x0, 0xd, 0x3 },
    { 0x9, 0x7, 0xa, 0x4 },
    { 0x5, 0xb, 0x6, 0x8 },
};

static void trellis_encode(const uint8_t decoded[P25_TSBK_DECODED_DIBITS],
                           uint8_t encoded[P25_TSBK_ENCODED_DIBITS])
{
    uint8_t state = 0;
    for (size_t i = 0; i <= P25_TSBK_DECODED_DIBITS; i++) {
        uint8_t next = i == P25_TSBK_DECODED_DIBITS ? 0 : decoded[i];
        uint8_t word = constellation[state][next];
        encoded[i * 2] = word >> 2;
        encoded[i * 2 + 1] = word & 3;
        state = next;
    }
}

static void interleave(const uint8_t input[P25_TSBK_ENCODED_DIBITS],
                       uint8_t wire[P25_TSBK_ENCODED_DIBITS])
{
    size_t out = 0;
    for (size_t start = 0; start < 8; start += 2) {
        size_t end = start == 0 ? 98 : 96;
        for (size_t i = start; i < end; i += 8) {
            wire[out++] = input[i];
            wire[out++] = input[i + 1];
        }
    }
    LS_EQ_UINT(out, P25_TSBK_ENCODED_DIBITS);
}

static void encode_block(const uint8_t block[P25_TSBK_BYTES],
                         uint8_t wire[P25_TSBK_ENCODED_DIBITS])
{
    uint8_t decoded[P25_TSBK_DECODED_DIBITS];
    uint8_t encoded[P25_TSBK_ENCODED_DIBITS];
    for (size_t i = 0; i < P25_TSBK_DECODED_DIBITS; i++)
        decoded[i] = (block[i / 4] >> (6 - (i % 4) * 2)) & 3;
    trellis_encode(decoded, encoded);
    interleave(encoded, wire);
}

static void set_bits(uint8_t block[P25_TSBK_BYTES], unsigned int first,
                     unsigned int count, uint32_t value)
{
    for (unsigned int i = 0; i < count; i++) {
        unsigned int bit = first + i;
        uint8_t mask = (uint8_t)(1u << (7 - bit % 8));
        if ((value >> (count - 1 - i)) & 1u)
            block[bit / 8] |= mask;
        else
            block[bit / 8] &= (uint8_t)~mask;
    }
}

static void add_crc(uint8_t block[P25_TSBK_BYTES])
{
    uint16_t crc;
    block[10] = 0;
    block[11] = 0;
    crc = p25_tsbk_crc16(block, P25_TSBK_BYTES);
    block[10] = crc >> 8;
    block[11] = crc & 0xff;
    LS_EQ_UINT(p25_tsbk_crc16(block, P25_TSBK_BYTES), 0);
}

static void make_tdma_iden(uint8_t block[P25_TSBK_BYTES], uint8_t identifier)
{
    memset(block, 0, P25_TSBK_BYTES);
    block[0] = 0x80 | 0x33;
    set_bits(block, 16, 4, identifier);
    set_bits(block, 20, 4, 3);          /* 2-slot, 12.5 kHz, half-rate voice */
    set_bits(block, 24, 1, 0);          /* subscriber transmit below receive */
    set_bits(block, 25, 13, 3600);      /* 3600 * 12.5 kHz = 45 MHz */
    set_bits(block, 38, 10, 100);       /* spacing 12,500 Hz */
    set_bits(block, 48, 32, 170202500); /* base 851.0125 MHz, units of 5 Hz */
    add_crc(block);
}

static void make_group_grant(uint8_t block[P25_TSBK_BYTES], uint16_t channel,
                             uint16_t talkgroup)
{
    memset(block, 0, P25_TSBK_BYTES);
    block[0] = 0x80 | 0x00;
    set_bits(block, 24, 16, channel);
    set_bits(block, 40, 16, talkgroup);
    set_bits(block, 56, 24, 0x123456);
    add_crc(block);
}


