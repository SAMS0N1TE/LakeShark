/* LS_TEST_SOURCES: the three screens plus tui_core, with the state they read faked */

#include "ls_test.h"
#include "ls_value.h"
#include "ls_map.h"
#include "ls_rec_replay.h"
#include <math.h>
#include <stdlib.h>
#include "rec_watch.h"
#include "tui_core.h"
#include "ls_tui_screen.h"
#include "ls_app.h"
#include "ls_app_docs.h"
#include "ls_anim.h"
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
#include "ls_wf_source.h"
#include "ls_skyview.h"
#include "ls_picker.h"
#include "scan_engine.h"

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
/* Sub-GHz display preferences. Real state, because the screen reads them
   back to draw the option lists with the current choice marked. */
static int s_sg_style, s_sg_colour;
int  settings_get_subghz_style(void) { return s_sg_style; }
void settings_set_subghz_style(int v) { s_sg_style = v; }
static int s_compass_opt;
int  settings_get_compass_options(void) { return s_compass_opt; }
bool settings_get_last_fix(float *lat, float *lon) { (void)lat; (void)lon; return false; }
bool settings_set_last_fix(float lat, float lon) { (void)lat; (void)lon; return false; }
bool settings_get_df_offset(int s, int m, float *d) { (void)s; (void)m; (void)d; return false; }
esp_err_t ls_board_hw_antenna_external(bool ext) { (void)ext; return ESP_OK; }
bool ls_board_hw_antenna_is_external(void) { return false; }
void settings_set_df_offset(int s, int m, float d) { (void)s; (void)m; (void)d; }
int  settings_get_df_option(int id, int f) { (void)id; return f; }
void settings_set_df_option(int id, int v) { (void)id; (void)v; }
int  settings_get_df_channels(int s, uint32_t *hz, int m) { (void)s; (void)hz; (void)m; return 0; }
void settings_set_df_channels(int s, const uint32_t *hz, int n) { (void)s; (void)hz; (void)n; }
void settings_set_compass_options(int v) { s_compass_opt = v; }
int  settings_get_subghz_colour(void) { return s_sg_colour; }
void settings_set_subghz_colour(int v) { s_sg_colour = v; }
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

/* Geographic rendering has its own real-reader/renderer host suite.
 * This screen fixture supplies a deterministic basemap projection only. */
static int map_zoom=8;
int ls_map_zoom(void) {return map_zoom;}
void ls_map_zoom_by(int dz) {map_zoom+=dz;}
int ls_map_tile_px(void) {return 76;}
bool ls_map_render_busy(void) {return false;}
const char *ls_map_status(void) {return NULL;}
void ls_map_preview_leave(void) {}
void ls_map_follow_set(bool enable) {(void)enable;}
void ls_map_preview_reserve(tui_rect a,int x,int y,int w) {(void)a;(void)x;(void)y;(void)w;}
void ls_map_preview_labels(tui_surface *sf,tui_rect a) {(void)sf;(void)a;}
void ls_map_get_center(double *lat,double *lon) {if(lat)*lat=43.4445;if(lon)*lon=-71.6473;}
void ls_map_pan(int dx,int dy) {(void)dx;(void)dy;}
const uint16_t *ls_map_render(int *w,int *h) {if(w)*w=0;if(h)*h=0;return NULL;}
void ls_map_preview(tui_surface *sf,tui_rect a,double lat,double lon)
{(void)sf;(void)a;(void)lat;(void)lon;}
bool ls_map_preview_point(double lat,double lon,tui_rect a,int *x,int *y)
{
    *x=a.x+a.w/2+(int)lround((lon+71.6473)*20);
    *y=a.y+a.h/2-(int)lround((lat-43.4445)*20);
    return *x>=a.x && *x<a.x+a.w && *y>=a.y && *y<a.y+a.h;
}

static bool s_gps_fix = false;
static bool s_gps_sat_view;
static double s_gps_lat = 43.60, s_gps_lon = -71.30;
void ls_gps_get(ls_gps_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->fix = s_gps_fix;
    if(s_gps_sat_view) { out->last_sentence_us=esp_timer_get_time(); out->sat_count=2; out->sats_visible=2; out->sats[1].prn=9; out->sats[0].prn=7; out->sats[0].snr=38; out->sats[0].azimuth=90; out->sats[0].elevation=45; }
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
static bool s_fm_frequency_locked;
static uint32_t s_fm_frequency_lock_hz;
void lakeshark_fm_frequency_lock(bool on)
{
    s_fm_frequency_locked=on;
    s_fm_frequency_lock_hz=on?FM.freq_hz:0;
}
bool lakeshark_fm_frequency_locked(void){return s_fm_frequency_locked;}
uint32_t lakeshark_fm_frequency_lock_hz(void){return s_fm_frequency_lock_hz;}
extern int ls_test_wf_pumps, ls_test_wf_releases;
const char *ls_wf_source_label(ls_wf_src_t src) { static const char *names[]={"AUTO","P25","FM","LORA"}; return names[src]; }
const char *ls_wf_source_blocked(ls_wf_src_t src) { (void)src; return NULL; }
const char *ls_wf_source_progress(void) { return NULL; }
const char *ls_wf_preset_none(ls_wf_src_t src) { (void)src; return NULL; }
void ls_wf_fm_sweep(bool on){FM.mode=on?FM_MODE_SCAN:FM_MODE_LISTEN;}
int ls_wf_preset_count(ls_wf_src_t src){return src==LS_WF_SRC_FM?1:0;}
const char *ls_wf_preset_label(ls_wf_src_t src,int i)
{return src==LS_WF_SRC_FM&&i==0?"VHF land":"";}
const char *ls_wf_preset_detail(ls_wf_src_t src,int i)
{return src==LS_WF_SRC_FM&&i==0?"150-162":"";}
const char *ls_wf_preset_current(ls_wf_src_t src)
{return src==LS_WF_SRC_FM?"VHF land":"-";}
bool ls_wf_preset_apply(ls_wf_src_t src,int i)
{return src==LS_WF_SRC_FM&&i==0;}

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

/* GPS screen is outside this pane fixture; the simulator links the real screen. */
static int rec_gps_draws, rec_gps_keys;
static void rec_gps_draw(tui_surface *sf,tui_rect a) {(void)sf;(void)a;rec_gps_draws++;}
static bool rec_gps_key(ls_tk_t k,char c) {(void)k;if(c=='r')rec_gps_keys++;return true;}
static bool rec_gps_touch(int x,int y) {(void)x;(void)y;return true;}
const ls_tui_screen_t ls_scr_gps = { .name = "GPS", .draw=rec_gps_draw, .key=rec_gps_key, .touch=rec_gps_touch };

extern const ls_tui_screen_t ls_scr_falls, ls_scr_settings, ls_scr_diag, ls_scr_rec,
                             ls_scr_home, ls_scr_fm, ls_scr_adsb, ls_scr_labs, ls_scr_journal, ls_scr_subghz, ls_scr_mixrf,
                             ls_scr_notes, ls_scr_compass;

static const ls_tui_screen_t *const SCREENS[] = {
    &ls_scr_settings, &ls_scr_diag, &ls_scr_rec, &ls_scr_home,
    &ls_scr_fm, &ls_scr_adsb, &ls_scr_labs, &ls_scr_journal, &ls_scr_subghz, &ls_scr_mixrf,
    &ls_scr_notes, &ls_scr_compass,
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

static float s_sql_seen;
static int   s_sql_calls;
/* The STEP control reads its current value before it writes the next one, and
   ls_value_builtin is not linked here, so the value has to be published too or
   the step reports UNAVAILABLE and never reaches the action. */
static bool fm_test_sql_value(ls_val_t *out)
{
    out->kind = LS_VAL_FLOAT;
    out->f = (float)FM.squelch_tenths;
    return true;
}
static ls_act_status_t fm_test_sql(const ls_args_t *args, ls_val_t *out)
{
    (void)out;
    if (!args || args->n != 1) return LS_ACT_BADARG;
    s_sql_seen = args->v[0].kind == LS_VAL_INT ? (float)args->v[0].i
                                               : args->v[0].f;
    s_sql_calls++;
    return LS_ACT_OK;
}

LS_CASE(squelch_down_lowers_squelch_instead_of_toggling_the_sweep)
{
    /* The screen's own key() claims some letters and returns before the quick
       bar is ever consulted, so a quick control that names one of those is a
       control with no key at all. SQUELCH named 'w', RUN SWEEP already had it,
       and winding the squelch down toggled the sweep and dropped the receiver
       back into whatever sub-mode it came from. */
    fresh();
    const tui_rect pane = {1, 2, 46, 63};
    grid_for(pane);
    ls_action_register("fm.sql", "f", LS_CAP_TUNE, fm_test_sql, "squelch");
    ls_value_publish("fm.sql", "%", fm_test_sql_value);
    s_sql_calls = 0; s_sql_seen = -1.0f;
    FM.mode = FM_MODE_LISTEN;
    FM.squelch_tenths = 40;

    ls_scr_fm.enter();
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'm'));          /* into the detail page */

    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'a'));          /* squelch down */
    LS_EQ_INT(1, s_sql_calls);
    LS_CHECK_MSG(s_sql_seen < 40.0f,
                 "squelch down asked for %.1f against a starting 40",
                 (double)s_sql_seen);
    LS_EQ_INT(FM_MODE_LISTEN, (int)FM.mode);

    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 's'));          /* squelch up */
    LS_EQ_INT(2, s_sql_calls);
    LS_CHECK_MSG(s_sql_seen > 40.0f,
                 "squelch up asked for %.1f against a starting 40",
                 (double)s_sql_seen);
    LS_EQ_INT(FM_MODE_LISTEN, (int)FM.mode);
}

LS_CASE(w_still_belongs_to_run_sweep)
{
    /* The other half of the fix: RUN SWEEP keeps the key it advertises on its
       own button, so moving squelch off 'w' cannot have taken it away. */
    fresh();
    const tui_rect pane = {1, 2, 46, 63};
    grid_for(pane);
    FM.mode = FM_MODE_LISTEN;
    ls_scr_fm.enter();
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'm'));
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'w'));
    LS_EQ_INT(FM_MODE_SCAN, (int)FM.mode);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'w'));
    LS_EQ_INT(FM_MODE_LISTEN, (int)FM.mode);
}

LS_CASE(fm_mode_picker_reaches_every_fm_receiver_and_nfm_disables_p25_mixing)
{
    fresh();
    const tui_rect pane = {1, 2, 46, 63};
    grid_for(pane);
    ls_action_register("fm.submode", "s", LS_CAP_TUNE, fm_test_select, "FM mode");
    const fm_mode_t modes[] = {FM_MODE_LISTEN, FM_MODE_WFM, FM_MODE_AM,
                              FM_MODE_POCSAG, FM_MODE_FLEX, FM_MODE_ACARS};
    FM.mode = FM_MODE_LISTEN;
    ls_scr_fm.enter();
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'm'));
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'e'));
    for (int i = 0; i < 6; ++i) {
        LS_CHECK(ls_picker_active());
        for (int j = 0; j < i; ++j) LS_CHECK(ls_picker_key(LS_TK_DOWN, 0));
        LS_CHECK(ls_picker_key(LS_TK_ENTER, 0));
        LS_EQ_INT(FM.mode, modes[i]);
        if (i + 1 < 6) LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'e'));
    }
    scan_engine_set_mixed(true);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'e'));
    LS_CHECK(ls_picker_key(LS_TK_ENTER, 0));
    LS_EQ_INT(FM.mode, FM_MODE_LISTEN);
    LS_CHECK(!scan_engine_mixed());
}

/* OPENING THE SPECTRUM MUST NOT HIJACK THE RECEIVER.

   It used to force FM_MODE_SCAN on entry, so asking to SEE a signal stopped
   you hearing it - and the page carries a RUN button that does exactly that
   when it is actually wanted. The page opens on whatever the receiver is
   already doing; starting a sweep is a decision, so it is a button. */
LS_CASE(opening_the_fm_spectrum_leaves_the_receiver_where_it_was)
{
    fresh();
    const tui_rect pane = {1, 2, 113, 24};
    grid_for(pane);
    FM.mode = FM_MODE_LISTEN;
    ls_scr_fm.enter();
    ls_scr_fm_show_page(2);
    LS_CHECK_MSG(FM.mode != FM_MODE_SCAN,
                 "opening SPECTRUM put the receiver into SCAN (mode %d)",
                 (int)FM.mode);
    ls_scr_fm.leave();
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
    /* Opening the page no longer starts a sweep - it used to, and the owner
       asked for it changed: entering SPECTRUM took a receiver that was
       listening and put it into SCAN, so asking to SEE a signal stopped you
       hearing it. Starting one is the RUN button, which is what the rest of
       this case then exercises. */
    LS_EQ_INT(FM.mode, FM_MODE_LISTEN);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'w'));
    LS_EQ_INT(FM.mode, FM_MODE_SCAN);
    /* The PAGER tab used to put a listening receiver into POCSAG on its own,
       which is a view reaching over and changing what the radio does. It no
       longer does, and outside a pager mode it is not offered at all, so
       asking for it leaves the receiver where it was. */
    ls_scr_fm_show_page(1);
    LS_EQ_INT(FM.mode, FM_MODE_SCAN);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'e'));
    LS_CHECK(ls_picker_key(LS_TK_DOWN, 0));
    LS_CHECK(ls_picker_key(LS_TK_ENTER, 0));
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
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 'm'));
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, '['));
    LS_CHECK(fabsf(s_fm_tuned_mhz - 99.9875f) < 0.0001f);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, 't'));
    LS_EQ_INT(s_fm_keypad_opens, 1);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR, ']'));
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
          LS_APP_EXTRA, &ls_scr_settings, NULL, &ls_doc_settings },
        { "diag", "DIAG", "health",   LS_ICON_CHIP,   TUI_WHITE,
          LS_APP_EXTRA, &ls_scr_diag, NULL, &ls_doc_diag },
        { "rec",  "REC",  "capture",  LS_ICON_RECORD, TUI_RED,
          LS_APP_EXTRA, &ls_scr_rec, NULL, &ls_doc_rec },
        { "home", "HOME", "directory", LS_ICON_SHARK, TUI_CYAN,
          LS_APP_MAIN, &ls_scr_home, NULL, &ls_doc_home },
        { "fm",   "FM",   "analogue", LS_ICON_WAVE,   TUI_YELLOW,
          LS_APP_MAIN, &ls_scr_fm, NULL, &ls_doc_fm },
        { "adsb", "ADSB", "aircraft", LS_ICON_PLANE,  TUI_MAGENTA,
          LS_APP_MAIN, &ls_scr_adsb, NULL, &ls_doc_adsb },
        { "labs", "LORA LABS", "experiments", LS_ICON_LABS, TUI_CYAN,
          LS_APP_EXTRA, &ls_scr_labs, NULL, &ls_doc_labs },
        { "notes", "NOTES", "field notes", LS_ICON_JOURNAL, TUI_GREEN,
          LS_APP_EXTRA, &ls_scr_notes, NULL, &ls_doc_notes },
        { "compass", "COMPASS", "bearings", LS_ICON_COMPASS, TUI_YELLOW,
          LS_APP_EXTRA, &ls_scr_compass, NULL, &ls_doc_compass },
        { "subghz", "SUB-GHZ", "watch", LS_ICON_RECORD, TUI_GREEN,
          LS_APP_EXTRA, &ls_scr_subghz, NULL, &ls_doc_subghz },
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

/* The splash was disabled on 2026-09-13 by swapping ls_anim_start for
   ls_anim_cancel in ls_app_open - one line inside a 112-file commit, with no
   reason recorded - and this case was added in the same commit to hold the
   new behaviour. It is wanted back, so the case now asserts the splash it
   used to forbid. What it still checks is the part that always mattered: the
   tile that was touched is the app that opened, and the splash is dismissable
   rather than something you have to sit through. */
LS_CASE(home_touch_opens_an_unselected_app_and_plays_its_splash)
{
    apps_once();
    for (int p = 0; p < 2; p++) {
        ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_home));
        ls_scr_home.key(LS_TK_F1, 0);
        fresh();
        draw_pane(&ls_scr_home, PANES[p]);
        int xhit = -1, yhit = -1;
        for (int y = PANES[p].y; y < PANES[p].y + PANES[p].h && xhit < 0; y++)
            for (int x = PANES[p].x; x < PANES[p].x + PANES[p].w; x++)
                if (ls_tile_hit(x, y) == 1) { xhit = x; yhit = y; break; }
        LS_CHECK(xhit >= 0);
        LS_CHECK(ls_scr_home.touch(xhit, yhit));
        LS_EQ_INT(ls_tui_screen_index_of(&ls_scr_adsb), ls_tui_screen_current());
        LS_CHECK(ls_anim_active());
        ls_anim_cancel();   /* leave the next iteration a clean slate */
    }
    ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_home));
}

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

/* Lettering, as opposed to the dither texture and the frame. Any printable
   character counts except the vertical rule, because labels carry '%', '/',
   '.' and '-' and stopping at the last letter measured "VOL 65 %" as ending
   at the 5. The dither glyphs are outside printable ASCII and the frame's
   verticals are the only printable thing drawn inside a button's columns. */
static bool centre_ink(char c)
{
    const unsigned char u = (unsigned char)c;
    return u > 32 && u < 127 && u != '|';
}

LS_CASE(every_button_a_real_screen_draws_is_centred)
{
    /* test_tui_button_centre sweeps the shared bar in isolation; this proves
       the screens actually get that bar, at the sizes the hardware hands
       them. A screen that grows its own button drawer is not covered here -
       which is the point. Adopting ls_btn_bar is what buys the check.
       Only rows inside the frame are measured: shortcut badges and the [*]
       state marker live on the border deliberately. */
    seed_health();
    int checked = 0;
    for (int s = 0; s < N_SCREENS; s++)
        for (int p = 0; p < N_PANES; p++) {
            if (SCREENS[s]->enter) SCREENS[s]->enter();
            fresh();
            /* What ls_tui_router_draw does before every frame. These cases
               call scr->draw directly, so without it a screen that draws no
               bar is measured against the previous screen's rects. */
            ls_btn_clear_hits();
            draw_pane(SCREENS[s], PANES[p]);

            for (int slot = 0; slot < 3; slot++)
                for (int i = 0; i < ls_btn_count_slot(slot); i++) {
                    int x = 0, y = 0, w = 0, h = 0;
                    if (!ls_btn_rect_slot(slot, i, &x, &y, &w, &h)) continue;
                    if (x < 0 || y < 0 || x + w > W || y + h > H) continue;

                    const int r0 = h >= 3 ? y + 1 : y;
                    const int r1 = h >= 3 ? y + h - 2 : y + h - 1;
                    for (int r = r0; r <= r1; r++) {
                        int first = -1, last = -1;
                        for (int c = 0; c < w; c++) {
                            if (!centre_ink(g_back[(size_t)r * W + x + c].ch)) continue;
                            if (first < 0) first = c;
                            last = c;
                        }
                        if (first < 0) continue;
                        /* The focus caret sits hard against the left edge,
                           the same way the shortcut badge sits against the
                           right. Neither is part of the label. */
                        if (first == 0 &&
                            g_back[(size_t)r * W + x].ch == '>') {
                            first = -1;
                            for (int c = 1; c < w; c++)
                                if (centre_ink(g_back[(size_t)r * W + x + c].ch))
                                { first = c; break; }
                            if (first < 0) continue;
                        }
                        /* A box too narrow for a bracketed badge puts the
                           shortcut as a bare character against the right
                           edge (see ls_tui_ui.c). That is a right-aligned
                           badge, not part of the label, so drop it before
                           measuring - otherwise every keyed button in a
                           narrow box reads as jammed right. */
                        if (last == w - 1 && last - 1 > first &&
                            !centre_ink(g_back[(size_t)r * W + x + last - 1].ch)) {
                            last = -1;
                            for (int c = first; c <= w - 2; c++)
                                if (centre_ink(g_back[(size_t)r * W + x + c].ch))
                                    last = c;
                        }
                        if (last < first) continue;
                        const int left = first, right = w - 1 - last;
                        const int skew = left > right ? left - right
                                                      : right - left;
                        checked++;
                        LS_CHECK_MSG(skew <= 1,
                                     "%s pane %d slot %d button %d: gaps %d/%d "
                                     "in a %d-wide box", SCREENS[s]->name, p,
                                     slot, i, left, right, w);
                        break;   /* the label row is enough */
                    }
                }
        }

    /* Without this the case passes by looking at nothing the day a screen
       stops drawing a bar, which is how a layout test usually dies. */
    LS_CHECK_MSG(checked >= 8,
                 "only %d labelled buttons examined across all screens",
                 checked);
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

    static const char *const NAMES[] = { "SET", "DIAG", "REC", "FM", "ADSB", "LORA LABS", "NOTES" };
    static const char GROUP[] = {'s','s','f','r','r','r','f'};
    for (unsigned i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        ls_scr_home.key((ls_tk_t)(LS_TK_F1+(GROUP[i]=='s'?2:GROUP[i]=='f'?1:0)), 0);
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
    const int radar_x = PANES[0].x + PANES[0].w / 2;
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
        if (radar_y < 0 && strstr(line, "- MINI MAP ")) radar_y = y;
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

static bool find_text(const char *text, int *col, int *row);

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
            if (strstr(line, "- MINI MAP ")) radar_y = y;
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
    int list_x,list_y;
    LS_CHECK(find_text("AIRCRAFT",&list_x,&list_y));
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + 2, list_y + 2));
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
    const int controls = find_row_text("DETAILS");
    LS_CHECK(controls >= 0);
    if (controls < 0) return;
    const uint32_t first = adsb_select_get_icao();
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + PANES[1].w - 3, controls));
    LS_CHECK(adsb_select_get_icao() != first);
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + PANES[1].w / 2, controls));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);
    LS_CHECK(find_row_text("CALLSIGN") >= 0);
    LS_CHECK(find_row_text("BACK") >= 0);
    LS_CHECK(ls_scr_adsb.touch(PANES[1].x + PANES[1].w / 2, controls));
    fresh();
    draw_pane(&ls_scr_adsb, PANES[1]);
    LS_CHECK(find_row_text("AIRCRAFT") >= 0);
    ls_scr_adsb.leave();
}

extern void ls_scr_rec_tools(void);

LS_CASE(rec_gps_source_delegates_without_claiming_an_sdr)
{
    LS_CHECK(ls_scr_rec.radio == NULL);
    ls_scr_rec.enter();
    LS_CHECK(ls_scr_rec.key(LS_TK_CHAR,'u'));
    LS_CHECK(ls_picker_active());
    ls_picker_key(LS_TK_DOWN,0);ls_picker_key(LS_TK_DOWN,0);
    ls_picker_key(LS_TK_ENTER,0);
    rec_gps_draws=rec_gps_keys=0;
    fresh();draw_pane(&ls_scr_rec,PANES[1]);
    LS_EQ_INT(1,rec_gps_draws);
    ls_scr_rec.key(LS_TK_CHAR,'r');
    LS_EQ_INT(1,rec_gps_keys);
    ls_scr_rec.leave();
    ls_scr_rec_tools(); /* restore the shared fixture */
}

LS_CASE(rec_history_uses_elapsed_time_and_clears_missing_data)
{
    const rec_hub_status_t saved = s_rec;
    s_rec.receiver_streaming = true;
    s_rec.phase = REC_ARMED;
    s_rec.bytes_sec = 0;
    ls_shim_time_set(1000000);
    ls_scr_rec.enter();
    ls_scr_rec_tools();
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
    ls_scr_rec_tools();
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
    ls_scr_rec_tools();
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
    ls_scr_rec_tools();
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
    ls_scr_rec_tools();
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
    ls_scr_rec_tools();
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
        for (unsigned i = 0; i < sizeof(GLOBAL) / sizeof(GLOBAL[0]); i++) {
            if(SCREENS[s]==&ls_scr_home && GLOBAL[i]>=LS_TK_F1 && GLOBAL[i]<=LS_TK_F6) continue;
            LS_CHECK_MSG(!SCREENS[s]->key(GLOBAL[i], 0),
                         "%s consumed a global key", SCREENS[s]->name);
        }
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
        "Brightness", "Keyboard light", "Auto dim", "Dim after", "Volume", "Theme", "Daylight",
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

void fm_get_receiver_status(ls_iq_control_status_t *out) { memset(out,0,sizeof(*out)); }

#include "ls_radio_panel.h"
#include "scan_engine.h"
#include "scan_channels.h"
#include "ls_numpad.h"

LS_CASE(large_text_radio_controls_survive_both_orientations_and_submenus)
{
    const tui_rect panes[] = {{0,5,34,41}, {0,2,79,17}};
    scan_engine_stop();
    for (unsigned size = 0; size < 2; size++) for (int fm = 0; fm < 2; fm++) {
        ls_radio_view_t view = {.fm=fm, .frequency=851012500, .mode=fm?"NFM":"P25"};
        for (int page = 0; page < 3; page++) {
            ls_radio_panel_t panel = {.focus=-1, .lists=page==1, .scan_choice=page==2};
            fresh(); grid_for(panes[size]);
            ls_radio_panel_draw(&panel, &view, &g_sf, panes[size]);
            LS_EQ_INT(escaped(panes[size]), 0);
            int x,y;
            LS_CHECK(!find_text("larger radio", &x, &y));
            for (int i = 0; i < 6; i++) {
                bool found = false;
                const char *label = panel.buttons[i].label;
                const int length = (int)strlen(label);
                for (y=panes[size].y; y<panes[size].y+panes[size].h; y++)
                    for (x=panes[size].x; x+length<=panes[size].x+panes[size].w; x++) {
                        if (!panel.buttons[i].dim && ls_btn_hit(x,y) != i) continue;
                        int n=0;
                        while(n<length && g_back[y*W+x+n].ch==label[n]) n++;
                        if(n==length) found=true;
                    }
                LS_CHECK_MSG(found, "missing readable/touchable %s at %dx%d",
                             label, panes[size].w, panes[size].h);
            }
        }
    }
}

LS_CASE(portrait_hides_letter_guides_but_keeps_the_same_actions)
{
    ls_btn_t button = {"SCAN", "OFF", 's', false, false};
    const tui_rect portrait = {0,5,34,41}, landscape = {0,2,79,17};
    fresh(); grid_for(portrait);
    ls_btn_bar_raised(&g_sf, tui_rect_make(3,36,28,5), &button, 1, -1);
    int x,y;
    LS_CHECK(find_text("SCAN", &x, &y));
    LS_EQ_INT(ls_btn_hit(x,y), 0);
    LS_EQ_INT(ls_btn_shortcut('s',0), 0);
    LS_CHECK(!find_text("[s]", &x, &y));
    fresh(); grid_for(landscape);
    ls_btn_bar_raised(&g_sf, tui_rect_make(3,10,28,5), &button, 1, -1);
    LS_CHECK(find_text("[s]", &x, &y));
}

LS_CASE(large_portrait_exposes_all_settings_and_maps_last_touch_correctly)
{
    const tui_rect pane = {0,5,34,41};
    fresh(); draw_pane(&ls_scr_settings, pane);
    const char *labels[] = {"Screen lock", "Brightness", "Auto dim", "Dim after",
        "Volume", "Theme", "Daylight", "Font", "Boot sound", "Alert sound",
        "Vibrate", "USB autoreboot"};
    int x,y;
    for (unsigned i=0; i<sizeof(labels)/sizeof(labels[0]); i++)
        LS_CHECK_MSG(find_text(labels[i], &x, &y), "missing setting %s", labels[i]);
    bool before = settings_get_usb_autoreboot();
    LS_CHECK(find_text("USB autoreboot", &x, &y));
    LS_CHECK(ls_scr_settings.touch(x,y));
    LS_CHECK(settings_get_usb_autoreboot() != before);
    settings_set_usb_autoreboot(before);
    LS_EQ_INT(escaped(pane),0);
}

LS_CASE(radio_dashboard_saved_list_and_touch_scan_use_the_same_controls)
{
    scan_channels_clear();
    scan_engine_set_source(SCAN_SRC_CHANNELS);
    ls_radio_panel_t panel={.focus=-1};
    ls_radio_view_t view={.fm=true,.frequency=154785000,.mode="NFM"};
    tui_rect pane={1,2,46,63};
    fresh();grid_for(pane);
    ls_radio_panel_draw(&panel,&view,&g_sf,pane);
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'l');
    LS_CHECK(panel.lists);
    ls_radio_panel_draw(&panel,&view,&g_sf,pane);
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'f');
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'f');
    LS_EQ_INT(scan_channels_count(),1);
    LS_EQ_INT(scan_channel_get(0)->freq_hz,154785000);
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'m');
    fresh();ls_radio_panel_draw(&panel,&view,&g_sf,pane);
    int x,y;LS_CHECK(find_text("SCAN",&x,&y));
    ls_radio_panel_touch(&panel,&view,x,y);
    LS_CHECK(panel.scan_choice);
    LS_CHECK(!scan_engine_active());
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'s');
    LS_CHECK(scan_engine_active());
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'h');
    LS_CHECK(scan_engine_manual_hold());
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'x');
    LS_CHECK(!scan_engine_active());
    scan_channels_clear();
}

LS_CASE(gps_and_mixed_scan_buttons_match_touch_and_keyboard)
{
    scan_engine_set_source(SCAN_SRC_CHANNELS);
    scan_engine_set_mixed(false); scan_engine_set_location(false);
    ls_radio_panel_t panel={.focus=-1,.scan_choice=true};
    ls_radio_view_t view={.mode="P25"};
    tui_rect pane={1,2,46,63}; fresh(); grid_for(pane);
    ls_radio_panel_draw(&panel,&view,&g_sf,pane);
    int x,y; LS_CHECK(find_text("MIXED AUDIO",&x,&y));
    ls_radio_panel_touch(&panel,&view,x,y);
    LS_CHECK(scan_engine_mixed());
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'g');
    LS_CHECK(scan_engine_location());
    fresh();ls_radio_panel_draw(&panel,&view,&g_sf,pane);
    LS_CHECK(find_text("GPS FILTER",&x,&y));
    ls_radio_panel_touch(&panel,&view,x,y);
    LS_CHECK(!scan_engine_location());
    scan_engine_set_mixed(false);
}

LS_CASE(saved_channel_list_blank_space_does_not_select_an_unseen_row)
{
    scan_engine_stop();
    scan_channels_clear();
    for (int i = 0; i < 20; i++)
        scan_channel_add("Test", 150000000 + i * 12500, SCAN_MODE_NFM, 0);
    ls_radio_panel_t panel = {.focus = -1, .lists = true};
    ls_radio_view_t view = {.fm = true, .mode = "NFM"};
    tui_rect pane = {1, 2, 46, 63};
    fresh();
    grid_for(pane);
    ls_radio_panel_draw(&panel, &view, &g_sf, pane);
    int rows = (panel.list_area.h - 7) / 3;
    ls_radio_panel_touch(&panel, &view, panel.list_area.x + 3,
                         panel.list_area.y + 2 + rows * 3);
    LS_EQ_INT(panel.selected, 0);
    ls_radio_panel_touch(&panel, &view, panel.list_area.x + 3, panel.list_area.y + 5);
    LS_EQ_INT(panel.selected, 1);
    scan_channels_clear();
}

LS_CASE(compact_waterfall_labels_leave_room_for_their_shortcuts)
{
    fresh();
    grid_for((tui_rect){0,2,79,17});
    ls_btn_t buttons[] = {{"DETAIL", "shade", 'f', false, false},
                          {"CNTRST", "soft", 'c', false, false}};
    tui_rect bar = {2, 3, 20, 2};
    ls_btn_bar(&g_sf, bar, buttons, 2, -1);
    LS_CHECK(diag_has(bar, "DETAIL f"));
    LS_CHECK(diag_has(bar, "CNTRST c"));
    LS_EQ_INT(ls_btn_shortcut('f', 0), 0);
    LS_EQ_INT(ls_btn_shortcut('c', 0), 1);
}

LS_CASE(fm_dashboard_bank_swap_uses_hz_and_squelch_opens_an_editor)
{
    ls_action_register("fm.freq_hz","i",LS_CAP_TUNE,fm_test_freq,"Frequency");
    FM.mode=FM_MODE_LISTEN;FM.freq_hz=154785000;
    ls_scr_fm.enter();
    /* The screen opens on the receiver now rather than on the shared scan
       panel, so the A/B swap is reached by going to RADIO first. '0' is the
       tab's own key. On the detail pages 'a' nudges squelch instead, which
       is a different view and not a clash. */
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR,'0'));
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR,'a'));
    LS_EQ_INT(s_fm_tuned_hz,152600000);
    FM.freq_hz=152600000;
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR,'a'));
    LS_EQ_INT(s_fm_tuned_hz,154785000);
    LS_CHECK(ls_scr_fm.key(LS_TK_CHAR,'q'));
    LS_CHECK(ls_numpad_active());
    ls_numpad_close();
}

LS_CASE(scan_choice_separates_saved_channels_from_frequency_steps)
{
    scan_engine_stop();scan_engine_set_source(SCAN_SRC_CHANNELS);
    scan_engine_set_band(150000000,162000000,12500);
    ls_radio_panel_t panel={.focus=-1};
    ls_radio_view_t view={.mode="P25"};
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'s');
    ls_radio_panel_draw(&panel,&view,&g_sf,pane);
    int x,y;LS_CHECK(find_text("CHANNEL LIST",&x,&y));
    LS_CHECK(find_text("BAND SCAN",&x,&y));
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'b');
    LS_EQ_INT(scan_engine_get_source(),SCAN_SRC_BAND);
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'i');
    uint32_t step;scan_engine_get_band(NULL,NULL,&step);LS_EQ_INT(step,15000);
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'c');
    LS_EQ_INT(scan_engine_get_source(),SCAN_SRC_CHANNELS);
    LS_CHECK(!scan_engine_active());
    ls_radio_panel_key(&panel,&view,LS_TK_CHAR,'s');
    LS_CHECK(scan_engine_active());scan_engine_stop();
}

/* THE SCREEN IS A FLOW, NOT A PILE.

   Point it, set it up, run it, act on what it heard - in that order, left to
   right, and everything that is not one of those five things lives behind
   MORE. The bar had TUNE and BANDS side by side, which are the same question
   asked twice, and no SCAN at all: a sweep could only be started - and worse,
   only stopped - from three rows down a menu.

   Checked by drawing rather than by reading the table, because the table is
   not what the operator presses. */
LS_CASE(subghz_bar_is_point_setup_run_scan_act)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    ls_scr_subghz.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    int x,y;
    LS_CHECK(find_text("TUNE",&x,&y));
    LS_CHECK(find_text("SETUP",&x,&y));
    LS_CHECK(find_text("WATCH",&x,&y));
    LS_CHECK(find_text("SCAN",&x,&y));
    LS_CHECK(find_text("CAPTURE",&x,&y));
    /* BANDS folded into TUNE. A second button for the same question is a
       button the operator has to rule out before pressing the first. */
    LS_CHECK(!find_text("BANDS",&x,&y));
    ls_scr_subghz.leave();
}

/* One question, one list. A typed frequency, a preset and a peak the sweep
   found are three answers to "where should it listen", and they were spread
   across two buttons and a menu row that only appeared after a sweep. */
LS_CASE(subghz_tune_offers_typing_presets_and_the_last_sweep)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    ls_scr_subghz.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'f'));
    LS_CHECK(ls_picker_active());
    fresh();ls_picker_draw(&g_sf,pane);
    int x,y;
    LS_CHECK(find_text("TYPE A FREQUENCY",&x,&y));
    LS_CHECK(find_text("433.9200 MHz",&x,&y));
    /* Typing still reaches the keypad - a list that swallowed the one way of
       entering a frequency it does not have a preset for would be a trade
       down from the button it replaced. */
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_CHECK(ls_numpad_active());
    ls_numpad_close();
    /* A preset tunes. Fourth row: typing, then 152.600, 154.785, 315. */
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'f'));
    for(int i=0;i<3;i++)LS_CHECK(ls_picker_key(LS_TK_DOWN,0));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_EQ_INT(rec_get_freq(),315000000);
    ls_scr_subghz.leave();
}

/* The sweep, and everything you can do to one while it runs.

   SCAN is on the bar so that stopping is one press: a running sweep holds
   the radio, and the control for letting go of it does not belong three rows
   down a menu called MORE. */
LS_CASE(subghz_scan_starts_from_the_bar_and_can_be_stopped_there)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    rec_set_freq(433920000);
    ls_scr_subghz.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'n'));
    LS_CHECK(ls_picker_active());
    fresh();ls_picker_draw(&g_sf,pane);
    int x,y;LS_CHECK(find_text("SCAN WHERE",&x,&y));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));       /* watch this frequency */
    LS_CHECK(rec_watch_scan_busy());
    /* Now the same button offers the sweep's own controls, stopping first. */
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'n'));
    LS_CHECK(ls_picker_active());
    fresh();ls_picker_draw(&g_sf,pane);
    LS_CHECK(find_text("STOP",&x,&y));
    LS_CHECK(find_text("THRESHOLD",&x,&y));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_CHECK(!rec_watch_scan_busy());
    ls_scr_subghz.leave();
}

/* The detections view outlives the sweep, and always has a way out.

   What a sweep found is worth looking at after it stops, so the view stays.
   Its off switch is the DETECTED counter in the banner, which is drawn
   whenever the view can be - not a row in a menu that only exists while a
   sweep is running. */
LS_CASE(subghz_detections_view_outlives_the_sweep_and_closes)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    rec_set_freq(433920000);
    ls_scr_subghz.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'n'));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_CHECK(rec_watch_scan_busy());
    /* SWEEP > SHOW DETECTIONS is the fourth row: stop, threshold, on
       detect, then the view toggle. */
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'n'));
    for(int i=0;i<3;i++)LS_CHECK(ls_picker_key(LS_TK_DOWN,0));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    fresh();ls_scr_subghz.draw(&g_sf,pane);
    int x,y;LS_CHECK(find_text("DETECTIONS",&x,&y));
    rec_watch_scan_stop();
    fresh();ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(find_text("DETECTIONS",&x,&y));
    /* The counter closes it, back to the capture list. */
    LS_CHECK(find_text("CLOSE",&x,&y));
    LS_CHECK(ls_scr_subghz.touch(x,y));
    fresh();ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(!find_text("DETECTIONS",&x,&y));
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'f'));
    fresh();ls_picker_draw(&g_sf,pane);
    LS_CHECK(find_text("FROM THE LAST SWEEP",&x,&y));
    ls_picker_close();
    /* And the next sweep starts on the band again. Carrying the view over
       would mean a sweep begun to see whether anything is out there opened
       on a table of what the LAST one found. */
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'n'));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_CHECK(rec_watch_scan_busy());
    fresh();ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(!find_text("DETECTIONS",&x,&y));
    rec_watch_scan_stop();
    ls_scr_subghz.leave();
}

/* MORE is what is left over, and it should be short.

   Nine rows, four of which were about the sweep or the spectrum, is a menu
   you read rather than use. RECEIVER TOOLS in particular could only ever
   print a sentence saying it was the wrong source - a row that exists to
   refuse itself. */
LS_CASE(subghz_more_is_short_and_receiver_tools_belong_to_the_rtl)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    ls_scr_subghz.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'m'));
    LS_CHECK(ls_picker_active());
    fresh();ls_picker_draw(&g_sf,pane);
    int x,y;
    LS_CHECK(find_text("LEARN SIGNAL",&x,&y));
    LS_CHECK(find_text("ALERTS",&x,&y));
    LS_CHECK(find_text("DISPLAY",&x,&y));
    LS_CHECK(find_text("NOTES",&x,&y));
    LS_CHECK(find_text("RECEIVER TOOLS",&x,&y));
    /* The sweep and the spectrum moved to the controls they belong to. */
    LS_CHECK(!find_text("ON DETECT",&x,&y));
    LS_CHECK(!find_text("COLOUR",&x,&y));
    ls_picker_close();
    /* On any other receiver the row is simply not there. */
    rec_watch_select_source(REC_SOURCE_SX1262);
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'m'));
    fresh();ls_picker_draw(&g_sf,pane);
    LS_CHECK(!find_text("RECEIVER TOOLS",&x,&y));
    LS_CHECK(find_text("DISPLAY",&x,&y));
    ls_picker_close();
    rec_watch_select_source(REC_SOURCE_RTL);
    ls_scr_subghz.leave();
}

/* LEARN and its answer were two rows of the same menu, and the second did
   nothing until the first had been run - with nothing in the list to say the
   order mattered. */
LS_CASE(subghz_learn_shows_what_it_heard_in_the_same_list)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_SX1262);
    ls_scr_subghz.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'m'));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));       /* LEARN SIGNAL */
    LS_CHECK(ls_picker_active());
    fresh();ls_picker_draw(&g_sf,pane);
    int x,y;LS_CHECK(find_text("LEARN SIGNAL",&x,&y));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));       /* listen */
    /* Opened again, the reading is in the same list as the control that
       produced it, applying it at the top. */
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'m'));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    fresh();ls_picker_draw(&g_sf,pane);
    LS_CHECK(find_text("APPLY THESE",&x,&y));
    LS_CHECK(find_text("2400 baud",&x,&y));
    LS_CHECK(find_text("Sync word",&x,&y));
    ls_picker_close();
    rec_watch_select_source(REC_SOURCE_RTL);
    ls_scr_subghz.leave();
}

/* Putting something back on air is a decision, and it had a hidden default.

   REPLAY sent at 14 dBm with no say in the matter, which is the wrong answer
   in both directions: too much for a receiver a foot away on the bench, and
   not enough for whatever you are actually trying to reach. */
int rec_watch_sim_replay_dbm(void);
LS_CASE(subghz_replay_asks_how_hard_to_send)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_SX1262);
    ls_scr_subghz.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    /* The third capture is the one from a receiver that can transmit. The
       other two cannot, and their menus have no REPLAY row at all. */
    LS_CHECK(ls_scr_subghz.key(LS_TK_DOWN,0));
    LS_CHECK(ls_scr_subghz.key(LS_TK_DOWN,0));
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'c'));
    LS_CHECK(ls_picker_active());
    fresh();ls_picker_draw(&g_sf,pane);
    int x,y;LS_CHECK(find_text("REPLAY",&x,&y));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    /* Nothing has gone out yet - the power list comes first. */
    LS_CHECK(ls_picker_active());
    fresh();ls_picker_draw(&g_sf,pane);
    LS_CHECK(find_text("SEND AT",&x,&y));
    LS_CHECK(find_text("-9 dBm",&x,&y));
    LS_CHECK(find_text("22 dBm",&x,&y));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_EQ_INT(rec_watch_sim_replay_dbm(),-9);
    /* And the choice is the operator's every time, not remembered as a new
       silent default. */
    LS_CHECK(ls_scr_subghz.key(LS_TK_CHAR,'c'));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    for(int i=0;i<3;i++)LS_CHECK(ls_picker_key(LS_TK_DOWN,0));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_EQ_INT(rec_watch_sim_replay_dbm(),22);
    rec_watch_select_source(REC_SOURCE_RTL);
    ls_scr_subghz.leave();
}

/* ROWS SPENT ON CONTROLS VERSUS ROWS SPENT ON THE SIGNAL.

   A landscape pane has a third of the rows a portrait one does, and this
   screen asked for the portrait treatment anyway: four rows of buttons at
   the top, then a rule that made it SIX whenever the pane was short - taller
   controls the less height there was to spare - over a hardcoded six at the
   bottom. Twelve of thirty-one rows were buttons and the captures got
   thirteen.

   Measured as a share of the pane rather than as row numbers, because the
   point is the proportion and the exact rows move whenever anything above
   them changes. */
LS_CASE(subghz_landscape_spends_its_rows_on_the_signal)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    ls_scr_subghz.enter();
    const tui_rect pane={1,2,113,24};
    fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);

    /* The top bar, the panel under it and the bar under that. */
    const int bar=find_row_text("TUNE");
    const int body=find_row_text("PASSIVE WATCH");
    const int foot=find_row_text("SOURCE");
    LS_CHECK_MSG(bar>=0 && body>bar && foot>body,
                 "landscape SUB-GHZ: bar=%d body=%d foot=%d",bar,body,foot);
    /* Three rows of button means one row of text with a frame either side,
       so the panel opens two rows after the lettering. Four would be three. */
    LS_EQ_INT(2,body-bar);
    /* The compact form, spelled out: label and value share the line. */
    int cx,cy;
    LS_CHECK(find_text("TUNE MHz",&cx,&cy));
    LS_CHECK(find_text("SETUP CAPTURE",&cx,&cy));
    /* And what is left over is most of the pane, not a third of it. */
    const int content=foot-body;
    LS_CHECK_MSG(content*2>pane.h,"only %d of %d rows left for the captures",
                 content,pane.h);
    ls_scr_subghz.leave();
}

/* The short bar is not free everywhere. Three rows is a frame plus ONE line,
   so a button carrying a value writes "LABEL VALUE" into it - and on a
   half-width landscape split, five buttons in fifty-six columns leave eight
   columns for that. SETUP CAPTURE is thirteen.

   The old code carried a comment saying exactly this and drew the wrong
   conclusion from it: it gave up the short bar in EVERY landscape pane
   rather than in the one that cannot take it. */
LS_CASE(subghz_gives_up_the_short_bar_rather_than_cut_a_label_in_half)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    ls_scr_subghz.enter();
    const tui_rect split={1,2,56,24};
    fresh();grid_for(split);
    ls_scr_subghz.draw(&g_sf,split);
    const int bar=find_row_text("SETUP");
    const int body=find_row_text("PASSIVE WATCH");
    LS_CHECK_MSG(bar>=0 && body>bar,"split SUB-GHZ: bar=%d body=%d",bar,body);
    /* Four rows: the value gets its own line, so every word survives. */
    LS_EQ_INT(3,body-bar);
    int x,y;
    LS_CHECK(find_text("SETUP",&x,&y));
    LS_CHECK(find_text("CAPTURE",&x,&y));
    LS_CHECK(find_text("WATCH",&x,&y));
    /* The clip this exists to prevent, spelled out. */
    LS_CHECK(!find_text("SETUP CA",&x,&y));
    LS_CHECK(!find_text("WATCH OF",&x,&y));
    ls_scr_subghz.leave();
}

/* Portrait is not touched by any of it: it has the rows to spare and the
   fatter target is worth them. */
LS_CASE(subghz_portrait_keeps_its_fat_buttons)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_watch_scan_stop();
    ls_scr_subghz.enter();
    const tui_rect pane={1,2,46,63};
    fresh();grid_for(pane);
    ls_scr_subghz.draw(&g_sf,pane);
    const int bar=find_row_text("TUNE");
    const int body=find_row_text("PASSIVE WATCH");
    LS_CHECK_MSG(bar>=0 && body>bar,"portrait SUB-GHZ: bar=%d body=%d",bar,body);
    /* Never the compact form: the value keeps its own row under the label,
       which is the difference the short bar trades away. Asserted as the
       shape rather than as a row count, because this pane is narrow enough
       that the five buttons wrap onto two rows and the count says more about
       the wrapping than about the posture. */
    int x,y;
    LS_CHECK(find_text("TUNE",&x,&y));
    LS_CHECK(!find_text("TUNE MHz",&x,&y));
    LS_CHECK(!find_text("WATCH OFF",&x,&y));
    LS_CHECK_MSG(body-bar>=4,"portrait bar collapsed to %d rows",body-bar);
    ls_scr_subghz.leave();
}

LS_CASE(rec_and_subghz_share_sources_and_preserve_rtl_frequency)
{
    rec_watch_enable(false);rec_watch_select_source(REC_SOURCE_RTL);
    rec_set_freq(152600000);
    ls_scr_rec.enter();
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);
    ls_scr_rec.draw(&g_sf,pane);
    int x,y;LS_CHECK(find_text("SOURCE",&x,&y));
    LS_CHECK(find_text("RTL OOK",&x,&y));
    /* SOURCE opens a list rather than stepping to the next receiver. Cycling
       gave no way to see what the other two were before committing to one,
       and no room to say that the CC1101 is missing or that the SX1262 takes
       the mesh down while it listens. The rows are in rec_source_t order, so
       the index IS the source. */
    ls_scr_rec.key(LS_TK_CHAR,'r');
    LS_CHECK(ls_picker_active());
    LS_EQ_INT(rec_watch_source(),REC_SOURCE_RTL);   /* not until it is chosen */
    LS_CHECK(ls_picker_key(LS_TK_DOWN,0));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_EQ_INT(rec_watch_source(),REC_SOURCE_CC1101);
    LS_EQ_INT(rec_get_freq(),433920000);
    fresh();ls_scr_rec.draw(&g_sf,pane);
    LS_CHECK(find_text("CC1101 OOK",&x,&y));
    /* A running watch refuses the change, and says so rather than moving. */
    ls_scr_rec.key(LS_TK_CHAR,'w');
    LS_CHECK(rec_watch_enabled());
    ls_scr_rec.key(LS_TK_CHAR,'r');
    LS_CHECK(!ls_picker_active());
    LS_EQ_INT(rec_watch_source(),REC_SOURCE_CC1101);
    ls_scr_rec.key(LS_TK_CHAR,'w');
    LS_CHECK(!rec_watch_enabled());
    /* The SX1262 is on the list now that the archive can describe what it
       hears and the runtime can feed it; it is also the only one of the
       three that can transmit, which is what makes replay possible at all. */
    ls_scr_rec.key(LS_TK_CHAR,'r');
    LS_CHECK(ls_picker_active());
    LS_CHECK(ls_picker_key(LS_TK_DOWN,0));
    LS_CHECK(ls_picker_key(LS_TK_DOWN,0));
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_EQ_INT(rec_watch_source(),REC_SOURCE_SX1262);
    /* And back to the RTL, which still remembers where it was pointed. Each
       receiver keeps its own frequency; sharing one meant switching to the
       CC1101 at a VHF frequency it cannot reach and back again to find the
       RTL retuned to 433. */
    ls_scr_rec.key(LS_TK_CHAR,'r');
    LS_CHECK(ls_picker_active());
    LS_CHECK(ls_picker_key(LS_TK_ENTER,0));
    LS_EQ_INT(rec_watch_source(),REC_SOURCE_RTL);
    LS_EQ_INT(rec_get_freq(),152600000);
    ls_scr_rec.leave();
}

LS_CASE(rec_all_metadata_sources_select_and_record_without_sdr_claim)
{
    const ls_field_source_t sources[]={LS_FIELD_HACKRF,LS_FIELD_MESH,LS_FIELD_NRF24,LS_FIELD_NFC,LS_FIELD_WIFI,LS_FIELD_BLE};
    LS_CHECK(ls_scr_rec.radio==NULL);
    for(int i=0;i<6;i++) {
        ls_field_record(false);
        ls_scr_rec.enter(); ls_scr_rec.key(LS_TK_CHAR,'u');
        LS_CHECK(ls_picker_active());
        for(int j=0;j<i+3;j++)ls_picker_key(LS_TK_DOWN,0);
        ls_picker_key(LS_TK_ENTER,0);
        ls_field_state_t state; ls_field_snapshot(&state);
        LS_EQ_INT(state.sample.source,sources[i]);
        fresh();draw_pane(&ls_scr_rec,PANES[1]);
        ls_scr_rec.key(LS_TK_CHAR,'c');
        LS_CHECK(ls_field_recording());
        ls_scr_rec.key(LS_TK_CHAR,'u'); LS_CHECK(!ls_picker_active());
        LS_CHECK(!ls_field_source(LS_FIELD_RTL));
        fresh();draw_pane(&ls_scr_rec,PANES[1]);
        ls_scr_rec.key(LS_TK_CHAR,'c'); LS_CHECK(!ls_field_recording());
        ls_scr_rec.leave();
    }
    ls_scr_rec_tools();
}

LS_CASE(falls_all_radio_data_views_release_spectrum_and_preserve_recording_source)
{
    int64_t old_time=esp_timer_get_time();
    ls_shim_time_set(1000000);
    const ls_field_source_t fields[]={LS_FIELD_CC1101,LS_FIELD_NONE,LS_FIELD_HACKRF,LS_FIELD_NRF24,LS_FIELD_NFC,LS_FIELD_WIFI,LS_FIELD_BLE};
    ls_field_record(false);
    LS_CHECK(ls_scr_falls.radio==NULL);
    for(int i=0;i<7;i++) {
        ls_scr_falls.enter();
        LS_CHECK(ls_scr_falls.key(LS_TK_CHAR,'v'));
        for(int j=0;j<LS_WF_SRC__COUNT+i;j++)ls_picker_key(LS_TK_DOWN,0);
        ls_picker_key(LS_TK_ENTER,0);
        ls_field_sample_t sample;ls_field_sample_snapshot(&sample);
        LS_EQ_INT(sample.source,fields[i]);
        int pumps=ls_test_wf_pumps;
        fresh(); draw_pane(&ls_scr_falls,PANES[1]);
        LS_EQ_INT(pumps,ls_test_wf_pumps);
        LS_CHECK(!ls_scr_falls.key(LS_TK_CHAR,'w'));
        LS_CHECK(!ls_scr_falls.key(LS_TK_CHAR,'t'));
        LS_CHECK(!ls_scr_falls.key(LS_TK_CHAR,'n'));
        LS_EQ_INT(0,escaped(PANES[1]));
        if(i==1) {
            int x,y;
            LS_CHECK(find_text("No fresh receiver data",&x,&y));
            s_gps_sat_view=true;
            fresh();draw_pane(&ls_scr_falls,PANES[1]);
            LS_CHECK(find_text("38 dB-Hz",&x,&y));
            LS_CHECK(find_text("ID 7",&x,&y));
            LS_CHECK(find_text("@ selected",&x,&y));
            LS_CHECK(find_text("rim=0 mid=45",&x,&y));
            LS_CHECK(ls_scr_falls.key(LS_TK_CHAR,'j'));
            fresh();draw_pane(&ls_scr_falls,PANES[1]);
            LS_CHECK(find_text("ID 9",&x,&y));
            LS_CHECK(ls_scr_falls.key(LS_TK_CHAR,'k'));
            fresh();draw_pane(&ls_scr_falls,PANES[1]);
            LS_CHECK(find_text("ID 7",&x,&y));
            for(int pane=0;pane<N_PANES;pane++) {
                fresh();draw_pane(&ls_scr_falls,PANES[pane]);
                LS_EQ_INT(0,escaped(PANES[pane]));
            }
            s_gps_sat_view=false;
        }
        ls_scr_falls.leave();
    }
    LS_CHECK(ls_test_wf_releases>=14);
    ls_field_source(LS_FIELD_CC1101);ls_field_record(true);
    ls_scr_falls.enter();ls_scr_falls.key(LS_TK_CHAR,'v');
    for(int j=0;j<LS_WF_SRC__COUNT+2;j++)ls_picker_key(LS_TK_DOWN,0);
    ls_picker_key(LS_TK_ENTER,0);
    ls_field_sample_t sample;ls_field_sample_snapshot(&sample);
    LS_EQ_INT(sample.source,LS_FIELD_CC1101);
    LS_CHECK(ls_field_recording());
    ls_field_record(false);ls_scr_falls.leave();
    ls_shim_time_set(old_time);
}

LS_CASE(sky_instrument_has_ascii_references_selection_and_stale_guard)
{
    int64_t old=esp_timer_get_time();ls_shim_time_set(1000000);
    ls_gps_state_t g={0};g.last_sentence_us=1000000;g.sat_count=2;
    g.sats[0]=(ls_gps_sat_t){.prn=7,.azimuth=90,.elevation=45,.snr=38,.used=true};
    g.sats[1]=(ls_gps_sat_t){.prn=9};
    fresh();ls_skyview_draw(&g_sf,PANES[1],&g,0);
    int x,y;
    LS_CHECK(find_text("ID 7",&x,&y));LS_CHECK(find_text("USED in fix",&x,&y));
    LS_CHECK(find_text("AZ 090 deg true  EL 45 deg",&x,&y));
    LS_CHECK(find_text("C/N0 38 dB-Hz",&x,&y));
    LS_CHECK(find_text("MARK ID  AZdeg ELdeg CN0dBHz FIX",&x,&y));
    LS_CHECK(y > PANES[1].y + 25);
    for(int row=PANES[1].y;row<PANES[1].y+PANES[1].h;row++)
        for(int col=PANES[1].x;col<PANES[1].x+PANES[1].w;col++)
            LS_CHECK((unsigned char)g_back[row*W+col].ch<=127);
    fresh();ls_skyview_draw(&g_sf,PANES[1],&g,1);
    LS_CHECK(find_text("ID 9",&x,&y));LS_CHECK(find_text("AZ/EL unreported",&x,&y));
    LS_CHECK(find_text("not tracked",&x,&y));
    LS_CHECK(find_text("---    --     --",&x,&y));
    ls_shim_time_set(4000001);fresh();ls_skyview_draw(&g_sf,PANES[1],&g,0);
    LS_CHECK(find_text("STALE",&x,&y));LS_CHECK(!find_text("ID 7",&x,&y));
    ls_shim_time_set(old);
}

LS_CASE(home_function_groups_numeric_launch_and_compass_rotation_policy)
{
    apps_once();
    ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_home));
    ls_anim_cancel();
    LS_CHECK(ls_tui_router_key(LS_TK_F3,0));
    fresh();draw_pane(&ls_scr_home,PANES[0]);
    int x,y;LS_CHECK(!find_text("PAGE",&x,&y));
    LS_EQ_INT(ls_btn_hit_slot(PANES[0].x+5,PANES[0].y+PANES[0].h-2,LS_BTN_SLOT_QUICK),-1);
    /* Slot 2 is the second tile of the open group, wherever the group's
       membership has got to: the same place RIGHT then ENTER lands. */
    LS_CHECK(ls_tui_router_key(LS_TK_RIGHT,0));
    LS_CHECK(ls_tui_router_key(LS_TK_ENTER,0));
    const int by_navigation = ls_tui_screen_current();
    ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_home));
    ls_anim_cancel();
    LS_CHECK(ls_tui_router_key(LS_TK_F3,0));
    LS_CHECK(ls_tui_router_key(LS_TK_CHAR,'2'));
    LS_EQ_INT(by_navigation,ls_tui_screen_current());
    ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_labs));
    LS_CHECK(ls_tui_screen_holds_rotation());
    ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_home));
    LS_CHECK(!ls_tui_screen_holds_rotation());
    LS_CHECK(!ls_scr_home.key(LS_TK_CHAR,']'));
}

/* Paging fixtures below are not apps and do not need a real contract, but
   registration refuses a descriptor without one - which is the point of it.
   One shared stand-in keeps the paging cases about paging. */
static const ls_app_doc_t k_fixture_doc = {
    .purpose = "A registration stand-in used only to fill the app directory "
               "while a paging case counts pages.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

LS_CASE(home_later_page_returns_and_sky_landscape_has_referenced_table)
{
    apps_once();
    static const ls_app_t extra[]={
        {.id="page-a",.name="TEST A",.cat=LS_APP_EXTRA,.screen=&ls_scr_fm,.doc=&k_fixture_doc},
        {.id="page-b",.name="TEST B",.cat=LS_APP_EXTRA,.screen=&ls_scr_fm,.doc=&k_fixture_doc},
        {.id="page-c",.name="TEST C",.cat=LS_APP_EXTRA,.screen=&ls_scr_fm,.doc=&k_fixture_doc}};
    for(int i=0;i<3;i++)ls_app_register(&extra[i]);
    ls_scr_home.key(LS_TK_F1,0);
    const tui_rect small={1,2,46,33};
    fresh();draw_pane(&ls_scr_home,small);
    ls_scr_home.key(LS_TK_F6,0);
    fresh();draw_pane(&ls_scr_home,small);
    int x,y;LS_CHECK(find_text("PAGE 2/",&x,&y));
    ls_scr_home.key(LS_TK_CHAR,'1');
    LS_CHECK(!strcmp(ls_tui_screen_name(ls_tui_screen_current()),"FM"));
    ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_home));
    fresh();draw_pane(&ls_scr_home,small);
    LS_CHECK(find_text("PAGE 2/",&x,&y));
    ls_scr_home.key(LS_TK_F5,0);
    fresh();draw_pane(&ls_scr_home,small);
    LS_CHECK(find_text("PAGE 1/",&x,&y));
    int64_t old=esp_timer_get_time();ls_shim_time_set(1000000);
    ls_gps_state_t g={0};g.last_sentence_us=1000000;g.sat_count=1;
    g.sats[0]=(ls_gps_sat_t){.prn=7,.azimuth=90,.elevation=45,.snr=38,.used=true};
    fresh();ls_skyview_draw(&g_sf,PANES[0],&g,0);
    LS_CHECK(find_text("NORTH-UP",&x,&y));
    LS_CHECK(find_text("AZdeg ELdeg CN0dBHz FIX",&x,&y));
    LS_CHECK(find_text("Green USED",&x,&y));
    ls_shim_time_set(old);
}

LS_CASE(home_portrait_pager_has_large_touch_targets_and_no_function_labels)
{
    apps_once();
    const tui_rect small={1,2,34,41};
    static const ls_app_t extra[]={
        {.id="touch-a",.name="TOUCH A",.cat=LS_APP_EXTRA,.screen=&ls_scr_fm,.doc=&k_fixture_doc},
        {.id="touch-b",.name="TOUCH B",.cat=LS_APP_EXTRA,.screen=&ls_scr_fm,.doc=&k_fixture_doc},
        {.id="touch-c",.name="TOUCH C",.cat=LS_APP_EXTRA,.screen=&ls_scr_fm,.doc=&k_fixture_doc}};
    for(int i=0;i<3;i++)ls_app_register(&extra[i]);
    ls_scr_home.key(LS_TK_F1,0);fresh();draw_pane(&ls_scr_home,small);
    int x,y;LS_CHECK(!find_text("F1",&x,&y));LS_CHECK(!find_text("F5",&x,&y));
    int hits=0,hitx=-1,hity=-1;
    for(int row=small.y;row<small.y+small.h;row++)for(int col=small.x;col<small.x+small.w;col++)
        if(ls_btn_hit_slot(col,row,LS_BTN_SLOT_QUICK)==2){hits++;hitx=col;hity=row;}
    LS_CHECK(hits>=40);LS_CHECK(ls_scr_home.touch(hitx,hity));
    fresh();draw_pane(&ls_scr_home,small);LS_CHECK(find_text("PAGE 2/",&x,&y));
}

static bool keyboard_light=true;
bool settings_get_keyboard_light(void) { return keyboard_light; }
void settings_set_keyboard_light(bool on) { keyboard_light=on; }
int ls_keypad_backlight(bool on) { (void)on; return 0; }

bool ls_track_rec_running(void) { return false; }

LS_CASE(keyboard_light_toggle_is_applied_and_stored)
{
    for(int orientation=0;orientation<2;orientation++) {
        fresh();draw_pane(&ls_scr_settings,PANES[orientation]);
        int x,y;LS_CHECK(find_text("Keyboard light",&x,&y));
        bool before=settings_get_keyboard_light();
        LS_CHECK(ls_scr_settings.touch(x,y));
        LS_CHECK(settings_get_keyboard_light()!=before);
    }
}

LS_CASE(receiver_arrows_tune_but_lists_keep_navigation)
{
    ls_radio_panel_t panel={.focus=-1};
    ls_radio_view_t view={.fm=true,.frequency=100000000,.mode="NFM"};
    LS_EQ_INT(ls_radio_panel_key(&panel,&view,LS_TK_LEFT,0),'[');
    LS_EQ_INT(ls_radio_panel_key(&panel,&view,LS_TK_RIGHT,0),']');
    panel.lists=true; panel.count=3;panel.selected=1;
    LS_EQ_INT(ls_radio_panel_key(&panel,&view,LS_TK_DOWN,0),0);
    LS_EQ_INT(panel.selected,2);
    LS_EQ_INT(ls_radio_panel_key(&panel,&view,LS_TK_RIGHT,0),0);
}
LS_CASE(partial_button_rows_are_centered_with_matching_hits)
{
    fresh();grid_for(tui_rect_make(0,5,34,41));
    ls_btn_t b[]={{"ONE",NULL,0,false,false},{"TWO",NULL,0,false,false},
        {"THREE",NULL,0,false,false},{"FOUR",NULL,0,false,false},{"FIVE",NULL,0,false,false}};
    tui_rect bar=tui_rect_make(0,15,34,10);
    ls_btn_bar_raised(&g_sf,bar,b,5,-1);
    for(int row=17;row<=22;row+=5) {
        int left=-1,right=-1;
        for(int x=0;x<34;x++)if(ls_btn_hit(x,row)>=0){if(left<0)left=x;right=x;}
        LS_CHECK(left>=0);
        LS_CHECK(abs(left-(33-right))<=1);
    }
}
LS_CASE(adsb_portrait_always_offers_map_home_and_radar_view)
{
    const tui_rect pane={0,5,34,41};fresh();grid_for(pane);
    ls_scr_adsb.draw(&g_sf,pane);
    int x,y;
    LS_CHECK(find_text("MAP",&x,&y));
    LS_CHECK(find_text("SET HOME",&x,&y));
    LS_CHECK(ls_scr_adsb.key(LS_TK_CHAR,'r'));
    fresh();grid_for(pane);ls_scr_adsb.draw(&g_sf,pane);
    LS_CHECK(find_text("MINI MAP",&x,&y));
    LS_CHECK(find_text("LIST",&x,&y));
    LS_EQ_INT(escaped(pane),0);
    ls_scr_adsb.key(LS_TK_CHAR,'r');
}

static uint32_t cursor_committed;
static bool cursor_tuner(ls_wf_owner_t owner,uint32_t hz)
{ (void)owner;cursor_committed=hz;return true; }
LS_CASE(fm_waterfall_arrows_select_space_commits)
{
    const ls_tui_screen_t *screens[]={&ls_scr_fm};
    for(int i=0;i<1;i++) {
        fresh();FM.mode=FM_MODE_LISTEN;
        if(screens[i]->enter)screens[i]->enter();
        screens[i]->key(LS_TK_CHAR,i?'2':'1');
        ls_wf_owner_t owner=i?LS_WF_OWNER_P25:LS_WF_OWNER_FM;
        ls_wf_claim(LS_WF_OWNER_NONE,NULL);ls_wf_claim(owner,"test");
        ls_wf_feed_t feed={.center_hz=100000000,.span_hz=240000,.live=true};
        float bins[64]={0};ls_wf_preview(owner,bins,64,&feed);
        ls_wf_draw(&g_sf,tui_rect_make(1,2,46,24));
        ls_wf_set_tuner(cursor_tuner);cursor_committed=0;
        uint32_t start=ls_wf_marker_hz();LS_CHECK(start>0);
        screens[i]->key(LS_TK_RIGHT,0);
        LS_CHECK(ls_wf_marker_hz()>start);LS_EQ_INT(cursor_committed,0);
        uint32_t selected=ls_wf_marker_hz();screens[i]->key(LS_TK_CHAR,' ');
        LS_EQ_INT(cursor_committed,selected);
        ls_wf_set_tuner(NULL);if(screens[i]->leave)screens[i]->leave();
    }
}

void rec_watch_sim_file_done(const char *result);
int rec_watch_sim_file_count(void);
int rec_watch_sim_file_dbm(void);
LS_CASE(record_replay_loads_without_tx_and_waits_for_real_completion)
{
    int64_t old_time=esp_timer_get_time();ls_shim_time_set(1000000);
    apps_once();rec_watch_enable(false);
    subghz_file_t f={.freq_hz=433920000,.filetype_ok=true,.edges=6,.edges_total=6,.span_us=1800};
    strcpy(f.protocol,"RAW");strcpy(f.preset,"FuriHalSubGhzPresetOok650Async");
    int32_t edges[]={300,-300,300,-300,300,-300};
    int before=rec_watch_sim_file_count();
    LS_CHECK(ls_scr_rec_replay_file("/sdcard/test.sub",&f,edges));
    LS_EQ_INT(rec_watch_sim_file_count(),before);
    LS_EQ_INT(ls_tui_screen_current(),ls_tui_screen_index_of(&ls_scr_rec));
    tui_rect pane={1,2,46,63};fresh();grid_for(pane);draw_pane(&ls_scr_rec,pane);
    int x,y;LS_CHECK(find_text("PLAY ONCE",&x,&y));LS_CHECK(find_text("433.9200",&x,&y));
    LS_CHECK(find_text("STORED PULSES",&x,&y));
    ls_scr_rec.key(LS_TK_CHAR,'+');ls_scr_rec.key(LS_TK_ENTER,0);
    LS_EQ_INT(rec_watch_sim_file_count(),before+1);LS_EQ_INT(rec_watch_sim_file_dbm(),0);
    ls_scr_rec.key(LS_TK_ENTER,0);LS_EQ_INT(rec_watch_sim_file_count(),before+1);
    LS_CHECK(!ls_scr_rec_replay_file("/sdcard/other.sub",&f,edges));
    fresh();draw_pane(&ls_scr_rec,pane);LS_CHECK(find_text("BUSY",&x,&y));
    LS_CHECK(find_text("STORED PULSES",&x,&y));int trace_row=y+2;
    int first=-1,second=-1;
    for(int i=0;i<W;i++)if(g_back[trace_row*W+i].ch==':')first=i;
    LS_CHECK(first>=0);ls_shim_time_advance(175000);
    fresh();draw_pane(&ls_scr_rec,pane);
    for(int i=0;i<W;i++)if(g_back[trace_row*W+i].ch==':')second=i;
    LS_CHECK(second-first>15); /* halfway across after 175 ms */
    LS_CHECK(find_text("#1 HIGH 300 us",&x,&y));
    rec_watch_sim_file_done("Sent file once on CC1101");
    fresh();draw_pane(&ls_scr_rec,pane);LS_CHECK(find_text("Sent file once",&x,&y));
    int completed=-1;
    for(int i=0;i<W;i++)if(g_back[trace_row*W+i].ch==':')completed=i;
    LS_EQ_INT(completed,second); /* no completion reset to the left */
    ls_shim_time_advance(300000);fresh();draw_pane(&ls_scr_rec,pane);
    for(int i=pane.x+3;i<pane.x+pane.w-3;i++)LS_CHECK(g_back[trace_row*W+i].ch!=':');
    LS_CHECK(find_text("STORED PULSES",&x,&y));
    bool plain=false;
    for(int i=pane.x+3;i<pane.x+pane.w-3;i++) {
        tui_cell cell=g_back[(y+1)*W+i];
        if(cell.ch!='-' && cell.ch!='|')continue;
        uint8_t fg=cell.attr&15;
        LS_CHECK(fg==TUI_GREEN || fg==(TUI_YELLOW|TUI_BRIGHT));
        if(fg==TUI_GREEN)plain=true;
    }
    LS_CHECK(plain); /* blue/cyan belongs only to the moving activity trail */
    f.invalid=true;LS_CHECK(!ls_scr_rec_replay_file("/sdcard/bad.sub",&f,edges));
    ls_scr_rec.key(LS_TK_TAB,0);ls_scr_rec.leave();ls_shim_time_set(old_time);
}
