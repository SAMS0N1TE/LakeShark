/* LS_TEST_SOURCES: ${APP}/p25/p25_tsbk.c ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c ${APP}/p25/p25_controls.c */
/* Grant follower: control -> grant -> traffic -> call end -> control.
 *
 * The parser fills state->p25_tsbk_frequency_hz on every grant and until
 * nothing read it. This suite drives the state machine through
 * every path the field description requires, using synthetic TSBKs so it
 * runs with no hardware and no ambient RF. */

#include "ls_test.h"
#include "grant_follower.h"
#include "p25_tsbk.h"

#include <string.h>

/* Test-side witness: what the follower asked the radio to do, in order. */
#define RETUNE_LOG_MAX 16
typedef struct {
    uint64_t hz[RETUNE_LOG_MAX];
    bool     to_traffic[RETUNE_LOG_MAX];
    int      count;
} retune_log_t;

static retune_log_t g_log;

static void retune_cb(void *user, uint64_t center_hz, bool to_traffic)
{
    retune_log_t *log = (retune_log_t *)user;
    if (log->count < RETUNE_LOG_MAX) {
        log->hz[log->count] = center_hz;
        log->to_traffic[log->count] = to_traffic;
    }
    log->count++;
}

static void reset_log(void) { memset(&g_log, 0, sizeof(g_log)); }

/* ------------------------------------------------------------------ */

/* test: this suite is about the follower, not the wire format.        */
/* ------------------------------------------------------------------ */

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
    block[10] = 0;
    block[11] = 0;
    uint16_t crc = p25_tsbk_crc16(block, P25_TSBK_BYTES);
    block[10] = (uint8_t)(crc >> 8);
    block[11] = (uint8_t)(crc & 0xff);
}

static void build_iden(uint8_t block[P25_TSBK_BYTES], uint8_t id,
                       uint16_t spacing_units, uint32_t base_units)
{
    memset(block, 0, P25_TSBK_BYTES);
    block[0] = 0x80 | 0x3d;
    set_bits(block, 16, 4, id);
    set_bits(block, 38, 10, spacing_units);
    set_bits(block, 48, 32, base_units);
    add_crc(block);
}

static void build_grant(uint8_t block[P25_TSBK_BYTES], uint16_t channel,
                        uint16_t talkgroup, uint32_t source)
{
    memset(block, 0, P25_TSBK_BYTES);
    block[0] = 0x80 | 0x00;
    set_bits(block, 24, 16, channel);
    set_bits(block, 40, 16, talkgroup);
    set_bits(block, 56, 24, source);
    add_crc(block);
}

static void build_grant_update(uint8_t block[P25_TSBK_BYTES], uint16_t channel,
                               uint16_t talkgroup)
{
    memset(block, 0, P25_TSBK_BYTES);
    block[0] = 0x80 | 0x02;
    set_bits(block, 16, 16, channel);
    set_bits(block, 32, 16, talkgroup);
    /* second pair is ignored by the current parser but the bits must exist */
    set_bits(block, 48, 16, channel);
    set_bits(block, 64, 16, talkgroup);
    add_crc(block);
}

/* Install IDEN 3, 12.5 kHz spacing, base 850 MHz - the same one the
 * existing tsbk test uses. Channel 0x302a resolves to 850,525,000 Hz. */
static void install_iden(dsd_state *state)
{
    uint8_t iden[P25_TSBK_BYTES];
    build_iden(iden, 3, 100, 170000000);
    LS_EQ_INT(p25_tsbk_parse(state, iden), 1);
}

/* ------------------------------------------------------------------ */
/* Cases                                                               */
/* ------------------------------------------------------------------ */

LS_CASE(follower_starts_on_control_and_never_retunes)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_INT(g_log.count, 0);
    /* Idle tick on control does nothing and issues no retune. */
    LS_CHECK(p25_grant_tick(&f, 100000000LL) == false);
    LS_EQ_INT(g_log.count, 0);
}

LS_CASE(same_talkgroup_on_another_carrier_does_not_extend_old_call)
{
    p25_grant_follower_t f;
    p25_grant_init(&f, 851012500, NULL, NULL);
    LS_CHECK(p25_grant_on_grant(&f, 42, 100, 850525000, 1000000));
    LS_CHECK(!p25_grant_on_grant(&f, 42, 200, 850550000, 2000000));
    LS_EQ_UINT(f.source, 100);
    LS_EQ_UINT(f.last_activity_us, 1000000);
    LS_CHECK(p25_grant_tick(&f, 3000000));
}

LS_CASE(rejected_vendor_block_cannot_replay_previous_grant)
{
    dsd_state state = {0};
    p25_grant_follower_t f;
    uint8_t block[12];
    install_iden(&state);
    build_grant(block, 0x302a, 42, 100);
    LS_CHECK(p25_tsbk_parse(&state, block));
    block[1] = 0x90;
    add_crc(block);
    LS_CHECK(!p25_tsbk_parse(&state, block));
    p25_grant_init(&f, 851012500, NULL, NULL);
    LS_CHECK(!p25_grant_from_state(&f, &state, 1000000));
}

LS_CASE(unrepresentable_carrier_never_enters_traffic_state)
{
    p25_grant_follower_t f;
    p25_grant_init(&f, 851012500, NULL, NULL);
    LS_CHECK(!p25_grant_on_grant(&f, 42, 100, (uint64_t)UINT32_MAX + 1, 1));
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
}

LS_CASE(grant_from_synthetic_tsbk_drives_retune_to_resolved_frequency)
{
    dsd_state state; memset(&state, 0, sizeof(state));
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    install_iden(&state);

    uint8_t grant[P25_TSBK_BYTES];
    build_grant(grant, 0x302a, 0x4567, 0x123456);
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 1);
    /* The parser resolved the frequency: 850,000,000 + 42 * 12,500 = 850,525,000 */
    LS_EQ_UINT(state.p25_tsbk_frequency_hz, 850525000ull);

    /* The follower reads the freshly parsed grant fields. */
    LS_CHECK(p25_grant_from_state(&f, &state, 1000000LL) == true);

    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.traffic_hz, 850525000ull);
    LS_EQ_UINT(f.talkgroup, 0x4567);
    LS_EQ_UINT(f.source, 0x123456);

    LS_EQ_INT(g_log.count, 1);
    LS_EQ_UINT(g_log.hz[0], 850525000ull);
    LS_CHECK(g_log.to_traffic[0] == true);
}

LS_CASE(control_grant_traffic_call_end_returns_to_control)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_INT(g_log.count, 1);
    LS_CHECK(g_log.to_traffic[0] == true);

    /* TDU / TDULC arrives: back to control immediately. */
    LS_CHECK(p25_grant_on_terminator(&f, 500000LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_UINT(f.talkgroup, 0);
    LS_EQ_INT(g_log.count, 2);
    LS_EQ_UINT(g_log.hz[1], 851012500ull);
    LS_CHECK(g_log.to_traffic[1] == false);

    /* Another terminator on control is a no-op. */
    LS_CHECK(p25_grant_on_terminator(&f, 600000LL) == false);
    LS_EQ_INT(g_log.count, 2);
}

LS_CASE(repeated_grant_for_active_call_does_not_retune)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_EQ_INT(g_log.count, 1);

    /* Ten GRANT_UPDATEs for the same TG - a busy site emits these constantly. */
    for (int i = 0; i < 10; i++) {
        LS_CHECK(p25_grant_on_grant(&f, 100, 0, 851525000ull,
                                    (int64_t)(i + 1) * 100000LL) == false);
    }
    LS_EQ_INT(g_log.count, 1);
    LS_EQ_UINT(f.duplicate_grants, 10);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 100);
    /* Duplicate grants refreshed the activity clock. */
    LS_EQ_INT(f.last_activity_us, 1000000LL);
}

LS_CASE(a_grant_for_a_different_tg_while_on_traffic_is_ignored)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    /* A busy trunk will scream new grants for other TGs; we hold. */
    LS_CHECK(p25_grant_on_grant(&f, 200, 6000, 852000000ull, 100000LL) == false);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 100);
    LS_EQ_UINT(f.foreign_grants, 1);
    LS_EQ_INT(g_log.count, 1);
}

LS_CASE(silence_beyond_the_hang_timer_returns_to_control)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);
    /* Default hang is 2 s; verify that number in the source is what we use. */
    LS_EQ_INT((int)(f.hang_us / 1000), 2000);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);

    /* Voice frames keep the call alive. */
    p25_grant_on_voice(&f, 500000LL);
    p25_grant_on_voice(&f, 1500000LL);
    LS_CHECK(p25_grant_tick(&f, 2400000LL) == false); /* 900ms since last voice */
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_INT(g_log.count, 1);

    /* Silence for 2s past the last voice: return. */
    LS_CHECK(p25_grant_tick(&f, 3500001LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_INT(g_log.count, 2);
    LS_CHECK(g_log.to_traffic[1] == false);
    LS_EQ_UINT(g_log.hz[1], 851012500ull);
}

LS_CASE(a_short_ptt_gap_does_not_drop_the_call)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    p25_grant_on_voice(&f, 500000LL);
    /* 1.5 s of silence, then someone else keys up: still the same call. */
    LS_CHECK(p25_grant_tick(&f, 1900000LL) == false);
    p25_grant_on_voice(&f, 1900000LL);
    LS_CHECK(p25_grant_tick(&f, 3000000LL) == false);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_INT(g_log.count, 1);
}

LS_CASE(allow_list_only_follows_listed_talkgroups)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    const uint16_t allow[] = { 100, 200 };
    p25_grant_set_filter(&f, P25_GRANT_FILTER_ALLOW, allow, 2);

    LS_CHECK(p25_grant_on_grant(&f, 999, 1, 851525000ull, 0LL) == false);
    LS_EQ_INT(g_log.count, 0);
    LS_EQ_UINT(f.filtered_grants, 1);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);

    LS_CHECK(p25_grant_on_grant(&f, 200, 1, 851525000ull, 1LL) == true);
    LS_EQ_INT(g_log.count, 1);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
}

LS_CASE(deny_list_skips_listed_talkgroups)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    const uint16_t deny[] = { 100 };
    p25_grant_set_filter(&f, P25_GRANT_FILTER_DENY, deny, 1);

    LS_CHECK(p25_grant_on_grant(&f, 100, 1, 851525000ull, 0LL) == false);
    LS_EQ_UINT(f.filtered_grants, 1);
    LS_EQ_INT(g_log.count, 0);

    LS_CHECK(p25_grant_on_grant(&f, 200, 1, 851525000ull, 1LL) == true);
    LS_EQ_INT(g_log.count, 1);
    LS_EQ_UINT(f.talkgroup, 200);
}

/* ------------------------------------------------------------------ */
/* encrypted-ESS return to control, and the skip window.        */
/* ------------------------------------------------------------------ */

LS_CASE(encrypted_ess_returns_to_control_and_stamps_a_skip)
{
    /* Followed grant lands, first LDU2 says ADP. The follower must retune
     * back to the control channel, increment the encrypted-returns counter,
     * and stamp a skip on the TG so the next grant does not thrash us
     * straight back onto the same encrypted channel. */
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_EQ_INT(g_log.count, 1);
    LS_CHECK(g_log.to_traffic[0] == true);

    /* ADP ALGID on the same TG - not CLEAR, so leave. */
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x84, 0x0000, 250000LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_UINT(f.encrypted_returns, 1);
    LS_EQ_INT(g_log.count, 2);
    LS_CHECK(g_log.to_traffic[1] == false);
    LS_EQ_UINT(g_log.hz[1], 851012500ull);

    /* Skip is active on TG 100 immediately after the return. */
    LS_CHECK(p25_grant_tg_is_skipped(&f, 100, 260000LL) == true);
    /* But not on any other TG. */
    LS_CHECK(p25_grant_tg_is_skipped(&f, 200, 260000LL) == false);
}

LS_CASE(clear_grant_is_still_followed_during_a_skip_on_a_different_tg)
{
    /* The whole point of a per-TG skip: one encrypted talkgroup must not
     * deafen the radio to a clear one. Encrypted TG 100 lands, we bail; a
     * grant for clear TG 200 must still be followed. */
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    /* Encrypt TG 100 and get out. */
    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x84, 0, 200000LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_INT(g_log.count, 2);

    /* Grant on TG 200 lands while the TG 100 skip is still live. */
    LS_CHECK(p25_grant_tg_is_skipped(&f, 100, 300000LL) == true);
    LS_CHECK(p25_grant_on_grant(&f, 200, 6000, 852000000ull, 300000LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 200);
    LS_EQ_INT(g_log.count, 3);
    LS_CHECK(g_log.to_traffic[2] == true);
    LS_EQ_UINT(f.encrypted_skips, 0);  /* the TG 200 grant is not a skip */
}

LS_CASE(a_repeat_grant_for_a_skipped_tg_is_refused_and_counted)
{
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);
    p25_grant_set_encrypted_skip_ms(&f, 5000);  /* short, so we can test expiry */

    /* Encrypted call arrives, we leave. */
    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x84, 0, 200000LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);

    /* Repeat grant on the same TG inside the skip window is refused. */
    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 1000000LL) == false);
    LS_EQ_UINT(f.encrypted_skips, 1);
    LS_EQ_INT(g_log.count, 2);       /* no extra retune */

    /* Once the skip has expired, the follower can pick it up again. If it
     * is still encrypted the next ESS will kick us out; if it turned clear,
     * we get to hear it. Either way, the skip should not deafen forever. */
    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 6000000LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.followed_count, 2);
    LS_EQ_UINT(f.encrypted_skips, 1);   /* unchanged - the retune actually happened */
}

LS_CASE(leave_on_encrypted_disabled_stays_on_traffic_channel)
{

    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);
    p25_grant_set_leave_on_encrypted(&f, false);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x84, 0, 200000LL) == false);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);   /* still there */
    LS_EQ_UINT(f.encrypted_returns, 0);
    LS_EQ_INT(g_log.count, 1);                  /* no return-to-control */
    /* But we did record the algid so a UI query can say "ADP on TG 100". */
    const p25_grant_tg_state_t *e = p25_grant_tg_state(&f, 0);
    LS_CHECK(e != NULL);
    LS_EQ_UINT(e->talkgroup, 100);
    LS_EQ_UINT(e->algid, 0x84);

    LS_CHECK(p25_grant_tg_is_skipped(&f, 100, 300000LL) == false);
}

LS_CASE(clear_ess_after_encrypted_clears_a_prior_skip)
{
    /* A system that flips a TG from ADP back to CLEAR should be followable
     * again immediately, not skipped for the remainder of the window. */
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x84, 0, 200000LL) == true);
    LS_CHECK(p25_grant_tg_is_skipped(&f, 100, 300000LL) == true);

    /* Now a CLEAR ESS lands (imagined out-of-band; in the field it would be
     * observed on a re-grant of the same TG). The prior skip clears. */
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x80, 0, 400000LL) == false);
    LS_CHECK(p25_grant_tg_is_skipped(&f, 100, 500000LL) == false);
}

LS_CASE(tg_state_table_is_bounded_and_evicts_the_oldest)
{
    /* A busy site could push through hundreds of unique TGs. The table has
     * a hard cap - the eviction counter proves the cap is enforced, and the
     * evicted entry is chosen by oldest last_seen so a live TG is preserved. */
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);
    p25_grant_set_leave_on_encrypted(&f, false); /* stay put; feed many ESS */

    /* Fill the table with distinct talkgroups, each with a fresh timestamp. */
    for (int i = 0; i < P25_GRANT_TG_STATE_MAX; i++) {
        (void)p25_grant_on_grant(&f, (uint16_t)(1000 + i), 0,
                                 851525000ull, (int64_t)(i + 1) * 100000LL);
        (void)p25_grant_on_ess(&f, (uint16_t)(1000 + i), 0x84, 0,
                               (int64_t)(i + 1) * 100000LL);
        /* stay on traffic - leave_on_encrypted is off - reset for the next TG */
        f.state = P25_GRANT_ON_CONTROL;
        f.talkgroup = 0;
    }
    LS_EQ_UINT(p25_grant_tg_state_count(&f), P25_GRANT_TG_STATE_MAX);
    LS_EQ_UINT(f.tg_state_evictions, 0);

    /* One more TG: eviction must happen and count. */
    (void)p25_grant_on_grant(&f, 9999, 0, 851525000ull, 999999LL);
    (void)p25_grant_on_ess(&f, 9999, 0x84, 0, 999999LL);
    LS_EQ_UINT(p25_grant_tg_state_count(&f), P25_GRANT_TG_STATE_MAX);
    LS_EQ_UINT(f.tg_state_evictions, 1);

    /* The oldest entry (TG 1000, timestamp 100000) should be gone; the
     * newest live TG must still be present. */
    bool has_old = false, has_new = false, has_added = false;
    for (size_t i = 0; i < p25_grant_tg_state_count(&f); i++) {
        const p25_grant_tg_state_t *e = p25_grant_tg_state(&f, i);
        if (!e) continue;
        if (e->talkgroup == 1000) has_old = true;
        if (e->talkgroup == 1000 + P25_GRANT_TG_STATE_MAX - 1) has_new = true;
        if (e->talkgroup == 9999) has_added = true;
    }
    LS_CHECK_MSG(!has_old, "oldest TG 1000 should have been evicted");
    LS_CHECK_MSG(has_new,  "most recent live TG should still be present");
    LS_CHECK_MSG(has_added, "just-added TG 9999 must be in the table");
}

LS_CASE(encrypted_return_uses_the_same_retune_writer_as_the_terminator)
{
    /* The task warned that a return-to-control path from the mute logic could
     * become a third writer of the tune. Pin that the encrypted-ESS retune
     * lands via the same retune callback and the same control_hz as the TDU
     * path - one owner for "return to control", called from three triggers
     * (terminator, silence tick, encrypted ESS). */
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    /* Terminator path. */
    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    LS_CHECK(p25_grant_on_terminator(&f, 100000LL) == true);
    LS_EQ_INT(g_log.count, 2);
    LS_EQ_UINT(g_log.hz[1], 851012500ull);
    LS_CHECK(g_log.to_traffic[1] == false);

    /* Encrypted-ESS path. */
    LS_CHECK(p25_grant_on_grant(&f, 200, 6000, 852000000ull, 200000LL) == true);
    LS_CHECK(p25_grant_on_ess(&f, 200, 0x89, 0, 300000LL) == true);
    LS_EQ_INT(g_log.count, 4);
    LS_EQ_UINT(g_log.hz[3], 851012500ull);   /* same control freq */
    LS_CHECK(g_log.to_traffic[3] == false);

    /* Silence-tick path. */
    p25_grant_set_hang_ms(&f, 50);
    /* Wait out the skip so we can be followed again. */
    LS_CHECK(p25_grant_on_grant(&f, 300, 7000, 853000000ull,
                                31000000LL) == true);
    LS_CHECK(p25_grant_tick(&f, 31200000LL) == true);
    LS_EQ_INT(g_log.count, 6);
    LS_EQ_UINT(g_log.hz[5], 851012500ull);
    LS_CHECK(g_log.to_traffic[5] == false);
}

LS_CASE(ess_on_a_different_tg_records_but_does_not_retune)
{
    /* A stale ESS - the follower is on TG 100, the LCW parser passes an
     * ESS keyed to TG 200 because a byte-of-noise flipped the LCW - must
     * NOT boot us off TG 100. Only an ESS for the currently-followed TG
     * causes a retune. The stray algid is still recorded on TG 200 so a
     * UI query is not silently dropped. */
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);

    LS_CHECK(p25_grant_on_grant(&f, 100, 5000, 851525000ull, 0LL) == true);
    /* ESS says "encrypted, but on TG 200" - the follower is on TG 100. */
    LS_CHECK(p25_grant_on_ess(&f, 200, 0x84, 0, 200000LL) == false);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 100);
    LS_EQ_INT(g_log.count, 1);      /* no return */
    LS_EQ_UINT(f.encrypted_returns, 0);

    /* Table records TG 200 as ADP-seen. */
    bool found = false;
    for (size_t i = 0; i < p25_grant_tg_state_count(&f); i++) {
        const p25_grant_tg_state_t *e = p25_grant_tg_state(&f, i);
        if (e && e->talkgroup == 200 && e->algid == 0x84) found = true;
    }
    LS_CHECK(found);
}

LS_CASE(full_grant_update_cycle_through_synthetic_tsdu)
{
    dsd_state state; memset(&state, 0, sizeof(state));
    p25_grant_follower_t f;
    reset_log();
    p25_grant_init(&f, 851012500ull, retune_cb, &g_log);
    /* Short hang so the test stays quick. */
    p25_grant_set_hang_ms(&f, 100);

    install_iden(&state);

    /* GRP_V_CH_GRANT for TG 0x4567 on channel 0x302a. */
    uint8_t grant[P25_TSBK_BYTES];
    build_grant(grant, 0x302a, 0x4567, 0x123456);
    LS_EQ_INT(p25_tsbk_parse(&state, grant), 1);
    LS_CHECK(p25_grant_from_state(&f, &state, 0LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.traffic_hz, 850525000ull);
    LS_EQ_INT(g_log.count, 1);

    /* GRP_V_CH_GRANT_UPDATE for the same TG - no retune. */
    uint8_t upd[P25_TSBK_BYTES];
    build_grant_update(upd, 0x302a, 0x4567);
    LS_EQ_INT(p25_tsbk_parse(&state, upd), 1);
    LS_CHECK(p25_grant_from_state(&f, &state, 25000LL) == false);
    LS_EQ_INT(g_log.count, 1);
    LS_EQ_UINT(f.duplicate_grants, 1);

    /* Silence past the (short) hang: back to control. */
    LS_CHECK(p25_grant_tick(&f, 200000LL) == true);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_INT(g_log.count, 2);
    LS_CHECK(g_log.to_traffic[1] == false);
    LS_EQ_UINT(g_log.hz[1], 851012500ull);
}
