/* DRONES: early warning for drone control and video links, receive only.
 *
 * Four pages under one bar. WATCH lists every link the watch hears, strongest
 * and closing first. BANDS draws the 900 MHz, 2.4 GHz and 5.8 GHz lanes with
 * the scan position each source last actually measured. REMOTE ID is the
 * existing ASTM Remote ID radar/list/detail. FIND turns one link into a big
 * strength readout to walk toward or away from.
 *
 * Nothing here moves on a timer. The rotors turn one step per real
 * measurement, cursors sit where the radio last measured, bars and
 * sparklines are the watch's own RSSI history. */
#include "ls_drone_watch.h"
#include "ls_rid_ui.h"
#include "ls_rid.h"
#include "ls_options.h"
#include "ls_compass_art.h"
#include "ls_i18n.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"

#define INK(c)  TUI_ATTR((c), TUI_BLACK)
#define BRIGHT(c) INK((c) | TUI_BRIGHT)

enum { PG_WATCH, PG_BANDS, PG_RID, PG_FIND, PG_N };

/* PSRAM .bss is zeroed at boot: no initializers on these. */
static EXT_RAM_BSS_ATTR ls_drone_watch_entry_t s_view[LS_DW_ENTRIES];
static EXT_RAM_BSS_ATTR ls_dot_t s_dots[112 * 66];
static int s_count, s_order[LS_DW_ENTRIES];
static int s_page, s_pick, s_rows = 1;
static uint64_t s_pick_id;
static int64_t s_refreshed;
static ls_drone_watch_status_t s_status;
static ls_dw_cursor_t s_cur[LS_DW_SOURCES];
static int64_t s_cur_seen[LS_DW_SOURCES];
static unsigned s_rotor;            /* advances once per real measurement */
static bool s_session_on;           /* watch switched on by opening the app */
static bool s_background;           /* keep it on after leaving */
static tui_rect s_list;

static const char *label(ls_dw_source_t s)
{
    static const char *const L[] = { "ELRS 900", "ELRS 2.4", "900 HOPPER",
                                     "2.4 WIDEBAND", "5.8 ANALOG FPV", "REMOTE ID" };
    return (unsigned)s < LS_DW_SOURCES ? ls_tr(L[s]) : "?";
}
static uint8_t conf_hue(ls_dw_confidence_t c)
{
    return c == LS_DW_CONFIRMED ? TUI_MAGENTA : c == LS_DW_LIKELY ? TUI_RED : TUI_YELLOW;
}
static const char *conf_word(ls_dw_confidence_t c)
{
    return ls_tr(c == LS_DW_CONFIRMED ? "CONFIRMED" : c == LS_DW_LIKELY ? "LIKELY" : "POSSIBLE");
}

/* ----------------------------------------------------------- data */

static bool closer(int a, int b)
{
    const ls_drone_watch_entry_t *x = &s_view[a], *y = &s_view[b];
    const bool ax = x->trend == LS_DW_APPROACHING, ay = y->trend == LS_DW_APPROACHING;
    if (ax != ay) return ax;
    return x->rssi_dbm > y->rssi_dbm;
}

static void refresh(int64_t now)
{
    ls_dw_cursor_t cur[LS_DW_SOURCES];
    ls_drone_watch_cursor(cur);
    for (int i = 0; i < LS_DW_SOURCES; i++) {
        if (cur[i].us && cur[i].us != s_cur[i].us) { s_rotor++; s_cur_seen[i] = now; }
        s_cur[i] = cur[i];
    }
    if (now - s_refreshed < 250000) return;
    s_refreshed = now;
    size_t n = ls_drone_watch_snapshot(s_view, LS_DW_ENTRIES);
    if (n != (size_t)-1) s_count = (int)(n > LS_DW_ENTRIES ? LS_DW_ENTRIES : n);
    ls_drone_watch_status_t st;
    if (ls_drone_watch_status(&st)) s_status = st;
    for (int i = 0; i < s_count; i++) s_order[i] = i;
    for (int i = 1; i < s_count; i++) {
        int v = s_order[i], j = i;
        while (j > 0 && closer(v, s_order[j - 1])) { s_order[j] = s_order[j - 1]; j--; }
        s_order[j] = v;
    }
    s_pick = 0;
    for (int i = 0; i < s_count; i++) if (s_view[s_order[i]].id == s_pick_id) { s_pick = i; break; }
    if (s_count) s_pick_id = s_view[s_order[s_pick]].id;
}

static const ls_drone_watch_entry_t *picked(void)
{
    return s_count ? &s_view[s_order[s_pick]] : NULL;
}

static int running_sources(void)
{
    int n = 0;
    for (int i = 0; i < LS_DW_SOURCES; i++) n += s_status.running[i];
    return n;
}
static int approaching(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) n += s_view[i].trend == LS_DW_APPROACHING;
    return n;
}

/* ---------------------------------------------------------- drawing */

static void put(tui_surface *sf, tui_rect a, int x, int y, const char *s, uint8_t ink)
{
    tui_put_str(sf, a, x, y, s, ink);
}

/* Quad art. Rotors step through - \ | / once per real measurement; the
   eye is red while any link is heard. 17 columns by 5 rows. */
static void art(tui_surface *sf, tui_rect a, int x, int y, bool compact)
{
    static const char spin[] = "-\\|/";
    const char r = spin[s_rotor & 3], r2 = spin[(s_rotor + 2) & 3];
    const uint8_t rotor = s_status.enabled ? BRIGHT(TUI_CYAN) : LS_ATTR_FAINT;
    const uint8_t arm = INK(TUI_WHITE), body = BRIGHT(TUI_YELLOW);
    const uint8_t eye = s_count ? BRIGHT(TUI_RED) : BRIGHT(TUI_GREEN);
    char rp[4] = { '(', r, ')', 0 }, rq[4] = { '(', r2, ')', 0 };
    if (compact) {   /* landscape: three rows */
        put(sf, a, x, y, rp, rotor); put(sf, a, x + 3, y, "\\_____/", arm); put(sf, a, x + 10, y, rq, rotor);
        put(sf, a, x + 3, y + 1, " [", arm); put(sf, a, x + 5, y + 1, "=", body);
        tui_put_char(sf, a, x + 6, y + 1, 'O', eye); put(sf, a, x + 7, y + 1, "=", body);
        put(sf, a, x + 8, y + 1, "] ", arm);
        put(sf, a, x, y + 2, rq, rotor); put(sf, a, x + 3, y + 2, "/     \\", arm); put(sf, a, x + 10, y + 2, rp, rotor);
        return;
    }
    put(sf, a, x, y, rp, rotor);              put(sf, a, x + 14, y, rq, rotor);
    put(sf, a, x + 2, y + 1, "\\\\  ___  //", arm);
    put(sf, a, x + 3, y + 2, "[=|", arm);
    put(sf, a, x + 6, y + 2, "_", body); tui_put_char(sf, a, x + 7, y + 2, 'O', eye);
    put(sf, a, x + 8, y + 2, "_", body); put(sf, a, x + 9, y + 2, "|=]", arm);
    put(sf, a, x + 2, y + 3, "//       \\\\", arm);
    put(sf, a, x, y + 4, rq, rotor);          put(sf, a, x + 14, y + 4, rp, rotor);
}

static const char *scanning_now(int64_t now)
{
    int best = -1;
    for (int i = 0; i < LS_DW_SOURCES; i++)
        if (s_cur[i].us && now - s_cur_seen[i] < 3000000 && (best < 0 || s_cur_seen[i] > s_cur_seen[best])) best = i;
    static char out[40];
    if (best < 0) return s_status.enabled ? ls_tr("waiting for a radio") : "";
    snprintf(out, sizeof(out), "%s %lu %s", label((ls_dw_source_t)best), (unsigned long)((s_cur[best].lo_mhz + s_cur[best].hi_mhz) / 2), ls_tr("MHz"));
    return out;
}

static int header(tui_surface *sf, tui_rect a, int64_t now)
{
    const bool wide = ls_tui_is_wide();
    art(sf, a, a.x + 1, a.y, wide);
    const int tx = a.x + (wide ? 15 : 20);
    char s[96];
    const char *title = ls_tr("DRONE WATCH");
    put(sf, a, tx, a.y, title, BRIGHT(TUI_WHITE));
    put(sf, a, tx + (int)strlen(title) + 1, a.y, ls_tr(s_status.enabled ? "ON" : "OFF"),
        s_status.enabled ? BRIGHT(TUI_GREEN) : BRIGHT(TUI_RED));
    if (s_status.enabled) {
        snprintf(s, sizeof(s), ls_tr("%d/%d sources  cycle %.1fs"), running_sources(), (int)LS_DW_SOURCES,
                 s_status.cycle_ms / 1000.0);
        put(sf, a, tx, a.y + 1, s, LS_ATTR_DIM);
        put(sf, a, tx, a.y + 2, scanning_now(now), INK(TUI_CYAN));
    } else {
        put(sf, a, tx, a.y + 1, ls_tr("receive only, nothing sent"), LS_ATTR_DIM);
        put(sf, a, tx, a.y + 2, ls_tr("W or OPTIONS to start"), INK(TUI_YELLOW));
    }
    if (!wide) {
        if (s_count == 1) snprintf(s, sizeof(s), "%s", ls_tr("1 link heard"));
        else snprintf(s, sizeof(s), ls_tr("%d links heard"), s_count);
        put(sf, a, tx, a.y + 3, s, s_count ? BRIGHT(TUI_RED) : INK(TUI_GREEN));
        const int ap = approaching();
        if (ap) { snprintf(s, sizeof(s), ls_tr("%d CLOSING"), ap); put(sf, a, tx, a.y + 4, s, BRIGHT(TUI_RED)); }
        else put(sf, a, tx, a.y + 4, ls_tr(s_status.alerts ? "alerts on" : "alerts off"), LS_ATTR_DIM);
        return 6;
    }
    return 4;
}

/* RSSI history as a dot line, -110..-30 dBm over the cell box. */
static void spark(tui_surface *sf, tui_rect box, const ls_drone_watch_entry_t *e, uint8_t hue)
{
    const int w = box.w * 2, h = box.h * 3;
    if (w < 4 || h < 3 || w * h > (int)(sizeof(s_dots) / sizeof(s_dots[0]))) return;
    memset(s_dots, 0, (size_t)w * h * sizeof(s_dots[0]));
    const unsigned n = e->history_count > LS_DW_HISTORY ? LS_DW_HISTORY : e->history_count;
    for (int x = 0; x < w; x++) s_dots[(h - 1) * w + x] = (ls_dot_t){ TUI_BLACK | TUI_BRIGHT, true };
    for (unsigned i = 0; n >= 1 && i < n; i++) {
        const int x = n == 1 ? w - 1 : (int)(i * (w - 1) / (n - 1));
        float v = (e->history[i].dbm + 110.0f) / 80.0f;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        const int top = h - 1 - (int)lroundf(v * (h - 1));
        for (int y = top; y < h; y++)
            s_dots[y * w + x] = (ls_dot_t){ (uint8_t)(y == top ? hue | TUI_BRIGHT : hue), true };
    }
    ls_dots_blit(sf, box, s_dots, w);
}

static void trend_words(const ls_drone_watch_entry_t *e, char *out, size_t n, uint8_t *ink)
{
    if (e->trend == LS_DW_APPROACHING) { snprintf(out, n, ls_tr("<<< CLOSER %+.0fdB"), e->db_per_10s); *ink = BRIGHT(TUI_RED); }
    else if (e->trend == LS_DW_RECEDING) { snprintf(out, n, ls_tr("FARTHER >>> %+.0fdB"), e->db_per_10s); *ink = BRIGHT(TUI_GREEN); }
    else { snprintf(out, n, "%s", ls_tr("steady")); *ink = LS_ATTR_DIM; }
}

static void watch_page(tui_surface *sf, tui_rect a, int64_t now)
{
    ls_panel_box(sf, a, s_count ? "LINKS HEARD / CLOSEST FIRST" : "SKY", s_count ? TUI_RED : TUI_GREEN);
    s_list = a;
    if (!s_count) {
        put(sf, a, a.x + 2, a.y + 2, ls_tr(s_status.enabled ? "SKY QUIET" : "WATCH OFF"),
            s_status.enabled ? BRIGHT(TUI_GREEN) : BRIGHT(TUI_YELLOW));
        put(sf, a, a.x + 2, a.y + 3, ls_tr(s_status.enabled ? "No drone link heard on any band."
                                                           : "Tap WATCH or press W to listen."), LS_ATTR_DIM);
        for (int i = 0; i < LS_DW_SOURCES && 5 + i * 2 < a.h - 1; i++) {
            char s[128];
            const bool on = s_status.running[i];
            tui_put_char(sf, a, a.x + 2, a.y + 5 + i * 2, on ? '*' : '.', on ? BRIGHT(TUI_GREEN) : LS_ATTR_FAINT);
            put(sf, a, a.x + 4, a.y + 5 + i * 2, label((ls_dw_source_t)i), on ? BRIGHT(TUI_CYAN) : INK(TUI_WHITE));
            snprintf(s, sizeof(s), "%.*s", a.w - 8 > 0 ? a.w - 8 : 0, s_status.reason[i][0] ? ls_tr(s_status.reason[i]) : "-");
            put(sf, a, a.x + 6, a.y + 6 + i * 2, s, LS_ATTR_DIM);
        }
        s_rows = 1;
        return;
    }
    const int per = 4;
    s_rows = (a.h - 2) / per;
    if (s_rows < 1) s_rows = 1;
    const int start = s_pick / s_rows * s_rows;
    for (int i = start; i < s_count && i < start + s_rows; i++) {
        const ls_drone_watch_entry_t *e = &s_view[s_order[i]];
        const int y = a.y + 1 + (i - start) * per;
        const uint8_t hue = conf_hue(e->confidence);
        char s[96], t[40];
        uint8_t tink;
        snprintf(s, sizeof(s), "%c%s", i == s_pick ? '>' : ' ', conf_word(e->confidence));
        put(sf, a, a.x + 1, y, s, i == s_pick ? BRIGHT(TUI_WHITE) : BRIGHT(hue));
        const char *where = e->channel[0] ? e->channel : NULL;
        if (where) snprintf(s, sizeof(s), "%s  %s", label(e->source), where);
        else if (e->centre_hz) snprintf(s, sizeof(s), "%s  %.1f %s", label(e->source), e->centre_hz / 1e6, ls_tr("MHz"));
        else snprintf(s, sizeof(s), "%s  %.20s", label(e->source), e->identity[0] ? e->identity : "");
        put(sf, a, a.x + 13, y, s, BRIGHT(TUI_CYAN));
        snprintf(s, sizeof(s), "%.0fs", (now - e->last_us) / 1e6);
        put(sf, a, a.x + a.w - 1 - (int)strlen(s), y, s, LS_ATTR_DIM);
        float v = (e->rssi_dbm + 110.0f) / 80.0f;
        snprintf(s, sizeof(s), e->calibrated ? "%4.0f dBm" : "%4.0f dB", e->rssi_dbm);
        put(sf, a, a.x + 2, y + 1, s, BRIGHT(TUI_WHITE));
        ls_meter_ascii(sf, a, a.x + 12, y + 1, a.w / 3, v < 0 ? 0 : v > 1 ? 1 : v, hue);
        trend_words(e, t, sizeof(t), &tink);
        put(sf, a, a.x + 13 + a.w / 3, y + 1, t, tink);
        spark(sf, tui_rect_make(a.x + 2, y + 2, a.w - 4, 1), e, hue);
    }
}

typedef struct { const char *title; unsigned lo, hi; ls_dw_source_t a, b; uint8_t hue; } lane_t;
static const lane_t LANES[] = {
    { "900 MHz  ELRS / HOPPERS", 860, 930, LS_DW_ELRS900, LS_DW_HOPPER900, TUI_YELLOW },
    { "2.4 GHz  ELRS / WIDEBAND", 2400, 2484, LS_DW_ELRS24, LS_DW_WIDE24, TUI_CYAN },
    { "5.8 GHz  ANALOG FPV", 5645, 5945, LS_DW_ANALOG58, LS_DW_ANALOG58, TUI_MAGENTA },
};

static void lane(tui_surface *sf, tui_rect box, const lane_t *L, int64_t now)
{
    ls_panel_box(sf, box, L->title, L->hue);
    const int status_rows = L->b == L->a ? 1 : 2;
    tui_rect plot = tui_rect_make(box.x + 1, box.y + 1, box.w - 2, box.h - 3 - status_rows);
    const int w = plot.w * 2, h = plot.h * 3;
    if (w < 8 || h < 3 || w * h > (int)(sizeof(s_dots) / sizeof(s_dots[0]))) return;
    memset(s_dots, 0, (size_t)w * h * sizeof(s_dots[0]));
    for (int x = 0; x < w; x += 2) s_dots[(h - 1) * w + x] = (ls_dot_t){ TUI_BLACK | TUI_BRIGHT, true };
    for (int i = 0; i < s_count; i++) {
        const ls_drone_watch_entry_t *e = &s_view[i];
        if ((e->source != L->a && e->source != L->b) || !e->centre_hz) continue;
        const double mhz = e->centre_hz / 1e6;
        if (mhz < L->lo || mhz > L->hi) continue;
        const int x = (int)((mhz - L->lo) * (w - 1) / (L->hi - L->lo));
        float v = (e->rssi_dbm + 110.0f) / 80.0f;
        if (v < 0.08f) v = 0.08f;
        if (v > 1) v = 1;
        const int top = h - 1 - (int)lroundf(v * (h - 1));
        const uint8_t hue = conf_hue(e->confidence);
        /* A line from the floor with a cap at the level: ASCII weight, not a block. */
        for (int y = top + 1; y < h; y++) s_dots[y * w + x] = (ls_dot_t){ hue, true };
        for (int dx = -2; dx <= 2; dx++)
            if (x + dx >= 0 && x + dx < w) s_dots[top * w + x + dx] = (ls_dot_t){ (uint8_t)(hue | TUI_BRIGHT), true };
    }
    ls_dots_blit(sf, plot, s_dots, w);
    /* Each link's level over its cap. */
    for (int i = 0; i < s_count; i++) {
        const ls_drone_watch_entry_t *e = &s_view[i];
        if ((e->source != L->a && e->source != L->b) || !e->centre_hz) continue;
        const double mhz = e->centre_hz / 1e6;
        if (mhz < L->lo || mhz > L->hi) continue;
        float v = (e->rssi_dbm + 110.0f) / 80.0f;
        if (v < 0.08f) v = 0.08f;
        if (v > 1) v = 1;
        const int cx = plot.x + (int)((mhz - L->lo) * (plot.w - 1) / (L->hi - L->lo));
        const int cy = plot.y + (plot.h - 1) - (int)lroundf(v * (plot.h - 1)) - 1;
        char lv[8];
        snprintf(lv, sizeof(lv), "%.0f", e->rssi_dbm);
        int lx = cx - (int)strlen(lv) / 2;
        if (lx < plot.x) lx = plot.x;
        if (lx + (int)strlen(lv) > plot.x + plot.w) lx = plot.x + plot.w - (int)strlen(lv);
        if (cy >= plot.y) put(sf, box, lx, cy, lv, BRIGHT(conf_hue(e->confidence)));
    }
    char s[64];
    snprintf(s, sizeof(s), "%u", L->lo); put(sf, box, plot.x, plot.y + plot.h, s, LS_ATTR_DIM);
    snprintf(s, sizeof(s), "%u %s", L->hi, ls_tr("MHz")); put(sf, box, plot.x + plot.w - (int)strlen(s), plot.y + plot.h, s, LS_ATTR_DIM);
    /* The cursor: where a source in this lane last actually measured. */
    for (int k = 0; k < 2; k++) {
        const ls_dw_source_t src = k ? L->b : L->a;
        if (k && L->b == L->a) break;
        if (!s_cur[src].us || now - s_cur_seen[src] > 3000000) continue;
        const unsigned mid = (s_cur[src].lo_mhz + s_cur[src].hi_mhz) / 2;
        if (mid < L->lo || mid > L->hi) continue;
        const int cx = plot.x + (int)((mid - L->lo) * (unsigned)(plot.w - 1) / (L->hi - L->lo));
        tui_put_char(sf, box, cx, plot.y + plot.h, '^', BRIGHT(TUI_WHITE));
    }
    for (int k = 0; k < 2; k++) {
        const ls_dw_source_t src = k ? L->b : L->a;
        if (k && L->b == L->a) break;
        const bool on = s_status.running[src];
        snprintf(s, sizeof(s), "%c %s %.*s", on ? '*' : '.', label(src), box.w - 18 > 0 ? box.w - 18 : 0,
                 on ? ls_tr("scanning") : (s_status.reason[src][0] ? ls_tr(s_status.reason[src]) : ls_tr("off")));
        put(sf, box, box.x + 2, box.y + box.h - 1 - status_rows + k, s, on ? INK(TUI_GREEN) : LS_ATTR_DIM);
    }
}

static void bands_page(tui_surface *sf, tui_rect a, int64_t now)
{
    const int n = 3;
    if (ls_tui_is_wide()) {
        const int w = a.w / n;
        for (int i = 0; i < n; i++)
            lane(sf, tui_rect_make(a.x + i * w, a.y, i == n - 1 ? a.w - i * w : w, a.h), &LANES[i], now);
    } else {
        const int h = a.h / n;
        for (int i = 0; i < n; i++)
            lane(sf, tui_rect_make(a.x, a.y + i * h, a.w, i == n - 1 ? a.h - i * h : h), &LANES[i], now);
    }
}

static void find_page(tui_surface *sf, tui_rect a, int64_t now)
{
    const ls_drone_watch_entry_t *e = picked();
    ls_panel_box(sf, a, LS_TEXT(t_ls_drone_ui_find_walk_toward_the, "FIND / WALK TOWARD THE STRONGEST"), TUI_MAGENTA);
    if (!e) {
        put(sf, a, a.x + 2, a.y + 2, ls_tr("Nothing to find yet."), BRIGHT(TUI_GREEN));
        put(sf, a, a.x + 2, a.y + 3, ls_tr("A link heard on WATCH shows here."), LS_ATTR_DIM);
        return;
    }
    char s[96], t[40];
    uint8_t tink;
    const uint8_t hue = conf_hue(e->confidence);
    snprintf(s, sizeof(s), "%s  %s", label(e->source), e->channel[0] ? e->channel : "");
    put(sf, a, a.x + 2, a.y + 1, s, BRIGHT(TUI_CYAN));
    snprintf(s, sizeof(s), ls_tr("%s  heard %.0fs ago  %u/%u"), conf_word(e->confidence), (now - e->last_us) / 1e6,
             (unsigned)s_pick + 1, (unsigned)s_count);
    put(sf, a, a.x + 2, a.y + 2, s, BRIGHT(hue));
    /* The tower: one row per 8 dB from -110, lit to the current level. */
    const int rows = a.h - 14 > 16 ? 16 : a.h - 14;
    float v = (e->rssi_dbm + 110.0f) / 80.0f;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    const int lit = (int)lroundf(v * rows);
    for (int r = 0; r < rows; r++) {
        const int level = rows - r;                 /* top row is strongest */
        const int bw = 4 + level * (a.w - 12) / (rows > 0 ? rows : 1);
        const uint8_t ink = level <= lit ? BRIGHT(level > rows * 2 / 3 ? TUI_RED : level > rows / 3 ? TUI_YELLOW : TUI_GREEN)
                                         : LS_ATTR_FAINT;
        for (int x = 0; x < bw && x < a.w - 6; x++)
            tui_put_char(sf, a, a.x + 3 + x, a.y + 4 + r, level <= lit ? '=' : '-', ink);
    }
    snprintf(s, sizeof(s), "%.0f %s   %s %.0f", e->rssi_dbm, e->calibrated ? "dBm" : ls_tr("dB"), ls_tr("peak"), e->peak_dbm);
    put(sf, a, a.x + 3, a.y + 5 + rows, s, BRIGHT(TUI_WHITE));
    trend_words(e, t, sizeof(t), &tink);
    put(sf, a, a.x + 3, a.y + 6 + rows, t, tink);
    spark(sf, tui_rect_make(a.x + 2, a.y + 8 + rows, a.w - 4, 2), e, hue);
    put(sf, a, a.x + 2, a.y + a.h - 2, ls_tr("Turn slowly; stronger is closer or facing it."), LS_ATTR_DIM);
}

/* ----------------------------------------------------------- options */

static int opt_get(const ls_opt_t *o)
{
    if (o->arg == 100) return s_background;
    if (o->arg == 101) return s_status.alerts;
    return (s_status.sources >> o->arg) & 1;
}
static void opt_set(const ls_opt_t *o, int v)
{
    if (o->arg == 100) {
        s_background = v;
        if (v) { (void)ls_drone_watch_enable(true); s_session_on = false; }
        return;
    }
    if (o->arg == 101) { (void)ls_drone_watch_alerts(v); return; }
    (void)ls_drone_watch_source_enable((ls_dw_source_t)o->arg, v);
}
static const ls_opt_t SOURCES[] = {
    {.label = "ELRS 900",       .kind = LS_OPT_TOGGLE, .arg = LS_DW_ELRS900,   .get = opt_get, .set = opt_set},
    {.label = "ELRS 2.4",       .kind = LS_OPT_TOGGLE, .arg = LS_DW_ELRS24,    .get = opt_get, .set = opt_set},
    {.label = "900 HOPPER",     .kind = LS_OPT_TOGGLE, .arg = LS_DW_HOPPER900, .get = opt_get, .set = opt_set},
    {.label = "2.4 WIDEBAND",   .kind = LS_OPT_TOGGLE, .arg = LS_DW_WIDE24,    .get = opt_get, .set = opt_set},
    {.label = "5.8 ANALOG FPV", .kind = LS_OPT_TOGGLE, .arg = LS_DW_ANALOG58,  .get = opt_get, .set = opt_set},
    {.label = "REMOTE ID",      .kind = LS_OPT_TOGGLE, .arg = LS_DW_RID,       .get = opt_get, .set = opt_set},
};
static const ls_opt_ctx_t SOURCES_CTX = {.name = "SOURCES", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(SOURCES)};
static const ls_opt_t OPTIONS[] = {
    {.label = "KEEP WATCHING IN BACKGROUND", .kind = LS_OPT_TOGGLE, .arg = 100, .get = opt_get, .set = opt_set},
    {.label = "ALERTS",  .kind = LS_OPT_TOGGLE, .arg = 101, .get = opt_get, .set = opt_set},
    {.label = "SOURCES", .kind = LS_OPT_MENU, .sub = &SOURCES_CTX},
};
static const ls_opt_ctx_t CTX = {.name = "DRONES", .job = -1, .radio = LS_RSEL_NONE, .back_to_screen = true, LS_OPT_ROWS(OPTIONS)};

/* ------------------------------------------------------------ screen */

static void watch_toggle(void)
{
    const bool on = !s_status.enabled;
    (void)ls_drone_watch_enable(on);
    s_status.enabled = on;
    s_session_on = on && !s_background;
}

static void draw(tui_surface *sf, tui_rect a)
{
    const int64_t now = esp_timer_get_time();
    refresh(now);
    if (a.w < 30 || a.h < 16) { ls_panel_notice(sf, a, "DRONES", LS_TEXT(t_ls_drone_ui_more_screen_space_needed, "More screen space needed"), "Use a smaller font"); return; }
    const bool wide = ls_tui_is_wide();
    const int bar_h = wide ? 3 : 8;   /* portrait: two rows of thumb-sized keys */
    ls_btn_t b[] = {
        {"WATCH", s_status.enabled ? "ON" : "OFF", 'w', s_page == PG_WATCH, false, false},
        {"BANDS", "900/2.4/5.8", '2', s_page == PG_BANDS, false, false},
        {"REMOTE", "ID", '3', s_page == PG_RID, false, false},
        {"FIND", s_count ? "LINK" : "--", '4', s_page == PG_FIND, false, false},
        ls_opt_button(&CTX),
    };
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), b, 5, -1);
    a.h -= bar_h;
    const int top = header(sf, a, now);
    tui_rect body = tui_rect_make(a.x, a.y + top, a.w, a.h - top);
    switch (s_page) {
    case PG_BANDS: bands_page(sf, body, now); break;
    case PG_RID:   ls_rid_draw_body(sf, body); break;
    case PG_FIND:  find_page(sf, body, now); break;
    default:       watch_page(sf, body, now); break;
    }
}

static void select_step(int d)
{
    if (!s_count) return;
    s_pick = (s_pick + s_count + d) % s_count;
    s_pick_id = s_view[s_order[s_pick]].id;
}

static bool key(ls_tk_t k, char ch)
{
    if (ls_opt_key(&CTX, ch)) return true;
    if (s_page == PG_RID && ls_rid_in_detail()) {
        if (k == LS_TK_ESC || ch == 'b' || ch == 'B') { ls_rid_close_detail(); return true; }
        return ls_rid_key(k, ch);
    }
    if (k == LS_TK_ESC || ch == 'b' || ch == 'B') {
        if (s_page != PG_WATCH) { s_page = PG_WATCH; return true; }
        if (k == LS_TK_ESC) return false;
        ls_tui_screen_show(0);
        return true;
    }
    if (ch == 'w' || ch == 'W') { if (s_page == PG_WATCH) watch_toggle(); s_page = PG_WATCH; return true; }
    if (ch == '1') { s_page = PG_WATCH; return true; }
    if (ch == '2') { s_page = PG_BANDS; return true; }
    if (ch == '3') { s_page = PG_RID; return true; }
    if (ch == '4') { s_page = PG_FIND; return true; }
    if (s_page == PG_RID) return ls_rid_key(k, ch);
    if (k == LS_TK_UP || ch == '[') { select_step(-1); return true; }
    if (k == LS_TK_DOWN || ch == ']') { select_step(1); return true; }
    if (k == LS_TK_ENTER && s_count) { s_page = PG_FIND; return true; }
    return false;
}

static bool touch(int x, int y)
{
    const int hit = ls_btn_hit(x, y);
    if (hit == 0) { if (s_page == PG_WATCH) watch_toggle(); s_page = PG_WATCH; return true; }
    if (hit == 1) { s_page = PG_BANDS; return true; }
    if (hit == 2) { s_page = PG_RID; return true; }
    if (hit == 3) { s_page = PG_FIND; return true; }
    if (hit == 4) { ls_opt_open(&CTX); return true; }
    if (s_page == PG_RID) return ls_rid_touch_body(x, y);
    if (s_page == PG_WATCH && s_count && x >= s_list.x && x < s_list.x + s_list.w &&
        y > s_list.y && y < s_list.y + s_list.h - 1) {
        const int i = s_pick / s_rows * s_rows + (y - s_list.y - 1) / 4;
        if (i < s_count) { s_pick = i; s_pick_id = s_view[s_order[i]].id; s_page = PG_FIND; }
        return true;
    }
    if (s_page == PG_FIND) { select_step(1); return true; }
    return false;
}

static void enter(void)
{
    s_refreshed = 0;
    ls_drone_watch_foreground(true);   /* on screen: may pause MeshCore to scan */
    (void)ls_drone_watch_status(&s_status);
    /* Opening the app listens; leaving stops it unless asked to keep going. */
    if (!s_status.enabled) { (void)ls_drone_watch_enable(true); s_status.enabled = true; s_session_on = !s_background; }
}
static void leave(void)
{
    ls_rid_leave();
    ls_drone_watch_foreground(false);
    if (s_session_on && !s_background) { (void)ls_drone_watch_enable(false); s_session_on = false; }
}

const ls_tui_screen_t ls_scr_drones = {
    .name = "DRONES",
    .hint = "W watch  2 bands  3 Remote ID  4 find  [ ] select  O options",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
};
