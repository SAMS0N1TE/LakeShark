/* LS_TEST_SOURCES: ${APP}/adsb/adsb_source.c ${FW}/components/lakeshark/radio/iq_app_control.c */
#include "ls_test.h"
#include "../../components/lakeshark/apps/adsb/app_adsb.c"
#include <string.h>

static int s_reads, s_releases, s_opens, s_gains, s_chip_starts, s_resets;
static int s_saved_gain, s_gain_writes;
static bool s_choose_chip, s_deadline_test, s_dual_test;
static int s_chip_ends;
static uint32_t s_caps = LS_LORA_CAP_MODES_RX;
static bool s_uat_present = true;
static bool s_dual_io_error;
static ls_radio_iq_config_t s_config;
static ls_radio_requirements_t s_requirements;
static ls_radio_err_t s_open_error;

bool ls_radio_endpoint_available(const ls_radio_requirements_t *r) { return r->sample_rate_hz == UAT_SAMPLE_RATE ? s_uat_present : true; }
ls_rsel_radio_t ls_rsel_saved(ls_rsel_job_t j) { (void)j; return s_dual_test ? LS_RSEL_ADSB_DUAL : s_choose_chip ? LS_RSEL_LORA : LS_RSEL_SDR_RTL; }
const char *ls_rsel_sdr_endpoint(ls_rsel_job_t j) { (void)j; return "test"; }
int ls_rsel_gain_max(ls_rsel_job_t j) { (void)j; return 1130; }
uint32_t ls_lora_caps(void) { return s_caps; }
ls_radio_err_t ls_radio_acquire(const char *owner, const ls_radio_requirements_t *r,
                               ls_radio_session_t **out)
{
    LS_EQ_STR(owner, "adsb");
    s_requirements = *r;
    s_opens++;
    if (s_deadline_test && s_opens == 2) s_rx_should_run = false;
    if (s_open_error != LS_RADIO_OK) return s_open_error;
    *out = (ls_radio_session_t *)&s_opens;
    return LS_RADIO_OK;
}
void ls_radio_release(ls_radio_session_t *s) { (void)s; s_releases++; }
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,
    const ls_radio_iq_config_t *r, ls_radio_iq_config_t *a) { (void)s; s_config = *r; *a = *r; return LS_RADIO_OK; }
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *s) { (void)s; return LS_RADIO_OK; }
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *s) { (void)s; return LS_RADIO_OK; }
ls_radio_err_t ls_radio_iq_set_gain(ls_radio_session_t *s, ls_radio_gain_mode_t m,
                                   int gain, int *actual)
{
    (void)s; (void)m; s_gains++; *actual = gain; return LS_RADIO_OK;
}
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s, void *dst, size_t n,
                               uint32_t timeout, size_t *got)
{
    (void)s; (void)dst; (void)n;
    s_reads++; *got = 0;
    if (s_dual_test) {
        LS_EQ_UINT(timeout, 1);
        if (s_dual_io_error) {
            s_rx_should_run = false;
            return LS_RADIO_ERR_DISCONNECTED;
        }
        memset(dst, 128, n);
        *got = n;
        if (s_reads >= 3) s_rx_should_run = false;
        return LS_RADIO_OK;
    }
    ls_shim_time_advance(timeout * 1000);
    if (s_reads == 3) adsb_request_gain(800);
    if (!s_deadline_test && s_reads == 5) s_choose_chip = true;
    if (s_reads > 150) s_rx_should_run = false;
    return LS_RADIO_ERR_TIMEOUT;
}
ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *s, uint64_t hz, bool fast, uint64_t *actual)
{
    (void)s; (void)fast; *actual = hz; return LS_RADIO_OK;
}
const char *ls_radio_err_name(ls_radio_err_t e) { (void)e; return "test"; }
esp_err_t ls_lora_modes_begin(uint32_t hz, int step) { (void)hz; (void)step; s_chip_starts++; return ESP_OK; }
int ls_lora_modes_poll(uint8_t *buf, size_t n, float *rssi)
{
    (void)buf; (void)n; (void)rssi; if (!s_dual_test) s_rx_should_run = false; return 0;
}
esp_err_t ls_lora_modes_set_gain(int step) { (void)step; return ESP_OK; }
esp_err_t ls_lora_modes_end(void) { s_chip_ends++; return ESP_OK; }
void adsb_decode_reset_stream(void) { s_resets++; }
void adsb_decode_init(void) { }
void adsb_decode_on_uat(const uat_fix_t *f) { (void)f; }
void app_switch_to(int index) { (void)index; }
int app_current_index(void) { return 0; }
void app_park(void) { }
void adsb_on_sample(uint8_t *iq, int len) { (void)iq; (void)len; }
void adsb_periodic_age(int64_t now) { (void)now; }
void adsb_demo_tick(void) { }
void adsb_inject_fake_aircraft(void) { }
void perf_count_bytes(uint32_t n) { (void)n; }
void perf_count_burst(void) { }
adsb_frame_result_t adsb_on_chips_ex(const uint8_t *b, int n, int v, int r, int *bad)
{
    (void)b; (void)n; (void)v; (void)r; (void)bad; return ADSB_FRAME_OK;
}
bool ls_radio_decode_worker_start(TaskFunction_t fn, const char *n, UBaseType_t p, BaseType_t c)
{
    (void)fn; (void)n; (void)p; (void)c; return false;
}
bool ls_radio_decode_worker_release(unsigned n, uint32_t ms) { (void)n; (void)ms; return true; }
int app_register(const app_t *a) { (void)a; return 0; }
uint32_t settings_get_freq(const app_t *a) { return a->default_freq; }
void settings_set_freq(const app_t *a, uint32_t hz) { (void)a; (void)hz; }
int settings_get_gain(const app_t *a) { (void)a; return s_saved_gain; }
void settings_set_gain(const app_t *a, int g) { (void)a; s_saved_gain = g; s_gain_writes++; }
int settings_get_adsb_follow(void) { return 0; }
void settings_set_adsb_follow(int m) { (void)m; }

static void fresh_rx(bool deadline)
{
    ls_shim_time_set(1000000);
    ls_iq_control_reset(&s_radio_control);
    s_session = NULL;
    s_dual_chip = false;
    s_console_uat = false;
    s_dual_test = s_dual_io_error = false;
    s_chip_ends = 0;
    s_caps = LS_LORA_CAP_MODES_RX;
    s_uat_present = true;
    s_source = ADSB_SRC_NONE;
    s_rx_should_run = true;
    s_choose_chip = false;
    s_deadline_test = deadline;
    s_open_error = LS_RADIO_OK;
    s_reads = s_releases = s_opens = s_gains = s_chip_starts = s_resets = 0;
}

LS_CASE(repeated_iq_timeouts_apply_gain_and_switch_to_chip)
{
    fresh_rx(false);
    adsb_rx_task(NULL);
    LS_CHECK(s_reads >= 5 && s_reads < 40);
    LS_EQ_INT(1, s_gains);
    LS_EQ_INT(1, s_releases);
    LS_EQ_INT(1, s_chip_starts);
    LS_CHECK(s_resets >= 3);
    LS_EQ_INT(ADSB_SRC_NONE, adsb_active_source());
}

LS_CASE(stalled_iq_stream_releases_and_reopens_its_lease)
{
    fresh_rx(true);
    adsb_rx_task(NULL);
    LS_EQ_INT(2, s_opens);
    LS_EQ_INT(2, s_releases);
    LS_CHECK(s_reads >= 100 && s_reads < 150);
    LS_EQ_INT(1, s_gains);
    LS_CHECK(s_session == NULL);
}

LS_CASE(boot_preserves_saved_auto_and_hackrf_gain_without_writing_defaults)
{
    s_gain_writes = 0;
    s_saved_gain = 0;
    adsb_cache_settings(&ADSB_APP);
    LS_EQ_INT(0, adsb_requested_gain());
    s_saved_gain = 1130;
    adsb_cache_settings(&ADSB_APP);
    LS_EQ_INT(1130, adsb_requested_gain());
    LS_EQ_INT(0, s_gain_writes);
}

LS_CASE(busy_and_failed_open_report_recovery_status)
{
    fresh_rx(false);
    s_open_error = LS_RADIO_ERR_BUSY;
    LS_CHECK(!adsb_radio_open(ADSB_SRC_IQ));
    LS_CHECK(strstr(adsb_receiver_status(), "busy"));
    s_open_error = LS_RADIO_ERR_IO;
    LS_CHECK(!adsb_radio_open(ADSB_SRC_IQ));
    LS_CHECK(strstr(adsb_receiver_status(), "failed"));
    s_rx_should_run = false;
}

LS_CASE(dual_session_configures_978_rtl_and_releases_both_on_stop_and_reentry)
{
    for (int run = 0; run < 2; run++) {
        fresh_rx(false);
        s_dual_test = true;
        adsb_rx_task(NULL);
        LS_EQ_UINT(s_config.center_hz, 978000000);
        LS_EQ_UINT(s_config.sample_rate_hz, UAT_SAMPLE_RATE);
        LS_EQ_STR(s_requirements.preferred_endpoint_id, LS_RADIO_ENDPOINT_RTL_USB);
        LS_EQ_UINT(s_requirements.min_hz, 978000000);
        LS_EQ_INT(s_chip_starts, 1);
        LS_EQ_INT(s_chip_ends, 1);
        LS_EQ_INT(s_releases, 1);
        LS_CHECK(!s_dual_chip && !s_session && !s_rx_running);
        uat_stats_t stats;
        adsb_uat_stats(&stats);
        LS_EQ_UINT(stats.samples, 3 * ADSB_IQ_READ_BYTES / 2);
    }
}

LS_CASE(dual_disconnect_and_missing_radio_fallback)
{
    fresh_rx(false);
    s_dual_test = s_dual_io_error = true;
    adsb_rx_task(NULL);
    LS_EQ_INT(s_chip_starts, 1);
    LS_EQ_INT(s_chip_ends, 1);
    LS_EQ_INT(s_releases, 1);
    fresh_rx(false);
    s_dual_test = true;
    s_caps = LS_LORA_CAP_FSK; /* SX1262 has no Mode S */
    LS_EQ_INT(adsb_choose(), ADSB_SRC_IQ);
    s_caps = LS_LORA_CAP_MODES_RX;
    s_uat_present = false;
    LS_EQ_INT(adsb_choose(), ADSB_SRC_LORA);
    s_dual_test = false;
    s_console_uat = true;
    LS_EQ_INT(adsb_choose(), ADSB_SRC_UAT);
    s_console_uat = false;
    s_rx_should_run = false;
}
