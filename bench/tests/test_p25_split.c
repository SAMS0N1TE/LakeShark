/* LS_TEST_SOURCES: ${APP}/p25/p25_split.c ${APP}/p25/p25_tsbk.c ${APP}/p25/scan_ctrl.c ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c ${APP}/p25/p25_controls.c */
#include "ls_test.h"
#include "p25_split.h"
#include "p25_tsbk.h"
#include "p25p1_check_nid.c"
#include "../fixtures/p25_tsbk_fixture.h"

static p25_split_t split;
static p25_grant_follower_t follower;
static p25_scan_ctrl_t scanner;
static unsigned tunes, idles, controls;
static uint64_t tuned_hz;
static int64_t now;

static void radio(void *user, uint64_t hz, bool traffic)
{
    (void)user;
    if (!traffic && split.active) { idles++; return; }
    if (traffic) tunes++; else controls++;
    tuned_hz = hz;
}

static bool replay(uint8_t block[12], bool inverted)
{
    uint8_t encoded[98], wire[303];
    encode_block(block, encoded);
    unsigned at = 0;
    for (unsigned i = 0; i < sizeof(wire); ++i)
        wire[i] = i >= 14 && (i - 14) % 36 == 0 ? 0 : at < 98 ? encoded[at++] : 0;
    int data[BCH_KK], parity[BCH_RR];
    char cw[63];
    for (int i = 0; i < 12; ++i) data[i] = (0x293 >> (11 - i)) & 1;
    for (int i = 0; i < 4; ++i) data[12 + i] = (7 >> (3 - i)) & 1;
    bch_encode(data, parity);
    for (int i = 0; i < 16; ++i) cw[i] = (char)data[i];
    for (int i = 0; i < 47; ++i) cw[16 + i] = (char)parity[i];
    uint64_t sync = UINT64_C(0x5575f5ff77ff);
    unsigned inv = inverted ? 2 : 0;
    for (int i = 23; i >= 0; --i)
        (void)p25_split_feed(&split, &scanner, &follower, ((sync >> (i * 2)) & 3) ^ inv, now);
    for (unsigned i = 0; i < 33; ++i) {
        unsigned j = i < 11 ? i : i - 1;
        uint8_t d = i == 11 ? 0 : i == 32 ? cw[62] << 1 : (cw[j * 2] << 1) | cw[j * 2 + 1];
        (void)p25_split_feed(&split, &scanner, &follower, d ^ inv, now);
    }
    bool tuned = false;
    for (unsigned i = 0; i < sizeof(wire); ++i)
        tuned |= p25_split_feed(&split, &scanner, &follower, wire[i] ^ inv, now);
    return tuned;
}

static void setup(void)
{
    now = 0;
    tunes = idles = controls = 0;
    tuned_hz = 0;
    p25_split_init(&split, 851012500, now);
    p25_grant_init(&follower, 851012500, radio, NULL);
    p25_scan_init(&scanner);
    scanner.auto_follow = true;
    uint8_t iden[12];
    make_tdma_iden(iden, 2);
    iden[0] = 0xbd; /* FDMA IDEN_UP, same base/spacing */
    add_crc(iden);
    for (unsigned second = 0; second < 3; ++second) {
        for (unsigned i = 0; i < 5; ++i) {
            now += 200000;
            (void)replay(iden, i & 1);
        }
        (void)p25_split_service(&split, &follower, true, true, now);
    }
    LS_CHECK(split.active);
    LS_EQ_UINT(split.crc_percent, 100);
    LS_EQ_UINT(split.cc.p25_control_nac, 0x293);
}

LS_CASE(grant_call_end_and_hang_leave_lr_on_control_and_rtl_idle)
{
    setup();
    printf("    split core=%u bytes; queue/stage=8704 bytes PSRAM\n", (unsigned)sizeof(split));
    uint8_t grant[12];
    make_group_grant(grant, 0x200f, 42);
    now += 100000;
    LS_CHECK(replay(grant, false));
    LS_EQ_UINT(tunes, 1);
    LS_CHECK(tuned_hz == 851200000);
    LS_CHECK(split.control_hz == 851012500);
    LS_CHECK(p25_grant_on_terminator(&follower, ++now));
    LS_EQ_UINT(idles, 1);
    LS_EQ_UINT(controls, 0);
    now += 100000;
    LS_CHECK(replay(grant, false));
    LS_CHECK(p25_grant_tick(&follower, now + follower.hang_us + 1));
    LS_EQ_UINT(idles, 2);
}

LS_CASE(priority_preempts_hold_and_lower_priority_have_visible_skip)
{
    setup();
    uint8_t grant[12];
    make_group_grant(grant, 0x200f, 42);
    LS_CHECK(replay(grant, false));
    make_group_grant(grant, 0x2010, 43);
    LS_CHECK(!replay(grant, false));
    LS_EQ_INT(split.reason, P25_SCAN_REASON_FOREIGN_GRANT);
    LS_CHECK(p25_scan_priority_set(&scanner, 43, 2));
    LS_CHECK(replay(grant, false));
    LS_EQ_UINT(follower.talkgroup, 43);
    LS_EQ_UINT(scanner.priority_preempts, 1);
    p25_scan_hold_set(&scanner, 43);
    make_group_grant(grant, 0x2011, 44);
    LS_CHECK(!replay(grant, false));
    LS_EQ_INT(split.reason, P25_SCAN_REASON_HOLD_MISMATCH);
    LS_EQ_UINT(split.skipped, 2);
}

LS_CASE(loss_falls_back_and_sustained_good_crc_recovers)
{
    setup();
    uint8_t grant[12];
    make_group_grant(grant, 0x200f, 42);
    LS_CHECK(replay(grant, false));
    now += P25_SPLIT_LOSS_US;
    LS_CHECK(p25_split_service(&split, &follower, true, true, now));
    LS_CHECK(!split.active);
    LS_EQ_UINT(controls, 1);
    LS_CHECK(tuned_hz == follower.control_hz);
    LS_EQ_UINT(split.fallbacks, 1);
    for (unsigned second = 0; second < 2; ++second) {
        for (unsigned i = 0; i < 5; ++i) { now += 200000; (void)replay(grant, false); }
        (void)p25_split_service(&split, &follower, true, true, now);
    }
    LS_CHECK(split.active);
    LS_EQ_UINT(split.recoveries, 2);
    LS_CHECK(replay(grant, true));
    LS_CHECK(p25_split_service(&split, &follower, true, false, ++now));
    LS_CHECK(!split.active);
}

LS_CASE(disabled_split_and_bad_crc_never_follow)
{
    setup();
    LS_CHECK(p25_split_service(&split, &follower, false, true, ++now));
    uint8_t grant[12];
    make_group_grant(grant, 0x200f, 42);
    grant[11] ^= 1;
    LS_CHECK(!replay(grant, false));
    LS_CHECK(split.bad > 0);
    LS_EQ_UINT(tunes, 0);
}

LS_CASE(phase_two_uses_identifier_slot_and_system_identity)
{
    setup();
    uint8_t block[12];
    memset(block, 0, sizeof(block));
    block[0] = 0xbb;
    set_bits(block, 24, 20, 0xabcde);
    set_bits(block, 44, 12, 0x123);
    add_crc(block);
    (void)replay(block, false);
    make_tdma_iden(block, 2);
    (void)replay(block, false);
    make_group_grant(block, 0x201f, 42);
    LS_CHECK(replay(block, false));
    LS_EQ_INT(follower.active_call.support, P25_CALL_PHASE2);
    LS_EQ_UINT(follower.active_call.slot, 1);
    LS_CHECK(follower.active_call.carrier_hz == 851200000);
    LS_EQ_UINT(follower.active_call.wacn, 0xabcde);
    LS_EQ_UINT(follower.active_call.sysid, 0x123);
    LS_EQ_UINT(follower.active_call.nac, 0x293);
}

LS_CASE(poor_crc_with_occasional_good_blocks_still_falls_back)
{
    setup();
    uint8_t block[12];
    make_group_grant(block, 0x200f, 42);
    LS_CHECK(replay(block, false));
    for (unsigned second = 0; second < 5; ++second) {
        for (unsigned i = 0; i < 5; ++i) {
            now += 200000;
            if (i) block[11] ^= 1;
            (void)replay(block, false);
            if (i) block[11] ^= 1;
        }
        (void)p25_split_service(&split, &follower, true, true, now);
    }
    LS_CHECK(!split.active);
    LS_EQ_UINT(split.fallbacks, 1);
    /* A CRC failure also loses the last-block flag, so the parser checks
     * the remaining two blocks. Those failed attempts count in the rate. */
    LS_CHECK(split.crc_percent > 0 && split.crc_percent <= 20);
    setup(); /* leaving/re-entering must start with independent state */
    LS_EQ_UINT(split.fallbacks, 0);
    LS_EQ_UINT(follower.followed_count, 0);
}
