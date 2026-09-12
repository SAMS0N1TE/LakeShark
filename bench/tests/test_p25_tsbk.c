/* LS_TEST_SOURCES: ${APP}/p25/p25_tsbk.c ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c ${APP}/p25/p25_controls.c */
/* P25 Phase 1 TSBK deinterleave, trellis, CRC, and selected messages. */

#include "ls_test.h"
#include "p25_tsbk.h"
#include "grant_follower.h"

#include <string.h>

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

LS_CASE(iden_up_tdma_populates_the_table_with_a_slot_count)
{
    dsd_state state = { 0 };
    uint8_t iden[12];

    /* IDEN_UP_TDMA (opcode 0x33), derived from boatbod/op25
     * gr-op25_repeater/apps/trunking.py's opcode 0x33 field shifts:
     *   bits 16-19  Identifier       (4 bits)
     *   bits 20-23  Channel type     (4 bits; type 3 is 2-slot TDMA,
     *                                 12.5 kHz, half-rate voice)
     *   bit  24     TxOffset sign    (0 = negative, 1 = positive)
     *   bits 25-37  TxOffset value   (13 bits, units of channel spacing)
     *   bits 38-47  Channel spacing  (10 bits, units of 125 Hz)
     *   bits 48-79  Base frequency   (32 bits, units of 5 Hz)
     * OP25's slots_per_carrier table maps channel type 3 to two slots. */
    make_tdma_iden(iden, 2);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_CHECK(state.p25_iden_table[2].valid);
    LS_EQ_UINT(state.p25_iden_table[2].base_hz, 851012500ull);
    LS_EQ_UINT(state.p25_iden_table[2].spacing_hz, 12500u);
    LS_EQ_UINT(state.p25_iden_table[2].channel_bw_hz, 12500u);
    LS_EQ_INT(state.p25_iden_table[2].tx_offset_hz, -45000000);
    LS_EQ_UINT(state.p25_iden_table[2].slots_per_carrier, 2);

    /* Type 5 is also a valid two-slot 12.5 kHz TDMA access type. Keeping
     * this in the same layout case guards the channel-type lookup itself,
     * not just the deployed type-3 row used by the remaining cases. */
    set_bits(iden, 20, 4, 5);
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_CHECK(state.p25_iden_table[2].valid);
    LS_EQ_UINT(state.p25_iden_table[2].channel_bw_hz, 12500u);
    LS_EQ_UINT(state.p25_iden_table[2].slots_per_carrier, 2);
}

LS_CASE(tdma_channel_number_resolves_carrier_and_slot)
{
    dsd_state state = { 0 };
    uint8_t block[12];
    make_tdma_iden(block, 4);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);

    static const struct {
        uint16_t carrier;
        uint8_t slot;
        uint64_t frequency_hz;
    } cases[] = {
        { 0, 0, 851012500ull },
        { 0, 1, 851012500ull },
        { 7, 0, 851100000ull },
        { 7, 1, 851100000ull },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint16_t channel = (uint16_t)((4u << 12) |
                                     (cases[i].carrier << 1) |
                                     cases[i].slot);
        make_group_grant(block, channel, (uint16_t)(100 + i));
        LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
        LS_EQ_UINT(state.p25_phase2_last_frequency_hz, cases[i].frequency_hz);
        LS_EQ_UINT(state.p25_phase2_last_slot, cases[i].slot);
    }
}

LS_CASE(fdma_iden_still_resolves_the_old_way)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };
    uint8_t grant[12];
    /* Resolve the same low 12-bit channel through TDMA and FDMA tables. This
     * both makes the case fail before 0x33 exists and guards against applying
     * TDMA's shift globally once it does. */
    make_tdma_iden(iden, 3);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    make_group_grant(grant, 0x300f, 500);
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 1);
    LS_EQ_UINT(state.p25_phase2_last_frequency_hz, 851100000ull);
    LS_EQ_UINT(state.p25_phase2_last_slot, 1);

    memset(iden, 0, sizeof(iden));
    iden[0] = 0x80 | 0x3d;
    set_bits(iden, 16, 4, 2);
    set_bits(iden, 38, 10, 100);
    set_bits(iden, 48, 32, 170000000);
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_EQ_UINT(state.p25_iden_table[2].slots_per_carrier, 0);

    make_group_grant(grant, 0x200f, 501);
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 1);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 850187500ull);
    LS_EQ_UINT(state.p25_phase2_grant_count, 1);
}

typedef struct {
    unsigned int calls;
    uint64_t last_hz;
} retune_spy_t;

static void phase2_retune_spy(void *user, uint64_t center_hz, bool to_traffic)
{
    retune_spy_t *spy = (retune_spy_t *)user;
    if (to_traffic) {
        spy->calls++;
        spy->last_hz = center_hz;
    }
}

LS_CASE(a_phase_2_grant_is_counted_and_not_followed)
{
    dsd_state state = { 0 };
    p25_grant_follower_t follower;
    retune_spy_t spy = { 0 };
    uint8_t block[12];
    make_tdma_iden(block, 6);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    make_group_grant(block, (uint16_t)((6u << 12) | (9u << 1) | 1u), 1234);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);

    p25_grant_init(&follower, 852000000ull, phase2_retune_spy, &spy);
    LS_CHECK(!p25_grant_from_state(&follower, &state, 1000000));
    LS_EQ_UINT(state.p25_phase2_grant_count, 1);
    LS_EQ_UINT(state.p25_phase2_last_talkgroup, 1234);
    LS_EQ_UINT(state.p25_phase2_last_frequency_hz, 851125000ull);
    LS_EQ_UINT(state.p25_phase2_last_slot, 1);
    LS_EQ_UINT(state.p25_phase2_last_slots_per_carrier, 2);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
    LS_EQ_UINT(follower.followed_count, 0);
    LS_EQ_INT(follower.state, P25_GRANT_ON_CONTROL);
    LS_EQ_UINT(follower.control_hz, 852000000ull);
    LS_EQ_UINT(spy.calls, 0);
    LS_EQ_UINT(spy.last_hz, 0ull);
}

LS_CASE(phase_2_status_appears_in_the_formatted_summary)
{
    dsd_state state = { 0 };
    uint8_t block[12];
    char out[2048];
    make_tdma_iden(block, 8);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    make_group_grant(block, (uint16_t)((8u << 12) | (3u << 1)), 4321);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    LS_CHECK(p25_tsbk_status_format(&state, out, sizeof(out)) > 0);
    LS_CHECK(strstr(out, "Phase 2") != NULL);
    LS_CHECK(strstr(out, "grants=1") != NULL);
    LS_CHECK(strstr(out, "TG=4321") != NULL);
    LS_CHECK(strstr(out, "slot=0/2") != NULL);
}

LS_CASE(deinterleave_restores_all_98_dibit_positions)
{
    static const uint8_t wire_position[98] = {
         0,  1, 26, 27, 50, 51, 74, 75,
         2,  3, 28, 29, 52, 53, 76, 77,
         4,  5, 30, 31, 54, 55, 78, 79,
         6,  7, 32, 33, 56, 57, 80, 81,
         8,  9, 34, 35, 58, 59, 82, 83,
        10, 11, 36, 37, 60, 61, 84, 85,
        12, 13, 38, 39, 62, 63, 86, 87,
        14, 15, 40, 41, 64, 65, 88, 89,
        16, 17, 42, 43, 66, 67, 90, 91,
        18, 19, 44, 45, 68, 69, 92, 93,
        20, 21, 46, 47, 70, 71, 94, 95,
        22, 23, 48, 49, 72, 73, 96, 97,
        24, 25,
    };
    uint8_t wire[98];
    uint8_t output[98];
    for (size_t i = 0; i < 98; i++)
        wire[i] = (uint8_t)i;

    p25_tsbk_deinterleave(wire, output);

    for (size_t i = 0; i < 98; i++)
        LS_EQ_UINT(output[i], wire_position[i]);
}

LS_CASE(viterbi_corrects_two_injected_dibit_symbol_errors)
{
    uint8_t input[48];
    uint8_t encoded[98];
    uint8_t output[48];
    for (size_t i = 0; i < 48; i++)
        input[i] = (uint8_t)((i * 3 + i / 5 + 1) & 3);
    trellis_encode(input, encoded);

    /* Two isolated dibit symbols, each changed by one code bit, are corrected
     * by the four-state path metric. This case records the exercised count;
     * it does not claim a guarantee for arbitrary two-symbol corruption. */
    encoded[17] ^= 1;
    encoded[73] ^= 2;

    LS_EQ_INT(p25_tsbk_trellis_decode(encoded, output), 2);
    LS_CHECK(memcmp(input, output, sizeof(input)) == 0);
}

LS_CASE(crc16_accepts_a_known_block_and_rejects_corruption)
{
    /* Appending two zero augmentation octets to the first ten bytes produces
     * CRC bytes 0x03df; processing the completed 12-byte block has residue 0. */
    uint8_t block[12] = {
        0xbd, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x03, 0xdf,
    };
    LS_EQ_UINT(p25_tsbk_crc16(block, sizeof(block)), 0);
    block[6] ^= 0x04;
    LS_CHECK(p25_tsbk_crc16(block, sizeof(block)) != 0);
}

LS_CASE(crc16_matches_op25_external_check_value)
{
    static const uint8_t check[] = "123456789";

    /* External anchor: boatbod/op25 p25p1_fdma.cc crc16() uses polynomial
     * 0x1021, init 0, MSB-first/no reflection, xorout 0xffff. Running that
     * implementation over this standard check string gives 0x4110. */
    LS_EQ_UINT(p25_tsbk_crc16(check, sizeof(check) - 1), 0x4110);
}

LS_CASE(iden_then_group_grant_resolves_hand_worked_frequency_hz)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };
    uint8_t grant[12] = { 0 };

    iden[0] = 0x80 | 0x3d;
    set_bits(iden, 16, 4, 3);          /* channel identifier 3 */
    set_bits(iden, 38, 10, 100);       /* 100 * 125 = 12,500 Hz spacing */
    set_bits(iden, 48, 32, 170000000); /* 170,000,000 * 5 = 850,000,000 Hz */
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);

    grant[0] = 0x80 | 0x00;
    set_bits(grant, 24, 16, 0x302a); /* table 3, channel number 42 */
    set_bits(grant, 40, 16, 0x4567);
    set_bits(grant, 56, 24, 0x123456);
    add_crc(grant);
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 1);

    /* 850,000,000 + (42 * 12,500) = 850,525,000 Hz. */
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 850525000ull);
    LS_EQ_UINT(state.p25_tsbk_channel, 0x302a);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0x4567);
    LS_EQ_UINT(state.p25_tsbk_source, 0x123456);
    LS_EQ_INT(state.lasttg, 0x4567);
    LS_EQ_INT(state.lastsrc, 0x123456);

    grant[5] ^= 1;
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 0);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0x4567);
}

LS_CASE(grant_update_and_network_status_fields_are_parsed)
{
    dsd_state state = { 0 };
    uint8_t update[12] = { 0 };
    uint8_t network[12] = { 0 };

    state.p25_iden_table[2].valid = 1;
    state.p25_iden_table[2].base_hz = 762000000;
    state.p25_iden_table[2].spacing_hz = 6250;
    update[0] = 0x80 | 0x02;
    set_bits(update, 16, 16, 0x2008);
    set_bits(update, 32, 16, 321);
    set_bits(update, 48, 16, 0x2009);
    set_bits(update, 64, 16, 654);
    add_crc(update);
    LS_EQ_INT(p25_tsbk_parse(&state, update), 1);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 762050000);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 321);
    LS_EQ_UINT(state.p25_tsbk_source, 0);

    network[0] = 0x80 | 0x3b;
    set_bits(network, 24, 20, 0xabcde);
    set_bits(network, 44, 12, 0x789);
    add_crc(network);
    LS_EQ_INT(p25_tsbk_parse(&state, network), 1);
    LS_EQ_UINT(state.p25_tsbk_wacn, 0xabcde);
    LS_EQ_UINT(state.p25_tsbk_sysid, 0x789);
}

LS_CASE(iden_up_vu_positive_offset_populates_table_and_grant_resolves)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };
    uint8_t grant[12] = { 0 };

    /* IDEN_UP_VU (opcode 0x34):
     *   bits 16-19  Identifier = 5
     *   bits 20-23  BW code    = 5   (=> 12.5 kHz)
     *   bit  24     TxOff sign = 1   (positive)
     *   bits 25-37  TxOff mag  = 20  (=> +5.0 MHz)
     *   bits 38-47  Spacing    = 100 (=> 12,500 Hz)
     *   bits 48-79  Base       = 30,000,000 (=> 150.000 MHz) */
    iden[0] = 0x80 | 0x34;
    set_bits(iden, 16, 4, 5);
    set_bits(iden, 20, 4, 5);
    set_bits(iden, 24, 1, 1);
    set_bits(iden, 25, 13, 20);
    set_bits(iden, 38, 10, 100);
    set_bits(iden, 48, 32, 30000000);
    add_crc(iden);

    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_CHECK(state.p25_iden_table[5].valid);
    LS_EQ_UINT(state.p25_iden_table[5].base_hz, 150000000ull);
    LS_EQ_UINT(state.p25_iden_table[5].spacing_hz, 12500u);
    LS_EQ_UINT(state.p25_iden_table[5].channel_bw_hz, 12500u);
    LS_EQ_INT(state.p25_iden_table[5].tx_offset_hz, 5000000);

    grant[0] = 0x80 | 0x00;
    set_bits(grant, 24, 16, 0x5064);   /* iden 5, channel 100 */
    set_bits(grant, 40, 16, 0x1234);
    set_bits(grant, 56, 24, 0x000010);
    add_crc(grant);
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 1);
    /* 150,000,000 + 100 * 12,500 = 151,250,000 Hz. */
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 151250000ull);
}

LS_CASE(iden_up_vu_negative_offset_stores_signed_offset_in_hz)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };
    iden[0] = 0x80 | 0x34;
    set_bits(iden, 16, 4, 7);
    set_bits(iden, 20, 4, 4);          /* BW code 4 => 6.25 kHz */
    set_bits(iden, 24, 1, 0);          /* sign 0 => negative */
    set_bits(iden, 25, 13, 24);        /* 24 * 250 kHz = 6.0 MHz */
    set_bits(iden, 38, 10, 50);        /* spacing 6,250 Hz */
    set_bits(iden, 48, 32, 90000000);  /* base 450.000 MHz */
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_EQ_INT(state.p25_iden_table[7].tx_offset_hz, -6000000);
    LS_EQ_UINT(state.p25_iden_table[7].channel_bw_hz, 6250u);
    LS_EQ_UINT(state.p25_iden_table[7].spacing_hz, 6250u);
    LS_EQ_UINT(state.p25_iden_table[7].base_hz, 450000000ull);
}

LS_CASE(iden_up_records_channel_bandwidth_and_signed_offset)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };

    /* IDEN_UP (opcode 0x3d): bits 20-28 BW (125 Hz units), bit 29 TxOff sign
     * (0 = negative), bits 30-37 TxOff magnitude (250 kHz units). Reuse the
     * existing 850 MHz iden numbers so this is a clean regression check for
     * the 800 MHz path. */
    iden[0] = 0x80 | 0x3d;
    set_bits(iden, 16, 4, 2);
    set_bits(iden, 20, 9, 100);        /* BW = 100 * 125 = 12,500 Hz */
    set_bits(iden, 29, 1, 0);          /* sign 0 => negative */
    set_bits(iden, 30, 8, 180);        /* 180 * 250 kHz = 45 MHz */
    set_bits(iden, 38, 10, 100);
    set_bits(iden, 48, 32, 170000000);
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_EQ_UINT(state.p25_iden_table[2].channel_bw_hz, 12500u);
    LS_EQ_INT(state.p25_iden_table[2].tx_offset_hz, -45000000);
    LS_EQ_UINT(state.p25_iden_table[2].base_hz, 850000000ull);
    LS_EQ_UINT(state.p25_iden_table[2].spacing_hz, 12500u);
}

LS_CASE(grant_for_unknown_identifier_still_resolves_to_zero_hz)
{
    dsd_state state = { 0 };
    uint8_t grant[12] = { 0 };
    grant[0] = 0x80 | 0x00;
    set_bits(grant, 24, 16, 0x600a);   /* identifier 6, never populated */
    set_bits(grant, 40, 16, 100);
    set_bits(grant, 56, 24, 0);
    add_crc(grant);
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 1);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
}

LS_CASE(vendor_mfid_is_counted_not_parsed_as_standard_grant)
{
    dsd_state state = { 0 };
    uint8_t block[12] = { 0 };

    /* Motorola (0x90) TSBK whose opcode happens to be 0x00. If this were
     * parsed as GRP_V_CH_GRANT the follower would retune to a fabricated
     * frequency; the guard is that MFID != 0x00 / 0x01 short-circuits. */
    block[0] = 0x80 | 0x00;
    block[1] = 0x90;
    set_bits(block, 24, 16, 0x302a);
    set_bits(block, 40, 16, 0x4567);
    set_bits(block, 56, 24, 0x123456);
    add_crc(block);

    LS_EQ_INT(p25_tsbk_parse(&state, block), 0);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);
    LS_EQ_UINT(state.p25_tsbk_channel, 0);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
    LS_EQ_UINT(state.p25_tsbk_vendor_count, 1);
    LS_EQ_UINT(state.p25_tsbk_last_vendor_mfid, 0x90);

    /* Harris (0xA4) with the same opcode - still not parsed as a grant. */
    block[1] = 0xA4;
    add_crc(block);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 0);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);
    LS_EQ_UINT(state.p25_tsbk_vendor_count, 2);
    LS_EQ_UINT(state.p25_tsbk_last_vendor_mfid, 0xA4);

    /* MFID 0x01 is standard: the same layout must parse. */
    state.p25_iden_table[3].valid = 1;
    state.p25_iden_table[3].base_hz = 850000000;
    state.p25_iden_table[3].spacing_hz = 12500;
    block[1] = 0x01;
    add_crc(block);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0x4567);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 850525000ull);
    LS_EQ_UINT(state.p25_tsbk_vendor_count, 2);
}

LS_CASE(identifier_15_writes_last_slot_only)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };
    iden[0] = 0x80 | 0x3d;
    set_bits(iden, 16, 4, 15);
    set_bits(iden, 38, 10, 100);
    set_bits(iden, 48, 32, 170000000);
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_CHECK(state.p25_iden_table[15].valid);
    LS_CHECK(!state.p25_iden_table[14].valid);
    LS_EQ_UINT(state.p25_iden_table[15].base_hz, 850000000ull);
}

LS_CASE(iden_table_is_cleared_when_wacn_or_sysid_changes)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };
    uint8_t network[12] = { 0 };

    /* Populate iden 3 on the empty state (system unknown yet). */
    iden[0] = 0x80 | 0x3d;
    set_bits(iden, 16, 4, 3);
    set_bits(iden, 38, 10, 100);
    set_bits(iden, 48, 32, 170000000);
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(&state, iden), 1);
    LS_CHECK(state.p25_iden_table[3].valid);

    /* First NET_STS: not a boundary. */
    network[0] = 0x80 | 0x3b;
    set_bits(network, 24, 20, 0xabcde);
    set_bits(network, 44, 12, 0x789);
    add_crc(network);
    LS_EQ_INT(p25_tsbk_parse(&state, network), 1);
    LS_CHECK(state.p25_iden_table[3].valid);
    LS_CHECK(state.p25_net_valid);
    LS_EQ_UINT(state.p25_net_generation, 1);

    /* Same NET_STS re-broadcast: still not a boundary. */
    LS_EQ_INT(p25_tsbk_parse(&state, network), 1);
    LS_CHECK(state.p25_iden_table[3].valid);
    LS_EQ_UINT(state.p25_net_generation, 2);

    /* Different WACN: the table is stale and must be cleared. */
    memset(network, 0, sizeof(network));
    network[0] = 0x80 | 0x3b;
    set_bits(network, 24, 20, 0xfffff);
    set_bits(network, 44, 12, 0x789);
    add_crc(network);
    LS_EQ_INT(p25_tsbk_parse(&state, network), 1);
    LS_CHECK(!state.p25_iden_table[3].valid);
    LS_EQ_UINT(state.p25_tsbk_wacn, 0xfffff);
    LS_EQ_UINT(state.p25_net_generation, 3);
}

/* bench cases for the wider TSBK dispatch. */

/* Common iden used by voice-grant cases. Identifier 3, spacing 12.5 kHz,
 * base 850 MHz - matches the number used elsewhere in this suite so a
 * failure is easy to spot by hand. */
static void install_common_iden(dsd_state *state)
{
    uint8_t iden[12] = { 0 };
    iden[0] = 0x80 | 0x3d;
    set_bits(iden, 16, 4, 3);
    set_bits(iden, 38, 10, 100);       /* 12.5 kHz spacing */
    set_bits(iden, 48, 32, 170000000); /* 850 MHz base */
    add_crc(iden);
    LS_EQ_INT(p25_tsbk_parse(state, iden), 1);
}

LS_CASE(grp_v_ch_grant_updt_exp_populates_voice_grant_fields)
{
    dsd_state state = { 0 };
    uint8_t block[12] = { 0 };
    install_common_iden(&state);

    /* 0x03 GRP_V_CH_GRANT_UPDT_EXP:
     *   bits 24-39  Channel T (this is what the follower must tune)
     *   bits 40-55  Channel R
     *   bits 56-71  Talkgroup */
    block[0] = 0x80 | 0x03;
    set_bits(block, 24, 16, 0x302a);   /* iden 3, ch 42 => 850,525,000 Hz */
    set_bits(block, 40, 16, 0x3060);   /* iden 3, ch 96 => uplink pair */
    set_bits(block, 56, 16, 0xabcd);
    add_crc(block);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    LS_EQ_UINT(state.p25_tsbk_channel, 0x302a);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0xabcd);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 850525000ull);
    LS_EQ_UINT(state.p25_tsbk_last_opcode, 0x03);
}

LS_CASE(uu_v_ch_grant_stores_addresses_and_does_not_touch_voice_grant)
{
    dsd_state state = { 0 };
    uint8_t block[12] = { 0 };
    install_common_iden(&state);

    /* 0x04 UU_V_CH_GRANT: bits 16-31 ch, 32-55 target, 56-79 source. */
    block[0] = 0x80 | 0x04;
    set_bits(block, 16, 16, 0x302a);
    set_bits(block, 32, 24, 0x111111);
    set_bits(block, 56, 24, 0x222222);
    add_crc(block);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    LS_EQ_UINT(state.p25_last_uu_channel, 0x302a);
    LS_EQ_UINT(state.p25_last_uu_target, 0x111111);
    LS_EQ_UINT(state.p25_last_uu_source, 0x222222);
    LS_EQ_UINT(state.p25_uu_grant_count, 1);
    /* Voice-grant fields untouched - the unit-to-unit path never calls
     * set_voice_grant, and this is what keeps the follower from chasing
     * a private call that has no talkgroup. */
    LS_EQ_UINT(state.p25_tsbk_channel, 0);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
}

LS_CASE(uu_ans_req_records_source_and_target)
{
    dsd_state state = { 0 };
    uint8_t block[12] = { 0 };

    block[0] = 0x80 | 0x05;
    set_bits(block, 32, 24, 0x333333);
    set_bits(block, 56, 24, 0x444444);
    add_crc(block);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    LS_EQ_UINT(state.p25_last_uu_target, 0x333333);
    LS_EQ_UINT(state.p25_last_uu_source, 0x444444);
    LS_EQ_UINT(state.p25_uu_grant_count, 1);
}

LS_CASE(uu_v_ch_grant_updt_records_addresses)
{
    dsd_state state = { 0 };
    uint8_t block[12] = { 0 };

    block[0] = 0x80 | 0x06;
    set_bits(block, 16, 16, 0x3055);
    set_bits(block, 32, 24, 0x555555);
    set_bits(block, 56, 24, 0x666666);
    add_crc(block);
    LS_EQ_INT(p25_tsbk_parse(&state, block), 1);
    LS_EQ_UINT(state.p25_last_uu_channel, 0x3055);
    LS_EQ_UINT(state.p25_last_uu_target, 0x555555);
    LS_EQ_UINT(state.p25_last_uu_source, 0x666666);
    LS_EQ_UINT(state.p25_uu_grant_count, 1);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
}

LS_CASE(telephone_interconnect_grant_is_counted_not_voice_grant)
{
    dsd_state state = { 0 };
    uint8_t g[12] = { 0 };
    uint8_t u[12] = { 0 };
    install_common_iden(&state);

    /* 0x08 TELE_INT_CH_GRANT: ch bits 24-39, address bits 56-79. */
    g[0] = 0x80 | 0x08;
    set_bits(g, 24, 16, 0x302a);
    set_bits(g, 56, 24, 0xaabbcc);
    add_crc(g);
    LS_EQ_INT(p25_tsbk_parse(&state, g), 1);
    LS_EQ_UINT(state.p25_last_interconnect_channel, 0x302a);
    LS_EQ_UINT(state.p25_last_interconnect_address, 0xaabbcc);
    LS_EQ_UINT(state.p25_interconnect_grant_count, 1);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);

    /* 0x09 update: ch bits 16-31, address bits 32-55. */
    u[0] = 0x80 | 0x09;
    set_bits(u, 16, 16, 0x3033);
    set_bits(u, 32, 24, 0xddeeff);
    add_crc(u);
    LS_EQ_INT(p25_tsbk_parse(&state, u), 1);
    LS_EQ_UINT(state.p25_last_interconnect_channel, 0x3033);
    LS_EQ_UINT(state.p25_last_interconnect_address, 0xddeeff);
    LS_EQ_UINT(state.p25_interconnect_grant_count, 2);
}

LS_CASE(data_channel_grant_is_counted_and_never_sets_follower_frequency)
{
    dsd_state state = { 0 };
    uint8_t g[12] = { 0 };
    uint8_t s[12] = { 0 };
    install_common_iden(&state);

    /* 0x10 GRP_D_CH_GRANT. Field bits 24-39 T channel, 56-71 group. */
    g[0] = 0x80 | 0x10;
    set_bits(g, 24, 16, 0x302a);
    set_bits(g, 56, 16, 0x1234);
    add_crc(g);
    LS_EQ_INT(p25_tsbk_parse(&state, g), 1);
    LS_EQ_UINT(state.p25_last_data_channel, 0x302a);
    LS_EQ_UINT(state.p25_last_data_group, 0x1234);
    LS_EQ_UINT(state.p25_data_grant_count, 1);
    /* Critical: p25_tsbk_frequency_hz stays 0 so the follower cannot chase
     * it even if some future patch mistakenly adds this to the whitelist. */
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);

    /* 0x14 SNDCP_CH_GRANT. */
    s[0] = 0x80 | 0x14;
    set_bits(s, 24, 16, 0x3033);
    add_crc(s);
    LS_EQ_INT(p25_tsbk_parse(&state, s), 1);
    LS_EQ_UINT(state.p25_last_data_channel, 0x3033);
    LS_EQ_UINT(state.p25_data_grant_count, 2);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
}

LS_CASE(registration_and_affiliation_responses_are_counted)
{
    dsd_state state = { 0 };
    uint8_t b[12];

    struct {
        uint8_t  op;
        unsigned int expected_count;
    } ops[] = {
        { 0x20, 1 }, { 0x27, 2 }, { 0x28, 3 }, { 0x2c, 4 }, { 0x2f, 5 },
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        memset(b, 0, sizeof(b));
        b[0] = 0x80 | ops[i].op;
        /* Put something distinct in the source/target-shaped bits. */
        set_bits(b, 24, 8, ops[i].op);
        set_bits(b, 32, 24, 0x100000u + i);
        set_bits(b, 56, 24, 0x200000u + i);
        add_crc(b);
        LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
        LS_EQ_UINT(state.p25_last_reg_opcode, ops[i].op);
        LS_EQ_UINT(state.p25_reg_event_count, ops[i].expected_count);
    }
}

LS_CASE(rfss_sts_bcst_populates_local_site_identity)
{
    dsd_state state = { 0 };
    uint8_t b[12] = { 0 };

    b[0] = 0x80 | 0x3a;
    set_bits(b, 16, 8, 0x5a);        /* LRA */
    set_bits(b, 24, 12, 0x789);      /* SYSID */
    set_bits(b, 36, 8, 4);           /* RFSS */
    set_bits(b, 44, 8, 17);          /* Site */
    set_bits(b, 52, 12, 0x123);      /* Ch T */
    set_bits(b, 64, 12, 0x456);      /* Ch R */
    set_bits(b, 76, 4, 0xd);         /* Svc class */
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_CHECK(state.p25_rfss_valid);
    LS_EQ_UINT(state.p25_rfss_lra, 0x5a);
    LS_EQ_UINT(state.p25_rfss_sysid, 0x789);
    LS_EQ_UINT(state.p25_rfss_id, 4);
    LS_EQ_UINT(state.p25_rfss_site_id, 17);
    LS_EQ_UINT(state.p25_rfss_ch_t, 0x123);
    LS_EQ_UINT(state.p25_rfss_ch_r, 0x456);
    LS_EQ_UINT(state.p25_rfss_svc_class, 0xd);
    LS_EQ_UINT(state.p25_rfss_generation, 1);
}

LS_CASE(adj_sts_bcst_populates_neighbour_list_and_does_not_retune)
{
    dsd_state state = { 0 };
    uint8_t b[12] = { 0 };
    install_common_iden(&state);

    /* First neighbour: rfss 2, site 5, channel resolvable via iden 3. */
    b[0] = 0x80 | 0x3c;
    set_bits(b, 16, 8, 0x11);           /* LRA */
    set_bits(b, 28, 12, 0x321);         /* SYSID */
    set_bits(b, 40, 8, 2);              /* RFSS */
    set_bits(b, 48, 8, 5);              /* Site */
    set_bits(b, 56, 16, 0x302a);        /* Channel iden 3, num 42 */
    set_bits(b, 72, 8, 0xC5);           /* Service class */
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);

    LS_EQ_UINT(state.p25_neighbor_count, 1);
    LS_CHECK(state.p25_neighbors[0].valid);
    LS_EQ_UINT(state.p25_neighbors[0].rfss_id, 2);
    LS_EQ_UINT(state.p25_neighbors[0].site_id, 5);
    LS_EQ_UINT(state.p25_neighbors[0].lra, 0x11);
    LS_EQ_UINT(state.p25_neighbors[0].sysid, 0x321);
    LS_EQ_UINT(state.p25_neighbors[0].channel, 0x302a);
    LS_EQ_UINT(state.p25_neighbors[0].service_class, 0xC5);
    LS_EQ_UINT(state.p25_neighbors[0].freq_hz, 850525000ull);
    LS_EQ_UINT(state.p25_neighbor_generation, 1);
    /* Voice-grant fields untouched - a neighbour broadcast must never
     * appear as a grant to the follower. */
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);

    /* Second broadcast for the same site: overwrite, not append. */
    memset(b, 0, sizeof(b));
    b[0] = 0x80 | 0x3c;
    set_bits(b, 16, 8, 0x22);
    set_bits(b, 28, 12, 0x321);
    set_bits(b, 40, 8, 2);
    set_bits(b, 48, 8, 5);
    set_bits(b, 56, 16, 0x3050);
    set_bits(b, 72, 8, 0xA5);
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_EQ_UINT(state.p25_neighbor_count, 1);
    LS_EQ_UINT(state.p25_neighbors[0].lra, 0x22);
    LS_EQ_UINT(state.p25_neighbors[0].channel, 0x3050);
    LS_EQ_UINT(state.p25_neighbors[0].service_class, 0xA5);
    LS_EQ_UINT(state.p25_neighbor_generation, 2);

    /* A different site takes a new slot. */
    memset(b, 0, sizeof(b));
    b[0] = 0x80 | 0x3c;
    set_bits(b, 40, 8, 3);
    set_bits(b, 48, 8, 9);
    set_bits(b, 56, 16, 0x3080);
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_EQ_UINT(state.p25_neighbor_count, 2);
    LS_EQ_UINT(state.p25_neighbors[1].rfss_id, 3);
    LS_EQ_UINT(state.p25_neighbors[1].site_id, 9);
    LS_EQ_UINT(state.p25_neighbors[1].channel, 0x3080);
    LS_EQ_UINT(state.p25_neighbor_generation, 3);
}

LS_CASE(sccb_populates_secondary_control_pair)
{
    dsd_state state = { 0 };
    uint8_t b[12] = { 0 };

    /* 0x3e SCCB. */
    b[0] = 0x80 | 0x3e;
    set_bits(b, 16, 8, 0x33);
    set_bits(b, 24, 8, 7);              /* RFSS */
    set_bits(b, 32, 8, 12);             /* Site */
    set_bits(b, 40, 8, 0xa0);           /* Svc class 1 */
    set_bits(b, 48, 16, 0x1234);        /* Ch1 */
    set_bits(b, 64, 8, 0xa1);           /* Svc class 2 */
    set_bits(b, 72, 8, 0x88);           /* Ch2 partial */
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_CHECK(state.p25_sccb_valid);
    LS_EQ_UINT(state.p25_sccb_rfss_id, 7);
    LS_EQ_UINT(state.p25_sccb_site_id, 12);
    LS_EQ_UINT(state.p25_sccb_ch1, 0x1234);
    LS_EQ_UINT(state.p25_sccb_ch2, 0x88);
    LS_EQ_UINT(state.p25_sccb_generation, 1);

    /* 0x39 SCCB_EXP - same handler. */
    memset(&state, 0, sizeof(state));
    memset(b, 0, sizeof(b));
    b[0] = 0x80 | 0x39;
    set_bits(b, 48, 16, 0x5678);
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_EQ_UINT(state.p25_sccb_ch1, 0x5678);
    LS_EQ_UINT(state.p25_sccb_generation, 1);
}

LS_CASE(sys_srv_bcst_captures_available_and_supported_services)
{
    dsd_state state = { 0 };
    uint8_t b[12] = { 0 };
    b[0] = 0x80 | 0x38;
    set_bits(b, 16, 8, 0xa5);
    set_bits(b, 24, 24, 0x123456u);
    set_bits(b, 48, 24, 0xabcdefu);
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_CHECK(state.p25_sys_srv_valid);
    LS_EQ_UINT(state.p25_sys_srv_twv, 0xa5);
    LS_EQ_UINT(state.p25_sys_srv_available, 0x123456u);
    LS_EQ_UINT(state.p25_sys_srv_supported, 0xabcdefu);
}

LS_CASE(sync_bcst_captures_time_and_microslot)
{
    dsd_state state = { 0 };
    uint8_t b[12] = { 0 };
    b[0] = 0x80 | 0x30;
    set_bits(b, 24, 4, 0x6);            /* Year offset => 2006 */
    set_bits(b, 28, 4, 0x3);            /* Month = 3 */
    set_bits(b, 32, 5, 0x15);           /* Day = 21 */
    set_bits(b, 47, 17, 12345);         /* Microsecond count */
    set_bits(b, 64, 16, 0xbeef);        /* Microslot */
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_CHECK(state.p25_sync_valid);
    LS_EQ_UINT(state.p25_sync_year, 2006);
    LS_EQ_UINT((state.p25_sync_month_day >> 8) & 0x0f, 3);
    LS_EQ_UINT(state.p25_sync_month_day & 0x1f, 0x15);
    LS_EQ_UINT(state.p25_sync_us, 12345);
    LS_EQ_UINT(state.p25_sync_microslot, 0xbeef);
}

LS_CASE(unhandled_opcode_increments_its_counter_and_changes_nothing_else)
{
    dsd_state state = { 0 };
    uint8_t b[12] = { 0 };
    /* Opcode 0x15 is not in the dispatch. Parsing must succeed (recognised
     * as an unhandled standard TSBK) and leave every field except the
     * counter and last_opcode alone. */
    b[0] = 0x80 | 0x15;
    set_bits(b, 24, 16, 0x302a);   /* would be channel if this were 0x00 */
    set_bits(b, 40, 16, 0xffff);   /* would be talkgroup */
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_EQ_UINT(state.p25_tsbk_unhandled[0x15], 1);
    LS_EQ_UINT(state.p25_tsbk_channel, 0);
    LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
    LS_EQ_UINT(state.p25_neighbor_count, 0);
    LS_EQ_UINT(state.p25_data_grant_count, 0);
    LS_EQ_UINT(state.p25_uu_grant_count, 0);

    /* A second copy increments only this opcode's slot. */
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
    LS_EQ_UINT(state.p25_tsbk_unhandled[0x15], 2);
    LS_EQ_UINT(state.p25_tsbk_unhandled[0x00], 0);
    LS_EQ_UINT(state.p25_tsbk_unhandled[0x3c], 0);
}

LS_CASE(bad_crc_rejects_every_new_opcode_before_reading_any_field)
{
    /* One block per new opcode, corrupted after add_crc(). All must be
     * rejected before touching per-opcode state - the parser's very first
     * gate is the CRC. */
    static const uint8_t opcodes[] = {
        0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x10, 0x14,
        0x20, 0x27, 0x28, 0x2c, 0x2f,
        0x30, 0x38, 0x39, 0x3a, 0x3c, 0x3e,
        0x15,  /* the unhandled path */
    };
    for (size_t i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++) {
        dsd_state state = { 0 };
        uint8_t b[12] = { 0 };
        b[0] = 0x80 | opcodes[i];
        set_bits(b, 16, 8, 0xff);
        set_bits(b, 40, 16, 0xabcd);
        add_crc(b);
        b[5] ^= 0x01;    /* one bit flip after CRC => reject */
        LS_EQ_INT(p25_tsbk_parse(&state, b), 0);
        LS_EQ_UINT(state.p25_tsbk_unhandled[opcodes[i]], 0);
        LS_EQ_UINT(state.p25_neighbor_count, 0);
        LS_EQ_UINT(state.p25_data_grant_count, 0);
        LS_EQ_UINT(state.p25_uu_grant_count, 0);
        LS_EQ_UINT(state.p25_reg_event_count, 0);
        LS_EQ_UINT(state.p25_rfss_valid, 0);
        LS_EQ_UINT(state.p25_sccb_valid, 0);
        LS_EQ_UINT(state.p25_sys_srv_valid, 0);
        LS_EQ_UINT(state.p25_sync_valid, 0);
        LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
    }
}

LS_CASE(status_format_reports_iden_and_neighbours_and_unhandled)
{
    dsd_state state = { 0 };
    uint8_t b[12] = { 0 };
    install_common_iden(&state);

    b[0] = 0x80 | 0x3c;
    set_bits(b, 40, 8, 2);
    set_bits(b, 48, 8, 5);
    set_bits(b, 56, 16, 0x302a);
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);

    memset(b, 0, sizeof(b));
    b[0] = 0x80 | 0x15;
    add_crc(b);
    LS_EQ_INT(p25_tsbk_parse(&state, b), 1);

    char out[2048];
    size_t n = p25_tsbk_status_format(&state, out, sizeof(out));
    LS_CHECK(n > 0);
    LS_CHECK(strstr(out, "iden") != NULL);
    LS_CHECK(strstr(out, "neighbours 1") != NULL);
    LS_CHECK(strstr(out, "rfss=2") != NULL);
    LS_CHECK(strstr(out, "unhandled:") != NULL);
    LS_CHECK(strstr(out, "$15=1") != NULL);
}

LS_CASE(grant_follower_still_ignores_data_and_status_broadcasts)
{
    /* Cross-check the structural guarantee at the follower's boundary:
     * every non-voice opcode above must leave the follower on control,
     * even after a full parse. Data-grant path never sets talkgroup /
     * freq, so p25_grant_from_state cannot pass the "freq_hz == 0" gate.
     * This complements the assertions in test_p25_grant.c. */
    static const uint8_t nonvoice[] = {
        0x10, 0x14,               /* data */
        0x08, 0x09,               /* telephone interconnect */
        0x04, 0x05, 0x06,         /* unit-to-unit */
        0x20, 0x27, 0x28, 0x2c, 0x2f, /* reg/affil */
        0x30, 0x38, 0x39, 0x3a, 0x3c, 0x3e,
    };
    for (size_t i = 0; i < sizeof(nonvoice) / sizeof(nonvoice[0]); i++) {
        dsd_state state = { 0 };
        uint8_t b[12] = { 0 };
        install_common_iden(&state);
        b[0] = 0x80 | nonvoice[i];
        set_bits(b, 24, 16, 0x302a);
        set_bits(b, 40, 16, 0x1234);
        set_bits(b, 56, 24, 0x777777);
        add_crc(b);
        LS_EQ_INT(p25_tsbk_parse(&state, b), 1);
        /* The voice-grant fields never change from install_common_iden. */
        LS_EQ_UINT(state.p25_tsbk_frequency_hz, 0ull);
        LS_EQ_UINT(state.p25_tsbk_talkgroup, 0);
    }
}

LS_CASE(two_block_tsdu_pipeline_skips_status_and_stops_on_last_block)
{
    dsd_state state = { 0 };
    uint8_t iden[12] = { 0 };
    uint8_t grant[12] = { 0 };
    uint8_t data[2 * P25_TSBK_ENCODED_DIBITS];
    uint8_t captured[P25_TSDU_DIBIT_COUNT] = { 0 };
    size_t data_pos = 0;

    iden[0] = 0x3d; /* LB=0: another TSBK follows. */
    set_bits(iden, 16, 4, 3);
    set_bits(iden, 38, 10, 100);
    set_bits(iden, 48, 32, 170000000);
    add_crc(iden);
    grant[0] = 0x80;
    set_bits(grant, 24, 16, 0x302a);
    set_bits(grant, 40, 16, 0x4567);
    set_bits(grant, 56, 24, 0x123456);
    add_crc(grant);

    encode_block(iden, data);
    encode_block(grant, data + P25_TSBK_ENCODED_DIBITS);
    for (size_t wire = 0; wire < sizeof(captured); wire++) {
        if (wire >= 14 && (wire - 14) % 36 == 0) {
            captured[wire] = 3; /* status symbol, not trellis data */
        } else if (data_pos < sizeof(data)) {
            captured[wire] = data[data_pos++];
        }
    }

    LS_EQ_UINT(data_pos, sizeof(data));
    LS_EQ_UINT(p25_tsbk_process_tsdu(&state, captured, sizeof(captured)), 2);
    LS_EQ_UINT(state.p25_tsbk_valid_count, 2);
    LS_EQ_UINT(state.p25_tsbk_crc_errors, 0);
    LS_EQ_UINT(state.p25_tsbk_trellis_errors, 0);
    LS_EQ_UINT(state.p25_tsbk_last_opcode, 0x00);
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 850525000ull);
}

LS_CASE(malformed_trellis_is_counted_as_an_invalid_tsbk)
{
    dsd_state state = { 0 };
    uint8_t captured[P25_TSDU_DIBIT_COUNT] = { 0 };
    /* First captured position is trellis data (status starts at offset 14).
     * Values 0..3 are dibits; 4 makes the production decoder reject. */
    captured[0] = 4;
    LS_EQ_UINT(p25_tsbk_process_tsdu(
                   &state, captured, P25_TSBK_ENCODED_DIBITS + 3U), 0);
    LS_EQ_UINT(state.p25_tsbk_valid_count, 0);
    LS_EQ_UINT(state.p25_tsbk_crc_errors, 0);
    LS_EQ_UINT(state.p25_tsbk_trellis_errors, 1);
}
