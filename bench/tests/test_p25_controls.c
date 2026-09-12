/* LS_TEST_SOURCES: ${APP}/p25/p25_controls.c ${APP}/p25/scan_ctrl.c
 * ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c */

#include "ls_test.h"

#include "p25_controls.h"
#include "scan_ctrl.h"

#include <limits.h>
#include <string.h>

typedef struct {
    uint64_t hz[8];
    bool to_traffic[8];
    int count;
} retune_log_t;

static void retune_cb(void *user, uint64_t hz, bool to_traffic)
{
    retune_log_t *log = (retune_log_t *)user;
    if (log->count < 8) {
        log->hz[log->count] = hz;
        log->to_traffic[log->count] = to_traffic;
    }
    log->count++;
}

static void reset_all(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
                      retune_log_t *log)
{
    memset(log, 0, sizeof(*log));
    p25_scan_init(sc);
    p25_grant_init(f, 851012500ULL, retune_cb, log);
}

LS_CASE(frequency_parser_accepts_exact_channel_centres_and_limits)
{
    uint32_t hz = 0;
    LS_CHECK(p25_controls_parse_mhz("851.00625", &hz));
    LS_EQ_UINT(hz, 851006250UL);
    LS_CHECK(p25_controls_parse_mhz("154.785000", &hz));
    LS_EQ_UINT(hz, 154785000UL);
    LS_CHECK(p25_controls_parse_mhz("24", &hz));
    LS_EQ_UINT(hz, P25_CONTROL_TUNER_MIN_HZ);
    LS_CHECK(p25_controls_parse_mhz("1766.000000", &hz));
    LS_EQ_UINT(hz, P25_CONTROL_TUNER_MAX_HZ);

    char text[24];
    p25_controls_format_mhz(text, sizeof(text), 851006250UL);
    LS_EQ_STR(text, "851.006250 MHz");
}

LS_CASE(frequency_parser_rejects_malformed_overflow_and_out_of_range)
{
    uint32_t hz = 123;
    LS_CHECK(!p25_controls_parse_mhz(NULL, &hz));
    LS_CHECK(!p25_controls_parse_mhz("", &hz));
    LS_CHECK(!p25_controls_parse_mhz(".", &hz));
    LS_CHECK(!p25_controls_parse_mhz("154.", &hz));
    LS_CHECK(!p25_controls_parse_mhz("154..785", &hz));
    LS_CHECK(!p25_controls_parse_mhz("154.785x", &hz));
    LS_CHECK(!p25_controls_parse_mhz(" 154.785", &hz));
    LS_CHECK(!p25_controls_parse_mhz("154.7850000", &hz));
    LS_CHECK(!p25_controls_parse_mhz("23.999999", &hz));
    LS_CHECK(!p25_controls_parse_mhz("1766.000001", &hz));
    LS_CHECK(!p25_controls_parse_mhz("429496729599999", &hz));
}

LS_CASE(control_defaults_and_duration_clamping_are_bounded)
{
    p25_scan_ctrl_t sc;
    p25_grant_follower_t f;
    retune_log_t log;
    reset_all(&sc, &f, &log);

    LS_CHECK(p25_scan_get_auto_follow(&sc));
    LS_CHECK(f.leave_on_encrypted);
    LS_EQ_UINT((uint64_t)f.encrypted_skip_us / 1000ULL,
               P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS);

    p25_grant_set_encrypted_skip_ms(&f, 0);
    LS_EQ_UINT((uint64_t)f.encrypted_skip_us / 1000ULL,
               P25_CONTROL_ENCRYPTED_SKIP_MIN_MS);
    p25_grant_set_encrypted_skip_ms(&f, UINT_MAX);
    LS_EQ_UINT((uint64_t)f.encrypted_skip_us / 1000ULL,
               P25_CONTROL_ENCRYPTED_SKIP_MAX_MS);
}

LS_CASE(auto_follow_off_blocks_new_traffic_retunes)
{
    p25_scan_ctrl_t sc;
    p25_grant_follower_t f;
    retune_log_t log;
    reset_all(&sc, &f, &log);

    LS_CHECK(!p25_scan_set_auto_follow(&sc, &f, false));
    p25_scan_decision_t d = p25_scan_apply(&sc, &f, 100, 1,
                                            851525000ULL, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_AUTO_FOLLOW_OFF);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_INT(log.count, 0);
}

LS_CASE(disabling_follow_returns_an_active_call_through_the_follower)
{
    p25_scan_ctrl_t sc;
    p25_grant_follower_t f;
    retune_log_t log;
    reset_all(&sc, &f, &log);

    (void)p25_scan_apply(&sc, &f, 100, 1, 851525000ULL, 0);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_INT(log.count, 1);

    LS_CHECK(p25_scan_set_auto_follow(&sc, &f, false));
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);
    LS_EQ_INT(log.count, 2);
    LS_CHECK(!log.to_traffic[1]);
    LS_EQ_UINT(log.hz[1], 851012500ULL);
}

LS_CASE(restored_controls_keep_roster_policy_and_reenable_normal_precedence)
{
    p25_scan_ctrl_t sc;
    p25_grant_follower_t f;
    retune_log_t log;
    reset_all(&sc, &f, &log);
    LS_CHECK(p25_scan_lockout_add(&sc, 100));

    p25_scan_restore_controls(&sc, &f, false, false, UINT_MAX);
    LS_CHECK(!p25_scan_get_auto_follow(&sc));
    LS_CHECK(!f.leave_on_encrypted);
    LS_EQ_UINT((uint64_t)f.encrypted_skip_us / 1000ULL,
               P25_CONTROL_ENCRYPTED_SKIP_MAX_MS);
    LS_CHECK(p25_scan_is_locked_out(&sc, 100));

    p25_scan_restore_controls(&sc, &f, true, true, 30000);
    p25_scan_decision_t d = p25_scan_apply(&sc, &f, 100, 1,
                                            851525000ULL, 0);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_LOCKED_OUT);
    LS_EQ_INT(log.count, 0);
    d = p25_scan_apply(&sc, &f, 200, 1, 852000000ULL, 1);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_DEFAULT);
}

LS_CASE(skip_off_does_not_authorize_encrypted_or_unknown_synthesis)
{
    p25_scan_ctrl_t sc;
    p25_grant_follower_t f;
    retune_log_t log;
    reset_all(&sc, &f, &log);
    p25_scan_restore_controls(&sc, &f, true, false, 30000);

    dsd_state state;
    dsd_opts opts;
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    opts.unmute_encrypted_p25 = 0;

    state.p25_ess_valid = 1;
    state.p25_algid = 0x84;
    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);
    state.p25_ess_valid = 0;
    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);
    LS_CHECK(!f.leave_on_encrypted);
    LS_EQ_INT(opts.unmute_encrypted_p25, 0);
}
