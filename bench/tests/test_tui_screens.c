/* LS_TEST_SOURCES: the three screens plus tui_core, with the state they read faked */

#include "ls_test.h"
#include "tui_core.h"
#include "ls_tui_screen.h"
#include "ls_app.h"
#include "ls_icons.h"
/* ls_tile_grid and ls_tile_shape: the layout under test. */
#include "ls_tui_ui.h"
#include "ls_theme.h"
#include "apps/rec/rec_state.h"
#include "apps/p25/p25_health.h"
#include "radio/radio_health.h"
#include "apps/fm/fm_state.h"
#include "apps/fm/fm_mode_label.h"
#include "ls_action.h"
#include "apps/adsb/adsb_state.h"
#include "esp_timer.h"
#include "ls_gps.h"
#include "ls_mesh.h"
#include "ls_lora.h"
#include "ls_gauge.h"
#include "ls_imu.h"
#include "ls_field.h"
#include "ls_waterfall.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------- fakes -- */
bool ls_mesh_peer_at(int rank, ls_mesh_peer_t *out)
{ (void)rank; (void)out; return false; }

/* Values chosen so every conditional branch in the screens is on: counters
   non-zero so they take the red path, a fix present, a ring nearly full. */
static int s_bright = 60, s_vol = 40, s_dimt = 60, s_boot = 1;
static bool s_autodim = true, s_usb = false;

int  settings_get_brightness(void) { return s_bright; }
void settings_set_brightness(int v) { s_bright = v; }
bool settings_get_autodim(void) { return s_autodim; }
void settings_set_autodim(bool v) { s_autodim = v; }
int  settings_get_autodim_timeout(void) { return s_dimt; }
void settings_set_autodim_timeout(int v) { s_dimt = v; }
/* SET goes through display_ctl now; on the bench it is the same fake. */
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

/* ADSB's radar page () reads a centre point through these two -
   the saved home here, a live fix through ls_gps_get() below. Faked rather
   than linked for the same reason the rest of settings is: both settings.c
   and ls_gps.c reach into NVS and a UART driver that do not exist on the
   host. No fix by default, so the radar's own "no position" branch is what
   a test that does not seed one exercises - the same reasoning seed_adsb()
   gives for aircraft. */
bool settings_get_home(float *lat, float *lon)
{
    if (lat) *lat = 43.4445f;
    if (lon) *lon = -71.6473f;
    return true;
}

static bool s_gps_fix = false;
static double s_gps_lat = 43.60, s_gps_lon = -71.30;
void ls_gps_get(ls_gps_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->fix = s_gps_fix;
    out->lat_deg = s_gps_lat;
    out->lon_deg = s_gps_lon;
}
/* DIAG lists the radios and sensors now, so this fixture has to
   answer for them. Everything reports ABSENT, which is the honest state of a
   host with no board attached and - more usefully - is the branch every
   other LakeShark variant renders. The screen's "not fitted" path is the one
   a test can actually check here; the fitted one belongs to lssim, which
   fakes a T-Display-P4. */
bool ls_lora_present(void) { return false; }
bool ls_gps_running(void) { return false; }
/* The IMU is the one part here that is switchable, because the
   health page's sensor detail has two branches worth rendering and they are
   different pages: the axes, and the sentence that says there are none.

   Absent by DEFAULT, like everything else in this block - that is the honest
   state of a host with no board, and it is the branch every other LakeShark
   variant renders. A case that wants the axes says so and puts it back. */
static bool            s_imu_fitted;
static ls_imu_sample_t s_imu_fake;

bool ls_imu_present(void) { return s_imu_fitted; }
ls_imu_pose_t ls_imu_pose(void) { return LS_IMU_FLAT; }

bool ls_imu_read(ls_imu_sample_t *out)
{
    if (!s_imu_fitted || !out) return false;
    *out = s_imu_fake;
    return true;
}

float ls_imu_heading(void) { return 119.0f; }

static void imu_fitted(bool on)
{
    s_imu_fitted = on;
    memset(&s_imu_fake, 0, sizeof(s_imu_fake));
    if (!on) return;
    s_imu_fake.ax = -0.98f; s_imu_fake.ay = 0.04f; s_imu_fake.az = -0.06f;
    s_imu_fake.gx = 0.3f;   s_imu_fake.gy = -0.2f; s_imu_fake.gz = 0.1f;
    s_imu_fake.mx = 18.0f;  s_imu_fake.my = -31.0f; s_imu_fake.mz = -44.0f;
    s_imu_fake.mag_valid = true;
    s_imu_fake.temp_c = 34.5f;
}
/* And the mesh counters on the same page. Zeros, which is what a
   radio that is not fitted has done. */
void ls_mesh_get_stats(ls_mesh_stats_t *o) { if (o) memset(o, 0, sizeof(*o)); }
/* ls_gauge_get is in bench/shims/tui_hw_stubs.c, which every one of
   these links - and it already answers "no gauge", which is the branch
   worth rendering. */
bool ls_mesh_running(void) { return false; }

/* What SET stored, so a case can check that a press persisted what
   it showed and not only what it drew. */
static int  s_theme_stored = -1;
static bool s_daylight_stored;
int  settings_get_theme(void) { return s_theme_stored < 0 ? 0 : s_theme_stored; }
void settings_set_theme(int v) { s_theme_stored = v; }
bool settings_get_daylight(void) { return s_daylight_stored; }
void settings_set_daylight(bool v) { s_daylight_stored = v; }

static const ls_tui_theme_t *s_active;

const ls_tui_theme_t *ls_tui_get_theme(void)
{
    return s_active ? s_active : ls_tui_theme_at(0);
}
void ls_tui_set_theme(const ls_tui_theme_t *t) { s_active = t; }

/* Daylight's flag lives beside that pointer, so it is faked beside
   it. Which palette that means is ls_theme.c's rule, linked for real. */
static bool s_daylight;
void ls_tui_set_daylight(bool on) { s_daylight = on; }
bool ls_tui_daylight(void) { return s_daylight; }
const ls_tui_theme_t *ls_tui_active_theme(void)
{
    return ls_tui_theme_effective(ls_tui_get_theme(), s_daylight);
}

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

void rec_get_hub_status(rec_hub_status_t *out) { if (out) *out = s_rec; }
uint64_t rec_dir_free_bytes(void) { return 1500ull * 1024 * 1024; }
const char *rec_dir(void) { return "/sdcard/rec"; }
bool rec_active(void) { return s_rec_armed; }
void rec_arm(void) { s_rec_armed = true; }
void rec_arm_request(void) { s_rec_armed = true; }
void rec_disarm(void) { s_rec_armed = false; }

/* FM's state is one global struct. adsb_state.c is linked for real, since it
   needs nothing but esp_timer, and is seeded with actual aircraft through its
   own creation path - a list screen tested against an empty list is a list
   screen tested against its early return. */
fm_state_t FM;

int audio_volume_get(void) { return 60; }

const uint16_t *perf_history_good(void)
{

    static uint16_t h[64];
    for (int i = 0; i < 64; i++) h[i] = (uint16_t)(i < 32 ? i * 8 : (63 - i) * 8);
    return h;
}

static void seed_fm(void)
{
    memset(&FM, 0, sizeof(FM));
    FM.freq_hz = 162550000u;
    FM.gain_tenths = 280;
    FM.squelch_tenths = 120;
    FM.squelch_open = true;
    FM.iq_level = 0.42f;
    FM.iq_bytes_sec = 240000;
    FM.pocsag_sync = true;
    FM.pocsag_pages = 12;
    FM.scan_bins = 128;
    FM.scan_sweeps = 4;
    FM.scan_peak_hz = 162400000u;
    /* A fraction of FM_SCAN_FLOOR_DB..FM_SCAN_TOP_DB, which is what
       the sweep writes. This filled it with -90..-51, the same mistaken dBFS
       reading the waterfall's pump made. */
    for (int i = 0; i < 128 && i < (int)(sizeof(FM.scan_db) / sizeof(FM.scan_db[0])); i++)
        FM.scan_db[i] = 0.05f + 0.02f * (float)(i % 40);

    FM.page_count = FM_PAGE_LOG_MAX;
    FM.page_head = 3;
    for (int i = 0; i < FM_PAGE_LOG_MAX; i++) {
        FM.pages[i].protocol = (i & 1) ? FM_PAGE_PROTOCOL_FLEX
                                       : FM_PAGE_PROTOCOL_POCSAG;
        snprintf(FM.pages[i].text, sizeof(FM.pages[i].text),
                 "page %d with text long enough to need truncating somewhere", i);
    }
}

static void seed_adsb(void)
{
    adsb_state_init();
    for (uint32_t i = 0; i < 6; i++) {
        adsb_aircraft_t *a = adsb_state_find_or_create(0xA00000u + i);
        if (!a) continue;
        a->good_msg_count = 40 + (int)i;
        a->crc_err_count = (int)i;
        a->pending_alt = 30000 + (int)i * 500;
        a->pending_vel = 420;
        a->pending_hdg = 90;
        for (int k = 0; k < 32; k++)
            a->alt_history[k] = (int16_t)(300 + k);
    }
}

uint32_t perf_get_crc_good(void)     { return 48213; }
uint32_t perf_get_crc_err(void)      { return 91; }
uint32_t perf_get_msgs_per_sec(void) { return 37; }

void ls_tui_touch_stats(uint32_t *reads, uint32_t *taps, int *col, int *row)
{
    if (reads) *reads = 4210;
    if (taps) *taps = 17;
    if (col) *col = 12;
    if (row) *row = 5;
}

void ls_tui_last_cost(uint32_t *us, int *cells)
{
    if (us) *us = 1234;
    if (cells) *cells = 62;
}

/* ------------------------------------------------------------- fixtures -- */

#define W 130
#define H 70
#define SENTINEL '#'

static tui_cell g_back[W * H];
static tui_cell g_front[W * H];
static tui_surface g_sf;

static void fresh(void)
{
    tui_surface_setup(&g_sf, g_back, g_front, W, H);
    for (int i = 0; i < W * H; i++) {
        g_back[i].ch = SENTINEL;
        g_back[i].attr = TUI_DEFAULT_ATTR;
    }
}

static int escaped(tui_rect r)
{
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (tui_rect_contains(r, x, y)) continue;
            if (g_back[y * W + x].ch != SENTINEL) n++;
        }
    return n;
}

extern const ls_tui_screen_t ls_scr_settings, ls_scr_diag, ls_scr_rec,
                             ls_scr_home, ls_scr_fm, ls_scr_adsb, ls_scr_labs, ls_scr_journal, ls_scr_subghz, ls_scr_mixrf;

static const ls_tui_screen_t *const SCREENS[] = {
    &ls_scr_settings, &ls_scr_diag, &ls_scr_rec, &ls_scr_home,
    &ls_scr_fm, &ls_scr_adsb, &ls_scr_labs, &ls_scr_journal, &ls_scr_subghz, &ls_scr_mixrf,
};
#define N_SCREENS ((int)(sizeof(SCREENS) / sizeof(SCREENS[0])))

/* ------------------------------------------------------------ the grid -- */

static int s_grid_cols = 115, s_grid_rows = 27;

void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = s_grid_cols;
    if (rows) *rows = s_grid_rows;
    if (cw)   *cw = 10;
    if (ch)   *ch = 17;
}

void ls_tui_invalidate(void) { }
/* The corner padding the real ls_tui.c works out from the panel's
   pixels. These panes have square corners, so there is nothing to stand off. */
int ls_tui_corner_pad(int row) { (void)row; return 0; }

static void grid_for(tui_rect pane)
{
    const bool wide = pane.w > pane.h;
    s_grid_cols = wide ? 115 : 48;
    s_grid_rows = wide ? 27 : 66;
}

static const tui_rect PANES[] = {
    {  1,  2, 113, 24 },   /* landscape body            */
    {  1,  2,  46, 63 },   /* portrait body             */
    {  1,  2,  46, 30 },   /* portrait, split in half   */
    {  1,  2,  56, 24 },   /* landscape, split in half  */
    {  1,  2,  20,  6 },   /* far smaller than intended */
    {  1,  2,  10,  3 },   /* absurd                    */
    {  1,  2,   4,  2 },   /* degenerate but positive   */
};

extern void ls_scr_fm_show_page(int page);

static ls_act_status_t fm_test_select(const ls_args_t *args, ls_val_t *out)
{
    (void)out;
    fm_mode_t mode;
    if (!args || args->n != 1 || !fm_mode_parse(args->v[0].s, &mode))
        return LS_ACT_BADARG;
    FM.mode = mode;
    return LS_ACT_OK;
}

LS_CASE(fm_mode_buttons_and_keyboard_reach_every_receiver)
{
    fresh();
    const tui_rect pane = {1, 2, 46, 63};
    grid_for(pane);
    ls_action_register("fm.submode", "s", LS_CAP_TUNE, fm_test_select, "FM mode");
    const fm_mode_t modes[] = {FM_MODE_LISTEN, FM_MODE_WFM, FM_MODE_POCSAG,
                              FM_MODE_FLEX, FM_MODE_ACARS, FM_MODE_SCAN, FM_MODE_AM};
    FM.mode = FM_MODE_LISTEN;
    ls_scr_fm.enter();
    for (int i = 0; i < 7; ++i) {
        ls_scr_fm.draw(&g_sf, pane);
        int col = pane.x + pane.w * (i % 4) / 4 + pane.w / 8;
        int row = pane.y + (i / 4) * 5 + 2;
        LS_CHECK(ls_scr_fm.touch(col, row));
        LS_EQ_INT(FM.mode, modes[i]);
        ls_scr_fm.draw(&g_sf, pane);
        int x0 = pane.x + pane.w * (i % 4) / 4;
        int x1 = pane.x + pane.w * (i % 4 + 1) / 4;
        int y0 = pane.y + (i / 4) * 5;
        for (int y = y0; y < y0 + 5; ++y)
            for (int x = x0; x < x1; ++x)
                if (y == y0 || y == y0 + 4 || x == x0 || x == x1 - 1)
                    LS_EQ_INT(g_back[y * W + x].attr,
                              TUI_ATTR(TUI_CYAN, TUI_BLACK));
    }
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'm'));
    LS_EQ_INT(FM.mode, FM_MODE_LISTEN);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'm'));
    LS_EQ_INT(FM.mode, FM_MODE_WFM);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'n'));
    LS_EQ_INT(FM.mode, FM_MODE_LISTEN);
}

LS_CASE(fm_sweep_can_be_left_by_touch_and_remote_pager_selection)
{
    fresh();
    const tui_rect pane = {1, 2, 46, 63};
    grid_for(pane);
    ls_action_register("fm.submode", "s", LS_CAP_TUNE, fm_test_select, "FM mode");
    FM.mode = FM_MODE_SCAN;
    ls_scr_fm.enter();
    ls_scr_fm.draw(&g_sf, pane);
    LS_CHECK(ls_scr_fm.touch(pane.x + pane.w / 6, pane.y + pane.h - 4));
    LS_EQ_INT(FM.mode, FM_MODE_LISTEN);
    ls_scr_fm_show_page(2);
    LS_EQ_INT(FM.mode, FM_MODE_SCAN);
    ls_scr_fm_show_page(1);
    LS_EQ_INT(FM.mode, FM_MODE_POCSAG);
    ls_scr_fm.draw(&g_sf, pane);
    LS_CHECK(ls_scr_fm.touch(pane.x + pane.w * 3 / 8, pane.y + 1));
    LS_EQ_INT(FM.mode, FM_MODE_WFM);
}

static float s_fm_tuned_mhz;
static int32_t s_fm_tuned_hz;
static int s_fm_keypad_opens;

static ls_act_status_t fm_test_freq(const ls_args_t *args, ls_val_t *out)
{
    (void)out;
    s_fm_tuned_hz = args->v[0].i;
    s_fm_tuned_mhz = (float)s_fm_tuned_hz / 1000000.0f;
    return LS_ACT_OK;
}

static ls_act_status_t fm_test_keypad(const ls_args_t *args, ls_val_t *out)
{
    (void)args;
    (void)out;
    ++s_fm_keypad_opens;
    return LS_ACT_OK;
}

LS_CASE(fm_tune_split_steps_frequency_and_opens_keypad)
{
    fresh();
    const tui_rect pane = {1, 2, 46, 63};
    grid_for(pane);
    ls_action_register("fm.freq_hz", "i", LS_CAP_TUNE, fm_test_freq, "Frequency");
    ls_action_register("fm.tune", "", LS_CAP_TUNE, fm_test_keypad, "Tune");
    FM.mode = FM_MODE_LISTEN;
    FM.freq_hz = 100000000;
    s_fm_keypad_opens = 0;
    ls_scr_fm.enter();
    ls_scr_fm.draw(&g_sf, pane);
    const int row = pane.y + 11 + 19 + 3;
    LS_CHECK(ls_scr_fm.touch(pane.x + pane.w / 6, row));
    LS_CHECK(fabsf(s_fm_tuned_mhz - 99.9875f) < 0.0001f);
    LS_CHECK(ls_scr_fm.touch(pane.x + pane.w / 2, row));
    LS_EQ_INT(s_fm_keypad_opens, 1);
    LS_CHECK(ls_scr_fm.touch(pane.x + 5 * pane.w / 6, row));
    LS_CHECK(fabsf(s_fm_tuned_mhz - 100.0125f) < 0.0001f);
    FM.mode = FM_MODE_WFM;
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, ']'));
    LS_CHECK(fabsf(s_fm_tuned_mhz - 100.1f) < 0.0001f);
    FM.mode = FM_MODE_ACARS;
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, '['));
    LS_CHECK(fabsf(s_fm_tuned_mhz - 99.975f) < 0.0001f);
    FM.freq_hz = 24000000;
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, '['));
    LS_CHECK(s_fm_tuned_mhz == 24.0f);
    FM.freq_hz = 1766000000;
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, ']'));
    LS_CHECK(s_fm_tuned_mhz == 1766.0f);
    FM.mode = FM_MODE_LISTEN;
    FM.freq_hz = 154785000;
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, '['));
    LS_EQ_INT(s_fm_tuned_hz, 154772500);
    FM.freq_hz = (uint32_t)s_fm_tuned_hz;
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, ']'));
    LS_EQ_INT(s_fm_tuned_hz, 154785000);
}
#define N_PANES ((int)(sizeof(PANES) / sizeof(PANES[0])))

/* HOME is the app directory, so it draws the app table and nothing else.

   With no apps registered it paints an empty grid, which is not a state the
   firmware can reach - ls_app_count() is six before the TUI takes the screen
   - and testing a launcher against an empty launcher tests none of its
   layout. So the same screens under test are registered as their apps, which
   is also what the firmware does: one table, screen and identity together. */
static void apps_once(void)
{
    static bool done;
    if (done) return;
    done = true;

    static const ls_app_t APPS[N_SCREENS] = {
        { "set",  "SET",  "display, theme", LS_ICON_GEAR,  TUI_BLUE,
          LS_APP_EXTRA, &ls_scr_settings, NULL },
        { "diag", "DIAG", "health",   LS_ICON_CHIP,   TUI_WHITE,
          LS_APP_EXTRA, &ls_scr_diag, NULL },
        { "rec",  "REC",  "capture",  LS_ICON_RECORD, TUI_RED,
          LS_APP_EXTRA, &ls_scr_rec, NULL },
        { "home", "HOME", "directory", LS_ICON_SHARK, TUI_CYAN,
          LS_APP_MAIN, &ls_scr_home, NULL },
        { "fm",   "FM",   "analogue", LS_ICON_WAVE,   TUI_YELLOW,
          LS_APP_MAIN, &ls_scr_fm, NULL },
        { "adsb", "ADSB", "aircraft", LS_ICON_PLANE,  TUI_MAGENTA,
          LS_APP_MAIN, &ls_scr_adsb, NULL },
        { "labs", "LORA LABS", "experiments", LS_ICON_LABS, TUI_CYAN,
          LS_APP_EXTRA, &ls_scr_labs, NULL },
        { "journal", "JOURNAL", "notes", LS_ICON_JOURNAL, TUI_GREEN,
          LS_APP_EXTRA, &ls_scr_journal, NULL },
        { "subghz", "SUB-GHZ", "watch", LS_ICON_RECORD, TUI_GREEN,
          LS_APP_EXTRA, &ls_scr_subghz, NULL },
    };
    for (int i = 0; i < N_SCREENS; i++) ls_app_register(&APPS[i]);
}

static void draw_pane(const ls_tui_screen_t *scr, tui_rect pane)
{
    apps_once();
    grid_for(pane);
    scr->draw(&g_sf, pane);
}

/* ---------------------------------------------------------------- cases -- */

static void seed_health(void)
{
    seed_fm();
    seed_adsb();
    p25_health_init();

    p25_health_raw_t raw;
    memset(&raw, 0, sizeof(raw));
    /* Non-zero counters put every "red once it moves" branch on. */
    raw.nid_valid = 1200;   raw.nid_invalid = 7;
    raw.tsbk_valid = 340;   raw.tsbk_invalid = 2;
    raw.usb_read_errors = 3;
    raw.usb_dropped_bytes = 8192;
    raw.audio_drops = 1;    raw.audio_underruns = 2;
    raw.ring_fill = 900;    raw.ring_size = 1024;   /* deliberately near full */
    raw.rf_level_permille = 812;
    raw.cpu_valid = true;   raw.cpu_core0_pct = 94; raw.cpu_core1_pct = 40;
    /* unhandled_total and unhandled_distinct are derived by the snapshot from
       the opcode array, not published, so they are set by publishing opcodes. */
    raw.unhandled[0] = 0x3A;
    raw.unhandled[1] = 0x5C;
    p25_health_publish(&raw, 1000);

    radio_health_init(NULL);
    radio_health_tick();
}

LS_CASE(no_screen_writes_outside_the_rect_it_was_handed)
{
    seed_health();
    for (int s = 0; s < N_SCREENS; s++)
        for (int p = 0; p < N_PANES; p++) {
            if (SCREENS[s]->enter) SCREENS[s]->enter();
            fresh();
            draw_pane(SCREENS[s], PANES[p]);
            LS_CHECK_MSG(escaped(PANES[p]) == 0,
                         "%s escaped its rect on a %dx%d pane (%d cells)",
                         SCREENS[s]->name, PANES[p].w, PANES[p].h,
                         escaped(PANES[p]));
        }
}

/* Count cells inside `r` that the screen actually changed. */
static int painted(tui_rect r)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h && y < H; y++)
        for (int x = r.x; x < r.x + r.w && x < W; x++)
            if (g_back[y * W + x].ch != SENTINEL) n++;
    return n;
}

LS_CASE(each_screen_actually_paints_its_pane)
{

    seed_health();
    static const int REAL[] = { 0, 1 };   /* landscape body, portrait body */

    for (int s = 0; s < N_SCREENS; s++)
        for (unsigned i = 0; i < sizeof(REAL) / sizeof(REAL[0]); i++) {
            tui_rect p = PANES[REAL[i]];
            if (SCREENS[s]->enter) SCREENS[s]->enter();
            fresh();
            draw_pane(SCREENS[s], p);
            int cells = painted(p);
            LS_CHECK_MSG(cells > 2 * (p.w + p.h),
                         "%s painted only %d of %d cells on a %dx%d pane",
                         SCREENS[s]->name, cells, p.w * p.h, p.w, p.h);
        }
}

LS_CASE(calibration_guide_stays_inside_portrait_landscape_and_split_panes)
{
    extern void lssim_field_calibration(int step,int hold,bool failed);
    for(int step=0;step<=7;step++)for(int p=0;p<N_PANES;p++) {
        ls_scr_labs.enter();
        fresh();draw_pane(&ls_scr_labs,PANES[p]);ls_scr_labs.key(LS_TK_CHAR,'k');
        lssim_field_calibration(step<6?step:6,11,step==7);
        fresh();draw_pane(&ls_scr_labs,PANES[p]);
        LS_CHECK_MSG(escaped(PANES[p])==0,"calibration step %d escaped %dx%d",step,PANES[p].w,PANES[p].h);
        ls_scr_labs.leave();
    }
}

/* One row of the grid as a string, so an assertion can name what should be on
   the glass instead of just counting painted cells. */
static void row_text(int y, char *out, size_t n)
{
    size_t k = 0;
    for (int x = 0; x < W && k + 1 < n; x++) out[k++] = g_back[y * W + x].ch;
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

LS_CASE(home_landscape_tiles_keep_their_labels)
{
    apps_once();
    if (ls_scr_home.enter) ls_scr_home.enter();
    fresh();
    draw_pane(&ls_scr_home, PANES[0]);   /* landscape body, 113x24 */

    static const char *const NAMES[] = { "SET", "DIAG", "REC", "FM", "ADSB", "LORA LABS", "JOURNAL" };
    static const char GROUP[] = {'s','s','f','r','r','r','f'};
    for (unsigned i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        ls_scr_home.key(LS_TK_CHAR, GROUP[i]);
        fresh();
        draw_pane(&ls_scr_home, PANES[0]);
        bool found = false;
        char line[W + 1];
        for (int y = 0; y < H && !found; y++) {
            row_text(y, line, sizeof(line));
            if (strstr(line, NAMES[i])) found = true;
        }
        LS_CHECK_MSG(found, "%s's tile has no visible name in landscape",
                     NAMES[i]);
    }
}

LS_CASE(a_landscape_tile_shows_its_whole_icon_not_a_crop)
{

    static const ls_tile_t TILES[11] = {
        { "P25",  "trunking",  LS_ICON_TOWER, TUI_GREEN,   false },
        { "FM",   "analogue",  LS_ICON_WAVE,  TUI_YELLOW,  false },
        { "ADSB", "aircraft",  LS_ICON_PLANE, TUI_MAGENTA, false },
        { "REC",  "capture",   LS_ICON_RECORD, TUI_RED,    false },
        { "DIAG", "health",    LS_ICON_CHIP,  TUI_WHITE,   false },
        { "SET",  "display",   LS_ICON_GEAR,  TUI_BLUE,    false },
        { "GPS",  "position",  LS_ICON_SAT,   TUI_YELLOW,  false },
        { "MAP",  "charts",    LS_ICON_MAP,   TUI_GREEN,   false },
        { "FALLS","spectrum",  LS_ICON_FALLS, TUI_BLUE,    false },
        { "MESH", "lora",      LS_ICON_MESH,  TUI_CYAN,    false },
        { "RADIOS","power",    LS_ICON_POWER, TUI_RED,     false },
    };

    /* The landscape body, minus the receiver panel on the right. */
    const tui_rect area = tui_rect_make(1, 2, 80, 24);

    fresh();
    ls_tile_grid(&g_sf, area, TILES, 11, -1);

    int cols = 0, rows = 0;
    ls_tile_shape(&cols, &rows);
    LS_CHECK_MSG(rows >= 1 && cols >= 1, "the grid drew nothing at all");
    if (rows < 1) return;

    /* Two borders, the art, and a row for the name. */
    const int per_row = area.h / rows;
    const int need = 2 + LS_ICON_ROWS + 1;
    LS_CHECK_MSG(per_row >= need,
                 "%d rows of tiles in %d gives each one %d rows, and a whole "
                 "icon with its name needs %d - the art is being cropped",
                 rows, area.h, per_row, need);

    /* An icon has a width as well, and a grid that solved the height by
       making the tiles too narrow would have moved the fault, not fixed it. */
    const int per_col = area.w / cols;
    LS_CHECK_MSG(per_col >= LS_ICON_COLS + 3,
                 "%d columns of tiles in %d gives each one %d, and the art is "
                 "%d wide", cols, area.w, per_col, LS_ICON_COLS);
}

LS_CASE(adsb_age_reflects_last_seen_not_a_constant_zero)
{
    apps_once();
    adsb_state_init();
    adsb_aircraft_t *a = adsb_state_find_or_create(0xABCDEFu);
    LS_CHECK(a != NULL);
    a->altitude = 30000;
    a->velocity = 0;
    a->heading = 0;

    const int64_t t0 = esp_timer_get_time();

    a->last_seen_us = t0;
    fresh();
    draw_pane(&ls_scr_adsb, PANES[0]);   /* landscape body, 113x24 */
    static tui_cell just_seen[W * H];
    memcpy(just_seen, g_back, sizeof(just_seen));

    a->last_seen_us = t0 - 30 * 1000000LL;
    fresh();
    draw_pane(&ls_scr_adsb, PANES[0]);

    LS_CHECK_MSG(memcmp(just_seen, g_back, sizeof(just_seen)) != 0,
                 "the row is identical whether the contact was seen just now "
                 "or 30 seconds ago - AGE regressed to a constant");
}

/* The hint has promised "ENTER details" since before there was a
   details page behind it. Pinned at the grid level: ENTER must swap the
   AIRCRAFT list for a page naming the selected contact's own ICAO and
   callsign, and ESC must hand the list back. */
LS_CASE(adsb_enter_opens_detail_esc_returns_to_the_list)
{
    apps_once();
    adsb_state_init();
    adsb_aircraft_t *a = adsb_state_find_or_create(0x123456u);
    LS_CHECK(a != NULL);
    snprintf(a->callsign, sizeof(a->callsign), "N42Q");
    a->altitude = 5500;
    a->last_seen_us = esp_timer_get_time();
    adsb_select_set_icao(a->icao);

    if (ls_scr_adsb.enter) ls_scr_adsb.enter();
    fresh();
    draw_pane(&ls_scr_adsb, PANES[0]);
    char line[W + 1];
    bool saw_list = false;
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        if (strstr(line, "AIRCRAFT")) saw_list = true;
    }
    LS_CHECK_MSG(saw_list, "expected the list title before ENTER");

    LS_CHECK(ls_scr_adsb.key(LS_TK_ENTER, 0));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[0]);
    bool saw_icao = false, saw_callsign = false;
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        if (strstr(line, "123456")) saw_icao = true;
        if (strstr(line, "N42Q")) saw_callsign = true;
    }
    LS_CHECK_MSG(saw_icao, "ENTER did not open a page naming the contact");
    LS_CHECK_MSG(saw_callsign, "the detail page did not show the callsign");

    LS_CHECK(ls_scr_adsb.key(LS_TK_ESC, 0));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[0]);
    saw_list = false;
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        if (strstr(line, "AIRCRAFT")) saw_list = true;
    }
    LS_CHECK_MSG(saw_list, "ESC did not return to the list");

    if (ls_scr_adsb.leave) ls_scr_adsb.leave();
}

LS_CASE(adsb_radar_does_not_merge_two_close_labels)
{
    apps_once();
    adsb_state_init();
    adsb_aircraft_t *a1 = adsb_state_find_or_create(0x100001u);
    adsb_aircraft_t *a2 = adsb_state_find_or_create(0x100002u);
    LS_CHECK(a1 != NULL && a2 != NULL);
    snprintf(a1->callsign, sizeof(a1->callsign), "AAA111");
    snprintf(a2->callsign, sizeof(a2->callsign), "BB22");
    a1->pos_valid = a2->pos_valid = true;
    a1->lat = a2->lat = 43.50f;
    a1->lon = a2->lon = -71.50f;
    a1->altitude = a2->altitude = 10000;
    a1->last_seen_us = a2->last_seen_us = esp_timer_get_time();

    if (ls_scr_adsb.enter) ls_scr_adsb.enter();
    fresh();
    draw_pane(&ls_scr_adsb, PANES[0]);   /* landscape body - the radar half draws */

    /* Both callsigns legitimately appear in the AIRCRAFT list beside the
       radar - "BB22" in the CALLSIGN column is correct there and would be a
       false failure if the whole row were searched. Only the radar's own
       columns, to the right of the split draw() uses, say anything about
       whether the collision check worked. */
    const int radar_x = PANES[0].x + (PANES[0].w - 38);
    char line[W + 1];
    bool saw_first = false, saw_second = false, saw_merge = false;
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        const int n = (int)strlen(line);
        const char *radar_part = radar_x < n ? line + radar_x : "";
        if (strstr(radar_part, "AAA111")) saw_first = true;
        if (strstr(radar_part, "BB22")) saw_second = true;
        /* The exact corruption the old code produced: the second label
           overwriting the front of the first and leaving its tail intact.
           Checked over the whole row since a corrupted string is not
           something the list would ever legitimately contain either. */
        if (strstr(line, "BB2211")) saw_merge = true;
    }
    LS_CHECK_MSG(saw_first, "the first contact's label should still be drawn");
    LS_CHECK_MSG(!saw_second, "a stacked contact's label should be skipped, "
                 "not drawn over the one already there");
    LS_CHECK_MSG(!saw_merge, "the two labels merged into one corrupted string");

    if (ls_scr_adsb.leave) ls_scr_adsb.leave();
}

LS_CASE(adsb_portrait_has_a_radar_under_a_list_that_holds_all_sixteen)
{
    apps_once();
    adsb_state_init();
    for (uint32_t i = 0; i < ADSB_MAX_TRACKED; i++) {
        adsb_aircraft_t *a = adsb_state_find_or_create(0xB00000u + i);
        LS_CHECK(a != NULL);
        if (!a) continue;
        a->altitude = 1000 + 1000 * (int)i;
        a->last_seen_us = esp_timer_get_time();
    }
    adsb_select_set_icao(0xB00000u + ADSB_MAX_TRACKED - 1);

    if (ls_scr_adsb.enter) ls_scr_adsb.enter();
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);       /* portrait body, 46x63 */

    int list_y = -1, radar_y = -1, rows = 0;
    char line[W + 1];
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        if (list_y < 0 && strstr(line, "AIRCRAFT")) list_y = y;
        if (radar_y < 0 && strstr(line, "RADAR")) radar_y = y;
        for (uint32_t i = 0; i < ADSB_MAX_TRACKED; i++) {
            char hex[8];
            snprintf(hex, sizeof(hex), "%06lX", (unsigned long)(0xB00000u + i));
            if (strstr(line, hex)) { rows++; break; }
        }
    }
    LS_CHECK_MSG(list_y >= 0, "no AIRCRAFT list on the portrait body");
    LS_CHECK_MSG(radar_y > list_y,
                 "portrait should draw a RADAR under the AIRCRAFT list "
                 "(list at row %d, radar at row %d)", list_y, radar_y);
    LS_CHECK_MSG(rows == ADSB_MAX_TRACKED,
                 "%d of %d contacts on screen - portrait must not scroll",
                 rows, ADSB_MAX_TRACKED);
    LS_EQ_INT(0, escaped(PANES[1]));

    if (ls_scr_adsb.leave) ls_scr_adsb.leave();
}

LS_CASE(adsb_radar_tap_picks_the_nearest_contact_and_a_second_opens_it)
{
    apps_once();
    adsb_state_init();
    adsb_aircraft_t *n = adsb_state_find_or_create(0xC00001u);
    adsb_aircraft_t *s = adsb_state_find_or_create(0xC00002u);
    LS_CHECK(n != NULL && s != NULL);
    if (!n || !s) return;
    snprintf(n->callsign, sizeof(n->callsign), "NORTH1");
    snprintf(s->callsign, sizeof(s->callsign), "SOUTH2");
    n->pos_valid = s->pos_valid = true;
    n->lat = 43.60f;                          /* home is 43.4445, -71.6473 */
    s->lat = 43.30f;
    n->lon = s->lon = -71.6473f;
    n->altitude = s->altitude = 3000;
    n->last_seen_us = s->last_seen_us = esp_timer_get_time();
    adsb_select_set_icao(n->icao);

    if (ls_scr_adsb.enter) ls_scr_adsb.enter();
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);       /* portrait body, 46x63 */

    /* Where the radar drew SOUTH2: a label starts one cell right of its dot.
       Only rows below the radar's title - the list above names both. */
    int radar_y = -1, dot_x = -1, dot_y = -1;
    char line[W + 1];
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        if (radar_y < 0) {
            if (strstr(line, "RADAR")) radar_y = y;
            continue;
        }
        const char *p = strstr(line, "SOUTH2");
        if (p) { dot_x = (int)(p - line) - 1; dot_y = y; break; }
    }
    LS_CHECK_MSG(dot_y > radar_y && dot_x >= 0,
                 "SOUTH2 was not drawn on the portrait radar");
    LS_CHECK(ls_scr_adsb.touch != NULL);
    if (dot_y < 0 || dot_x < 0 || !ls_scr_adsb.touch) return;

    LS_CHECK(ls_scr_adsb.touch(dot_x + 2, dot_y + 1));
    LS_EQ_UINT(0xC00002u, adsb_select_get_icao());

    LS_CHECK(ls_scr_adsb.touch(dot_x, dot_y));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);
    bool saw_page = false, saw_list = false;
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        if (strstr(line, "C00002")) saw_page = true;
        if (strstr(line, "AIRCRAFT")) saw_list = true;
    }
    LS_CHECK_MSG(saw_page && !saw_list,
                 "a second tap on the picked contact should open its page");

    /* A row tap opens that aircraft directly, regardless of selection. */
    LS_CHECK(ls_scr_adsb.key(LS_TK_ESC, 0));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + 2, PANES[1].y + 2));
    LS_EQ_UINT(0xC00001u, adsb_select_get_icao());

    if (ls_scr_adsb.leave) ls_scr_adsb.leave();
}

static int find_row_text(const char *text)
{
    char line[W + 1];
    for (int y = 0; y < H; y++) {
        row_text(y, line, sizeof(line));
        if (strstr(line, text)) return y;
    }
    return -1;
}

LS_CASE(adsb_visible_touch_controls_open_step_and_return)
{
    seed_adsb();
    ls_scr_adsb.leave();
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);
    const int controls = find_row_text("ENTER details");
    LS_CHECK(controls >= 0);
    if (controls < 0) return;
    const uint32_t first = adsb_select_get_icao();
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + PANES[1].w - 3, controls));
    LS_CHECK(adsb_select_get_icao() != first);
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + PANES[1].w / 2, controls));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);
    LS_CHECK(find_row_text("CALLSIGN") >= 0);
    LS_CHECK(find_row_text("ENTER back") >= 0);
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + PANES[1].w / 2, controls));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);
    LS_CHECK(find_row_text("AIRCRAFT") >= 0);
    ls_scr_adsb.leave();
}

LS_CASE(rec_history_uses_elapsed_time_and_clears_missing_data)
{
    const rec_hub_status_t saved = s_rec;
    s_rec.receiver_streaming = true;
    s_rec.phase = REC_ARMED;
    s_rec.bytes_sec = 0;
    ls_shim_time_set(1000000);
    ls_scr_rec.enter();
    for (int i = 0; i < 11; i++) {
        fresh();
        draw_pane(&ls_scr_rec, PANES[1]);
        ls_shim_time_advance(320000);
    }
    LS_CHECK(find_row_text("3.2s ago") >= 0);
    LS_CHECK(find_row_text("Armed: waiting for trigger") >= 0);
    LS_CHECK(find_row_text("Waiting for receiver samples") >= 0);
    ls_shim_time_advance(3000000);
    fresh();
    draw_pane(&ls_scr_rec, PANES[1]);
    LS_CHECK(find_row_text("0.0s ago") >= 0);
    s_rec.receiver_streaming = false;
    fresh();
    draw_pane(&ls_scr_rec, PANES[1]);
    LS_CHECK(find_row_text("RX stopped: no live samples") >= 0);
    s_rec = saved;
    ls_scr_rec.enter();
}

LS_CASE(rec_level_does_not_repaint_between_samples)
{
    const rec_hub_status_t saved = s_rec;
    static tui_cell before[W * H];
    for (int pane = 0; pane < 2; pane++) {
        s_rec.receiver_streaming = true;
        s_rec.phase = REC_IDLE;
        s_rec.mag_now = 35;
        s_rec.mag_thresh = 30;
        ls_shim_time_set(1000000);
        ls_scr_rec.enter();
        fresh();
        draw_pane(&ls_scr_rec, PANES[pane]);
        memcpy(before, g_back, sizeof(before));
        s_rec.mag_now = 70;
        s_rec.mag_thresh = 100;
        ls_shim_time_advance(10000);
        fresh();
        draw_pane(&ls_scr_rec, PANES[pane]);
        LS_CHECK_MSG(memcmp(before, g_back, sizeof(before)) == 0,
                     "live threshold repainted history before a sample was due");
        ls_shim_time_advance(320000);
        fresh();
        draw_pane(&ls_scr_rec, PANES[pane]);
        LS_CHECK(find_row_text("level 70  trigger 100") >= 0);
    }
    s_rec = saved;
    ls_scr_rec.enter();
}

LS_CASE(rec_level_preserves_colors_and_rescales_after_a_quiet_window)
{
    const rec_hub_status_t saved = s_rec;
    const tui_rect pane = PANES[1];
    s_rec.receiver_streaming = true;
    s_rec.phase = REC_IDLE;
    s_rec.mag_now = 35;
    s_rec.mag_thresh = 30;
    ls_shim_time_set(1000000);
    ls_scr_rec.enter();
    fresh();
    draw_pane(&ls_scr_rec, pane);
    const int x = pane.x + pane.w - 3;
    const int header = find_row_text("LEVEL");
    const int footer = find_row_text("level 35");
    int point_y = -1;
    for (int y = header + 1; y < footer; y++)
        if (g_back[y * W + x].attr == TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK))
            point_y = y;
    LS_CHECK(point_y >= 0);
    s_rec.mag_thresh = 50;
    ls_shim_time_advance(320000);
    fresh();
    draw_pane(&ls_scr_rec, pane);
    if (point_y >= 0) {
        LS_CHECK(g_back[point_y * W + x - 1].attr ==
                 TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
        LS_CHECK(g_back[point_y * W + x].attr ==
                 TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }
    s_rec.mag_now = 200;
    ls_shim_time_advance(320000);
    fresh();
    draw_pane(&ls_scr_rec, pane);
    LS_CHECK(find_row_text("scale 0-256 raw") >= 0);
    s_rec.mag_now = 15;
    s_rec.mag_thresh = 8;
    for (int i = 0; i < 10; i++) {
        ls_shim_time_advance(320000);
        fresh();
        draw_pane(&ls_scr_rec, pane);
    }
    LS_CHECK(find_row_text("scale 0-256 raw") >= 0);
    for (int i = 0; i < 130; i++) {
        ls_shim_time_advance(320000);
        fresh();
        draw_pane(&ls_scr_rec, pane);
    }
    LS_CHECK(find_row_text("scale 0-64 raw") >= 0);
    s_rec.freq_hz++;
    ls_shim_time_advance(320000);
    fresh();
    draw_pane(&ls_scr_rec, pane);
    LS_CHECK(find_row_text("scale 0-64 raw") >= 0);
    s_rec = saved;
    ls_scr_rec.enter();
}

LS_CASE(drawing_twice_from_the_same_state_gives_the_same_grid)
{
    seed_health();
    static tui_cell first[W * H];
    for (int s = 0; s < N_SCREENS; s++)
        for (int p = 0; p < N_PANES; p++) {
            fresh();
            draw_pane(SCREENS[s], PANES[p]);
            memcpy(first, g_back, sizeof(first));

            fresh();
            draw_pane(SCREENS[s], PANES[p]);
            LS_CHECK_MSG(memcmp(first, g_back, sizeof(first)) == 0,
                         "%s is not idempotent on a %dx%d pane",
                         SCREENS[s]->name, PANES[p].w, PANES[p].h);
        }
}

LS_CASE(screens_pass_through_the_keys_they_do_not_own)
{
    /* A screen that swallows F1 breaks app switching from that screen, and
       nothing else in the system can tell. */
    static const ls_tk_t GLOBAL[] = {
        LS_TK_F1, LS_TK_F2, LS_TK_F5, LS_TK_F9, LS_TK_F10, LS_TK_F11,
    };
    for (int s = 0; s < N_SCREENS; s++) {
        if (!SCREENS[s]->key) continue;
        for (unsigned i = 0; i < sizeof(GLOBAL) / sizeof(GLOBAL[0]); i++)
            LS_CHECK_MSG(!SCREENS[s]->key(GLOBAL[i], 0),
                         "%s consumed a global key", SCREENS[s]->name);
    }
}

LS_CASE(every_key_is_survivable_on_every_pane)
{
    seed_health();

    for (int s = 0; s < N_SCREENS; s++) {
        if (!SCREENS[s]->key) continue;
        for (int k = LS_TK_NONE; k <= LS_TK_F11; k++) {
            SCREENS[s]->key((ls_tk_t)k, 'x');
            fresh();
            draw_pane(SCREENS[s], PANES[0]);
            LS_CHECK_MSG(escaped(PANES[0]) == 0,
                         "%s escaped after key %d", SCREENS[s]->name, k);
        }
    }
}

LS_CASE(settings_enter_cycles_a_value_and_wraps)
{
    /* Brightness is the one with a floor that is not zero: it wraps to 5 so a
       stray press cannot leave the panel unreadable. */
    s_bright = 100;
    ls_scr_settings.key(LS_TK_ENTER, 0);
    LS_CHECK_MSG(s_bright >= 5, "brightness wrapped to %d", s_bright);

    int seen_low = 0;
    for (int i = 0; i < 40; i++) {
        ls_scr_settings.key(LS_TK_ENTER, 0);
        if (s_bright < 5) seen_low = 1;
    }
    LS_EQ_INT(0, seen_low);
}

LS_CASE(settings_selection_moves_and_stays_in_range)
{
    /* Walk far past both ends. An index that escapes shows up as a draw that
       reads a label off the end of the table. */
    for (int i = 0; i < 50; i++) ls_scr_settings.key(LS_TK_UP, 0);
    fresh();
    draw_pane(&ls_scr_settings, PANES[0]);
    LS_EQ_INT(0, escaped(PANES[0]));

    for (int i = 0; i < 50; i++) ls_scr_settings.key(LS_TK_DOWN, 0);
    fresh();
    draw_pane(&ls_scr_settings, PANES[0]);
    LS_EQ_INT(0, escaped(PANES[0]));
}

static bool find_text(const char *text, int *col, int *row)
{
    const int n = (int)strlen(text);
    for (int y = 0; y < H; y++)
        for (int x = 0; x + n <= W; x++) {
            int k = 0;
            while (k < n && g_back[y * W + x + k].ch == text[k]) k++;
            if (k == n) { *col = x; *row = y; return true; }
        }
    return false;
}

LS_CASE(pager_touch_and_arrow_navigation_work_in_list_and_detail)
{
    for (int orientation = 0; orientation < 2; ++orientation) {
        seed_fm();
        FM.mode = FM_MODE_POCSAG;
        FM.page_count = 3;
        FM.page_head = 3;
        ls_scr_fm.enter();
        for (int i = 0; i < FM_PAGE_LOG_MAX; ++i)
            ls_scr_fm.key(LS_TK_UP, 0);
        int x, y;
        fresh();
        draw_pane(&ls_scr_fm, PANES[orientation]);
        LS_CHECK(find_text("OPEN 1/3", &x, &y));
        LS_CHECK(find_text("DOWN", &x, &y));
        LS_CHECK(ls_scr_fm.touch(x, y));
        fresh();
        draw_pane(&ls_scr_fm, PANES[orientation]);
        LS_CHECK(find_text("OPEN 2/3", &x, &y));
        LS_CHECK(ls_scr_fm.touch(x, y));
        fresh();
        draw_pane(&ls_scr_fm, PANES[orientation]);
        LS_CHECK(find_text("LIST 2/3", &x, &y));
        LS_CHECK(find_text("DOWN", &x, &y));
        LS_CHECK(ls_scr_fm.touch(x, y));
        LS_CHECK(ls_scr_fm.key(LS_TK_DOWN, 0));
        fresh();
        draw_pane(&ls_scr_fm, PANES[orientation]);
        LS_CHECK(find_text("LIST 3/3", &x, &y));
        LS_CHECK(find_text("UP", &x, &y));
        LS_CHECK(ls_scr_fm.touch(x, y));
        LS_CHECK(ls_scr_fm.key(LS_TK_UP, 0));
        fresh();
        draw_pane(&ls_scr_fm, PANES[orientation]);
        LS_CHECK(find_text("LIST 1/3", &x, &y));
        LS_CHECK(ls_scr_fm.touch(x, y));
        fresh();
        draw_pane(&ls_scr_fm, PANES[orientation]);
        LS_CHECK(find_text("OPEN 1/3", &x, &y));
    }
}

LS_CASE(every_setting_has_a_box_in_both_postures)
{
    /* Daylight made eleven, and six boxes deep at the three-row
       minimum does not fit a short landscape body; the draw stops at the
       first box that would cross the edge, and the one it would have
       dropped was Daylight's. Every label must be on the glass in both
       postures and in half a landscape pane, which is the tightest case. */
    static const char *const LABELS[] = {
        "Brightness", "Auto dim", "Dim after", "Volume", "Theme", "Daylight",
        "Font", "Boot sound", "Alert sound", "Vibrate", "USB autoreboot",
    };
    static const int PANE_IDX[] = { 0, 1, 3 };
    s_daylight = false;
    for (unsigned p = 0; p < sizeof(PANE_IDX) / sizeof(PANE_IDX[0]); p++) {
        const tui_rect pane = PANES[PANE_IDX[p]];
        fresh();
        draw_pane(&ls_scr_settings, pane);
        LS_EQ_INT(0, escaped(pane));
        for (unsigned i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++) {
            int c, r;
            LS_CHECK_MSG(find_text(LABELS[i], &c, &r),
                         "no '%s' box on a %dx%d pane", LABELS[i], pane.w, pane.h);
        }
    }
}

LS_CASE(the_daylight_box_turns_it_on_and_off_and_keeps_the_theme)
{
    /* One press on, one press off, applied and stored each time,
       and the theme underneath never touched - in both postures. */
    static const int PANE_IDX[] = { 0, 1 };
    for (unsigned p = 0; p < sizeof(PANE_IDX) / sizeof(PANE_IDX[0]); p++) {
        const tui_rect pane = PANES[PANE_IDX[p]];
        s_active = ls_tui_theme_at(2);
        s_daylight = false;
        s_daylight_stored = false;

        fresh();
        draw_pane(&ls_scr_settings, pane);
        int c, r;
        LS_CHECK(find_text("Daylight", &c, &r));
        LS_CHECK(ls_scr_settings.touch(c, r));
        LS_CHECK_MSG(s_daylight, "the Daylight box did not turn it on");
        LS_CHECK_MSG(s_daylight_stored, "Daylight went on and was not stored");
        LS_CHECK(ls_tui_get_theme() == ls_tui_theme_at(2));
        LS_CHECK(ls_tui_active_theme() == &ls_theme_daylight);

        fresh();
        draw_pane(&ls_scr_settings, pane);
        LS_CHECK(ls_scr_settings.touch(c, r));
        LS_CHECK_MSG(!s_daylight, "the Daylight box did not turn it off");
        LS_CHECK_MSG(!s_daylight_stored, "Daylight went off and was not stored");
        LS_CHECK_MSG(ls_tui_active_theme() == ls_tui_theme_at(2),
                     "Daylight off did not give back the theme underneath");
    }
    s_active = NULL;
}

LS_CASE(the_theme_box_under_daylight_steps_the_theme_and_keeps_daylight)
{

    s_active = ls_tui_theme_at(0);
    s_daylight = true;
    s_theme_stored = -1;

    fresh();
    draw_pane(&ls_scr_settings, PANES[1]);
    int c, r;
    LS_CHECK(find_text("Theme", &c, &r));
    LS_CHECK(ls_scr_settings.touch(c, r));

    LS_CHECK_MSG(s_daylight, "pressing Theme under Daylight turned Daylight off");
    LS_CHECK(ls_tui_active_theme() == &ls_theme_daylight);
    LS_CHECK_MSG(ls_tui_get_theme() == ls_tui_theme_at(1),
                 "the theme under Daylight did not step");
    LS_EQ_INT(1, s_theme_stored);

    /* Not silent: the box's own value changed to name what comes back. */
    fresh();
    draw_pane(&ls_scr_settings, PANES[1]);
    char want[48];
    snprintf(want, sizeof(want), "%s, after Daylight", ls_tui_theme_at(1)->name);
    int vc, vr;
    LS_CHECK_MSG(find_text(want, &vc, &vr), "the Theme box does not read '%s'", want);

    /* And turning Daylight off afterwards draws the theme that was stepped to. */
    s_daylight = false;
    LS_CHECK(ls_tui_active_theme() == ls_tui_theme_at(1));
    s_active = NULL;
}

/* A screen that overwrites its own frame. */

static int border_breaks(tui_rect pane, int *out_x, int *out_y, char *out_ch)
{
    int breaks = 0;
    for (int side = 0; side < 2; side++) {
        const int x = side ? pane.x + pane.w - 1 : pane.x;
        if (x < 0 || x >= W) continue;

        int top = -1;
        for (int y = pane.y; y < pane.y + pane.h && y < H; y++) {
            const char c = g_back[y * W + x].ch;
            if (c == '+') {
                if (top >= 0) {
                    /* Two corners with only blank between them are the
                       bottom of one box and the top of the next, which is
                       how a stack of tiles looks and is not damage. A run
                       that is neither all side nor all gap has had
                       something written into it. */
                    int side = 0, gap = 0;
                    for (int j = top + 1; j < y; j++) {
                        const char c = g_back[j * W + x].ch;
                        if (c == '|') side++;
                        else if (c == ' ' || c == SENTINEL) gap++;
                    }
                    const int span = y - top - 1;
                    if (span > 0 && side != span && gap != span) {
                        for (int j = top + 1; j < y; j++) {
                            const char c = g_back[j * W + x].ch;
                            if (c == '|') continue;
                            breaks++;
                            if (out_x) { *out_x = x; *out_y = j;
                                         *out_ch = c; out_x = NULL; }
                        }
                    }
                }
                top = y;
            }
        }
    }
    return breaks;
}

LS_CASE(no_screen_eats_its_own_border_at_either_real_width)
{
    seed_health();
    static const int REAL[] = { 0, 1 };   /* landscape body, portrait body */

    for (int s = 0; s < N_SCREENS; s++)
        for (unsigned i = 0; i < sizeof(REAL) / sizeof(REAL[0]); i++) {
            const tui_rect p = PANES[REAL[i]];
            if (SCREENS[s]->enter) SCREENS[s]->enter();
            fresh();
            draw_pane(SCREENS[s], p);
            int bx = -1, by = -1; char bc = 0;
            const int n = border_breaks(p, &bx, &by, &bc);
            LS_CHECK_MSG(n == 0,
                         "%s writes over its frame %d times on a %dx%d pane, "
                         "first at col %d row %d with '%c'",
                         SCREENS[s]->name, n, p.w, p.h, bx, by,
                         bc >= 0x20 && bc < 0x7F ? bc : '?');
        }
}

/* DIAG joins them, because it now hit-tests its own targets. Its
   blocks are in the middle third of the pane, which is exactly where the
   router's fallback means ENTER - and this page now has four things ENTER
   could plausibly do. */
static const ls_tui_screen_t *const CLAIMERS[] = {
    &ls_scr_settings, &ls_scr_home, &ls_scr_diag,
};
#define N_CLAIMERS ((int)(sizeof(CLAIMERS) / sizeof(CLAIMERS[0])))

LS_CASE(a_screen_that_hit_tests_its_own_targets_claims_every_tap_in_its_pane)
{
    seed_health();

    for (int c = 0; c < N_CLAIMERS; c++) {
        const ls_tui_screen_t *scr = CLAIMERS[c];
        LS_CHECK_MSG(scr->touch != NULL, "%s has no touch handler", scr->name);
        if (!scr->touch) continue;

        for (int p = 0; p < N_PANES; p++) {
            const tui_rect pane = PANES[p];
            if (pane.w < 8 || pane.h < 6) continue;   /* nothing is drawn */

            if (scr->enter) scr->enter();
            fresh();
            draw_pane(scr, pane);

            int refused = 0, first_x = -1, first_y = -1;
            for (int y = pane.y; y < pane.y + pane.h; y++)
                for (int x = pane.x; x < pane.x + pane.w; x++) {
                    if (scr->touch(x, y)) continue;
                    if (first_x < 0) { first_x = x; first_y = y; }
                    refused++;
                }

            LS_CHECK_MSG(refused == 0,
                         "%s let %d of %d taps fall through to the router on a "
                         "%dx%d pane, first at (%d,%d)",
                         scr->name, refused, pane.w * pane.h,
                         pane.w, pane.h, first_x, first_y);
        }
    }
}

/* --------------------------------------------- DIAG detail -- */

/* Somewhere on the pane, as text. The detail pages are lists of named rows,
   so what is worth asserting is that a named row IS there - a co-ordinate
   would pin a layout that is free to move. */
static bool diag_has(tui_rect pane, const char *needle)
{
    for (int y = pane.y; y < pane.y + pane.h && y < H; y++) {
        char line[W + 1];
        row_text(y, line, sizeof(line));
        if (strstr(line, needle)) return true;
    }
    return false;
}

static const tui_rect DIAG_PANE = { 1, 4, 52, 66 };

static void diag_fresh(void)
{
    /* ESC twice: once to close whatever a previous case left open, and the
       second is harmless - the screen hands an unclaimed ESC back. */
    ls_scr_diag.key(LS_TK_ESC, 0);
    ls_scr_diag.key(LS_TK_ESC, 0);
    if (ls_scr_diag.enter) ls_scr_diag.enter();
    fresh();
    draw_pane(&ls_scr_diag, DIAG_PANE);
}

LS_CASE(diag_opens_the_detail_behind_a_block_and_esc_closes_it)
{

    seed_health();
    imu_fitted(true);
    diag_fresh();

    /* The list is up: the sensor block's one-line form is on it. */
    LS_CHECK_MSG(diag_has(DIAG_PANE, "imu icm20948"),
                 "the sensor list is not on the screen to open");
    LS_CHECK_MSG(!diag_has(DIAG_PANE, "gyro dps"),
                 "the detail is showing before anything was opened");

    /* R is the sensor block, per the hint row. */
    LS_CHECK(ls_scr_diag.key(LS_TK_CHAR, 'r'));
    fresh();
    draw_pane(&ls_scr_diag, DIAG_PANE);

    LS_CHECK_MSG(diag_has(DIAG_PANE, "accel g"),
                 "the sensor detail does not show the accelerometer axes");
    LS_CHECK_MSG(diag_has(DIAG_PANE, "gyro dps"),
                 "the sensor detail does not show the gyroscope axes");
    LS_CHECK_MSG(diag_has(DIAG_PANE, "BACK"),
                 "the detail has no way out that a finger can reach");

    /* And out again. */
    LS_CHECK(ls_scr_diag.key(LS_TK_ESC, 0));
    fresh();
    draw_pane(&ls_scr_diag, DIAG_PANE);
    LS_CHECK_MSG(diag_has(DIAG_PANE, "imu icm20948"),
                 "ESC did not bring the list back");
    LS_CHECK_MSG(!diag_has(DIAG_PANE, "gyro dps"),
                 "the detail is still drawn under the list");

    /* And the other branch, which is the one every board without an
       IMU renders: a sentence, not an empty block. */
    imu_fitted(false);
    diag_fresh();
    LS_CHECK(ls_scr_diag.key(LS_TK_CHAR, 'r'));
    fresh();
    draw_pane(&ls_scr_diag, DIAG_PANE);
    LS_CHECK_MSG(diag_has(DIAG_PANE, "not fitted"),
                 "a board with no IMU gets an empty sensor page rather than "
                 "one that says so");
    ls_scr_diag.key(LS_TK_ESC, 0);
}

LS_CASE(diag_hands_esc_back_when_no_detail_is_open)
{

    seed_health();
    diag_fresh();
    LS_CHECK_MSG(!ls_scr_diag.key(LS_TK_ESC, 0),
                 "DIAG ate an ESC with no detail open - the shell's own back "
                 "is gone from this screen");

    LS_CHECK(ls_scr_diag.key(LS_TK_CHAR, 'm'));
    LS_CHECK_MSG(ls_scr_diag.key(LS_TK_ESC, 0),
                 "ESC did not close an open detail");
    LS_CHECK_MSG(!ls_scr_diag.key(LS_TK_ESC, 0),
                 "the second ESC was eaten too");
}

LS_CASE(every_diag_block_opens_something_different)
{
    /* Four blocks, four pages. A key that opened the wrong one, or the same
       one twice, is a layout error nothing else here would catch. */
    static const struct { char key; const char *proof; } BLOCK[] = {
        { 'e', "DECLARED ENDPOINTS" },
        { 'm', "FREE / LARGEST BLOCK" },
        { 'a', "AW86224" },
        { 'r', "IMU ICM20948" },
    };

    seed_health();
    for (int i = 0; i < 4; i++) {
        diag_fresh();
        LS_CHECK(ls_scr_diag.key(LS_TK_CHAR, BLOCK[i].key));
        fresh();
        draw_pane(&ls_scr_diag, DIAG_PANE);
        LS_CHECK_MSG(diag_has(DIAG_PANE, BLOCK[i].proof),
                     "'%c' did not open the page carrying \"%s\"",
                     BLOCK[i].key, BLOCK[i].proof);
        for (int j = 0; j < 4; j++) {
            if (j == i) continue;
            LS_CHECK_MSG(!diag_has(DIAG_PANE, BLOCK[j].proof),
                         "'%c' opened a page carrying \"%s\" as well",
                         BLOCK[i].key, BLOCK[j].proof);
        }
    }
    ls_scr_diag.key(LS_TK_ESC, 0);
}

LS_CASE(a_diag_detail_keeps_the_other_half_of_the_diagnosis)
{
    /* The detail replaces the LEFT panel, not the screen. In portrait
       the split is top and bottom, so the decode chain sits under it and has
       to still be there - the page whose job is "what exactly is not working"
       should not make you close one answer to read another. */
    seed_health();
    diag_fresh();
    LS_CHECK(diag_has(DIAG_PANE, "P25 CHAIN"));

    LS_CHECK(ls_scr_diag.key(LS_TK_CHAR, 'e'));
    fresh();
    draw_pane(&ls_scr_diag, DIAG_PANE);
    LS_CHECK_MSG(diag_has(DIAG_PANE, "P25 CHAIN"),
                 "opening a detail took the decode chain off the screen");
    ls_scr_diag.key(LS_TK_ESC, 0);
}

LS_CASE(a_diag_detail_stays_inside_every_pane_it_is_offered)
{
    /* Including the panes too small to draw a page in at all, which is where
       a body rect computed as h-4 goes negative. */
    seed_health();
    for (int b = 0; b < 4; b++) {
        static const char KEYS[] = { 'e', 'm', 'a', 'r' };
        for (int p = 0; p < N_PANES; p++) {
            ls_scr_diag.key(LS_TK_ESC, 0);
            if (ls_scr_diag.enter) ls_scr_diag.enter();
            ls_scr_diag.key(LS_TK_CHAR, KEYS[b]);
            fresh();
            draw_pane(&ls_scr_diag, PANES[p]);
            LS_CHECK_MSG(escaped(PANES[p]) == 0,
                         "detail '%c' escaped a %dx%d pane", KEYS[b],
                         PANES[p].w, PANES[p].h);
        }
    }
    ls_scr_diag.key(LS_TK_ESC, 0);
}

LS_CASE(journal_distinguishes_saved_entry_sensors_from_live_sensors)
{
    ls_scr_journal.enter();
    ls_scr_journal.key(LS_TK_ENTER,0);
    ls_scr_journal.key(LS_TK_CHAR,'v');
    fresh();draw_pane(&ls_scr_journal,PANES[1]);
    LS_CHECK(diag_has(PANES[1],"ENTRY ATTACHMENTS"));
    LS_EQ_INT(escaped(PANES[1]),0);
    ls_scr_journal.key(LS_TK_BACKSPACE,0);
    ls_scr_journal.key(LS_TK_CHAR,'v');
    fresh();draw_pane(&ls_scr_journal,PANES[1]);
    LS_CHECK(diag_has(PANES[1],"LIVE ATTACHMENTS"));
    ls_scr_journal.leave();
}

void ls_map_center(double lat, double lon) { (void)lat; (void)lon; }

LS_CASE(compact_controls_keep_values_and_keyboard_focus)
{
    fresh();
    tui_rect a=tui_rect_make(2,2,110,24);
    LS_EQ_INT(ls_btn_raised_height(a,5),3);
    ls_btn_t btn[]={{"FIRST","ON",'a',true,false},{"BLOCKED",NULL,'b',false,true},{"LAST","OFF",'c',false,false}};
    ls_btn_bar_raised(&g_sf,tui_rect_make(a.x,a.y,a.w,3),btn,3,-1);
    int slot=0,focus=-1;
    LS_CHECK(ls_btn_navigate(LS_TK_RIGHT,&slot,&focus,false));
    LS_EQ_INT(focus,0);
    ls_btn_navigate(LS_TK_RIGHT,&slot,&focus,false);
    LS_EQ_INT(focus,2);
    ls_btn_navigate(LS_TK_LEFT,&slot,&focus,false);
    LS_EQ_INT(focus,0);
    LS_CHECK(!ls_btn_enabled(0,1));
    LS_CHECK(diag_has(a,"FIRST ON"));
    LS_CHECK(diag_has(a,"LAST OFF"));
}

LS_CASE(fm_waterfall_shortcuts_do_not_tune_or_change_receiver_gain)
{
    const ls_tui_screen_t *screens[] = {&ls_scr_fm};
    for (int i=0;i<1;i++) {
        if(screens[i]->enter)screens[i]->enter();
        screens[i]->key(LS_TK_CHAR,'3');
        ls_wf_cfg_t before=*ls_wf_cfg();
        screens[i]->key(LS_TK_CHAR,'f');
        LS_CHECK(ls_wf_cfg()->grain != before.grain);
        screens[i]->key(LS_TK_CHAR,'g');
        LS_CHECK(ls_wf_cfg()->range != before.range);
        screens[i]->key(LS_TK_CHAR,'h');
        LS_CHECK(ls_wf_cfg()->paused != before.paused);
        ls_wf_cfg_set(&before);
        if(screens[i]->leave)screens[i]->leave();
    }
}

LS_CASE(button_shortcuts_and_disabled_touch_follow_displayed_controls)
{
    fresh();
    ls_btn_t buttons[]={{"READ","CARD",'r',false,false},{"SEND","BUSY",'t',false,true}};
    tui_rect bar={2,3,44,7};
    ls_btn_bar_raised(&g_sf,bar,buttons,2,-1);
    LS_EQ_INT(ls_btn_shortcut('R',0),0);
    LS_EQ_INT(ls_btn_shortcut('t',0),-1);
    for(int y=bar.y;y<bar.y+bar.h;y++)for(int x=bar.x;x<bar.x+bar.w;x++)
        LS_CHECK(ls_btn_hit(x,y)!=1);
    ls_btn_clear_hits();
    LS_EQ_INT(ls_btn_shortcut('r',0),-1);
    for(int y=bar.y;y<bar.y+bar.h;y++)for(int x=bar.x;x<bar.x+bar.w;x++)
        LS_EQ_INT(ls_btn_hit(x,y),-1);
}
