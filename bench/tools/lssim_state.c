

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"
#include "ls_theme.h"
#include "ls_gps.h"

#include "apps/rec/rec_state.h"
#include "apps/fm/fm_state.h"
#include "ls_imu.h"
#include "ls_gauge.h"
#include "p25_state.h"
#include "ls_track_log.h"
#include "apps/p25/p25_program.h"

/* ------------------------------------------------------------ settings -- */

static int s_bright = 60, s_vol = 75, s_dimt = 60, s_boot = 1;
static bool s_autodim = true, s_usb = false;

int  settings_get_brightness(void) { return s_bright; }
void settings_set_brightness(int v) { s_bright = v; }
bool settings_get_autodim(void) { return s_autodim; }
void settings_set_autodim(bool v) { s_autodim = v; }
int  settings_get_autodim_timeout(void) { return s_dimt; }
void settings_set_autodim_timeout(int v) { s_dimt = v; }
/* SET goes through display_ctl now; in lssim it is the same fake. */
int  display_ctl_get_user(void) { return s_bright; }
void display_ctl_set_user(int v) { s_bright = v; }
bool display_ctl_autodim_enabled(void) { return s_autodim; }
void display_ctl_set_autodim(bool v) { s_autodim = v; }
int  display_ctl_autodim_timeout(void) { return s_dimt; }
void display_ctl_set_autodim_timeout(int v) { s_dimt = v; }
int  settings_get_volume(void) { return s_vol; }
void settings_set_volume(int v) { s_vol = v; }
int  settings_get_boot_sound(void) { return s_boot; }
void settings_set_boot_sound(int v) { s_boot = v; }
bool settings_get_usb_autoreboot(void) { return s_usb; }
void settings_set_usb_autoreboot(bool v) { s_usb = v; }
/* Both default on, the way the firmware's do. */
static bool s_alert_ring = true, s_alert_vibe = true;
static bool s_ant_ext;
bool settings_get_antenna_external(void) { return s_ant_ext; }
void settings_set_antenna_external(bool v) { s_ant_ext = v; }
bool settings_get_alert_ring(void) { return s_alert_ring; }
void settings_set_alert_ring(bool v) { s_alert_ring = v; }
bool settings_get_alert_vibe(void) { return s_alert_vibe; }
void settings_set_alert_vibe(bool v) { s_alert_vibe = v; }
int  settings_get_theme(void) { return 0; }
void settings_set_theme(int v) { (void)v; }
/* SET's Daylight row stores as well as applies; -D turns it on. */
static bool s_daylight;
bool settings_get_daylight(void) { return s_daylight; }
void settings_set_daylight(bool v) { s_daylight = v; }

/* Franklin, NH again - the GPS fix below already puts the radar's live
   centre there, so the saved-home fallback agreeing is what makes -e (no
   fix) and the ordinary case draw the same picture instead of two
   unrelated ones. */
bool settings_get_home(float *lat, float *lon)
{
    if (lat) *lat = 43.4445f;
    if (lon) *lon = -71.6473f;
    return true;
}

/* No theme accessors here: lssim links ls_tui.c for real, so the board's own
   ones are present. The tests fake them because they do not link the
   blitter; this does. */

/* ----------------------------------------------------------------- rec -- */

static rec_hub_status_t s_rec = {
    .phase = REC_CAPTURING,
    .freq_hz = 433920000u,
    .edges = 47,
    .mag_now = 900,
    .mag_thresh = 600,
    .bytes_sec = 48000,
    .captures = 3,
    .receiver_streaming = true,
};
static bool s_rec_armed = true;

/* The capture screen charts these over time, so a fixture that
   returns one number forever draws two flat lines and proves nothing about
   a chart. This is a band with a remote being keyed in it: a floor that
   wanders, bursts well over the trigger at a steady interval, and a write
   rate that follows the bursts because that is when bytes are produced. */
void rec_get_hub_status(rec_hub_status_t *out)
{
    if (!out) return;
    static uint32_t t;
    t++;

    const int phase = (int)(t % 200);
    const bool burst = (phase < 26);

    s_rec.mag_now = 180 + (int)((t * 37) % 90);        /* floor, textured */
    if (burst) s_rec.mag_now = 700 + (int)((t * 53) % 260);
    s_rec.bytes_sec = burst ? 38000u + (t * 311u) % 9000u : 0u;
    s_rec.edges = burst ? 40 + (int)((t * 7) % 22) : 0;
    s_rec.phase = burst ? REC_CAPTURING : REC_ARMED;

    *out = s_rec;
}
uint64_t rec_dir_free_bytes(void) { return 29ull * 1024 * 1024 * 1024; }
const char *rec_dir(void) { return "/sdcard/rec"; }
bool rec_active(void) { return s_rec_armed; }
void rec_arm(void) { s_rec_armed = true; }
void rec_arm_request(void) { s_rec_armed = true; }
void rec_disarm(void) { s_rec_armed = false; }

/* ------------------------------------------------------------- radios -- */

fm_state_t FM;
p25_state_t P25;
scan_state_t SCAN;
uint32_t s_tune_freq_hz = 851012500u;

int audio_volume_get(void) { return s_vol; }
void audio_volume_set(int v) { s_vol = v; }

/* No perf_history_good here either: perf.c is linked for real, so the
   sparkline on ADSB is fed by the same ring the board fills. */

void p25_get_receiver_status(ls_iq_control_status_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->receiver_streaming = true;
    }
}
/* ls_wf_source asks FM's receiver whether it is streaming before it
   treats FM as a spectrum. Streaming, like P25 above, because the simulator
   draws the busy branch; the fixture's FM is in LISTEN, so it still is not
   a sweep and AUTO still picks P25. */
void fm_get_receiver_status(ls_iq_control_status_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->receiver_streaming = true;
    }
}
void p25_request_gain(int tenths) { P25.rtl_gain_tenths = tenths; }
bool p25_running(void) { return true; }

void lakeshark_fm_set_freq(uint32_t hz) { FM.freq_hz = hz; }
/* what the keypad seeds itself from. */
uint32_t lakeshark_fm_get_freq(void) { return FM.freq_hz; }
uint32_t lakeshark_p25_get_freq(void) { return s_tune_freq_hz; }
void lakeshark_fm_set_mode(int mode) { FM.mode = (fm_mode_t)mode; }
/* A band preset sets the range and restarts the sweep. The simulator
   has no sweep to restart, so the range is the whole of what it models - and
   that is the part the FALLS band list actually renders. */
void lakeshark_fm_scan_restart(void) { }

/* The track recorder, faked as OFF and empty - which is the state a
   simulator honestly has: no receiver, no card, nothing recorded. The GPS
   screen's TRACK control renders from these, and a fixture that claimed to
   be recording would mean the not-recording layout was the one nobody could
   look at. */
bool ls_track_rec_running(void) { return false; }
int  ls_track_points(void) { return 0; }
esp_err_t ls_track_rec_start(void) { return -1; }
void ls_track_rec_stop(void) { }
int  lakeshark_fm_get_mode(void) { return (int)FM.mode; }
void lakeshark_fm_set_gain(int tenths) { FM.gain_tenths = tenths; }
void lakeshark_fm_set_gain_live(int tenths) { FM.gain_tenths = tenths; }
int  lakeshark_fm_gain_tenths(void) { return FM.gain_tenths; }
void lakeshark_fm_set_squelch(int v) { FM.squelch_tenths = v; }
int  lakeshark_fm_squelch_get(void) { return FM.squelch_tenths; }

/* --------------------------------------------------------------- gps ---- */

/* A fix, because the no-fix layout is three fields and a sentence and the
   one worth looking at is the full one. Franklin, New Hampshire - the same
   place the map fixture covers, so the two screens agree about where this
   imaginary radio is. */
static ls_gps_state_t s_gps = {
    .alive = true,
    .fix = true,
    .quality = 1,
    .sats_used = 9,
    .sats_visible = 14,
    .lat_deg = 43.44450,
    .lon_deg = -71.64730,
    .alt_m = 132.0f,
    .hdop = 1.2f,
    .speed_kts = 3.4f,
    .course_deg = 271.0f,
    .hour = 21, .minute = 43, .second = 35,
    .day = 9, .month = 9, .year = 2026,
    .bytes = 184320,
    .sentences = 2044,
    .checksum_errors = 0,
    /* The satellites themselves, which this fixture never had. */

    .sat_count = 14,
    .sats = {
        {  2, 71, 156, 44, true  },   /* high and strong, nearly overhead */
        {  5, 54,  92, 41, true  },
        {  9, 47, 243, 38, true  },
        { 12, 39,  47, 35, true  },
        { 15, 33, 301, 33, true  },
        { 18, 28, 128, 30, true  },
        { 21, 22, 205, 26, true  },
        { 24, 17, 274, 22, true  },
        { 29, 11,  63, 19, true  },   /* ninth: the last one in the fix   */
        { 31,  9, 349, 14, false },   /* low in the north, weak           */
        {  7,  6,   8, 11, false },
        { 13,  4, 337,  0, false },   /* seen, not tracked                */
        { 25, 15, 172,  0, false },
        { 30, 62, 218, 29, false },   /* high but not in the solution     */
    },
};

void ls_gps_get(ls_gps_state_t *out) { if (out) *out = s_gps; }

bool ls_imu_present(void) { return true; }
ls_imu_pose_t ls_imu_pose(void) { return LS_IMU_LEFT; }

bool ls_imu_read(ls_imu_sample_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->ax = -0.98f; out->ay = 0.04f; out->az = -0.06f;
    out->gx = 0.3f;   out->gy = -0.2f; out->gz = 0.1f;
    out->mx = 18.0f;  out->my = -31.0f; out->mz = -44.0f;
    out->mag_valid = true;
    out->temp_c = 34.5f;
    return true;
}

float ls_imu_heading(void) { return 119.0f; }
/* ls_gauge_get is in bench/shims/tui_hw_stubs.c, which every one of
   these links - and it already answers "no gauge", which is the branch
   worth rendering. */

/* Powered is its own state here too, and the fake has to keep it
   separately or it cannot reproduce the case the real driver had: a
   receiver that is running and not yet talking. Seeded true because the
   simulator's GPS screen is drawn with a fix. */
static bool s_gps_running = true;
bool ls_gps_running(void) { return s_gps_running; }

/* The antenna select, for the simulator. Internal until switched,
   which is what the board comes up as. */
static bool s_ant_ext_hw;
esp_err_t ls_board_hw_antenna_external(bool ext) { s_ant_ext_hw = ext; return ESP_OK; }
bool ls_board_hw_antenna_is_external(void) { return s_ant_ext_hw; }

esp_err_t ls_gps_start(void)
{
    s_gps_running = true;
    s_gps.alive = true;
    return 0;
}

void ls_gps_stop(void)
{
    s_gps_running = false;
    s_gps.alive = false;
}

/* --------------------------------------------------------------- seed --- */

void lssim_seed_state(void)
{
    memset(&FM, 0, sizeof(FM));
    FM.freq_hz = 162550000u;
    FM.gain_tenths = 280;
    FM.squelch_tenths = 120;
    FM.squelch_open = true;
    FM.iq_level = 0.42f;
    FM.iq_bytes_sec = 240000;
    FM.pocsag_sync = true;
    FM.pocsag_baud = 1200;
    FM.pocsag_lock_baud = 1200;
    FM.pocsag_pages = 12;
    FM.scan_bins = 128;
    FM.scan_sweeps = 4;

    memset(&P25, 0, sizeof(P25));
    P25.dsd_nac = 0x293;
    P25.dsd_tg = 1041;
    P25.dsd_src = 220158;
    P25.dsd_has_sync = true;
    P25.iq_level = 0.61f;
    P25.rtl_gain_tenths = 280;
    /* Well past any clock this tool runs with - frozen at 1.5s (the
       default) or live from esp_timer's own zero (-T) - so the busiest
       branch ('s own reasoning) is the one a static render shows:
       a call in progress, not one that just ended. */
    P25.voice_active_until_us = esp_timer_get_time() + 5000000;
    snprintf(P25.dsd_modulation, sizeof(P25.dsd_modulation), "C4FM");
}

/* -------------------------------------------------------------- tick ---- */

/* What moves between frames.

   The POCSAG tape is built from deltas in the decoder's counters, so a
   fixture that sets them once draws an empty tape however busy the numbers
   look. This advances them the way a real pager channel does: mostly idle
   batches, an address and a message together when somebody is paged, and
   the occasional uncorrectable codeword because the signal is never
   perfect. */
void lssim_tick_state(void)
{
    static uint32_t t;
    t++;

    FM.pocsag_frames += 1;                       /* a batch a frame */
    if (t % 40 == 0) { FM.pocsag_addr += 1; FM.pocsag_msg += 3;
                       FM.pocsag_pages += 1; }
    if (t % 23 == 0)   FM.pocsag_cw_errs += 1;
}

/* -------------------------------------------------------------- mesh ---- */

/* The whole mesh fixture is in lssim_mesh.c: peers, events, messages, the
   radio settings, the channels and the scope. It outgrew a corner of this
   file the moment the MESH screen got pages. */

/* ---------------------------------------------------------- spectrum ----- */

#include "apps/p25/p25_spectrum.h"

static bool s_spec_on;
static uint32_t s_spec_seq;

/* Set by lssim -e. With it on the reader says "nothing published", which is
   what a board with no dongle says, and every screen takes its idle branch. */
static bool s_no_radio;

void lssim_set_empty(bool on)
{
    s_no_radio = on;
    if (on) { s_gps.fix = false; s_gps.sats_used = 0; }
}

/* So the mesh fixture in lssim_mesh.c can honour -e as well. Same
   reason the GPS fix is cleared above: -e means the idle branches, and an
   idle branch nobody can render is one nobody checks. */
bool lssim_is_empty(void) { return s_no_radio != 0; }

void p25_spectrum_enable(bool on) { s_spec_on = on; }
#include "apps/fm/fm_spectrum.h"
static bool s_fm_spec_on;
void fm_spectrum_enable(bool on) { s_fm_spec_on = on; }
bool fm_spectrum_read(float *out, int n, uint32_t now,
                       fm_spectrum_snapshot_t *snap)
{
    if (!s_fm_spec_on || s_no_radio || !out || n < 1) return false;
    for (int i = 0; i < n; ++i) out[i] = i == n / 2 ? 0.8f : 0.15f;
    *snap = (fm_spectrum_snapshot_t){FM.freq_hz, FM_RTL_RATE, now, now / 100 + 1};
    return true;
}
bool p25_spectrum_enabled(void) { return s_spec_on; }
void p25_spectrum_invalidate(void) { s_spec_seq = 0; }
void p25_spectrum_init(void) { s_spec_seq = 0; }

/* A bump: 1.0 at the middle, falling to zero `half` bins away. */
static float peak_at(int i, int centre, int half, float amp)
{
    int d = i - centre;
    if (d < 0) d = -d;
    if (d >= half) return 0.0f;
    const float t = 1.0f - (float)d / (float)half;
    return amp * t * t;
}

bool p25_spectrum_read(float *out, int n, uint32_t now_ms, uint32_t max_age_ms,
                       p25_spectrum_snapshot_t *snap)
{
    (void)max_age_ms;
    if (!out || n <= 0 || s_no_radio) return false;

    s_spec_seq++;
    const int mid = n / 2;

    for (int i = 0; i < n; i++) {

        const float a = (float)((i * 7) % 23) / 23.0f;
        const float b = (float)((i * 3 + s_spec_seq / 7) % 11) / 11.0f;
        float v = 0.05f + 0.035f * a + 0.02f * b;

        v += peak_at(i, mid, n / 40 + 2, 0.74f);                /* control  */
        v += peak_at(i, mid - n / 5, n / 30 + 2, 0.42f);        /* neighbour */
        v += peak_at(i, mid + n / 4, n / 24 + 2, 0.31f);
        /* One that fades in and out, so the history has a shape down it. */
        v += peak_at(i, mid + n / 9, n / 50 + 1,
                     0.36f * (float)((s_spec_seq / 3) % 5) / 4.0f);

        out[i] = (v > 1.0f) ? 1.0f : v;
    }

    if (snap) {
        memset(snap, 0, sizeof(*snap));
        snap->center_hz = 851012500u;
        snap->span_hz   = 240000u;
        snap->filter_hz = 0;
        snap->captured_ms = now_ms;
        snap->sequence = s_spec_seq;
        snap->fft_bins = P25_SPECTRUM_BINS;
        snap->blocks_per_fft = P25_SPECTRUM_BLOCK_STRIDE;
        snap->following_voice = false;
    }
    return true;
}

/* --------------------------------------------------------- touch stats -- */

/* ls_tui_touch.c owns the digitiser and the debounce, neither of which has a
   meaning here. DIAG reads these to tell a controller that is answering from
   one that is not, so the numbers say "answering, and somebody has tapped". */
void ls_tui_touch_stats(uint32_t *reads, uint32_t *taps,
                        int *last_col, int *last_row)
{
    if (reads)    *reads = 4211;
    if (taps)     *taps = 17;
    if (last_col) *last_col = 22;
    if (last_row) *last_row = 31;
}

/* The P25 tuner. One line, and the only thing ls_action_builtin wants from a
   receiver this machine does not have. */
void lakeshark_p25_set_freq(uint32_t hz) { s_tune_freq_hz = hz; }

/* No programmed profile in the simulator, which is the first-run
   state on a board too: p25_program_session() returns NULL until one has
   been read. Rendering THAT is the point - the band list has an empty case
   with a sentence in it, and this is how it gets drawn without a card. */
const p25_program_t *p25_program_session(void) { return 0; }
bool p25_program_step_control_now(int delta) { (void)delta; return false; }

/* perf.c publishes a heartbeat onto the event bus, which is a task and a
   queue and nothing a still image needs. */
void event_bus_publish_heartbeat(const void *hb) { (void)hb; }

/* shim_impl.c gates its logging on this, and the test runner that normally
   defines it carries a main() this tool has its own version of. Off: a
   simulator that printed every ESP_LOG the screens make would bury the one
   line saying where the image went. */
int ls_shim_log_enabled = 0;
