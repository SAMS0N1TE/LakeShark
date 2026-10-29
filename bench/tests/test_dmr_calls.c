/* LS_TEST_SOURCES: ${DMR_WATCH_SRCS}
 *
 * Both slots of a repeater, call by call: voice headers, voice superframes
 * with embedded LC, terminators and timeouts, talker alias and GPS riding
 * the embedded LC, CSBKs and data headers in between - fed as symbols, the
 * way the P25 frame-sync observer hands them over, so these are about what
 * the watcher does with bursts rather than about timing recovery. */

#include "ls_test.h"

#include "dmr.h"
#include "dmr_watch.h"
#include "dmr_gen.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const uint8_t BS_VOICE[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };
static const uint8_t BS_DATA [6] = { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D };
/* Systematic TACT, AT=1, TC=0/1, LCSS=00 - slot 1 and slot 2. */
static const uint8_t CACH[2][3] = { {0x80, 0x02, 0x02}, {0x88, 0x00, 0x20} };

#define CC 5

static bool s_invert;

static void feed_bits(const uint8_t *bits, unsigned n_bits)
{
    for (unsigned i = 0; i + 1 < n_bits; i += 2) {
        const int hi = (bits[i >> 3] >> (7u - (i & 7u))) & 1u;
        const int lo = (bits[(i + 1) >> 3] >> (7u - ((i + 1) & 7u))) & 1u;
        const int sym = !hi ? (lo ? 3600 : 1200) : (lo ? -3600 : -1200);
        dmr_watch_symbol(s_invert ? -sym : sym, 0, 2400, -2400);
    }
}

static void slot_burst(unsigned slot, const uint8_t burst[33])
{
    feed_bits(CACH[slot - 1], 24);
    feed_bits(burst, DMR_BURST_BITS);
}

static void idle_burst(unsigned slot)
{
    uint8_t block[12] = {0}, b[33];
    dmr_gen_data_burst(b, BS_DATA, CC, 9, block);
    slot_burst(slot, b);
}

static void lc9_of(uint8_t flco, uint32_t dst, uint32_t src, uint8_t opts, uint8_t lc9[9])
{
    dmr_lc_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.flco = flco;
    lc.destination = dst;
    lc.source = src;
    lc.service_options = opts;
    dmr_lc_pack(&lc, lc9);
}

static void lc_burst(unsigned slot, uint8_t dt, const uint8_t lc9[9], int corrupt_octet)
{
    uint8_t block[12], b[33];
    dmr_gen_lc_signed(lc9, dt, block);
    if (corrupt_octet >= 0) block[corrupt_octet] ^= 0x5A;
    dmr_gen_data_burst(b, BS_DATA, CC, dt, block);
    slot_burst(slot, b);
}

/* One frame: a burst in `slot`, the other slot idle - or, if `other` is
   given, that burst in the other slot. */
static void frame(unsigned slot, const uint8_t burst[33], const uint8_t *other)
{
    const unsigned o = 3u - slot;
    if (slot == 1) { slot_burst(1, burst); if (other) slot_burst(o, other); else idle_burst(o); }
    else { if (other) slot_burst(o, other); else idle_burst(o); slot_burst(2, burst); }
}

/* A voice superframe in `slot` whose embedded LC is lc9: A carries the
   sync, B-E the four fragments, F a null single fragment. */
static void superframe(unsigned slot, const uint8_t lc9[9])
{
    uint32_t frag[4];
    dmr_emb_lc_encode(lc9, frag);
    static const uint8_t lcss[4] = { DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST };
    uint8_t b[33];
    dmr_gen_voice_burst(b, BS_VOICE, 0, 0, 1);
    frame(slot, b, NULL);
    for (unsigned i = 0; i < 4; i++) {
        dmr_gen_voice_burst(b, NULL, dmr_emb_encode(CC, false, lcss[i]), frag[i], 2 + i);
        frame(slot, b, NULL);
    }
    dmr_gen_voice_burst(b, NULL, dmr_emb_encode(CC, false, DMR_LCSS_SINGLE), 0, 6);
    frame(slot, b, NULL);
}

static void start(bool invert)
{
    s_invert = invert;
    ls_shim_time_set(1000000);
    dmr_watch_reset();
    idle_burst(1);
    idle_burst(2);
}

static dmr_watch_t snap(void)
{
    dmr_watch_t w;
    memset(&w, 0, sizeof(w));
    dmr_watch_get(&w);
    return w;
}

LS_CASE(a_group_call_runs_header_superframes_terminator)
{
    uint8_t id[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 2621441, 0, id);
    start(false);
    lc_burst(1, 1, id, -1);
    idle_burst(2);
    superframe(1, id);
    superframe(1, id);
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.lc_ok, 1);
    LS_EQ_UINT(w.emb_lc_ok, 2);
    LS_EQ_UINT(w.voice_bursts, 10);
    LS_EQ_INT(w.polarity, 0);
    LS_CHECK(w.call[0].active && w.call[0].group && !w.call[0].late_entry);
    LS_EQ_UINT(w.call[0].lc.destination, 9);
    LS_EQ_UINT(w.call[0].lc.source, 2621441);
    LS_EQ_UINT(w.call[0].colour_code, CC);
    LS_EQ_UINT(w.call[0].voice_bursts, 12);       /* A-F twice */
    LS_CHECK(!w.call[1].have_lc);

    ls_shim_time_advance(250000);
    lc_burst(1, 2, id, -1);
    w = snap();
    LS_EQ_UINT(w.lc_ok, 2);
    LS_CHECK(!w.call[0].active);
    LS_EQ_UINT(w.call[0].end_reason, DMR_END_TERMINATOR);
    LS_EQ_INT((int)(w.call[0].end_us - w.call[0].start_us), 250000);
    LS_EQ_INT((int)dmr_watch_verdict(&w), (int)DMR_VERDICT_DMR);
}

LS_CASE(a_call_joined_late_takes_its_identity_from_the_embedded_lc)
{
    /* No header: the receiver tuned in mid-call. The voice syncs say
       nothing about who is talking; the embedded LC does. */
    uint8_t id[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 3100, 3106000, 0, id);
    start(false);
    superframe(1, id);
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.lc_ok, 0);
    LS_EQ_UINT(w.emb_lc_ok, 1);
    LS_CHECK(w.call[0].have_lc && w.call[0].active && w.call[0].late_entry);
    LS_EQ_UINT(w.call[0].lc.destination, 3100);
    /* One embedded LC is a 5-bit checksum behind its Hamming rows: shown,
       not yet evidence. */
    LS_EQ_INT((int)dmr_watch_verdict(&w), (int)DMR_VERDICT_NOT_DMR);
    superframe(1, id);
    w = snap();
    LS_EQ_UINT(w.emb_lc_ok, 2);
    LS_EQ_INT((int)dmr_watch_verdict(&w), (int)DMR_VERDICT_DMR);
    /* A header for the same call clears the late-entry mark, keeps the start. */
    const int64_t started = w.call[0].start_us;
    lc_burst(1, 1, id, -1);
    w = snap();
    LS_CHECK(!w.call[0].late_entry);
    LS_EQ_INT((int)(w.call[0].start_us - started), 0);
}

LS_CASE(the_embedded_lc_upside_down_settles_the_polarity)
{
    uint8_t id[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 3100, 3106000, 0, id);
    start(true);
    superframe(1, id);
    superframe(1, id);
    dmr_watch_t w = snap();
    LS_EQ_INT(w.polarity, 1);
    LS_EQ_UINT(w.emb_lc_ok, 2);
    LS_CHECK(w.call[0].have_lc);
    LS_EQ_UINT(w.call[0].lc.source, 3106000);
}

LS_CASE(talker_alias_and_gps_ride_the_embedded_lc_into_the_call)
{
    uint8_t id[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 2621441, 0, id);
    static const uint8_t ta_hdr[9] = {0x04, 0x00, 0x15, 0x39, 0x84, 0x38, 0x33, 0x26, 0x20};
    static const uint8_t ta_b1[9]  = {0x05, 0x00, 0x85, 0xBF, 0x10, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t gps[9]    = {0x08, 0x00, 0x02, 0x13, 0x10, 0x9D, 0x4A, 0xB1, 0xF3};
    start(false);
    lc_burst(1, 1, id, -1);
    idle_burst(2);
    superframe(1, id);
    superframe(1, ta_hdr);
    superframe(1, ta_b1);
    superframe(1, gps);
    superframe(1, id);
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.emb_lc_ok, 5);
    LS_EQ_STR(w.call[0].alias.text, "N0CALL Bob");
    LS_CHECK(w.call[0].alias.complete);
    LS_CHECK(w.call[0].have_gps);
    LS_NEAR(w.call[0].gps.latitude, 52.52, 0.001);
    LS_EQ_UINT(w.call[0].lc.source, 2621441);    /* identity untouched */

    char text[1024];
    dmr_watch_format(&w, esp_timer_get_time(), text, sizeof(text));
    LS_CHECK_MSG(strstr(text, "slot 1: LIVE group 2621441 -> TG 9 cc=5") != NULL, "%s", text);
    LS_CHECK_MSG(strstr(text, "alias \"N0CALL Bob\"") != NULL, "%s", text);
    LS_CHECK_MSG(strstr(text, "gps 52.52") != NULL, "%s", text);
    LS_CHECK_MSG(strstr(text, "verdict: DMR - 1 link control") != NULL, "%s", text);
}

LS_CASE(two_slots_carry_two_calls_that_do_not_mix)
{
    uint8_t grp[9], priv[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 2621441, 0, grp);
    lc9_of(DMR_FLCO_UNIT_VOICE, 3120101, 3120102, 0x40, priv);
    start(false);
    lc_burst(1, 1, grp, -1);
    lc_burst(2, 1, priv, -1);

    /* Interleaved superframes: each slot's voice bursts are 60 ms apart,
       the other slot's sit between them. */
    uint32_t f1[4], f2[4];
    dmr_emb_lc_encode(grp, f1);
    dmr_emb_lc_encode(priv, f2);
    static const uint8_t lcss[4] = { DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST };
    for (unsigned sf = 0; sf < 2; sf++) {
        uint8_t a[33], b[33];
        dmr_gen_voice_burst(a, BS_VOICE, 0, 0, 10);
        dmr_gen_voice_burst(b, BS_VOICE, 0, 0, 11);
        slot_burst(1, a);
        slot_burst(2, b);
        for (unsigned i = 0; i < 5; i++) {
            const uint8_t ls = i < 4 ? lcss[i] : DMR_LCSS_SINGLE;
            dmr_gen_voice_burst(a, NULL, dmr_emb_encode(CC, false, ls), i < 4 ? f1[i] : 0, 20 + i);
            dmr_gen_voice_burst(b, NULL, dmr_emb_encode(CC, false, ls), i < 4 ? f2[i] : 0, 30 + i);
            slot_burst(1, a);
            slot_burst(2, b);
        }
    }
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.emb_lc_ok, 4);
    LS_CHECK(w.call[0].active && w.call[1].active);
    LS_CHECK(w.call[0].group && !w.call[1].group);
    LS_EQ_UINT(w.call[0].lc.destination, 9);
    LS_EQ_UINT(w.call[1].lc.destination, 3120101);
    LS_EQ_UINT(w.call[1].lc.source, 3120102);
    LS_CHECK(!w.call[0].encrypted && w.call[1].encrypted);
    LS_EQ_UINT(w.tracker.slot[0].bursts, w.tracker.slot[1].bursts);

    /* Slot 2 ends; slot 1 carries on. */
    lc_burst(2, 2, priv, -1);
    w = snap();
    LS_CHECK(w.call[0].active && !w.call[1].active);
}

LS_CASE(a_slot_that_goes_quiet_times_its_call_out)
{
    uint8_t id[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 2621441, 0, id);
    start(false);
    lc_burst(1, 1, id, -1);
    idle_burst(2);
    dmr_watch_t w = snap();
    LS_CHECK(w.call[0].active);
    const int64_t last = w.call[0].last_us;
    ls_shim_time_advance(DMR_CALL_TIMEOUT_US + 100000);
    for (unsigned i = 0; i < 4; i++) { idle_burst(1); idle_burst(2); }
    w = snap();
    LS_CHECK(!w.call[0].active);
    LS_EQ_UINT(w.call[0].end_reason, DMR_END_TIMEOUT);
    LS_EQ_INT((int)(w.call[0].end_us - last), 0);
}

LS_CASE(a_different_talker_on_a_live_slot_is_a_new_call)
{
    uint8_t a[9], b[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 1111111, 0, a);
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 2222222, 0, b);
    start(false);
    lc_burst(1, 1, a, -1);
    idle_burst(2);
    const int64_t first = snap().call[0].start_us;
    ls_shim_time_advance(500000);
    lc_burst(1, 1, b, -1);
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.call[0].lc.source, 2222222);
    LS_EQ_INT((int)(w.call[0].start_us - first), 500000);
}

LS_CASE(csbk_and_data_header_are_decoded_but_are_not_evidence)
{
    static const uint8_t csbk[12] =
        {0x84, 0x00, 0x00, 0x00, 0x2F, 0x9B, 0xE5, 0x2F, 0x9B, 0xE6, 0xE2, 0x84};
    static const uint8_t dh[12] =
        {0x82, 0x43, 0x00, 0x00, 0x09, 0x28, 0x00, 0x01, 0x83, 0x08, 0xC1, 0x98};
    uint8_t b[33];
    start(false);
    dmr_gen_data_burst(b, BS_DATA, CC, 3, csbk);
    slot_burst(2, b);
    dmr_gen_data_burst(b, BS_DATA, CC, 6, dh);
    slot_burst(1, b);
    idle_burst(2);
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.csbk_ok, 1);
    LS_CHECK(w.have_csbk);
    LS_EQ_UINT(w.csbk.opcode, DMR_CSBKO_UU_V_REQ);
    LS_EQ_UINT(w.csbk.source, 3120102);
    LS_EQ_UINT(w.csbk_slot, 2);
    LS_EQ_UINT(w.data_hdr_ok, 1);
    LS_EQ_UINT(w.data_hdr.dpf, DMR_DPF_UNCONFIRMED);
    LS_EQ_UINT(w.data_hdr.destination, 9);
    LS_EQ_UINT(w.data_hdr_slot, 1);
    LS_EQ_INT(w.polarity, 0);
    LS_EQ_INT((int)dmr_watch_verdict(&w), (int)DMR_VERDICT_NOT_DMR);

    char text[1024];
    dmr_watch_format(&w, esp_timer_get_time(), text, sizeof(text));
    LS_CHECK_MSG(strstr(text, "last CSBK: UU voice request") != NULL, "%s", text);
    LS_CHECK_MSG(strstr(text, "last data header: unconfirmed") != NULL, "%s", text);
    LS_CHECK_MSG(strstr(text, "verdict: no DMR") != NULL, "%s", text);
}

LS_CASE(an_rs_corrected_lc_is_used_but_needs_a_second_to_count)
{
    uint8_t id[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 2621441, 0, id);
    start(false);
    lc_burst(1, 1, id, 4);
    idle_burst(2);
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.lc_ok, 0);
    LS_EQ_UINT(w.lc_fixed, 1);
    LS_CHECK(w.call[0].have_lc && w.call[0].corrected);
    LS_EQ_UINT(w.call[0].lc.source, 2621441);
    LS_EQ_INT((int)dmr_watch_verdict(&w), (int)DMR_VERDICT_NOT_DMR);
    lc_burst(1, 1, id, 10);
    idle_burst(2);
    w = snap();
    LS_EQ_UINT(w.lc_fixed, 2);
    LS_EQ_INT((int)dmr_watch_verdict(&w), (int)DMR_VERDICT_DMR);
    /* An exact one later clears the mark on the call. */
    lc_burst(1, 1, id, -1);
    w = snap();
    LS_CHECK(!w.call[0].corrected);
}

LS_CASE(a_burst_framed_both_ways_round_is_counted_once)
{
    /* Voice and data syncs are each other's negation, so every burst is
       framed by both framers until a checked block says which is right. */
    uint8_t id[9];
    lc9_of(DMR_FLCO_GROUP_VOICE, 9, 2621441, 0, id);
    s_invert = false;
    dmr_watch_reset();
    lc_burst(1, 1, id, -1);
    dmr_watch_t w = snap();
    LS_EQ_UINT(w.bursts, 1);
    LS_EQ_UINT(w.lc_ok, 1);
    LS_EQ_INT((int)w.last_class, (int)DMR_SYNC_BS_DATA);
    LS_CHECK(!w.inverted);
}
