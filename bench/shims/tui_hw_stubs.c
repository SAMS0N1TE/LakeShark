#include <stdlib.h>
#include "adsb_source.h"
#include "uat_stream.h"


#include <stdbool.h>
#include <stddef.h>

#include "ls_gauge.h"
#include "ls_wf_source.h"
#include "ls_userapp.h"
#include "ls_field.h"
#include <string.h>

/* DRONES needs a position even in router tests without a field worker.
 * These targets use an explicitly invalid fix; real provider builds omit it. */

#ifdef LS_FIELD_SAMPLE_STUB
void ls_field_sample_snapshot(ls_field_sample_t *out)
{
    if(out) memset(out,0,sizeof(*out));
}
#endif

/* Benches that link the waterfall without ls_tui.c: no image cells, so it
   draws its history in cells and never asks for a theme. */
#if defined(LS_TUI_IMAGE_STUB) || defined(LS_TUI_THEME_STUB)
#include "ls_theme.h"
bool ls_tui_has_images(void) { return false; }
const ls_tui_theme_t *ls_tui_active_theme(void) { return NULL; }
#endif
#ifdef LS_TUI_IMAGE_STUB
#include "ls_tui.h"
void ls_tui_image(tui_rect cells, const uint16_t *src, int w, int h, uint32_t serial) { (void)cells; (void)src; (void)w; (void)h; (void)serial; }
bool ls_tui_image_now(tui_rect *cells, const uint16_t **src, int *w, int *h, uint32_t *serial) { (void)cells; (void)src; (void)w; (void)h; (void)serial; return false; }
void ls_tui_image_ring(tui_rect cells, const uint16_t *src, int w, int h, int x0, int y0, int dx, int dy, uint32_t serial) { (void)cells; (void)src; (void)w; (void)h; (void)x0; (void)y0; (void)dx; (void)dy; (void)serial; }
void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch) { if (cols) *cols = 0; if (rows) *rows = 0; if (cw) *cw = 0; if (ch) *ch = 0; }
#endif

#ifdef LS_TUI_CORNER_STUB
int ls_tui_corner_pad(int row) { (void)row; return 0; }
#endif
#ifdef LS_TUI_WIDE_STUB
bool ls_tui_is_wide(void) { return true; }
bool ls_keypad_present(void);
bool ls_tui_keyboard_mode(void) { return ls_keypad_present(); }
#endif

/* ---- ADS-B receiver --------------------------------------------------- */

/* adsb_source_t is an enum whose ADSB_SRC_NONE is 0; the header is not on
   every stub user's include path. */
adsb_source_t adsb_active_source(void) { return 0; }
void adsb_uat_use_picker(void) { }
void adsb_uat_stats(uat_stats_t *out) { if (out) memset(out, 0, sizeof(*out)); }

/* The gain setting, kept here as the back end keeps it: tenths of a dB, 0
   automatic. */
static int s_adsb_gain = 496;
int  lakeshark_adsb_gain_tenths(void) { return s_adsb_gain; }
void lakeshark_adsb_set_gain(int tenths) { s_adsb_gain = tenths < 0 ? 0 : tenths > 496 ? 496 : tenths; }
/* The mini map's FOLLOW mode, kept as app_adsb.c keeps it: -1 until one is
   chosen, which the screen reads as its default. */
static int s_adsb_follow = -1;
int  adsb_map_follow(void) { return s_adsb_follow; }
void adsb_set_map_follow(int mode) { if (mode >= 0 && mode <= 15) s_adsb_follow = mode; }
/* adsb_source.c's mapping, which that file's test checks against the part. */
int adsb_lr_gain_step(int gain_tenths_db)
{
    if (gain_tenths_db <= 0 || gain_tenths_db >= 496) return 13;
    const int step = (gain_tenths_db * 13 + 495) / 496;
    return step < 1 ? 1 : step > 13 ? 13 : step;
}

/* A Mode S session on the LR2021, off until a test starts one. */
#include "ls_lora.h"
bool ls_test_modes_session;
ls_lora_modes_tuning_t ls_test_modes_tuning = { 13, 7, 3076923, false, 0 };
int ls_test_modes_raw = 70;
bool ls_lora_modes_active(void) { return ls_test_modes_session; }
esp_err_t ls_lora_modes_tuning(ls_lora_modes_tuning_t *out)
{
    if (!ls_test_modes_session) return ESP_ERR_INVALID_STATE;
    *out = ls_test_modes_tuning;
    return ESP_OK;
}
esp_err_t ls_lora_modes_set_boost(int boost)
{
    if (!ls_test_modes_session) return ESP_ERR_INVALID_STATE;
    ls_test_modes_tuning.boost = boost;
    return ESP_OK;
}
esp_err_t ls_lora_modes_set_bw(uint32_t hz, uint32_t *chosen_hz)
{
    if (!ls_test_modes_session) return ESP_ERR_INVALID_STATE;
    ls_test_modes_tuning.rx_bw_hz = hz;
    if (chosen_hz) *chosen_hz = hz;
    return ESP_OK;
}
esp_err_t ls_lora_modes_set_threshold(int level_db, bool automatic)
{
    if (!ls_test_modes_session) return ESP_ERR_INVALID_STATE;
    ls_test_modes_tuning.thresh_override = !automatic;
    ls_test_modes_tuning.thresh_level = automatic ? 0 : level_db;
    return ESP_OK;
}
esp_err_t ls_lora_modes_read_threshold(int *raw)
{
    if (!ls_test_modes_session) return ESP_ERR_INVALID_STATE;
    *raw = ls_test_modes_raw;
    return ESP_OK;
}

/* ---- fuel gauge ---------------------------------------------------- */

static ls_gauge_t s_test_gauge;
void ls_shim_gauge(const ls_gauge_t *g)
{ s_test_gauge = g ? *g : (ls_gauge_t){0}; }
bool ls_gauge_present(void) { return s_test_gauge.present; }
bool ls_gauge_get(ls_gauge_t *out)
{
    if (out) *out = s_test_gauge;
    return s_test_gauge.present;
}

/* ---- what feeds the waterfall --------------------------------------- */

/* Stubbed for the tests, real for the simulator. */

#ifndef LS_WF_SOURCE_REAL

static ls_wf_src_t s_src;
int ls_test_wf_pumps, ls_test_wf_releases;

void        ls_wf_source_select(ls_wf_src_t src) { s_src = src; }
ls_wf_src_t ls_wf_source_get(void)               { return s_src; }
const char *ls_wf_source_name(void)              { return "none"; }
void ls_wf_source_view(uint32_t hz) { (void)hz; }
void        ls_wf_source_pump(void)              { ls_test_wf_pumps++; }
void        ls_wf_source_release(void)           { ls_test_wf_releases++; }
/* FM's SWEEP page starts the sweep through this, so the screen tests
   need it. Selecting is all a stub can honestly do: there is no receiver. */
bool        ls_wf_source_start(ls_wf_src_t src)  { s_src = src; return true; }

#endif

/* ---- the receiver a screen asks for ---------------------------------- */

/* The router asks for a receiver on every screen change. There is no
   receiver on a desktop and no radio app registry either, so this records
   nothing and does nothing - the question under test is whether the router
   ASKS, and a test that wants to check that asks the router, not this. */
void ls_tui_radio_want(const char *mode_name) { (void)mode_name; }
/* The bench has no receiver, so nothing is ever claimed. A screen
   that lights its lamp off this would light it never here, which is the
   right way for a stub to be wrong. */
const char *ls_test_radio_claimed;
const char *ls_tui_radio_claimed(void) { return ls_test_radio_claimed; }

/* ---- apps loaded from the card --------------------------------------- */

int ls_userapp_load_dir(const char *dir) { (void)dir; return 0; }

const char *ls_test_userapp_error;
const char *ls_userapp_last_error(void)
{ return ls_test_userapp_error ? ls_test_userapp_error : getenv("LSSIM_USERAPP_ERROR"); }

/* ---- the card, the build and the last crash -------------------------- */

#include "ls_sdcard.h"
#include "ls_crash.h"
#include "ls_version.h"

bool ls_test_sd_mounted;
esp_err_t ls_test_sd_result = ESP_ERR_NOT_SUPPORTED;
unsigned ls_test_sd_mount_calls, ls_test_sd_check_calls;
esp_err_t ls_sdcard_mount(void)
{
    ls_test_sd_mount_calls++;
    if (ls_test_sd_result == ESP_OK) ls_test_sd_mounted = true;
    return ls_test_sd_result;
}
void ls_sdcard_unmount(void) { ls_test_sd_mounted = false; }
bool ls_sdcard_mounted(void) { return ls_test_sd_mounted; }

bool ls_sdcard_size(uint64_t *total, uint64_t *free_bytes)
{
    if (total) *total = ls_test_sd_mounted ? 1024 * 1024 : 0;
    if (free_bytes) *free_bytes = ls_test_sd_mounted ? 512 * 1024 : 0;
    return ls_test_sd_mounted && ls_test_sd_result == ESP_OK;
}

esp_err_t ls_sdcard_check(uint64_t *total, uint64_t *free_bytes)
{
    ls_test_sd_check_calls++;
    ls_sdcard_size(total, free_bytes);
    return ls_test_sd_result;
}

const char *ls_sdcard_name(void) { return ls_test_sd_mounted ? "bench" : NULL; }
void        ls_sdcard_diagnostics(void) { }

bool ls_crash_present(void) { return false; }

/* ls_version_format and ls_version_is_dirty are pure and linked for real
   where a test wants them; only the part that reads the running image is
   stood down. */
void ls_version_get(ls_version_info_t *v)
{
    if (!v) return;
    v->version = "host";
    v->board = "bench";
    v->idf = "none";
    v->date = __DATE__;
    v->time = __TIME__;
}

/* ---- how this boot started ------------------------------------------- */

/* The health page reports the reset cause, which the device reads
   once at startup and keeps. ls_safe_mode.c itself is linked for real - it
   is pure - so only the part that consulted the chip is stood down here.

   A clean power-on, because that is what a host process did. The interesting
   branch is a panic, and a stub claiming one would put the page permanently
   in its alarming state and hide the ordinary one. */
#include "ls_safe_mode.h"

const ls_safe_boot_t *ls_safe_boot_result(void)
{
    static ls_safe_boot_t boot;
    boot.reset = LS_SAFE_RESET_POWERON;
    boot.reset_class = LS_SAFE_CLASS_CLEAN;
    boot.safe = false;
    return &boot;
}

__attribute__((weak)) bool ls_field_owned(void) { return false; }

/* No keyboard board in the simulator: editors show their touch keyboard. */
/* LSSIM_KEYPAD=1 renders a screen as it looks with the keyboard attached. */
/* LSSIM_KEYPAD=1 in the simulator; a test forces it with ls_shim_keypad. */
static int s_keypad_forced = -1;
void ls_shim_keypad(int present) { s_keypad_forced = present; }
bool ls_keypad_present(void)
{
    if (s_keypad_forced >= 0) return s_keypad_forced != 0;
    const char *e = getenv("LSSIM_KEYPAD");
    return e && e[0] == '1';
}

/* No speech engine on the host screens: Settings shows the voice as off,
   unless LSSIM_SPEECH=1 asks to see the rows that need one. */
#include "../../components/lakeshark/audio/speech.h"
bool speech_available(void)
{
    const char *e = getenv("LSSIM_SPEECH");
    return e && e[0] == '1';
}
static speech_voice_t s_voice;
static int s_speech_pct = 100;
speech_voice_t speech_voice_get(void) { return s_voice; }
speech_voice_t speech_voice_step(int dir)
{ s_voice = (speech_voice_t)((s_voice + SPEECH_VOICE_COUNT + dir) % SPEECH_VOICE_COUNT); return s_voice; }
const char *speech_voice_name(speech_voice_t v)
{ return v == SPEECH_VOICE_DARK ? "dark" : v == SPEECH_VOICE_FEMALE ? "female" : "glitch"; }
speech_result_t speech_say_async(const char *t) { (void)t; return SPEECH_UNAVAILABLE; }
void speech_cancel(void) {}
int  speech_volume_get(void) { return s_speech_pct; }
void speech_volume_set(int pct) { s_speech_pct = pct; }
void settings_speech_volume_set(int pct) { (void)pct; }
void audio_out_ensure_unmuted(void) {}

#include "../../components/lakeshark/audio/audio_events.h"
static audio_mode_t s_callout[AUDIO_EVT_KIND_COUNT];
static audio_mesh_say_t s_mesh_say;
audio_mode_t audio_event_mode_get(audio_evt_kind_t k) { return s_callout[k]; }
audio_mode_t audio_event_mode_cycle(audio_evt_kind_t k)
{ s_callout[k] = (audio_mode_t)((s_callout[k] + 1) % AUD_MODE_COUNT); return s_callout[k]; }
void audio_events_play_test(void) {}
audio_mesh_say_t audio_events_mesh_say_get(void) { return s_mesh_say; }
audio_mesh_say_t audio_events_mesh_say_cycle(void)
{ s_mesh_say = (audio_mesh_say_t)((s_mesh_say + 1) % AUD_MESH_COUNT); return s_mesh_say; }
void audio_event_mode_set(audio_evt_kind_t k, audio_mode_t m) { s_callout[k] = m; }
void audio_events_mesh_say_set(audio_mesh_say_t m) { s_mesh_say = m; }
static bool s_mesh_direct;
bool audio_events_mesh_direct_only(void) { return s_mesh_direct; }
void audio_events_mesh_set_direct_only(bool on) { s_mesh_direct = on; }
const char *audio_mesh_say_label(audio_mesh_say_t m) { (void)m; return "sender"; }
void audio_events_mesh_message(const char *text, bool direct) { (void)text; (void)direct; }

static bool s_key_dim = true;
bool display_ctl_keyboard_dim(void) { return s_key_dim; }
void display_ctl_set_keyboard_dim(bool on) { s_key_dim = on; }

#ifndef LS_WF_SETTINGS_FAKE
uint32_t settings_get_waterfall(uint32_t fallback) { return fallback; }
void settings_set_waterfall(uint32_t value) { (void)value; }
#endif

const char *adsb_receiver_status(void) { return "no receiver: attach radio"; }
const char *adsb_active_source_name(void) { return "none"; }
void adsb_chip_diag_reset(void) { }
void adsb_chip_diag_snapshot(adsb_chip_diag_t *out) { if (out) memset(out, 0, sizeof(*out)); }
#include "ls_mesh.h"
void ls_mesh_socket_get(ls_mesh_socket_t *out)
{ if (out) { memset(out, 0, sizeof(*out)); strcpy(out->chip, "none");
    strcpy(out->owner, "none"); strcpy(out->mode, "LoRa"); } }

#ifdef LS_WF_SOURCE_REAL
/* Simulator has no raw hardware FIFO. */
esp_err_t lr_wf_start(uint32_t hz) { (void)hz; return ESP_ERR_NOT_SUPPORTED; }
void lr_wf_stop(void) {}
#endif

__attribute__((weak)) void ls_tui_last_cost(uint32_t *us, int *cells)
{ if (us) *us = 0; if (cells) *cells = 0; }
