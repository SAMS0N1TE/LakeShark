/* RF SURVEY: walk, and watch the band's heat build on the ground under you.

   One look answers "what is the band doing here, and where was it loud":
   the HEAT FIELD (the cells walked, each a stipple of dots, denser and
   hotter where the chosen channel was louder, the track through them and
   you at the centre) over the CHANNEL STRIP (every channel of the plan as a
   bar over its learnt floor, lit when it stands clear of it). A row landing
   flashes its cell; a pass moves the bars; nothing moves when nothing is
   heard. START, RADIO, BAND, MAP and OPTIONS sit on the shared keys; detail
   (spacing, cell size, threshold, replay, export) is in OPTIONS.

   Everything shown comes from ls_rfs_run's snapshot; the radio and the card
   are worked on the direction-finding task, never here. */

#include "../../ls_tui_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "../../ls_app.h"
#include "../../ls_braille.h"
#include "../../ls_field.h"
#include "../../ls_map.h"
#include "../../ls_map_ink.h"
#include "../../ls_motion.h"
#include "../../ls_numpad.h"
#include "../../ls_options.h"
#include "../../ls_picker.h"
#include "../../ls_radio_panel.h"
#include "../../ls_radio_select.h"
#include "../../ls_rfmap.h"
#include "../../ls_rfs.h"
#include "../../ls_rfs_run.h"
#include "../../ls_rfsim.h"
#include "../../ls_tui_ui.h"
#include "ls_textpack.h"

#define WHITE   TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define CYAN    TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
#define MAGENTA TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK)
#define YELLOW  TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
#define GREEN   TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
#define BLUE    TUI_ATTR(TUI_BLUE | TUI_BRIGHT, TUI_BLACK)
#define RED     TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)
#define M_PER_DEG_LAT 111132.0

EXT_RAM_BSS_ATTR static ls_rfs_settings_t s_set;
EXT_RAM_BSS_ATTR static ls_rfs_view_t s_v;
EXT_RAM_BSS_ATTR static ls_rfs_plan_t s_card[LS_RFS_CARD_PLANS];
static bool s_set_ready;
static int s_plan_pick;                 /* presets, then card plans, then CUSTOM */
static double s_lo_mhz = 902, s_hi_mhz = 928, s_cells = 16;
static ls_fresh_t s_row_fresh, s_pass_fresh;
static tui_rect s_heat, s_strip;
static int s_strip_x0, s_strip_pitch;
static char s_hint[96] = "S START  R RADIO  B BAND  M MAP  O OPTIONS";

static void settings_ready(void)
{
    if (s_set_ready) return;
    ls_rfs_settings_default(&s_set);
    /* Start on a radio that is here: the LoRa chip, else an SDR, else SIM. */
    static const ls_rfs_src_t TRY[] = { LS_RFS_SRC_LORA, LS_RFS_SRC_RTL, LS_RFS_SRC_HACKRF, LS_RFS_SRC_SIM };
    for (unsigned i = 0; i < sizeof(TRY) / sizeof(TRY[0]); i++)
        if (!ls_rfs_src_unavailable(TRY[i])) { s_set.src = TRY[i]; break; }
    s_set_ready = true;
}

static bool live(void) { return s_v.running || s_v.replaying; }
static const char *idle_why(const ls_opt_t *o) { (void)o; return live() ? "Stop the survey to change this" : NULL; }

/* ------------------------------------------------------------- pickers -- */

static void radio_extra(int row)
{
    static const ls_rfs_src_t X[] = { LS_RFS_SRC_SWEEP, LS_RFS_SRC_LR_IQ, LS_RFS_SRC_SIM };
    const char *why = ls_rfs_src_unavailable(X[row]);
    if (why) { ls_tui_status_set(why, NULL); return; }
    s_set.src = X[row];
}

static void radio_done(ls_rsel_radio_t r)
{
    ls_rfs_src_t src = LS_RFS_SRCS;
    if (r == LS_RSEL_SDR_RTL) src = LS_RFS_SRC_RTL;
    else if (r == LS_RSEL_SDR_HACKRF) src = LS_RFS_SRC_HACKRF;
    else if (r == LS_RSEL_LORA) src = LS_RFS_SRC_LORA;
    else if (r == LS_RSEL_CC1101) src = LS_RFS_SRC_CC1101;
    if (src == LS_RFS_SRCS) { ls_tui_status_set(LS_TEXT(t_scr_rfsurvey_that_radio_cannot_measure, "That radio cannot measure a channel list"), NULL); return; }
    s_set.src = src;
}

static void open_radio(void)
{
    if (live()) { ls_tui_status_set(LS_TEXT(t_scr_rfsurvey_stop_the_survey_to, "Stop the survey to change the radio"), NULL); return; }
    static ls_rsel_extra_t extra[3];
    extra[0] = (ls_rsel_extra_t){ "LORA SWEEP", ls_rfs_src_unavailable(LS_RFS_SRC_SWEEP) ? ls_rfs_src_unavailable(LS_RFS_SRC_SWEEP) : ls_rfs_src_detail(LS_RFS_SRC_SWEEP) };
    extra[1] = (ls_rsel_extra_t){ "LR2021 IQ", ls_rfs_src_unavailable(LS_RFS_SRC_LR_IQ) ? ls_rfs_src_unavailable(LS_RFS_SRC_LR_IQ) : ls_rfs_src_detail(LS_RFS_SRC_LR_IQ) };
    extra[2] = (ls_rsel_extra_t){ "SIMULATED", ls_rfs_src_detail(LS_RFS_SRC_SIM) };
    ls_rsel_open_with(LS_RSEL_FIND, radio_done, extra, 3, radio_extra);
}

static int card_count(void) { return ls_rfs_card_plans(s_card, LS_RFS_CARD_PLANS); }

static void custom_plan(void)
{
    if (!ls_rfs_plan_range(&s_set.plan, "CUSTOM", (uint32_t)(s_lo_mhz * 1e6), (uint32_t)(s_hi_mhz * 1e6), (int)s_cells))
        ls_tui_status_set(LS_TEXT(t_scr_rfsurvey_custom_range_lo_below, "CUSTOM range: LO below HI, 1 to 32 cells"), NULL);
}

static void band_done(int i)
{
    const int np = ls_rfs_preset_count(), nc = card_count();
    if (i < np) ls_rfs_preset(i, &s_set.plan);
    else if (i < np + nc) s_set.plan = s_card[i - np];
    else custom_plan();
    s_plan_pick = i;
    ls_rfs_show(-1, LS_RFS_MAX);
}

static void open_band(void)
{
    if (live()) { ls_tui_status_set(LS_TEXT(t_scr_rfsurvey_stop_the_survey_to_2, "Stop the survey to change the band"), NULL); return; }
    ls_picker_open("BAND", band_done);
    char detail[96];
    for (int i = 0; i < ls_rfs_preset_count(); i++) {
        ls_rfs_plan_t p; ls_rfs_preset(i, &p);
        LS_SNPRINTF(t_scr_rfsurvey_d_s_3f_3f, detail, sizeof(detail), "%d %s, %.3f-%.3f MHz", p.n, p.range ? "cells" : "channels",
                 (p.ch[0].hz - p.ch[0].bw_hz / 2) / 1e6, (p.ch[p.n - 1].hz + p.ch[p.n - 1].bw_hz / 2) / 1e6);
        ls_picker_add(p.name, detail);
    }
    const int nc = card_count();
    for (int i = 0; i < nc; i++) {
        LS_SNPRINTF(t_scr_rfsurvey_card_plans_json_d, detail, sizeof(detail), "card plans.json, %d %s", s_card[i].n, s_card[i].range ? "cells" : "channels");
        ls_picker_add(s_card[i].name, detail);
    }
    LS_SNPRINTF(t_scr_rfsurvey_3f_3f_mhz_in, detail, sizeof(detail), "%.3f-%.3f MHz in %d cells (OPTIONS > BAND)", s_lo_mhz, s_hi_mhz, (int)s_cells);
    ls_picker_add("CUSTOM", detail);
    if (!nc && ls_rfs_card_plans_why()) {
        char note[96]; snprintf(note, sizeof(note), "Card plans: %s", ls_rfs_card_plans_why());
        ls_picker_note(note);
    }
    ls_picker_select(s_plan_pick);
}

static void open_map(void)
{
    if (s_v.where_ok) { ls_map_follow_set(false); ls_map_center(s_v.lat, s_v.lon); }
    /* Street level, where a heat cell or an ellipse is a few cells across. */
    if (ls_map_zoom() < 16) ls_map_zoom_by(16 - ls_map_zoom());
    const ls_app_t *a = ls_app_by_id("map");
    const int i = a ? ls_tui_screen_index_of(a->screen) : -1;
    if (i >= 0) ls_tui_screen_show(i);
}

/* ------------------------------------------------------------- options -- */

static double num(const ls_opt_t *o)
{
    switch (o->arg) {
    case 0: return s_set.step_m;
    case 1: return s_set.cell_m;
    case 2: return s_set.threshold_db;
    case 3: return s_set.dwell_ms;
    case 4: return s_lo_mhz;
    case 5: return s_hi_mhz;
    default: return s_cells;
    }
}
static void set_num(const ls_opt_t *o, double v)
{
    switch (o->arg) {
    case 0: s_set.step_m = (float)v; break;
    case 1: s_set.cell_m = (float)v; break;
    case 2: s_set.threshold_db = (float)v; break;
    case 3: s_set.dwell_ms = (uint16_t)v; break;
    case 4: s_lo_mhz = v; break;
    case 5: s_hi_mhz = v; break;
    default: s_cells = v; break;
    }
}
static int get_wide(const ls_opt_t *o) { (void)o; return s_set.wide; }
static void set_wide(const ls_opt_t *o, int v) { (void)o; s_set.wide = v; }
static int get_metric(const ls_opt_t *o) { (void)o; ls_rfs_metric_t m; ls_rfs_shown(NULL, &m); return m; }
static void set_metric(const ls_opt_t *o, int v) { (void)o; int ch; ls_rfs_shown(&ch, NULL); ls_rfs_show(ch, (ls_rfs_metric_t)v); }
static const char *const METRICS[] = { "PEAK", "MEAN", "BUSY %", "OVER FLOOR" };

static void act_custom(const ls_opt_t *o) { (void)o; custom_plan(); s_plan_pick = ls_rfs_preset_count() + card_count(); }
static void act_replay(const ls_opt_t *o) { if (!ls_rfs_replay(NULL, o->arg)) ls_tui_status_set(LS_TEXT(t_scr_rfsurvey_busy_stop_the_survey, "Busy: stop the survey first"), NULL); }
static void act_export(const ls_opt_t *o) { (void)o; if (!ls_rfs_export(NULL)) ls_tui_status_set(LS_TEXT(t_scr_rfsurvey_nothing_to_export_yet, "Nothing to export yet"), NULL); }
static void act_clear(const ls_opt_t *o) { (void)o; ls_rfs_clear(); }
static void act_channel(const ls_opt_t *o) { (void)o; ls_rfmap_pick(); }

EXT_RAM_BSS_ATTR static char s_files[16][40];
static int s_nfiles;
static void replay_pick(int i) { if (i >= 0 && i < s_nfiles) ls_rfs_replay(s_files[i], 20); }
static void act_files(const ls_opt_t *o)
{
    (void)o;
    s_nfiles = ls_rfs_files(s_files, 16);
    ls_picker_open("REPLAY", replay_pick);
    for (int i = 0; i < s_nfiles; i++) ls_picker_add(s_files[i], LS_TEXT(t_scr_rfsurvey_replay_at_20_rows, "replay at 20 rows a second"));
    if (!s_nfiles) ls_picker_empty_reason(LS_TEXT(t_scr_rfsurvey_no_lsrf_files_in, "No .lsrf files in /sdcard/lakeshark/rfsurvey"));
    ls_rfs_files_refresh();
}
static void show_file(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%s", s_v.file[0] ? s_v.file : "none yet"); }

static const ls_opt_t O_SURVEY[] = {
    { .label = "ROW EVERY", .kind = LS_OPT_LEVEL, .arg = 0, .num = num, .set_num = set_num, .lo = 0, .hi = 500, .step = 5, .unit = "metres (0: every pass)", .why_not = idle_why },
    { .label = "HEAT CELL", .kind = LS_OPT_LEVEL, .arg = 1, .num = num, .set_num = set_num, .lo = 5, .hi = 1000, .step = 5, .unit = "metres a side", .why_not = idle_why },
    { .label = "BUSY OVER FLOOR", .kind = LS_OPT_LEVEL, .arg = 2, .num = num, .set_num = set_num, .lo = 3, .hi = 40, .step = 1, .unit = "dB", .why_not = idle_why },
    { .label = "DWELL", .kind = LS_OPT_LEVEL, .arg = 3, .num = num, .set_num = set_num, .lo = 150, .hi = 2000, .step = 50, .unit = "ms per retune (SDR, LoRa, CC1101)", .why_not = idle_why },
    { .label = "LR2021 WIDE IQ", .kind = LS_OPT_TOGGLE, .get = get_wide, .set = set_wide, .why_not = idle_why },
};
static const ls_opt_t O_BAND[] = {
    { .label = "CUSTOM LOW", .kind = LS_OPT_NUMBER, .arg = 4, .num = num, .set_num = set_num, .lo = 1, .hi = 6000, .unit = "MHz", .why_not = idle_why },
    { .label = "CUSTOM HIGH", .kind = LS_OPT_NUMBER, .arg = 5, .num = num, .set_num = set_num, .lo = 1, .hi = 6000, .unit = "MHz", .why_not = idle_why },
    { .label = "CUSTOM CELLS", .kind = LS_OPT_LEVEL, .arg = 6, .num = num, .set_num = set_num, .lo = 1, .hi = 32, .step = 1, .unit = "channels", .why_not = idle_why },
    { .label = "USE CUSTOM", .kind = LS_OPT_ACTION, .act = act_custom, .why_not = idle_why },
};
static const ls_opt_t O_HEAT[] = {
    { .label = "SHOW", .kind = LS_OPT_CYCLE, .names = METRICS, .n = 4, .get = get_metric, .set = set_metric, .step = 1 },
    { .label = "CHANNEL", .kind = LS_OPT_ACTION, .act = act_channel, .leaves = true },
    { .label = "CLEAR HEAT", .kind = LS_OPT_ACTION, .act = act_clear, .why_not = idle_why },
};
static const ls_opt_t O_FILES[] = {
    { .label = "FILE", .kind = LS_OPT_ACTION, .show = show_file },
    { .label = "REPLAY LATEST", .kind = LS_OPT_ACTION, .arg = 20, .act = act_replay, .leaves = true },
    { .label = "REPLAY AT ONCE", .kind = LS_OPT_ACTION, .arg = 0, .act = act_replay, .leaves = true },
    { .label = "REPLAY A FILE", .kind = LS_OPT_ACTION, .act = act_files, .leaves = true },
    { .label = "EXPORT CSV + GEOJSON", .kind = LS_OPT_ACTION, .act = act_export },
};
static void options_preview(tui_surface *sf, tui_rect a);
static const ls_opt_ctx_t C_SURVEY = { .name = "SURVEY", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_SURVEY) };
static const ls_opt_ctx_t C_BAND = { .name = "BAND", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_BAND) };
static const ls_opt_ctx_t C_HEAT = { .name = "HEAT", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_HEAT) };
static const ls_opt_ctx_t C_FILES = { .name = "FILES", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_FILES) };
static const ls_opt_t O_ROOT[] = {
    { .label = "SURVEY", .kind = LS_OPT_MENU, .sub = &C_SURVEY },
    { .label = "HEAT", .kind = LS_OPT_MENU, .sub = &C_HEAT },
    { .label = "BAND", .kind = LS_OPT_MENU, .sub = &C_BAND },
    { .label = "FILES", .kind = LS_OPT_MENU, .sub = &C_FILES },
};
static const ls_opt_ctx_t C_ROOT = { .name = "RF SURVEY", .back_to_screen = true, .preview = options_preview, .job = -1,
                                     .radio = LS_RSEL_NONE, LS_OPT_ROWS(O_ROOT) };

/* --------------------------------------------------------------- drawing -- */

static void put(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t at) { tui_put_str(sf, clip, x, y, s, at); }

/* The emblem: a hand-held with its whip, walking a field of heat. */
static void emblem(tui_surface *sf, tui_rect a, int x, int y)
{
    static const char *const ART[5] = {
        "   |  ) ) )",
        "  [=]       ",
        "  /|\\  . : + * #",
        "  / \\  ===========",
        " ..:++**##*+:.. ",
    };
    put(sf, a, x, y, ART[0], YELLOW);
    put(sf, a, x + 6, y, ") ) )", CYAN);
    put(sf, a, x, y + 1, ART[1], WHITE);
    put(sf, a, x, y + 2, "  /|\\", WHITE);
    for (int s = 0; s < 5; s++) tui_put_char(sf, a, x + 8 + 2 * s, y + 2, ls_rfmap_glyph(s), TUI_ATTR(ls_rfmap_hue(s), TUI_BLACK));
    put(sf, a, x, y + 3, "  / \\", WHITE);
    put(sf, a, x + 7, y + 3, "===========", MAGENTA);
    const char *g = ART[4];
    for (int i = 0; g[i]; i++) {
        const char *p = strchr(".:+*#", g[i]);
        if (p) tui_put_char(sf, a, x + i, y + 4, g[i], TUI_ATTR(ls_rfmap_hue((int)(p - ".:+*#")), TUI_BLACK));
    }
}

static void header(tui_surface *sf, tui_rect a)
{
    const char *state = s_v.busy ? "RF SURVEY / BUSY" : s_v.running ? "RF SURVEY / LIVE" :
                        s_v.replaying ? "RF SURVEY / REPLAY" : "RF SURVEY / READY";
    ls_radio_bezel(sf, a, state);
    int x = a.x + 2;
    const bool heard = ls_fresh(&s_pass_fresh, s_v.pass_serial, 700) > 128;
    x = ls_radio_chip(sf, a, x, a.y + 1, s_v.running ? "REC" : s_v.replaying ? "PLAY" : "IDLE", live(), TUI_RED);
    x = ls_radio_chip(sf, a, x, a.y + 1, "RX", heard, TUI_CYAN);
    const char *where = !s_v.running && !s_v.replaying ? "GPS" : !s_v.where_ok ? "NO FIX" :
                        s_v.where_flags & LS_RFS_ROW_SIM ? "SIM POS" : "GPS FIX";
    x = ls_radio_chip(sf, a, x, a.y + 1, where, live(), s_v.where_ok ? TUI_GREEN : TUI_YELLOW);
    char t[96];
    snprintf(t, sizeof(t), "%.14s", live() ? s_v.src_name : ls_rfs_src_name(s_set.src));
    ls_radio_chip(sf, a, x, a.y + 1, t, true, TUI_MAGENTA);
    const ls_rfs_plan_t *p = &s_set.plan;
    if (live()) snprintf(t, sizeof(t), "%s  %d ch  %s", s_v.plan, s_v.nch, s_v.unit);
    else LS_SNPRINTF(t_scr_rfsurvey_s_d_s_3f, t, sizeof(t), "%s  %d %s  %.3f-%.3f MHz", p->name, p->n, p->range ? "cells" : "ch",
                  (p->ch[0].hz - p->ch[0].bw_hz / 2) / 1e6, (p->ch[p->n - 1].hz + p->ch[p->n - 1].bw_hz / 2) / 1e6);
    put(sf, a, a.x + 2, a.y + 2, t, CYAN);
}

static void idle_art(tui_surface *sf, tui_rect a)
{
    /* The ground before a walk: a surveyor's grid of faint dots, one per
       heat cell, with the emblem and three lines in a clearing. */
    for (int r = 1; r < a.h; r += 2)
        for (int c = 1; c < a.w; c += 4) ls_braille_put(sf, a, a.x + c, a.y + r, (int16_t)0x2804, LS_ATTR_FAINT);
    const int x = a.x + (a.w - 18) / 2, y = a.y + a.h / 2 - 6;
    for (int r = y - 1; r < y + 12; r++)
        for (int c = a.x + 4; c < a.x + a.w - 4; c++) tui_put_char(sf, a, c, r, ' ', LS_ATTR_DIM);
    emblem(sf, a, x, y);
    static const char *const L[] = { "WALK A HEAT MAP OF THE BAND", "RADIO picks the receiver", "BAND picks the channels",
                                     "START, then walk or drive" };
    static const uint8_t H[] = { WHITE, CYAN, MAGENTA, GREEN };
    for (int i = 0; i < 4; i++) {
        const int n = (int)strlen(L[i]);
        put(sf, a, a.x + (a.w - n) / 2, y + 7 + i * (i ? 1 : 2) + (i ? 1 : 0), L[i], H[i]);
    }
}

/* Local metres to a heat cell index, as ls_rfs_grid_index does but on a
   const grid. */
static void cell_of(const ls_rfs_grid_t *g, double lat, double lon, int *ix, int *iy)
{
    double dlon = lon - g->lon0;
    if (dlon > 180) dlon -= 360;
    if (dlon < -180) dlon += 360;
    *ix = (int)floor(dlon * g->m_per_deg_lon / g->cell_m);
    *iy = (int)floor((lat - g->lat0) * M_PER_DEG_LAT / g->cell_m);
}

/* How much of a heat cell's dots are lit, by step: a quiet cell is a
   sprinkle, a loud one nearly solid, so the heat reads even without colour.
   An ordered dither over the cell's own dots, never the same as a block. */
static bool stipple(int x, int y, int step)
{
    static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    static const uint8_t LIT[6] = { 3, 6, 8, 11, 13, 16 };
    return BAYER[y & 3][x & 3] < LIT[step < 0 ? 0 : step > 5 ? 5 : step];
}

/* The heat around you. A heat cell is drawn `zoom` times two columns by
   `zoom` rows (about square on this glass), as large as lets the whole walk
   fit; north is up; you are the '@'. */
static void heat_field(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "HEAT FIELD", TUI_MAGENTA);
    const tui_rect in = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    s_heat = in;
    if (in.w < 8 || in.h < 4) return;
    int ch; ls_rfs_metric_t m; ls_rfs_shown(&ch, &m);
    const ls_rfs_grid_t *g = ls_rfs_grid_lock();
    if (!g->count) {
        ls_rfs_grid_unlock();
        if (!live()) idle_art(sf, in);
        else {
            const char *w = s_v.where_ok ? "First row lands after one pass" : "Waiting for a position: GPS or rfsim";
            put(sf, in, in.x + (in.w - (int)strlen(w)) / 2, in.y + in.h / 2, w, YELLOW);
        }
        return;
    }
    float lo = 0, hi = 0;
    const bool any = ls_rfs_grid_range(g, ch, m, &lo, &hi);
    if (m == LS_RFS_OCC) { lo = 0; hi = 100; }
    else if (hi - lo < 12) lo = hi - 12;
    /* The walk's extent, and where we are. */
    int bx0 = g->cell[0].ix, bx1 = bx0, by0 = g->cell[0].iy, by1 = by0;
    for (int i = 1; i < g->count; i++) {
        if (g->cell[i].ix < bx0) bx0 = g->cell[i].ix;
        if (g->cell[i].ix > bx1) bx1 = g->cell[i].ix;
        if (g->cell[i].iy < by0) by0 = g->cell[i].iy;
        if (g->cell[i].iy > by1) by1 = g->cell[i].iy;
    }
    double tl[1], tn[1];
    const bool have_track = ls_rfs_track(tl, tn, 1) > 0;
    int px = 0, py = 0;
    if (s_v.where_ok) cell_of(g, s_v.lat, s_v.lon, &px, &py);
    else if (have_track) cell_of(g, tl[0], tn[0], &px, &py);
    const bool here = s_v.where_ok || have_track;
    if (here) { if (px < bx0) bx0 = px; if (px > bx1) bx1 = px; if (py < by0) by0 = py; if (py > by1) by1 = py; }
    int zoom = 4;
    while (zoom > 1 && ((bx1 - bx0 + 3) * 2 * zoom > in.w || (by1 - by0 + 3) * zoom > in.h)) zoom--;
    const int cols = in.w / (2 * zoom), rows = in.h / zoom;
    /* The whole walk when it fits; otherwise follow where we are. */
    int ccx = (bx0 + bx1 + 1) / 2, ccy = (by0 + by1 + 1) / 2;
    if ((bx1 - bx0 + 1 > cols || by1 - by0 + 1 > rows) && here) { ccx = px; ccy = py; }
    const int ix0 = ccx - cols / 2, iy0 = ccy + rows / 2;       /* the top row is the most northern */
    const int ox = in.x + (in.w - cols * 2 * zoom) / 2, oy = in.y + (in.h - rows * zoom) / 2;
    /* The newest cell, lit white for a moment when a row lands in it. */
    int nx = 0, ny = 0;
    const bool fresh = have_track && ls_fresh(&s_row_fresh, s_v.rows, 900) > 96;
    if (fresh) cell_of(g, tl[0], tn[0], &nx, &ny);
    const tui_rect canvas = tui_rect_make(ox, oy, cols * 2 * zoom, rows * zoom);
    ls_ink_begin_braille(canvas);
    ls_ink_set_tint(0);
    const int cw = 4 * zoom, chh = 4 * zoom;       /* a heat cell in braille dots */
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const int ix = ix0 + c, iy = iy0 - r;
            const int idx = ls_rfs_grid_find(g, ix, iy);
            float v;
            if (idx < 0 || !any || !ls_rfs_grid_value(g, idx, ch, m, &v)) {
                /* Ground inside the walk: one faint dot a cell, a surveyor's grid. */
                if (ix >= bx0 && ix <= bx1 && iy >= by0 && iy <= by1) ls_ink_dot(c * cw + cw / 2, r * chh + chh / 2, LS_ATTR_FAINT, 1);
                continue;
            }
            const bool lit = fresh && ix == nx && iy == ny;
            const int step = lit ? 5 : ls_rfmap_step(v, lo, hi);
            const uint8_t at = lit ? WHITE : TUI_ATTR(ls_rfmap_hue(step), TUI_BLACK);
            for (int dy = 0; dy < chh; dy++)
                for (int dx = 0; dx < cw; dx++)
                    if (stipple(dx, dy, step)) ls_ink_dot(c * cw + dx, r * chh + dy, at, 2);
        }
    /* The track, a dotted line through the walk. */
    EXT_RAM_BSS_ATTR static double lat[LS_RFS_TRACK], lon[LS_RFS_TRACK];
    const int n = ls_rfs_track(lat, lon, LS_RFS_TRACK);
    const double cell = g->cell_m, mlon = g->m_per_deg_lon, la0 = g->lat0, lo0 = g->lon0;
    ls_rfs_grid_unlock();
    int lx = 0, ly = 0; bool prev = false;
    for (int i = 0; i < n; i++) {
        const double fx = (lon[i] - lo0) * mlon / cell - ix0, fy = iy0 + 1 - (lat[i] - la0) * M_PER_DEG_LAT / cell;
        const int x = (int)floor(fx * cw), y = (int)floor(fy * chh);
        if (prev && (x != lx || y != ly)) ls_ink_line(lx, ly, x, y, TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK), 1, -2);
        lx = x; ly = y; prev = true;
    }
    ls_ink_flush(sf, tui_rect_make(0, -1, 0, 0));
    /* You, north, and the scale, as characters over the dots. */
    if (here) {
        const int ux = ox + (px - ix0) * 2 * zoom + zoom - 1, uy = oy + (iy0 - py) * zoom + zoom / 2;
        if (tui_rect_contains(in, ux, uy)) put(sf, in, ux, uy, "@", s_v.where_ok ? WHITE : LS_ATTR_DIM);
    }
    put(sf, in, in.x + in.w - 3, in.y, "N^", YELLOW);
    char scale[24];
    const int span = cols >= 8 ? 4 : 2;                     /* heat cells the bar spans */
    const int bar = span * 2 * zoom;
    snprintf(scale, sizeof(scale), "%.0f m", span * cell);
    tui_put_char(sf, in, in.x, in.y + in.h - 1, '|', LS_ATTR_DIM);
    for (int i = 1; i < bar; i++) tui_put_char(sf, in, in.x + i, in.y + in.h - 1, '-', LS_ATTR_DIM);
    tui_put_char(sf, in, in.x + bar, in.y + in.h - 1, '|', LS_ATTR_DIM);
    put(sf, in, in.x + bar + 2, in.y + in.h - 1, scale, LS_ATTR_DIM);
    /* What the colours are, at the foot. */
    char lab[48];
    snprintf(lab, sizeof(lab), "%.0f", lo);
    int x = in.x + in.w - 18;
    put(sf, in, x, in.y + in.h - 1, lab, LS_ATTR_DIM);
    x += (int)strlen(lab) + 1;
    for (int s = 0; s < 5; s++) tui_put_char(sf, in, x + s, in.y + in.h - 1, ls_rfmap_glyph(s), TUI_ATTR(ls_rfmap_hue(s), TUI_BLACK));
    snprintf(lab, sizeof(lab), " %.0f%s", hi, m == LS_RFS_OCC ? "%" : "");
    put(sf, in, x + 5, in.y + in.h - 1, lab, LS_ATTR_DIM);
}

/* Every channel as a bar over its floor: '|' from the floor up to the
   level, '-' the floor itself, '.' the room above; lit in the heat colours
   when it stands clear of the floor. */
static void channel_strip(tui_surface *sf, tui_rect a)
{
    int ch; ls_rfs_metric_t m; ls_rfs_shown(&ch, &m);
    const int nch = live() ? s_v.nch : s_set.plan.n;
    const ls_rfs_ch_t *chs = live() ? s_v.ch : s_set.plan.ch;
    char title[64];
    if (ch >= 0 && ch < nch && isfinite(s_v.level[ch]))
        snprintf(title, sizeof(title), "CH %s  %.1f %s%s", chs[ch].label, s_v.level[ch], s_v.unit, (s_v.occ >> ch) & 1 ? "  BUSY" : "");
    else if (ch >= 0 && ch < nch) snprintf(title, sizeof(title), "CH %s  %.4f MHz", chs[ch].label, chs[ch].hz / 1e6);
    else snprintf(title, sizeof(title), "CHANNELS  %d  %s", nch, live() ? s_v.unit : "");
    ls_panel_box(sf, a, title, TUI_CYAN);
    s_strip = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    const int h = a.h - 4;                   /* bar rows; one for labels, two frame */
    if (h < 2 || nch < 1) return;
    s_strip_pitch = (a.w - 4) / nch >= 2 ? ((a.w - 4) / nch > 6 ? 6 : (a.w - 4) / nch) : 1;
    s_strip_x0 = a.x + 2 + ((a.w - 4) - s_strip_pitch * nch) / 2;
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < nch && (s_v.passes > 0 || s_v.replaying); i++) {
        if (isfinite(s_v.floor[i]) && s_v.floor[i] < lo) lo = s_v.floor[i];
        if (isfinite(s_v.level[i]) && s_v.level[i] > hi) hi = s_v.level[i];
    }
    if (!isfinite(lo) || !isfinite(hi)) { lo = -120; hi = -40; }
    lo -= 3;
    if (hi - lo < 30) hi = lo + 30;
    const int base = a.y + 1 + h;            /* the row under the bars */
    for (int i = 0; i < nch; i++) {
        const int x = s_strip_x0 + i * s_strip_pitch;
        const bool data = s_v.passes > 0 || s_v.replaying;
        const float lv = data ? s_v.level[i] : NAN, fl = data ? s_v.floor[i] : NAN;
        const int top = isfinite(lv) ? (int)lroundf((lv - lo) / (hi - lo) * h) : 0;
        const int fr = isfinite(fl) ? (int)lroundf((fl - lo) / (hi - lo) * h) : -1;
        const bool busy = (s_v.occ >> i) & 1;
        const float over = isfinite(lv) && isfinite(fl) ? lv - fl : 0;
        const uint8_t lit = busy ? TUI_ATTR(ls_rfmap_hue(over > 30 ? 4 : over > 20 ? 3 : 2), TUI_BLACK) : TUI_ATTR(TUI_BLUE | TUI_BRIGHT, TUI_BLACK);
        for (int r = 0; r < h; r++) {
            const int y = base - 1 - r;
            char c = '.'; uint8_t at = LS_ATTR_FAINT;
            if (r == fr) { c = '-'; at = LS_ATTR_DIM; }
            if (r < top && r > fr) { c = '|'; at = lit; }
            if (r < top && r <= fr) { c = ':'; at = TUI_ATTR(TUI_BLUE, TUI_BLACK); }
            tui_put_char(sf, a, x, y, c, at);
            for (int k = 1; k < s_strip_pitch - 1 && k < 3; k++) tui_put_char(sf, a, x + k, y, c == '.' ? ' ' : c, at);
        }
        char lab = i < 10 ? (char)('0' + i) : (char)('A' + i - 10);
        tui_put_char(sf, a, x, base, lab, i == ch ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : LS_ATTR_DIM);
    }
    if (s_v.passes > 0 || s_v.replaying) {
        char scale[24];
        snprintf(scale, sizeof(scale), "%.0f", hi);
        put(sf, a, a.x + a.w - 1 - (int)strlen(scale), a.y + 1, scale, LS_ATTR_FAINT);
    }
}

static void stats(tui_surface *sf, tui_rect a, int y)
{
    char t[96];
    if (s_v.replaying) LS_SNPRINTF(t_scr_rfsurvey_replay_lu_lu_walked, t, sizeof(t), "REPLAY %lu/%lu  WALKED %.2f km  CELLS %d", (unsigned long)s_v.replay_done,
                                (unsigned long)s_v.replay_total, s_v.walked_m / 1000, s_v.cells);
    else LS_SNPRINTF(t_scr_rfsurvey_rows_lu_walked_2f, t, sizeof(t), "ROWS %lu  WALKED %.2f km  CELLS %d  %02lu:%02lu", (unsigned long)s_v.rows, s_v.walked_m / 1000,
                  s_v.cells, (unsigned long)(s_v.elapsed_s / 60), (unsigned long)(s_v.elapsed_s % 60));
    put(sf, a, a.x, y, t, WHITE);
    if (s_v.status[0]) put(sf, a, a.x, y + 1, s_v.status, s_v.running ? GREEN : YELLOW);
    else put(sf, a, a.x, y + 1, s_v.file[0] ? s_v.file : "tap the heat or a bar to pick a channel", LS_ATTR_DIM);
}

static void options_preview(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "RF SURVEY / PLAN", TUI_MAGENTA);
    emblem(sf, a, a.x + 2, a.y + 2);
    char t[64];
    snprintf(t, sizeof(t), "%s on %s", ls_rfs_src_name(s_set.src), s_set.plan.name);
    put(sf, a, a.x + 2, a.y + 8, t, WHITE);
    LS_SNPRINTF(t_scr_rfsurvey_row_every_0f_m, t, sizeof(t), "Row every %.0f m, cells %.0f m", s_set.step_m, s_set.cell_m);
    put(sf, a, a.x + 2, a.y + 10, t, CYAN);
    LS_SNPRINTF(t_scr_rfsurvey_busy_at_0f_db, t, sizeof(t), "Busy at %.0f dB over the floor", s_set.threshold_db);
    put(sf, a, a.x + 2, a.y + 11, t, GREEN);
    put(sf, a, a.x + 2, a.y + 13, LS_TEXT(t_scr_rfsurvey_files_sdcard_lakeshark_rfsurvey, "Files: /sdcard/lakeshark/rfsurvey"), LS_ATTR_DIM);
    put(sf, a, a.x + 2, a.y + 14, LS_TEXT(t_scr_rfsurvey_export_writes_csv_and, "EXPORT writes .csv and .geojson"), LS_ATTR_DIM);
}

static void draw(tui_surface *sf, tui_rect area)
{
    settings_ready();
    ls_rfs_view(&s_v);
    const bool wide = ls_tui_is_wide();
    const int bar_h = ls_btn_raised_height(area, 5);
    const int head_h = 4;
    const int body_y = area.y + head_h, body_h = area.h - head_h - bar_h - 3;
    header(sf, tui_rect_make(area.x, area.y, area.w, head_h));
    if (wide) {
        const int hw = area.w * 11 / 20;
        heat_field(sf, tui_rect_make(area.x, body_y, hw, body_h));
        channel_strip(sf, tui_rect_make(area.x + hw, body_y, area.w - hw, body_h));
    } else {
        const int strip_h = body_h / 3 < 10 ? 10 : body_h / 3 > 17 ? 17 : body_h / 3;
        heat_field(sf, tui_rect_make(area.x, body_y, area.w, body_h - strip_h));
        channel_strip(sf, tui_rect_make(area.x, body_y + body_h - strip_h, area.w, strip_h));
    }
    stats(sf, area, area.y + area.h - bar_h - 3);
    if (!wide) ls_safe_line(sf, area, area.y + area.h - bar_h - 1, s_hint, LS_ATTR_DIM);
    char src[24], plan[24];
    snprintf(src, sizeof(src), "%.12s", live() ? s_v.src_name : ls_rfs_src_name(s_set.src));
    snprintf(plan, sizeof(plan), "%.12s", live() ? s_v.plan : s_set.plan.name);
    const ls_btn_t b[] = {
        { s_v.busy ? "WAIT" : live() ? "STOP" : "START", s_v.replaying ? "replay" : NULL, 's', live(), s_v.busy, false },
        { "RADIO", src, 'r', false, live(), false },
        { "BAND", plan, 'b', false, live(), false },
        { "MAP", s_v.cells ? "heat" : NULL, 'm', false, false, s_v.cells > 0 },
        ls_opt_button(&C_ROOT),
    };
    ls_btn_bar_raised(sf, tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h), b, 5, -1);
}

/* ---------------------------------------------------------------- input -- */

static void start_stop(void)
{
    if (s_v.busy) return;
    if (live()) { ls_rfs_stop(); return; }
    if (!ls_rfs_start(&s_set)) { ls_rfs_view(&s_v); ls_tui_status_set(s_v.status[0] ? s_v.status : "Could not start", NULL); }
}

static bool key(ls_tk_t k, char ch)
{
    if (k != LS_TK_CHAR) return false;
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    switch (ch) {
    case 's': start_stop(); return true;
    case 'r': open_radio(); return true;
    case 'b': open_band(); return true;
    case 'm': open_map(); return true;
    case 'c': ls_rfmap_pick(); return true;
    default: return ls_opt_key(&C_ROOT, ch);
    }
}

static bool touch(int col, int row)
{
    const int b = ls_btn_hit(col, row);
    if (b >= 0) return key(LS_TK_CHAR, "srbmo"[b]);
    if (tui_rect_contains(s_strip, col, row) && s_strip_pitch > 0) {
        const int nch = live() ? s_v.nch : s_set.plan.n;
        const int i = (col - s_strip_x0) / s_strip_pitch;
        ls_rfs_metric_t m; ls_rfs_shown(NULL, &m);
        int cur; ls_rfs_shown(&cur, NULL);
        if (col >= s_strip_x0 && i >= 0 && i < nch) ls_rfs_show(cur == i ? -1 : i, m);
        return true;
    }
    if (tui_rect_contains(s_heat, col, row)) { ls_rfmap_pick(); return true; }
    return true;
}

static void enter(void)
{
    settings_ready();
    ls_field_start();
    ls_rfs_files_refresh();
}

const ls_tui_screen_t ls_scr_rfsurvey = {
    .name = "RF SURVEY",
    .hint = s_hint,
    .enter = enter,
    .draw = draw,
    .key = key,
    .touch = touch,
};
