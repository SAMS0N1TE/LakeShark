#include <stdlib.h>


#include <stdbool.h>
#include <stddef.h>

#include "ls_gauge.h"
#include "ls_wf_source.h"
#include "ls_userapp.h"

#ifdef LS_TUI_CORNER_STUB
int ls_tui_corner_pad(int row) { (void)row; return 0; }
#endif
#ifdef LS_TUI_WIDE_STUB
bool ls_tui_is_wide(void) { return true; }
#endif

/* ---- fuel gauge ---------------------------------------------------- */

bool ls_gauge_present(void) { return false; }

bool ls_gauge_get(ls_gauge_t *out)
{
    if (out) {
        const ls_gauge_t empty = { 0 };
        *out = empty;
    }
    return false;
}

/* ---- what feeds the waterfall --------------------------------------- */

/* Stubbed for the tests, real for the simulator. */

#ifndef LS_WF_SOURCE_REAL

static ls_wf_src_t s_src;
int ls_test_wf_pumps, ls_test_wf_releases;

void        ls_wf_source_select(ls_wf_src_t src) { s_src = src; }
ls_wf_src_t ls_wf_source_get(void)               { return s_src; }
const char *ls_wf_source_name(void)              { return "none"; }
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
const char *ls_tui_radio_claimed(void) { return 0; }

/* ---- apps loaded from the card --------------------------------------- */

int ls_userapp_load_dir(const char *dir) { (void)dir; return 0; }

const char *ls_userapp_last_error(void) { return NULL; }

/* ---- the card, the build and the last crash -------------------------- */

#include "ls_sdcard.h"
#include "ls_crash.h"
#include "ls_version.h"

esp_err_t ls_sdcard_mount(void)   { return ESP_ERR_NOT_SUPPORTED; }
void      ls_sdcard_unmount(void) { }
bool      ls_sdcard_mounted(void) { return false; }

bool ls_sdcard_size(uint64_t *total, uint64_t *free_bytes)
{
    if (total) *total = 0;
    if (free_bytes) *free_bytes = 0;
    return false;
}

const char *ls_sdcard_name(void) { return NULL; }
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
bool ls_keypad_present(void) { const char *e = getenv("LSSIM_KEYPAD"); return e && e[0] == '1'; }

/* No speech engine on the host screens: Settings shows the voice as off. */
#include "../../components/lakeshark/audio/speech.h"
bool speech_available(void) { return false; }
speech_voice_t speech_voice_get(void) { return SPEECH_VOICE_GLITCH; }
speech_voice_t speech_voice_step(int dir) { (void)dir; return SPEECH_VOICE_GLITCH; }
const char *speech_voice_name(speech_voice_t v) { (void)v; return "glitch"; }
speech_result_t speech_say_async(const char *t) { (void)t; return SPEECH_UNAVAILABLE; }
void speech_cancel(void) {}
int  speech_volume_get(void) { return 100; }
void speech_volume_set(int pct) { (void)pct; }
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
const char *audio_mesh_say_label(audio_mesh_say_t m) { (void)m; return "sender"; }
void audio_events_mesh_message(const char *text, bool direct) { (void)text; (void)direct; }

static bool s_key_dim = true;
bool display_ctl_keyboard_dim(void) { return s_key_dim; }
void display_ctl_set_keyboard_dim(bool on) { s_key_dim = on; }
