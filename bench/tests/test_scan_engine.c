#include "ls_test.h"
#include <setjmp.h>
#include <string.h>
#include "scan_journal.h"

void scan_journal_init(void) {}
bool scan_journal_session_start(const scan_journal_record_t *r) { (void)r; return true; }
bool scan_journal_emit(const scan_journal_record_t *r) { (void)r; return true; }
bool scan_journal_session_stop(const scan_journal_record_t *r) { (void)r; return true; }
void scan_journal_get_status(scan_journal_status_t *out) { if (out) memset(out, 0, sizeof(*out)); }

#include "../../components/lakeshark/core/scan_engine.c"

p25_state_t P25;
fm_state_t FM;
volatile float p25_rx_power;
static app_t foreground = {.name = "FM"};
static scan_channel_t channels[3];
static ls_iq_control_status_t receiver;
static jmp_buf done;
static int tune_count, scenario, phase;
static uint32_t tuned[32];
static int64_t deadline;
static bool phase2_enabled;
static int switched;
/* The NFM receiver as the FM app presents it. The squelch is a noise gate that
   re-arms on every retune and cannot open for 128 ms after one (64 ms settle,
   64 ms of quiet), and the IQ level is the peak of a wideband block: about 3%
   on dead air and 5% on a strong broadcast. Defaults are the old fixture's. */
#define ANY_HZ 0xFFFFFFFFu
static uint32_t nfm_open_hz;           /* squelch opens on this frequency, 0 never */
static float nfm_iq = 0.01f, nfm_noise;
static int nfm_squelch = 4;
static int64_t last_tune_us;
static bool autosq_only;
uint8_t settings_get_scan_options(void) { return 0; }
bool settings_set_scan_options(uint8_t options) { (void)options; return true; }
int app_count(void) { return 2; }
const app_t *app_at(int i) { static app_t apps[]={{.name="P25"},{.name="FM"}}; return i>=0 && i<2 ? &apps[i] : NULL; }
void app_switch_to(int i) { foreground = *app_at(i); ++switched; }
void ls_gps_get(ls_gps_state_t *gps) { memset(gps,0,sizeof(*gps)); }
bool ls_gps_running(void) { return true; }
esp_err_t ls_gps_start(void) { return ESP_OK; }
void p25_p2_enable(bool on) { phase2_enabled = on; }

const app_t *app_current(void)
{
    return &foreground;
}
bool app_parked(void)
{
    return false;
}
bool app_switch_in_progress(void)
{
    return false;
}
int settings_get_scan_zone(void)
{
    return -1;
}
void settings_set_scan_zone(int z)
{
    (void)z;
}
int scan_channels_count(void)
{
    return 3;
}
const scan_channel_t *scan_channel_get(int i)
{
    return i >= 0 && i < 3 ? &channels[i] : NULL;
}
bool lakeshark_iq_receiver_ready(void)
{
    return receiver.receiver_streaming;
}
void fm_get_receiver_status(ls_iq_control_status_t *s)
{
    *s = receiver;
}
void p25_get_receiver_status(ls_iq_control_status_t *s)
{
    *s = receiver;
}
void lakeshark_fm_set_mode(int m)
{
    FM.mode = m;
}
void lakeshark_fm_set_squelch(int v)
{
    FM.squelch_tenths = v;
}
void lakeshark_fm_tune_transient(uint32_t hz)
{
    if (tune_count < 32)
        tuned[tune_count] = hz;
    tune_count++;
    FM.squelch_open = false;
    last_tune_us = esp_timer_get_time();
    receiver.requested_center_hz = hz;
    receiver.effective_center_hz = hz;
    receiver.effective_center_known = true;
    receiver.tune_state = scenario == 5 ? LS_IQ_RESULT_FAILED : LS_IQ_RESULT_EFFECTIVE;
    receiver.tune_error = scenario == 5 ? LS_RADIO_ERR_IO : LS_RADIO_OK;
}
void p25_request_tune(uint32_t hz, bool fast)
{
    lakeshark_fm_tune_transient(hz);
}
void p25_return_to_control(void) {}
bool p25_program_survey_cancel_now(p25_survey_cancel_t reason)
{
    return true;
}
const char *ls_radio_err_name(ls_radio_err_t e)
{
    return e == LS_RADIO_OK ? "ok" : "error";
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name, uint32_t depth,
                                           void *arg, UBaseType_t priority, StackType_t *stack,
                                           StaticTask_t *tcb, BaseType_t core)
{
    return (TaskHandle_t)1;
}
void vTaskDelay(TickType_t ticks)
{
    ls_shim_time_advance((int64_t)ticks * 1000);
    int64_t now = esp_timer_get_time();
    if (tune_count && nfm_open_hz &&
        (nfm_open_hz == ANY_HZ || nfm_open_hz == receiver.requested_center_hz) &&
        now - last_tune_us >= 130000)
        FM.squelch_open = true;
    if (scenario == 13 && now >= 700000)
        nfm_open_hz = channels[0].freq_hz;
    if (tune_count && !phase) {
        if (scenario == 1 || scenario == 2 || scenario == 4)
            scan_engine_hold(true);
        if (scenario == 3)
            scan_engine_skip();
        phase = 1;
    }
    if (scenario == 2 && phase == 1 && now >= 500000) {
        scan_engine_next();
        phase = 2;
    }
    if (scenario == 4 && phase == 1 && now >= 500000) {
        receiver.receiver_streaming = false;
        phase = 2;
    }
    if (scenario == 8 && phase == 1 && now >= 200000) {
        scan_engine_skip();
        phase = 2;
    }
    if (now >= deadline)
        longjmp(done, 1);
}
static void run(int which, int ms)
{
    s_mixed = s_location = false;
    scan_engine_stop();
    memset(&FM, 0, sizeof(FM));
    memset(&P25, 0, sizeof(P25));
    memset(&receiver, 0, sizeof(receiver));
    receiver.receiver_streaming = true;
    FM.mode = FM_MODE_LISTEN;
    FM.squelch_tenths = nfm_squelch;
    FM.iq_level = nfm_iq;
    FM.noise = nfm_noise;
    for (int i = 0; i < 3; i++) {
        memset(&channels[i], 0, sizeof(channels[i]));
        channels[i].freq_hz = 150000000 + i * 12500;
        channels[i].mode = SCAN_MODE_NFM;
        channels[i].flags = SCAN_FLAG_ENABLED;
        snprintf(channels[i].name, 16, "CH%d", i);
    }
    s_fg_mode = -1;
    s_order_n = 0;
    s_cur = -1;
    const bool p25 = which >= 6 && which < 11;
    foreground.name = p25 ? "P25" : "FM";
    p25_rx_power = p25 ? 0.5f : 0.0f;
    P25.dsd_has_sync = which == 7;
    if (p25)
        for (int i = 0; i < 3; i++)
            channels[i].mode = SCAN_MODE_P25;
    scenario = which;
    phase = tune_count = 0;
    scan_engine_init();
    scan_engine_set_source(SCAN_SRC_CHANNELS);
    if (which == 9) {
        s_mixed = true; switched = 0; foreground.name = "FM";
        channels[0].mode = SCAN_MODE_NFM; channels[1].mode = SCAN_MODE_P25; channels[2].mode = SCAN_MODE_NFM;
        p25_rx_power = 0.01f;
    }
    if (which == 10) s_location = true;
    if (which == 13) channels[0].flags |= SCAN_FLAG_PRIORITY;
    if (which == 13) scan_engine_set_priority_ms(500);
    if (autosq_only) s_autosq_req = true;
    else scan_engine_start();
    ls_shim_time_set(0);
    deadline = (int64_t)ms * 1000;
    if (!setjmp(done))
        scan_task(NULL);
}
static void nfm_reset(void)
{
    nfm_open_hz = 0;
    nfm_iq = 0.01f;
    nfm_noise = 0.0f;
    nfm_squelch = 4;
    autosq_only = false;
    scan_engine_set_priority_ms(0);
}
LS_CASE(scan_quiet_channels_cycle_after_confirmed_tunes)
{
    run(0, 850);
    LS_CHECK(tune_count >= 4);
    LS_EQ_INT(tuned[0], 150000000);
    LS_EQ_INT(tuned[1], 150012500);
    LS_EQ_INT(tuned[2], 150025000);
    LS_EQ_INT(tuned[3], 150000000);
}
LS_CASE(scan_manual_hold_waits_even_without_carrier)
{
    run(1, 1000);
    LS_EQ_INT(tune_count, 1);
    LS_CHECK(scan_engine_manual_hold());
    LS_EQ_INT(scan_engine_phase(), SCAN_PHASE_HELD);
}
LS_CASE(scan_next_releases_hold_without_skipping_channel)
{
    run(2, 1100);
    LS_CHECK(tune_count >= 4);
    LS_CHECK(!scan_engine_manual_hold());
    LS_EQ_INT(tuned[3], 150000000);
    LS_EQ_INT(s_session_skip[0], 0);
}
LS_CASE(scan_skip_during_measurement_excludes_only_that_channel)
{
    run(3, 800);
    LS_CHECK(tune_count >= 4);
    LS_EQ_INT(s_session_skip[0], 1);
    for (int i = 1; i < tune_count; i++)
        LS_CHECK(tuned[i] != 150000000);
}
LS_CASE(scan_disconnect_releases_manual_hold)
{
    run(4, 1000);
    LS_EQ_INT(tune_count, 1);
    LS_CHECK(!scan_engine_manual_hold());
    LS_EQ_INT(scan_engine_candidate(), -1);
    LS_EQ_INT(scan_engine_phase(), SCAN_PHASE_ERROR);
}
LS_CASE(scan_failed_tune_never_becomes_a_signal_hold)
{
    run(5, 800);
    LS_EQ_INT(scan_engine_current(), -1);
    LS_EQ_INT(scan_engine_phase(), SCAN_PHASE_ERROR);
}

LS_CASE(p25_scan_waits_for_sync_before_holding)
{
    run(6, 1400);
    LS_EQ_INT(tune_count, 2);
    LS_EQ_INT(scan_engine_current(), -1);
}
LS_CASE(p25_scan_holds_a_synchronized_channel)
{
    run(7, 1400);
    LS_EQ_INT(tune_count, 1);
    LS_EQ_INT(scan_engine_phase(), SCAN_PHASE_HELD);
}
LS_CASE(p25_skip_interrupts_the_sync_dwell)
{
    run(8, 500);
    LS_EQ_INT(tune_count, 2);
    LS_EQ_INT(s_session_skip[0], 1);
}

LS_CASE(starting_p25_scan_disables_manual_phase2)
{
    foreground.name = "P25";
    phase2_enabled = true;
    scan_engine_start();
    LS_CHECK(!phase2_enabled);
    scan_engine_stop();
}
LS_CASE(mixed_scan_switches_decoder_and_keeps_list_progress)
{
    run(9,1000);
    LS_CHECK(switched >= 2); LS_CHECK(tune_count >= 3);
    LS_EQ_INT(tuned[0],150000000); LS_EQ_INT(tuned[1],150012500); LS_EQ_INT(tuned[2],150025000);
}
/* The default squelch (30) and the level a real strong broadcast reads (5%).
   The level never reaches the squelch number, which is what kept the scanner
   from ever holding an analog channel; the open squelch is the carrier. */
LS_CASE(nfm_scan_holds_a_channel_the_squelch_opens_on)
{
    nfm_squelch = 30; nfm_iq = 0.05f; nfm_open_hz = ANY_HZ;
    run(11, 1500);
    nfm_reset();
    LS_EQ_INT(tune_count, 1);
    LS_EQ_INT(scan_engine_current(), 0);
    LS_EQ_INT(scan_engine_phase(), SCAN_PHASE_HELD);
}
LS_CASE(nfm_scan_does_not_hold_with_the_squelch_closed)
{
    nfm_squelch = 30; nfm_iq = 0.05f;
    run(12, 1500);
    nfm_reset();
    LS_CHECK(tune_count >= 4);
    LS_EQ_INT(scan_engine_current(), -1);
    LS_CHECK(scan_engine_phase() != SCAN_PHASE_HELD);
}
LS_CASE(nfm_scan_ignores_a_high_level_while_the_squelch_is_closed)
{
    nfm_squelch = 30; nfm_iq = 0.9f;
    run(12, 1500);
    nfm_reset();
    LS_CHECK(tune_count >= 4);
    LS_EQ_INT(scan_engine_current(), -1);
}
LS_CASE(nfm_priority_sample_sees_the_squelch_open)
{
    /* CH2 holds first; the priority channel CH0 only opens later. Its sample
       window has to outlast the squelch's re-arm or it can never be seen. */
    nfm_squelch = 30; nfm_iq = 0.05f; nfm_open_hz = 150025000;
    run(13, 2500);
    nfm_reset();
    LS_EQ_INT(scan_engine_current(), 0);
    LS_EQ_INT(scan_engine_phase(), SCAN_PHASE_HELD);
}
LS_CASE(autosquelch_sets_the_gate_below_the_noise_floor_not_from_the_iq_level)
{
    /* Dead air: 3% level, 88% noise. The squelch opens under (100 - setting)
       so it must land under 88, not at level+margin = 9. 100-88+15 = 27. */
    nfm_squelch = 30; nfm_iq = 0.03f; nfm_noise = 0.88f; autosq_only = true;
    run(14, 1500);
    const int sq = FM.squelch_tenths, floor = scan_engine_autosquelch_floor();
    nfm_reset();
    LS_EQ_INT(floor, 88);
    LS_EQ_INT(sq, 27);
}
LS_CASE(autosquelch_leaves_the_squelch_alone_without_a_noise_reading)
{
    nfm_squelch = 30; nfm_iq = 0.03f; nfm_noise = 0.0f; autosq_only = true;
    run(14, 1500);
    const int sq = FM.squelch_tenths;
    nfm_reset();
    LS_EQ_INT(sq, 30);
}

LS_CASE(location_scan_never_tunes_without_a_fix)
{
    run(10,1000);
    LS_EQ_INT(tune_count,0);
    char status[96]; scan_engine_status(status,sizeof(status));
    LS_CHECK(strstr(status,"GPS") != NULL);
}

LS_CASE(session_skip_supports_channels_above_64)
{
    memset(s_session_skip,0,sizeof(s_session_skip));
    sess_skip(64); sess_skip(SCAN_MAX_CHANNELS-1);
    LS_CHECK(sess_skipped(64)); LS_CHECK(sess_skipped(SCAN_MAX_CHANNELS-1));
    LS_CHECK(!sess_skipped(0)); LS_CHECK(!sess_skipped(63));
}

LS_CASE(scan_audio_is_closed_during_search_and_decoder_changes)
{
    s_enabled=true; s_cur=-1; s_handoff=false;
    LS_CHECK(!scan_engine_audio_open());
    s_cur=1; LS_CHECK(scan_engine_audio_open());
    s_handoff=true; LS_CHECK(!scan_engine_audio_open());
    s_handoff=false; s_enabled=false; s_cur=-1;
    LS_CHECK(scan_engine_audio_open());
}

LS_CASE(nfm_tune_restores_the_channels_receive_tone)
{
    scan_channel_t channel={.name="Tone",.freq_hz=154785000,.mode=SCAN_MODE_NFM,
                            .rsv=2,.flags=SCAN_FLAG_ENABLED | SCAN_FLAG_TONE_HIGH};
    scenario=0; tune_count=0;
    tune_to(&channel);
    LS_EQ_INT(258,FM.tone_required);
    LS_CHECK(!carrier_held(SCAN_MODE_NFM));
    channel.rsv=0; channel.flags=SCAN_FLAG_ENABLED;
    tune_to(&channel);
    LS_EQ_INT(0,FM.tone_required);
}
