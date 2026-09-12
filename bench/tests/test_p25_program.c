/* LS_TEST_SOURCES: ${APP}/p25/p25_program.c ${APP}/p25/p25_profile.c
 *                  ${APP}/p25/scan_ctrl.c ${APP}/p25/grant_follower.c
 *                  ${APP}/p25/p25_controls.c ${APP}/p25/p25_ess.c */

#include "ls_test.h"

#include "p25_profile_fixture.h"
#include "p25_program.h"
#include "dsd.h"

#include <string.h>

static const ls_radio_range_t s_tune_ranges[] = {
    {24000000, 1766000000},
};
static const p25_profile_parse_config_t s_config = {
    s_tune_ranges, sizeof(s_tune_ranges) / sizeof(s_tune_ranges[0]),
};

static p25_program_staging_t s_staging;

/* ------------------------------------------------------------------ fakes */

#define OPS_LOG_MAX 64

typedef struct {
    char     log[OPS_LOG_MAX];
    unsigned ops;

    /* Fake decoder + radio. */
    bool     on_traffic;
    uint64_t traffic_hz;
    uint64_t control_hz;
    uint64_t tune_latch;     /* single slot: last request wins              */
    unsigned tune_requests;

    bool     auto_follow;
    bool     skip_encrypted;
    uint32_t skip_ms;
    int      demod_preference;
    p25_cqpsk_config_t cqpsk;
    uint8_t  roster_count;
    uint16_t roster_first_tg;
} fake_radio_t;

static void log_op(fake_radio_t *r, char c)
{
    if (r->ops + 1U < sizeof(r->log)) r->log[r->ops++] = c;
    r->log[r->ops] = '\0';
}

static void op_release(void *user)
{
    fake_radio_t *r = (fake_radio_t *)user;
    log_op(r, 'R');
    if (r->on_traffic) {
        r->on_traffic = false;
        r->traffic_hz = 0;
    }
    /* The follower's return-to-control path issues a tune of its own. */
    r->tune_latch = r->control_hz;
    r->tune_requests++;
}

static void op_auto_follow(void *user, bool enabled)
{
    fake_radio_t *r = (fake_radio_t *)user;
    log_op(r, 'F');
    r->auto_follow = enabled;
}

static void op_encrypted(void *user, bool skip_enabled, uint32_t skip_ms)
{
    fake_radio_t *r = (fake_radio_t *)user;
    log_op(r, 'E');
    r->skip_encrypted = skip_enabled;
    r->skip_ms = skip_ms;
}

static void op_demod(void *user, int preference)
{
    fake_radio_t *r = (fake_radio_t *)user;
    log_op(r, 'D');
    r->demod_preference = preference;
}

static void op_cqpsk(void *user, const p25_cqpsk_config_t *config)
{
    fake_radio_t *r = (fake_radio_t *)user;
    log_op(r, 'Q');
    r->cqpsk = *config;
}

static void op_roster(void *user, const p25_profile_talkgroup_t *tgs, size_t n)
{
    fake_radio_t *r = (fake_radio_t *)user;
    log_op(r, 'S');
    r->roster_count = (uint8_t)n;
    r->roster_first_tg = n ? tgs[0].id : 0;
}

static void op_restore_roster(void *user,
                              const p25_profile_talkgroup_t *tgs, size_t n)
{
    op_roster(user, tgs, n);
}

static void op_control(void *user, uint64_t control_hz)
{
    fake_radio_t *r = (fake_radio_t *)user;
    log_op(r, 'C');
    r->control_hz = control_hz;
    r->tune_latch = control_hz;
    r->tune_requests++;
}

static void fake_radio_init(fake_radio_t *r, p25_program_ops_t *ops)
{
    memset(r, 0, sizeof(*r));
    r->demod_preference = -99;
    ops->release              = op_release;
    ops->set_auto_follow      = op_auto_follow;
    ops->set_encrypted_policy = op_encrypted;
    ops->set_cqpsk_loops      = op_cqpsk;
    ops->set_demod_preference = op_demod;
    ops->set_roster           = op_roster;
    ops->restore_roster       = op_restore_roster;
    ops->set_control          = op_control;
    ops->user                 = r;
}

typedef struct {
    const char          *text;      /* NULL when the read should fail        */
    p25_program_result_t status;
    unsigned             calls;
    const char          *last_path;
} fake_file_t;

static p25_program_result_t fake_read(void *ctx, const char *path, char *dst,
                                      size_t cap, size_t *out_len)
{
    fake_file_t *f = (fake_file_t *)ctx;
    f->calls++;
    f->last_path = path;
    if (f->status != P25_PROGRAM_OK) return f->status;

    size_t n = strlen(f->text);
    if (n > cap) return P25_PROGRAM_ERR_TOO_LARGE;
    memcpy(dst, f->text, n);
    *out_len = n;
    return P25_PROGRAM_OK;
}

static p25_program_result_t load(p25_program_t *program, fake_file_t *file,
                                 const p25_program_ops_t *ops, const char *path)
{
    if (!p25_program_claim(program, path)) return P25_PROGRAM_ERR_BUSY;
    return p25_program_reload(program, fake_read, file, &s_staging, &s_config,
                              ops);
}

/* A profile for a different system, so a second load is distinguishable from
 * the first in every field the apply touches. */
static const char SECOND_PROFILE[] =
    "version=1\n"
    "system=Second System\n"
    "site=South Site\n"
    "control=770031250\n"
    "control=771031250\n"
    "preferred=771031250\n"
    "auto_follow=false\n"
    "encrypted_skip=false\n"
    "encrypted_skip_ms=15000\n"
    "demod=c4fm\n"
    "cqpsk_timing_gain=0.000078125\n"
    "cqpsk_carrier_gain=0.005\n"
    "tg=4001|Second Ops|true|3\n";

/* ------------------------------------------------------------------ cases */

LS_CASE(first_run_is_idle_and_shows_an_empty_state)
{
    p25_program_t program;
    p25_program_init(&program);

    char text[P25_PROGRAM_CONTROL_TEXT_MAX];
    LS_EQ_INT(program.state, P25_PROGRAM_IDLE);
    LS_CHECK(!program.active_valid);
    LS_EQ_STR(p25_program_state_name(&program), "IDLE");
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "NO PROFILE LOADED");
    p25_program_format_source(&program, text, sizeof(text));
    LS_EQ_STR(text, "(none)");
    p25_program_format_controls(&program, text, sizeof(text));
    LS_EQ_STR(text, "(no profile loaded)");
    p25_program_format_selected(&program, text, sizeof(text));
    LS_EQ_STR(text, "-");
    p25_program_format_roster(&program, text, sizeof(text));
    LS_EQ_STR(text, "-");
    LS_EQ_UINT(p25_program_selected_control_hz(&program), 0);
}

LS_CASE(reload_applies_every_op_in_order_then_commits)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);

    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);

    /* release, auto-follow, encrypted policy, CQPSK loops, demod, roster,
     * control. The
     * tune is last so the radio never sits on the new control channel while
     * the old system's policy is still in force. */
    LS_EQ_STR(radio.log, "RFEQDSC");
    LS_EQ_INT(program.state, P25_PROGRAM_LOADED);
    LS_CHECK(program.active_valid);
    LS_EQ_STR(program.active.system_name, "Example Regional Radio");
    LS_EQ_STR(program.active_path, "/sdcard/p25_profile.txt");
    LS_EQ_UINT(program.loads_ok, 1);
    LS_EQ_UINT(program.loads_failed, 0);

    /* preferred= names the second control, so that is what is selected. */
    LS_EQ_UINT(program.selected_control, 1);
    LS_EQ_UINT(p25_program_selected_control_hz(&program), 852237500);
    LS_EQ_UINT(radio.control_hz, 852237500);
    LS_EQ_UINT(radio.tune_latch, 852237500);
    LS_CHECK(!radio.auto_follow);
    LS_CHECK(!radio.skip_encrypted);
    LS_EQ_UINT(radio.skip_ms, 45000);
    LS_EQ_INT(radio.demod_preference, DEMOD_CQPSK);
    LS_NEAR(radio.cqpsk.timing_gain, 0.0003125f, 0.0f);
    LS_NEAR(radio.cqpsk.carrier_gain, 0.02f, 0.0f);
    LS_EQ_UINT(radio.roster_count, 2);
    LS_EQ_UINT(radio.roster_first_tg, 1201);

    char text[P25_PROGRAM_CONTROL_TEXT_MAX];
    p25_program_format_selected(&program, text, sizeof(text));
    LS_EQ_STR(text, "2/2  852.237500 MHz");
    p25_program_format_controls(&program, text, sizeof(text));
    LS_EQ_STR(text, "  1  851.012500 MHz\n> 2  852.237500 MHz");
    p25_program_format_roster(&program, text, sizeof(text));
    LS_EQ_STR(text, "2 TG  1 allowed  1 priority");
    p25_program_format_source(&program, text, sizeof(text));
    LS_EQ_STR(text, "/sdcard/p25_profile.txt");
}

LS_CASE(reload_without_a_claim_is_refused_and_touches_nothing)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_MINIMAL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);

    LS_EQ_INT(p25_program_reload(&program, fake_read, &file, &s_staging,
                                 &s_config, &ops),
              P25_PROGRAM_ERR_NOT_CLAIMED);
    LS_EQ_UINT(file.calls, 0);
    LS_EQ_STR(radio.log, "");
    LS_EQ_INT(program.state, P25_PROGRAM_IDLE);
    LS_EQ_UINT(program.loads_failed, 0);
}

LS_CASE(a_second_claim_while_busy_is_refused)
{
    p25_program_t program;
    p25_program_init(&program);

    LS_CHECK(p25_program_claim(&program, "/sdcard/a.txt"));
    LS_EQ_INT(program.state, P25_PROGRAM_BUSY);
    LS_CHECK(!p25_program_claim(&program, "/sdcard/b.txt"));
    LS_EQ_STR(program.last_path, "/sdcard/a.txt");

    char text[64];
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "LOADING");
}

LS_CASE(an_over_long_path_is_refused_with_a_reason_and_clears_busy)
{
    p25_program_t program;
    p25_program_init(&program);

    char path[P25_PROGRAM_PATH_MAX + 8];
    memset(path, 'x', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    path[0] = '/';

    LS_CHECK(!p25_program_claim(&program, path));
    LS_EQ_INT(program.state, P25_PROGRAM_FAILED);
    LS_EQ_INT(program.last_result, P25_PROGRAM_ERR_PATH_TOO_LONG);

    char text[96];
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "profile path is too long  (no active profile)");
}

LS_CASE(a_missing_file_keeps_the_active_profile_and_runs_no_op)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t good = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };
    fake_file_t missing = { NULL, P25_PROGRAM_ERR_NOT_FOUND, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &good, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);

    uint64_t applied_control = radio.control_hz;
    unsigned tunes = radio.tune_requests;
    radio.log[0] = '\0';
    radio.ops = 0;

    LS_EQ_INT(load(&program, &missing, &ops, "/sdcard/missing.txt"),
              P25_PROGRAM_ERR_NOT_FOUND);

    LS_EQ_STR(radio.log, "");
    LS_EQ_UINT(radio.tune_requests, tunes);
    LS_EQ_UINT(radio.control_hz, applied_control);
    LS_CHECK(program.active_valid);
    LS_EQ_STR(program.active.system_name, "Example Regional Radio");
    LS_EQ_STR(program.active_path, "/sdcard/p25_profile.txt");
    LS_EQ_UINT(program.selected_control, 1);
    LS_EQ_INT(program.state, P25_PROGRAM_FAILED);
    LS_EQ_UINT(program.loads_ok, 1);
    LS_EQ_UINT(program.loads_failed, 1);

    char text[96];
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "no profile file at that path  (active profile kept)");

    p25_program_format_source(&program, text, sizeof(text));
    LS_EQ_STR(text, "/sdcard/p25_profile.txt");
}

LS_CASE(an_oversized_file_is_refused_before_any_op)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t good = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };
    fake_file_t huge = { NULL, P25_PROGRAM_ERR_TOO_LARGE, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &good, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    radio.log[0] = '\0';
    radio.ops = 0;

    LS_EQ_INT(load(&program, &huge, &ops, "/sdcard/huge.txt"),
              P25_PROGRAM_ERR_TOO_LARGE);
    LS_EQ_STR(radio.log, "");
    LS_EQ_INT(program.state, P25_PROGRAM_FAILED);
    LS_EQ_STR(program.active.system_name, "Example Regional Radio");

    char text[96];
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "profile file is too large  (active profile kept)");
}

LS_CASE(an_empty_file_is_refused)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t empty = { "", P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &empty, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_ERR_EMPTY);
    LS_EQ_STR(radio.log, "");
    LS_CHECK(!program.active_valid);
}

LS_CASE(an_invalid_profile_reports_its_line_and_retains_the_active_one)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t good = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };
    static const char BAD[] =
        "version=1\n"
        "system=Broken\n"
        "site=Broken Site\n"
        "control=851012500\n"
        "banana=1\n";
    fake_file_t bad = { BAD, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &good, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    radio.log[0] = '\0';
    radio.ops = 0;

    LS_EQ_INT(load(&program, &bad, &ops, "/sdcard/bad.txt"),
              P25_PROGRAM_ERR_PARSE);
    LS_EQ_STR(radio.log, "");
    LS_EQ_UINT(program.last_diagnostic.line, 5);
    LS_EQ_INT(program.last_diagnostic.code, P25_PROFILE_ERROR_UNKNOWN_FIELD);
    LS_EQ_STR(program.active.system_name, "Example Regional Radio");
    LS_EQ_UINT(program.selected_control, 1);

    char text[128];
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "line 5: unknown field  (active profile kept)");
}

LS_CASE(a_first_load_that_fails_leaves_no_active_profile)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t missing = { NULL, P25_PROGRAM_ERR_READ, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &missing, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_ERR_READ);

    char text[96];
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "profile file could not be read  (no active profile)");
    p25_program_format_source(&program, text, sizeof(text));
    LS_EQ_STR(text, "(none)");
}

LS_CASE(a_profile_change_while_on_traffic_returns_to_control_before_tuning)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t first = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };
    fake_file_t second = { SECOND_PROFILE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &first, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);

    /* A grant lands and the follower moves to a traffic channel of the
     * outgoing system, with a retune already latched for it. */
    radio.on_traffic = true;
    radio.traffic_hz = 853512500;
    radio.tune_latch = 853512500;
    radio.log[0] = '\0';
    radio.ops = 0;

    LS_EQ_INT(load(&program, &second, &ops, "/sdcard/second.txt"),
              P25_PROGRAM_OK);

    LS_EQ_STR(radio.log, "RFEQDSC");
    LS_CHECK(!radio.on_traffic);
    /* The stale traffic retune is gone: the last tune to reach the latch is
     * the new system's control channel, not the old system's traffic one. */
    LS_EQ_UINT(radio.tune_latch, 771031250);
    LS_EQ_UINT(radio.control_hz, 771031250);
    LS_EQ_STR(program.active.system_name, "Second System");
    LS_EQ_UINT(program.selected_control, 1);
    LS_EQ_UINT(radio.roster_first_tg, 4001);
    LS_NEAR(radio.cqpsk.timing_gain, 0.000078125f, 0.0f);
    LS_NEAR(radio.cqpsk.carrier_gain, 0.005f, 0.0f);
}

LS_CASE(app_handoff_reapplies_the_active_profile_without_rereading_it)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    LS_EQ_UINT(file.calls, 1);

    /* Leave P25, come back: the decoder restarts with defaults and the
     * program session has to put the site back. */
    fake_radio_init(&radio, &ops);
    LS_CHECK(p25_program_reapply(&program, &ops));

    LS_EQ_UINT(file.calls, 1);
    LS_EQ_STR(radio.log, "RFEQDSC");
    LS_EQ_UINT(radio.control_hz, 852237500);
    LS_EQ_UINT(radio.tune_latch, 852237500);
    LS_EQ_INT(radio.demod_preference, DEMOD_CQPSK);
    LS_NEAR(radio.cqpsk.timing_gain, 0.0003125f, 0.0f);
    LS_NEAR(radio.cqpsk.carrier_gain, 0.02f, 0.0f);
    LS_EQ_UINT(radio.roster_count, 2);
    LS_EQ_INT(program.state, P25_PROGRAM_LOADED);
}

LS_CASE(reapply_without_a_profile_does_nothing)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_CHECK(!p25_program_reapply(&program, &ops));
    LS_EQ_STR(radio.log, "");
}

LS_CASE(control_selection_steps_wraps_and_retunes_once)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    radio.log[0] = '\0';
    radio.ops = 0;

    LS_CHECK(p25_program_step_control(&program, +1, &ops));
    LS_EQ_UINT(program.selected_control, 0);
    LS_EQ_UINT(radio.control_hz, 851012500);
    LS_EQ_STR(radio.log, "RC");

    LS_CHECK(p25_program_step_control(&program, -1, &ops));
    LS_EQ_UINT(program.selected_control, 1);
    LS_EQ_UINT(radio.control_hz, 852237500);

    LS_CHECK(!p25_program_select_control(&program, 2, &ops));
    LS_EQ_UINT(program.selected_control, 1);

    char text[64];
    p25_program_format_selected(&program, text, sizeof(text));
    LS_EQ_STR(text, "2/2  852.237500 MHz");
}

LS_CASE(control_selection_is_refused_while_a_reload_is_in_flight)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    radio.log[0] = '\0';
    radio.ops = 0;

    LS_CHECK(p25_program_claim(&program, "/sdcard/p25_profile.txt"));
    LS_CHECK(!p25_program_step_control(&program, +1, &ops));
    LS_CHECK(!p25_program_reapply(&program, &ops));
    LS_EQ_STR(radio.log, "");
    LS_EQ_UINT(program.selected_control, 1);
}

LS_CASE(a_single_control_profile_has_nothing_to_step_to)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_MINIMAL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    radio.log[0] = '\0';
    radio.ops = 0;

    LS_CHECK(!p25_program_step_control(&program, +1, &ops));
    LS_EQ_STR(radio.log, "");

    char text[64];
    p25_program_format_controls(&program, text, sizeof(text));
    LS_EQ_STR(text, "> 1  851.012500 MHz");
}

LS_CASE(a_profile_that_disables_skip_does_not_open_the_vocoder_mute_gate)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    /* encrypted_skip=false in the full fixture: the follower stops leaving an
     * encrypted call, which is an operator's counting mode. It must not also
     * become permission to decode one. */
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    LS_CHECK(!radio.skip_encrypted);

    dsd_state state;
    dsd_opts opts;
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    opts.unmute_encrypted_p25 = 0;

    state.p25_ess_valid = 1;
    state.p25_algid = 0x84;
    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);
    /* Unknown ESS mutes too - a profile cannot turn that off either. */
    state.p25_ess_valid = 0;
    LS_EQ_INT(p25_ldu_should_mute_encrypted(&state, &opts), 1);
    LS_EQ_INT(opts.unmute_encrypted_p25, 0);
}

LS_CASE(panel_text_truncates_rather_than_overrunning)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);

    char guard[16];
    memset(guard, '#', sizeof(guard));
    p25_program_format_controls(&program, guard, 8);
    LS_EQ_STR(guard, "  1  85");
    LS_EQ_INT(guard[8], '#');

    /* A zero-size buffer must not be written at all. */
    guard[0] = '#';
    p25_program_format_status(&program, guard, 0);
    LS_EQ_INT(guard[0], '#');
}

LS_CASE(a_null_session_renders_the_first_run_empty_state)
{
    char text[64];
    p25_program_format_status(NULL, text, sizeof(text));
    LS_EQ_STR(text, "NO PROFILE LOADED");
    p25_program_format_source(NULL, text, sizeof(text));
    LS_EQ_STR(text, "(none)");
    p25_program_format_selected(NULL, text, sizeof(text));
    LS_EQ_STR(text, "-");
    p25_program_format_controls(NULL, text, sizeof(text));
    LS_EQ_STR(text, "(no profile loaded)");
    LS_EQ_STR(p25_program_state_name(NULL), "IDLE");
}

LS_CASE(abandoning_a_claim_clears_busy_and_keeps_the_active_profile)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };

    p25_program_init(&program);
    fake_radio_init(&radio, &ops);
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);

    /* The device path this models: the worker could not get 16 KiB of PSRAM
     * for staging, so the reload never happened at all. */
    LS_CHECK(p25_program_claim(&program, "/sdcard/p25_profile.txt"));
    LS_EQ_INT(p25_program_abandon(&program, P25_PROGRAM_ERR_NO_MEMORY),
              P25_PROGRAM_ERR_NO_MEMORY);
    LS_EQ_INT(program.state, P25_PROGRAM_FAILED);
    LS_CHECK(program.active_valid);
    LS_EQ_STR(program.active.system_name, "Example Regional Radio");

    char text[96];
    p25_program_format_status(&program, text, sizeof(text));
    LS_EQ_STR(text, "no memory to stage the profile  (active profile kept)");

    /* And the next reload is accepted, so nothing is wedged. */
    LS_EQ_INT(load(&program, &file, &ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    LS_EQ_INT(p25_program_abandon(&program, P25_PROGRAM_ERR_NO_MEMORY),
              P25_PROGRAM_ERR_NOT_CLAIMED);
}

LS_CASE(a_profile_roster_replaces_the_scan_controllers_lists)
{
    p25_scan_ctrl_t scan;
    p25_scan_init(&scan);

    LS_CHECK(p25_scan_allow_add(&scan, 9001));
    LS_CHECK(p25_scan_priority_set(&scan, 9001, 4));
    LS_CHECK(p25_scan_lockout_add(&scan, 9002));
    p25_scan_hold_set(&scan, 9001);
    p25_scan_list_set_mode(&scan, P25_SCAN_LIST_ALLOW);

    const p25_profile_talkgroup_t roster[] = {
        { 1201, "Operations", true,  7 },
        { 1202, "Facilities", true,  0 },
        { 1299, "Training",   false, 5 },
    };
    p25_program_apply_roster(&scan, roster, 3);

    LS_CHECK(p25_scan_is_allowed(&scan, 1201));
    LS_CHECK(p25_scan_is_allowed(&scan, 1202));
    LS_CHECK(!p25_scan_is_allowed(&scan, 1299));
    LS_CHECK(!p25_scan_is_allowed(&scan, 9001));
    LS_EQ_INT(p25_scan_list_get_mode(&scan), P25_SCAN_LIST_ALLOW);
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 1201), 7);
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 9001), 0);
    /* A disabled row cannot be followed, so it does not get a rank either. */
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 1299), 0);
    /* The hold named a talkgroup on the outgoing system. */
    LS_CHECK(!p25_scan_hold_active(&scan));

    LS_CHECK(p25_scan_is_locked_out(&scan, 9002));
}

LS_CASE(a_roster_with_no_enabled_talkgroups_follows_nothing)
{
    p25_scan_ctrl_t scan;
    p25_scan_init(&scan);
    LS_CHECK(p25_scan_allow_add(&scan, 9001));
    p25_scan_list_set_mode(&scan, P25_SCAN_LIST_ALLOW);

    const p25_profile_talkgroup_t roster[] = {
        { 1201, "Operations", false, 0 },
        { 1202, "Facilities", false, 0 },
    };
    p25_program_apply_roster(&scan, roster, 2);

    LS_EQ_INT(p25_scan_list_get_mode(&scan), P25_SCAN_LIST_NONE);
    LS_CHECK(!p25_scan_is_allowed(&scan, 1201));
    LS_CHECK(!p25_scan_is_allowed(&scan, 9001));
    /* The outgoing system's entry is gone as well. */
    LS_EQ_UINT(scan.allow_count, 0);

    /* A profile with no tg rows at all means no filter for the same reason. */
    p25_program_apply_roster(&scan, NULL, 0);
    LS_EQ_INT(p25_scan_list_get_mode(&scan), P25_SCAN_LIST_OFF);
}

LS_CASE(app_handoff_restores_priority_without_overwriting_persisted_edits)
{
    p25_scan_ctrl_t scan;
    p25_scan_init(&scan);
    p25_scan_list_set_mode(&scan, P25_SCAN_LIST_ALLOW);
    LS_CHECK(p25_scan_allow_add(&scan, 1202));
    LS_CHECK(p25_scan_lockout_add(&scan, 1201));
    p25_scan_hold_set(&scan, 1202);
    LS_CHECK(p25_scan_priority_set(&scan, 9999, 9));

    const p25_profile_talkgroup_t roster[] = {
        { 1201, "Operations", true,  7 },
        { 1202, "Facilities", false, 4 },
        { 1203, "Training",   true,  1 },
    };
    p25_program_restore_roster(&scan, roster, 3);

    LS_EQ_INT(p25_scan_list_get_mode(&scan), P25_SCAN_LIST_ALLOW);
    LS_CHECK(!p25_scan_allow_contains(&scan, 1201));
    LS_CHECK(p25_scan_allow_contains(&scan, 1202));
    LS_EQ_UINT(p25_scan_hold_get(&scan), 1202);
    LS_CHECK(p25_scan_is_locked_out(&scan, 1201));
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 9999), 0);
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 1201), 7);
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 1202), 0);
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 1203), 1);
}

/* ------------------------------------------------ survey fixtures */

static void survey_fixture(p25_program_t *program, fake_radio_t *radio,
                           p25_program_ops_t *ops)
{
    fake_file_t file = { P25_PROFILE_FULL_FIXTURE, P25_PROGRAM_OK, 0, NULL };
    p25_program_init(program);
    fake_radio_init(radio, ops);
    LS_EQ_INT(load(program, &file, ops, "/sdcard/p25_profile.txt"),
              P25_PROGRAM_OK);
    radio->log[0] = '\0';
    radio->ops = 0;
    radio->tune_requests = 0;
}

LS_CASE(survey_scores_only_profile_candidates_with_valid_protocol_evidence)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    survey_fixture(&program, &radio, &ops);

    /* Preferred index 1 is visited first.  Evidence during the 250 ms settle
     * is discarded; the first dwell gets 2 NIDs + 1 TSBK. */
    LS_CHECK(p25_program_survey_start(&program, 100, 10, 20, &ops));
    LS_EQ_UINT(radio.control_hz, 852237500);
    LS_CHECK(p25_program_survey_poll(&program, 350, 11, 20, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 1850, 13, 21, &ops));
    LS_EQ_UINT(radio.control_hz, 851012500);

    /* The other listed candidate wins with more CRC-valid TSBKs. */
    LS_CHECK(p25_program_survey_poll(&program, 2100, 13, 21, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 3600, 20, 25, &ops));
    LS_EQ_INT(program.survey.state, P25_SURVEY_FOUND);
    LS_EQ_UINT(program.selected_control, 0);
    LS_EQ_UINT(p25_program_selected_control_hz(&program), 851012500);
    LS_EQ_UINT(program.survey.selected_nids, 7);
    LS_EQ_UINT(program.survey.selected_tsbks, 4);

    char text[128];
    p25_program_format_survey(&program, text, sizeof(text));
    LS_EQ_STR(text, "LOCKED  851.012500 MHz  NID 7  TSBK 4");
}

LS_CASE(noise_only_survey_reports_no_control_and_restores_previous)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    survey_fixture(&program, &radio, &ops);

    LS_CHECK(p25_program_survey_start(&program, 100, 5, 7, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 350, 5, 7, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 1850, 5, 7, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 2100, 5, 7, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 3600, 5, 7, &ops));

    LS_EQ_INT(program.survey.state, P25_SURVEY_NO_CONTROL);
    LS_EQ_UINT(program.selected_control, 1);
    LS_EQ_UINT(radio.control_hz, 852237500);
    char text[128];
    p25_program_format_survey(&program, text, sizeof(text));
    LS_EQ_STR(text, "SEARCHING / NO CONTROL  (previous restored)");
}

LS_CASE(counter_loss_cannot_turn_unsigned_wrap_into_a_lock)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    survey_fixture(&program, &radio, &ops);

    LS_CHECK(p25_program_survey_start(&program, 0, 100, 80, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 250, 100, 80, &ops));
    /* Decoder stats reset during the dwell.  Unsigned subtraction would
     * fabricate an enormous score; the entire candidate must be rejected. */
    LS_CHECK(p25_program_survey_poll(&program, 1750, 2, 1, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 2000, 2, 1, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 3500, 2, 1, &ops));
    LS_EQ_INT(program.survey.state, P25_SURVEY_NO_CONTROL);
    LS_EQ_UINT(program.selected_control, 1);
}

LS_CASE(operator_cancel_restores_previous_control_and_is_visible)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    survey_fixture(&program, &radio, &ops);

    LS_CHECK(p25_program_survey_start(&program, 0, 0, 0, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 250, 0, 0, &ops));
    LS_CHECK(p25_program_survey_poll(&program, 1750, 0, 0, &ops));
    LS_EQ_UINT(radio.control_hz, 851012500);
    LS_CHECK(p25_program_survey_cancel(&program, P25_SURVEY_CANCEL_OPERATOR,
                                       &ops));
    LS_EQ_UINT(radio.control_hz, 852237500);
    LS_EQ_UINT(program.selected_control, 1);

    char text[128];
    p25_program_format_survey(&program, text, sizeof(text));
    LS_EQ_STR(text, "CANCELED - OPERATOR  (previous restored)");
}

LS_CASE(following_call_takes_ownership_after_cancel_restore)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    survey_fixture(&program, &radio, &ops);

    LS_CHECK(p25_program_survey_start(&program, 0, 0, 0, &ops));
    LS_CHECK(p25_program_survey_cancel(
        &program, P25_SURVEY_CANCEL_FOLLOWING_CALL, &ops));
    LS_EQ_UINT(radio.tune_latch, 852237500);

    /* Mirrors the app callback order: cancellation restores first, then the
     * current grant replaces that request.  No stale survey tune survives. */
    op_control(&radio, 853500000);
    radio.on_traffic = true;
    LS_EQ_UINT(radio.tune_latch, 853500000);
    LS_EQ_INT(program.survey.state, P25_SURVEY_CANCELED);
    char text[128];
    p25_program_format_survey(&program, text, sizeof(text));
    LS_EQ_STR(text, "CANCELED - FOLLOWING CALL  (previous restored)");
}

LS_CASE(profile_change_cancels_stale_survey_before_atomic_apply)
{
    p25_program_t program;
    fake_radio_t radio;
    p25_program_ops_t ops;
    survey_fixture(&program, &radio, &ops);
    LS_CHECK(p25_program_survey_start(&program, 0, 0, 0, &ops));

    fake_file_t second = { SECOND_PROFILE, P25_PROGRAM_OK, 0, NULL };
    LS_EQ_INT(load(&program, &second, &ops, "/sdcard/second.txt"),
              P25_PROGRAM_OK);
    LS_EQ_UINT(p25_program_selected_control_hz(&program), 771031250);
    LS_EQ_UINT(radio.tune_latch, 771031250);
    LS_EQ_INT(program.survey.state, P25_SURVEY_CANCELED);
    LS_EQ_INT(program.survey.cancel_reason, P25_SURVEY_CANCEL_PROFILE_CHANGE);
}
