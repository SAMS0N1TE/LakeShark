/* COMPASS: a tilt-compensated compass that reads true north, and the tools
   that hang off one: follow a target, find a transmitter by its signal
   strength on any radio, and a level. See ls_compass_live.h, ls_df.h,
   ls_df_sources.h, ls_df_hits.h, ls_df_view.h and ls_compass_art.h for the
   parts. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_compass_live.h"
#include "../../ls_compass_art.h"
#include "../../ls_df.h"
#include "../../ls_df_sources.h"
#include "../../ls_df_cal.h"
#include "../../ls_df_hits.h"
#include "../../ls_df_view.h"
#include "../../ls_field.h"
#include "../../ls_glyph.h"
#include "../../ls_notes.h"
#include "../../ls_note_blocks.h"
#include "../../ls_numpad.h"
#include "../../ls_picker.h"
#include "../../ls_app.h"
#include "../../ls_map.h"
#include "../../ls_sun.h"
#include "board/ls_board_hw.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "core/ls_time.h"
#include "core/settings.h"
#include "ls_mesh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { P_DIAL, P_FIND, P_GOTO, P_LEVEL, P_COUNT };
static const char *const PAGE_NAMES[P_COUNT] = { "DIAL", "FIND", "GO TO", "LEVEL" };

extern void ls_scr_labs_request_calibration(void);
extern void ls_scr_labs_classic_compass(tui_surface *sf, tui_rect a);

static int s_page;
static int button_focus = -1, button_slot;
EXT_RAM_BSS_ATTR static ls_compass_reading_t s_r;
static ls_compass_spring_t s_spring;
/* The heading and tilt as shown: held steady at rest (ls_compass_steady_step). */
static ls_compass_steady_t s_steady;
static int64_t s_last_us;
static bool s_magnetic, s_simple;
/* LOOKS: the lobe's fill (LS_COMPASS_FILL_*), the green trail of directions
   just heard, and the direction letters' colour. */
static int s_fill = LS_COMPASS_FILL_LIGHT;
static bool s_trail = true, s_white_letters;
static float s_lock = NAN;            /* in the dial's reference */
EXT_RAM_BSS_ATTR static char s_feedback[96];
/* Direction finding: the shown track's lobe and answer. */
EXT_RAM_BSS_ATTR static float s_lobe[LS_DF_BINS];
EXT_RAM_BSS_ATTR static ls_df_bearing_t s_bearings[LS_DF_BEARINGS];
static int s_bearing_count;
static ls_df_method_t s_method = LS_DF_PEAK;
EXT_RAM_BSS_ATTR static ls_dfs_status_t s_dfs;
static ls_dfs_t s_dfs_source = LS_DFS_MESH;
static uint32_t s_dfs_freq = 915000000;
/* Beacon calibration (ls_df_cal.h) and the correction in use for the
   current radio and method, NAN when there is none. */
EXT_RAM_BSS_ATTR static ls_df_cal_t s_cal;
static float s_df_offset = NAN;
/* How fast the board is being turned, degrees a second, smoothed. */
static float s_turn_rate, s_turn_prev = NAN;
static int64_t s_turn_prev_us;
/* The beacon is off 50 ms in every 300, and about one read in six lands in
   a gap at the noise floor. Filed as they are, those reads made deep false
   nulls, and a floor 60 dB under the signal passed any contrast test. While
   calibrating, each level is the strongest heard in the last 350 ms, one
   beacon period and a little more. */
#define BEACON_HOLD_US 350000
EXT_RAM_BSS_ATTR static struct { int64_t us; float level; } s_recent[16];
static int s_recent_n;
static ls_df_estimate_t s_est;
EXT_RAM_BSS_ATTR static ls_df_fix_t s_fix;
static bool s_find_on;
/* The strongest level lately on the shown track: walking toward a
   transmitter is watching this climb. It holds and falls as the lobe does. */
static float s_peak = NAN;

static void show_page(int p);
static void say(const char *t) { snprintf(s_feedback, sizeof(s_feedback), "%s", t); }

static void save_options(void) { settings_set_compass_options((s_simple ? 1 : 0) | (s_magnetic ? 2 : 0)); }

/* ------------------------------------------------------------ bearings -- */

static bool true_mode(void) { return s_r.true_valid && !s_magnetic; }
/* The heading as the sensor gives it this frame. */
static float raw_heading(void)
{
    if (!s_r.valid) return NAN;
    return true_mode() ? s_r.true_deg : s_r.magnetic;
}
/* The heading the dial shows: true when it can be, unless asked not to,
   and steadied. */
static float shown_heading(void)
{
    if (!s_r.valid || !s_steady.started) return raw_heading();
    return s_steady.heading;
}
/* A true bearing in the dial's reference, and back. */
static float to_dial(float t)
{
    if (!isfinite(t)) return NAN;
    if (true_mode()) return t;
    return isfinite(s_r.declination) ? fmodf(t - s_r.declination + 720.0f, 360.0f) : t;
}
static float to_true(float d)
{
    if (!isfinite(d)) return NAN;
    if (true_mode()) return d;
    return isfinite(s_r.declination) ? fmodf(d + s_r.declination + 720.0f, 360.0f) : d;
}
static const char *ref(void) { return true_mode() ? "T" : "M"; }

static const char *cardinal(float deg)
{
    static const char *const P[] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                     "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    return isfinite(deg) ? P[(int)lroundf(fmodf(deg + 360.0f, 360.0f) / 22.5f) % 16] : "--";
}

static bool position(double *lat, double *lon)
{
    ls_field_sample_t p; ls_field_sample_snapshot(&p);
    if (!p.gps_valid) return false;
    *lat = p.lat; *lon = p.lon; return true;
}

static float sun_azimuth(float *elevation)
{
    double lat, lon;
    if (!ls_time_is_synced() || !position(&lat, &lon)) return NAN;
    ls_sun_t sun; ls_sun_position(time(NULL), lat, lon, &sun);
    if (elevation) *elevation = (float)sun.elevation;
    return sun.elevation > -2 ? (float)sun.azimuth : NAN;
}

static bool target_nav(double *bearing_true, double *metres, char *name, size_t cap)
{
    double tlat, tlon, lat, lon;
    if (!ls_compass_target(&tlat, &tlon, name, cap) || !position(&lat, &lon)) return false;
    ls_compass_nav(lat, lon, tlat, tlon, bearing_true, metres);
    return true;
}

static void distance_text(double m, char *out, size_t cap)
{
    if (m < 1000) snprintf(out, cap, "%.0f m", m);
    else if (m < 20000) snprintf(out, cap, "%.2f km", m / 1000);
    else snprintf(out, cap, "%.0f km", m / 1000);
}

/* ---------------------------------------------------------------- draw -- */

static void big_number(tui_surface *sf, tui_rect clip, int x, int y, float deg, uint8_t attr)
{
    char t[8];
    if (isfinite(deg)) snprintf(t, sizeof(t), "%03d", (int)lroundf(fmodf(deg + 360.0f, 360.0f)) % 360);
    else snprintf(t, sizeof(t), "---");
    for (int i = 0; i < 3; i++) {
        tui_rect cell = tui_rect_make(x + i * (LS_GLYPH_COLS + 1), y, LS_GLYPH_COLS, LS_GLYPH_ROWS);
        if (t[i] == '-') { for (int k = 0; k < LS_GLYPH_COLS; k++) tui_put_char(sf, clip, cell.x + k, y + 2, '-', attr); }
        else ls_glyph_draw(sf, cell, t[i], attr);
    }
}

static void scene(ls_compass_scene_t *sc, bool with_df)
{
    memset(sc, 0, sizeof(*sc));
    sc->valid = s_r.valid;
    sc->heading = s_spring.started && isfinite(s_spring.angle) ? s_spring.angle : 0;
    sc->pitch = s_r.back_axis ? 0 : s_steady.started ? s_steady.pitch : s_r.pitch;
    sc->roll = s_r.back_axis ? 0 : s_steady.started ? s_steady.roll : s_r.roll;
    sc->seconds = (float)(esp_timer_get_time() / 1e6);
    sc->simple = s_simple;
    sc->fill = (uint8_t)s_fill;
    sc->white_letters = s_white_letters;
    double b;
    sc->target = target_nav(&b, NULL, NULL, 0) ? to_dial((float)b) : NAN;
    sc->sun = to_dial(sun_azimuth(NULL));
    sc->lock = s_lock;
    sc->df = sc->df_spread = NAN;
    if (with_df) {
        for (int i = 0; i < LS_DF_BINS && !sc->lobe; i++) if (s_lobe[i] > 0) sc->lobe = s_lobe;
        if (s_est.valid) { sc->df = to_dial(s_est.bearing); sc->df_spread = s_est.spread; }
    }
}

/* The interference warning, as an instrument's caution light: a small
   steady lamp in the dial's corner rather than anything laid over the
   compass. It lights after the field has been off for two seconds and
   stays lit three (ls_compass_bend_step), so it never blinks. */
#define MAG_LAMP_W 9
static void mag_lamp(tui_surface *sf, tui_rect a, int x, int y)
{
    const uint8_t rim = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t lamp = TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT);
    tui_put_str(sf, a, x, y, "+-------+", rim);
    tui_put_str(sf, a, x, y + 1, "|", rim);
    tui_put_str(sf, a, x + 1, y + 1, " ! MAG ", lamp);
    tui_put_str(sf, a, x + 8, y + 1, "|", rim);
    tui_put_str(sf, a, x, y + 2, "+-------+", rim);
}

/* Pages without the big readout carry the lamp in the dial's corner. */
static void mag_caution(tui_surface *sf, tui_rect dial)
{
    if (s_r.interference && dial.w >= 12 && dial.h >= 5) mag_lamp(sf, dial, dial.x, dial.y);
}

/* The heading readout: three big digits, T or M, and where that is. */
static void readout(tui_surface *sf, tui_rect a, int y)
{
    const float h = shown_heading();
    const int w = 3 * (LS_GLYPH_COLS + 1) - 1;
    const int x = a.x + (a.w - w - 5) / 2;
    big_number(sf, a, x, y, h, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    /* Beside the number it qualifies. */
    if (s_r.interference && x - a.x >= MAG_LAMP_W + 2) mag_lamp(sf, a, x - MAG_LAMP_W - 2, y + 1);
    tui_put_str(sf, a, x + w + 1, y, ref(), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, a, x + w + 1, y + 2, cardinal(h), TUI_ATTR(ls_compass_letter_ink(false, s_white_letters), TUI_BLACK));
}

/* THE TUI STACK IS 6 KB, IN DRAM, and COMPASS was 600 bytes from its end:
   with every page inlined into draw, all their locals shared one 1680-byte
   frame. Pages are kept out of line and their text buffers are static. */
__attribute__((noinline)) static void info_lines(tui_surface *sf, tui_rect a, int y, int rows)
{
    EXT_RAM_BSS_ATTR static char line[120];
    const float h = shown_heading();
    int n = 0;
#define LINE(attr) do { if (n < rows) ls_safe_line(sf, a, y + n++, line, (attr)); } while (0)
    if (!s_r.calibrated)
        { snprintf(line, sizeof(line), "NOT CALIBRATED: MORE > CALIBRATE"); LINE(TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)); }
    else if (s_r.interference)
        { snprintf(line, sizeof(line), "MAG CAUTION  field %+.0f%%  dip %+.0f", s_r.strength_off * 100, s_r.dip_off);
          LINE(TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)); }
    snprintf(line, sizeof(line), "%s NORTH  %s%s", true_mode() ? "TRUE" : "MAGNETIC",
             !s_r.valid ? "waiting for a heading" : s_r.back_axis ? "sighting the back" : "board top",
             true_mode() && s_r.position_saved ? "  (saved position)" : "");
    LINE(TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    /* Strength against the calibration's own figure when there is one:
       that is what the interference check uses (ls_compass_apply_model). */
    if (isfinite(s_r.declination)) snprintf(line, sizeof(line), "DECL %+.1f  DIP %.0f/%.0f  %.0f/%.0f uT",
                                            s_r.declination, s_r.dip, s_r.expected_dip, s_r.field_ut,
                                            isfinite(s_r.cal_ut) ? s_r.cal_ut : s_r.expected_ut);
    else snprintf(line, sizeof(line), "DIP %.0f  %.0f uT  true north needs a GPS fix once", s_r.dip, s_r.field_ut);
    LINE(LS_ATTR_DIM);
    float el = NAN; const float sun = sun_azimuth(&el);
    if (isfinite(sun)) { snprintf(line, sizeof(line), "SUN %03.0f %s  %.0f deg up", to_dial(sun), ref(), el); LINE(TUI_ATTR(TUI_YELLOW, TUI_BLACK)); }
    snprintf(line, sizeof(line), "BACK %03.0f  TILT %.0f", isfinite(h) ? fmodf(h + 180.0f, 360.0f) : 0.0f, s_r.tilt);
    LINE(LS_ATTR_DIM);
    if (isfinite(s_lock) && isfinite(h)) {
        const float off = fmodf(s_lock - h + 540.0f, 360.0f) - 180.0f;
        snprintf(line, sizeof(line), "LOCK %03.0f  steer %s %.0f", s_lock, off >= 0 ? "right" : "left", fabsf(off));
        LINE(TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
    }
    double tb, tm; char tn[40];
    if (target_nav(&tb, &tm, tn, sizeof(tn))) {
        char d[24]; distance_text(tm, d, sizeof(d));
        snprintf(line, sizeof(line), "%.20s  %03.0f %s  %s", tn, to_dial((float)tb), ref(), d);
        LINE(TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }
#undef LINE
}

static tui_rect art_and_side(tui_rect body, tui_rect *side, int side_rows)
{
    if (body.w > body.h * 2) {
        tui_rect art = tui_rect_make(body.x, body.y, body.w * 11 / 20, body.h);
        *side = tui_rect_make(art.x + art.w + 1, body.y, body.w - art.w - 2, body.h);
        return art;
    }
    tui_rect art = tui_rect_make(body.x, body.y, body.w, body.h - side_rows);
    *side = tui_rect_make(body.x + 1, body.y + art.h, body.w - 2, side_rows);
    return art;
}

__attribute__((noinline)) static void draw_dial(tui_surface *sf, tui_rect body)
{
    ls_compass_scene_t sc; scene(&sc, false);
    ls_btn_t buttons[] = { { "LOCK", isfinite(s_lock) ? "CLEAR" : "HERE", 'l', isfinite(s_lock), !s_r.valid },
                           { "MARK", "NOTE", 'm', false, false },
                           { "CAL", "SETUP", 'k', !s_r.calibrated, false } };
    tui_rect side;
    const int bar_h = ls_btn_raised_height(body, 3);
    if (s_simple) {
        ls_scr_labs_classic_compass(sf, tui_rect_make(body.x, body.y, body.w, body.h - bar_h));
        mag_caution(sf, tui_rect_make(body.x, body.y, body.w, body.h - bar_h));
        ls_btn_bar_raised(sf, tui_rect_make(body.x, body.y + body.h - bar_h, body.w, bar_h), buttons, 3, button_focus);
        return;
    }
    tui_rect art = art_and_side(tui_rect_make(body.x, body.y, body.w, body.h - bar_h), &side, 15);
    ls_compass_art_draw(sf, art, &sc);
    readout(sf, side, side.y + 1);
    info_lines(sf, side, side.y + 7, side.h - 7);
    ls_btn_bar_raised(sf, tui_rect_make(body.x, body.y + body.h - bar_h, body.w, bar_h), buttons, 3, button_focus);
}

/* ---------------------------------------------------------------- FIND -- */

/* Every radio slot and channel is a track with its own sweep: track
   slot * LS_DFS_CHANNELS + channel. Slot 0's are tagged 1-8, slot 1's A-H. */
#define TRACKS (LS_DFS_SLOTS * LS_DFS_CHANNELS)
EXT_RAM_BSS_ATTR static ls_df_sweep_t s_sw[TRACKS];
EXT_RAM_BSS_ATTR static ls_df_estimate_t s_te[TRACKS];
EXT_RAM_BSS_ATTR static struct { float peak, last; int64_t us, last_us, clip_us; } s_tlev[TRACKS];
static int s_sel;                         /* the track whose lobe is drawn */
EXT_RAM_BSS_ATTR static ls_dfs_status_t s_dfs2;
EXT_RAM_BSS_ATTR static ls_df_log_t s_log;
/* Channel lists, as the operator set them. */
EXT_RAM_BSS_ATTR static uint32_t s_ch[LS_DFS_SLOTS][LS_DFS_CHANNELS];
static int s_nch[LS_DFS_SLOTS] = { 1, 1 };
static ls_dfs_t s_src2 = LS_DFS_COUNT;    /* the second radio, COUNT when off */
static float s_df_offset2 = NAN;
/* What a beacon calibration borrowed, to give back when it ends. */
static ls_dfs_t s_cal_prev_src = LS_DFS_COUNT;
/* Pictures and the log. */
enum { V_DIAL, V_RADAR, V_HEAT, V_COUNT };
static const char *const VIEW_NAMES[V_COUNT] = { "DIAL", "RADAR", "HEAT" };
static int s_view;
static bool s_logmode;
static int s_log_pick = -1;               /* row picked in the log, 0 newest */
static int s_log_top;
static tui_rect s_log_rows;               /* where the rows were drawn, for touch */
/* HEAT's history of the shown track: a row every HIST_US. */
#define HIST_ROWS 40
#define HIST_US   2000000
EXT_RAM_BSS_ATTR static uint8_t s_hist[HIST_ROWS][LS_DF_BINS];
static int s_hist_n, s_hist_head;
static int64_t s_hist_us;

/* Settings: each an index into its table, stored as that index. */
enum { O_HOLD, O_DECAY, O_FORGET, O_THRESH, O_SPIKE, O_FADE, O_DWELL, O_PEAKS, O_TUNED, O_VIEW = O_TUNED, O_SRC2, O_SRC1,
       O_FILL, O_TRAIL, O_LETTERS };
static const float HOLD_S[] = { 0, 2, 5, 10, 30 };
static const float DECAY_DB[] = { 0, 0.5f, 1, 2, 5, 10 };
static const float FORGET_S[] = { 30, 60, 180, 600 };
static const float THRESH_DB[] = { 3, 6, 10, 15, 20 };
static const float SPIKE_N[] = { 1, 2, 3, 5 };
static const float FADE_S[] = { 5, 15, 30, 60, 120 };
static const float DWELL_MS[] = { 200, 400, 800, 1500 };
static const float PEAKS_DB[] = { 0, 3, 6, 10 };
static const struct { const float *v; int n, dflt; const char *name, *unit, *zero; } OPT[O_TUNED] = {
    { HOLD_S, 5, 2, "Hold each peak", "s", "no hold" },
    { DECAY_DB, 6, 2, "Then let it fall", "dB/s", "never: hold forever" },
    { FORGET_S, 4, 2, "Forget a direction after", "s", NULL },
    { THRESH_DB, 5, 1, "A hit stands over noise by", "dB", NULL },
    { SPIKE_N, 4, 1, "Believe a hit after", "reads", NULL },
    { FADE_S, 5, 2, "Hit blips last", "s", NULL },
    { DWELL_MS, 4, 1, "Scan dwell per channel", "ms", NULL },
    { PEAKS_DB, 4, 2, "Mark other lobes this tall", "dB", "off" },
};
static int s_opt[O_TUNED];
#define OPTV(i) (OPT[i].v[s_opt[i]])

static char track_tag(int t) { return t < LS_DFS_CHANNELS ? (char)('1' + t) : (char)('A' + t - LS_DFS_CHANNELS); }
static const ls_dfs_status_t *slot_status(int k) { return k ? &s_dfs2 : &s_dfs; }
static bool track_live(int t)
{
    const ls_dfs_status_t *st = slot_status(t / LS_DFS_CHANNELS);
    return st->active && t % LS_DFS_CHANNELS < (st->channels ? st->channels : 1);
}
static uint32_t track_freq(int t)
{
    const ls_dfs_status_t *st = slot_status(t / LS_DFS_CHANNELS);
    return st->tunable ? st->freqs[t % LS_DFS_CHANNELS] : 0;
}
static int live_tracks(void) { int n = 0; for (int t = 0; t < TRACKS; t++) n += track_live(t); return n; }
/* Seconds between visits to one channel of a scanning slot: a dwell on
   each, and about 40 ms to retune (measured on the SX1262). */
static float scan_lap_s(int k)
{
    const ls_dfs_status_t *st = slot_status(k);
    return st->active && st->tunable && st->channels > 1 ? st->channels * (OPTV(O_DWELL) + 50.0f) / 1000.0f : 0.0f;
}
/* A level is fresh, and a hit still going, for longer than one lap: a
   transmitter that never stops is one hit, not one a lap. */
static float lap_hold_s(int k) { return fmaxf(1.5f, 1.3f * scan_lap_s(k)); }
static const ls_df_hit_cfg_t *hit_cfg(void)
{
    static ls_df_hit_cfg_t c;
    c.threshold_db = OPTV(O_THRESH);
    c.gap_s = fmaxf(lap_hold_s(0), lap_hold_s(1));
    return &c;
}
static bool hit_believed(const ls_df_hit_t *h) { return h->count >= (int)OPTV(O_SPIKE); }

static void clear_track(int t)
{
    ls_df_clear(&s_sw[t]); memset(&s_te[t], 0, sizeof(s_te[t]));
    s_tlev[t].peak = s_tlev[t].last = NAN; s_tlev[t].us = s_tlev[t].last_us = s_tlev[t].clip_us = 0;
    ls_df_log_reset_track(&s_log, t);
}
static void clear_all(void)
{
    for (int t = 0; t < TRACKS; t++) clear_track(t);
    memset(&s_est, 0, sizeof(s_est)); s_peak = NAN;
    memset(s_lobe, 0, sizeof(s_lobe));
    s_hist_n = s_hist_head = 0;
}
static void clear_slot(int k) { for (int c = 0; c < LS_DFS_CHANNELS; c++) clear_track(k * LS_DFS_CHANNELS + c); }

/* Somewhere each radio can tune, for a list that suits none of it. */
static uint32_t default_freq(ls_dfs_t src)
{
    static const uint32_t TRY[] = { 915000000u, 433920000u, 462562500u, 2440000000u };
    for (unsigned i = 0; i < sizeof(TRY) / sizeof(TRY[0]); i++) if (ls_dfs_in_range(src, TRY[i])) return TRY[i];
    return 915000000u;
}

static void load_df_offset(void)
{
    float d;
    s_df_offset = settings_get_df_offset((int)s_dfs_source, (int)s_method, &d) ? d : NAN;
    s_df_offset2 = s_src2 < LS_DFS_COUNT && settings_get_df_offset((int)s_src2, (int)s_method, &d) ? d : NAN;
}

static void load_find_settings(void)
{
    for (int i = 0; i < O_TUNED; i++) {
        s_opt[i] = settings_get_df_option(i, OPT[i].dflt);
        if (s_opt[i] < 0 || s_opt[i] >= OPT[i].n) s_opt[i] = OPT[i].dflt;
    }
    s_view = settings_get_df_option(O_VIEW, V_DIAL);
    if (s_view < 0 || s_view >= V_COUNT) s_view = V_DIAL;
    s_fill = settings_get_df_option(O_FILL, LS_COMPASS_FILL_LIGHT);
    if (s_fill < 0 || s_fill >= LS_COMPASS_FILL_COUNT) s_fill = LS_COMPASS_FILL_LIGHT;
    s_trail = settings_get_df_option(O_TRAIL, 1) != 0;
    s_white_letters = settings_get_df_option(O_LETTERS, 0) == 1;
    const int src1 = settings_get_df_option(O_SRC1, 0);
    if (src1 >= 1 && src1 <= LS_DFS_COUNT) s_dfs_source = (ls_dfs_t)(src1 - 1);
    const int src2 = settings_get_df_option(O_SRC2, 0);
    s_src2 = src2 >= 1 && src2 <= LS_DFS_COUNT ? (ls_dfs_t)(src2 - 1) : LS_DFS_COUNT;
    for (int k = 0; k < LS_DFS_SLOTS; k++) {
        const int n = settings_get_df_channels(k, s_ch[k], LS_DFS_CHANNELS);
        if (n > 0) s_nch[k] = n;
        else { s_ch[k][0] = k ? 433920000u : s_dfs_freq; s_nch[k] = 1; }
    }
    s_dfs_freq = s_ch[0][0];
}

/* Start what the operator chose: slot 0 always, slot 1 when set and able. */
static void find_start(void)
{
    s_find_on = !ls_dfs_unavailable(s_dfs_source) && ls_dfs_select_slot(0, s_dfs_source, s_ch[0][0]);
    if (s_find_on) ls_dfs_set_channels(0, s_ch[0], s_nch[0], (uint32_t)OPTV(O_DWELL));
    if (s_src2 < LS_DFS_COUNT && s_cal.phase == LS_DF_CAL_OFF && !ls_dfs_unavailable(s_src2) &&
        ls_dfs_select_slot(1, s_src2, s_ch[1][0]))
        ls_dfs_set_channels(1, s_ch[1], s_nch[1], (uint32_t)OPTV(O_DWELL));
    else ls_dfs_stop_slot(1);
}

static void find_stop(void)
{
    ls_dfs_stop_slot(0); ls_dfs_stop_slot(1);
    s_find_on = false;
}

/* The level a reading is filed at while calibrating: the strongest of the
   last BEACON_HOLD_US, so the beacon's gaps are not taken for nulls. */
static float beacon_hold(float level, int64_t now)
{
    memmove(s_recent + 1, s_recent, sizeof(s_recent[0]) * (sizeof(s_recent) / sizeof(s_recent[0]) - 1));
    s_recent[0].us = now; s_recent[0].level = level;
    if (s_recent_n < (int)(sizeof(s_recent) / sizeof(s_recent[0]))) s_recent_n++;
    for (int i = 1; i < s_recent_n && now - s_recent[i].us <= BEACON_HOLD_US; i++)
        if (s_recent[i].level > level) level = s_recent[i].level;
    return level;
}

/* A track's PEAK readout, held and let fall the way its bins are. */
static float track_peak(int t, int64_t now)
{
    float p = s_tlev[t].peak;
    if (!isfinite(p)) return NAN;
    const float hold = OPTV(O_HOLD), decay = OPTV(O_DECAY);
    const float past = (float)(now - s_tlev[t].us) / 1e6f - hold;
    if (decay > 0 && past > 0) p -= decay * past;
    return isfinite(s_tlev[t].last) && s_tlev[t].last > p ? s_tlev[t].last : p;
}

__attribute__((noinline)) static void update_find(void)
{
    ls_dfs_poll_slot(0, &s_dfs);
    ls_dfs_poll_slot(1, &s_dfs2);
    const int64_t now = esp_timer_get_time();
    const float h = to_true(shown_heading());
    const bool cal = s_cal.phase != LS_DF_CAL_OFF;

    /* Turning faster than the receiver reads leaves bins empty. */
    if (isfinite(h) && isfinite(s_turn_prev) && now > s_turn_prev_us) {
        const float dt = (float)(now - s_turn_prev_us) / 1e6f;
        const float d = fabsf(fmodf(h - s_turn_prev + 540.0f, 360.0f) - 180.0f);
        if (dt > 0.01f && dt < 1.0f) s_turn_rate += (d / dt - s_turn_rate) * fminf(1.0f, dt / 0.5f);
    }
    s_turn_prev = h; s_turn_prev_us = now;

    EXT_RAM_BSS_ATTR static ls_dfs_reading_t rd[32];
    const int n = s_find_on ? ls_dfs_take(rd, 32) : 0;
    for (int i = 0; i < n; i++) {
        const int t = rd[i].slot * LS_DFS_CHANNELS + rd[i].channel;
        if (t < 0 || t >= TRACKS || (cal && t)) continue;
        const float level = cal ? beacon_hold(rd[i].level, rd[i].us) : rd[i].level;
        /* A clipped reading says only "loud": the converter flattened
           the very difference between directions being looked for. It is
           logged, and kept out of the sweep. */
        const bool clipped = rd[i].flags & LS_DFS_READ_OVERLOAD;
        if (clipped) s_tlev[t].clip_us = rd[i].us;
        if (isfinite(h) && !clipped) ls_df_add(&s_sw[t], h, level, rd[i].us);
        uint8_t flags = 0;
        if (rd[i].flags & LS_DFS_READ_OVERLOAD) flags |= LS_DF_HIT_OVERLOAD;
        if (s_turn_rate > LS_DF_CAL_TURN_MAX * 2) flags |= LS_DF_HIT_FAST;
        if (s_te[t].coverage >= 180 && s_te[t].contrast < 3.0f) flags |= LS_DF_HIT_FLAT;
        ls_df_log_feed(&s_log, hit_cfg(), t, rd[i].freq_hz, rd[i].level, h, flags, rd[i].us);
        s_tlev[t].last = rd[i].level; s_tlev[t].last_us = rd[i].us;
        if (!isfinite(track_peak(t, rd[i].us)) || rd[i].level >= track_peak(t, rd[i].us)) {
            s_tlev[t].peak = rd[i].level; s_tlev[t].us = rd[i].us;
        }
    }

    /* Held levels let go as the settings say, on every track. */
    const ls_df_decay_t decay = { OPTV(O_HOLD), OPTV(O_DECAY) };
    const int64_t forget = (int64_t)(OPTV(O_FORGET) * 1e6f);
    for (int t = 0; t < TRACKS; t++) {
        if (!s_sw[t].samples) { memset(&s_te[t], 0, sizeof(s_te[t])); continue; }
        ls_df_decay(&s_sw[t], now, &decay);
        ls_df_age(&s_sw[t], now, forget);
        ls_df_estimate(&s_sw[t], s_method, &s_te[t]);
        const float off = t < LS_DFS_CHANNELS ? s_df_offset : s_df_offset2;
        if (!cal && s_te[t].valid) s_te[t].bearing = ls_df_cal_apply(s_te[t].bearing, off);
    }
    if (!track_live(s_sel)) s_sel = 0;
    const ls_df_sweep_t *sw = &s_sw[s_sel];
    s_est = s_te[s_sel];
    s_peak = track_peak(s_sel, now);
    /* The lobe spans at least 12 dB below the strongest bearing: stretched
       over whatever the spread was, a 2 dB wobble filled the whole scale. */
    const float span = fmaxf(12.0f, sw->top - sw->floor);
    for (int i = 0; i < LS_DF_BINS; i++)
        s_lobe[i] = sw->count[i] ? fmaxf(0.02f, fminf(1.0f, (sw->level[i] - (sw->top - span)) / span)) : 0;

    /* HEAT's history: the shown track's circle, every HIST_US. */
    if (sw->samples && now - s_hist_us >= HIST_US) {
        s_hist_us = now;
        const float noise = ls_df_log_noise(&s_log, s_sel);
        for (int i = 0; i < LS_DF_BINS; i++)
            s_hist[s_hist_head][i] = ls_df_heat_cell(sw->level[i] - (isfinite(noise) ? noise : sw->floor), sw->count[i] > 0);
        s_hist_head = (s_hist_head + 1) % HIST_ROWS;
        if (s_hist_n < HIST_ROWS) s_hist_n++;
    }

    if (s_cal.phase == LS_DF_CAL_TURN) {
        /* A full circle heard well enough is one measurement; start the
           next circle from nothing so they stay independent. */
        if (ls_df_cal_circle(&s_cal, &s_est)) {
            ls_df_cal_result_t r; ls_df_cal_result(&s_cal, &r);
            char t[64]; snprintf(t, sizeof(t), "Circle %d: %+.0f deg%s", r.circles,
                                 s_cal.offset[(s_cal.next + LS_DF_CAL_CIRCLES - 1) % LS_DF_CAL_CIRCLES],
                                 r.ready ? "  - agree, SAVE CAL" : "");
            say(t);
            clear_track(0); memset(&s_est, 0, sizeof(s_est)); s_peak = NAN;
        }
    }
}

/* The hits worth a blip: believed, heard with a heading, not yet faded. */
EXT_RAM_BSS_ATTR static ls_compass_blip_t s_blips[LS_COMPASS_BLIPS];
EXT_RAM_BSS_ATTR static ls_compass_mark_t s_marks[LS_COMPASS_MARKS];
EXT_RAM_BSS_ATTR static float s_peaks[LS_COMPASS_PEAKS];

static int build_blips(bool dial_ref)
{
    const int64_t now = esp_timer_get_time();
    const int64_t fade = (int64_t)(OPTV(O_FADE) * 1e6f);
    int n = 0;
    for (int i = 0; i < ls_df_log_count(&s_log) && n < LS_COMPASS_BLIPS; i++) {
        const ls_df_hit_t *h = ls_df_log_at(&s_log, i);
        if (now - h->last_us > fade) continue;
        if (!hit_believed(h) || !isfinite(h->heading)) continue;
        s_blips[n].bearing = dial_ref ? to_dial(h->heading) : h->heading;
        s_blips[n].strength = h->snr / LS_DF_VIEW_SCALE_DB;
        s_blips[n].age = (float)(now - h->last_us) / (float)fade;
        n++;
    }
    return n;
}

/* How lately the shown track heard each direction, 1 just now to 0 at
   RECENT_US: the directions the turn has covered, fading behind it. */
#define RECENT_US 4000000
EXT_RAM_BSS_ATTR static float s_recent_glow[LS_DF_BINS];
static bool hearing_now(void)
{
    return isfinite(s_tlev[s_sel].last) && esp_timer_get_time() - s_tlev[s_sel].last_us < 1500000;
}
static const float *recent_glow(void)
{
    const int64_t now = esp_timer_get_time();
    const ls_df_sweep_t *sw = &s_sw[s_sel];
    bool any = false;
    for (int i = 0; i < LS_DF_BINS; i++) {
        const int64_t age = sw->count[i] ? now - sw->seen_us[i] : RECENT_US;
        s_recent_glow[i] = age < RECENT_US ? 1.0f - (float)age / RECENT_US : 0.0f;
        any |= s_recent_glow[i] > 0;
    }
    return any ? s_recent_glow : NULL;
}

static void find_scene(ls_compass_scene_t *sc)
{
    scene(sc, true);
    sc->facing = hearing_now();
    sc->recent = s_trail ? recent_glow() : NULL;
    sc->blips = s_blips; sc->blip_count = build_blips(true);
    int m = 0;
    for (int t = 0; t < TRACKS && m < LS_COMPASS_MARKS; t++)
        if (t != s_sel && track_live(t) && s_te[t].valid) { s_marks[m].bearing = to_dial(s_te[t].bearing); s_marks[m].tag = track_tag(t); m++; }
    sc->marks = s_marks; sc->mark_count = m;
    int p = 0;
    if (OPTV(O_PEAKS) > 0 && s_method == LS_DF_PEAK && s_est.valid) {
        EXT_RAM_BSS_ATTR static ls_df_peak_t pk[LS_COMPASS_PEAKS + 1];
        const int n = ls_df_peaks(&s_sw[s_sel], OPTV(O_PEAKS), pk, LS_COMPASS_PEAKS + 1);
        const float off = s_sel < LS_DFS_CHANNELS ? s_df_offset : s_df_offset2;
        for (int i = 0; i < n && p < LS_COMPASS_PEAKS; i++) {
            const float b = ls_df_cal_apply(pk[i].bearing, off);
            /* The main lobe already carries the estimate. */
            if (fabsf(fmodf(b - s_est.bearing + 540.0f, 360.0f) - 180.0f) < 15.0f) continue;
            s_peaks[p++] = to_dial(b);
        }
    }
    sc->peaks = s_peaks; sc->peak_count = p;
    if (s_logmode && s_log_pick >= 0) {
        const ls_df_hit_t *h = ls_df_log_at(&s_log, s_log_pick);
        if (h && isfinite(h->heading)) sc->lock = to_dial(h->heading);
    }
}

/* The picture: DIAL, RADAR or HEAT. */
__attribute__((noinline)) static void draw_find_view(tui_surface *sf, tui_rect art)
{
    EXT_RAM_BSS_ATTR static ls_compass_scene_t sc;
    find_scene(&sc);
    /* Calibrating, the marked beacon bearing is the lock wedge on the
       bezel: the lobe's peak should come round to it. */
    if (s_cal.phase == LS_DF_CAL_TURN) sc.lock = to_dial(s_cal.mark);
    if (s_view == V_DIAL || s_cal.phase != LS_DF_CAL_OFF) {
        ls_compass_art_draw(sf, art, &sc);
        mag_caution(sf, art);
        return;
    }
    const float heading = to_true(shown_heading());
    const ls_df_hit_t *pick = s_logmode && s_log_pick >= 0 ? ls_df_log_at(&s_log, s_log_pick) : NULL;
    const float pick_b = pick && isfinite(pick->heading) ? pick->heading : NAN;
    if (s_view == V_RADAR) {
        EXT_RAM_BSS_ATTR static ls_df_radar_t r;
        static const uint8_t HUE[] = { TUI_CYAN, TUI_YELLOW, TUI_MAGENTA, TUI_BLUE, TUI_WHITE };
        memset(&r, 0, sizeof(r));
        r.heading = heading; r.pick = pick_b;
        r.facing = sc.facing; r.recent = sc.recent;
        r.lobe = sc.lobe;
        r.blips = s_blips; r.blip_count = build_blips(false);
        for (int t = 0; t < TRACKS && r.cones < 16; t++) {
            if (!track_live(t)) continue;
            ls_df_cone_t *c = &r.cone[r.cones];
            c->valid = s_te[t].valid; c->bearing = s_te[t].bearing; c->spread = s_te[t].spread;
            const float noise = ls_df_log_noise(&s_log, t);
            c->over_db = isfinite(noise) ? s_sw[t].top - noise : s_te[t].contrast;
            c->colour = t == s_sel ? TUI_RED : HUE[r.cones % 5];
            c->tag = track_tag(t); c->shown = t == s_sel;
            r.cones++;
        }
        ls_df_radar_draw(sf, art, &r);
        mag_caution(sf, tui_rect_make(art.x, art.y + 1, art.w, art.h - 1));
        return;
    }
    /* HEAT: every live channel's circle now, then the shown one over time. */
    EXT_RAM_BSS_ATTR static uint8_t cells[HIST_ROWS][LS_DF_BINS];
    EXT_RAM_BSS_ATTR static char names[HIST_ROWS][16];
    EXT_RAM_BSS_ATTR static const char *labels[HIST_ROWS];
    int rows = 0, selected = -1;
    for (int t = 0; t < TRACKS; t++) {
        if (!track_live(t)) continue;
        const float noise = ls_df_log_noise(&s_log, t);
        for (int i = 0; i < LS_DF_BINS; i++)
            cells[rows][i] = ls_df_heat_cell(s_sw[t].level[i] - (isfinite(noise) ? noise : s_sw[t].floor), s_sw[t].count[i] > 0);
        const uint32_t f = track_freq(t);
        if (f) snprintf(names[rows], sizeof(names[0]), "%c %.4f", track_tag(t), f / 1e6);
        else snprintf(names[rows], sizeof(names[0]), "%c %.9s", track_tag(t), ls_dfs_name(slot_status(t / LS_DFS_CHANNELS)->source));
        labels[rows] = names[rows];
        if (t == s_sel) selected = rows;
        rows++;
    }
    /* One descriptor for both grids, and not on the TUI task's stack. */
    EXT_RAM_BSS_ATTR static ls_df_heat_t hm;
    hm = (ls_df_heat_t){ .title = "HEAT  bearing by channel, now", .rows = rows, .cells = cells[0], .labels = labels,
                         .selected = selected, .heading = heading, .pick = pick_b };
    const int used = ls_df_heat_draw(sf, art, &hm);
    const tui_rect rest = tui_rect_make(art.x, art.y + used + 1, art.w, art.h - used - 1);
    if (rest.h < 6 || !s_hist_n) return;
    for (int r = 0; r < s_hist_n; r++) {
        memcpy(cells[r], s_hist[(s_hist_head - 1 - r + 2 * HIST_ROWS) % HIST_ROWS], LS_DF_BINS);
        snprintf(names[r], sizeof(names[0]), "-%ds", (int)((int64_t)r * HIST_US / 1000000));
        labels[r] = names[r];
    }
    EXT_RAM_BSS_ATTR static char title[48];
    snprintf(title, sizeof(title), "HEAT  channel %c over time, newest on top", track_tag(s_sel));
    hm = (ls_df_heat_t){ .title = title, .rows = s_hist_n, .cells = cells[0], .labels = labels, .selected = -1,
                         .heading = heading, .pick = pick_b };
    ls_df_heat_draw(sf, rest, &hm);
}

/* The Flipper's DF Beacon runs on 915 MHz (the default, a continuous
   low-power transmitter in the US band that allows one) or 433.92 MHz. */
#define BEACON_915 915000000u
#define BEACON_433 433920000u
static uint32_t s_cal_hz = BEACON_915;

/* The receiver a beacon calibration uses. At 915 the board's own SX1262 is
   best: its antenna is matched for 915, and it is always there. At 433 the
   keyboard's CC1101 is made for the band, an RTL-SDR hears it well, and the
   SX1262 still hears a beacon a few metres away on the external port. */
static ls_dfs_t cal_receiver(uint32_t hz)
{
    static const ls_dfs_t at915[] = { LS_DFS_LORA, LS_DFS_RTL, LS_DFS_CC1101 };
    static const ls_dfs_t at433[] = { LS_DFS_CC1101, LS_DFS_RTL, LS_DFS_LORA };
    const ls_dfs_t *order = hz == BEACON_915 ? at915 : at433;
    for (unsigned i = 0; i < 3; i++)
        if (!ls_dfs_unavailable(order[i]) && ls_dfs_in_range(order[i], hz)) return order[i];
    return LS_DFS_COUNT;
}

/* The LoRa radio calibrates at 433 on the external (MMCX) port, where a
   433 MHz antenna goes, and at 915 on the internal one; the stored choice is
   put back when calibration ends. -1 is nothing to put back. */
static int s_ant_restore = -1;

static void cal_antenna(bool calibrating)
{
    const bool want_ext = s_cal_hz != BEACON_915;
    if (calibrating && s_dfs_source == LS_DFS_LORA && s_ant_restore < 0) {
        s_ant_restore = ls_board_hw_antenna_is_external() ? 1 : 0;
        if (s_ant_restore != (int)want_ext) ls_board_hw_antenna_external(want_ext);
    } else if (!calibrating && s_ant_restore >= 0) {
        if (s_ant_restore != (int)want_ext) ls_board_hw_antenna_external(s_ant_restore == 1);
        s_ant_restore = -1;
    }
}

static void cal_end(void)
{
    const bool was = s_cal.phase != LS_DF_CAL_OFF;
    s_cal.phase = LS_DF_CAL_OFF;
    cal_antenna(false);
    /* Back to the radio and channels in use before, and the second radio. */
    if (was && s_cal_prev_src < LS_DFS_COUNT) {
        s_dfs_source = s_cal_prev_src; s_cal_prev_src = LS_DFS_COUNT;
        s_dfs_freq = s_ch[0][0];
        if (s_find_on) { find_stop(); find_start(); }
        clear_all();
        load_df_offset();
    }
}

/* Calibration against the Flipper's DF BEACON. */
static void cal_start(uint32_t hz)
{
    const ls_dfs_t src = cal_receiver(hz);
    if (src == LS_DFS_COUNT) {
        say(hz == BEACON_915 ? "No radio here can hear 915 MHz" : "No radio here can hear 433 MHz");
        return;
    }
    if (s_cal_prev_src == LS_DFS_COUNT) s_cal_prev_src = s_dfs_source;
    find_stop();
    s_cal_hz = hz;
    s_dfs_source = src; s_dfs_freq = hz;
    s_find_on = ls_dfs_select_slot(0, s_dfs_source, s_dfs_freq);
    if (s_find_on) ls_dfs_set_channels(0, &s_dfs_freq, 1, (uint32_t)OPTV(O_DWELL));
    s_sel = 0;
    clear_all();
    ls_df_cal_begin(&s_cal);
    s_recent_n = 0;
    cal_antenna(true);
    load_df_offset();
    say(!s_find_on ? "That radio would not start" :
        s_dfs_source == LS_DFS_LORA && hz != BEACON_915 ? "LoRa on the external port: start the beacon, aim at it" :
                                                          "Start the beacon, then aim the board's top at it");
}

static void cal_step(void)
{
    if (s_cal.phase == LS_DF_CAL_AIM) {
        const float h = to_true(shown_heading());
        if (!isfinite(h)) { say("No heading yet"); return; }
        ls_df_cal_mark(&s_cal, h);
        clear_track(0); memset(&s_est, 0, sizeof(s_est)); s_peak = NAN;
        char t[64]; snprintf(t, sizeof(t), "Beacon at %03.0f. Turn slowly, full circles", h);
        say(t);
        return;
    }
    ls_df_cal_result_t r; ls_df_cal_result(&s_cal, &r);
    if (!r.ready) return;
    settings_set_df_offset((int)s_dfs_source, (int)s_method, r.offset);
    const ls_dfs_t calibrated = s_dfs_source;
    cal_end();
    char t[80]; snprintf(t, sizeof(t), "Saved: %s %s bearings corrected by %+.1f deg", ls_dfs_name(calibrated),
                         s_method == LS_DF_PEAK ? "PEAK" : "NULL", -r.offset);
    say(t);
}

/* Back to aiming, for a mark that was off: its circles go with it. */
static void cal_remark(void)
{
    ls_df_cal_begin(&s_cal);
    s_recent_n = 0;
    clear_track(0); memset(&s_est, 0, sizeof(s_est)); s_peak = NAN;
    say("Aim the board's top at the beacon again, then MARK");
}

static void cal_cancel(void)
{
    cal_end();
    say("Calibration cancelled; the saved correction is unchanged");
}

/* The side panel while calibrating. */
__attribute__((noinline)) static int cal_lines(tui_surface *sf, tui_rect side, int y)
{
    EXT_RAM_BSS_ATTR static char line[120];
    const uint8_t head = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK), dim = LS_ATTR_DIM;
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK), warn = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    snprintf(line, sizeof(line), "BEACON CALIBRATION  %.2f MHz  %s%s  %s", s_dfs_freq / 1e6,
             ls_dfs_name(s_dfs_source), s_dfs_source == LS_DFS_LORA && s_cal_hz != BEACON_915 ? " (EXT ANT)" : "",
             s_method == LS_DF_PEAK ? "PEAK" : "NULL");
    ls_safe_line(sf, side, y++, line, head);
    if (s_cal.phase == LS_DF_CAL_AIM) {
        snprintf(line, sizeof(line), "1 Flipper: LakeShark > DF Beacon, %s > OK", s_cal_hz == BEACON_915 ? "915" : "433");
        ls_safe_line(sf, side, y++, line, dim);
        ls_safe_line(sf, side, y++, "  set it down 5-10 m away, in the open", dim);
        ls_safe_line(sf, side, y++, "2 Aim the board's top at it, then MARK", warn);
        return y;
    }
    ls_df_cal_result_t r; ls_df_cal_result(&s_cal, &r);
    snprintf(line, sizeof(line), "Beacon at %03.0f. Circle %d: turned %d of 360, %.0f dB", s_cal.mark,
             r.circles + 1, s_est.coverage, s_est.contrast);
    ls_safe_line(sf, side, y++, line, dim);
    if (!s_dfs.fresh) ls_safe_line(sf, side, y++, "No level: is the beacon on?", warn);
    else if (s_turn_rate > LS_DF_CAL_TURN_MAX) {
        snprintf(line, sizeof(line), "TOO FAST: %.0f deg/s, keep under %.0f", s_turn_rate, LS_DF_CAL_TURN_MAX);
        ls_safe_line(sf, side, y++, line, warn);
    } else {
        snprintf(line, sizeof(line), "Turning %.0f deg/s: good", s_turn_rate);
        ls_safe_line(sf, side, y++, line, dim);
    }
    if (r.circles) {
        snprintf(line, sizeof(line), "Offset %+.1f  spread %.1f  from %d circle%s", r.offset, r.spread,
                 r.circles, r.circles == 1 ? "" : "s");
        ls_safe_line(sf, side, y++, line, r.ready ? good : dim);
    }
    if (s_est.coverage >= LS_DF_CAL_COVERAGE && s_est.contrast < LS_DF_CAL_CONTRAST) {
        snprintf(line, sizeof(line), "Only %.0f dB between directions: hold it to your chest, or go outside",
                 s_est.contrast);
        ls_safe_line(sf, side, y++, line, warn);
    }
    if (r.ready) ls_safe_line(sf, side, y++, "They agree: SAVE CAL, or turn more", good);
    else if (r.circles >= LS_DF_CAL_NEEDED) ls_safe_line(sf, side, y++, "Circles disagree: keep turning, steadily", warn);
    else {
        snprintf(line, sizeof(line), "%d more circle%s", LS_DF_CAL_NEEDED - r.circles,
                 LS_DF_CAL_NEEDED - r.circles == 1 ? "" : "s");
        ls_safe_line(sf, side, y++, line, dim);
    }
    return y;
}

/* The flags a hit carries, in the log's own short words. */
static void hit_flags(const ls_df_hit_t *h, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s%s%s", hit_believed(h) ? "" : "1x ", h->flags & LS_DF_HIT_OVERLOAD ? "CLIP " : "",
             h->flags & LS_DF_HIT_FAST ? "FAST " : "", h->flags & LS_DF_HIT_FLAT ? "WIDE" : "");
}

static void hit_where(const ls_df_hit_t *h, char *out, size_t cap)
{
    if (h->freq_hz) snprintf(out, cap, "%.4f", h->freq_hz / 1e6);
    else snprintf(out, cap, "%.8s", ls_dfs_name(slot_status(h->track / LS_DFS_CHANNELS)->source));
}

/* LOG: a row a hit, newest on top, to judge them by. */
__attribute__((noinline)) static void draw_log(tui_surface *sf, tui_rect side)
{
    EXT_RAM_BSS_ATTR static char line[160];
    const int64_t now = esp_timer_get_time();
    int y = side.y;
    const int total = ls_df_log_count(&s_log);
    snprintf(line, sizeof(line), "HIT LOG %d/%lu  over %.0f dB  sure at %.0fx",
             total, (unsigned long)s_log.total, OPTV(O_THRESH), OPTV(O_SPIKE));
    ls_safe_line(sf, side, y++, line, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    ls_safe_line(sf, side, y++, " # AGE  CH FREQ/SRC    PEAK  SNR  BRG  N  FLAGS", TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    /* Rows until the detail and the key at the foot. */
    const int rows = side.y + side.h - y - 4;
    s_log_rows = tui_rect_make(side.x, y, side.w, rows > 0 ? rows : 0);
    if (s_log_pick >= total) s_log_pick = total - 1;
    if (s_log_pick >= 0 && s_log_pick < s_log_top) s_log_top = s_log_pick;
    if (rows > 0 && s_log_pick >= s_log_top + rows) s_log_top = s_log_pick - rows + 1;
    if (s_log_top > total - 1) s_log_top = total > 0 ? total - 1 : 0;
    if (!total) ls_safe_line(sf, side, y, "No hits yet: a level over its channel noise", LS_ATTR_DIM);
    for (int r = 0; r < rows && s_log_top + r < total; r++) {
        const int i = s_log_top + r;
        const ls_df_hit_t *h = ls_df_log_at(&s_log, i);
        char where[16], flags[24], brg[8], age[16];
        hit_where(h, where, sizeof(where)); hit_flags(h, flags, sizeof(flags));
        if (isfinite(h->heading)) snprintf(brg, sizeof(brg), "%03.0f%s", to_dial(h->heading), ref());
        else snprintf(brg, sizeof(brg), " -- ");
        const int secs = (int)((now - h->last_us) / 1000000);
        if (secs < 100) snprintf(age, sizeof(age), "%3ds", secs);
        else snprintf(age, sizeof(age), "%3dm", secs / 60 > 999 ? 999 : secs / 60);
        snprintf(line, sizeof(line), "%2d %s  %c %-10.10s %6.1f %4.0f %s %2u  %s", i + 1, age, track_tag(h->track),
                 where, h->peak, h->snr, brg, h->count > 99 ? 99 : h->count, flags);
        uint8_t at = hit_believed(h) ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM;
        if (h->flags) at = TUI_ATTR(TUI_YELLOW, TUI_BLACK);
        if (ls_df_hit_open(h, hit_cfg(), now)) at = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
        if (i == s_log_pick) at = TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
        ls_safe_line(sf, side, y + r, line, at);
    }
    y = side.y + side.h - 4;
    const ls_df_hit_t *p = s_log_pick >= 0 ? ls_df_log_at(&s_log, s_log_pick) : NULL;
    if (p) {
        char where[16]; hit_where(p, where, sizeof(where));
        const float noise = ls_df_log_noise(&s_log, p->track);
        snprintf(line, sizeof(line), "#%d %s ch%c %.1f, +%.0f over %.0f, %ux %.1fs",
                 s_log_pick + 1, where, track_tag(p->track), p->peak, p->snr, noise, p->count,
                 (double)(p->last_us - p->start_us) / 1e6);
        ls_safe_line(sf, side, y++, line, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        const ls_df_estimate_t *e = &s_te[p->track];
        if (e->valid) snprintf(line, sizeof(line), " facing %03.0f%s; ch%c lobe %03.0f%s +/-%.0f",
                                to_dial(p->heading), ref(), track_tag(p->track), to_dial(e->bearing), ref(), e->spread);
        else snprintf(line, sizeof(line), " facing %03.0f%s; turn a circle for ch%c",
                      to_dial(p->heading), ref(), track_tag(p->track));
        ls_safe_line(sf, side, y++, line, LS_ATTR_DIM);
    } else {
        ls_safe_line(sf, side, y++, "Tap a row: its heading shows on the dial", LS_ATTR_DIM);
        y++;
    }
    ls_safe_line(sf, side, y++, "1x one read  CLIP overloaded  FAST turned fast", LS_ATTR_DIM);
    ls_safe_line(sf, side, y++, "WIDE no direction: close by or reflections", LS_ATTR_DIM);
}

static void tune_value(char *out, size_t cap)
{
    if (s_dfs.tunable && s_dfs.channels > 1) snprintf(out, cap, "%d CH", s_dfs.channels);
    else if (s_dfs.tunable) snprintf(out, cap, "%.3f", s_dfs.freq_hz / 1e6);
    else snprintf(out, cap, "%.10s", s_dfs.target);
}

__attribute__((noinline)) static void draw_find(tui_surface *sf, tui_rect body)
{
    update_find();
    const bool cal = s_cal.phase != LS_DF_CAL_OFF;
    int bar_h;
    if (s_logmode && !cal) {
        /* The controls fold away to one short row; the log gets the room. */
        const ls_btn_t buttons[] = { { "LOG", "DONE", 'g', true, false },
                                     { "SAVE", "NOTE", 's', false, !ls_df_log_count(&s_log) },
                                     { "CLEAR", "LOG", 'c', false, !ls_df_log_count(&s_log) },
                                     { "VIEW", VIEW_NAMES[s_view], 'v', false, false } };
        bar_h = 3;
        ls_btn_bar_raised(sf, tui_rect_make(body.x, body.y, body.w, bar_h), buttons, 4, button_focus);
        tui_rect side;
        const tui_rect rest = tui_rect_make(body.x, body.y + bar_h, body.w, body.h - bar_h);
        const tui_rect art = art_and_side(rest, &side, rest.h * 11 / 20);
        draw_find_view(sf, art);
        draw_log(sf, side);
        return;
    }
    char tune[24]; tune_value(tune, sizeof(tune));
    ls_df_cal_result_t cr; ls_df_cal_result(&s_cal, &cr);
    char circles[16]; snprintf(circles, sizeof(circles), "%d/%d", cr.circles, LS_DF_CAL_NEEDED);
    char radio[24];
    if (s_dfs2.active) snprintf(radio, sizeof(radio), "%.6s+%.6s", ls_dfs_name(s_dfs_source), ls_dfs_name(s_dfs2.source));
    else snprintf(radio, sizeof(radio), "%s", ls_dfs_name(s_dfs_source));
    const ls_btn_t buttons[] = {
        { "RADIO", radio, 'r', s_dfs2.active, cal },
        { s_dfs.tunable ? (s_dfs.channels > 1 ? "SCAN" : "TUNE") : "TARGET", tune, 't', s_dfs.channels > 1,
          cal || (!s_dfs.tunable && !s_dfs.targets) },
        { "METHOD", s_method == LS_DF_PEAK ? "PEAK" : "NULL", 'n', s_method == LS_DF_NULL, cal },
        !cal ? (ls_btn_t){ "SAVE", "BEARING", 's', s_est.valid, false } :
        s_cal.phase == LS_DF_CAL_AIM ? (ls_btn_t){ "MARK", "BEACON", 's', true, !s_r.valid } :
        cr.ready ? (ls_btn_t){ "SAVE", "CAL", 's', true, false } :
                   (ls_btn_t){ "CIRCLES", circles, 's', false, true },
        !cal ? (ls_btn_t){ "CLEAR", "SWEEP", 'c', false, !s_sw[s_sel].samples } :
               (ls_btn_t){ "CANCEL", "CAL", 'c', false, false },
        !cal ? (ls_btn_t){ "FIX", s_bearing_count ? "CROSS" : "NEED 2", 'x', s_fix.valid, s_bearing_count < 2 } :
               (ls_btn_t){ "MARK", "AGAIN", 'x', false, s_cal.phase != LS_DF_CAL_TURN },
        { "VIEW", VIEW_NAMES[s_view], 'v', s_view != V_DIAL, cal },
        { "LOG", "HITS", 'g', false, cal } };
    const int nb = (int)(sizeof(buttons) / sizeof(buttons[0]));
    bar_h = ls_btn_raised_height(body, nb);
    ls_btn_bar_raised(sf, tui_rect_make(body.x, body.y, body.w, bar_h), buttons, nb, button_focus);
    tui_rect side;
    const tui_rect art = art_and_side(tui_rect_make(body.x, body.y + bar_h, body.w, body.h - bar_h), &side, 15);
    draw_find_view(sf, art);
    EXT_RAM_BSS_ATTR static char line[120];
    int y = side.y + 1;
    if (cal) y = cal_lines(sf, side, y) + 1;
    /* The level, big, and a bar: what the turn is being judged by. */
    const int64_t now = esp_timer_get_time();
    const bool scanning = live_tracks() > 1;
    const float cur = s_tlev[s_sel].last;
    const bool fresh = isfinite(cur) && now - s_tlev[s_sel].last_us < (int64_t)(lap_hold_s(s_sel / LS_DFS_CHANNELS) * 1e6f);
    char tag[8] = "";
    if (scanning) snprintf(tag, sizeof(tag), "CH %c  ", track_tag(s_sel));
    if (fresh && isfinite(s_peak)) snprintf(line, sizeof(line), "%s%6.1f %s   PEAK %.1f", tag, cur, s_dfs.unit, s_peak);
    else if (fresh) snprintf(line, sizeof(line), "%s%6.1f %s", tag, cur, s_dfs.unit);
    else snprintf(line, sizeof(line), "%s  --   %s", tag, s_dfs.active ? "no level yet" : "");
    ls_safe_line(sf, side, y++, line, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (s_tlev[s_sel].clip_us && now - s_tlev[s_sel].clip_us < 2000000)
        ls_safe_line(sf, side, y++, "CLIPPING: too close or too loud - no bearing from these",
                     TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
    const ls_df_sweep_t *sw = &s_sw[s_sel];
    if (fresh && sw->top > sw->floor) {
        const float v = (cur - sw->floor) / (sw->top - sw->floor);
        ls_bar(sf, side, y - side.y, 1, side.w - 2, fmaxf(0, fminf(1, v)));
    }
    y++;
    /* Calibrating, the panel above already says how the circle is going. */
    if (!cal) {
        snprintf(line, sizeof(line), "TURNED %d of 360  CONTRAST %.0f dB", s_est.coverage, s_est.contrast);
        ls_safe_line(sf, side, y++, line, LS_ATTR_DIM);
        if (s_est.valid) snprintf(line, sizeof(line), "SIGNAL AT %03.0f %s  +/-%.0f", to_dial(s_est.bearing), ref(), s_est.spread);
        else snprintf(line, sizeof(line), "%s", s_method == LS_DF_PEAK ? "Turn slowly through a full circle" :
                                          "Board to your chest, turn a full circle");
        ls_safe_line(sf, side, y++, line, TUI_ATTR((s_est.valid ? TUI_GREEN : TUI_YELLOW) | TUI_BRIGHT, TUI_BLACK));
        /* Every other channel's answer, in one line. */
        if (scanning) {
            int n = snprintf(line, sizeof(line), "OTHERS");
            for (int t = 0; t < TRACKS && n < (int)sizeof(line) - 12; t++)
                if (t != s_sel && track_live(t))
                    n += s_te[t].valid ? snprintf(line + n, sizeof(line) - n, "  %c %03.0f", track_tag(t), to_dial(s_te[t].bearing))
                                       : snprintf(line + n, sizeof(line) - n, "  %c ---", track_tag(t));
            ls_safe_line(sf, side, y++, line, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
        }
        /* The newest believed hit, so a burst is not missed between frames. */
        for (int i = 0; i < ls_df_log_count(&s_log); i++) {
            const ls_df_hit_t *h = ls_df_log_at(&s_log, i);
            if (!hit_believed(h)) continue;
            char where[16]; hit_where(h, where, sizeof(where));
            snprintf(line, sizeof(line), "LAST HIT %s ch %c  %.0f dB over  facing %03.0f  %ds ago", where,
                     track_tag(h->track), h->snr, isfinite(h->heading) ? to_dial(h->heading) : 0.0f,
                     (int)((now - h->last_us) / 1000000));
            ls_safe_line(sf, side, y++, line, TUI_ATTR(TUI_GREEN, TUI_BLACK));
            break;
        }
    }
    ls_safe_line(sf, side, y++, s_dfs.status, LS_ATTR_DIM);
    if (s_dfs2.active) {
        snprintf(line, sizeof(line), "2ND %s: %s", ls_dfs_name(s_dfs2.source), s_dfs2.status);
        ls_safe_line(sf, side, y++, line, LS_ATTR_DIM);
    }
    if (!cal) {
        snprintf(line, sizeof(line), "BEARINGS SAVED %d", s_bearing_count);
        ls_safe_line(sf, side, y++, line, LS_ATTR_DIM);
    }
    if (!cal && isfinite(s_df_offset)) {
        snprintf(line, sizeof(line), "Corrected %+.1f deg by beacon calibration", -s_df_offset);
        ls_safe_line(sf, side, y++, line, LS_ATTR_DIM);
    }
    if (s_fix.valid) {
        snprintf(line, sizeof(line), "FIX %.5f, %.5f  +/-%.0f m", s_fix.lat, s_fix.lon, s_fix.radius_m);
        ls_safe_line(sf, side, y++, line, TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }
    if (!s_r.calibrated)
        ls_safe_line(sf, side, y++, "Calibrate first: a bent heading bends the bearing", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

__attribute__((noinline)) static void draw_goto(tui_surface *sf, tui_rect body)
{
    double tb, tm; char name[48];
    const bool have = target_nav(&tb, &tm, name, sizeof(name));
    const bool set = ls_compass_target(NULL, NULL, NULL, 0);
    ls_btn_t buttons[] = { { "SET", "TARGET", 't', false, false }, { "MAP", "SHOW", 'm', false, !set },
                           { "CLEAR", NULL, 'c', false, !set } };
    const int bar_h = ls_btn_raised_height(body, 3);
    ls_btn_bar_raised(sf, tui_rect_make(body.x, body.y, body.w, bar_h), buttons, 3, button_focus);
    ls_compass_scene_t sc; scene(&sc, false);
    tui_rect side;
    tui_rect art = art_and_side(tui_rect_make(body.x, body.y + bar_h, body.w, body.h - bar_h), &side, 13);
    ls_compass_art_draw(sf, art, &sc);
    mag_caution(sf, art);
    EXT_RAM_BSS_ATTR static char line[120];
    int y = side.y + 1;
    if (!set) {
        ls_safe_line(sf, side, y++, "No target. SET picks a note with a place,", LS_ATTR_DIM);
        ls_safe_line(sf, side, y++, "a mesh node, the map centre, home or a fix.", LS_ATTR_DIM);
        return;
    }
    ls_safe_line(sf, side, y++, name, TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    if (!have) { ls_safe_line(sf, side, y++, "Waiting for a GPS fix to measure from", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)); return; }
    const float bearing = to_dial((float)tb);
    big_number(sf, side, side.x + 2, y, bearing, TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    char d[24]; distance_text(tm, d, sizeof(d));
    tui_put_str(sf, side, side.x + 2 + 3 * (LS_GLYPH_COLS + 1), y, ref(), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, side, side.x + 2 + 3 * (LS_GLYPH_COLS + 1), y + 2, d, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    y += 6;
    const float h = shown_heading();
    if (isfinite(h)) {
        const float off = fmodf(bearing - h + 540.0f, 360.0f) - 180.0f;
        if (fabsf(off) < 5) snprintf(line, sizeof(line), ">>> STRAIGHT AHEAD <<<");
        else snprintf(line, sizeof(line), "%s %.0f deg %s", off > 0 ? "TURN RIGHT" : "TURN LEFT", fabsf(off), off > 0 ? "-->" : "<--");
        ls_safe_line(sf, side, y++, line, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    ls_gps_state_t g; ls_gps_get(&g);
    if (g.fix && g.speed_kts > 0.8f) {
        const double ms = g.speed_kts * 0.514444;
        snprintf(line, sizeof(line), "%.1f km/h  about %.0f min", ms * 3.6, tm / ms / 60.0);
        ls_safe_line(sf, side, y++, line, LS_ATTR_DIM);
    }
}

__attribute__((noinline)) static void draw_level(tui_surface *sf, tui_rect body)
{
    tui_rect box = tui_rect_make(body.x + 1, body.y + 1, body.w - 2, body.h - 8);
    ls_panel_box(sf, box, "LEVEL", TUI_CYAN);
    int cw = 10, ch = 17; ls_tui_geometry(NULL, NULL, &cw, &ch);
    const int cx = box.x + box.w / 2, cy = box.y + box.h / 2;
    float ry = (box.h - 4) * 0.45f, rx = ry * ch / cw;
    if (rx > (box.w - 6) * 0.45f) { rx = (box.w - 6) * 0.45f; ry = rx * cw / ch; }
    for (int i = 0; i < 72; i++) {
        const float a = i * 5 * 0.0174533f;
        tui_put_char(sf, box, cx + (int)lroundf(sinf(a) * rx), cy - (int)lroundf(cosf(a) * ry), '.', LS_ATTR_DIM);
        tui_put_char(sf, box, cx + (int)lroundf(sinf(a) * rx * 0.33f), cy - (int)lroundf(cosf(a) * ry * 0.33f), '.', LS_ATTR_DIM);
    }
    for (int k = -2; k <= 2; k++) { tui_put_char(sf, box, cx + k, cy, '-', LS_ATTR_DIM); tui_put_char(sf, box, cx, cy + k / 2, '|', LS_ATTR_DIM); }
    /* The bubble floats to the high side: 15 degrees reaches the rim. */
    const float scale = 1.0f / 15.0f;
    const float bx = fmaxf(-1, fminf(1, -s_r.roll * scale)), by = fmaxf(-1, fminf(1, -s_r.pitch * scale));
    const int px = cx + (int)lroundf(bx * rx), py = cy + (int)lroundf(by * ry);
    const bool level = fabsf(s_r.pitch) < 0.5f && fabsf(s_r.roll) < 0.5f;
    const uint8_t hue = level ? TUI_GREEN | TUI_BRIGHT : TUI_YELLOW | TUI_BRIGHT;
    tui_put_str(sf, box, px - 1, py - 1, " _ ", TUI_ATTR(hue, TUI_BLACK));
    tui_put_str(sf, box, px - 1, py, "(O)", TUI_ATTR(hue, TUI_BLACK));
    EXT_RAM_BSS_ATTR static char line[100];
    snprintf(line, sizeof(line), "PITCH %+5.1f   ROLL %+5.1f   TILT %.1f", s_r.pitch, s_r.roll, s_r.tilt);
    ls_safe_line(sf, body, body.y + body.h - 6, line, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    ls_safe_line(sf, body, body.y + body.h - 5, level ? "LEVEL" : "Lay it on the surface; the bubble shows the high side",
                 TUI_ATTR(hue, TUI_BLACK));
    snprintf(line, sizeof(line), "Stood on an edge, TILT is the slope: %.1f deg, %.0f%% grade",
             90 - s_r.tilt, tanf((90 - s_r.tilt) * 0.0174533f) * 100);
    if (s_r.tilt > 45) ls_safe_line(sf, body, body.y + body.h - 4, line, LS_ATTR_DIM);
}

static bool s_steady_true;
static float s_gyro_rest[3];
static float s_rate = NAN;
static void find_requests(void);

static void draw(tui_surface *sf, tui_rect a)
{
    if (a.h < 16 || a.w < 30) {
        ls_panel_box(sf, a, "COMPASS", TUI_CYAN);
        tui_put_str(sf, a, a.x + 1, a.y + 1, "Enlarge the pane", LS_ATTR_DIM);
        return;
    }
    const int64_t now = esp_timer_get_time();
    ls_compass_live(&s_r);
    const float dt = s_last_us ? (float)((now - s_last_us) / 1e6) : 0.04f;
    s_last_us = now;
    {
        /* Turning is judged by the gyro's whole rate, whichever way the
           board is held, less what it reads lying still: this board's gyro
           rests near 4 deg/s, which alone looked like a turn. The rest
           reading is learnt whenever the rate is under 6 deg/s, over about 5 s. */
        ls_field_sample_t p; ls_field_sample_snapshot(&p);
        float rate = NAN;
        if (p.imu_valid) {
            const float g[3] = { p.imu.gx, p.imu.gy, p.imu.gz };
            float d[3], r2 = 0;
            for (int i = 0; i < 3; i++) { d[i] = g[i] - s_gyro_rest[i]; r2 += d[i] * d[i]; }
            rate = sqrtf(r2);
            if (rate < 6.0f) for (int i = 0; i < 3; i++) s_gyro_rest[i] += d[i] * fminf(1.0f, dt / 5.0f);
        }
        /* A switch between true and magnetic is a jump, not a turn. */
        if (s_steady.started && s_steady_true != true_mode()) s_steady.started = false;
        s_steady_true = true_mode();
        ls_compass_steady_step(&s_steady, raw_heading(), s_r.pitch, s_r.roll, rate, dt);
        s_rate = rate;
    }
    ls_compass_spring_step(&s_spring, shown_heading(), dt);

    ls_btn_t tabs[P_COUNT + 2];
    for (int i = 0; i < P_COUNT; i++) tabs[i] = (ls_btn_t){ PAGE_NAMES[i], NULL, (char)('1' + i), s_page == i, false };
    tabs[P_COUNT] = (ls_btn_t){ "MORE", s_page == P_FIND ? "FIND" : s_simple ? "SIMPLE" : "3D", 'o', false, false };
    tabs[P_COUNT + 1] = (ls_btn_t){ "LOOKS", NULL, 'y', false, false };
    /* The six tabs take the room five did, in two short rows if need be,
       so LOOKS costs the dial none of its height. */
    const int bar_h = ls_btn_raised_height(a, P_COUNT + 1);
    ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y, a.w, bar_h), tabs, P_COUNT + 2, -1, LS_BTN_SLOT_QUICK);
    tui_rect body = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h - 1);
    find_requests();
    if (s_page == P_DIAL) draw_dial(sf, body);
    else if (s_page == P_FIND) draw_find(sf, body);
    else if (s_page == P_GOTO) draw_goto(sf, body);
    else draw_level(sf, body);
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback, LS_ATTR_DIM);
}

/* -------------------------------------------------------------- actions -- */

static void open_app(const char *id)
{
    for (int k = 0; k < ls_app_count(); k++) {
        const ls_app_t *app = ls_app_at(k);
        if (app->id && !strcmp(app->id, id)) { ls_app_open(k); return; }
    }
}

static void mark_note(void)
{
    char body[600], line[200];
    size_t n = 0;
    ls_note_fmt_heading(line, sizeof(line), s_r.magnetic, s_r.true_deg, s_r.tilt, s_r.calibrated);
    n += (size_t)snprintf(body + n, sizeof(body) - n, "%s", line);
    ls_note_live_gps(line, sizeof(line));
    n += (size_t)snprintf(body + n, sizeof(body) - n, "%s", line);
    if (isfinite(s_lock) && n < sizeof(body))
        n += (size_t)snprintf(body + n, sizeof(body) - n, "Locked bearing %03.0f %s\n", s_lock, ref());
    say(ls_notes_mark("Compass mark", body) ? "Saved to NOTES" : "NOTES is busy; try again");
}

static void find_settings(void);

enum { M_NORTH, M_STYLE, M_CAL, M_LOCK, M_TARGET, M_BEARINGS, M_DFCAL, M_DFCLEAR };
static void more_done(int i)
{
    if (i == M_NORTH) { s_magnetic = !s_magnetic; s_spring.started = false; save_options();
        say(s_magnetic ? "Magnetic north" : s_r.true_valid ? "True north" : "True north once there is a GPS fix"); }
    else if (i == M_STYLE) { s_simple = !s_simple; save_options(); say(s_simple ? "Simple style" : "3D style"); }
    else if (i == M_CAL) { ls_scr_labs_request_calibration(); open_app("labs"); }
    else if (i == M_LOCK) { s_lock = NAN; say("Lock cleared"); }
    else if (i == M_TARGET) { ls_compass_clear_target(); say("Target cleared"); }
    else if (i == M_BEARINGS) { s_bearing_count = 0; memset(&s_fix, 0, sizeof(s_fix)); say("Saved bearings cleared"); }
    else if (i == M_DFCAL) { show_page(P_FIND); cal_start(BEACON_915); }
    else if (i == M_DFCLEAR) { settings_set_df_offset((int)s_dfs_source, (int)s_method, NAN); s_df_offset = NAN;
                               say("FIND correction cleared for this radio and method"); }
}

static void open_more(void)
{
    if (s_page == P_FIND && s_cal.phase == LS_DF_CAL_OFF) { find_settings(); return; }
    ls_picker_open("COMPASS", more_done);
    ls_picker_add(s_magnetic ? "Use true north" : "Use magnetic north",
                  isfinite(s_r.declination) ? "declination from the World Magnetic Model" : "true north needs one GPS fix, or HOME");
    ls_picker_add(s_simple ? "3D style" : "Simple style", s_simple ? "the lit, tilting instrument" : "the flat ASCII ring");
    ls_picker_add("Calibrate", "six positions and a figure eight, in LORA LABS");
    ls_picker_add("Clear the lock", isfinite(s_lock) ? "stop steering to it" : "no lock set");
    ls_picker_add("Clear the target", ls_compass_target(NULL, NULL, NULL, 0) ? "stop following it" : "no target set");
    char d[40]; snprintf(d, sizeof(d), "%d kept", s_bearing_count);
    ls_picker_add("Clear saved bearings", d);
    ls_picker_add("Calibrate FIND with a beacon", "the Flipper's DF Beacon, 915 MHz");
    char c[48];
    if (isfinite(s_df_offset)) snprintf(c, sizeof(c), "%s %s: %+.1f deg", ls_dfs_name(s_dfs_source),
                                        s_method == LS_DF_PEAK ? "PEAK" : "NULL", -s_df_offset);
    else snprintf(c, sizeof(c), "none for %s", ls_dfs_name(s_dfs_source));
    ls_picker_add("Clear FIND correction", c);
}

/* ---- LOOKS ---- */

static const char *const FILL_NAMES[LS_COMPASS_FILL_COUNT] = { "full", "light", "edge only", "off" };
enum { L_FILL, L_TRAIL, L_LETTERS, L_STYLE };
static void open_looks(void);

/* Each row steps to its next choice and the menu comes back, showing it. */
static void looks_done(int i)
{
    if (i < 0) return;
    if (i == L_FILL) { s_fill = (s_fill + 1) % LS_COMPASS_FILL_COUNT; settings_set_df_option(O_FILL, s_fill); }
    else if (i == L_TRAIL) { s_trail = !s_trail; settings_set_df_option(O_TRAIL, s_trail); }
    else if (i == L_LETTERS) { s_white_letters = !s_white_letters; settings_set_df_option(O_LETTERS, s_white_letters); }
    else if (i == L_STYLE) { s_simple = !s_simple; save_options(); }
    open_looks();
}

static void open_looks(void)
{
    ls_picker_open("LOOKS", looks_done);
    ls_picker_add("Signal fill inside the card", FILL_NAMES[s_fill]);
    ls_picker_add("Green trail of directions heard", s_trail ? "on" : "off");
    ls_picker_add("Direction letters", s_white_letters ? "white" : "orange");
    ls_picker_add("Style", s_simple ? "simple: the flat ASCII ring" : "3D: the lit, tilting instrument");
}

/* ---- FIND settings ---- */

static int s_opt_edit;

static void opt_text(int i, int v, char *out, size_t cap)
{
    const float x = OPT[i].v[v];
    if (x == 0 && OPT[i].zero) snprintf(out, cap, "%s", OPT[i].zero);
    else if (x != floorf(x)) snprintf(out, cap, "%.1f %s", x, OPT[i].unit);
    else snprintf(out, cap, "%.0f %s", x, OPT[i].unit);
}

static void opt_value_done(int v)
{
    if (v < 0 || v >= OPT[s_opt_edit].n) { find_settings(); return; }
    s_opt[s_opt_edit] = v;
    settings_set_df_option(s_opt_edit, v);
    if (s_opt_edit == O_DWELL && s_find_on) {
        ls_dfs_set_channels(0, s_ch[0], s_nch[0], (uint32_t)OPTV(O_DWELL));
        if (s_dfs2.active) ls_dfs_set_channels(1, s_ch[1], s_nch[1], (uint32_t)OPTV(O_DWELL));
    }
    find_settings();
}

static void opt_values(int i)
{
    s_opt_edit = i;
    ls_picker_open(OPT[i].name, opt_value_done);
    for (int v = 0; v < OPT[i].n; v++) {
        char t[32]; opt_text(i, v, t, sizeof(t));
        ls_picker_add(t, v == s_opt[i] ? "in use" : v == OPT[i].dflt ? "the default" : "");
    }
}

static void channels_open(int slot);

static void second_done(int i)
{
    if (i < 0) { find_settings(); return; }
    ls_dfs_t pick = i >= LS_DFS_COUNT ? LS_DFS_COUNT : (ls_dfs_t)i;
    if (pick < LS_DFS_COUNT) {
        const char *why = ls_dfs_unavailable(pick);
        if (!why && pick == s_dfs_source) why = "That is the first radio already";
        if (!why) why = ls_dfs_conflict(1, pick);
        if (why) { say(why); find_settings(); return; }
    }
    ls_dfs_stop_slot(1);
    clear_slot(1);
    s_src2 = pick;
    settings_set_df_option(O_SRC2, pick < LS_DFS_COUNT ? pick + 1 : 0);
    /* A list kept for another radio may not suit this one. */
    int n = 0;
    for (int c = 0; c < s_nch[1]; c++) if (pick < LS_DFS_COUNT && ls_dfs_in_range(pick, s_ch[1][c])) s_ch[1][n++] = s_ch[1][c];
    if (!n && pick < LS_DFS_COUNT) {
        s_ch[1][0] = ls_dfs_in_range(pick, s_ch[0][0]) ? s_ch[0][0] : default_freq(pick);
        n = 1;
    }
    if (n) { s_nch[1] = n; settings_set_df_channels(1, s_ch[1], n); }
    load_df_offset();
    if (s_find_on) find_start();
    say(pick < LS_DFS_COUNT ? "Second radio on: its channels are tagged A-H" : "Second radio off");
}

static void second_open(void)
{
    ls_picker_open("SECOND RADIO", second_done);
    for (int i = 0; i < LS_DFS_COUNT; i++) {
        const char *why = ls_dfs_unavailable((ls_dfs_t)i);
        if (!why && (ls_dfs_t)i == s_dfs_source) why = "the first radio";
        if (!why) {
            /* Against the first radio, not whatever slot 1 holds now. */
            const bool same = (i <= LS_DFS_LORA && s_dfs_source <= LS_DFS_LORA) ||
                              ((i == LS_DFS_RTL || i == LS_DFS_HACKRF) && (s_dfs_source == LS_DFS_RTL || s_dfs_source == LS_DFS_HACKRF)) ||
                              ((i == LS_DFS_CC1101 || i == LS_DFS_NRF24) && (s_dfs_source == LS_DFS_CC1101 || s_dfs_source == LS_DFS_NRF24));
            if (same) why = "shares hardware with the first radio";
        }
        ls_picker_add(ls_dfs_name((ls_dfs_t)i), why ? why : (ls_dfs_t)i == s_src2 ? "in use" : "ready");
    }
    ls_picker_add("Off", "one radio");
}

enum { F_OPTS = 0, F_SECOND = O_TUNED, F_SECOND_CH, F_CLEARLOG, F_DFCAL, F_DFCLEAR, F_BEARINGS, F_COMPASS };
static void find_settings_done(int i)
{
    if (i < 0) return;
    if (i < O_TUNED) { opt_values(i); return; }
    if (i == F_SECOND) second_open();
    else if (i == F_SECOND_CH) { if (s_src2 < LS_DFS_COUNT) channels_open(1); else find_settings(); }
    else if (i == F_CLEARLOG) { ls_df_log_clear(&s_log); s_log_pick = -1; s_log_top = 0; say("Hit log cleared"); }
    else if (i == F_DFCAL) cal_start(BEACON_915);
    else if (i == F_DFCLEAR) more_done(M_DFCLEAR);
    else if (i == F_BEARINGS) more_done(M_BEARINGS);
    else if (i == F_COMPASS) {
        /* The compass's own options, as on the other pages. */
        const int page = s_page; s_page = P_DIAL; open_more(); s_page = page;
    }
}

static void find_settings(void)
{
    ls_picker_open("FIND SETTINGS", find_settings_done);
    for (int i = 0; i < O_TUNED; i++) {
        char t[32]; opt_text(i, s_opt[i], t, sizeof(t));
        ls_picker_add(OPT[i].name, t);
    }
    char d[40];
    snprintf(d, sizeof(d), "%s", s_src2 < LS_DFS_COUNT ? ls_dfs_name(s_src2) : "off");
    ls_picker_add("Second radio at once", d);
    snprintf(d, sizeof(d), s_src2 < LS_DFS_COUNT ? "%d channel%s" : "turn a second radio on", s_nch[1], s_nch[1] == 1 ? "" : "s");
    ls_picker_add("Second radio's channels", d);
    snprintf(d, sizeof(d), "%d kept", ls_df_log_count(&s_log));
    ls_picker_add("Clear the hit log", d);
    ls_picker_add("Calibrate FIND with a beacon", "the Flipper's DF Beacon, 915 MHz");
    if (isfinite(s_df_offset)) snprintf(d, sizeof(d), "%s: %+.1f deg", ls_dfs_name(s_dfs_source), -s_df_offset);
    else snprintf(d, sizeof(d), "none for %s", ls_dfs_name(s_dfs_source));
    ls_picker_add("Clear FIND correction", d);
    snprintf(d, sizeof(d), "%d kept", s_bearing_count);
    ls_picker_add("Clear saved bearings", d);
    ls_picker_add("Compass options", "north, style, calibration");
}

/* ---- FIND channels ---- */

static int s_edit_slot, s_edit_ch;
static ls_dfs_t slot_source(int k) { return k ? s_src2 : s_dfs_source; }

static void apply_channels(int k)
{
    settings_set_df_channels(k, s_ch[k], s_nch[k]);
    if (!k) s_dfs_freq = s_ch[0][0];
    clear_slot(k);
    if (s_find_on) ls_dfs_set_channels(k, s_ch[k], s_nch[k], (uint32_t)OPTV(O_DWELL));
}

static void channel_freq_done(double mhz)
{
    const int k = s_edit_slot;
    if (!(mhz > 1 && mhz < 6000)) { say("Out of range"); return; }
    const uint32_t hz = (uint32_t)llround(mhz * 1e6);
    if (!ls_dfs_in_range(slot_source(k), hz)) {
        char t[64]; snprintf(t, sizeof(t), "%s cannot tune to %.4f MHz", ls_dfs_name(slot_source(k)), mhz);
        say(t); return;
    }
    if (s_edit_ch < 0) { if (s_nch[k] < LS_DFS_CHANNELS) s_ch[k][s_nch[k]++] = hz; }
    else s_ch[k][s_edit_ch] = hz;
    apply_channels(k);
    channels_open(k);
}

enum { CE_SHOW, CE_CHANGE, CE_REMOVE };
static void channel_edit_done(int i)
{
    const int k = s_edit_slot, c = s_edit_ch;
    if (i == CE_SHOW) { s_sel = k * LS_DFS_CHANNELS + c; say("Showing that channel's lobe"); }
    else if (i == CE_CHANGE) ls_numpad_open("FREQUENCY", "MHz", s_ch[k][c] / 1e6, channel_freq_done);
    else if (i == CE_REMOVE && s_nch[k] > 1) {
        memmove(&s_ch[k][c], &s_ch[k][c + 1], sizeof(uint32_t) * (s_nch[k] - c - 1));
        s_nch[k]--;
        if (s_sel >= k * LS_DFS_CHANNELS + s_nch[k]) s_sel = k * LS_DFS_CHANNELS;
        apply_channels(k);
        channels_open(k);
    } else channels_open(k);
}

/* Lists worth scanning together. An SDR takes the ones inside one capture
   at once: all of FRS, or all seven weather channels. */
static const struct { const char *name; uint32_t hz[LS_DFS_CHANNELS]; int n; } PRESETS[] = {
    { "ISM remotes: 315, 433.92, 868.35, 915", { 315000000, 433920000, 868350000, 915000000 }, 4 },
    { "FRS/GMRS 1-7", { 462562500, 462587500, 462612500, 462637500, 462662500, 462687500, 462712500 }, 7 },
    { "MURS 1-5", { 151820000, 151880000, 151940000, 154570000, 154600000 }, 5 },
    { "NOAA weather 1-7", { 162550000, 162400000, 162475000, 162425000, 162450000, 162500000, 162525000 }, 7 },
    { "Marine 16 and 9", { 156800000, 156450000 }, 2 },
    { "LoRa US915 low", { 902300000, 902500000, 902700000, 902900000, 903100000, 903300000, 903500000, 903700000 }, 8 },
    { "Meshtastic US LongFast", { 906875000 }, 1 },
    { "MeshCore US", { 910525000 }, 1 },
};
#define PRESET_N (int)(sizeof(PRESETS) / sizeof(PRESETS[0]))

static void preset_done(int i)
{
    const int k = s_edit_slot;
    if (i < 0 || i >= PRESET_N) { channels_open(k); return; }
    int n = 0;
    for (int c = 0; c < PRESETS[i].n; c++)
        if (ls_dfs_in_range(slot_source(k), PRESETS[i].hz[c])) s_ch[k][n++] = PRESETS[i].hz[c];
    if (!n) { say("This radio reaches none of those"); channels_open(k); return; }
    s_nch[k] = n; s_sel = k * LS_DFS_CHANNELS;
    apply_channels(k);
    char t[64]; snprintf(t, sizeof(t), "Scanning %d of %d channels", n, PRESETS[i].n);
    say(t);
}

static void presets_open(void)
{
    ls_picker_open("SCAN PRESETS", preset_done);
    for (int i = 0; i < PRESET_N; i++) {
        int n = 0;
        for (int c = 0; c < PRESETS[i].n; c++) n += ls_dfs_in_range(slot_source(s_edit_slot), PRESETS[i].hz[c]);
        char d[40];
        if (n == PRESETS[i].n) snprintf(d, sizeof(d), "%d channel%s", n, n == 1 ? "" : "s");
        else snprintf(d, sizeof(d), "%d of %d in this radio's range", n, PRESETS[i].n);
        ls_picker_add(PRESETS[i].name, d);
    }
}

static void channels_done(int i)
{
    const int k = s_edit_slot;
    if (i < 0) return;
    if (i < s_nch[k]) {
        s_edit_ch = i;
        char title[32]; snprintf(title, sizeof(title), "CH %c  %.4f MHz", track_tag(k * LS_DFS_CHANNELS + i), s_ch[k][i] / 1e6);
        ls_picker_open(title, channel_edit_done);
        ls_picker_add("Show this channel", "its lobe on the dial, its PEAK readout");
        ls_picker_add("Change the frequency", "");
        ls_picker_add("Remove it", s_nch[k] > 1 ? "" : "the last channel stays");
        return;
    }
    i -= s_nch[k];
    if (i == 0) {
        if (s_nch[k] >= LS_DFS_CHANNELS) { say("Eight channels is the most"); channels_open(k); return; }
        s_edit_ch = -1;
        ls_numpad_open("ADD CHANNEL", "MHz", s_ch[k][s_nch[k] - 1] / 1e6, channel_freq_done);
    } else if (i == 1) presets_open();
    else if (i == 2 && s_nch[k] > 1) { s_nch[k] = 1; s_sel = k * LS_DFS_CHANNELS; apply_channels(k); say("One frequency, no scanning"); }
}

static void channels_open(int k)
{
    s_edit_slot = k;
    char title[32]; snprintf(title, sizeof(title), "%s CHANNELS", ls_dfs_name(slot_source(k)));
    ls_picker_open(title, channels_done);
    const ls_dfs_status_t *st = slot_status(k);
    for (int c = 0; c < s_nch[k]; c++) {
        const int t = k * LS_DFS_CHANNELS + c;
        char label[32], d[40];
        snprintf(label, sizeof(label), "%c  %.4f MHz", track_tag(t), s_ch[k][c] / 1e6);
        if (s_te[t].valid) snprintf(d, sizeof(d), "%s%03.0f %s +/-%.0f", t == s_sel ? "shown  " : "",
                                    to_dial(s_te[t].bearing), ref(), s_te[t].spread);
        else snprintf(d, sizeof(d), "%s%s", t == s_sel ? "shown  " : "",
                      st->active && st->channels > 1 && st->channel == c ? "listening now" : "no bearing yet");
        ls_picker_add(label, d);
    }
    ls_picker_add("Add a channel", s_nch[k] < LS_DFS_CHANNELS ? "scan up to eight" : "eight is the most");
    ls_picker_add("Scan presets", "FRS, MURS, weather, ISM, LoRa");
    if (s_nch[k] > 1) ls_picker_add("One frequency only", "keep the first, stop scanning");
}

/* ---- FIND ---- */

static void pick_source_done(int i)
{
    if (i == LS_DFS_COUNT) { cal_start(BEACON_915); return; }
    if (i == LS_DFS_COUNT + 1) { cal_start(BEACON_433); return; }
    if (i < 0 || i >= LS_DFS_COUNT) return;
    const char *why = ls_dfs_unavailable((ls_dfs_t)i);
    if (why) { say(why); return; }
    if ((ls_dfs_t)i == s_src2) { s_src2 = LS_DFS_COUNT; settings_set_df_option(O_SRC2, 0); }
    find_stop();
    s_dfs_source = (ls_dfs_t)i;
    settings_set_df_option(O_SRC1, i + 1);
    /* A list kept for another radio may not suit this one. */
    int n = 0;
    for (int c = 0; c < s_nch[0]; c++) if (ls_dfs_in_range(s_dfs_source, s_ch[0][c])) s_ch[0][n++] = s_ch[0][c];
    if (n) s_nch[0] = n;
    else if (s_dfs_source <= LS_DFS_NRF24 && s_dfs_source != LS_DFS_MESH) {
        s_nch[0] = 1; s_ch[0][0] = default_freq(s_dfs_source);
        settings_set_df_channels(0, s_ch[0], 1);
    }
    s_sel = 0;
    find_start();
    if (!s_dfs2.active && s_src2 < LS_DFS_COUNT && ls_dfs_conflict(1, s_src2)) say("Second radio off: it shares hardware with this one");
    clear_all();
    load_df_offset();
    if (s_find_on) say("Now turn slowly through a full circle");
    else say("That radio would not start");
}

static void pick_source(void)
{
    ls_picker_open("FIND WITH", pick_source_done);
    for (int i = 0; i < LS_DFS_COUNT; i++) {
        const char *why = ls_dfs_unavailable((ls_dfs_t)i);
        ls_picker_add(ls_dfs_name((ls_dfs_t)i), why ? why : (ls_dfs_t)i == s_src2 ? "ready; takes it from the second slot" : "ready");
    }
    /* A preset, last: the Flipper's DF Beacon, straight into calibration. */
    static const uint32_t HZ[2] = { BEACON_915, BEACON_433 };
    for (int k = 0; k < 2; k++) {
        const ls_dfs_t rx = cal_receiver(HZ[k]);
        char d[48], label[40];
        snprintf(label, sizeof(label), "FLIPPER BEACON %s: CALIBRATE", k ? "433" : "915");
        if (rx == LS_DFS_COUNT) snprintf(d, sizeof(d), "no receiver for %s MHz", k ? "433.92" : "915");
        else snprintf(d, sizeof(d), "%s on %s", k ? "433.92" : "915.00", ls_dfs_name(rx));
        ls_picker_add(label, d);
    }
}

static void target_done(int i)
{
    ls_dfs_target_pick(i - 1);                  /* row 0 is "everything" */
    clear_slot(0);
}

static void pick_target(void)
{
    ls_picker_open("FOLLOW", target_done);
    ls_picker_add("Everything heard", "any packet, advert or scan result");
    const int n = ls_dfs_target_count();
    for (int i = 0; i < n && i < 40; i++) {
        char label[40], detail[48];
        if (ls_dfs_target_label(i, label, sizeof(label), detail, sizeof(detail))) ls_picker_add(label, detail);
    }
    if (!n) ls_picker_empty_reason("Nothing heard yet: wait a few seconds");
}

static void save_bearing(void)
{
    float bearing = s_est.bearing, spread = s_est.spread;
    if (!s_est.valid) {
        /* No sweep answer: keep where the board points, as a hand-aimed
           bearing with a generous spread. */
        bearing = to_true(shown_heading()); spread = 10;
        if (!isfinite(bearing)) { say("No heading to save"); return; }
    }
    const ls_df_sweep_t *sw = &s_sw[s_sel];
    const ls_dfs_status_t *st = slot_status(s_sel / LS_DFS_CHANNELS);
    ls_df_bearing_t b = { .bearing = bearing, .spread = spread, .level = sw->top,
                          .time_us = esp_timer_get_time() };
    const uint32_t f = track_freq(s_sel);
    if (f) snprintf(b.source, sizeof(b.source), "%.6s %.3f", ls_dfs_name(st->source), f / 1e6);
    else snprintf(b.source, sizeof(b.source), "%s", ls_dfs_name(st->source));
    const bool placed = position(&b.lat, &b.lon);
    if (placed) {
        if (s_bearing_count == LS_DF_BEARINGS) { memmove(s_bearings, s_bearings + 1, sizeof(s_bearings[0]) * (LS_DF_BEARINGS - 1)); s_bearing_count--; }
        s_bearings[s_bearing_count++] = b;
    }
    EXT_RAM_BSS_ATTR static char line[200], title[48];
    ls_note_fmt_bearing(line, sizeof(line), bearing, spread, b.source, sw->top, st->unit, b.lat, b.lon, placed);
    snprintf(title, sizeof(title), "Bearing: %s %.10s", b.source, st->tunable ? "" : st->target);
    ls_notes_mark(title, line);
    say(placed ? "Bearing saved. Move somewhere else and take another." : "Saved to NOTES, but no GPS: it cannot join a fix");
    clear_track(s_sel); memset(&s_est, 0, sizeof(s_est));
}

static void make_fix(void)
{
    if (!ls_df_triangulate(s_bearings, s_bearing_count, &s_fix)) {
        say("These bearings do not cross: take one from further round"); return;
    }
    ls_compass_set_target(s_fix.lat, s_fix.lon, "DF fix");
    char body[200];
    snprintf(body, sizeof(body), "> FIX %.6f, %.6f  +/-%.0f m  from %d bearings\n", s_fix.lat, s_fix.lon, s_fix.radius_m, s_fix.used);
    ls_notes_mark("Transmitter fix", body);
    say("Fix saved to NOTES and set as the GO TO target");
}

/* The log as a note: the newest rows that fit. */
static void save_log(void)
{
    EXT_RAM_BSS_ATTR static char body[1200];
    const int64_t now = esp_timer_get_time();
    int n = snprintf(body, sizeof(body), "Hits over %.0f dB, believed at %.0f reads. Bearings %s.\n\n"
                     " # AGE  CH FREQ/SRC    PEAK  SNR  BRG  N  FLAGS\n", OPTV(O_THRESH), OPTV(O_SPIKE),
                     true_mode() ? "true" : "magnetic");
    for (int i = 0; i < ls_df_log_count(&s_log) && n < (int)sizeof(body) - 80; i++) {
        const ls_df_hit_t *h = ls_df_log_at(&s_log, i);
        char where[16], flags[24];
        hit_where(h, where, sizeof(where)); hit_flags(h, flags, sizeof(flags));
        n += snprintf(body + n, sizeof(body) - n, "%2d %3ds  %c %-10.10s %6.1f %4.0f %03.0f %2u  %s\n", i + 1,
                      (int)((now - h->last_us) / 1000000), track_tag(h->track), where, h->peak, h->snr,
                      isfinite(h->heading) ? to_dial(h->heading) : 0.0f, h->count > 99 ? 99 : h->count, flags);
    }
    say(ls_notes_mark("FIND hit log", body) ? "Hit log saved to NOTES" : "NOTES is busy; try again");
}

static void set_view(int v)
{
    s_view = (v + V_COUNT) % V_COUNT;
    settings_set_df_option(O_VIEW, s_view);
}

static void log_mode(bool on)
{
    s_logmode = on; s_log_pick = -1; s_log_top = 0; button_focus = -1;
}

static void log_pick(int row)
{
    const int total = ls_df_log_count(&s_log);
    if (row < 0 || row >= total) return;
    s_log_pick = row;
    const ls_df_hit_t *h = ls_df_log_at(&s_log, row);
    if (h && track_live(h->track)) s_sel = h->track;
}

/* ---- GO TO ---- */

EXT_RAM_BSS_ATTR static struct { double lat, lon; char name[40]; } s_places[32];
static int s_place_n;

static void place_done(int i)
{
    if (i < 0 || i >= s_place_n) return;
    ls_compass_set_target(s_places[i].lat, s_places[i].lon, s_places[i].name);
    say("Target set");
}

static void add_place(double lat, double lon, const char *name, const char *detail)
{
    if (s_place_n >= (int)(sizeof(s_places) / sizeof(s_places[0]))) return;
    s_places[s_place_n].lat = lat; s_places[s_place_n].lon = lon;
    snprintf(s_places[s_place_n].name, sizeof(s_places[0].name), "%s", name);
    s_place_n++;
    ls_picker_add(name, detail);
}

static void pick_place(void)
{
    s_place_n = 0;
    ls_picker_open("GO TO", place_done);
    char d[48];
    double lat, lon;
    ls_map_get_center(&lat, &lon);
    snprintf(d, sizeof(d), "%.5f, %.5f", lat, lon);
    add_place(lat, lon, "Map centre", d);
    float hl, ho;
    if (settings_get_home(&hl, &ho)) { snprintf(d, sizeof(d), "%.5f, %.5f", hl, ho); add_place(hl, ho, "Home", d); }
    if (s_fix.valid) { snprintf(d, sizeof(d), "+/-%.0f m", s_fix.radius_m); add_place(s_fix.lat, s_fix.lon, "DF fix", d); }
    ls_note_info_t n;
    for (int i = 0; ls_notes_at(i, &n) && s_place_n < 24; i++)
        if (n.has_place) { snprintf(d, sizeof(d), "note  %.5f, %.5f", n.lat, n.lon); add_place(n.lat, n.lon, n.title, d); }
    ls_mesh_peer_t p;
    for (int r = 0; r < 16 && ls_mesh_peer_at(r, &p); r++)
        if (p.has_loc) {
            snprintf(d, sizeof(d), "mesh node  %.0f dBm", p.rssi);
            add_place(p.lat_e6 / 1e6, p.lon_e6 / 1e6, p.name[0] ? p.name : p.id, d);
        }
}

static void action(int i)
{
    s_feedback[0] = 0;
    if (s_page == P_DIAL) {
        if (i == 0) { if (isfinite(s_lock)) s_lock = NAN; else s_lock = shown_heading(); }
        else if (i == 1) mark_note();
        else if (i == 2) more_done(M_CAL);
    } else if (s_page == P_FIND) {
        if (s_cal.phase != LS_DF_CAL_OFF) {
            if (i == 3) cal_step();
            else if (i == 4) cal_cancel();
            else if (i == 5 && s_cal.phase == LS_DF_CAL_TURN) cal_remark();
            return;
        }
        if (s_logmode) {
            if (i == 0) log_mode(false);
            else if (i == 1) save_log();
            else if (i == 2) { ls_df_log_clear(&s_log); s_log_pick = -1; s_log_top = 0; say("Hit log cleared"); }
            else if (i == 3) set_view(s_view + 1);
            return;
        }
        if (i == 0) pick_source();
        else if (i == 1) {
            if (s_dfs.tunable) channels_open(0);
            else if (s_dfs.targets) pick_target();
        }
        else if (i == 2) { s_method = s_method == LS_DF_PEAK ? LS_DF_NULL : LS_DF_PEAK; load_df_offset(); }
        else if (i == 3) save_bearing();
        else if (i == 4) { clear_all(); ls_df_log_clear(&s_log); }
        else if (i == 5) make_fix();
        else if (i == 6) set_view(s_view + 1);
        else if (i == 7) log_mode(true);
    } else if (s_page == P_GOTO) {
        if (i == 0) pick_place();
        else if (i == 1) { double lat, lon; if (ls_compass_target(&lat, &lon, NULL, 0)) { ls_map_center(lat, lon); open_app("map"); } }
        else if (i == 2) ls_compass_clear_target();
    }
}

static void show_page(int p)
{
    s_page = p; button_focus = -1; s_feedback[0] = 0;
    if (p == P_FIND && !s_find_on) find_start();
    if (p != P_FIND && s_find_on) { find_stop(); cal_end(); }
    if (p != P_FIND) s_logmode = false;
}

static bool key(ls_tk_t k, char ch)
{
    if (s_page == P_FIND && s_logmode && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        const int total = ls_df_log_count(&s_log);
        int row = s_log_pick < 0 ? 0 : s_log_pick + (k == LS_TK_DOWN ? 1 : -1);
        if (row < 0) row = 0;
        if (row >= total) row = total - 1;
        log_pick(row);
        return true;
    }
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT || k == LS_TK_TAB)
        return ls_btn_navigate(k, &button_slot, &button_focus, false);
    if (k == LS_TK_ENTER && button_focus >= 0) { if (ls_btn_enabled(0, button_focus)) action(button_focus); return true; }
    if (k == LS_TK_ESC && s_page == P_FIND && s_logmode) { log_mode(false); return true; }
    if (k != LS_TK_CHAR) return false;
    if (ch >= '1' && ch < '1' + P_COUNT) { show_page(ch - '1'); return true; }
    if (ch == 'o' || ch == 'O') { open_more(); return true; }
    if (ch == 'y' || ch == 'Y') { open_looks(); return true; }
    /* In FIND, [ and ] step the shown channel through the live ones. */
    if (s_page == P_FIND && (ch == '[' || ch == ']')) {
        for (int step = 1; step <= TRACKS; step++) {
            const int t = (s_sel + (ch == ']' ? step : TRACKS - step)) % TRACKS;
            if (track_live(t)) { s_sel = t; break; }
        }
        return true;
    }
    const int b = ls_btn_shortcut(ch, 0);
    if (b >= 0) { action(b); return true; }
    return false;
}

static bool touch(int col, int row)
{
    const int tab = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
    if (tab >= 0) {
        if (tab < P_COUNT) show_page(tab);
        else if (tab == P_COUNT) open_more();
        else open_looks();
        return true;
    }
    const int b = ls_btn_hit(col, row);
    if (b >= 0) { action(b); return true; }
    if (s_page == P_FIND && s_logmode && s_log_rows.h > 0 && row >= s_log_rows.y && row < s_log_rows.y + s_log_rows.h &&
        col >= s_log_rows.x && col < s_log_rows.x + s_log_rows.w)
        log_pick(s_log_top + row - s_log_rows.y);
    return true;
}

static void enter(void)
{
    static bool ready;
    if (!ready) { ready = true; ls_df_log_clear(&s_log); clear_all(); }
    button_focus = -1; button_slot = 0; s_feedback[0] = 0;
    const int opt = settings_get_compass_options();
    s_simple = opt & 1; s_magnetic = opt & 2;
    load_find_settings();
    ls_field_start(); ls_field_watch(true);
    s_spring.started = false; s_last_us = 0;
    load_df_offset();
    show_page(s_page);
}

static void leave(void)
{
    if (s_find_on) find_stop();
    cal_end();
    s_logmode = false;
    ls_field_watch(false);
}

/* ------------------------------------------------------ console: find -- */

/* The console runs in its own task; FIND's state belongs to the TUI's. A
   change is left here and picked up by the next frame (find_requests);
   the report only reads, and a torn number in it is harmless. */
enum { RQ_NONE, RQ_SRC, RQ_CH, RQ_CLEAR, RQ_VIEW, RQ_METHOD, RQ_SHOW, RQ_LOGCLEAR, RQ_SETTING };
static volatile int s_rq_kind;
static int s_rq_slot, s_rq_arg, s_rq_n, s_rq_val;
EXT_RAM_BSS_ATTR static uint32_t s_rq_hz[LS_DFS_CHANNELS];

static void find_requests(void)
{
    const int kind = s_rq_kind;
    if (kind == RQ_NONE) return;
    if (s_page == P_FIND && s_cal.phase == LS_DF_CAL_OFF) {
        if (kind == RQ_SRC && !s_rq_slot) pick_source_done(s_rq_arg);
        else if (kind == RQ_SRC) second_done(s_rq_arg);
        else if (kind == RQ_CH) {
            const int k = s_rq_slot;
            int n = 0;
            for (int i = 0; i < s_rq_n; i++) if (ls_dfs_in_range(slot_source(k), s_rq_hz[i])) s_ch[k][n++] = s_rq_hz[i];
            if (n) { s_nch[k] = n; s_sel = k * LS_DFS_CHANNELS; apply_channels(k); }
            else say("That radio reaches none of those");
        }
        else if (kind == RQ_CLEAR) { clear_all(); ls_df_log_clear(&s_log); s_log_pick = -1; }
        else if (kind == RQ_LOGCLEAR) { ls_df_log_clear(&s_log); s_log_pick = -1; }
        else if (kind == RQ_VIEW) set_view(s_rq_arg);
        else if (kind == RQ_METHOD) { s_method = (ls_df_method_t)s_rq_arg; load_df_offset(); }
        else if (kind == RQ_SHOW && track_live(s_rq_arg)) s_sel = s_rq_arg;
        else if (kind == RQ_SETTING) { s_opt[s_rq_arg] = s_rq_val; settings_set_df_option(s_rq_arg, s_rq_val); }
    }
    s_rq_kind = RQ_NONE;
}

static int parse_source(const char *name)
{
    if (!strcasecmp(name, "off")) return LS_DFS_COUNT;
    for (int i = 0; i < LS_DFS_COUNT; i++)
        if (!strncasecmp(ls_dfs_name((ls_dfs_t)i), name, strlen(name))) return i;
    return -1;
}

static void report_track(int t, int64_t now)
{
    const ls_df_sweep_t *sw = &s_sw[t];
    const ls_df_estimate_t *e = &s_te[t];
    const float noise = ls_df_log_noise(&s_log, t);
    const uint32_t f = track_freq(t);
    char est[32];
    if (e->valid) snprintf(est, sizeof(est), "%05.1fT +/-%.0f", e->bearing, e->spread);
    else snprintf(est, sizeof(est), "---");
    printf("find:  %c %10.4f  last %7.1f (%4.1fs)  noise %7.1f  over %5.1f  peak %7.1f  bins %2d  cov %3d  con %4.1f  est %s%s\n",
           track_tag(t), f / 1e6, s_tlev[t].last,
           s_tlev[t].last_us ? (double)(now - s_tlev[t].last_us) / 1e6 : -1.0, noise,
           isfinite(noise) && isfinite(s_tlev[t].last) ? s_tlev[t].last - noise : NAN,
           track_peak(t, now), (int)(sw->samples ? e->coverage / 5 : 0), e->coverage, e->contrast, est,
           t == s_sel ? "  <- shown" : "");
    if (s_tlev[t].clip_us && now - s_tlev[t].clip_us < 2000000)
        printf("find:    %c CLIPPING: these readings are kept out of the bearing\n", track_tag(t));
}

/* `find` on the console: what FIND hears, and control of it. */
bool ls_scr_compass_console(int argc, char **argv)
{
    const char *verb = argc > 1 ? argv[1] : "status";
    const int slot = argc > 3 && !strcmp(argv[argc - 1], "2") ? 1 : 0;
    if (!strcmp(verb, "status") || !strcmp(verb, "log")) {
        const int64_t now = esp_timer_get_time();
        printf("find: %s  method %s  view %s%s\n", s_page == P_FIND && s_find_on ? "FIND running" : "FIND not open (call ui.screen 9, tui key 2)",
               s_method == LS_DF_PEAK ? "PEAK" : "NULL", VIEW_NAMES[s_view], s_logmode ? "  LOG" : "");
        printf("find: heading raw %.1f  shown %.1f  %s  gyro %.1f deg/s  turn %.1f deg/s  tilt %.0f%s\n",
               raw_heading(), shown_heading(), ref(), s_rate, s_turn_rate, s_r.tilt, s_r.interference ? "  MAG CAUTION" : "");
        printf("find: hold %.0f s  fall %.1f dB/s  forget %.0f s  hit %.0f dB over  sure at %.0f  dwell %.0f ms\n",
               OPTV(O_HOLD), OPTV(O_DECAY), OPTV(O_FORGET), OPTV(O_THRESH), OPTV(O_SPIKE), OPTV(O_DWELL));
        for (int k = 0; k < LS_DFS_SLOTS; k++) {
            const ls_dfs_status_t *st = slot_status(k);
            if (!st->active) { if (k) printf("find: slot 2 off\n"); continue; }
            printf("find: slot %d %s  %d ch  on %d  %s  [%s]\n", k + 1, ls_dfs_name(st->source), st->channels,
                   st->channel + 1, st->unit, st->status);
            for (int c = 0; c < (st->channels ? st->channels : 1); c++) report_track(k * LS_DFS_CHANNELS + c, now);
        }
        const int total = ls_df_log_count(&s_log);
        printf("find: hits %d kept, %lu since clear\n", total, (unsigned long)s_log.total);
        const int rows = !strcmp(verb, "log") ? total : (total < 5 ? total : 5);
        for (int i = 0; i < rows; i++) {
            const ls_df_hit_t *h = ls_df_log_at(&s_log, i);
            char where[16], flags[24]; hit_where(h, where, sizeof(where)); hit_flags(h, flags, sizeof(flags));
            printf("find:  #%-2d %5.1fs ago  %c %-10s peak %6.1f  +%4.1f dB  facing %5.1f  %3ux over %4.1fs  %s\n",
                   i + 1, (double)(now - h->last_us) / 1e6, track_tag(h->track), where, h->peak, h->snr,
                   h->heading, h->count, (double)(h->last_us - h->start_us) / 1e6, flags);
        }
        return true;
    }
    /* A change is applied by FIND's own frame, so FIND has to be on screen. */
    if (s_page != P_FIND || !s_find_on) { printf("find: open FIND first: call ui.screen 9, then tui key 2\n"); return false; }
    if (s_rq_kind != RQ_NONE) { printf("find: the last change is still waiting for the screen\n"); return false; }
    if (!strcmp(verb, "src") && argc > 2) {
        const int src = parse_source(argv[2]);
        if (src < 0 || (src == LS_DFS_COUNT && !slot)) { printf("find: no radio called %s\n", argv[2]); return false; }
        s_rq_slot = slot; s_rq_arg = src; s_rq_kind = RQ_SRC;
    } else if (!strcmp(verb, "off2")) {
        s_rq_slot = 1; s_rq_arg = LS_DFS_COUNT; s_rq_kind = RQ_SRC;
    } else if (!strcmp(verb, "ch") && argc > 2) {
        s_rq_n = 0;
        char list[96]; snprintf(list, sizeof(list), "%s", argv[2]);
        for (char *tok = strtok(list, ","); tok && s_rq_n < LS_DFS_CHANNELS; tok = strtok(NULL, ","))
            s_rq_hz[s_rq_n++] = (uint32_t)llround(atof(tok) * 1e6);
        if (!s_rq_n) return false;
        s_rq_slot = slot; s_rq_kind = RQ_CH;
    } else if (!strcmp(verb, "clear")) s_rq_kind = RQ_CLEAR;
    else if (!strcmp(verb, "logclear")) s_rq_kind = RQ_LOGCLEAR;
    else if (!strcmp(verb, "view") && argc > 2) {
        int v = -1;
        for (int i = 0; i < V_COUNT; i++) if (!strcasecmp(argv[2], VIEW_NAMES[i])) v = i;
        if (v < 0) return false;
        s_rq_arg = v; s_rq_kind = RQ_VIEW;
    } else if (!strcmp(verb, "method") && argc > 2) {
        s_rq_arg = !strcasecmp(argv[2], "null") ? LS_DF_NULL : LS_DF_PEAK; s_rq_kind = RQ_METHOD;
    } else if (!strcmp(verb, "show") && argc > 2) {
        const char c = argv[2][0];
        const int t = c >= '1' && c <= '8' ? c - '1' : c >= 'A' && c <= 'H' ? LS_DFS_CHANNELS + c - 'A' :
                      c >= 'a' && c <= 'h' ? LS_DFS_CHANNELS + c - 'a' : -1;
        if (t < 0) return false;
        s_rq_arg = t; s_rq_kind = RQ_SHOW;
    } else if (!strcmp(verb, "set") && argc > 3) {
        static const char *const KEYS[O_TUNED] = { "hold", "fall", "forget", "hit", "sure", "blips", "dwell", "lobes" };
        int id = -1;
        for (int i = 0; i < O_TUNED; i++) if (!strcmp(argv[2], KEYS[i])) id = i;
        if (id < 0) { printf("find: set hold|fall|forget|hit|sure|blips|dwell|lobes <value>\n"); return false; }
        const float want = (float)atof(argv[3]);
        int best = 0;
        for (int v = 1; v < OPT[id].n; v++) if (fabsf(OPT[id].v[v] - want) < fabsf(OPT[id].v[best] - want)) best = v;
        s_rq_arg = id; s_rq_val = best; s_rq_kind = RQ_SETTING;
        printf("find: %s -> %g %s\n", argv[2], OPT[id].v[best], OPT[id].unit);
    } else {
        printf("find [status|log]  |  src <radio> [2]  off2  ch <MHz,MHz,...> [2]  show <1-8|A-H>\n"
               "     clear  logclear  view <dial|radar|heat>  method <peak|null>  set <name> <value>\n");
        return false;
    }
    return true;
}

const ls_tui_screen_t ls_scr_compass = {
    .name = "COMPASS", .hint = "1-4 page  O options  [ ] channel",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
    .hold_auto_rotation = true,
};
