/* real BCH/frame + TSBK/trellis + scanner/follower replay.
 * LDU/vocoder is a spy: PCM assertions prove routing/gating, not speech quality. */
/* P25 frame dispatch and raw TSDU payload retention. */

#include "ls_test.h"
#include "dsd.h"
#include "p25_tsbk.h"
#include "p25_receive.h"
#include "p25p1_check_nid.c"

#define INPUT_CAPACITY (33 + P25_TSDU_DIBIT_COUNT)

static int s_input[INPUT_CAPACITY];
static int s_input_count;
static int s_input_pos;
static int s_nac = 0x293;
static uint16_t s_voice_tg;
static int s_skip_count;
static int s_hdu_calls;
static int s_ldu1_calls;
static int s_ldu2_calls;
static int s_tdu_calls;
static int s_tdulc_calls;
static int s_mbe_init_calls;

int autoscan_bch_ok_flag;
int dsd_bch_fail_counter;

int getDibit(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    LS_CHECK_MSG(s_input_pos < s_input_count,
                 "dispatcher read dibit %d past %d", s_input_pos, s_input_count);
    return s_input[s_input_pos++];
}

void skipDibit(dsd_opts *opts, dsd_state *state, int count)
{
    (void)opts;
    (void)state;
    s_skip_count += count;
    s_input_pos += count;
}

void processHDU(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_hdu_calls++;
}

void processLDU1(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_ldu1_calls++;
    if (s_voice_tg) {
        uint8_t lcinfo[7] = {0, 0, (uint8_t)(s_voice_tg >> 8), (uint8_t)s_voice_tg, 0, 0, 123};
        LS_CHECK(p25_lcw_dispatch(state, 0, 0, lcinfo, 1, 1));
    }
    if (!p25_ldu_should_mute_encrypted(state, opts)) state->pcm_out_write = 160;
}

void processLDU2(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_ldu2_calls++;
    if (!p25_ldu_should_mute_encrypted(state, opts)) state->pcm_out_write = 160;
}

void processTDU(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_tdu_calls++;
}

void processTDULC(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_tdulc_calls++;
}

void mbe_initMbeParms(mbe_parms *cur_mp, mbe_parms *prev_mp,
                      mbe_parms *prev_mp_enhanced)
{
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;
    s_mbe_init_calls++;
}

void audio_beep_request(int kind)
{
    (void)kind;
}

static void make_codeword(int nac12, int duid, char *cw63)
{
    int data[BCH_KK];
    int par[BCH_RR];

    for (int i = 0; i < 12; i++) data[i] = (nac12 >> (11 - i)) & 1;
    for (int i = 0; i < 4; i++) data[12 + i] = (duid >> (3 - i)) & 1;
    bch_encode(data, par);

    for (int i = 0; i < BCH_KK; i++) cw63[i] = (char)data[i];
    for (int i = 0; i < BCH_RR; i++) cw63[BCH_KK + i] = (char)par[i];
}

static void load_frame(int duid, int with_tsdu_payload)
{
    char cw[63];
    make_codeword(s_nac, duid, cw);

    s_input_count = 0;
    s_input_pos = 0;
    s_skip_count = 0;

    for (int i = 0; i < 11; i++)
        s_input[s_input_count++] = (cw[i * 2] << 1) | cw[i * 2 + 1];
    s_input[s_input_count++] = 0; /* status dibit */
    for (int i = 11; i < 31; i++)
        s_input[s_input_count++] = (cw[i * 2] << 1) | cw[i * 2 + 1];
    s_input[s_input_count++] = cw[62] << 1; /* parity is currently ignored */

    if (with_tsdu_payload) {
        for (int i = 0; i < P25_TSDU_DIBIT_COUNT; i++)
            s_input[s_input_count++] = (i * 3 + 1) & 3;
    }
}

static void reset_dispatch_counts(void)
{
    s_hdu_calls = 0;
    s_ldu1_calls = 0;
    s_ldu2_calls = 0;
    s_tdu_calls = 0;
    s_tdulc_calls = 0;
    s_mbe_init_calls = 0;
}

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

static dsd_state decoder;
static dsd_opts options;
static p25_grant_follower_t follower;
static p25_scan_ctrl_t scanner;
static uint64_t tunes[16];
static unsigned int tune_count;
static int64_t now_us;

static void tune_spy(void *user, uint64_t hz, bool traffic)
{
    (void)user;
    LS_CHECK(tune_count < 16);
    if (tune_count < 16) tunes[tune_count++] = hz;
    if (traffic) {
        LS_EQ_UINT(follower.active_call.carrier_hz, hz);
        LS_EQ_UINT(follower.active_call.nac, s_nac);
        LS_EQ_UINT(follower.active_call.slots_per_carrier, 1);
    } else {
        LS_EQ_INT(follower.active_call.support, P25_CALL_NONE);
    }
}

static void setup(void)
{
    memset(&decoder, 0, sizeof(decoder));
    memset(&options, 0, sizeof(options));
    reset_dispatch_counts();
    p25_scan_init(&scanner);
    p25_grant_init(&follower, 852000000, tune_spy, NULL);
    tune_count = 0;
    now_us = 1000000;
    s_nac = 0x293;
    s_voice_tg = 0;
}

static void frame(int duid)
{
    load_frame(duid, 0);
    decoder.pcm_out_write = 0;
    processFrame(&options, &decoder);
    now_us += 180000;
    (void)p25_receive_frame(&scanner, &follower, &decoder, now_us, true);
}

/* Render 1-3 TSBKs with real trellis/interleave/status symbols after a BCH NID. */
static void tsdu(uint8_t blocks[][12], size_t count)
{
    LS_CHECK(count >= 1 && count <= 3);
    uint8_t encoded[3 * 98];
    for (size_t i = 0; i < count; i++) {
        blocks[i][0] = (uint8_t)((blocks[i][0] & 0x7f) | (i + 1 == count ? 0x80 : 0));
        add_crc(blocks[i]);
        encode_block(blocks[i], encoded + i * 98);
    }
    load_frame(7, 0);
    unsigned int payload = 0;
    for (unsigned int wire = 0; wire < P25_TSDU_DIBIT_COUNT; wire++) {
        bool status = wire >= 14 && (wire - 14) % 36 == 0;
        s_input[s_input_count++] = status || payload >= count * 98 ? 0 : encoded[payload++];
    }
    processFrame(&options, &decoder);
    now_us += 180000;
    (void)p25_receive_frame(&scanner, &follower, &decoder, now_us, true);
    LS_EQ_INT(s_input_pos, s_input_count);
}

static void fdma_iden(uint8_t b[12], uint8_t id)
{
    memset(b, 0, 12);
    b[0] = 0x3d;
    set_bits(b, 16, 4, id);
    set_bits(b, 20, 9, 100);
    set_bits(b, 38, 10, 100);
    set_bits(b, 48, 32, 170202500);
    add_crc(b);
}

static void network(uint8_t b[12], uint32_t wacn, uint16_t sysid)
{
    memset(b, 0, 12);
    b[0] = 0x3b;
    set_bits(b, 24, 20, wacn);
    set_bits(b, 44, 12, sysid);
    add_crc(b);
}

LS_CASE(identifier_grant_status_in_one_tsdu_follows_then_returns_and_relocks)
{
    setup();
    uint8_t b[3][12];
    network(b[0], 0xabcde, 0x123);
    tsdu(b, 1);
    fdma_iden(b[0], 2);
    make_group_grant(b[1], 0x200f, 42);
    memset(b[2], 0, 12); b[2][0] = 0x38;
    tsdu(b, 3);
    LS_EQ_UINT(tune_count, 1);
    LS_EQ_UINT(tunes[0], 851200000);
    LS_EQ_INT(follower.receive_state, P25_RX_TRAFFIC_WAIT);
    LS_EQ_UINT(follower.active_call.talkgroup, 42);
    LS_EQ_UINT(follower.active_call.source, 0x123456);
    LS_EQ_UINT(follower.active_call.wacn, 0xabcde);
    LS_EQ_UINT(follower.active_call.sysid, 0x123);
    LS_EQ_UINT(follower.active_call.channel, 0x200f);
    LS_CHECK(follower.active_call.system_valid);
    LS_EQ_INT(decoder.p25_ess_valid, 0);
    frame(5); /* valid voice with unknown ESS is activity, not decoded PCM */
    LS_EQ_INT(follower.receive_state, P25_RX_TRAFFIC_SYNC);
    LS_EQ_INT(decoder.pcm_out_write, 0);
    decoder.p25_ess_valid = 1; decoder.p25_algid = 0x80;
    frame(10);
    LS_EQ_INT(follower.receive_state, P25_RX_AUDIO);
    LS_EQ_INT(decoder.pcm_out_write, 160);
    frame(3);
    LS_EQ_UINT(tune_count, 2);
    LS_EQ_UINT(tunes[1], 852000000);
    LS_EQ_INT(follower.receive_state, P25_RX_CONTROL_SEARCH);
    LS_EQ_INT(decoder.p25_ess_valid, 0);
    LS_EQ_INT(follower.active_call.support, P25_CALL_NONE);
    LS_CHECK(decoder.p25_iden_table[2].valid);
    network(b[0], 0xabcde, 0x123); tsdu(b, 1);
    LS_EQ_INT(follower.receive_state, P25_RX_CONTROL_LOCKED);
    LS_EQ_UINT(follower.control_relocks, 2);
    make_group_grant(b[0], 0x200f, 42); tsdu(b, 1);
    frame(5);
    LS_EQ_INT(decoder.pcm_out_write, 0); /* same TG must not inherit CLEAR */
}

LS_CASE(all_channel_types_have_truthful_support_and_canonical_slot_defaults)
{
    for (unsigned int type = 0; type < 16; type++) {
        setup();
        uint8_t b[2][12];
        make_tdma_iden(b[0], 2); set_bits(b[0], 20, 4, type);
        make_group_grant(b[1], 0x200f, 42);
        tsdu(b, 2);
        const p25_call_info_t *g = &follower.observed_grant;
        LS_EQ_UINT(g->channel_type, type);
        LS_EQ_UINT(tune_count, type == 1 ? 1 : 0);
        if (type < 6) {
            unsigned int slots = type < 3 ? 1 : type == 4 ? 4 : 2;
            LS_EQ_UINT(g->slots_per_carrier, slots);
            LS_EQ_UINT(g->slot, 15 % slots);
            LS_EQ_UINT(g->carrier_hz, 851012500 + (15 / slots) * 12500);
            LS_EQ_INT(g->support, type == 1 ? P25_CALL_PHASE1 : P25_CALL_UNSUPPORTED);
        } else {
            LS_CHECK(!decoder.p25_iden_table[2].valid);
            LS_EQ_UINT(g->carrier_hz, 0);
            LS_EQ_INT(g->support, P25_CALL_MISSING_IDEN);
        }
        LS_EQ_INT(s_ldu1_calls + s_ldu2_calls, 0);
        LS_EQ_INT(decoder.pcm_out_write, 0);
        LS_EQ_UINT(decoder.p25_tsbk_unhandled[0x33], 0);
    }
}

LS_CASE(tdma_and_phase1_pairs_survive_the_actual_scanner_policy)
{
    setup();
    uint8_t b[3][12];
    make_tdma_iden(b[0], 2); fdma_iden(b[1], 3);
    memset(b[2], 0, 12); b[2][0] = 2;
    set_bits(b[2], 16, 16, 0x200f); set_bits(b[2], 32, 16, 42);
    set_bits(b[2], 48, 16, 0x300f); set_bits(b[2], 64, 16, 43);
    tsdu(b, 3);
    LS_EQ_UINT(follower.unsupported_grants, 1);
    LS_EQ_UINT(follower.active_call.talkgroup, 43);
    LS_EQ_UINT(tune_count, 1);
    LS_EQ_UINT(tunes[0], 851200000);
    frame(15);
    LS_EQ_INT(follower.state, P25_GRANT_ON_CONTROL);
}

LS_CASE(unsupported_held_priority_grant_does_not_preempt_or_refresh_phase1)
{
    setup();
    uint8_t b[2][12];
    fdma_iden(b[0], 3); make_group_grant(b[1], 0x300f, 43); tsdu(b, 2);
    int64_t activity = follower.last_activity_us;
    p25_scan_hold_set(&scanner, 42);
    make_tdma_iden(b[0], 2); make_group_grant(b[1], 0x200f, 42); tsdu(b, 2);
    LS_EQ_UINT(tune_count, 1);
    LS_EQ_UINT(follower.active_call.talkgroup, 43);
    LS_EQ_INT(follower.last_activity_us, activity);
    LS_EQ_UINT(follower.observed_grant.slot, 1);
    LS_EQ_INT(follower.observed_grant.support, P25_CALL_UNSUPPORTED);
    LS_CHECK(p25_grant_tick(&follower, activity + 2000000));
    p25_receive_call_reset(&decoder);
    LS_EQ_UINT(tunes[1], 852000000);
}

LS_CASE(unknown_identifier_and_malformed_grants_do_not_replay_or_tune)
{
    setup();
    uint8_t b[12];
    make_group_grant(b, 0x200f, 42);
    LS_CHECK(p25_tsbk_parse(&decoder, b));
    LS_CHECK(!p25_scan_apply_from_state(&scanner, &follower, &decoder, now_us));
    LS_EQ_INT(follower.observed_grant.support, P25_CALL_MISSING_IDEN);
    fdma_iden(b, 2); LS_CHECK(p25_tsbk_parse(&decoder, b));
    for (int kind = 0; kind < 5; kind++) {
        make_group_grant(b, 0x200f, 42);
        if (kind == 0) set_bits(b, 40, 16, 0);
        if (kind == 1) b[1] = 0x90;
        if (kind == 2) b[0] |= 0x40;
        if (kind == 3) set_bits(b, 24, 16, 0xffff);
        add_crc(b);
        if (kind == 4) b[5] ^= 1;
        (void)p25_tsbk_parse(&decoder, b);
        LS_CHECK(!p25_scan_apply_from_state(&scanner, &follower, &decoder, now_us));
    }
    LS_EQ_UINT(tune_count, 0);
    make_group_grant(b, 0x200f, 42); LS_CHECK(p25_tsbk_parse(&decoder, b));
    decoder.nac = 0x293; decoder.p25_grants[0].nac = 0x293;
    LS_CHECK(p25_scan_apply_from_state(&scanner, &follower, &decoder, now_us));
    LS_CHECK(p25_grant_force_return_to_control(&follower));
    LS_CHECK(!p25_scan_apply_from_state(&scanner, &follower, &decoder, now_us));
    LS_EQ_UINT(tune_count, 2);
}

LS_CASE(system_change_clears_bandplan_pending_grants_and_encrypted_skip)
{
    setup();
    uint8_t b[3][12];
    network(b[0], 0xabcde, 0x123); tsdu(b, 1);
    fdma_iden(b[0], 2); make_group_grant(b[1], 0x200f, 42); tsdu(b, 2);
    decoder.p25_ess_valid = 1; decoder.p25_algid = 0x84;
    decoder.lastp25type = 1;
    frame(10);
    LS_EQ_UINT(follower.encrypted_returns, 1);
    LS_CHECK(p25_grant_tg_is_skipped(&follower, 42, now_us));
    make_group_grant(b[0], 0x200f, 42);
    network(b[1], 0xabcde, 0x124); tsdu(b, 2);
    LS_CHECK(!p25_grant_tg_is_skipped(&follower, 42, now_us));
    LS_CHECK(!decoder.p25_iden_table[2].valid);
    LS_EQ_INT(follower.observed_grant.support, P25_CALL_NONE);
    LS_EQ_UINT(tune_count, 2);
    make_group_grant(b[0], 0x200f, 42); tsdu(b, 1);
    LS_EQ_INT(follower.observed_grant.support, P25_CALL_MISSING_IDEN);
    fdma_iden(b[0], 2); make_group_grant(b[1], 0x200f, 42); tsdu(b, 2);
    LS_EQ_UINT(tune_count, 3);
    LS_EQ_UINT(follower.active_call.sysid, 0x124);
}

LS_CASE(tdma_signed_offsets_and_invalid_replacement_do_not_reuse_old_plan)
{
    setup();
    uint8_t b[12];
    for (int sign = 0; sign < 2; sign++) {
        make_tdma_iden(b, 15); set_bits(b, 24, 1, sign); add_crc(b);
        LS_CHECK(p25_tsbk_parse(&decoder, b));
        LS_EQ_INT(decoder.p25_iden_table[15].tx_offset_hz, sign ? 45000000 : -45000000);
    }
    for (int invalid = 0; invalid < 3; invalid++) {
        make_tdma_iden(b, 15);
        if (invalid == 0) set_bits(b, 38, 10, 0);
        if (invalid == 1) set_bits(b, 48, 32, 0);
        if (invalid == 2) set_bits(b, 48, 32, UINT32_MAX);
        add_crc(b); LS_CHECK(p25_tsbk_parse(&decoder, b));
        LS_CHECK(!decoder.p25_iden_table[15].valid);
        make_group_grant(b, 0xf001, 42);
        LS_CHECK(p25_tsbk_parse(&decoder, b));
        LS_EQ_UINT(decoder.p25_grants[0].carrier_hz, 0);
    }
    p25_tsbk_reset_system(&decoder);
    LS_EQ_UINT(decoder.p25_grant_count, 0);
    LS_EQ_INT(decoder.p25_net_valid, 0);
}

LS_CASE(ignored_ldu2_and_corrupt_nid_are_not_terminators_or_audio)
{
    setup();
    uint8_t b[2][12];
    fdma_iden(b[0], 2); make_group_grant(b[1], 0x200f, 42); tsdu(b, 2);
    frame(10); /* ignored LDU2, lastp25type=0 is not a terminator */
    LS_EQ_UINT(tune_count, 1);
    LS_EQ_UINT(s_ldu2_calls, 0);
    for (int previous = 1; previous <= 2; previous++) {
        decoder.lastp25type = previous;
        load_frame(1, 0); processFrame(&options, &decoder);
        LS_EQ_UINT(s_ldu1_calls + s_ldu2_calls, 0);
        LS_EQ_INT(decoder.p25_frame_valid, 0);
    }
    load_frame(5, 0);
    /* Deterministic damage across the BCH parity, leaving a plausible raw LDU1. */
    for (int i = 14; i < 31; i++) s_input[i] ^= (i & 1) ? 3 : 2;
    decoder.pcm_out_write = 0;
    processFrame(&options, &decoder);
    LS_EQ_INT(decoder.p25_frame_valid, 0);
    LS_EQ_UINT(s_ldu1_calls + s_ldu2_calls, 0);
    (void)p25_receive_frame(&scanner, &follower, &decoder, now_us, true);
    LS_EQ_UINT(tune_count, 1);
    LS_EQ_INT(decoder.pcm_out_write, 0);
}

LS_CASE(return_reset_clears_lcw_ess_and_pcm_but_preserves_identifiers)
{
    setup();
    uint8_t b[12]; fdma_iden(b, 2); LS_CHECK(p25_tsbk_parse(&decoder, b));
    decoder.p25_ess_valid = 1; decoder.p25_algid = 0x80;
    decoder.p25_lcw_valid = 1; decoder.p25_lcw_emergency = 1;
    decoder.p25_lcw_talkgroup = 42; decoder.lasttg = 42; decoder.lastsrc = 123;
    decoder.p25_lcw_alias_ready = 1; strcpy(decoder.p25_lcw_alias, "old call");
    decoder.pcm_out_write = 160; decoder.lastp25type = 2;
    p25_receive_call_reset(&decoder);
    LS_EQ_INT(decoder.p25_ess_valid, 0); LS_EQ_INT(decoder.p25_lcw_valid, 0);
    LS_EQ_INT(decoder.p25_lcw_emergency, 0); LS_EQ_INT(decoder.p25_lcw_alias_ready, 0);
    LS_EQ_INT(decoder.lasttg, 0); LS_EQ_INT(decoder.lastsrc, 0);
    LS_EQ_INT(decoder.pcm_out_write, 0); LS_EQ_INT(decoder.lastp25type, 0);
    LS_CHECK(decoder.p25_iden_table[2].valid);
}

LS_CASE(control_nac_change_retires_plan_before_first_grant)
{
    setup();
    uint8_t b[2][12];
    fdma_iden(b[0], 2); tsdu(b, 1);
    s_nac = 0x294;
    make_group_grant(b[0], 0x200f, 42); tsdu(b, 1);
    LS_EQ_UINT(tune_count, 0);
    LS_CHECK(!decoder.p25_iden_table[2].valid);
    LS_EQ_INT(follower.observed_grant.support, P25_CALL_MISSING_IDEN);
    LS_EQ_UINT(follower.observed_grant.nac, 0x294);
    fdma_iden(b[0], 2); make_group_grant(b[1], 0x200f, 42); tsdu(b, 2);
    LS_EQ_UINT(tune_count, 1);
    s_nac = 0x293;
    frame(3); /* foreign NAC terminator must not terminate this call */
    LS_EQ_UINT(tune_count, 1);
    LS_EQ_INT(follower.receive_state, P25_RX_TRAFFIC_WAIT);
}

LS_CASE(valid_new_lcw_is_retained_while_old_ess_alias_and_pcm_are_retired)
{
    setup();
    decoder.lasttg = 42;
    decoder.p25_lcw_valid = 1; decoder.p25_lcw_talkgroup = 42;
    decoder.p25_ess_valid = 1; decoder.p25_algid = 0x80;
    decoder.p25_lcw_alias_ready = 1; strcpy(decoder.p25_lcw_alias, "previous");
    s_voice_tg = 43;
    frame(5);
    LS_EQ_UINT(decoder.p25_lcw_talkgroup, 43);
    LS_EQ_INT(decoder.p25_lcw_valid, 1);
    LS_EQ_INT(decoder.p25_lcw_alias_ready, 0);
    LS_EQ_INT(decoder.p25_ess_valid, 0);
    LS_EQ_INT(decoder.pcm_out_write, 0);
}

LS_CASE(contradictory_lcw_returns_without_stamping_wrong_talkgroup_skip)
{
    setup();
    uint8_t b[2][12];
    fdma_iden(b[0], 2); make_group_grant(b[1], 0x200f, 42); tsdu(b, 2);
    s_voice_tg = 43;
    frame(5);
    LS_EQ_UINT(tune_count, 2);
    LS_EQ_UINT(follower.encrypted_returns, 0);
    LS_EQ_INT(decoder.pcm_out_write, 0);
    LS_EQ_INT(follower.active_call.support, P25_CALL_NONE);
}

LS_CASE(new_talker_on_same_carrier_retires_prior_clear_ess_without_retune)
{
    setup();
    uint8_t b[2][12];
    fdma_iden(b[0], 2); make_group_grant(b[1], 0x200f, 42); tsdu(b, 2);
    decoder.p25_ess_valid = 1; decoder.p25_algid = 0x80;
    make_group_grant(b[0], 0x200f, 42);
    set_bits(b[0], 56, 24, 999); tsdu(b, 1);
    LS_EQ_UINT(tune_count, 1);
    LS_EQ_UINT(follower.active_call.source, 999);
    LS_EQ_INT(decoder.p25_ess_valid, 0);
    frame(5);
    LS_EQ_INT(decoder.pcm_out_write, 0);
}
