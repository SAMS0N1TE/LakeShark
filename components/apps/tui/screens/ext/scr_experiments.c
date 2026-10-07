/* EXPERIMENTS: the list of experiments (ls_experiments.h), and a page for
   each with START/STOP, RADIO, OPTIONS and its live readout. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_motion.h"
#include "../../ls_scan_look.h"
#include "../../ls_experiments.h"
#include "../../ls_notes.h"
#include "../../ls_notify.h"
#include "../../ls_keyboard.h"
#include "../../experiments/lr433_history.h"
#include "../../experiments/rs41_store.h"
#include "../../ls_geo.h"
#include "../../ls_compass_live.h"
#include "../../ls_route_live.h"
#include "ls_gps.h"
#include "esp_timer.h"
#include "core/ls_time.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define LIVE_MAX 24

/* -1 for the list, else the experiment open. */
static int s_open = -1;
static int s_sel;
static int s_focus = -1, s_slot;
/* What the bar drawn last holds, for keys and taps. */
static char s_bar_keys[6];
static bool s_bar_dim[6];
static int s_bar_n;
/* The list's rows as drawn, for taps: two cells each. */
static tui_rect s_rows_hit;
static int s_rows_first;
/* OPTIONS keeps a pointer to its context while its list is up. */
static ls_opt_ctx_t s_ctx;
static EXT_RAM_BSS_ATTR char s_live[LIVE_MAX][LS_EXP_LINE];
static EXT_RAM_BSS_ATTR char s_feedback[64];
static EXT_RAM_BSS_ATTR char s_hint[64];
/* A NOTE press whose write the notes worker has not finished yet. */
static bool s_note_wait;
static bool s_history;
static int s_sensor_sel, s_sensor_detail = -1, s_graph;
static uint8_t s_sensor_order[SH_SENSORS];
static int s_sensor_count, s_sensor_first;
static tui_rect s_sensor_hit;
static EXT_RAM_BSS_ATTR sh_sensor_t s_sensor;
static EXT_RAM_BSS_ATTR rs41_sonde_t s_sonde;
static EXT_RAM_BSS_ATTR ls_gps_state_t s_sonde_gps;
static int s_sonde_sel, s_sonde_detail = -1, s_sonde_count, s_sonde_first;
static uint8_t s_sonde_order[RS41_SONDES];
static tui_rect s_sonde_hit;
static bool is_rs41(const ls_experiment_t *e) { return e && !strcmp(e->id,"rs41"); }


static bool sensor_probe(ls_notice_t *n)
{
    float c;
    char name[SH_NAME];
    if (!lr433_history_alert(name, sizeof(name), &c)) return false;
    snprintf(n->title, sizeof(n->title), "SENSOR ALERT");
    bool f = lr433_history_fahrenheit();
    snprintf(n->body, sizeof(n->body), "%s: %.1f %c above threshold", name, (double)(f ? c * 1.8f + 32 : c), f ? 'F' : 'C');
    n->accent = TUI_YELLOW | TUI_BRIGHT;
    n->haptic_only = true;
    return true;
}

static bool is_lr433(const ls_experiment_t *e) { return e && !strcmp(e->id, "lr433"); }

static const ls_experiment_t *opened(void) { return s_open >= 0 ? ls_exp_at(s_open) : NULL; }

static const ls_opt_ctx_t *options(const ls_experiment_t *e)
{
    /* Under OPTIONS on the button: the console name, which is short, in
       the button's capitals. */
    static char tag[12];
    if (!e || !e->opts || e->n_opts <= 0) return NULL;
    size_t i = 0;
    for (; e->id[i] && i + 1 < sizeof(tag); i++)
        tag[i] = e->id[i] >= 'a' && e->id[i] <= 'z' ? (char)(e->id[i] - 'a' + 'A') : e->id[i];
    tag[i] = 0;
    s_ctx = (ls_opt_ctx_t){ .name = e->name, .job = e->no_radio ? -1 : LS_RSEL_EXPERIMENT,
                            .radio = LS_RSEL_NONE, .opt = e->opts, .n = e->n_opts, .tag = tag };
    return &s_ctx;
}

static uint8_t badge_attr(ls_exp_maturity_t m)
{
    switch (m) {
    case LS_EXP_WORKS:  return TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    case LS_EXP_TRYING: return TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    default:            return LS_ATTR_DIM;
    }
}

static void open_page(int i)
{
    if (!ls_exp_at(i)) return;
    s_open = i;
    s_history = false;
    s_focus = -1;
    s_feedback[0] = 0;
}

/* NOTE: the open experiment's readout, or the running one's from the list,
   saved as a note the way COMPASS and MAP mark one. */
static void save_note(const ls_experiment_t *e)
{
    s_note_wait = false;
    if (!e) { snprintf(s_feedback, sizeof(s_feedback), "Nothing running to save"); return; }
    const char *st = ls_notes_status();
    if (!strncmp(st, "No SD", 5) || !strncmp(st, "SAVE FAILED", 11)) {
        snprintf(s_feedback, sizeof(s_feedback), "NOTES unavailable: no SD card");
        return;
    }
    EXT_RAM_BSS_ATTR static char lines[LIVE_MAX][LS_EXP_LINE];
    EXT_RAM_BSS_ATTR static char body[1200];
    char title[LS_NOTE_TITLE], stamp[LS_TIME_STAMP_MAX], state[80];
    const int n = ls_exp_read_lines(e, lines, LIVE_MAX);
    ls_exp_state_line(e, state, sizeof(state));
    ls_time_render_stamp(stamp, sizeof(stamp));
    ls_exp_note_text(e, stamp, state, (const char (*)[LS_EXP_LINE])lines, n, title, sizeof(title), body, sizeof(body));
    if (ls_notes_mark(title, body)) {
        s_note_wait = true;
        snprintf(s_feedback, sizeof(s_feedback), "SAVING TO NOTES...");
    } else {
        snprintf(s_feedback, sizeof(s_feedback), "NOTES is busy; try again");
    }
}

/* Settles the message once the notes worker has finished the write. */
static void note_progress(void)
{
    if (!s_note_wait) return;
    const char *st = ls_notes_status();
    if (!strncmp(st, "Saving", 6) || !strncmp(st, "Still saving", 12)) return;
    s_note_wait = false;
    if (!strncmp(st, "SAVE FAILED", 11) || !strncmp(st, "No SD", 5))
        snprintf(s_feedback, sizeof(s_feedback), "NOTES unavailable: SAVE FAILED, check SD");
    else
        snprintf(s_feedback, sizeof(s_feedback), "SAVED TO NOTES");
}

static void close_page(void)
{
    s_sel = s_open >= 0 ? s_open : s_sel;
    s_open = -1;
    s_focus = -1;
    s_feedback[0] = 0;
}

/* Running, or about to be: what START/STOP shows and does. */
static bool is_running_or_starting(const ls_experiment_t *e)
{
    return ls_exp_busy() ? ls_exp_wanted() == e : ls_exp_running() == e;
}

static void action(char k)
{
    const ls_experiment_t *e = opened();
    if (s_history && is_rs41(e)) {
        if (k == 'b') { if (s_sonde_detail >= 0) s_sonde_detail = -1; else s_history = false; s_focus = -1; return; }
        if (k == 'g') {
            if (rs41_store_copy(s_sonde_detail,&s_sonde,esp_timer_get_time()) && s_sonde.fix_us) {
                ls_route_live_clear();
                ls_compass_set_target(s_sonde.report.lat,s_sonde.report.lon,s_sonde.report.serial);
                snprintf(s_feedback,sizeof(s_feedback),"COMPASS target: %s / last received fix",s_sonde.report.serial);
            }
            return;
        }
        if (k == 'o') { ls_opt_open(options(e)); return; }
    }
    if (s_history && is_lr433(e)) {
        if (k == 'b') { if (s_sensor_detail >= 0) s_sensor_detail = -1; else s_history = false; s_focus = -1; return; }
        if (k == 'g') { s_graph = (s_graph + 1) % 5; return; }
        if (k == 'o') { ls_opt_open(lr433_history_options(s_sensor_detail)); return; }
        if (k == 'n' && lr433_history_copy(s_sensor_detail, &s_sensor)) {
            lr433_history_options(s_sensor_detail);
            ls_keyboard_open("NAME SENSOR", s_sensor.name, SH_NAME - 1, lr433_history_name);
            return;
        }
    }
    for (int i = 0; i < s_bar_n; i++)
        if (s_bar_keys[i] == k && s_bar_dim[i]) {
            if (k == 'r') snprintf(s_feedback, sizeof(s_feedback), "Stop it to change the radio");
            if (k == 'n') snprintf(s_feedback, sizeof(s_feedback), "Nothing running to save");
            return;
        }
    s_feedback[0] = 0;
    s_note_wait = false;
    switch (k) {
    case 'h':
        if (is_rs41(e)) { s_history = true; s_sonde_detail = -1; s_focus = -1; }
        if (is_lr433(e)) { s_history = true; s_sensor_detail = -1; s_focus = -1; }
        break;
    case 'n':
        save_note(e ? e : ls_exp_running());
        break;
    case 'b':
        if (e) close_page();
        else ls_tui_screen_show(0);
        break;
    case 'x':
        ls_exp_stop();
        break;
    case 's':
        if (!e) break;
        if (is_running_or_starting(e)) ls_exp_stop();
        else ls_exp_start(e);
        break;
    case 'r':
        if (e && !e->no_radio) ls_rsel_open(LS_RSEL_EXPERIMENT, NULL);
        break;
    case 'o':
        ls_opt_open(options(e));
        break;
    default:
        break;
    }
}

/* The bar along the top: BACK first, then the page's own controls. The
   sizing rule SUB-GHZ pages use: three rows in a wide pane when the labels
   fit the compact form, the raised form otherwise. */
static void page_bar(tui_surface *sf, tui_rect *a, const ls_btn_t *btn, int n)
{
    const bool wide = a->w > a->h * 2;
    const bool thumbs = !ls_tui_keyboard_mode() && a->h >= 22;
    int h = wide ? (!thumbs && ls_btn_compact_fits(tui_rect_make(a->x, a->y, a->w, 3), btn, n) ? 3 : 4)
                 : ls_btn_raised_height(*a, n);
    if (h > a->h - 6) h = a->h > 9 ? 3 : 0;
    s_bar_n = 0;
    if (h <= 0) return;
    ls_btn_bar_raised(sf, tui_rect_make(a->x, a->y, a->w, h), btn, n, s_focus);
    s_bar_n = n < (int)sizeof(s_bar_keys) ? n : (int)sizeof(s_bar_keys);
    for (int i = 0; i < s_bar_n; i++) { s_bar_keys[i] = btn[i].key; s_bar_dim[i] = btn[i].dim; }
    a->y += h;
    a->h -= h;
}

/* `text` across up to `rows` lines of `r`, broken at spaces. Returns the
   lines used. */
static int wrap(tui_surface *sf, tui_rect r, int y, int rows, const char *text, uint8_t attr)
{
    const int w = r.w;
    int used = 0;
    while (*text && used < rows && w > 0) {
        int n = (int)strlen(text);
        if (n > w) {
            n = w;
            while (n > 0 && text[n] != ' ') n--;
            if (n == 0) n = w;
        }
        char line[LS_EXP_LINE * 2];
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        memcpy(line, text, (size_t)n);
        line[n] = 0;
        tui_put_str(sf, r, r.x, y + used, line, attr);
        used++;
        text += n;
        while (*text == ' ') text++;
    }
    return used;
}

/* ------------------------------------------------------------------ list */

static void draw_list(tui_surface *sf, tui_rect a)
{
    const ls_experiment_t *run = ls_exp_running();
    const ls_btn_t btn[] = {
        { "BACK", "HOME", 'b', false, false },
        { "STOP", run ? run->name : "NOTHING ON", 'x', run != NULL, run == NULL },
        { "NOTE", run ? "SAVE" : "NOTHING ON", 'n', false, run == NULL },
    };
    note_progress();
    page_bar(sf, &a, btn, 3);
    const int count = ls_exp_count();
    if (s_sel >= count) s_sel = count ? count - 1 : 0;
    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - 1);
    ls_panel_box(sf, body, "EXPERIMENTS", TUI_CYAN);
    ls_motion_busy(sf, body, run != NULL);
    tui_rect list = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    s_rows_hit = tui_rect_make(0, 0, 0, 0);
    if (list.w < 4 || list.h < 2) return;
    if (!count) {
        tui_put_str(sf, list, list.x, list.y, "No experiments in this build", LS_ATTR_DIM);
    } else {
        int rows = list.h / 2;
        if (rows < 1) rows = 1;
        const int first = s_sel / rows * rows;
        s_rows_hit = list;
        s_rows_first = first;
        for (int i = 0; i < rows && first + i < count; i++) {
            const ls_experiment_t *e = ls_exp_at(first + i);
            const int y = list.y + i * 2;
            if (first + i == s_sel) ls_fill_dither(sf, tui_rect_make(list.x, y, list.w, 2), LS_DITHER_LIGHT, TUI_CYAN);
            char badge[10];
            snprintf(badge, sizeof(badge), "%-6s", ls_exp_maturity_name(e->maturity));
            tui_put_str(sf, list, list.x, y, badge, badge_attr(e->maturity));
            tui_put_str(sf, list, list.x + 7, y, e->name, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
            if (e == run) {
                char on[8];
                snprintf(on, sizeof(on), "%c ON", ls_motion_pip(true));
                tui_put_str(sf, list, list.x + list.w - 4, y, on, TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
            }
            if (e->lr2021_only && list.w > 40)
                tui_put_str(sf, list, list.x + list.w - 17, y, "LR2021 only", LS_ATTR_DIM);
            tui_put_str(sf, list, list.x + 7, y + 1, e->sub ? e->sub : "", LS_ATTR_DIM);
        }
    }
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback[0] ? s_feedback : "UP/DOWN pick, ENTER opens", LS_ATTR_DIM);
}

/* ------------------------------------------------------------------ page */

static float graph_value(const sh_reading_t *r, bool fahrenheit)
{
    float v = s_graph == 0 ? r->temp_c : s_graph == 1 ? r->humidity :
        s_graph == 2 ? r->rain_mm : s_graph == 3 ? r->wind_ms : r->kpa;
    return s_graph == 0 && fahrenheit ? v * 1.8f + 32 : v;
}

static void draw_history(tui_surface *sf, tui_rect a)
{
    const bool fahrenheit = lr433_history_fahrenheit();
    bool detail = lr433_history_copy(s_sensor_detail, &s_sensor);
    if (!detail) s_sensor_detail = -1;
    ls_btn_t btn[4] = { { "BACK", detail ? "SENSORS" : "LR433", 'b' },
        { "OPTIONS", detail ? "SENSOR" : "HISTORY", 'o' },
        { "NAME", "SENSOR", 'n' }, { "GRAPH", "FIELD", 'g' } };
    page_bar(sf, &a, btn, detail ? 4 : 2);
    tui_rect box = tui_rect_make(a.x, a.y, a.w, a.h - 1);
    ls_panel_box(sf, box, detail && s_sensor.name[0] ? s_sensor.name : "SENSORS / RX ONLY", TUI_CYAN);
    ls_motion_busy(sf, box, ls_exp_running() == opened());
    tui_rect in = tui_rect_make(box.x + 2, box.y + 1, box.w - 4, box.h - 2);
    s_sensor_hit = tui_rect_make(0, 0, 0, 0);
    if (in.w < 4 || in.h < 2) return;
    if (!detail) {
        s_sensor_count = lr433_history_list(s_sensor_order);
        if (s_sensor_sel >= s_sensor_count) s_sensor_sel = s_sensor_count ? s_sensor_count - 1 : 0;
        int rows = in.h / 2;
        s_sensor_first = s_sensor_sel / rows * rows;
        s_sensor_hit = in;
        for (int j = 0; j < rows && s_sensor_first + j < s_sensor_count; j++) {
            int idx = s_sensor_first + j, y = in.y + 2 * j;
            if (!lr433_history_copy(s_sensor_order[idx], &s_sensor)) continue;
            if (idx == s_sensor_sel) ls_fill_dither(sf, tui_rect_make(in.x, y, in.w, 2), LS_DITHER_LIGHT, TUI_CYAN);
            char line[100];
            snprintf(line, sizeof(line), "%s / %s ch %d", s_sensor.name[0] ? s_sensor.name : s_sensor.id,
                lr433_proto_label(s_sensor.proto), s_sensor.channel);
            tui_put_str(sf, in, in.x, y, line, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
            long long age = lr433_history_now() - s_sensor.last_seen;
            if (age < 0) age = 0;
            if (lr433_proto_class(s_sensor.proto) == LR433_TPMS && !s_sensor.own)
                snprintf(line, sizeof(line), "MY TPMS ID off / %lldm ago", age / 60);
            else {
                bool f = lr433_history_fahrenheit();
                char value[48];
                float t = s_sensor.last.temp_c;
                if (isfinite(s_sensor.last.kpa)) snprintf(value, sizeof(value), "%.1f kPa", (double)s_sensor.last.kpa);
                else if (isfinite(t)) snprintf(value, sizeof(value), "%.1f%c", (double)(f ? t * 1.8f + 32 : t), f ? 'F' : 'C');
                else if (isfinite(s_sensor.last.wind_ms)) snprintf(value, sizeof(value), "%.1f m/s", (double)s_sensor.last.wind_ms);
                else if (isfinite(s_sensor.last.rain_mm)) snprintf(value, sizeof(value), "%.1f mm", (double)s_sensor.last.rain_mm);
                else if (isfinite(s_sensor.last.humidity)) snprintf(value, sizeof(value), "%.0f%%", (double)s_sensor.last.humidity);
                else snprintf(value, sizeof(value), "--");
                snprintf(line, sizeof(line), "%s / %lld%s ago%s", value, age < 60 ? age : age / 60,
                    age < 60 ? "s" : "m", s_sensor.last.battery_ok == 0 ? " BAT LOW" : "");
            }
            tui_put_str(sf, in, in.x, y + 1, line, LS_ATTR_DIM);
        }
        if (!s_sensor_count) tui_put_str(sf, in, in.x, in.y, ls_exp_running() == opened() ? "Listening / no sensors heard yet" : "No sensors heard / START LR433", LS_ATTR_DIM);
    } else {
        static const char *const field[] = { "TEMPERATURE", "HUMIDITY %", "RAIN mm", "WIND m/s", "PRESSURE kPa" };
        char line[100];
        snprintf(line, sizeof(line), "%s / %s ch %d", lr433_proto_label(s_sensor.proto), s_sensor.id, s_sensor.channel);
        tui_put_str(sf, in, in.x, in.y, line, LS_ATTR_DIM);
        float lo = INFINITY, hi = -INFINITY, first = NAN, last = NAN;
        for (unsigned i = 0; i < s_sensor.count; i++) {
            float v = graph_value(sh_at(&s_sensor, i), fahrenheit);
            if (!isfinite(v)) continue;
            if (isnan(first)) first = v;
            last = v; if (v < lo) lo = v; if (v > hi) hi = v;
        }
        snprintf(line, sizeof(line), "%s%s / %u points", field[s_graph], s_graph == 0 ? lr433_history_fahrenheit() ? " F" : " C" : "", s_sensor.count);
        tui_put_str(sf, in, in.x, in.y + 1, line, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        if (isfinite(last)) {
            snprintf(line, sizeof(line), "MIN %.1f MAX %.1f TREND %+.1f", (double)lo, (double)hi, (double)(last - first));
            tui_put_str(sf, in, in.x, in.y + 2, line, LS_ATTR_DIM);
            tui_rect graph = tui_rect_make(in.x, in.y + 4, in.w, in.h - 5);
            if (graph.h > 0) {
                ls_fill_dither(sf, graph, LS_DITHER_LIGHT, TUI_CYAN);
                unsigned count = s_sensor.count;
                for (int x = 0; x < graph.w; x++) {
                    /* Bucket all retained samples, widening sparse histories
                       to the pane without inventing intermediate readings. */
                    unsigned start = (unsigned)x * count / graph.w;
                    unsigned end = (unsigned)(x + 1) * count / graph.w;
                    if (end <= start) end = start + 1;
                    float v = 0; unsigned valid = 0;
                    for (unsigned i = start; i < end && i < count; i++) {
                        float sample = graph_value(sh_at(&s_sensor, i), fahrenheit);
                        if (isfinite(sample)) { v += sample; valid++; }
                    }
                    if (!valid) continue;
                    v /= valid;
                    int h = 1 + (hi > lo ? (int)((v - lo) / (hi - lo) * (graph.h - 1)) : (graph.h - 1) / 2);
                    for (int y = 0; y < h; y++) tui_put_char(sf, graph, graph.x + x, graph.y + graph.h - 1 - y,
                        y == h - 1 && x == graph.w - 1 ? ls_motion_pip(true) : '|', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
                }
                tui_put_str(sf, in, in.x, in.y + in.h - 1, "OLDER < samples > NEWER", LS_ATTR_DIM);
            }
        } else tui_put_str(sf, in, in.x, in.y + 2, "No readings for this field", LS_ATTR_DIM);
        snprintf(line, sizeof(line), "%s / %.1f%c%s", s_sensor.alert_on ? "ALERT ABOVE" : "ALERT OFF",
            (double)(lr433_history_fahrenheit() ? s_sensor.threshold_c * 1.8f + 32 : s_sensor.threshold_c),
            lr433_history_fahrenheit() ? 'F' : 'C', s_sensor.alarm ? " / ACTIVE" : "");
        if (in.h > 3) tui_put_str(sf, in, in.x, in.y + 3, line, LS_ATTR_DIM);
    }
    ls_safe_line(sf, a, a.y + a.h - 1, lr433_history_status(), LS_ATTR_DIM);
}

/* Retained flights share the experiment panels, selection dither and glyphs. */
static void sonde_range(char *out, size_t cap, const rs41_sonde_t *s, int64_t now)
{
    ls_gps_get(&s_sonde_gps);
    if (!s->fix_us) { snprintf(out,cap,"waiting for GPS position"); return; }
    if (!s_sonde_gps.fix || !s_sonde_gps.last_fix_us || now < s_sonde_gps.last_fix_us ||
        now-s_sonde_gps.last_fix_us > 10000000 || !isfinite(s_sonde_gps.lat_deg) || !isfinite(s_sonde_gps.lon_deg)) {
        snprintf(out,cap,"range needs fresh receiver GPS"); return;
    }
    double bearing, metres;
    ls_geo_bearing_range(s_sonde_gps.lat_deg,s_sonde_gps.lon_deg,s->report.lat,s->report.lon,&bearing,&metres);
    snprintf(out,cap,"%.2f km / %03.0f %s",metres/1000,bearing,ls_geo_compass(bearing));
}
/* A list line. The picked entry is one solid band, every line padded to the
   full width: text drawn over a dither punches black boxes in it, so the
   fill appeared to stop where each line's text ended. */
static void sonde_line(tui_surface *sf, tui_rect in, int y, const char *text, bool pick, uint8_t attr, uint8_t pick_attr)
{
    tui_put_str(sf, in, in.x, y, text, pick ? pick_attr : attr);
    if (!pick) return;
    for (int x = in.x + (int)strlen(text); x < in.x + in.w; ++x)
        tui_put_char(sf, in, x, y, ' ', pick_attr);
}

/* A flight's altitude against time, drawn the way the SUB-GHZ analyser draws
   a band: columns tiling the whole width, filled to the reading, coloured by
   the shared look, partial top cells in the BARS grain. Altitude is read from
   0, and the columns between samples are interpolated so a handful of frames
   reads as a profile instead of a handful of stems. */
static void draw_profile(tui_surface *sf, tui_rect g, const rs41_sonde_t *s, bool live)
{
    if (g.h < 3 || g.w < 8 || !s->count) return;
    ls_scan_look_load();
    const int rows = g.h, eighth_rows = rows * 8;
    float top = 1;
    unsigned peak = 0;
    for (unsigned i = 0; i < s->count; ++i)
        if (s->track[i].alt > s->track[peak].alt) peak = i;
    if (s->track[peak].alt > top) top = s->track[peak].alt;
    const int64_t first = s->track[0].us, span = s->track[s->count-1].us - first;
    const float W = LS_SCAN_WINDOW_DB;

    /* Quarter lines under the columns, labelled afterwards. */
    for (int q = 1; q < 4; ++q) {
        const int y = g.y + rows - 1 - (rows * q + 2) / 4;
        for (int x = 0; x < g.w; x += 2)
            tui_put_char(sf, g, g.x + x, y, '-', LS_ATTR_FAINT);
    }
    int peak_x = g.w - 1, last_h = 0;
    unsigned j = 0;
    for (int x = 0; x < g.w; ++x) {
        float alt;
        if (s->count == 1 || span <= 0) {
            if (x != g.w - 1) continue;
            alt = s->track[s->count-1].alt;
        } else {
            const int64_t t = first + span * x / (g.w - 1);
            while (j + 2 < s->count && s->track[j+1].us < t) ++j;
            const int64_t t0 = s->track[j].us, t1 = s->track[j+1].us;
            float f = t1 > t0 ? (float)(t - t0) / (float)(t1 - t0) : 1.0f;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            alt = s->track[j].alt + (s->track[j+1].alt - s->track[j].alt) * f;
        }
        float v = alt / top;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        const int he = (int)(v * eighth_rows + 0.5f), h = (he + 7) / 8;
        const float over = v * W;
        const uint8_t hue = ls_scan_colour(over, W);
        const int level = ls_scan_level(over, W);
        for (int y = 0; y < rows; ++y) {
            const int fb = rows - 1 - y;
            char c;
            uint8_t attr;
            if (fb < h - 1) { c = ls_scan_glyph(level, 8); attr = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK); }
            else if (fb < h) {
                const int part = he - fb * 8;
                c = ls_scan_glyph(level, part < 1 ? 1 : part);
                attr = TUI_ATTR(hue, TUI_BLACK);
            } else if (fb == 0) { c = LS_TUI_SHADE_25; attr = TUI_ATTR(TUI_CYAN, TUI_BLACK); }
            else continue;
            tui_put_char(sf, g, g.x + x, g.y + y, c, attr);
        }
        if (s->count > 1 && span > 0 &&
            x == (int)((double)(s->track[peak].us - first) / span * (g.w - 1) + 0.5))
            peak_x = x;
        if (x == g.w - 1) last_h = h;
    }
    /* The newest sample, as the pip the other live panels use. */
    if (last_h > 0)
        tui_put_char(sf, g, g.x + g.w - 1, g.y + rows - last_h, ls_motion_pip(live),
                     TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    /* Scale on the quarter lines. */
    for (int q = 1; q < 4; ++q) {
        char lab[12];
        const float a = top * (float)q / 4;
        if (a >= 1000) snprintf(lab, sizeof(lab), "%.1fk", (double)(a / 1000));
        else snprintf(lab, sizeof(lab), "%.0f", (double)a);
        tui_put_str(sf, g, g.x, g.y + rows - 1 - (rows * q + 2) / 4, lab, LS_ATTR_DIM);
    }
    /* The highest point, named beside its column, to the left if it would
       run off the pane. */
    char pk[16];
    snprintf(pk, sizeof(pk), "%.0f m", (double)s->track[peak].alt);
    const int pw = (int)strlen(pk);
    int tx = g.x + peak_x + 2;
    if (tx + pw > g.x + g.w) tx = g.x + peak_x - pw - 1;
    if (tx < g.x) tx = g.x;
    tui_put_str(sf, g, tx, g.y, pk, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

/* What an empty list says: START only when nothing is listening. */
static const char *ls_exp_sonde_empty_text(bool running)
{
    return running ? "Listening / no flights heard yet" : "No flights heard / START RS41";
}
#define sonde_empty() ls_exp_sonde_empty_text(ls_exp_running() == opened())

static void draw_sondes(tui_surface *sf, tui_rect a)
{
    int64_t now = esp_timer_get_time();
    bool detail = rs41_store_copy(s_sonde_detail,&s_sonde,now);
    if (!detail) s_sonde_detail = -1;
    ls_btn_t btn[] = { { "BACK",detail ? "SONDES" : "RS41",'b' },
        { "OPTIONS","RS41",'o' }, { "RECOVER","COMPASS",'g',false,!detail || !s_sonde.fix_us } };
    page_bar(sf,&a,btn,detail ? 3 : 2);
    tui_rect box = tui_rect_make(a.x,a.y,a.w,a.h-1);
    ls_panel_box(sf,box,detail ? s_sonde.report.serial : "SONDES / RX ONLY",TUI_CYAN);
    ls_motion_busy(sf,box,ls_exp_running() == opened());
    tui_rect in = tui_rect_make(box.x+2,box.y+1,box.w-4,box.h-2);
    s_sonde_hit = tui_rect_make(0,0,0,0);
    if (in.w < 4 || in.h < 3) return;
    char line[100], range[64];
    if (!detail) {
        s_sonde_count = 0;
        for (int i = 0; i < RS41_SONDES; ++i) if (rs41_store_copy(i,&s_sonde,now)) s_sonde_order[s_sonde_count++] = (uint8_t)i;
        if (s_sonde_sel >= s_sonde_count) s_sonde_sel = s_sonde_count ? s_sonde_count-1 : 0;
        int rows = in.h/3; s_sonde_first = s_sonde_sel/rows*rows; s_sonde_hit = in;
        for (int j = 0; j < rows && s_sonde_first+j < s_sonde_count; ++j) {
            int idx = s_sonde_first+j, y = in.y+3*j;
            if (!rs41_store_copy(s_sonde_order[idx],&s_sonde,now)) continue;
            const bool pick = idx == s_sonde_sel;
            snprintf(line,sizeof(line),"%s / frame %u / %llds ago",s_sonde.report.serial,s_sonde.report.frame,(long long)((now-s_sonde.heard_us)/1000000));
            sonde_line(sf,in,y,line,pick,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK),TUI_ATTR(TUI_BLACK,TUI_CYAN|TUI_BRIGHT));
            if (s_sonde.fix_us) snprintf(line,sizeof(line),"ALT %.0f m / CLIMB %+.1f m/s / fix %llds",s_sonde.report.alt,(double)s_sonde.report.climb,(long long)((now-s_sonde.fix_us)/1000000));
            else snprintf(line,sizeof(line),"ALT -- / CLIMB --");
            sonde_line(sf,in,y+1,line,pick,LS_ATTR_DIM,TUI_ATTR(TUI_BLACK,TUI_CYAN));
            sonde_range(range,sizeof(range),&s_sonde,now);
            sonde_line(sf,in,y+2,range,pick,LS_ATTR_DIM,TUI_ATTR(TUI_BLACK,TUI_CYAN));
        }
        if (!s_sonde_count) tui_put_str(sf,in,in.x,in.y,sonde_empty(),LS_ATTR_DIM);
    } else {
        sonde_range(range,sizeof(range),&s_sonde,now);
        tui_put_str(sf,in,in.x,in.y,range,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        snprintf(line,sizeof(line),"FIX %llds ago / BATT %.1f V / #%u",s_sonde.fix_us ? (long long)((now-s_sonde.fix_us)/1000000) : -1,(double)s_sonde.report.battery,s_sonde.report.frame);
        tui_put_str(sf,in,in.x,in.y+1,line,LS_ATTR_DIM);
        if (s_sonde.fix_us) snprintf(line,sizeof(line),"%.5f %.5f / %.0f m / %+.1f m/s",s_sonde.report.lat,s_sonde.report.lon,s_sonde.report.alt,(double)s_sonde.report.climb);
        else snprintf(line,sizeof(line),"Position pending");
        tui_put_str(sf,in,in.x,in.y+2,line,LS_ATTR_DIM);
        float lo = INFINITY, hi = -INFINITY;
        for (unsigned i = 0; i < s_sonde.count; ++i) { float v = s_sonde.track[i].alt; if (v < lo) lo = v; if (v > hi) hi = v; }
        if (s_sonde.count && in.h > 6) {
            snprintf(line,sizeof(line),"ALTITUDE m / %.0f..%.0f / %u points",(double)lo,(double)hi,s_sonde.count);
            tui_put_str(sf,in,in.x,in.y+3,line,LS_ATTR_DIM);
            tui_rect graph = tui_rect_make(in.x,in.y+4,in.w,in.h-5);
            draw_profile(sf,graph,&s_sonde,now-s_sonde.fix_us < 10000000);
            const long long dur = s_sonde.count > 1 ? (long long)((s_sonde.track[s_sonde.count-1].us-s_sonde.track[0].us)/1000000) : 0;
            tui_put_str(sf,in,in.x,in.y+in.h-1,"OLDER",LS_ATTR_DIM);
            snprintf(line,sizeof(line),"%lldm%02llds / last fix NEWER",dur/60,dur%60);
            if ((int)strlen(line)+8 < in.w) tui_put_str(sf,in,in.x+in.w-(int)strlen(line),in.y+in.h-1,line,LS_ATTR_DIM);
        }
    }
    ls_safe_line(sf,a,a.y+a.h-1,s_feedback[0] ? s_feedback : detail ? "G recover in COMPASS / ESC list" : "ENTER detail / G recover in COMPASS",LS_ATTR_DIM);
}

static void draw_page(tui_surface *sf, tui_rect a, const ls_experiment_t *e)
{
    if (s_history && is_rs41(e)) { draw_sondes(sf,a); return; }
    if (s_history && is_lr433(e)) { draw_history(sf, a); return; }
    const bool running = ls_exp_running() == e;
    const bool live = is_running_or_starting(e);
    const ls_opt_ctx_t *ctx = options(e);
    ls_btn_t btn[6];
    int n = 0;
    btn[n++] = (ls_btn_t){ "BACK", "LIST", 'b', false, false };
    btn[n++] = (ls_btn_t){ live ? "STOP" : "START", running ? "RUNNING" : live ? "WAIT" : "STOPPED", 's', live, false };
    if (!e->no_radio) {
        btn[n] = ls_rsel_button(LS_RSEL_EXPERIMENT);
        btn[n++].dim = live;
    }
    if (ls_opt_count(ctx)) btn[n++] = ls_opt_button(ctx);
    btn[n++] = (ls_btn_t){ "NOTE", "SAVE", 'n', false, false };
    if (is_rs41(e)) btn[n++] = (ls_btn_t){ "SONDES", "FLIGHTS", 'h', false, false };
    if (is_lr433(e)) btn[n++] = (ls_btn_t){ "SENSORS", "HISTORY", 'h', false, false };
    note_progress();
    page_bar(sf, &a, btn, n);

    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - 1);
    char title[48];
    snprintf(title, sizeof(title), "%s / %s", e->name, ls_exp_maturity_name(e->maturity));
    ls_panel_box(sf, body, title, TUI_CYAN);
    ls_motion_busy(sf, body, running);
    tui_rect in = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    if (in.w < 4 || in.h < 2) return;

    int y = in.y;
    const int last = in.y + in.h - 1;    /* the status line's row */
    y += wrap(sf, in, y, last - y > 3 ? 2 : 1, e->sub ? e->sub : "", LS_ATTR_DIM);
    if (e->needs && y < last - 1) {
        char needs[160];
        snprintf(needs, sizeof(needs), "NEEDS %s", e->needs);
        y += wrap(sf, in, y, 2, needs, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    if (y < last - 1) y++;

    int room = last - y - 1;
    if (room > LIVE_MAX) room = LIVE_MAX;
    const int got = ls_exp_read_lines(e, s_live, room);
    const uint8_t ink = running ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM;
    for (int i = 0; i < got; i++) tui_put_str(sf, in, in.x, y + i, s_live[i], ink);

    char state[80];
    ls_exp_state_line(e, state, sizeof(state));
    const bool failed = !live && strcmp(state, "stopped") != 0;
    const uint8_t sattr = running ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                        : failed  ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                                  : LS_ATTR_DIM;
    char status[96];
    snprintf(status, sizeof(status), "%c %s", ls_motion_pip(running), state);
    tui_put_str(sf, in, in.x, last, status, sattr);

    const char *keys = e->no_radio ? "S start/stop  O options  N note  ESC back" : "S start/stop  R radio  O options  N note  ESC back";
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback[0] ? s_feedback : ls_tui_keyboard_mode() ? keys : "", LS_ATTR_DIM);
}

/* ------------------------------------------------------------ the screen */

static void enter(void)
{
    ls_exp_register_builtin();
    ls_notify_add_probe(sensor_probe);
    ls_exp_hw_wake();
    s_history = false;
    s_focus = -1;
    s_slot = 0;
    s_feedback[0] = 0;
    s_note_wait = false;
    ls_notes_start();
    /* Coming back to a running experiment lands on its page. */
    const ls_experiment_t *run = ls_exp_running();
    s_open = -1;
    for (int i = 0; run && i < ls_exp_count(); i++)
        if (ls_exp_at(i) == run) s_open = i;
}

static void draw(tui_surface *sf, tui_rect a)
{
    const ls_experiment_t *e = opened();
    snprintf(s_hint, sizeof(s_hint), "%s", e ? "S start/stop  R radio  O options  N note  ESC back"
                                             : "ENTER open  X stop  N note  ESC home");
    if (s_history) snprintf(s_hint, sizeof(s_hint), is_rs41(e) ? (s_sonde_detail >= 0 ? "G recover  O options  ESC flights" : "ENTER detail  O options  G recover  ESC back") : "ENTER detail  O options  N name  G graph  ESC back");
    if (a.w < 24 || a.h < 12) {
        s_bar_n = 0;
        s_rows_hit = tui_rect_make(0, 0, 0, 0);
        ls_panel_notice(sf, a, "EXPERIMENTS", "Enlarge the pane", "A running one keeps going");
        return;
    }
    if (e) draw_page(sf, a, e);
    else draw_list(sf, a);
}

static bool key(ls_tk_t k, char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    const ls_experiment_t *e = opened();
    if (s_history && is_rs41(e)) {
        if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { action('b'); return true; }
        if (s_sonde_detail < 0 && (k == LS_TK_UP || k == LS_TK_DOWN)) {
            s_focus = -1;
            if (k == LS_TK_UP && s_sonde_sel > 0) --s_sonde_sel;
            if (k == LS_TK_DOWN && s_sonde_sel+1 < s_sonde_count) ++s_sonde_sel;
            return true;
        }
        if (k == LS_TK_ENTER && s_focus < 0) {
            if (s_sonde_detail < 0 && s_sonde_sel < s_sonde_count) s_sonde_detail = s_sonde_order[s_sonde_sel];
            return true;
        }
        if (k == LS_TK_CHAR && ch && strchr("bog",ch)) { action(ch); return true; }
        if (k != LS_TK_TAB && k != LS_TK_LEFT && k != LS_TK_RIGHT && !(k == LS_TK_ENTER && s_focus >= 0)) return false;
    }
    if (s_history && is_lr433(e)) {
        if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { action('b'); return true; }
        if (s_sensor_detail < 0 && (k == LS_TK_UP || k == LS_TK_DOWN)) {
            s_focus = -1;
            if (k == LS_TK_UP && s_sensor_sel > 0) s_sensor_sel--;
            if (k == LS_TK_DOWN && s_sensor_sel + 1 < s_sensor_count) s_sensor_sel++;
            return true;
        }
        if (k == LS_TK_ENTER && s_focus < 0 && s_sensor_detail < 0 && s_sensor_sel < s_sensor_count) {
            s_sensor_detail = s_sensor_order[s_sensor_sel]; return true;
        }
        if (k == LS_TK_CHAR && ch && strchr("bong", ch)) { action(ch); return true; }
        if (k != LS_TK_TAB && k != LS_TK_LEFT && k != LS_TK_RIGHT &&
            !(k == LS_TK_ENTER && s_focus >= 0)) return false;
    }
    if (k == LS_TK_TAB || k == LS_TK_LEFT || k == LS_TK_RIGHT)
        return ls_btn_navigate(k, &s_slot, &s_focus, false);
    if (k == LS_TK_ENTER && s_focus >= 0 && s_focus < s_bar_n) {
        action(s_bar_keys[s_focus]);
        return true;
    }
    if (e) {
        if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { close_page(); return true; }
        if (k == LS_TK_ENTER) { action('s'); return true; }
        if (k != LS_TK_CHAR || !ch) return false;
        if (ls_opt_key(options(e), ch)) return true;
        if (strchr("bsrnh", ch)) { action(ch); return true; }
        return false;
    }
    const int count = ls_exp_count();
    if (k == LS_TK_UP || k == LS_TK_DOWN) s_focus = -1;
    if (k == LS_TK_UP)   { if (s_sel > 0) s_sel--; return true; }
    if (k == LS_TK_DOWN) { if (s_sel + 1 < count) s_sel++; return true; }
    if (k == LS_TK_ENTER) { open_page(s_sel); return true; }
    if (k != LS_TK_CHAR || !ch) return false;
    if (ch == 'b' || ch == 'x' || ch == 'n') { action(ch); return true; }
    return false;
}

static bool touch(int x, int y)
{
    const int i = ls_btn_hit(x, y);
    if (i >= 0 && i < s_bar_n) { action(s_bar_keys[i]); return true; }
    if (s_history && is_rs41(opened()) && s_sonde_detail < 0 && tui_rect_contains(s_sonde_hit,x,y)) {
        int row = s_sonde_first+(y-s_sonde_hit.y)/3;
        if (row < s_sonde_count) {
            if (row == s_sonde_sel) s_sonde_detail = s_sonde_order[row]; else s_sonde_sel = row;
            s_focus = -1;
        }
        return true;
    }
    if (s_history && is_lr433(opened()) && s_sensor_detail < 0 && tui_rect_contains(s_sensor_hit, x, y)) {
        int row = s_sensor_first + (y - s_sensor_hit.y) / 2;
        if (row < s_sensor_count) {
            if (row == s_sensor_sel) s_sensor_detail = s_sensor_order[row];
            else s_sensor_sel = row;
            s_focus = -1;
        }
        return true;
    }
    if (s_open < 0 && s_rows_hit.w > 0 && tui_rect_contains(s_rows_hit, x, y)) {
        const int row = s_rows_first + (y - s_rows_hit.y) / 2;
        /* First tap selects, a tap on the selected one opens it. */
        if (row < ls_exp_count()) {
            if (row == s_sel) open_page(row);
            else s_sel = row;
        }
    }
    return true;
}

const ls_tui_screen_t ls_scr_experiments = {
    .name = "EXPERIMENTS", .hint = s_hint, .enter = enter, .draw = draw, .key = key, .touch = touch,
};
