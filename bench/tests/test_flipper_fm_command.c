/* LS_TEST_SOURCES: ${FW}/main/flipper_link.c ${APP}/fm/fm_mode_label.c */
#include "ls_test.h"

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
        "listen", "scan", "pocsag", "wfm", "acars", "flex"
    };
    start_link();

    for (int mode = 0; mode < FM_MODE_COUNT; ++mode) {
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
        "FM 7", "FM 999999999999999999999"
    };
    s_fm_mode = FM_MODE_FLEX;
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
void lakeshark_fm_tune(int delta_hz) { (void)delta_hz; }
void lakeshark_fm_set_freq(uint32_t hz) { (void)hz; ++s_freq_sets; }
uint32_t lakeshark_fm_get_freq(void) { return 0; }
void lakeshark_fm_set_baud(int baud) { (void)baud; }
int lakeshark_fm_get_baud(void) { return 1200; }
void lakeshark_fm_squelch_delta(int d) { (void)d; }
void lakeshark_fm_set_squelch(int v) { (void)v; }
int lakeshark_fm_squelch_get(void) { return 0; }
void lakeshark_fm_scan_restart(void) {}
void lakeshark_fm_tune_to_peak(void) {}
void lakeshark_fm_telemetry(lakeshark_fm_tel_t *out) { memset(out, 0, sizeof(*out)); }

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
void rec_set_freq(uint32_t hz) { (void)hz; }
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
{ (void)buf; (void)len; (void)t; (void)common; return 0; }
int ls_telemetry_build_adsb(char *buf, size_t len, const lakeshark_adsb_tel_t *t,
                            const ls_telemetry_common_t *common)
{ (void)buf; (void)len; (void)t; (void)common; return 0; }
int ls_telemetry_build_p25(char *buf, size_t len, const lakeshark_p25_tel_t *t,
                           const char *mode, const char *demod,
                           const ls_telemetry_common_t *common)
{ (void)buf; (void)len; (void)t; (void)mode; (void)demod; (void)common; return 0; }
int ls_telemetry_build_rec(char *buf, size_t len, const rec_status_t *status,
                           const ls_telemetry_common_t *common)
{ (void)buf; (void)len; (void)status; (void)common; return 0; }
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
