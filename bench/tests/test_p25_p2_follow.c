/* How long a followed Phase II call lasts (p25_p2_follow.c).
 *
 * The first cases drive the policy with hand-built decoder status. The last
 * ones follow the public mixed Phase I/II recording (docs/P25_PHASE2.md) the
 * way the firmware would: a grant arrives, the decoder is configured fresh
 * for the slot, and the follower is ticked every 10 ms of air until it says
 * to go back. Every transmission in it must be heard to its end and left
 * promptly after. The recording is not redistributed: set LS_P25P2_CAPTURE
 * (bench/tools/check_p25p2_sample.py does). Without it those cases say so
 * and pass.
 */
#include "ls_test.h"
#include "p25_p2_follow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static p25_p2_follow_t f;
static p25p2_status_t st;

static void begin(uint16_t tg)
{
    p25_p2_follow_init(&f);
    memset(&st, 0, sizeof(st));
    st.algorithm = 0xff;
    p25_p2_follow_start(&f, tg, 0);
}

/* One burst of sync, and optionally a MAC PDU or voice, at a time in ms. */
static void burst(uint32_t ms, int opcode, int voice)
{
    st.symbols = ms * 6;
    st.bursts++;
    st.synchronized = true;
    if (voice) {
        st.voice_frames += 2;
        st.last_voice_symbol = st.symbols;
    }
    if (opcode >= 0) {
        st.control_ok++;
        st.mac_opcodes[opcode]++;
        st.last_mac_opcode = (uint8_t)opcode;
        st.last_mac_symbol = st.symbols + (voice ? 0 : 1);
    }
}

static p25_p2f_verdict_t at(uint32_t ms) { return p25_p2_follow_tick(&f, &st, ms); }

LS_CASE(a_call_is_held_through_its_voice_and_left_half_a_second_after_end_ptt)
{
    begin(2222);
    LS_EQ_INT(p25_p2_follow_tick(&f, NULL, 200), P25_P2F_STAY);
    burst(300, P25P2_MAC_PTT, 0);
    LS_EQ_INT(at(300), P25_P2F_STAY);
    LS_EQ_INT(f.phase, P25_P2F_CALL);
    for (uint32_t t = 330; t < 5000; t += 30) {
        burst(t, (t % 360) < 30 ? P25P2_MAC_ACTIVE : -1, 1);
        LS_EQ_INT(at(t), P25_P2F_STAY);
    }
    burst(5030, P25P2_MAC_END_PTT, 0);
    LS_EQ_INT(at(5030), P25_P2F_STAY);
    LS_EQ_INT(f.phase, P25_P2F_ENDING);
    LS_EQ_INT(at(5500), P25_P2F_STAY);
    LS_EQ_INT(at(5530), P25_P2F_LEAVE_ENDED);
    LS_EQ_INT(f.phase, P25_P2F_OFF);
    LS_EQ_UINT(f.leaves[P25_P2F_LEAVE_ENDED], 1);
}

LS_CASE(a_new_ptt_after_end_ptt_keeps_the_call)
{
    begin(7);
    burst(100, P25P2_MAC_PTT, 0);
    at(100);
    burst(400, P25P2_MAC_END_PTT, 0);
    at(400);
    burst(700, P25P2_MAC_PTT, 0);
    LS_EQ_INT(at(700), P25_P2F_STAY);
    LS_EQ_INT(f.phase, P25_P2F_CALL);
    LS_EQ_INT(at(1600), P25_P2F_STAY);
    LS_EQ_INT(at(1700), P25_P2F_LEAVE_LOST);
}

LS_CASE(hangtime_holds_the_channel_for_the_hang_and_no_longer)
{
    begin(7);
    burst(100, P25P2_MAC_PTT, 0);
    at(100);
    burst(400, P25P2_MAC_END_PTT, 0);
    at(400);
    /* The system repeats HANGTIME; the deadline is set by the first. */
    for (uint32_t t = 430; t < 2400; t += 60) {
        burst(t, P25P2_MAC_HANGTIME, 0);
        LS_EQ_INT(at(t), P25_P2F_STAY);
        LS_EQ_INT(f.phase, P25_P2F_HANG);
    }
    burst(2430, P25P2_MAC_HANGTIME, 0);
    LS_EQ_INT(at(2430), P25_P2F_LEAVE_HANG);
}

LS_CASE(a_reply_during_hangtime_brings_the_call_back)
{
    begin(7);
    burst(100, P25P2_MAC_HANGTIME, 0);
    at(100);
    burst(1500, P25P2_MAC_PTT, 0);
    LS_EQ_INT(at(1500), P25_P2F_STAY);
    LS_EQ_INT(f.phase, P25_P2F_CALL);
    for (uint32_t t = 1530; t < 4000; t += 30) {
        burst(t, -1, 1);
        LS_EQ_INT(at(t), P25_P2F_STAY);
    }
}

LS_CASE(an_idle_slot_is_left_at_once)
{
    begin(7);
    burst(200, P25P2_MAC_IDLE, 0);
    LS_EQ_INT(at(200), P25_P2F_LEAVE_IDLE);
}

LS_CASE(a_slot_that_never_syncs_is_left_after_the_acquire_time)
{
    begin(7);
    LS_EQ_INT(p25_p2_follow_tick(&f, NULL, 1000), P25_P2F_STAY);
    LS_EQ_INT(at(1400), P25_P2F_STAY);
    LS_EQ_INT(at(1500), P25_P2F_LEAVE_NO_SYNC);
}

LS_CASE(losing_sync_mid_call_ends_it_after_the_lost_time)
{
    begin(7);
    burst(100, P25P2_MAC_PTT, 0);
    at(100);
    burst(400, -1, 1);
    at(400);
    LS_EQ_INT(at(1300), P25_P2F_STAY);
    LS_EQ_INT(at(1400), P25_P2F_LEAVE_LOST);
}

LS_CASE(an_encrypted_ptt_is_left_at_once)
{
    begin(7);
    st.algorithm = 0x84;
    burst(100, P25P2_MAC_PTT, 0);
    LS_EQ_INT(at(100), P25_P2F_LEAVE_ENCRYPTED);
}

LS_CASE(a_ptt_for_another_talkgroup_is_not_ours)
{
    begin(7);
    st.talkgroup = 8;
    burst(100, P25P2_MAC_PTT, 0);
    LS_EQ_INT(at(100), P25_P2F_LEAVE_OTHER_TG);
}

LS_CASE(voice_without_a_valid_mac_pdu_does_not_hold_the_channel)
{
    /* The wrong WACN/system/NAC: voice bursts are recognised before they
       are descrambled, but no scrambled PDU passes its CRC. */
    begin(7);
    for (uint32_t t = 100; t < 3500; t += 30) {
        burst(t, -1, 1);
        LS_EQ_INT(at(t), P25_P2F_STAY);
        LS_EQ_INT(f.phase, P25_P2F_ACQUIRE);
    }
    burst(3510, -1, 1);
    LS_EQ_INT(at(3510), P25_P2F_LEAVE_QUIET);
}

LS_CASE(voice_in_the_same_tick_as_end_ptt_does_not_undo_it)
{
    begin(7);
    burst(100, P25P2_MAC_PTT, 0);
    at(100);
    /* Voice, then END_PTT 60 ms later, both before the next tick. */
    burst(400, -1, 1);
    burst(460, P25P2_MAC_END_PTT, 0);
    LS_EQ_INT(at(470), P25_P2F_STAY);
    LS_EQ_INT(f.phase, P25_P2F_ENDING);
}

LS_CASE(a_restarted_decoder_is_not_mistaken_for_no_progress)
{
    begin(7);
    burst(100, P25P2_MAC_PTT, 0);
    at(100);
    for (uint32_t t = 130; t < 600; t += 30) { burst(t, -1, 1); at(t); }
    /* The decoder was recreated: its counts start again from zero. */
    memset(&st, 0, sizeof(st));
    st.algorithm = 0xff;
    burst(900, P25P2_MAC_ACTIVE, 0);
    LS_EQ_INT(at(900), P25_P2F_STAY);
    LS_EQ_INT(f.phase, P25_P2F_CALL);
    LS_EQ_UINT(f.seen_control, 1);
}

/* ------------------------------------------------ the public recording -- */

#define WACN 0x92715
#define SYSID 0x1f6
#define NAC 0x01a
#define TICK 60 /* symbols: 10 ms at 6000 baud */

static uint8_t *capture;
static size_t capture_n;
static uint32_t voice_heard;

static void quiet(const int16_t *pcm, size_t n, void *ctx) { (void)pcm; (void)n; (void)ctx; }

static bool load(void)
{
    if (capture) return true;
    const char *path = getenv("LS_P25P2_CAPTURE");
    FILE *in = path ? fopen(path, "rb") : NULL;
    if (!in) return false;
    capture = malloc(500000);
    capture_n = capture ? fread(capture, 1, 500000, in) : 0;
    fclose(in);
    return capture_n > 0;
}

/* Follow a grant made at `grant_s` on `slot`; returns the leave time. */
static double follow(double grant_s, unsigned slot, uint32_t wacn,
                     p25_p2f_verdict_t *verdict)
{
    p25p2_decoder_t *d = p25p2_create(quiet, NULL);
    p25p2_configure(d, wacn, SYSID, NAC, slot);
    p25_p2_follow_init(&f);
    size_t at_sym = (size_t)(grant_s * 6000.0);
    p25_p2_follow_start(&f, 2222, (uint32_t)(at_sym / 6));
    *verdict = P25_P2F_STAY;
    p25p2_status_t s = {0};
    while (at_sym + TICK <= capture_n && *verdict == P25_P2F_STAY) {
        p25p2_push(d, capture + at_sym, TICK);
        at_sym += TICK;
        p25p2_status(d, &s);
        *verdict = p25_p2_follow_tick(&f, &s, (uint32_t)(at_sym / 6));
    }
    voice_heard += s.voice_frames;
    p25p2_destroy(d);
    return at_sym / 6000.0;
}

/* The six transmissions on slot 0, from `p25p2_replay ... --timeline`:
   the voice run's first and last frame, in seconds. The grant is placed
   just before each, as the control channel would. */
static const struct { double grant, first, last; unsigned frames; } calls[] = {
    { 20.0, 20.129, 22.469, 124 },
    { 26.7, 27.021, 28.881,  98 },
    { 31.6, 31.757, 48.437, 840 },
    { 51.2, 51.380, 53.660, 120 },
    { 56.3, 56.431, 57.691,  70 },
    { 60.1, 60.287, 62.507, 116 },
};

LS_CASE(every_call_in_the_public_recording_is_heard_to_its_end_and_left_after_it)
{
    if (!load()) {
        printf("  (LS_P25P2_CAPTURE not set: recording cases skipped)\n");
        return;
    }
    voice_heard = 0;
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        p25_p2f_verdict_t v;
        double left = follow(calls[i].grant, 0, WACN, &v);
        printf("  call %zu: grant %.3f s, left %.3f s (%s), %.0f ms after "
               "its last voice\n", i + 1, calls[i].grant, left,
               p25_p2_follow_verdict_name(v),
               (left - calls[i].last) * 1000.0);
        LS_EQ_INT(v, P25_P2F_LEAVE_ENDED);
        LS_CHECK(left > calls[i].last);
        LS_CHECK(left - calls[i].last < 1.0);
    }
    /* Every voice frame on the air was inside a followed window: the
       follower never cut a transmission short. */
    LS_EQ_UINT(voice_heard, 1368);
}

LS_CASE(the_recordings_idle_slot_is_left_within_a_burst_or_two)
{
    if (!load()) return;
    p25_p2f_verdict_t v;
    double left = follow(20.0, 1, WACN, &v);
    printf("  idle slot: left %.3f s (%s)\n", left, p25_p2_follow_verdict_name(v));
    LS_EQ_INT(v, P25_P2F_LEAVE_IDLE);
    LS_CHECK(left < 20.5);
}

LS_CASE(the_wrong_system_does_not_hold_the_recordings_longest_call)
{
    /* Descrambled with the wrong WACN: voice bursts, no valid PDU until the
       unscrambled END_PTT - or the quiet limit, whichever is first. */
    if (!load()) return;
    p25_p2f_verdict_t v;
    double left = follow(31.6, 0, 0x12345, &v);
    printf("  wrong system: left %.3f s (%s)\n", left, p25_p2_follow_verdict_name(v));
    LS_EQ_INT(v, P25_P2F_LEAVE_QUIET);
    LS_CHECK(left < 31.6 + 3.6);
}

