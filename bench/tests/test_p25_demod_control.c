/* LS_TEST_SOURCES: p25_demod_control.c p25_qual.c */
#include "ls_test.h"
#include "p25_demod_control.h"
#include "p25_qual.h"

LS_CASE(protocol_quality_scores_only_valid_results)
{
    LS_EQ_INT(p25_qual_protocol_score(0, 0), P25_QUAL_NO_SCORE);
    LS_EQ_INT(p25_qual_protocol_score(3, 0), 3);
    LS_EQ_INT(p25_qual_protocol_score(0, 2), 8);
    LS_EQ_INT(p25_qual_protocol_score(3, 2), 11);
}

LS_CASE(validated_c4fm_does_not_wait_or_switch_away)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(!p25_demod_control_tick(&c, 100, 1, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 200, 2, 0, false));
    LS_CHECK(c.locked);
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_CHECK(!p25_demod_control_tick(&c, 1500, 8, 2, false));
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_EQ_UINT(c.reacquire_count, 0);
}

LS_CASE(validated_cqpsk_locks_without_second_full_dwell)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(p25_demod_control_tick(&c, 1500, 0, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 1600, 1, 1, false));
    LS_CHECK(c.locked);
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    LS_EQ_INT(c.cqpsk_nids, 1);
    LS_EQ_INT(c.cqpsk_tsbks, 1);
}

LS_CASE(no_lock_listening_accepts_protocol_before_retry)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(p25_demod_control_tick(&c, 1500, 0, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 3000, 0, 0, false));
    LS_EQ_INT(c.phase, P25_DEMOD_NO_LOCK);
    LS_CHECK(p25_demod_control_tick(&c, 3200, 2, 0, true));
    LS_CHECK(c.locked);
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_CHECK(!p25_demod_control_tick(&c, 4000, 2, 0, true));
}

LS_CASE(validated_bursts_hold_last_good_mode_until_real_protocol_loss)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(p25_demod_control_tick(&c, 200, 2, 0, true));
    LS_CHECK(c.locked);
    LS_CHECK(!p25_demod_control_tick(&c, 4100, 3, 0, false));
    LS_CHECK(!p25_demod_control_tick(&c, 8000, 4, 0, false));
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_EQ_UINT(c.reacquire_count, 0);
    LS_CHECK(!p25_demod_control_tick(&c, 11999, 4, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 12000, 4, 0, false));
    LS_EQ_UINT(c.reacquire_count, 1);
    LS_CHECK(!c.locked);
    LS_CHECK(p25_demod_control_tick(&c, 13500, 4, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 13600, 6, 0, false));
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    LS_CHECK(c.locked);
}

LS_CASE(auto_windows_count_samples_not_startup_or_usb_wait_time)
{
    p25_demod_control_t old, c;
    p25_demod_control_init(&old, P25_DEMOD_AUTO, 0, 0, 0);
    /* Reproduce the old production call: a delayed first block exhausted
     * the 1500 ms C4FM trial after just 34 ms of actual input. */
    LS_CHECK(p25_demod_control_tick(&old, 2500, 0, 0, false));
    LS_EQ_INT(old.active, DEMOD_CQPSK);
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    for (unsigned i = 0; i < 100; ++i)
        LS_CHECK(!p25_demod_control_feed(&c, 0, 240000, 0, 0, false));
    LS_EQ_UINT(c.stream_ms, 0);
    for (unsigned i = 0; i < 43; ++i)
        LS_CHECK(!p25_demod_control_feed(&c, 8192, 240000, 0, 0, false));
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_EQ_UINT(c.stream_ms, 1467);
    LS_CHECK(p25_demod_control_feed(&c, 8192, 240000, 0, 0, false));
    LS_EQ_UINT(c.stream_ms, 1501);
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    for (unsigned i = 0; i < 43; ++i)
        LS_CHECK(!p25_demod_control_feed(&c, 8192, 240000, 1, 0, false));
    LS_CHECK(p25_demod_control_feed(&c, 8192, 240000, 2, 2, false));
    LS_EQ_UINT(c.stream_ms, 3003);
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    LS_CHECK(c.locked);
}

LS_CASE(sample_clock_retains_fraction_wraps_and_rejects_missing_rate)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, UINT32_MAX - 999, 0, 0);
    LS_CHECK(!p25_demod_control_feed(&c, 8192, 0, 0, 0, false));
    LS_EQ_UINT(c.stream_ms, UINT32_MAX - 999);
    for (unsigned i = 0; i < 359999; ++i)
        LS_CHECK(!p25_demod_control_feed(&c, 1, 240000, 0, 0, false));
    LS_EQ_UINT(c.stream_ms, 499);
    LS_CHECK(p25_demod_control_feed(&c, 1, 240000, 0, 0, false));
    LS_EQ_UINT(c.stream_ms, 500);
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    p25_demod_control_init(&c, DEMOD_C4FM, 0, 0, 0);
    LS_CHECK(!p25_demod_control_feed(&c, 2400000, 240000, 0, 0, false));
    LS_EQ_INT(c.active, DEMOD_C4FM);
}

LS_CASE(autodetect_selects_c4fm_protocol_winner)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_EQ_INT(c.active, DEMOD_C4FM);

    LS_CHECK(p25_demod_control_tick(&c, 1500, 8, 2, false));
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_CHECK(!p25_demod_control_tick(&c, 3000, 9, 2, false));
    LS_CHECK(c.locked);
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_EQ_STR(p25_demod_control_name(&c), "AUTO:C4FM");
    LS_EQ_INT(c.c4fm_nids, 8);
    LS_EQ_INT(c.c4fm_tsbks, 2);
}

LS_CASE(autodetect_selects_lsm_cqpsk_protocol_winner)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(p25_demod_control_tick(&c, 1500, 0, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 3000, 8, 2, false));
    LS_CHECK(c.locked);
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    LS_EQ_STR(p25_demod_control_name(&c), "AUTO:CQPSK");
    LS_EQ_INT(c.cqpsk_nids, 8);
    LS_EQ_INT(c.cqpsk_tsbks, 2);
}

LS_CASE(autodetect_noise_reports_no_lock_and_retries)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(p25_demod_control_tick(&c, 1500, 0, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 3000, 0, 0, false));
    LS_CHECK(!c.locked);
    LS_EQ_STR(p25_demod_control_name(&c), "AUTO:NO LOCK");
    LS_CHECK(!p25_demod_control_tick(&c, 3999, 0, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 4000, 0, 0, false));
    LS_EQ_INT(c.phase, P25_DEMOD_HUNT_C4FM);
}

LS_CASE(autodetect_never_switches_mid_call)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(!p25_demod_control_tick(&c, 2000, 1, 0, true));
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_CHECK(p25_demod_control_tick(&c, 2100, 1, 0, false));
    LS_EQ_INT(c.active, DEMOD_CQPSK);
}

LS_CASE(sparse_evidence_still_compares_and_ties_prefer_c4fm)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(!p25_demod_control_tick(&c, 200, 1, 0, false));
    LS_CHECK(!c.locked);
    LS_CHECK(p25_demod_control_tick(&c, 1500, 1, 0, false));
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    /* The second trial must not inherit the first mode's valid NID. */
    LS_CHECK(!p25_demod_control_tick(&c, 1700, 2, 0, false));
    LS_CHECK(!c.locked);
    LS_CHECK(p25_demod_control_tick(&c, 3000, 2, 0, false));
    LS_EQ_INT(c.c4fm_score, 1);
    LS_EQ_INT(c.cqpsk_score, 1);
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_CHECK(c.locked);
}

LS_CASE(control_channel_tsdu_is_not_mistaken_for_a_call)
{
    LS_CHECK(p25_demod_call_active(1, false));
    LS_CHECK(p25_demod_call_active(2, false));
    LS_CHECK(!p25_demod_call_active(3, false));
    LS_CHECK(!p25_demod_call_active(4, false));
    LS_CHECK(p25_demod_call_active(0, true));
}

LS_CASE(locked_mode_reacquires_after_protocol_loss)
{
    p25_demod_control_t c;
    p25_demod_control_init(&c, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(p25_demod_control_tick(&c, 1500, 0, 0, false));
    LS_CHECK(p25_demod_control_tick(&c, 3000, 4, 1, false));
    LS_EQ_INT(c.active, DEMOD_CQPSK);

    LS_CHECK(!p25_demod_control_tick(&c, 6999, 4, 1, false));
    LS_CHECK(p25_demod_control_tick(&c, 7000, 4, 1, false));
    LS_EQ_INT(c.active, DEMOD_C4FM);
    LS_EQ_INT(c.reacquire_count, 1);

    /* A live call holds the selected mode even after the timeout. */
    LS_CHECK(p25_demod_control_tick(&c, 8500, 5, 1, false));
    LS_CHECK(p25_demod_control_tick(&c, 10000, 9, 2, false));
    LS_EQ_INT(c.active, DEMOD_CQPSK);
    LS_CHECK(!p25_demod_control_tick(&c, 15000, 9, 2, true));
    LS_EQ_INT(c.active, DEMOD_CQPSK);
}

LS_CASE(manual_mode_survives_controller_reentry)
{
    p25_demod_control_t first, second;
    p25_demod_control_init(&first, DEMOD_FSK4_TRACKING, 100, 0, 0);
    p25_demod_control_init(&second, first.preference, 200, 0, 0);
    LS_EQ_INT(second.preference, DEMOD_FSK4_TRACKING);
    LS_EQ_INT(second.active, DEMOD_FSK4_TRACKING);
    LS_CHECK(second.locked);
}

LS_CASE(dsp_and_dsd_mode_notions_are_reconciled)
{
    for (int mode = DEMOD_C4FM; mode <= DEMOD_FSK4_TRACKING; mode++) {
        int c4fm = -1, qpsk = -1, gfsk = -1;
        p25_demod_dsd_flags((demod_mode_t)mode, &c4fm, &qpsk, &gfsk);
        LS_EQ_INT(c4fm, 1);
        LS_EQ_INT(qpsk, 0);
        LS_EQ_INT(gfsk, 0);
    }
}
