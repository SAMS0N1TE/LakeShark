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
static scan_channel_t channels[128];
static int s_channel_count = 3;
static ls_iq_control_status_t receiver;
static jmp_buf done;
static int tune_count, scenario, phase, seek_interrupt;
static int completion_foreground_checks;
static uint32_t tuned[32];
static int64_t deadline;
static bool phase2_enabled;
static int switched;
static bool mock_lr_radio;
static bool mock_sdr_present;
static bool mock_sdr_owned;
static bool mock_sdr_lora_range;
static bool mock_endpoint_metadata_unknown;
static ls_rsel_radio_t mock_saved_radio = LS_RSEL_NONE;
static int mock_endpoint_count_calls, mock_endpoint_info_calls;

ls_lora_chip_t ls_lora_chip(void)
{
    return mock_lr_radio ? LS_LORA_CHIP_LR20XX : LS_LORA_CHIP_SX126X;
}
uint32_t ls_lora_caps(void)
{
    return LS_LORA_CAP_FSK_STREAM | LS_LORA_CAP_RX_WIDE | LS_LORA_CAP_BAND_1G5_2G5;
}
size_t ls_radio_endpoint_count(void)
{
    mock_endpoint_count_calls++;
    return mock_sdr_present ? 1 : 0;
}
ls_radio_err_t ls_radio_endpoint_info(size_t index, ls_radio_endpoint_info_t *out)
{
    mock_endpoint_info_calls++;
    if (mock_endpoint_metadata_unknown) return LS_RADIO_ERR_IO;
    if (!mock_sdr_present || index != 0 || !out) return LS_RADIO_ERR_UNAVAILABLE;
    memset(out, 0, sizeof(*out));
    snprintf(out->endpoint_id, sizeof(out->endpoint_id), "%s", LS_RADIO_ENDPOINT_RTL_USB);
    out->present = true;
    out->leased = mock_sdr_owned;
    if (mock_sdr_owned) snprintf(out->owner, sizeof(out->owner), "fm");
    out->frequency_range_count = 1;
    out->frequency_ranges[0].min_hz = mock_sdr_lora_range ? 150000000u : 24000000u;
    out->frequency_ranges[0].max_hz = mock_sdr_lora_range ? 960000000u : 1766000000u;
    return LS_RADIO_OK;
}
ls_radio_err_t ls_radio_endpoint_try_get(const char *id, ls_radio_endpoint_info_t *out)
{
    (void)id;
    return ls_radio_endpoint_info(0, out);
}
ls_rsel_radio_t ls_rsel_saved(ls_rsel_job_t job)
{
    (void)job;
    return mock_saved_radio;
}
const char *ls_rsel_sdr_endpoint(ls_rsel_job_t job)
{
    (void)job;
    return mock_sdr_present && mock_saved_radio != LS_RSEL_LORA
        ? LS_RADIO_ENDPOINT_RTL_USB : NULL;
}
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
    /* The third foreground check after measurement is the final completion
       check. Its generation operand has already been read by the worker. */
    if (scenario == 37 && phase != 2 && s_pass.measure_us > 0 &&
        ++completion_foreground_checks == 3) {
        phase = 2;
        LS_CHECK(scan_engine_seek(150000000, 12500, -1, 150000000, 150025000));
    }
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
    return s_channel_count;
}
const scan_channel_t *scan_channel_get(int i)
{
    return i >= 0 && i < s_channel_count ? &channels[i] : NULL;
}
bool lakeshark_iq_receiver_ready(void)
{
    return (!mock_lr_radio || (mock_sdr_present && mock_saved_radio != LS_RSEL_LORA)) &&
           receiver.receiver_streaming;
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
    FM.freq_hz = hz;
    FM.squelch_open = false;
    last_tune_us = esp_timer_get_time();
    if (FM.lr_iq_active) {
        /* LR restarts its existing waveform consumer on FM.freq_hz; USB
           discovery remains empty. A new block acknowledges the new tune. */
        receiver.requested_center_hz = hz;
        receiver.tune_state = LS_IQ_RESULT_PENDING;
        receiver.receiver_streaming = false;
        return;
    }
    receiver.requested_center_hz = hz;
    receiver.effective_center_hz = hz;
    receiver.effective_center_known = true;
    const bool explicitly_unsupported = (scenario == 22 || scenario == 23) &&
                                         hz == channels[0].freq_hz;
    receiver.tune_state = (scenario == 5 || explicitly_unsupported)
        ? LS_IQ_RESULT_FAILED : LS_IQ_RESULT_EFFECTIVE;
    receiver.tune_error = scenario == 5 ? LS_RADIO_ERR_IO
        : explicitly_unsupported ? LS_RADIO_ERR_UNSUPPORTED : LS_RADIO_OK;
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
    if (scenario == 37 && phase == 2) {
        LS_CHECK(scan_engine_active());
        LS_EQ_INT(scan_engine_seek_direction(), -1);
        longjmp(done, 1);
    }
    ls_shim_time_advance((int64_t)ticks * 1000);
    int64_t now = esp_timer_get_time();
    if (FM.lr_iq_active && tune_count && now - last_tune_us >= 30000) {
        receiver.receiver_streaming = true;
        receiver.effective_center_hz = FM.freq_hz;
        receiver.effective_center_known = true;
        receiver.tune_state = LS_IQ_RESULT_EFFECTIVE;
        receiver.tune_error = LS_RADIO_OK;
    }
    if (tune_count && nfm_open_hz &&
        (nfm_open_hz == ANY_HZ || nfm_open_hz == receiver.requested_center_hz) &&
        now - last_tune_us >= 130000)
        FM.squelch_open = true;
    if (scenario == 13 && now >= 700000)
        nfm_open_hz = channels[0].freq_hz;
    if (tune_count && !phase && scenario != 20) {
        if (scenario == 1 || scenario == 2 || scenario == 4 || scenario == 31)
            scan_engine_hold(true);
        if (scenario == 3 || scenario == 35)
            scan_engine_skip();
        if (scenario == 36) scan_engine_next();
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
    if (scenario == 20 && tune_count > 0 && phase == 0) {
        scan_engine_set_source(SCAN_SRC_BAND);
        phase = 1;
    }
    if (scenario == 31 && phase == 1 && now >= 500000) {
        LS_EQ_INT(tune_count, 1);
        LS_CHECK(scan_engine_manual_hold());
        LS_CHECK(scan_engine_audio_open());
        scan_engine_next(); phase = 2;
    }
    if (scan_engine_seek_direction() && tune_count && now >= 80000 && phase < 2) {
        if (scenario == 32 || scenario == 33) {
            phase = 2;
            if (scenario == 32) scan_engine_next();
            else scan_engine_skip();
        } else if (scenario == 25) {
            phase=2; scan_engine_stop(); FM.freq_hz=155000000;
        } else if (scenario == 26) {
            phase=2;
            (void)scan_engine_seek(150000000,12500,-1,150000000,150025000);
        }
    }
    if (now >= deadline)
        longjmp(done, 1);
}
static void run(int which, int ms)
{
    s_channel_count = 3;
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
    mock_lr_radio = false;
    mock_sdr_present = false;
    mock_sdr_owned = false;
    mock_sdr_lora_range = false;
    mock_endpoint_metadata_unknown = false;
    mock_saved_radio = LS_RSEL_NONE;
    mock_endpoint_count_calls = mock_endpoint_info_calls = 0;
    if (which >= 15 && which <= 20) {
        mock_lr_radio = true;
        mock_saved_radio = LS_RSEL_LORA;
        mock_sdr_owned = which == 18;
        mock_sdr_present = mock_sdr_owned;
        if (which == 18) mock_saved_radio = LS_RSEL_SDR_RTL;
        channels[0].freq_hz = 88000000;
        channels[1].freq_hz = 162550000;
        channels[2].freq_hz = 450000000;
        if (which == 16) channels[1].freq_hz = 2800000000u;
        if (which == 16) channels[2].freq_hz = 100000000;
        if (which == 19) {
            channels[1].freq_hz = 1550000000;
            channels[2].freq_hz = 3000000000u;
        }
    }
    if (which == 21) {
        mock_lr_radio = mock_sdr_present = mock_sdr_lora_range = true;
        mock_saved_radio = LS_RSEL_SDR_RTL;
        channels[0].freq_hz = 88000000;
        channels[1].freq_hz = 162550000;
        channels[2].freq_hz = 450000000;
    }
    if (which == 23) {
        /* A selected SDR with unknown frequency metadata remains eligible;
           the driver result must still skip a specifically unsupported tune. */
        mock_lr_radio = mock_sdr_present = mock_endpoint_metadata_unknown = true;
        mock_saved_radio = LS_RSEL_SDR_RTL;
        channels[0].freq_hz = 140000000u;
    }
    if (which == 24) {
        /* An explicit LR selection needs no unrelated endpoint metadata to
           reject these frequencies before the app falls through to SDR open. */
        mock_lr_radio = mock_sdr_present = mock_endpoint_metadata_unknown = true;
        mock_saved_radio = LS_RSEL_LORA;
        channels[0].freq_hz = 90000000u;     /* outside the LR2021's 104-2740 MHz */
        channels[1].freq_hz = 2900000000u;
        channels[2].freq_hz = 100000000u;
    }
    s_fg_mode = -1;
    s_order_n = 0;
    s_cur = -1;
    const bool p25 = (which >= 6 && which < 11) || which == 19;
    foreground.name = p25 ? "P25" : "FM";
    p25_rx_power = p25 ? 0.5f : 0.0f;
    P25.dsd_has_sync = which == 7;
    if (p25)
        for (int i = 0; i < 3; i++)
            channels[i].mode = SCAN_MODE_P25;
    if (which >= 27 && which <= 30) {
        mock_lr_radio = true;
        mock_saved_radio = which == 30 ? LS_RSEL_NONE : LS_RSEL_LORA;
        FM.lr_iq_active = true;
        if (which == 30) channels[0].freq_hz = 103987500;
    }
    scenario = which;
    phase = tune_count = 0;
    scan_engine_init();
    scan_engine_set_source(SCAN_SRC_CHANNELS);
    if (which == 35 || which == 36) {
        scan_engine_set_band(150000000, 150025000, 12500);
        scan_engine_set_source(SCAN_SRC_BAND);
    }
    if (which == 9) {
        s_mixed = true; switched = 0; foreground.name = "FM";
        channels[0].mode = SCAN_MODE_NFM; channels[1].mode = SCAN_MODE_P25; channels[2].mode = SCAN_MODE_NFM;
        p25_rx_power = 0.01f;
    }
    if (which == 10) s_location = true;
    if (which == 13) channels[0].flags |= SCAN_FLAG_PRIORITY;
    if (which == 13) scan_engine_set_priority_ms(500);
    if (which == 29) {
        scan_engine_set_band(103987500,104025000,12500);
        scan_engine_set_source(SCAN_SRC_BAND);
    }
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
    run(0, 1100);
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
    run(2, 1450);
    LS_CHECK(tune_count >= 4);
    LS_CHECK(!scan_engine_manual_hold());
    LS_EQ_INT(tuned[3], 150000000);
    LS_EQ_INT(s_session_skip[0], 0);
}
LS_CASE(scan_skip_during_measurement_excludes_only_that_channel)
{
    run(3, 1100);
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

LS_CASE(lr_scan_skips_first_unsupported_frequency_and_wraps_supported_list)
{
    run(15, 900);
    LS_CHECK(tune_count >= 3);
    LS_EQ_INT(tuned[0], 162550000);
    LS_EQ_INT(tuned[1], 450000000);
    LS_EQ_INT(tuned[2], 162550000);
    for (int i = 0; i < tune_count && i < 32; ++i)
        LS_CHECK(tuned[i] != 88000000);
    LS_CHECK(scan_engine_active());
}

LS_CASE(lr_scan_mixed_list_continues_after_unsupported_entry)
{
    run(17, 650);
    LS_CHECK(tune_count >= 2);
    LS_EQ_INT(tuned[0], 162550000);
    LS_CHECK(tuned[1] == 450000000 || tuned[1] == 162550000);
    for (int i = 0; i < tune_count && i < 32; ++i)
        LS_CHECK(tuned[i] != 88000000);
}

LS_CASE(lr_scan_all_unsupported_idles_with_reason_and_no_tunes)
{
    run(16, 900);
    LS_EQ_INT(tune_count, 0);
    LS_CHECK(scan_engine_active());
    LS_EQ_INT(candidate_count(), 0);
    char status[96];
    scan_engine_status(status, sizeof(status));
    LS_CHECK(strstr(status, "unsupported") != NULL);
}

LS_CASE(sdr_scan_does_not_apply_lr_frequency_restrictions)
{
    /* With an SDR held by the active app, the LR2021's narrower FM path does
       not reject the channel. */
    run(18, 350);
    LS_CHECK(tune_count > 0);
    LS_EQ_INT(tuned[0], 88000000);
}

LS_CASE(sdr_scan_skips_channels_outside_its_advertised_ranges)
{
    run(21, 600);
    LS_CHECK(tune_count >= 2);
    LS_EQ_INT(tuned[0], 162550000);
    for (int i = 0; i < tune_count && i < 32; ++i)
        LS_CHECK(tuned[i] != 88000000);
}

LS_CASE(channel_capability_snapshot_reads_registry_once_per_list_pass)
{
    s_channel_count = 96;
    mock_lr_radio = true;
    mock_sdr_present = true;
    mock_sdr_owned = false;
    mock_sdr_lora_range = false;
    mock_saved_radio = LS_RSEL_SDR_RTL;
    mock_endpoint_count_calls = mock_endpoint_info_calls = 0;
    s_src = SCAN_SRC_CHANNELS;
    s_fg_mode = SCAN_MODE_NFM;
    s_mixed = s_location = false;
    s_zone = -1;
    memset(s_session_skip, 0, sizeof(s_session_skip));
    for (int i = 0; i < s_channel_count; ++i) {
        channels[i].freq_hz = 155000000u + (uint32_t)i * 12500u;
        channels[i].mode = SCAN_MODE_NFM;
        channels[i].flags = SCAN_FLAG_ENABLED;
    }

    LS_EQ_INT(candidate_count(), s_channel_count);
    LS_EQ_INT(mock_endpoint_count_calls, 1);
    LS_EQ_INT(mock_endpoint_info_calls, 1);

    s_channel_count = 3;
    mock_lr_radio = false;
    mock_sdr_present = false;
    mock_saved_radio = LS_RSEL_NONE;
}

LS_CASE(lr_p25_scan_uses_the_noncontiguous_receiver_ranges)
{
    run(19, 500);
    LS_CHECK(tune_count > 0);
    LS_EQ_INT(tuned[0], 1550000000);
    for (int i = 0; i < tune_count && i < 32; ++i) {
        LS_CHECK(tuned[i] != 88000000);
        LS_CHECK(tuned[i] != 3000000000u);
    }
}

LS_CASE(switching_to_band_source_rebuilds_after_lr_channel_filtering)
{
    run(20, 650);
    LS_CHECK(tune_count >= 2);
    LS_EQ_INT(scan_engine_get_source(), SCAN_SRC_BAND);
    LS_CHECK(tuned[0] != 88000000);
}

LS_CASE(explicit_unsupported_tune_skips_only_that_channel_for_the_session)
{
    run(22, 1000);
    LS_CHECK(tune_count >= 3);
    LS_EQ_INT(tuned[0], 150000000);
    LS_EQ_INT(tuned[1], 150012500);
    LS_CHECK(sess_skipped(0));
    for (int i = 1; i < tune_count && i < 32; ++i)
        LS_CHECK(tuned[i] != 150000000);
    LS_CHECK(scan_engine_phase() != SCAN_PHASE_ERROR);
}

LS_CASE(unknown_endpoint_metadata_still_skips_driver_reported_unsupported_tune)
{
    run(23, 1000);
    LS_CHECK(tune_count >= 2);
    LS_EQ_INT(tuned[0], 140000000u);
    LS_EQ_INT(tuned[1], 150012500u);
    LS_CHECK(sess_skipped(0));
    LS_CHECK(scan_engine_phase() != SCAN_PHASE_ERROR);
}

LS_CASE(explicit_lr_selection_filters_unsupported_fm_when_other_metadata_is_unknown)
{
    run(24, 800);
    LS_EQ_INT(tune_count, 0);
    LS_EQ_INT(candidate_count(), 0);
    char status[96];
    scan_engine_status(status, sizeof(status));
    LS_CHECK(strstr(status, "unsupported") != NULL);
}

LS_CASE(mixed_p25_mode_honors_saved_sdr_while_it_is_unleased)
{
    scan_channel_t channel = {.freq_hz = 1550000000u, .mode = SCAN_MODE_P25,
                              .flags = SCAN_FLAG_ENABLED};
    mock_lr_radio = mock_sdr_present = true;
    mock_sdr_owned = false;
    mock_saved_radio = LS_RSEL_SDR_RTL;
    radio_snapshot_refresh();
    LS_CHECK(channel_radio_supported(&channel));
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

static bool seek_locked;
void lakeshark_fm_frequency_lock(bool on) { seek_locked = on; }
bool lakeshark_fm_set_freq(uint32_t hz) { FM.freq_hz = hz; return true; }
static bool seek_lr;
static void run_seek_range(int dir, uint32_t from, uint32_t hit, uint32_t lo, uint32_t hi)
{
    nfm_reset(); run(0, 1); scan_engine_stop();
    tune_count = 0; phase=0; completion_foreground_checks=0; scenario=seek_interrupt; seek_interrupt=0; nfm_open_hz=hit;
    FM.freq_hz=from; s_task_ready=true; foreground.name="FM";
    if (seek_lr) {
        mock_lr_radio = true; mock_saved_radio = LS_RSEL_LORA;
        FM.lr_iq_active = true;
    }
    LS_CHECK(scan_engine_seek(from, 12500, dir, lo, hi));
    ls_shim_time_set(0); deadline=2000000;
    if (!setjmp(done)) scan_task(NULL);
}
static void run_seek(int dir, uint32_t from, uint32_t hit)
{
    run_seek_range(dir,from,hit,150000000,150025000);
}
LS_CASE(seek_up_wraps_once_then_locks_without_a_hit)
{
    run_seek(1,150012500,0);
    LS_EQ_INT(tune_count,3);
    LS_EQ_UINT(tuned[0],150025000); LS_EQ_UINT(tuned[1],150000000);
    LS_EQ_UINT(tuned[2],150012500);
    LS_CHECK(!scan_engine_active()); LS_CHECK(seek_locked);
}
LS_CASE(seek_down_qualifies_squelch_and_stays_on_first_hit)
{
    run_seek(-1,150000000,150012500);
    LS_EQ_INT(tune_count,2);
    LS_EQ_UINT(tuned[0],150025000); LS_EQ_UINT(tuned[1],150012500);
    LS_EQ_UINT(FM.freq_hz,150012500); LS_CHECK(seek_locked);
    LS_CHECK(!scan_engine_seek_direction());
    nfm_reset();
}
LS_CASE(seek_cancel_and_invalid_requests)
{
    run_seek(1,150000000,0);
    LS_CHECK(scan_engine_seek(150000000,6250,1,150000000,150025000));
    LS_CHECK(!seek_locked); scan_engine_stop(); LS_CHECK(seek_locked);
    LS_CHECK(!scan_engine_seek_direction());
    LS_CHECK(!scan_engine_seek(150000000,0,1,150000000,150025000));
    LS_CHECK(!scan_engine_seek(149000000,6250,1,150000000,150025000));
}

LS_CASE(seek_cancellation_during_qualification_keeps_the_manual_carrier)
{
    seek_interrupt=25; run_seek(1,150000000,150012500);
    LS_EQ_INT(tune_count,1); LS_EQ_UINT(FM.freq_hz,155000000);
    LS_CHECK(!scan_engine_active()); LS_CHECK(seek_locked); nfm_reset();
}
LS_CASE(replacement_seek_cannot_finish_the_old_measurement)
{
    seek_interrupt=26; run_seek(1,150000000,150025000);
    LS_EQ_INT(tune_count,2); LS_EQ_UINT(tuned[1],150025000);
    LS_EQ_UINT(FM.freq_hz,150025000); LS_CHECK(seek_locked); nfm_reset();
}
LS_CASE(seek_down_finishes_one_quiet_lap_and_preserves_scanner_band)
{
    uint32_t lo,hi,step;
    scan_engine_set_band(433000000,434000000,6250);
    run_seek(-1,150000000,0);
    LS_EQ_INT(tune_count,3); LS_EQ_UINT(tuned[0],150025000);
    LS_EQ_UINT(tuned[2],150000000);
    scan_engine_get_band(&lo,&hi,&step);
    LS_EQ_UINT(lo,433000000); LS_EQ_UINT(hi,434000000); LS_EQ_UINT(step,6250);
}
LS_CASE(seek_keeps_an_off_grid_manual_entry_on_its_own_raster)
{
    run_seek(1,150001234,0);
    LS_EQ_INT(tune_count,2);
    LS_EQ_UINT(tuned[0],150013734); LS_EQ_UINT(tuned[1],150001234);
}

LS_CASE(lr_list_scan_steps_without_a_usb_iq_receiver)
{
    nfm_reset(); run(27,1100);
    LS_CHECK(!lakeshark_iq_receiver_ready());
    LS_CHECK(tune_count >= 4);
    LS_EQ_UINT(tuned[0],150000000); LS_EQ_UINT(tuned[1],150012500);
    LS_EQ_INT(scan_engine_phase(),SCAN_PHASE_SCANNING);
    char status[96]; scan_engine_status(status,sizeof(status));
    LS_CHECK(strstr(status,"no IQ receiver") == NULL);
}
LS_CASE(lr_list_scan_stops_on_published_squelch_not_iq_level)
{
    nfm_reset(); nfm_open_hz=150012500; nfm_iq=0;
    run(28,1200);
    LS_EQ_INT(tune_count,2); LS_EQ_INT(scan_engine_current(),1);
    LS_EQ_INT(scan_engine_phase(),SCAN_PHASE_HELD);
    nfm_reset();
}
LS_CASE(lr_band_scan_skips_below_104mhz_and_holds_open)
{
    nfm_reset(); nfm_open_hz=104012500;
    run(29,1200);
    LS_EQ_INT(tune_count,2);
    LS_EQ_UINT(tuned[0],104000000); LS_EQ_UINT(tuned[1],104012500);
    LS_EQ_INT(scan_engine_phase(),SCAN_PHASE_HELD); nfm_reset();
}
LS_CASE(lr_fallback_without_saved_picker_choice_filters_receive_range)
{
    nfm_reset(); run(30,800);
    LS_CHECK(tune_count >= 2);
    LS_EQ_UINT(tuned[0],150012500);
    LS_EQ_INT(scan_engine_phase(),SCAN_PHASE_SCANNING);
}
LS_CASE(lr_seek_wraps_and_finishes_on_first_open_carrier)
{
    seek_lr=true; run_seek(1,150012500,150000000); seek_lr=false;
    LS_EQ_INT(tune_count,2);
    LS_EQ_UINT(tuned[0],150025000); LS_EQ_UINT(tuned[1],150000000);
    LS_CHECK(!scan_engine_active()); LS_CHECK(seek_locked); nfm_reset();
}
LS_CASE(lr_seek_quiet_lap_wraps_once_in_both_directions)
{
    seek_lr=true;
    run_seek(1,150012500,0);
    LS_EQ_INT(tune_count,3); LS_EQ_UINT(tuned[2],150012500);
    run_seek(-1,150000000,0);
    LS_EQ_INT(tune_count,3); LS_EQ_UINT(tuned[0],150025000);
    LS_EQ_UINT(tuned[2],150000000);
    LS_CHECK(!scan_engine_active()); LS_CHECK(seek_locked);
    seek_lr=false; nfm_reset();
}

LS_CASE(lr_seek_skips_below_receive_range_and_wraps_to_open_carrier)
{
    seek_lr=true;
    run_seek_range(-1,104000000,104012500,103987500,104012500);
    seek_lr=false;
    LS_EQ_INT(tune_count,1); LS_EQ_UINT(tuned[0],104012500);
    LS_CHECK(!scan_engine_active()); LS_CHECK(seek_locked); nfm_reset();
}

LS_CASE(seek_manual_hold_waits_for_next_and_then_completes)
{
    for (int lr = 0; lr < 2; lr++) {
        seek_lr = lr != 0;
        seek_interrupt = 31; run_seek(1, 150000000, 0);
        LS_EQ_INT(phase, 2); LS_EQ_INT(tune_count, 3);
        LS_CHECK(!scan_engine_active()); nfm_reset();
    }
    seek_lr = false;
}
LS_CASE(seek_next_and_skip_override_a_carrier_during_qualification)
{
    for (int lr = 0; lr < 2; lr++) for (int action = 32; action <= 33; action++) {
        seek_lr = lr != 0;
        seek_interrupt = action; run_seek(1, 150000000, 150012500);
        LS_EQ_INT(tune_count, 3);
        LS_EQ_UINT(FM.freq_hz, 150000000);
        LS_CHECK(!scan_engine_active()); nfm_reset();
    }
    seek_lr = false;
}
LS_CASE(band_skip_locks_out_the_position_for_the_session_but_next_does_not)
{
    nfm_reset(); run(35, 1500);
    LS_CHECK(tune_count > 3); LS_CHECK(sess_skipped(0));
    for (int i = 1; i < tune_count && i < 32; i++) LS_CHECK(tuned[i] != 150000000);
    run(36, 1500);
    LS_CHECK(!sess_skipped(0));
    LS_EQ_UINT(tuned[3], 150000000);
    nfm_reset();
}
LS_CASE(band_session_bitmap_covers_the_entire_existing_band_limit)
{
    sess_skip(SCAN_MAX_BAND_STEPS - 1);
    LS_CHECK(sess_skipped(SCAN_MAX_BAND_STEPS - 1));
    scan_engine_start();
    LS_CHECK(!sess_skipped(SCAN_MAX_BAND_STEPS - 1));
    scan_engine_stop();
}

LS_CASE(old_seek_completion_preserves_a_new_seek_and_its_direction)
{
    seek_interrupt = 37;
    run_seek(1, 150000000, ANY_HZ);
    LS_EQ_INT(completion_foreground_checks, 3);
    LS_EQ_INT(phase, 2);
    LS_CHECK(scan_engine_active());
    LS_EQ_INT(scan_engine_seek_direction(), -1);
    LS_CHECK(!seek_locked);
    scan_engine_stop(); nfm_reset();
}

/* Exercise the real P25 seek worker with the same tune acknowledgement fake
   as channel scanning. A strong unsynchronized carrier must be passed. */
static void run_p25_seek(int direction, bool sync)
{
    nfm_reset(); run(0, 1); scan_engine_stop();
    foreground.name = "P25"; s_task_ready = true;
    tune_count = phase = scenario = 0;
    P25.dsd_has_sync = sync;
    p25_rx_power = sync ? 0.0f : 1.0f;
    s_pass.measure_us = 0;
    LS_CHECK(scan_engine_seek(150000000, 12500, direction, 150000000, 150025000));
    LS_CHECK(scan_engine_active());
    LS_EQ_INT(selected_mode(), SCAN_MODE_P25);
    ls_shim_time_set(0); deadline = 1500000;
    if (!setjmp(done)) scan_task(NULL);
    LS_CHECK(!scan_engine_active());
    LS_EQ_INT(scan_engine_seek_direction(), 0);
    LS_EQ_INT(s_fg_mode, SCAN_MODE_P25);
}
LS_CASE(p25_seek_stops_on_sync_in_both_directions_even_below_power_threshold)
{
    for (int direction = -1; direction <= 1; direction += 2) {
        run_p25_seek(direction, true);
        LS_EQ_INT(tune_count, 1);
        LS_EQ_UINT(tuned[0], direction < 0 ? 150025000 : 150012500);
        LS_CHECK(s_pass.measure_us >= (SETTLE_MS + MEASURE_MS) * 1000);
    }
    scan_engine_stop(); foreground.name = "FM"; nfm_reset();
}
LS_CASE(p25_seek_passes_a_strong_carrier_without_sync_and_completes_one_lap)
{
    run_p25_seek(1, false);
    LS_EQ_INT(tune_count, 3);
    LS_EQ_UINT(tuned[0], 150012500); LS_EQ_UINT(tuned[2], 150000000);
    scan_engine_stop(); foreground.name = "FM"; nfm_reset();
}
