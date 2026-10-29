/* FOX HUNT: one receiver, a compass and your body find a transmitter.

   Hold the board in front of you and turn slowly. The TURN SCOPE files
   every level under the heading it was heard at: the green lobe is what
   each direction hears, the white needle is where the board points now
   (it swings with the compass), the cyan ticks on the rim are the
   directions already covered, and the yellow arrow is the bearing the turn
   points to (ls_df_fit). TAKE keeps that bearing with the place it was taken
   (a pulse says it was kept); from somewhere else take another, and where
   the lines cross is the fix, drawn on MAP inside its error ellipse.

   Readings come from the same receivers as COMPASS FIND (ls_dfs, slot 0),
   or from the pretend fox of ls_rfsim. Bearings, the fix and the card log
   are ls_fox_run's. Nothing here waits on a radio or the card. */

#include "../../ls_tui_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "../../ls_app.h"
#include "../../ls_compass_live.h"
#include "../../ls_df.h"
#include "../../ls_df_fit.h"
#include "../../ls_df_sources.h"
#include "../../ls_field.h"
#include "../../ls_fox.h"
#include "../../ls_fox_run.h"
#include "../../ls_geo.h"
#include "../../ls_map.h"
#include "../../ls_map_ink.h"
#include "../../ls_motion.h"
#include "../../ls_numpad.h"
#include "../../ls_options.h"
#include "../../ls_radio_panel.h"
#include "../../ls_radio_select.h"
#include "../../ls_rfs_run.h"
#include "../../ls_rfsim.h"
#include "../../ls_tui_ui.h"
#include "ls_textpack.h"

#define WHITE   TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define CYAN    TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
#define MAGENTA TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK)
#define YELLOW  TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
#define GREEN   TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
#define RED     TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)
#define SIM_SOURCE LS_DFS_COUNT
#define SIM_EVERY_US 80000
#define AGE_US (45 * 1000000LL)          /* a turn older than this is another place */
#define HHIST 48

EXT_RAM_BSS_ATTR static ls_df_sweep_t s_sw;
static ls_df_fit_t s_fit;
static ls_df_estimate_t s_est;
static ls_dfs_t s_src = SIM_SOURCE;
static bool s_src_chosen, s_on;
static uint32_t s_hz = 146565000;        /* the usual two-metre fox frequency */
static int s_method = LS_DF_FIT;
static float s_heading = NAN, s_level = NAN;
static int64_t s_level_us, s_sim_us;
static bool s_true;
static char s_say[96];
static int64_t s_say_us;
static ls_fresh_t s_kept, s_heard;
static uint32_t s_reads;
static tui_rect s_scope;
static char s_hint[96] = "R RADIO  T TUNE  K TAKE  M MAP  O OPTIONS";
EXT_RAM_BSS_ATTR static struct { int64_t us; float h; } s_hh[HHIST];
static int s_hh_n, s_hh_at;

static void say(const char *t) { snprintf(s_say, sizeof(s_say), "%s", t); s_say_us = esp_timer_get_time(); }

/* ------------------------------------------------------------- heading -- */

static void heading_note(int64_t us, float h)
{
    if (!isfinite(h)) return;
    s_hh[s_hh_at].us = us; s_hh[s_hh_at].h = h;
    s_hh_at = (s_hh_at + 1) % HHIST;
    if (s_hh_n < HHIST) s_hh_n++;
}

/* The heading when a level was measured, not when the frame drew it: a
   batch filed at the frame's heading leads the turn by its age. */
static float heading_at(int64_t us)
{
    float best = s_heading; int64_t gap = INT64_MAX;
    for (int i = 0; i < s_hh_n; i++) {
        const int64_t d = s_hh[i].us > us ? s_hh[i].us - us : us - s_hh[i].us;
        if (d < gap) { gap = d; best = s_hh[i].h; }
    }
    return best;
}

static void read_heading(int64_t now)
{
    ls_compass_reading_t r;
    ls_compass_live(&r);
    if (!r.valid || r.low_confidence) return;
    s_true = r.true_valid;
    const float h = r.true_valid ? r.true_deg : r.magnetic;
    s_heading = isfinite(s_heading) ? ls_compass_ease(s_heading, h, 0.5f) : h;
    heading_note(now, s_heading);
}

/* --------------------------------------------------------------- source -- */

static const char *src_name(void) { return s_src == SIM_SOURCE ? "SIM FOX" : ls_dfs_name(s_src); }

static void listen(void)
{
    s_on = false;
    if (s_src == SIM_SOURCE) { s_on = true; return; }
    if (ls_rfs_running()) { say(LS_TEXT(t_scr_fox_rf_survey_has_the, "RF SURVEY has the receiver: stop it, or use SIM")); return; }
    const char *why = ls_dfs_unavailable(s_src);
    if (why) { say(why); return; }
    if (!ls_dfs_select_slot(0, s_src, s_hz)) { say(LS_TEXT(t_scr_fox_that_receiver_would_not, "That receiver would not start")); return; }
    ls_dfs_set_channels(0, &s_hz, 1, 300);
    { ls_dfs_reading_t junk[16]; while (ls_dfs_take(junk, 16) > 0) {} }
    s_on = true;
}

static void forget_turn(void) { ls_df_clear(&s_sw); memset(&s_fit, 0, sizeof(s_fit)); memset(&s_est, 0, sizeof(s_est)); }

static void radio_done(ls_rsel_radio_t r)
{
    static const struct { ls_rsel_radio_t r; ls_dfs_t d; } MAP[] = {
        { LS_RSEL_SDR_RTL, LS_DFS_RTL }, { LS_RSEL_SDR_HACKRF, LS_DFS_HACKRF }, { LS_RSEL_LORA, LS_DFS_LORA },
        { LS_RSEL_CC1101, LS_DFS_CC1101 }, { LS_RSEL_NRF24, LS_DFS_NRF24 }, { LS_RSEL_WIFI, LS_DFS_WIFI }, { LS_RSEL_BLE, LS_DFS_BLE },
    };
    for (unsigned i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (MAP[i].r == r) { if (s_on && s_src != SIM_SOURCE) ls_dfs_stop_slot(0); s_src = MAP[i].d; s_src_chosen = true; forget_turn(); listen(); return; }
    say(LS_TEXT(t_scr_fox_that_radio_has_no, "That radio has no level to turn by"));
}

static void radio_sim(int row)
{
    (void)row;
    if (s_on && s_src != SIM_SOURCE) ls_dfs_stop_slot(0);
    s_src = SIM_SOURCE; s_src_chosen = true; forget_turn(); listen();
}

static void open_radio(void)
{
    static const ls_rsel_extra_t X[] = { { "SIMULATED FOX", "a pretend transmitter (rfsim on the console)" } };
    ls_rsel_open_with(LS_RSEL_FIND, radio_done, X, 1, radio_sim);
}

static void tune_done(double mhz)
{
    const uint32_t hz = (uint32_t)lround(mhz * 1e6);
    if (s_src != SIM_SOURCE && !ls_dfs_in_range(s_src, hz)) { say(LS_TEXT(t_scr_fox_that_radio_does_not, "That radio does not tune there")); return; }
    s_hz = hz; forget_turn();
    if (s_on && s_src != SIM_SOURCE) ls_dfs_set_channels(0, &s_hz, 1, 300);
}

static void open_tune(void) { ls_numpad_open("FOX FREQUENCY", "MHz", s_hz / 1e6, tune_done); }

/* ---------------------------------------------------------------- input -- */

static void take(void)
{
    float bearing = NAN, spread = 10;
    if (s_fit.valid) { bearing = s_fit.bearing; spread = 2 * s_fit.sigma; }
    else if (s_est.valid) { bearing = s_est.bearing; spread = s_est.spread; }
    if (!isfinite(bearing)) { say(LS_TEXT(t_scr_fox_turn_a_slow_full, "Turn a slow full circle first")); return; }
    ls_df_bearing_t b = { .bearing = bearing, .spread = spread < 1 ? 1 : spread, .level = s_sw.top, .time_us = esp_timer_get_time() };
    uint16_t fl;
    if (!ls_rf_where(&b.lat, &b.lon, &fl, NULL)) { say(LS_TEXT(t_scr_fox_no_position_wait_for, "No position: wait for GPS (or rfsim here)")); return; }
    snprintf(b.source, sizeof(b.source), "%.6s %.3f", src_name(), s_hz / 1e6);
    ls_fox_keep(&b);
    ls_fresh_bump(&s_kept);
    forget_turn();
    say(ls_fox_count() < 2 ? "Kept. Walk a few hundred metres to the side, then turn again" : "Kept. The fix is on MAP");
}

static void open_map(void)
{
    ls_fox_fix_t f;
    double lat, lon; uint16_t fl;
    if (ls_fox_fix_now(&f)) { ls_map_follow_set(false); ls_map_center(f.lat, f.lon); }
    else if (ls_rf_where(&lat, &lon, &fl, NULL)) { ls_map_follow_set(false); ls_map_center(lat, lon); }
    /* Street level, where a heat cell or an ellipse is a few cells across. */
    if (ls_map_zoom() < 16) ls_map_zoom_by(16 - ls_map_zoom());
    const ls_app_t *a = ls_app_by_id("map");
    const int i = a ? ls_tui_screen_index_of(a->screen) : -1;
    if (i >= 0) ls_tui_screen_show(i);
}

/* ------------------------------------------------------------- options -- */

static const char *const METHODS[] = { "PEAK", "NULL", "FIT" };
static int get_method(const ls_opt_t *o) { (void)o; return s_method; }
static void set_method(const ls_opt_t *o, int v) { (void)o; s_method = v; }
static void act_forget(const ls_opt_t *o) { (void)o; forget_turn(); }
static void act_undo(const ls_opt_t *o) { (void)o; say(ls_fox_undo() ? "Last bearing taken back" : "No bearings to take back"); }
static void act_new(const ls_opt_t *o) { (void)o; ls_fox_new(); forget_turn(); }
static void act_load(const ls_opt_t *o) { (void)o; ls_fox_reload(); say(LS_TEXT(t_scr_fox_loading_the_newest_hunt, "Loading the newest hunt from the card")); }
static void act_sim(const ls_opt_t *o)
{
    (void)o;
    if (ls_rfsim_mode() != LS_RFSIM_OFF) { ls_rfsim_off(); say(LS_TEXT(t_scr_fox_simulated_position_off_gps, "Simulated position off: GPS again")); return; }
    double lat = 43.2081, lon = -71.5376; uint16_t fl;
    if (!ls_rf_where(&lat, &lon, &fl, NULL)) { lat = 43.2081; lon = -71.5376; }
    ls_rfsim_walk(lat, lon, 90, 2.0f);
    say(LS_TEXT(t_scr_fox_simulated_walk_east_turn, "Simulated walk east: turn the board to hunt the SIM FOX"));
}
static void show_sim(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%s", ls_rfsim_mode() == LS_RFSIM_OFF ? "off" : ls_rfsim_mode() == LS_RFSIM_WALK ? "walking" : "standing"); }
static void show_file(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%s", ls_fox_file()[0] ? ls_fox_file() : "none yet"); }

static const ls_opt_t O_TURN[] = {
    { .label = "AIM BY", .kind = LS_OPT_CYCLE, .names = METHODS, .n = 3, .get = get_method, .set = set_method, .step = 1 },
    { .label = "FORGET THIS TURN", .kind = LS_OPT_ACTION, .act = act_forget },
};
static const ls_opt_t O_HUNT[] = {
    { .label = "FILE", .kind = LS_OPT_ACTION, .show = show_file },
    { .label = "UNDO LAST BEARING", .kind = LS_OPT_ACTION, .act = act_undo },
    { .label = "NEW HUNT", .kind = LS_OPT_ACTION, .act = act_new },
    { .label = "LOAD LAST HUNT", .kind = LS_OPT_ACTION, .act = act_load },
    { .label = "SIMULATED WALK", .kind = LS_OPT_ACTION, .act = act_sim, .show = show_sim },
};
static void options_preview(tui_surface *sf, tui_rect a);
static const ls_opt_ctx_t C_TURN = { .name = "TURN", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_TURN) };
static const ls_opt_ctx_t C_HUNT = { .name = "HUNT", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_HUNT) };
static const ls_opt_t O_ROOT[] = {
    { .label = "TURN", .kind = LS_OPT_MENU, .sub = &C_TURN },
    { .label = "HUNT", .kind = LS_OPT_MENU, .sub = &C_HUNT },
};
static const ls_opt_ctx_t C_ROOT = { .name = "FOX HUNT", .back_to_screen = true, .preview = options_preview, .job = -1,
                                     .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_ROOT) };

/* ---------------------------------------------------------------- frame -- */

static void feed(int64_t now)
{
    if (!s_on) return;
    if (s_src == SIM_SOURCE) {
        if (now - s_sim_us < SIM_EVERY_US || !isfinite(s_heading)) return;
        s_sim_us = now;
        double lat, lon; uint16_t fl;
        if (!ls_rf_where(&lat, &lon, &fl, NULL)) {
            /* The pretend fox needs a place to be heard from. */
            ls_rfsim_here(43.2081, -71.5376);
            ls_rf_where(&lat, &lon, &fl, NULL);
        }
        s_level = ls_rfsim_fox_level(lat, lon, s_heading, now);
        s_level_us = now; s_reads++;
        ls_df_add(&s_sw, s_heading, s_level, now);
        return;
    }
    ls_dfs_status_t st;
    ls_dfs_poll_slot(0, &st);
    if (!st.active || st.source != s_src) { s_on = false; say(LS_TEXT(t_scr_fox_another_app_took_the, "Another app took the receiver")); return; }
    /* Static, not on the UI task's stack (see draw). */
    EXT_RAM_BSS_ATTR static ls_dfs_reading_t rd[32];
    const int n = ls_dfs_take(rd, 32);
    for (int i = 0; i < n; i++) {
        if (rd[i].slot != 0 || !isfinite(rd[i].level)) continue;
        const float h = heading_at(rd[i].us);
        if (isfinite(h)) ls_df_add(&s_sw, h, rd[i].level, rd[i].us);
        s_level = rd[i].level; s_level_us = rd[i].us; s_reads++;
    }
}

static void estimate(int64_t now)
{
    ls_df_age(&s_sw, now, AGE_US);
    const ls_df_method_t m = (ls_df_method_t)s_method;
    if (!ls_df_fit(&s_sw, m == LS_DF_PEAK ? LS_DF_FIT : m, NULL, &s_fit)) memset(&s_fit, 0, sizeof(s_fit));
    if (!ls_df_estimate(&s_sw, m == LS_DF_FIT ? LS_DF_PEAK : m, &s_est)) memset(&s_est, 0, sizeof(s_est));
    if (m == LS_DF_PEAK) s_fit.valid = false;          /* PEAK aims by the loudest bin alone */
}

/* --------------------------------------------------------------- drawing -- */

static void put(tui_surface *sf, tui_rect c, int x, int y, const char *s, uint8_t at) { tui_put_str(sf, c, x, y, s, at); }

static int covered(void)
{
    int n = 0;
    for (int i = 0; i < LS_DF_BINS; i++) n += s_sw.count[i] > 0;
    return n * 360 / LS_DF_BINS;
}

static void header(tui_surface *sf, tui_rect a, const ls_fox_hunt_t *h)
{
    const bool aimed = s_fit.valid || s_est.valid;
    ls_radio_bezel(sf, a, !s_on ? "FOX HUNT / IDLE" : aimed ? "FOX HUNT / AIMED" : "FOX HUNT / TURN");
    int x = a.x + 2;
    const bool heard = ls_fresh(&s_heard, s_reads, 400) > 100;
    x = ls_radio_chip(sf, a, x, a.y + 1, "RX", heard, TUI_GREEN);
    double lat, lon; uint16_t fl;
    const bool where = ls_rf_where(&lat, &lon, &fl, NULL);
    x = ls_radio_chip(sf, a, x, a.y + 1, !where ? "NO FIX" : fl & LS_RF_WHERE_SIM ? "SIM POS" : "GPS FIX", true, where ? TUI_GREEN : TUI_YELLOW);
    x = ls_radio_chip(sf, a, x, a.y + 1, s_true ? "TRUE N" : "MAG N", isfinite(s_heading), TUI_CYAN);
    char t[24];
    snprintf(t, sizeof(t), "%d BRG", h->n);
    ls_radio_chip(sf, a, x, a.y + 1, t, h->n > 0, TUI_YELLOW);
    snprintf(t, sizeof(t), "%.10s", src_name());
    char line[64];
    snprintf(line, sizeof(line), "%s  %.4f MHz", t, s_hz / 1e6);
    put(sf, a, a.x + 2, a.y + 2, line, CYAN);
}

/* Dot geometry of the glass: a braille dot is half a cell across and a quarter
   of one down, so a circle needs more dots of radius down than across. */
static float dot_aspect(void)
{
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1 || ch < 1) return 1.18f;
    return (cw / 2.0f) / (ch / 4.0f);
}

static void polar(int cx, int cy, float r, float ry_k, float brg, int *x, int *y)
{
    const float t = brg * 0.0174533f;
    *x = cx + (int)lrintf(r * sinf(t));
    *y = cy - (int)lrintf(r * ry_k * cosf(t));
}

static void scope(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "TURN SCOPE", TUI_YELLOW);
    const tui_rect in = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    s_scope = in;
    if (in.w < 10 || in.h < 6) return;
    const float k = dot_aspect();
    const int cx = in.w, cy = (in.h * 4) / 2;
    float R = (float)(in.w - 6);
    if (R * k > cy - 6) R = (cy - 6) / k;
    ls_ink_begin_braille(in);
    ls_ink_set_tint(0);
    ls_ink_ellipse(cx, cy, (int)R, (int)(R * k), LS_ATTR_FAINT, 1, -3);
    ls_ink_ellipse(cx, cy, (int)(R / 2), (int)(R * k / 2), LS_ATTR_FAINT, 1, -4);
    /* Covered directions: a tick on the rim each. */
    for (int i = 0; i < LS_DF_BINS; i++) {
        if (!s_sw.count[i]) continue;
        int x0, y0, x1, y1;
        polar(cx, cy, R, k, i * 5.0f + 2.5f, &x0, &y0);
        polar(cx, cy, R + 2, k, i * 5.0f + 2.5f, &x1, &y1);
        ls_ink_line(x0, y0, x1, y1, TUI_ATTR(TUI_CYAN, TUI_BLACK), 3, 0);
    }
    /* The lobe: what each direction typically hears. */
    const float span = s_sw.top - s_sw.floor > 6 ? s_sw.top - s_sw.floor : 6;
    int px = 0, py = 0, fx = 0, fy = 0; bool prev = false, first = false;
    for (int i = 0; i <= LS_DF_BINS; i++) {
        const int b = i % LS_DF_BINS;
        float v = ls_df_typical(&s_sw, b);
        if (!isfinite(v)) { prev = false; continue; }
        const float f = (v - s_sw.floor) / span;
        int x, y;
        polar(cx, cy, R * (0.15f + 0.8f * (f < 0 ? 0 : f > 1 ? 1 : f)), k, b * 5.0f + 2.5f, &x, &y);
        if (prev) ls_ink_line(px, py, x, y, GREEN, 4, 0);
        else if (!first) { fx = x; fy = y; first = true; }
        px = x; py = y; prev = true;
    }
    (void)fx; (void)fy;
    /* The bearing, with its spread on the rim. */
    const float brg = s_fit.valid ? s_fit.bearing : s_est.valid ? s_est.bearing : NAN;
    const float spread = s_fit.valid ? 2 * s_fit.sigma : s_est.spread;
    if (isfinite(brg)) {
        for (float d = -spread; d <= spread; d += 2) {
            int x, y; polar(cx, cy, R - 1, k, brg + d, &x, &y);
            ls_ink_dot(x, y, YELLOW, 6);
        }
        int x, y; polar(cx, cy, R + 3, k, brg, &x, &y);
        ls_ink_line(cx, cy, x, y, YELLOW, 7, 0);
    }
    /* Where the board points: moves only with the compass. */
    if (isfinite(s_heading)) {
        int x, y; polar(cx, cy, R * 0.92f, k, s_heading, &x, &y);
        ls_ink_line(cx, cy, x, y, WHITE, 8, 0);
    }
    /* A bearing kept: one ring out from the centre. */
    const uint8_t kept = ls_fresh_level(&s_kept, 900);
    if (kept) {
        ls_ink_pulse(true);
        const float r = R * (1.0f - kept / 255.0f) + 2;
        ls_ink_ellipse(cx, cy, (int)r, (int)(r * k), MAGENTA, 9, 0);
        ls_ink_pulse(false);
    }
    ls_ink_flush(sf, tui_rect_make(0, -1, 0, 0));
    /* Cardinals and the bearing's number, as characters. */
    static const char *const CARD[4] = { "N", "E", "S", "W" };
    for (int c = 0; c < 4; c++) {
        int x, y; polar(cx, cy, R + 4, k, c * 90.0f, &x, &y);
        put(sf, in, in.x + x / 2, in.y + y / 4, CARD[c], c ? LS_ATTR_DIM : RED);
    }
    put(sf, in, in.x + cx / 2, in.y + cy / 4, "o", WHITE);
    if (isfinite(brg)) {
        char t[16]; snprintf(t, sizeof(t), "%03.0f", fmodf(brg + 360, 360));
        int x, y; polar(cx, cy, R * 0.62f, k, brg + (brg > 90 && brg < 270 ? -14 : 14), &x, &y);
        put(sf, in, in.x + x / 2 - 1, in.y + y / 4, t, TUI_ATTR(TUI_BLACK, TUI_YELLOW));
    }
    if (!s_sw.samples) {
        const char *m = s_on ? "hold it out, turn slowly" : "R RADIO to listen";
        put(sf, in, in.x + (in.w - (int)strlen(m)) / 2, in.y + in.h - 1, m, LS_ATTR_DIM);
    }
}

static void readout(tui_surface *sf, tui_rect a, const ls_fox_hunt_t *h)
{
    char t[96];
    int y = a.y;
    /* Level, breathing with every read. */
    const bool fresh = isfinite(s_level) && esp_timer_get_time() - s_level_us < 2000000;
    if (fresh) snprintf(t, sizeof(t), "LEVEL %6.1f %s", s_level, s_src == SIM_SOURCE || s_src == LS_DFS_LORA ? "dBm" : "dB");
    else snprintf(t, sizeof(t), "LEVEL    --");
    put(sf, a, a.x, y, t, fresh ? GREEN : LS_ATTR_DIM);
    const float span = s_sw.top - s_sw.floor > 6 ? s_sw.top - s_sw.floor : 6;
    const float f = fresh && s_sw.samples ? (s_level - s_sw.floor) / span : 0;
    ls_meter_ascii(sf, a, a.x + 18, y, a.w - 19, f < 0 ? 0 : f > 1 ? 1 : f, TUI_GREEN);
    y++;
    LS_SNPRINTF(t_scr_fox_turned_3d_of_360, t, sizeof(t), "TURNED %3d of 360  DEPTH %4.1f dB  %s", covered(), s_fit.valid ? s_fit.depth : s_est.contrast, METHODS[s_method]);
    put(sf, a, a.x, y++, t, CYAN);
    y++;
    ls_fox_fix_t fix;
    double lat, lon; uint16_t fl;
    if (ls_fox_fix_now(&fix)) {
        double brg = fix.bearing, m = fix.range_m;
        if (ls_rf_where(&lat, &lon, &fl, NULL)) ls_geo_bearing_range(lat, lon, fix.lat, fix.lon, &brg, &m);
        LS_SNPRINTF(t_scr_fox_fix_s_0f_m, t, sizeof(t), "FIX %s %.0f m  at %03.0f %s", m >= 1000 ? "" : "", m, brg, ls_geo_compass(brg));
        if (m >= 1000) LS_SNPRINTF(t_scr_fox_fix_2f_km_at, t, sizeof(t), "FIX %.2f km  at %03.0f %s", m / 1000, brg, ls_geo_compass(brg));
        put(sf, a, a.x, y++, t, RED);
        LS_SNPRINTF(t_scr_fox_0f_x_0f_m, t, sizeof(t), "  +/- %.0f x %.0f m (2 sigma)  %d lines%s", 2 * fix.major_m, 2 * fix.minor_m, fix.used,
                 fix.behind ? ", some behind" : "");
        put(sf, a, a.x, y++, t, LS_ATTR_DIM);
    } else {
        put(sf, a, a.x, y++, h->n ? "FIX needs two bearings that cross" : "FIX: take a bearing, move, take another", YELLOW);
        y++;
    }
    /* The bearings kept, newest first. */
    for (int i = h->n - 1; i >= 0 && y < a.y + a.h - 1; i--) {
        const ls_df_bearing_t *b = &h->b[i];
        LS_SNPRINTF(t_scr_fox_2d_03_0f_3s, t, sizeof(t), "%2d  %03.0f %-3s +/-%2.0f  %.5f, %.5f", i + 1, b->bearing, ls_geo_compass(b->bearing), b->spread / 2, b->lat, b->lon);
        put(sf, a, a.x, y++, t, i == h->n - 1 ? YELLOW : TUI_ATTR(TUI_YELLOW, TUI_BLACK));
    }
    const bool show = s_say[0] && esp_timer_get_time() - s_say_us < 6000000;
    put(sf, a, a.x, a.y + a.h - 1, show ? s_say : ls_fox_status(), show ? WHITE : LS_ATTR_DIM);
}

/* The hunt from above: where each bearing was taken and its line, the fix
   and its two-sigma ellipse, north up, scaled to fit. */
static void hunt_plan(tui_surface *sf, tui_rect a, const ls_fox_hunt_t *h)
{
    if (a.w < 16 || a.h < 6 || !h->n) return;
    ls_fox_fix_t fix;
    const bool have = ls_fox_fix_now(&fix);
    const double lat0 = have ? fix.lat : h->b[h->n - 1].lat, lon0 = have ? fix.lon : h->b[h->n - 1].lon;
    const double mlon = 111132.0 * cos(lat0 * M_PI / 180);
    /* Extent in metres east and north of the middle. */
    double ext = have ? 2.5 * fix.major_m : 100;
    for (int i = 0; i < h->n; i++) {
        const double e = fabs((h->b[i].lon - lon0) * mlon), n = fabs((h->b[i].lat - lat0) * 111132.0);
        if (e > ext) ext = e;
        if (n > ext) ext = n;
    }
    ext *= 1.15;
    const float k = dot_aspect();
    const int cx = a.w, cy = a.h * 2;                       /* braille dots */
    const double per = ext / fmin(cx - 2, (cy - 2) / k);    /* metres a dot across */
    #define PX(e) (cx + (int)lround((e) / per))
    #define PY(n) (cy - (int)lround((n) / per * k))
    ls_ink_begin_braille(a);
    ls_ink_set_tint(0);
    for (int i = 0; i < h->n; i++) {
        const ls_df_bearing_t *b = &h->b[i];
        const double e = (b->lon - lon0) * mlon, n = (b->lat - lat0) * 111132.0;
        const double t = b->bearing * M_PI / 180, reach = ext * 3;
        const bool newest = i == h->n - 1;
        const uint8_t at = newest ? YELLOW : TUI_ATTR(TUI_YELLOW, TUI_BLACK);
        ls_ink_line(PX(e), PY(n), PX(e + reach * sin(t)), PY(n + reach * cos(t)), at, newest ? 5 : 4, newest ? 0 : -2);
        ls_ink_dot(PX(e), PY(n), WHITE, 7);
    }
    if (have) {
        const double ta = fix.major_deg * M_PI / 180;
        int px = 0, py = 0;
        for (int j = 0; j <= 40; j++) {
            const double u = j * 2 * M_PI / 40, p = 2 * fix.major_m * cos(u), q = 2 * fix.minor_m * sin(u);
            const int x = PX(p * sin(ta) + q * cos(ta)), y = PY(p * cos(ta) - q * sin(ta));
            if (j) ls_ink_line(px, py, x, y, RED, 6, 0);
            px = x; py = y;
        }
        ls_ink_line(cx - 2, cy - 2, cx + 2, cy + 2, RED, 8, 0);
        ls_ink_line(cx - 2, cy + 2, cx + 2, cy - 2, RED, 8, 0);
    }
    ls_ink_flush(sf, tui_rect_make(0, -1, 0, 0));
    #undef PX
    #undef PY
    for (int i = 0; i < h->n; i++) {
        const double e = (h->b[i].lon - lon0) * mlon, n = (h->b[i].lat - lat0) * 111132.0;
        const int x = a.x + (cx + (int)lround(e / per)) / 2 + 1, y = a.y + (cy - (int)lround(n / per * k)) / 4;
        char t[4]; snprintf(t, sizeof(t), "%d", i + 1);
        if (tui_rect_contains(a, x, y)) put(sf, a, x, y, t, i == h->n - 1 ? YELLOW : LS_ATTR_DIM);
    }
    char t[24];
    snprintf(t, sizeof(t), "N^  %.0f m across", 2 * ext);
    put(sf, a, a.x, a.y, t, LS_ATTR_DIM);
}

static void options_preview(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "FOX HUNT / HOW", TUI_YELLOW);
    static const char *const L[] = {
        "1  Hold the board out at arm's length",
        "2  Turn slowly, a full circle, 20 s",
        "3  TAKE keeps the bearing and the place",
        "4  Walk 200 m or more to one side",
        "5  Turn and TAKE again: MAP shows the fix",
        "",
        "AIM BY  FIT  the whole turn (best)",
        "        PEAK the loudest direction",
        "        NULL away from your body's shadow",
    };
    static const uint8_t H[] = { WHITE, CYAN, YELLOW, MAGENTA, GREEN, 0, WHITE, LS_ATTR_DIM, LS_ATTR_DIM };
    for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++) if (L[i][0]) put(sf, a, a.x + 2, a.y + 2 + (int)i, L[i], H[i]);
}

static void draw(tui_surface *sf, tui_rect area)
{
    const int64_t now = esp_timer_get_time();
    read_heading(now);
    feed(now);
    estimate(now);
    /* The hunt copy (24 bearings), the readings feed() takes and the fix
       below them were 2.4 KB of draw's frame; with the compass's magnetic
       model under them the UI task's 6 KB stack overflowed on opening FOX
       HUNT. Static: only the UI task draws. */
    EXT_RAM_BSS_ATTR static ls_fox_hunt_t h;
    ls_fox_get(&h);
    const bool wide = ls_tui_is_wide();
    const int bar_h = ls_btn_raised_height(area, 5);
    const int head_h = 4;
    header(sf, tui_rect_make(area.x, area.y, area.w, head_h), &h);
    const int body_y = area.y + head_h, body_h = area.h - head_h - bar_h - 1;
    if (wide) {
        /* The scope as wide as its circle, the hunt the rest. */
        const float R = ((body_h - 2) * 2 - 6) / dot_aspect();
        int sw = (int)R + 10;
        if (sw < 30) sw = 30;
        if (sw > area.w / 2) sw = area.w / 2;
        scope(sf, tui_rect_make(area.x, body_y, sw, body_h));
        ls_panel_box(sf, tui_rect_make(area.x + sw, body_y, area.w - sw, body_h), "HUNT", TUI_RED);
        const int tw = area.w - sw - 3 > 50 ? 50 : area.w - sw - 3;
        readout(sf, tui_rect_make(area.x + sw + 2, body_y + 1, tw, body_h - 2), &h);
        if (area.w - sw - tw - 5 >= 16)
            hunt_plan(sf, tui_rect_make(area.x + sw + tw + 3, body_y + 1, area.w - sw - tw - 5, body_h - 2), &h);
    } else {
        const int rh = body_h * 2 / 5 < 10 ? 10 : body_h * 2 / 5;
        scope(sf, tui_rect_make(area.x, body_y, area.w, body_h - rh));
        ls_panel_box(sf, tui_rect_make(area.x, body_y + body_h - rh, area.w, rh), "HUNT", TUI_RED);
        readout(sf, tui_rect_make(area.x + 2, body_y + body_h - rh + 1, area.w - 4, rh - 2), &h);
    }
    if (!wide) ls_safe_line(sf, area, area.y + area.h - bar_h - 1, s_hint, LS_ATTR_DIM);
    char freq[16]; snprintf(freq, sizeof(freq), "%.3f", s_hz / 1e6);
    char src[16]; snprintf(src, sizeof(src), "%.10s", src_name());
    const bool aimed = s_fit.valid || s_est.valid;
    const ls_btn_t b[] = {
        { "RADIO", src, 'r', s_on, false, false },
        { "TUNE", freq, 't', false, s_src == SIM_SOURCE, false },
        { "TAKE", aimed ? "now" : "turn", 'k', aimed, !aimed, false },
        { "MAP", h.n ? "bearings" : NULL, 'm', false, false, h.n > 1 },
        ls_opt_button(&C_ROOT),
    };
    ls_btn_bar_raised(sf, tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h), b, 5, -1);
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_ENTER) { take(); return true; }
    if (k != LS_TK_CHAR) return false;
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    switch (ch) {
    case 'r': open_radio(); return true;
    case 't': if (s_src != SIM_SOURCE) open_tune(); return true;
    case 'k': case ' ': take(); return true;
    case 'm': open_map(); return true;
    case 'f': forget_turn(); return true;
    default: return ls_opt_key(&C_ROOT, ch);
    }
}

static bool touch(int col, int row)
{
    const int b = ls_btn_hit(col, row);
    if (b >= 0) return key(LS_TK_CHAR, "rtkmo"[b]);
    return true;
}

static void enter(void)
{
    ls_field_start();
    ls_field_watch(true);
    if (!s_src_chosen) {
        /* The receiver COMPASS FIND would use, else the pretend fox. */
        const ls_rsel_radio_t r = ls_rsel_effective(LS_RSEL_FIND);
        s_src = r == LS_RSEL_SDR_RTL ? LS_DFS_RTL : r == LS_RSEL_SDR_HACKRF ? LS_DFS_HACKRF :
                r == LS_RSEL_LORA ? LS_DFS_LORA : r == LS_RSEL_CC1101 ? LS_DFS_CC1101 : SIM_SOURCE;
        if (s_src != SIM_SOURCE && ls_dfs_unavailable(s_src)) s_src = SIM_SOURCE;
        /* A simulated position set on the console means a simulated hunt. */
        if (ls_rfsim_mode() != LS_RFSIM_OFF) s_src = SIM_SOURCE;
        if (s_src == LS_DFS_LORA && !ls_dfs_in_range(s_src, s_hz)) s_hz = 433920000;
    }
    listen();
}

static void leave(void)
{
    if (s_on && s_src != SIM_SOURCE) ls_dfs_stop_slot(0);
    s_on = false;
    ls_field_watch(false);
}

const ls_tui_screen_t ls_scr_fox = {
    .name = "FOX HUNT",
    .hint = s_hint,
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
    .hold_auto_rotation = true,
};
