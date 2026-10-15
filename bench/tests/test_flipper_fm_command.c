/* LS_TEST_SOURCES: ${FW}/main/flipper_link.c ${APP}/fm/fm_mode_label.c */
#include "ls_test.h"
#include "esp_timer.h"

#include "audio_eq.h"
#include "audio_out.h"
#include "ble_link.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "flipper_link.h"
#include "flipper_link_telemetry.h"
#include "fm_mode_label.h"
#include "lakeshark_backend.h"
#include "ls_version.h"
#include "radio_health.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "tone.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static int s_fm_mode = FM_MODE_LISTEN;
static int s_set_count;
static int s_select_count;
static const char *s_host_mode = "P25";
static char s_trace[8];
static size_t s_trace_len;
static const char *s_receiver_status = "app=? park=? rx=?";
static int s_receiver_queries;
static int s_freq_sets;
static int s_p25_mode;
static int s_p25_sets;
static int s_p25_cycles;
static ls_telemetry_common_t s_seen;
static int s_fm_sub_seen;
static int s_fm_tel_submode;

static void trace(char c)
{
    if (s_trace_len + 1 < sizeof(s_trace)) {
        s_trace[s_trace_len++] = c;
        s_trace[s_trace_len] = '\0';
    }
}

static void select_mode(const char *name)
{
    LS_EQ_STR("fm", name);
    ++s_select_count;
    trace('S');
    /* Deliberately leave current_mode_name() at P25. The LCD callback only
       queues its GUI transition; the FM request must still be latched now. */
}

static const char *current_mode(void) { return s_host_mode; }

static const flipper_link_host_t TEST_HOST = {
    .select_mode_by_name = select_mode,
    .current_mode_name = current_mode,
};

static void start_link(void)
{
    flipper_link_cfg_t cfg = FLIPPER_LINK_CFG_DEFAULT();
    LS_EQ_INT(ESP_OK, flipper_link_start(&cfg, &TEST_HOST));
}

static void reset_observations(void)
{
    s_set_count = 0;
    s_select_count = 0;
    s_trace_len = 0;
    s_trace[0] = '\0';
    s_receiver_queries = 0;
    s_freq_sets = 0;
}

LS_CASE(bluetooth_commands_select_receiver_without_uart_start)
{
    char reply[64];
    flipper_link_stop();
    flipper_link_set_host(&TEST_HOST);
    s_host_mode = "P25";
    reset_observations();

    flipper_link_inject("FM POCSAG", reply, sizeof(reply));

    LS_EQ_STR("+OK fm=pocsag\n", reply);
    LS_EQ_INT(FM_MODE_POCSAG, s_fm_mode);
    LS_EQ_INT(1, s_select_count);
    LS_EQ_INT(1, s_set_count);
    LS_CHECK(!flipper_link_running());
}

LS_CASE(actual_demod_command_restores_auto_without_changing_numeric_modes)
{
    start_link();
    char reply[64];
    s_p25_sets = s_p25_cycles = 0;
    for (int mode = -1; mode <= 3; mode++) {
        char line[24], expected[64];
        snprintf(line, sizeof(line), "DEMOD %d", mode);
        snprintf(expected, sizeof(expected), "+OK dm=%d auto\n", mode);
        flipper_link_inject(line, reply, sizeof(reply));
        LS_EQ_STR(reply, expected);
        LS_EQ_INT(s_p25_mode, mode);
    }
    flipper_link_inject("DEMOD auto", reply, sizeof(reply));
    LS_EQ_INT(s_p25_mode, -1);
    LS_EQ_INT(s_p25_sets, 6);
    flipper_link_inject("DEMOD CYCLE", reply, sizeof(reply));
    LS_EQ_INT(s_p25_cycles, 1);
    const char *invalid[] = {"DEMOD -2", "DEMOD 4", "DEMOD AUTOMATIC", "DEMOD 0junk"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        flipper_link_inject(invalid[i], reply, sizeof(reply));
        LS_EQ_STR(reply, "-ERR demod\n");
    }
    LS_EQ_INT(s_p25_sets, 6);
    flipper_link_stop();
}

LS_CASE(actual_fl_fm_path_selects_every_mode_and_reports_it)
{
    static const char *const commands[] = {
        "listen", "scan", "pocsag", "wfm", "acars", "flex", NULL, "am", NULL
    };
    start_link();

    for (int mode = 0; mode < FM_MODE_COUNT; ++mode) {
        if (mode == 6 || mode == FM_MODE_SAME || mode == FM_MODE_APRS || mode == FM_MODE_AIS) continue;
        char line[32];
        char expected[32];
        char reply[64];
        snprintf(line, sizeof(line), "FM %s", commands[mode]);
        snprintf(expected, sizeof(expected), "+OK fm=%s\n", commands[mode]);
        reset_observations();
        flipper_link_inject(line, reply, sizeof(reply));
        LS_EQ_STR(expected, reply);
        LS_EQ_INT(mode, s_fm_mode);
        LS_EQ_INT(1, s_set_count);
        LS_EQ_INT(1, s_select_count);
    }

    flipper_link_stop();
}

LS_CASE(actual_fl_fm_acars_survives_asynchronous_gui_selection)
{
    char reply[64];
    start_link();
    reset_observations();
    s_host_mode = "P25";

    flipper_link_inject("FM ACARS", reply, sizeof(reply));

    LS_EQ_STR("+OK fm=acars\n", reply);
    LS_EQ_INT(FM_MODE_ACARS, s_fm_mode);
    LS_EQ_INT(1, s_select_count);
    LS_EQ_INT(1, s_set_count);
    LS_EQ_STR("SM", s_trace);
    flipper_link_stop();
}

LS_CASE(actual_fl_fm_queries_alias_numeric_and_invalid_inputs_are_bounded)
{
    char reply[64];

    start_link();
    s_host_mode = "FM";
    for (int mode = 0; mode < FM_MODE_COUNT; ++mode) {
        if (mode == 6 || mode == FM_MODE_SAME || mode == FM_MODE_APRS || mode == FM_MODE_AIS) continue;
        char line[16];
        char expected[32];
        snprintf(line, sizeof(line), "FM %d", mode);
        snprintf(expected, sizeof(expected), "+OK fm=%s\n",
                 fm_mode_command_name((fm_mode_t)mode));
        reset_observations();
        flipper_link_inject(line, reply, sizeof(reply));
        LS_EQ_STR(expected, reply);
        LS_EQ_INT(mode, s_fm_mode);
        LS_EQ_INT(1, s_set_count);
        LS_EQ_INT(0, s_select_count);

        flipper_link_inject("FM", reply, sizeof(reply));
        LS_EQ_STR(expected, reply);
    }

    reset_observations();
    flipper_link_inject("FM nbfm", reply, sizeof(reply));
    LS_EQ_STR("+OK fm=listen\n", reply);
    LS_EQ_INT(FM_MODE_LISTEN, s_fm_mode);
    LS_EQ_INT(1, s_set_count);

    static const char *const invalid[] = {
        "FM bogus", "FM -1", "FM 6", "FM TRAP", "FM TargetTag",
        "FM 8", "FM same", "FM SAME", "FM 9", "FM 10", "FM ais", "FM AIS", "FM 999999999999999999999"
    };
    s_fm_mode = FM_MODE_FLEX;
    s_host_mode = "P25";
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset_observations();
        flipper_link_inject(invalid[i], reply, sizeof(reply));
        LS_EQ_STR("-ERR fm\n", reply);
        LS_EQ_INT(FM_MODE_FLEX, s_fm_mode);
        LS_EQ_INT(0, s_set_count);
        LS_EQ_INT(0, s_select_count);
    }

    s_fm_mode = FM_MODE_COUNT;
    flipper_link_inject("FM", reply, sizeof(reply));
    LS_EQ_STR("+OK fm=unknown\n", reply);
    flipper_link_stop();
}

LS_CASE(receiver_queries_return_the_read_only_snapshot)
{
    char reply[384];
    start_link();
    reset_observations();

    static const char *const states[] = {
        "app=P25 park=0 rx=disconnected freq_req=851012500 freq_eff=?",
        "app=P25 park=1 rx=parked freq_req=851012500 freq_eff=851012500",
        "app=P25 park=0 rx=active freq_req=851012500 freq_eff=851012500",
        "app=? park=? rx=? freq_req=? freq_eff=?",
    };
    static const char *const commands[] = { "STAT", "FREQ", "SDR", "STAT" };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        s_receiver_status = states[i];
        flipper_link_inject(commands[i], reply, sizeof(reply));
        char expected[384];
        snprintf(expected, sizeof(expected), "+OK %s\n", states[i]);
        LS_EQ_STR(expected, reply);
    }
    LS_EQ_INT(s_receiver_queries, 4);
    LS_EQ_INT(s_freq_sets, 0);
    flipper_link_stop();
}

/* The command parser is compiled intact above. These host fakes make every
   unrelated command and transport reference link without touching hardware. */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t copy = n < size - 1 ? n : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return n;
}

void lakeshark_fm_set_mode(int mode)
{
    trace('M');
    s_fm_mode = mode;
    ++s_set_count;
}
int lakeshark_fm_get_mode(void) { return s_fm_mode; }
bool lakeshark_fm_tune(int delta_hz) { (void)delta_hz; return true; }
bool lakeshark_fm_set_freq(uint32_t hz) { (void)hz; ++s_freq_sets; return true; }
uint32_t lakeshark_fm_get_freq(void) { return 0; }
void lakeshark_fm_set_baud(int baud) { (void)baud; }
int lakeshark_fm_get_baud(void) { return 1200; }
void lakeshark_fm_squelch_delta(int d) { (void)d; }
void lakeshark_fm_set_squelch(int v) { (void)v; }
int lakeshark_fm_squelch_get(void) { return 0; }
void lakeshark_fm_scan_restart(void) {}
void lakeshark_fm_tune_to_peak(void) {}
void lakeshark_fm_telemetry(lakeshark_fm_tel_t *out) { memset(out, 0, sizeof(*out)); out->submode = s_fm_tel_submode; }

void lakeshark_p25_tune(int delta_hz) { (void)delta_hz; }
void lakeshark_p25_set_freq(uint32_t hz) { (void)hz; }
uint32_t lakeshark_p25_get_freq(void) { return 0; }
const char *lakeshark_p25_cycle_mode(void) { s_p25_cycles++; return "auto"; }
const char *lakeshark_p25_mode_name(void) { return "auto"; }
int lakeshark_p25_mode_index(void) { return s_p25_mode; }
void lakeshark_p25_set_mode(int mode) { s_p25_mode = mode; s_p25_sets++; }
void lakeshark_p25_reset_stats(void) {}
void lakeshark_p25_gain_step(void) {}
void lakeshark_p25_agc(void) {}
int lakeshark_p25_gain_tenths(void) { return 0; }
void lakeshark_p25_beep_toggle(void) {}
bool lakeshark_p25_beep_enabled(void) { return false; }
void lakeshark_p25_set_voice_gate(int value) { (void)value; }
int lakeshark_p25_voice_gate(void) { return 0; }
void lakeshark_p25_toggle_polarity(void) {}
bool lakeshark_p25_polarity_inverted(void) { return false; }
void lakeshark_p25_telemetry(lakeshark_p25_tel_t *out) { memset(out, 0, sizeof(*out)); }
void lakeshark_radio_set_gain(int gain) { (void)gain; }
bool lakeshark_iq_receiver_ready(void) { return false; }
bool lakeshark_radio_endpoint_ready(const char *id) { (void)id; return false; }
int lakeshark_receiver_status(char *out, size_t len)
{
    ++s_receiver_queries;
    return snprintf(out, len, "%s", s_receiver_status);
}

void lakeshark_adsb_telemetry(lakeshark_adsb_tel_t *out) { memset(out, 0, sizeof(*out)); }

void rec_get_status(rec_status_t *out) { memset(out, 0, sizeof(*out)); }
bool rec_set_freq(uint32_t hz) { (void)hz; return true; }
/* REC SCAN's sweep, which these tests do not exercise. */
bool rec_watch_scan_busy(void) { return false; }
void rec_watch_scan_stop(void) { }
bool rec_watch_enabled(void) { return false; }
bool rec_watch_request_scan(uint32_t lo, uint32_t hi, uint32_t s, int b)
{ (void)lo; (void)hi; (void)s; (void)b; return false; }
int rec_watch_scan_live(rec_scan_bin_t *o, int m) { (void)o; (void)m; return 0; }
float rec_watch_scan_live_floor(void) { return 0; }
int rec_watch_scan_result(rec_scan_bin_t *o, int m) { (void)o; (void)m; return 0; }
int rec_watch_scan_events(void) { return 0; }
uint32_t rec_get_freq(void) { return 0; }
void rec_set_gain(int gain) { (void)gain; }
void rec_set_thresh(int threshold) { (void)threshold; }
void rec_set_gap_ms(int ms) { (void)ms; }
void rec_set_bw(uint32_t hz) { (void)hz; }
void rec_set_min_pulse(uint32_t us) { (void)us; }
void rec_set_max_span(uint32_t us) { (void)us; }
void rec_set_min_edges(int count) { (void)count; }
void rec_arm_request(void) {}
void rec_disarm(void) {}
int rec_edges_copy(int from, int32_t *out, int max) { (void)from; (void)out; (void)max; return 0; }
int rec_save(const char *name, char *path, size_t len) { (void)name; (void)path; (void)len; return -1; }
int rec_files_snapshot(void) { return 0; }
static int s_rec_mod;
static bool s_rec_autosave = true;
void rec_set_mod(int mod) { s_rec_mod = mod; }
int rec_get_mod(void) { return s_rec_mod; }
void rec_set_autosave(bool on) { s_rec_autosave = on; }
bool rec_get_autosave(void) { return s_rec_autosave; }
int rec_file_info(int index, char *name, size_t len, uint32_t *freq, long *size)
{ (void)index; (void)name; (void)len; (void)freq; (void)size; return 0; }
int rec_load(int index) { (void)index; return -1; }
int rec_remove(const char *name) { (void)name; return -1; }

void audio_eq_get(audio_eq_cfg_t *out) { memset(out, 0, sizeof(*out)); }
void audio_eq_set(const audio_eq_cfg_t *in) { (void)in; }
bool audio_eq_apply_preset(int preset) { (void)preset; return true; }
const char *audio_eq_preset_name(int preset) { (void)preset; return "flat"; }
int audio_eq_preset_from_name(const char *name) { (void)name; return -1; }
int audio_eq_gr_db10(void) { return 0; }
bool audio_is_muted(void) { return false; }
void audio_out_ensure_unmuted(void) {}
void audio_toggle_mute(void) {}
void audio_volume_delta(int delta) { (void)delta; }
void audio_volume_set(int volume) { (void)volume; }
int audio_volume_get(void) { return 0; }

static int s_tts_vol = 100, s_tts_vol_saved = -1;
int  speech_volume_get(void) { return s_tts_vol; }
void speech_volume_set(int pct) { s_tts_vol = pct; }
void settings_speech_volume_set(int pct) { s_tts_vol_saved = pct; }

LS_CASE(the_head_sets_the_speech_volume_and_it_is_kept)
{
    char reply[64];
    flipper_link_set_host(&TEST_HOST);
    flipper_link_inject("TTSVOL 40", reply, sizeof(reply));
    LS_EQ_STR("+OK tv=40\n", reply);
    LS_EQ_INT(40, s_tts_vol_saved);
    flipper_link_inject("TTSVOL 250", reply, sizeof(reply));
    LS_EQ_STR("+OK tv=100\n", reply);
    flipper_link_inject("TTSVOL", reply, sizeof(reply));
    LS_EQ_STR("+OK tv=100\n", reply);
    flipper_link_inject("TTSVOL loud", reply, sizeof(reply));
    LS_EQ_STR("-ERR ttsvol\n", reply);
}

bool snd_test_start(int which) { (void)which; return true; }
int snd_test_from_name(const char *name) { (void)name; return -1; }
const char *snd_test_name(int which) { (void)which; return "test"; }
bool snd_test_busy(void) { return false; }

bool radio_health_get(const char *id, radio_health_snapshot_t *out)
{ (void)id; memset(out, 0, sizeof(*out)); return false; }
void radio_health_note_progress_reset(const char *id) { (void)id; }
const char *radio_health_state_name(rh_state_t state) { (void)state; return "absent"; }
ble_link_state_t ble_link_state(void) { return BLE_LINK_OFF; }

int ls_telemetry_build_fm(char *buf, size_t len, const lakeshark_fm_tel_t *t,
                          const ls_telemetry_common_t *common)
{ (void)buf; (void)len; (void)t; s_seen = *common; s_fm_sub_seen = t->submode; return 0; }
int ls_telemetry_build_adsb(char *buf, size_t len, const lakeshark_adsb_tel_t *t,
                            const ls_telemetry_common_t *common)
{ (void)buf; (void)len; (void)t; s_seen = *common; return 0; }
int ls_telemetry_build_p25(char *buf, size_t len, const lakeshark_p25_tel_t *t,
                           const char *mode, const char *demod,
                           const ls_telemetry_common_t *common)
{ (void)buf; (void)len; (void)t; (void)mode; (void)demod; s_seen = *common; return 0; }
int ls_telemetry_build_rec(char *buf, size_t len, const rec_status_t *status,
                           const ls_telemetry_common_t *common)
{ (void)buf; (void)len; (void)status; s_seen = *common; return 0; }
int ls_version_line(char *out, size_t len)
{ if (len) out[0] = '\0'; return 0; }

esp_log_level_t esp_log_level_get(const char *tag) { (void)tag; return ESP_LOG_NONE; }
void esp_log_level_set(const char *tag, esp_log_level_t level) { (void)tag; (void)level; }
/* Moved to the shared shim: it is the host standing in for a ROM
   routine, which is what bench/harness is for. */
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }

esp_err_t gpio_set_pull_mode(gpio_num_t gpio, gpio_pull_mode_t pull)
{ (void)gpio; (void)pull; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level) { (void)gpio; (void)level; return ESP_OK; }

#include "ls_mesh.h"
#include "ls_gps.h"
#include "ls_track_log.h"

static ls_mesh_stats_t  g_mesh;
static int              g_mesh_peers;
static int              g_sightings;
static ls_gps_state_t   g_gps;
static bool             g_gps_on;
static bool             g_track_on;
static int              g_track_points;
static bool             g_track_refuses;

void ls_mesh_get_stats(ls_mesh_stats_t *o) { if (o) *o = g_mesh; }
int  ls_mesh_peers(ls_mesh_peer_t *o, int m) { (void)o; (void)m; return g_mesh_peers; }
int  ls_mesh_sightings(void) { return g_sightings; }

void ls_gps_get(ls_gps_state_t *o) { if (o) *o = g_gps; }
bool ls_gps_running(void) { return g_gps_on; }

esp_err_t ls_track_rec_start(void)
{
    if (g_track_refuses) return ESP_FAIL;
    g_track_on = true;
    return ESP_OK;
}
void ls_track_rec_stop(void) { g_track_on = false; }
bool ls_track_rec_running(void) { return g_track_on; }
int  ls_track_points(void) { return g_track_points; }
int gpio_get_level(gpio_num_t gpio) { (void)gpio; return 0; }

bool uart_is_driver_installed(int uart_num) { (void)uart_num; return false; }
esp_err_t uart_driver_install(int uart_num, int rx_size, int tx_size,
                              int queue_size, void *queue, int flags)
{ (void)uart_num; (void)rx_size; (void)tx_size; (void)queue_size; (void)queue; (void)flags; return ESP_OK; }
esp_err_t uart_driver_delete(int uart_num) { (void)uart_num; return ESP_OK; }
esp_err_t uart_param_config(int uart_num, const uart_config_t *config)
{ (void)uart_num; (void)config; return ESP_OK; }
esp_err_t uart_set_pin(int uart_num, int tx, int rx, int rts, int cts)
{ (void)uart_num; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK; }
esp_err_t uart_flush_input(int uart_num) { (void)uart_num; return ESP_OK; }
esp_err_t uart_get_buffered_data_len(int uart_num, size_t *size)
{ (void)uart_num; *size = 0; return ESP_OK; }
int uart_read_bytes(int uart_num, void *buffer, uint32_t len, uint32_t wait)
{ (void)uart_num; (void)buffer; (void)len; (void)wait; return 0; }
int uart_write_bytes(int uart_num, const void *source, size_t size)
{ (void)uart_num; (void)source; return (int)size; }

BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       uint32_t stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *created_task)
{
    (void)task; (void)name; (void)stack_depth; (void)arg; (void)priority;
    if (created_task) *created_task = (TaskHandle_t)1;
    return pdPASS;
}

/* ------------------------------------------------ the new verbs -- */

/* A head asks and reads the answer. Nothing here checks a field position -
   the replies are key=value, and pinning an order would pin a layout the
   protocol does not promise. What is checked is that the ANSWER CHANGES with
   the state, because a verb that returns the same line whatever the board is
   doing is worse than no verb: it reads as working. */
static bool reply_has(const char *line, const char *needle)
{
    char reply[320];
    reply[0] = 0;
    flipper_link_inject(line, reply, sizeof(reply));
    return strstr(reply, needle) != NULL;
}

LS_CASE(the_head_can_ask_about_the_mesh)
{
    /* Twenty-nine verbs and every one about the RTL-SDR, the audio
       path or the board's health - written when that was all there was. The
       mesh radio has been listening from boot for a while now and a wired
       head could not see it at all. */
    memset(&g_mesh, 0, sizeof(g_mesh));
    g_mesh.running = false;
    LS_CHECK_MSG(reply_has("MESH", "run=0"),
                 "a stopped mesh does not report itself as stopped");

    g_mesh.running = true;
    g_mesh.tx_enabled = true;
    g_mesh.rx_packets = 42;
    g_mesh.rx_bad = 3;
    g_mesh.last_rssi = -91.0f;
    g_mesh_peers = 5;
    snprintf(g_mesh.self_id, sizeof(g_mesh.self_id), "A1B2C3D4E5F60718");

    LS_CHECK(reply_has("MESH", "run=1"));
    LS_CHECK(reply_has("MESH", "rx=42"));
    LS_CHECK(reply_has("MESH", "bad=3"));
    LS_CHECK(reply_has("MESH", "peers=5"));
    LS_CHECK(reply_has("MESH", "A1B2C3D4E5F60718"));
    /* The transmitter being armed is the one field a head has a duty to
       show: it is the difference between a receiver and a radio that is
       emitting. */
    LS_CHECK_MSG(reply_has("MESH", "tx=1"),
                 "an armed transmitter is not reported to the head");
}

LS_CASE(the_head_can_tell_a_gps_that_is_off_from_one_that_is_searching)
{
    /*'s distinction, which is the whole value of this verb: bytes
       climbing with sentences flat is a baud rate, both climbing with no fix
       is the antenna, and neither of those is "the GPS is broken". A head
       that only saw a fix flag could not tell any of them apart. */
    memset(&g_gps, 0, sizeof(g_gps));
    g_gps_on = false;
    LS_CHECK_MSG(reply_has("GPS", "on=0"),
                 "a powered-down receiver does not say so");

    g_gps_on = true;
    g_gps.alive = false;
    g_gps.bytes = 9000;
    g_gps.sentences = 0;
    LS_CHECK(reply_has("GPS", "on=1"));
    LS_CHECK(reply_has("GPS", "alive=0"));
    LS_CHECK_MSG(reply_has("GPS", "bytes=9000") && reply_has("GPS", "ok=0"),
                 "bytes arriving with no sentences parsed is the signature of "
                 "a wrong baud rate, and the head cannot see it");

    g_gps.alive = true;
    g_gps.sentences = 120;
    g_gps.fix = true;
    g_gps.last_fix_us = g_gps.last_sentence_us = esp_timer_get_time();
    g_gps.sats_used = 7;
    g_gps.lat_deg = 43.4445;
    g_gps.lon_deg = -71.6473;
    LS_CHECK(reply_has("GPS", "fix=1"));
    LS_CHECK(reply_has("GPS", "used=7"));
    LS_CHECK(reply_has("GPS", "43.444"));
}

LS_CASE(a_position_is_not_reported_without_a_fix)
{
    /*, and are all the same mistake in three places:
       a zero that is not a position being drawn, mapped or centred on as
       though it were. A head is the fourth place it could happen, and it is
       the one that would send the coordinates somewhere else. */
    memset(&g_gps, 0, sizeof(g_gps));
    g_gps_on = true;
    g_gps.alive = true;
    g_gps.fix = false;
    /* The receiver is still carrying the last position it computed. */
    g_gps.lat_deg = 43.4445;
    g_gps.lon_deg = -71.6473;

    LS_CHECK_MSG(!reply_has("GPS", "43.444"),
                 "a stale position was handed to the head with fix=0 - the "
                 "same zero-is-not-a-position mistake in a fourth place");
    LS_CHECK(reply_has("GPS", "fix=0"));
}

LS_CASE(the_head_can_start_the_track_and_see_whether_it_kept_anything)
{
    /*'s point, at the other end of a cable: a recorder that is on and
       has kept nothing looks exactly like one that is working, until the
       count is somewhere a person can see it. */
    g_track_on = false;
    g_track_refuses = false;
    g_track_points = 0;
    g_sightings = 0;

    LS_CHECK(reply_has("TRACK", "on=0"));
    LS_CHECK(reply_has("TRACK on", "+OK"));
    LS_CHECK_MSG(g_track_on, "TRACK on did not start the recorder");
    LS_CHECK(reply_has("TRACK", "on=1"));

    g_track_points = 314;
    g_sightings = 9;
    LS_CHECK(reply_has("TRACK", "points=314"));
    LS_CHECK(reply_has("TRACK", "nodes=9"));

    LS_CHECK(reply_has("TRACK off", "+OK"));
    LS_CHECK(!g_track_on);

    g_track_refuses = true;
    LS_CHECK_MSG(reply_has("TRACK on", "-ERR"),
                 "a recorder that would not start still answered +OK");
    g_track_refuses = false;
}

LS_CASE(the_head_sets_rec_modulation_and_autosave)
{
    /* LS-1241/1242: an FSK trap needs REC set to FSK before it records. */
    char reply[64];
    flipper_link_set_host(&TEST_HOST);
    flipper_link_inject("REC MOD FSK", reply, sizeof(reply));
    LS_EQ_STR("+OK mo=1\n", reply);
    LS_EQ_INT(REC_MOD_FSK, s_rec_mod);
    flipper_link_inject("rec mod ook", reply, sizeof(reply));
    LS_EQ_STR("+OK mo=0\n", reply);
    flipper_link_inject("REC MOD AM", reply, sizeof(reply));
    LS_EQ_STR("-ERR rec mod\n", reply);
    flipper_link_inject("REC AUTOSAVE OFF", reply, sizeof(reply));
    LS_EQ_STR("+OK autosave=0\n", reply);
    LS_CHECK(!s_rec_autosave);
    flipper_link_inject("REC AUTOSAVE", reply, sizeof(reply));
    LS_EQ_STR("+OK autosave=0\n", reply);
    flipper_link_inject("REC AUTOSAVE ON", reply, sizeof(reply));
    LS_EQ_STR("+OK autosave=1\n", reply);
}

LS_CASE(an_unknown_verb_is_still_refused_and_counted)
{
    /* The three new branches sit immediately before the fallthrough, so a
       missing else would swallow everything after them. */
    uint32_t rx = 0, tx = 0, bad0 = 0, bad1 = 0;
    flipper_link_stats(&rx, &tx, &bad0);
    LS_CHECK(reply_has("NOTAVERB", "-ERR"));
    flipper_link_stats(&rx, &tx, &bad1);
    LS_CHECK_MSG(bad1 > bad0, "an unknown verb was not counted as bad");
}

/* --------------------------------------------- P25 system for the head */

#include "p25_program.h"
#include "p25_state.h"

p25_state_t P25;
static bool s_p2_follow;
static const char *s_active_profile;
static p25_program_t s_fake_program;
static const char *s_card[] = { "p25_profile.txt", "p25_profile_seabrook.txt" };
static const char *s_card_system[] = { "Home County", "Seabrook Station" };
static char s_load_requested[64];

bool p25_get_phase2_follow(void) { return s_p2_follow; }
void p25_set_phase2_follow(bool on) { s_p2_follow = on; }
const char *p25_program_active_name(void) { return s_active_profile; }
const p25_program_t *p25_program_session(void) { return &s_fake_program; }
void p25_program_geo_code_now(char *out, size_t cap) { snprintf(out, cap, "off"); }

bool p25_program_is_profile_name(const char *name)
{
    const size_t n = name ? strlen(name) : 0;
    return n >= 15 && !strncmp(name, "p25_profile", 11) &&
           !strcmp(name + n - 4, ".txt") && !strchr(name, '/');
}

int p25_program_card_profile(int index, char *name, size_t name_cap,
                             char *system, size_t system_cap)
{
    const int total = (int)(sizeof(s_card) / sizeof(s_card[0]));
    if (index < 0 || index >= total) return total;
    if (name) snprintf(name, name_cap, "%s", s_card[index]);
    if (system) snprintf(system, system_cap, "%s", s_card_system[index]);
    return total;
}

bool p25_program_request_card_profile(const char *name)
{
    snprintf(s_load_requested, sizeof(s_load_requested), "%s", name);
    return true;
}

LS_CASE(the_head_sees_the_p25_system_state)
{
    s_p2_follow = true;
    P25.p25_phase2_grant_count = 3;
    P25.sync_unconfirmed_total = 4;
    P25.sync_unconfirmed_nac = 0x2E7;
    s_active_profile = "p25_profile_seabrook.txt";
    memset(&s_fake_program, 0, sizeof(s_fake_program));
    s_fake_program.active_valid = true;
    snprintf(s_fake_program.active.system_name,
             sizeof(s_fake_program.active.system_name), "Seabrook Station");

    LS_CHECK(reply_has("PSYS", "+OK psys "));
    LS_CHECK(reply_has("PSYS", "p2f=1"));
    LS_CHECK(reply_has("PSYS", "p2g=3"));
    LS_CHECK(reply_has("PSYS", "pf=p25_profile_seabrook.txt"));
    /* Free text crosses the wire with its spaces turned to underscores. */
    LS_CHECK(reply_has("PSYS", "sys=Seabrook_Station"));
    LS_CHECK(reply_has("PSYS", "geo=off"));
    LS_CHECK(reply_has("PSYS", "nu=4"));
    LS_CHECK(reply_has("PSYS", "unac=2E7"));

    s_active_profile = NULL;
    s_fake_program.active_valid = false;
    LS_CHECK(reply_has("PSYS", "pf=- sys=-"));
}

LS_CASE(the_head_switches_phase2_follow)
{
    s_p2_follow = false;
    LS_CHECK(reply_has("P2 FOLLOW on", "p2f=1"));
    LS_CHECK(s_p2_follow);
    LS_CHECK(reply_has("P2 follow OFF", "p2f=0"));
    LS_CHECK(!s_p2_follow);
    LS_CHECK(reply_has("P2 FOLLOW maybe", "-ERR"));
    LS_CHECK(reply_has("P2", "-ERR"));
}

LS_CASE(the_head_lists_and_loads_card_profiles)
{
    LS_CHECK(reply_has("PROF 0", "%P 0 2 p25_profile.txt Home_County"));
    LS_CHECK(reply_has("PROF 1", "%P 1 2 p25_profile_seabrook.txt Seabrook_Station"));
    /* Past the end is an empty row, which is how the head knows to stop. */
    LS_CHECK(reply_has("PROF 2", "%P 2 2 - -"));
    LS_CHECK(reply_has("PROF x", "-ERR"));

    s_load_requested[0] = '\0';
    LS_CHECK(reply_has("PROF LOAD p25_profile_seabrook.txt",
                       "+OK prof loading p25_profile_seabrook.txt"));
    LS_EQ_STR(s_load_requested, "p25_profile_seabrook.txt");
    /* Only a profile name in the card root, never a path. */
    s_load_requested[0] = '\0';
    LS_CHECK(reply_has("PROF LOAD ../p25_profile_x.txt", "-ERR"));
    LS_CHECK(reply_has("PROF LOAD notes.txt", "-ERR"));
    LS_EQ_STR(s_load_requested, "");
}

LS_CASE(aprs_is_refused_without_changing_the_receiver)
{
    s_fm_mode = FM_MODE_LISTEN;
    LS_CHECK(reply_has("FM APRS", "-ERR"));
    LS_CHECK(reply_has("FM 9", "-ERR"));
    LS_EQ_INT(FM_MODE_LISTEN, s_fm_mode);
}

LS_CASE(ais_is_refused_without_selecting_or_changing_the_receiver)
{
    start_link(); s_fm_mode = FM_MODE_LISTEN; s_host_mode = "P25";
    static const char *const commands[] = {"FM ais", "FM AIS", "FM 10"};
    for (unsigned i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        char reply[64]; reset_observations();
        flipper_link_inject(commands[i], reply, sizeof(reply));
        LS_EQ_STR("-ERR fm\n", reply); LS_EQ_INT(FM_MODE_LISTEN, s_fm_mode);
        LS_EQ_INT(0, s_select_count); LS_EQ_INT(0, s_set_count);
    }
    flipper_link_stop();
}

/* Status providers are bounded fakes; exercise the actual command router/cache. */
#include "ls_wifi.h"
#include "tui/ls_sweep_app.h"
#include "tui/ls_rid.h"
#include "tui/ls_field.h"
#include "esp_timer.h"
static unsigned aux_reads, mute_saves;
static bool aux_save_fail;
static ls_sweep_settings_t aux_settings={.version=3,.alerts_muted=true};
void ls_wifi_link_status(ls_wifi_link_status_t *out) {
    aux_reads++;memset(out,0,sizeof(*out));out->saved=8;out->connected=true;
    strcpy(out->ssid,"Lake House");strcpy(out->ip,"10.0.0.2");
}
void ls_sweep_summary(ls_sweep_summary_t *out,int64_t now) {
    memset(out,0,sizeof(*out));out->running=true;out->counts[0]=2;
    out->serial[0]=42;out->rssi[0]=-45;
}
void ls_field_sample_snapshot(ls_field_sample_t *out) {memset(out,0,sizeof(*out));}
void ls_rid_summary(ls_rid_summary_t *out,bool gps,double lat,double lon,int64_t now) {
    memset(out,0,sizeof(*out));out->count=3;out->nearest_m=-1;
}
void ls_sweep_settings_get(ls_sweep_settings_t *out) {*out=aux_settings;}
bool ls_sweep_settings_set(const ls_sweep_settings_t *in) {
    mute_saves++;aux_settings=*in;return !aux_save_fail;
}
LS_CASE(aux_is_opt_in_rate_limited_and_mute_ack_is_immediate) {
    ls_shim_time_set(100000000);unsigned reads=aux_reads,saves=mute_saves;
    LS_CHECK(reply_has("AUX","& av=1 wc=1 wn=8 ws=Lake_House wi=10.0.0.2 sr=1 sm=1"));
    LS_CHECK(reply_has("AUX","c0=C42:-45 c1=- dn=3 dr=-1 di=-"));
    LS_EQ_UINT(aux_reads,reads+1);
    LS_CHECK(reply_has("SWEEP MUTE 0","sm=0"));LS_EQ_UINT(mute_saves,saves+1);
    LS_CHECK(reply_has("SWEEP MUTE 0","sm=0"));LS_EQ_UINT(mute_saves,saves+1);
    LS_EQ_UINT(aux_reads,reads+1);
    ls_shim_time_advance(500000);LS_CHECK(reply_has("AUX","av=1"));LS_EQ_UINT(aux_reads,reads+2);
    LS_CHECK(reply_has("SWEEP MUTE 2","-ERR"));LS_CHECK(reply_has("SWEEP MUTE 1 extra","-ERR"));
    LS_CHECK(reply_has("SWEEP","-ERR"));LS_CHECK(reply_has("AUX 2","-ERR"));
    aux_save_fail=true;LS_CHECK(reply_has("SWEEP MUTE 1","-ERR sweep settings"));aux_save_fail=false;
}

LS_CASE(gps_reply_rejects_stale_and_stopped_fixes)
{
    int64_t old=esp_timer_get_time();ls_shim_time_set(10000000);
    memset(&g_gps,0,sizeof(g_gps));g_gps_on=true;
    g_gps.fix=g_gps.alive=true;g_gps.lat_deg=43.4445;g_gps.lon_deg=-71.6473;
    g_gps.last_fix_us=1000000;g_gps.last_sentence_us=10000000;
    LS_CHECK(reply_has("GPS","fix=0"));LS_CHECK(!reply_has("GPS","43.444"));
    g_gps.last_fix_us=10000000;g_gps_on=false;
    LS_CHECK(reply_has("GPS","fix=0"));LS_CHECK(reply_has("GPS","alive=0"));
    ls_shim_time_set(old);
}

/* ------------------------------------- receiver identity, MESH and DET */

#include "tui/ls_radio_select.h"
#include "tui/ls_detector.h"
#include "tui/ls_detector_air.h"

/* What the RADIO picker answers, as a fixture. */
static ls_rsel_radio_t s_rx_radio = LS_RSEL_SDR_RTL;
static const char *s_rx_cant, *s_rx_absent;
static ls_rsel_job_t s_rx_job_seen;
ls_rsel_radio_t ls_rsel_effective(ls_rsel_job_t job) { s_rx_job_seen = job; return s_rx_radio; }
const char *ls_rsel_name(ls_rsel_radio_t r)
{
    static const char *const n[] = { "RTL-SDR", "HackRF", "LR2021" };
    return r < 3 ? n[r] : "none";
}
const char *ls_rsel_cant(ls_rsel_job_t job, ls_rsel_radio_t r) { (void)job; (void)r; return s_rx_cant; }
const char *ls_rsel_absent(ls_rsel_radio_t r) { (void)r; return s_rx_absent; }

static void ask_telemetry(const char *mode)
{
    char buf[600];
    s_host_mode = mode;
    flipper_link_set_host(&TEST_HOST);
    flipper_link_snapshot(buf, sizeof(buf));
}

LS_CASE(telemetry_names_the_receiver_the_mode_really_uses)
{
    s_rx_cant = s_rx_absent = NULL;
    s_rx_radio = LS_RSEL_LORA;
    ask_telemetry("P25");
    LS_EQ_INT(LS_RSEL_P25, s_rx_job_seen);
    LS_EQ_STR("LR2021", s_seen.rx_name);
    LS_CHECK_MSG(s_seen.rx_why == NULL,
                 "a P25 mode on the LR2021 was given a reason to complain");

    s_rx_radio = LS_RSEL_SDR_HACKRF;
    ask_telemetry("ADSB");
    LS_EQ_INT(LS_RSEL_ADSB, s_rx_job_seen);
    LS_EQ_STR("HackRF", s_seen.rx_name);
    LS_CHECK(s_seen.rx_why == NULL);

    s_rx_radio = LS_RSEL_SDR_RTL;
    s_rx_absent = "Plug an RTL-SDR into the USB-A port";
    ask_telemetry("REC");
    LS_EQ_INT(LS_RSEL_REC, s_rx_job_seen);
    LS_EQ_STR("Plug an RTL-SDR into the USB-A port", s_seen.rx_why);

    /* What a radio cannot do outranks what is not plugged in. */
    s_rx_cant = "LR tunes 150-960 MHz; no broadcast FM / 2 m";
    ask_telemetry("FM");
    LS_EQ_INT(LS_RSEL_FM, s_rx_job_seen);
    LS_EQ_STR("LR tunes 150-960 MHz; no broadcast FM / 2 m", s_seen.rx_why);

    s_fm_tel_submode = FM_MODE_POCSAG;
    ask_telemetry("FM");
    LS_EQ_INT(LS_RSEL_PAGER, s_rx_job_seen);
    s_fm_tel_submode = FM_MODE_ACARS;
    ask_telemetry("FM");
    LS_EQ_INT(LS_RSEL_ACARS, s_rx_job_seen);
    s_fm_tel_submode = FM_MODE_LISTEN;

    s_rx_radio = LS_RSEL_NONE;
    ask_telemetry("P25");
    LS_CHECK(s_seen.rx_why != NULL);
    s_rx_radio = LS_RSEL_SDR_RTL;
    s_rx_cant = s_rx_absent = NULL;
    s_host_mode = "P25";
}

/* ---- MESH ---- */
static char s_mesh_name[24] = "LakeShark";
static uint32_t s_mesh_seq = 7, s_mesh_clock = 1000;
static ls_mesh_peer_t s_peers[3];
static int s_peer_n;
static ls_mesh_msg_t s_msgs[5];
static int s_msg_n;
static esp_err_t s_send_result = ESP_OK;
static char s_sent[128];

const char *ls_mesh_name(void) { return s_mesh_name; }
uint32_t ls_mesh_msg_seq(void) { return s_mesh_seq; }
uint32_t ls_mesh_now(void) { return s_mesh_clock; }
bool ls_mesh_peer_at(int rank, ls_mesh_peer_t *out)
{
    if (rank < 0 || rank >= s_peer_n) return false;
    *out = s_peers[rank];
    return true;
}
int ls_mesh_messages(ls_mesh_msg_t *out, int max)
{
    int n = s_msg_n < max ? s_msg_n : max;
    for (int i = 0; i < n; i++) out[i] = s_msgs[i];
    return n;
}
esp_err_t ls_mesh_send_text(const char *text)
{
    snprintf(s_sent, sizeof(s_sent), "%s", text);
    return s_send_result;
}
static int s_sent_chan = -1;
static char s_sent_id[20];
static ls_mesh_chan_t s_chans[LS_MESH_MAX_CHANNELS];
static int s_chan_active;
static bool s_tx_refuses;
int ls_mesh_channels(ls_mesh_chan_t *out, int max)
{
    int n = max < LS_MESH_MAX_CHANNELS ? max : LS_MESH_MAX_CHANNELS;
    for (int i = 0; i < n; i++) {
        out[i] = s_chans[i];
        out[i].fixed = (i == 0);
        if (i == 0) snprintf(out[i].name, sizeof(out[i].name), "Public");
    }
    return n;
}
int ls_mesh_channel_active(void) { return s_chan_active; }
esp_err_t ls_mesh_send_text_on(int chan, const char *text)
{
    s_sent_chan = chan;
    snprintf(s_sent, sizeof(s_sent), "%s", text);
    return s_send_result;
}
esp_err_t ls_mesh_send_dm_id(const char *id, const char *text)
{
    snprintf(s_sent_id, sizeof(s_sent_id), "%s", id);
    snprintf(s_sent, sizeof(s_sent), "%s", text);
    return s_send_result;
}
bool ls_mesh_running(void) { return g_mesh.running; }
void ls_mesh_set_tx(bool on) { if (!s_tx_refuses) g_mesh.tx_enabled = on; }
bool ls_mesh_tx_enabled(void) { return g_mesh.tx_enabled; }

static void ask(const char *line, char *reply, size_t cap)
{
    reply[0] = 0;
    flipper_link_inject(line, reply, cap);
}

static bool one_line(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && s[n - 1] == '\n' && strchr(s, '\n') == s + n - 1;
}

LS_CASE(the_head_reads_the_mesh_summary_as_key_values)
{
    char r[384];
    memset(&g_mesh, 0, sizeof(g_mesh));
    g_mesh_peers = 2;
    ask("MESH S", r, sizeof(r));
    LS_CHECK_MSG(!strncmp(r, "%M s ", 5), r);
    LS_CHECK(strstr(r, "run=0") && strstr(r, "st=OFFLINE"));
    g_mesh.running = true; g_mesh.receiving = true;
    g_mesh.rx_packets = 42; g_mesh.last_rssi = -91.0f;
    snprintf(g_mesh.self_id, sizeof(g_mesh.self_id), "A1B2C3D4E5F60718");
    ask("MESH S", r, sizeof(r));
    LS_CHECK(strstr(r, "st=LISTENING") && strstr(r, "rx=42") && strstr(r, "pr=2"));
    LS_CHECK(strstr(r, "ms=7") && strstr(r, "ch=0") && strstr(r, "nm=LakeShark") && strstr(r, "id=A1B2C3D4E5F60718"));
    LS_CHECK_MSG(strstr(r, "tx=0"), "the armed flag must be 0 while disarmed");
    g_mesh.tx_enabled = true;
    ask("MESH S", r, sizeof(r));
    LS_CHECK(strstr(r, "st=ARMED") && strstr(r, "tx=1"));
    g_mesh.on_air = true;
    ask("MESH S", r, sizeof(r));
    LS_CHECK(strstr(r, "st=ON_AIR"));
    /* The old one-line verb is unchanged. */
    ask("MESH", r, sizeof(r));
    LS_CHECK(!strncmp(r, "+OK run=1 tx=1 rx=42", 20));
    ask("MESH BOGUS", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    memset(&g_mesh, 0, sizeof(g_mesh));
    g_mesh_peers = 0;
}

LS_CASE(the_head_pages_through_mesh_nodes_one_row_a_request)
{
    char r[384];
    memset(s_peers, 0, sizeof(s_peers));
    snprintf(s_peers[0].id, sizeof(s_peers[0].id), "00112233AABBCCDD");
    snprintf(s_peers[0].name, sizeof(s_peers[0].name), "Ridge Repeater");
    s_peers[0].type = 2; s_peers[0].rssi = -101.4f; s_peers[0].last_heard = 940;
    snprintf(s_peers[1].id, sizeof(s_peers[1].id), "99887766FFEEDDCC");
    s_peers[1].type = 1;                        /* never heard, no name */
    s_peer_n = 2; g_mesh_peers = 2;
    ask("MESH P 0", r, sizeof(r));
    LS_EQ_STR("%M p 0 2 00112233AABBCCDD 2 -101 60 Ridge Repeater\n", r);
    ask("MESH P 1", r, sizeof(r));
    LS_EQ_STR("%M p 1 2 99887766FFEEDDCC 1 0 -1 -\n", r);
    /* Past the end is an empty row that still carries the total. */
    ask("MESH P 2", r, sizeof(r));
    LS_EQ_STR("%M p 2 2 - 0 0 -1 -\n", r);
    ask("MESH P x", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    s_peer_n = 0; g_mesh_peers = 0;
}

LS_CASE(the_head_reads_mesh_messages_newest_first)
{
    char r[384];
    memset(s_msgs, 0, sizeof(s_msgs));
    snprintf(s_msgs[0].text, sizeof(s_msgs[0].text), "Ann: first");
    s_msgs[0].t = 900;
    snprintf(s_msgs[1].text, sizeof(s_msgs[1].text), "LakeShark: on my way");
    s_msgs[1].mine = true; s_msgs[1].state = LS_MSG_HEARD; s_msgs[1].t = 990;
    snprintf(s_msgs[2].text, sizeof(s_msgs[2].text), "Bo: dm\tonly");
    s_msgs[2].direct = true; s_msgs[2].t = 1000;
    s_msg_n = 3;
    ask("MESH M 0", r, sizeof(r));
    LS_EQ_STR("%M m 0 3 id 0 0 a Bo: dm only\n", r);      /* control char flattened */
    ask("MESH M 1", r, sizeof(r));
    LS_EQ_STR("%M m 1 3 o 3 10 a LakeShark: on my way\n", r);
    ask("MESH M 2", r, sizeof(r));
    LS_EQ_STR("%M m 2 3 i 0 100 a Ann: first\n", r);
    ask("MESH M 3", r, sizeof(r));
    LS_EQ_STR("%M m 3 3 - 0 -1 a -\n", r);
    /* A message at the ring's limit still fits one reply and one line. */
    memset(s_msgs[0].text, 'x', sizeof(s_msgs[0].text) - 1);
    s_msgs[0].text[sizeof(s_msgs[0].text) - 1] = 0;
    s_msg_n = 1;
    ask("MESH M 0", r, sizeof(r));
    LS_CHECK(strlen(r) < 160 && one_line(r));
    s_msg_n = 0;
}

LS_CASE(the_head_sends_a_canned_mesh_message_only_when_the_board_allows)
{
    char r[96];
    s_sent[0] = 0; s_send_result = ESP_OK;
    ask("MESH SEND On my way", r, sizeof(r));
    LS_EQ_STR("+OK mesh sent\n", r);
    LS_EQ_STR("On my way", s_sent);
    s_send_result = ESP_ERR_NOT_ALLOWED;
    ask("MESH SEND OK", r, sizeof(r));
    LS_EQ_STR("-ERR mesh tx disarmed\n", r);
    s_send_result = ESP_ERR_INVALID_STATE;
    ask("MESH SEND OK", r, sizeof(r));
    LS_EQ_STR("-ERR mesh not running\n", r);
    s_send_result = ESP_OK; s_sent[0] = 0;
    /* Free text is the rest of the line, not the tokenizer's seven words. */
    ask("mesh send Heading to the north lot, back by 5:30 ok?  ", r, sizeof(r));
    LS_EQ_STR("+OK mesh sent\n", r);
    LS_EQ_STR("Heading to the north lot, back by 5:30 ok?", s_sent);
    s_sent[0] = 0;
    char longer[160];
    memset(longer, 'x', sizeof(longer));
    memcpy(longer, "MESH SEND ", 10);
    longer[sizeof(longer) - 1] = 0;
    ask(longer, r, sizeof(r));
    LS_EQ_STR("-ERR mesh text too long\n", r);
    LS_CHECK(!s_sent[0]);
    ask("MESH SEND", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4) && !s_sent[0]);
    ask("MESH SEND   ", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4) && !s_sent[0]);
    ask("MESHY SEND hi", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4) && !s_sent[0]);
}

/* ---- DETECTOR ---- */
static ls_detector_status_t s_det;
static ls_detector_event_t s_det_events[3];
static size_t s_det_n;
static bool s_det_busy, s_det_started;

bool ls_detector_snapshot(ls_detector_status_t *out, ls_detector_event_t *events,
                          size_t cap, size_t *n)
{
    if (s_det_busy) return false;
    *out = s_det;
    *n = s_det_n < cap ? s_det_n : cap;
    if (events) for (size_t i = 0; i < *n; i++) events[i] = s_det_events[i];
    return true;
}
void ls_detector_start(bool on) { s_det_started = on; s_det.running = on; }
const char *ls_detector_source(unsigned s) { return s == DET_ADSB ? "adsb" : "rid.ble"; }
void ls_detector_air_event_text(const ls_detector_event_t *e, char *out, size_t cap)
{ snprintf(out, cap, "%s sq%04u", e->callsign, e->squawk); }
bool ls_detector_air_alarm(unsigned squawk, unsigned emergency)
{ return emergency || squawk == 7700; }

LS_CASE(the_head_reads_detector_status_and_alerts)
{
    char r[384];
    memset(&s_det, 0, sizeof(s_det));
    memset(s_det_events, 0, sizeof(s_det_events));
    s_det.running = true; s_det.rules = 9; s_det.matches = 2; s_det.recent = 2;
    snprintf(s_det.adsb, sizeof(s_det.adsb), "RTL 1090");
    s_det_n = 2; s_det_busy = false;
    ask("DET", r, sizeof(r));
    LS_EQ_STR("%A s run=1 rules=9 hits=2 sup=0 drop=0 hist=2 adsb=RTL_1090\n", r);
    ask("DET S", r, sizeof(r));
    LS_CHECK(!strncmp(r, "%A s run=1", 10));

    ls_shim_time_set(100 * 1000000LL);
    s_det_events[0].source = DET_ADSB; s_det_events[0].squawk = 7700;
    snprintf(s_det_events[0].callsign, sizeof(s_det_events[0].callsign), "UAL123");
    s_det_events[0].at = 90 * 1000000LL;
    s_det_events[1].source = DET_RID; s_det_events[1].at = 40 * 1000000LL;
    snprintf(s_det_events[1].reason, sizeof(s_det_events[1].reason), "remote-id");
    ask("DET 0", r, sizeof(r));
    LS_EQ_STR("%A a 0 2 10 1 adsb UAL123 sq7700\n", r);
    ask("DET 1", r, sizeof(r));
    LS_EQ_STR("%A a 1 2 60 0 rid.ble remote-id\n", r);
    ask("DET 2", r, sizeof(r));
    LS_EQ_STR("%A a 2 2 -1 0 - -\n", r);
    s_det_busy = true;
    ask("DET 0", r, sizeof(r));
    LS_CHECK_MSG(!strncmp(r, "-ERR det busy", 13), "a busy detector must not read as no alerts");
    s_det_busy = false;
    ask("DET -1", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    ask("DET RUN 0", r, sizeof(r));
    LS_CHECK(!s_det_started && strstr(r, "run=0"));
    ask("DET RUN 1", r, sizeof(r));
    LS_CHECK(s_det_started && strstr(r, "run=1"));
    ask("DET RUN 2", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
}

LS_CASE(the_eq_line_always_carries_the_detector_state_and_stop_is_seen)
{
    char line[96];
    memset(&s_det, 0, sizeof(s_det));
    s_det_busy = false;
    flipper_link_eq_snapshot(line, sizeof(line));
    LS_CHECK_MSG(strstr(line, "dtm=0 dth=0 dtr=0"),
                 "a stopped detector must say it is stopped, or the head keeps showing it on");
    s_det.running = true; s_det.matches = 4294967295u; s_det.recent = 24;
    int n = flipper_link_eq_snapshot(line, sizeof(line));
    LS_CHECK(strstr(line, "dtm=4294967295 dth=24 dtr=1") != NULL);
    LS_CHECK(n == (int)strlen(line) && one_line(line));
    LS_CHECK(strncmp(line, "& eq=", 5) == 0);
    s_det_busy = true;
    flipper_link_eq_snapshot(line, sizeof(line));
    LS_CHECK_MSG(strstr(line, "eq=") && !strstr(line, "dtm="), "a busy lock must only skip the tail");
    s_det_busy = false;
}

/* ---- channels, direct messages and the transmit switch ---- */

static void seed_conversations(void)
{
    memset(s_msgs, 0, sizeof(s_msgs));
    /* oldest first, as the ring keeps them */
    snprintf(s_msgs[0].text, sizeof(s_msgs[0].text), "Ann: public one");
    s_msgs[0].t = 900;  s_msgs[0].chan = 0; s_msgs[0].id = 11;
    snprintf(s_msgs[1].text, sizeof(s_msgs[1].text), "Zed: crew channel");
    s_msgs[1].t = 920;  s_msgs[1].chan = 1; s_msgs[1].id = 12;
    snprintf(s_msgs[2].text, sizeof(s_msgs[2].text), "Bo: hello you");
    s_msgs[2].t = 950;  s_msgs[2].direct = true; s_msgs[2].id = 13;
    snprintf(s_msgs[2].peer, sizeof(s_msgs[2].peer), "99887766FFEEDDCC");
    snprintf(s_msgs[3].text, sizeof(s_msgs[3].text), "LakeShark: reply to Bo");
    s_msgs[3].t = 960;  s_msgs[3].direct = true; s_msgs[3].mine = true; s_msgs[3].id = 14;
    s_msgs[3].state = LS_MSG_SENT;
    snprintf(s_msgs[3].peer, sizeof(s_msgs[3].peer), "99887766FFEEDDCC");
    snprintf(s_msgs[4].text, sizeof(s_msgs[4].text), "Ann: public two");
    s_msgs[4].t = 990;  s_msgs[4].chan = 0; s_msgs[4].id = 15;
    s_msg_n = 5;
}

LS_CASE(the_head_lists_channels_with_their_newest_message_id)
{
    char r[200];
    memset(s_chans, 0, sizeof(s_chans));
    snprintf(s_chans[1].name, sizeof(s_chans[1].name), "Crew Net");
    snprintf(s_chans[1].psk, sizeof(s_chans[1].psk), "AAAAAAAAAAAAAAAAAAAAAA==");
    s_chan_active = 1;
    seed_conversations();
    ask("MESH C 0", r, sizeof(r));
    LS_EQ_STR("%M c 0 4 1 0 2 15 Public\n", r);         /* two messages, newest id 15 */
    ask("MESH C 1", r, sizeof(r));
    LS_EQ_STR("%M c 1 4 1 1 1 12 Crew Net\n", r);
    ask("MESH C 2", r, sizeof(r));
    LS_EQ_STR("%M c 2 4 0 0 0 0 -\n", r);               /* no key: not usable */
    ask("MESH C 9", r, sizeof(r));
    LS_EQ_STR("%M c 9 4 0 0 0 0 -\n", r);
    ask("MESH C x", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    s_chan_active = 0; s_msg_n = 0;
}

LS_CASE(the_head_reads_one_conversation_at_a_time)
{
    char r[384];
    seed_conversations();
    /* a channel: only its messages, newest first, total counted inside it */
    ask("MESH M 0 C 0", r, sizeof(r));
    LS_EQ_STR("%M m 0 2 i 0 10 c0 Ann: public two\n", r);
    ask("MESH M 1 C 0", r, sizeof(r));
    LS_EQ_STR("%M m 1 2 i 0 100 c0 Ann: public one\n", r);
    ask("MESH M 0 C 1", r, sizeof(r));
    LS_EQ_STR("%M m 0 1 i 0 80 c1 Zed: crew channel\n", r);
    ask("MESH M 0 C 3", r, sizeof(r));
    LS_EQ_STR("%M m 0 0 - 0 -1 c3 -\n", r);
    /* a node: only the direct messages with it, and the direct flag */
    ask("MESH M 0 D 99887766ffeeddcc", r, sizeof(r));     /* the id is case-blind */
    LS_EQ_STR("%M m 0 2 od 2 40 d99887766FFEEDDCC LakeShark: reply to Bo\n", r);
    ask("MESH M 1 D 99887766FFEEDDCC", r, sizeof(r));
    LS_EQ_STR("%M m 1 2 id 0 50 d99887766FFEEDDCC Bo: hello you\n", r);
    ask("MESH M 0 D 0000000000000000", r, sizeof(r));
    LS_EQ_STR("%M m 0 0 - 0 -1 d0000000000000000 -\n", r);
    /* everything, unchanged but for its scope */
    ask("MESH M 0", r, sizeof(r));
    LS_EQ_STR("%M m 0 5 i 0 10 a Ann: public two\n", r);
    ask("MESH M 0 D nothex", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    ask("MESH M 0 C 9", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    ask("MESH M 0 X 1", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    s_msg_n = 0;
}

LS_CASE(the_head_sends_on_a_channel_or_to_a_node_with_free_text)
{
    char r[96];
    memset(s_chans, 0, sizeof(s_chans));
    snprintf(s_chans[1].psk, sizeof(s_chans[1].psk), "AAAAAAAAAAAAAAAAAAAAAA==");
    s_send_result = ESP_OK; s_sent[0] = 0; s_sent_chan = -1; s_sent_id[0] = 0;

    ask("MESH SENDC 1 Meet at the north lot, 5:30 sharp!  ", r, sizeof(r));
    LS_EQ_STR("+OK mesh sent\n", r);
    LS_EQ_INT(1, s_sent_chan);
    LS_EQ_STR("Meet at the north lot, 5:30 sharp!", s_sent);
    ask("mesh sendc 0 Case Kept", r, sizeof(r));
    LS_EQ_STR("+OK mesh sent\n", r);
    LS_EQ_INT(0, s_sent_chan);
    LS_EQ_STR("Case Kept", s_sent);

    s_sent[0] = 0; s_sent_chan = -1;
    ask("MESH SENDC 2 nobody listens here", r, sizeof(r));   /* slot without a key */
    LS_EQ_STR("-ERR mesh channel\n", r);
    ask("MESH SENDC 7 nobody listens here", r, sizeof(r));
    LS_EQ_STR("-ERR mesh channel\n", r);
    ask("MESH SENDC x hi", r, sizeof(r));
    LS_EQ_STR("-ERR mesh channel\n", r);
    LS_CHECK(!s_sent[0] && s_sent_chan == -1);
    ask("MESH SENDC 1", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));

    ask("MESH DM 99887766ffeeddcc see you at six, ok?", r, sizeof(r));
    LS_EQ_STR("+OK mesh sent\n", r);
    LS_EQ_STR("99887766FFEEDDCC", s_sent_id);              /* the id as the board keeps it */
    LS_EQ_STR("see you at six, ok?", s_sent);
    s_sent[0] = 0;
    ask("MESH DM 9988 too short an id", r, sizeof(r));
    LS_EQ_STR("-ERR mesh no such node\n", r);
    ask("MESH DM zz887766FFEEDDCC not hex", r, sizeof(r));
    LS_EQ_STR("-ERR mesh no such node\n", r);
    LS_CHECK(!s_sent[0]);
    ask("MESH DM 99887766FFEEDDCC", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));

    /* the board's own refusals reach the head as they are */
    s_send_result = ESP_ERR_NOT_FOUND;
    ask("MESH DM 99887766FFEEDDCC hi", r, sizeof(r));
    LS_EQ_STR("-ERR mesh no such node\n", r);
    s_send_result = ESP_ERR_NOT_ALLOWED;
    ask("MESH DM 99887766FFEEDDCC hi", r, sizeof(r));
    LS_EQ_STR("-ERR mesh tx disarmed\n", r);
    ask("MESH SENDC 1 hi", r, sizeof(r));
    LS_EQ_STR("-ERR mesh tx disarmed\n", r);
    s_send_result = ESP_OK;

    /* too long is refused whole, on every route */
    char longer[200];
    memset(longer, 'x', sizeof(longer));
    longer[sizeof(longer) - 1] = 0;
    memcpy(longer, "MESH DM 99887766FFEEDDCC ", 25);
    s_sent[0] = 0;
    ask(longer, r, sizeof(r));
    LS_EQ_STR("-ERR mesh text too long\n", r);
    memcpy(longer, "MESH SENDC 1 ", 13);
    ask(longer, r, sizeof(r));
    LS_EQ_STR("-ERR mesh text too long\n", r);
    LS_CHECK(!s_sent[0]);
}

LS_CASE(the_head_arms_and_disarms_transmit_and_is_told_what_happened)
{
    char r[96];
    memset(&g_mesh, 0, sizeof(g_mesh));
    s_tx_refuses = false;
    ask("MESH TX 1", r, sizeof(r));
    LS_EQ_STR("-ERR mesh not running\n", r);              /* nothing to arm */
    LS_CHECK(!g_mesh.tx_enabled);

    g_mesh.running = true;
    ask("MESH TX 1", r, sizeof(r));
    LS_EQ_STR("+OK mesh tx=1\n", r);
    LS_CHECK(g_mesh.tx_enabled);
    ask("MESH TX 0", r, sizeof(r));
    LS_EQ_STR("+OK mesh tx=0\n", r);
    LS_CHECK(!g_mesh.tx_enabled);

    /* The reply is the state read back, so a switch that did not take says so. */
    s_tx_refuses = true;
    ask("MESH TX 1", r, sizeof(r));
    LS_EQ_STR("+OK mesh tx=0\n", r);
    s_tx_refuses = false;

    ask("MESH TX 2", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    ask("MESH TX", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    ask("MESH TX 1 1", r, sizeof(r));
    LS_CHECK(!strncmp(r, "-ERR", 4));
    LS_CHECK(!g_mesh.tx_enabled);
    memset(&g_mesh, 0, sizeof(g_mesh));
}

LS_CASE(a_detector_stopped_from_the_console_is_seen_on_the_next_eq_line)
{
    char line[96];
    memset(&s_det, 0, sizeof(s_det));
    s_det_busy = false;
    s_det.running = true; s_det.matches = 0;
    flipper_link_eq_snapshot(line, sizeof(line));
    LS_CHECK(strstr(line, "dtr=1"));
    ls_detector_start(false);                              /* what `detector stop` does */
    flipper_link_eq_snapshot(line, sizeof(line));
    LS_CHECK_MSG(strstr(line, "dtr=0"), "the stop was not reported to the head");
    /* and DET S says the same thing */
    char r[160];
    ask("DET S", r, sizeof(r));
    LS_CHECK(strstr(r, "run=0"));
}
