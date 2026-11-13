/* FREQS: the frequency library. A header with the entry under the cursor in
   the house digits, four filters, the list, and TUNE / SAVE / BANK /
   OPTIONS under the thumb. The banks are CSV files on the card; see
   ls_freqs.h for the format and ls_freqs_run.c for where TUNE goes. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_freqs.h"
#include "../../ls_picker.h"
#include "../../ls_drag_scroll.h"
#include "../../ls_keyboard.h"
#include "../../ls_numpad.h"
#include "../../ls_radio_panel.h"
#include "../../ls_motion.h"
#include "../../ls_value.h"
#include "../../ls_app.h"
#include "ls_gps.h"
#include "core/settings.h"
#include "scan_import.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ls_textpack.h"
#include "scr_freqs_desk.h"

#define HUE TUI_YELLOW
/* Lines read off the card each frame: a 1000-line bank in under a second,
   and never long enough to hold a frame up. */
#define PUMP_LINES 48

static EXT_RAM_BSS_ATTR uint16_t s_idx[LS_FREQ_MAX];
static EXT_RAM_BSS_ATTR ls_freq_filter_t s_f;
static EXT_RAM_BSS_ATTR char s_say[96];
static int s_n, s_sel, s_first, s_rows, s_per;
static uint32_t s_built_gen = UINT32_MAX;
static bool s_stale = true, s_restored, s_fix;
static bool s_top;             /* the next rebuild starts at the top */
static double s_lat, s_lon, s_built_lat, s_built_lon;
static int button_focus = -1, button_slot;
static tui_rect s_list, s_scroll, s_head;
/* A finger dragged on the list moves the window on its own; the selection
   stays where it was until a key or a tap moves it, and only then does the
   window go back to following it. */
static ls_drag_scroll_t s_drag;
static bool s_free;
static int64_t s_armed_us;     /* DELETE / LOAD INTO SCANNER asked once */
static int s_armed;            /* which of them */
static int64_t s_said_us;

static void say(const char *text) { snprintf(s_say, sizeof(s_say), "%s", text); s_said_us = esp_timer_get_time(); }

/* NULL as well when a reload has taken the entry away under the view. */
static const ls_freq_t *selected(void) { return s_sel >= 0 && s_sel < s_n ? ls_freqs_at(s_idx[s_sel]) : NULL; }

/* --------------------------------------------------------- the settings -- */

/* Band, sort, NEAR and the bank (by a hash of its name, since the files on a
   card come and go) in one stored word. */
static uint32_t bank_hash(int which)
{
    if (which == LS_FREQS_ALL) return 0;
    if (which == LS_FREQS_BUILTIN) return 1;
    uint32_t h = 2166136261u;
    for (const char *p = ls_freqs_bank(which); *p; p++) h = (h ^ (uint8_t)(*p | 0x20)) * 16777619u;
    return 2 + h % 254;
}

static void store_view(void)
{
    settings_set_freqs_view((uint32_t)s_f.band | (uint32_t)s_f.sort << 3 | (uint32_t)s_f.near << 5 |
                            bank_hash(ls_freqs_which()) << 6);
}

static void restore_view(void)
{
    const uint32_t v = settings_get_freqs_view();
    s_f.band = (uint8_t)((v & 7) < LS_FB__COUNT ? (v & 7) : 0);
    s_f.sort = (uint8_t)((v >> 3) & 3);
    s_f.near = (v >> 5) & 1;
    const uint32_t want = (v >> 6) & 0xFF;
    int which = want == 1 ? LS_FREQS_BUILTIN : LS_FREQS_ALL;
    for (int i = 0; want > 1 && i < ls_freqs_banks(); i++) if (bank_hash(i) == want) which = i;
    ls_freqs_load(which);
}

/* ----------------------------------------------------------------- view -- */

static void fix_now(void)
{
    static EXT_RAM_BSS_ATTR ls_gps_state_t g;
    ls_gps_get(&g);
    const int64_t now = esp_timer_get_time();
    /* A minute is still where you are on foot, and NEAR should not blink off
       under a tree. */
    s_fix = (g.fix || (g.last_fix_us > 0 && now - g.last_fix_us < 60000000)) && fabs(g.lat_deg) <= 90 &&
            fabs(g.lon_deg) <= 180 && (g.lat_deg != 0 || g.lon_deg != 0);
    if (s_fix) { s_lat = g.lat_deg; s_lon = g.lon_deg; }
}

/* The entry the cursor is on, kept by value: a reload empties the set first
   and fills it over several frames, and the cursor finds its entry again
   when it comes back in. */
static uint32_t s_want_hz;
static char s_want_name[LS_FREQ_NAME];
static bool s_want_pending;
static int s_built_sel;

static void want(const ls_freq_t *e)
{
    s_want_hz = e ? e->hz : 0;
    snprintf(s_want_name, sizeof(s_want_name), "%s", e ? e->name : "");
}

static void rebuild(void)
{
    const uint32_t hz = s_want_hz;
    const char *name = s_want_name;
    s_n = ls_freqs_view(&s_f, s_fix, s_lat, s_lon, s_idx, LS_FREQ_MAX);
    s_built_gen = ls_freqs_generation();
    s_built_lat = s_lat; s_built_lon = s_lon;
    s_stale = false;
    /* Keep the cursor on the same entry across a reload or a filter. */
    s_sel = s_built_sel = 0;
    s_want_pending = false;
    if (s_top) { s_top = false; return; }
    for (int i = 0; hz && i < s_n; i++) {
        const ls_freq_t *e = ls_freqs_at(s_idx[i]);
        if (e && e->hz == hz && !strcmp(e->name, name)) { s_sel = s_built_sel = i; return; }
    }
    s_want_pending = hz && ls_freqs_loading();
}

static void refresh(void)
{
    /* Moved by hand while a reload ran: that one is the one to keep now. */
    if (s_want_pending && s_sel != s_built_sel) s_want_pending = false;
    /* Only while the view still matches the set: after a reload has begun,
       s_idx points into entries that are gone. */
    if (!s_want_pending && s_built_gen == ls_freqs_generation()) want(selected());
    ls_freqs_pump(PUMP_LINES);
    fix_now();
    const bool wants_fix = s_f.near || s_f.sort == LS_FS_NEAR;
    if (wants_fix && s_fix && ls_freq_km(s_lat, s_lon, s_built_lat, s_built_lon) > 0.3) s_stale = true;
    if (s_stale || s_built_gen != ls_freqs_generation()) rebuild();
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
}

/* --------------------------------------------------------------- emblem -- */

/* A dial with its needle. At rest the needle stands where the entry under
   the cursor sits on the radio spectrum, 1 MHz to 3 GHz on a log scale; it
   sweeps the scale only while a bank is being read off the card. */
void ls_freqs_dial(tui_surface *sf, tui_rect clip, int x, int y, const ls_freq_t *e, bool busy)
{
    const uint8_t edge = TUI_ATTR(HUE, TUI_BLACK), tick = LS_ATTR_FAINT;
    const uint8_t needle = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    tui_put_str(sf, clip, x, y, ".---------.", edge);
    tui_put_str(sf, clip, x, y + 1, "|", edge);
    tui_put_str(sf, clip, x + 1, y + 1, ".:.:.:.:.", tick);
    tui_put_str(sf, clip, x + 10, y + 1, "|", edge);
    tui_put_str(sf, clip, x, y + 2, "'---------'", edge);
    tui_put_str(sf, clip, x + 1, y + 3, "HF VHF UHF", LS_ATTR_FAINT);
    int at;
    if (busy) {
        const int p = ls_motion_phase(16, 1400);
        at = p < 9 ? p : 16 - p;
    } else if (e) {
        /* Nine stops, a little over a factor of 2.7 each, from 1 MHz. */
        at = 0;
        for (uint32_t edge = 1600000u; at < 8 && e->hz >= edge; edge = edge / 10 * 27) at++;
    } else at = 4;
    tui_put_char(sf, clip, x + 1 + at, y + 1, '|', needle);
    tui_put_char(sf, clip, x + 1 + at, y, 'v', TUI_ATTR(HUE | TUI_BRIGHT, TUI_BLACK));
}

/* ------------------------------------------------------------- the face -- */

static void distance_text(const ls_freq_t *e, char *out, size_t n)
{
    if (e->lat_e5 == LS_FREQ_NOWHERE) snprintf(out, n, "anywhere");
    else if (!s_fix) snprintf(out, n, "no fix");
    else {
        /* Tenths of a km under ten, whole ones past it, in integers. */
        const float lat = e->lat_e5 * 1e-5f, lon = e->lon_e5 * 1e-5f;
        const int d = (int)(ls_freq_distance(e, s_lat, s_lon) * 10 + 0.5f);
        const char *b = ls_freq_bearing(s_lat, s_lon, lat, lon);
        if (d < 1) snprintf(out, n, "here");
        else if (d < 100) snprintf(out, n, "%d.%d km %s", d / 10, d % 10, b);
        else snprintf(out, n, "%d km %s", (d + 5) / 10, b);
    }
}

/* A bank as the screen names it: the file's name in capitals. */
static const char *bank_label(int which)
{
    static char t[2][28];
    static int k;
    k ^= 1;
    /* ALL with nothing on the card is the built-in set, and says so. */
    snprintf(t[k], sizeof(t[k]), "%s", which == LS_FREQS_ALL && !ls_freqs_banks() ? "NO CARD: BUILT-IN" : ls_freqs_bank(which));
    for (char *p = t[k]; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    return t[k];
}

static const char *bank_title(void)
{
    static char t[40];
    snprintf(t, sizeof(t), "FREQS / %s", bank_label(ls_freqs_which()));
    return t;
}

/* The header band: what the list holds, the entry under the cursor in the
   house digits with its name, group, mode and how far away it is. */
static int header(tui_surface *sf, tui_rect a, bool wide)
{
    const uint8_t white = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK), amber = TUI_ATTR(HUE | TUI_BRIGHT, TUI_BLACK);
    const int h = wide ? 10 : 11;
    tui_rect box = tui_rect_make(a.x, a.y, a.w, h);
    s_head = box;
    ls_panel_box(sf, box, bank_title(), HUE);
    const ls_freq_t *e = selected();
    const bool busy = ls_freqs_loading();
    ls_freqs_dial(sf, box, box.x + box.w - 13, box.y + 1, e, busy);
    char text[96];
    const ls_freq_tally_t *t = ls_freqs_tally();
    if (busy) snprintf(text, sizeof(text), "reading the card... %d", ls_freqs_count());
    else if (t->truncated) snprintf(text, sizeof(text), "%d of %d; bank cut at %d", s_n, ls_freqs_count(), LS_FREQ_MAX);
    else if (t->skipped) snprintf(text, sizeof(text), "%d of %d, %d line%s skipped (%d)", s_n, ls_freqs_count(),
                                  t->skipped, t->skipped == 1 ? "" : "s", t->first_bad);
    else snprintf(text, sizeof(text), "%d of %d", s_n, ls_freqs_count());
    tui_put_str(sf, box, box.x + 2, box.y + 1, text, busy ? amber : t->skipped ? amber : LS_ATTR_DIM);
    tui_rect digits = tui_rect_make(box.x + 1, box.y, box.w - 15, box.h);
    char mhz[16];
    if (e) {
        ls_freq_mhz_text(e->hz, mhz, sizeof(mhz));
        ls_radio_digits_draw(sf, digits, 2, mhz, amber);
    } else ls_radio_digits_draw(sf, digits, 2, "000.000", LS_ATTR_FAINT);
    tui_put_str(sf, box, box.x + box.w - 6, box.y + 6, "MHz", e ? white : LS_ATTR_FAINT);
    int y = box.y + 7;
    if (!e) {
        tui_put_str(sf, box, box.x + 2, y, busy ? "" : "Nothing here: SAVE adds, BANK imports", LS_ATTR_DIM);
        return box.y + box.h;
    }
    const tui_rect line = tui_rect_make(box.x + 2, y, box.w - 4, box.y + box.h - 1 - y);
    tui_put_str(sf, line, line.x, y, e->name, white);
    y++;
    int x = box.x + 2;
    x = ls_radio_chip(sf, box, x, y, ls_freq_mode_name(e->mode), true, TUI_CYAN);
    x = ls_radio_chip(sf, box, x, y, e->group[0] ? e->group : "NO GROUP", e->group[0] != 0, ls_freq_group_hue(e->group));
    if (strstr(e->tags, "tx-ok")) x = ls_radio_chip(sf, box, x, y, "TX OK", true, TUI_GREEN);
    distance_text(e, text, sizeof(text));
    tui_put_str(sf, box, x, y, text, e->lat_e5 != LS_FREQ_NOWHERE && s_fix ? amber : LS_ATTR_DIM);
    if (!wide) {
        y++;
        char more[96];
        snprintf(more, sizeof(more), "%s%s%s%s", e->tone[0] ? "tone " : "", e->tone,
                 e->tone[0] && (e->notes[0] || e->tags[0]) ? "  " : "", e->notes[0] ? e->notes : e->tags);
        tui_put_str(sf, line, line.x, y, more, LS_ATTR_DIM);
    }
    return box.y + box.h;
}

static const char *near_value(void)
{
    return !s_f.near ? "OFF" : s_fix ? "ON" : "NO FIX";
}

static void filters(tui_surface *sf, tui_rect r)
{
    ls_btn_t b[4] = {
        { "BAND", ls_freq_band_name(s_f.band), 'b', s_f.band != LS_FB_ALL, false, false },
        { "GROUP", s_f.group[0] ? ls_freq_group_tag(s_f.group) : "ALL", 'g', s_f.group[0] != 0, false, false },
        { "NEAR", near_value(), 'n', s_f.near && s_fix, false, false },
        { "FIND", s_f.find[0] ? s_f.find : "ANY", 'f', s_f.find[0] != 0, false, false },
    };
    /* A group without a tag of its own shows its own name. */
    if (s_f.group[0] && !strcmp(b[1].value, "---")) b[1].value = s_f.group;
    ls_btn_bar_rows_slot(sf, r, b, 4, -1, LS_BTN_SLOT_QUICK, 1);
}

/* Landscape's spare column: everything about the entry under the cursor
   that the row has no room for, and where TUNE will take it. */
static void detail(tui_surface *sf, tui_rect r)
{
    const ls_freq_t *e = selected();
    if (r.h < 4 || !e) return;
    ls_panel_box(sf, r, "THIS ONE", HUE);
    const tui_rect in = tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    const ls_freq_route_t route = ls_freq_route(e->mode, e->hz);
    const ls_app_t *app = route.app ? ls_app_by_id(route.app) : NULL;
    char text[96];
    int y = in.y;
    snprintf(text, sizeof(text), "TUNE  %s%s%s", app ? app->name : "nowhere here", route.arg ? " " : "",
             route.arg ? route.arg : "");
    for (char *p = text; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    tui_put_str(sf, in, in.x, y++, text, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (route.note) tui_put_str(sf, in, in.x, y++, route.note, LS_ATTR_DIM);
    const char *label[4] = { "tags", "tone", "notes", "bank" };
    const char *value[4] = { e->tags, e->tone, e->notes, ls_freqs_bank(e->bank) };
    for (int i = 0; i < 4 && y < in.y + in.h; i++) {
        if (!value[i][0]) continue;
        snprintf(text, sizeof(text), "%-6s%s", label[i], value[i]);
        tui_put_str(sf, in, in.x, y++, text, i == 3 ? LS_ATTR_FAINT : TUI_ATTR(TUI_WHITE, TUI_BLACK));
    }
}

static void list(tui_surface *sf, tui_rect panel, bool wide)
{
    char title[48];
    snprintf(title, sizeof(title), "%s  %d", ls_freq_sort_name(s_f.sort == LS_FS_NEAR && !s_fix ? LS_FS_LISTED : s_f.sort), s_n);
    ls_panel_box(sf, panel, title, HUE);
    s_list = tui_rect_make(panel.x + 1, panel.y + 1, panel.w - 4, panel.h - 2);
    s_scroll = tui_rect_make(panel.x + panel.w - 3, panel.y + 1, 2, panel.h - 2);
    s_per = wide ? 1 : 2;
    s_rows = s_list.h / s_per;
    if (s_rows < 1) s_rows = 1;
    if (!s_n) {
        const char *why = ls_freqs_loading() ? "Reading the card..." :
            !ls_freqs_count() ? "No frequencies here yet." :
            s_f.near && s_fix ? "Nothing placed within reach of here." :
            s_f.near ? "NEAR waits for a GPS fix." : "Nothing matches the filters.";
        tui_put_str(sf, s_list, s_list.x + 1, s_list.y + 1, why, LS_ATTR_DIM);
        tui_put_str(sf, s_list, s_list.x + 1, s_list.y + 3,
                    s_f.near ? "NEAR lists entries that have a place." : "ESC clears FIND; tap a filter to change it.", LS_ATTR_DIM);
        return;
    }
    if (ls_drag_scroll(&s_drag, s_list, s_per, &s_first, s_n, s_rows)) s_free = true;
    if (!s_free) {
        if (s_sel < s_first) s_first = s_sel;
        if (s_sel >= s_first + s_rows) s_first = s_sel - s_rows + 1;
    }
    if (s_first > s_n - s_rows) s_first = s_n - s_rows > 0 ? s_n - s_rows : 0;
    /* The columns: group tag, MHz, name, mode; in landscape the distance and
       the tags after the mode, in portrait on the row's second line. */
    const int w = s_list.w, x0 = s_list.x + 1;
    int name_w = wide ? LS_FREQ_NAME - 1 : w - 23;
    if (name_w < 8) name_w = 8;
    const int mode_x = wide ? x0 + 14 + name_w + 1 : s_list.x + w - 7;
    const tui_rect name_clip = tui_rect_make(x0 + 14, s_list.y, name_w, s_list.h);
    for (int r = 0; r < s_rows && s_first + r < s_n; r++) {
        const int k = s_first + r;
        const ls_freq_t *e = ls_freqs_at(s_idx[k]);
        if (!e) continue;
        const int y = s_list.y + r * s_per;
        const bool on = k == s_sel;
        if (on) {
            ls_fill_dither(sf, tui_rect_make(s_list.x, y, w, s_per), LS_DITHER_MEDIUM, HUE);
            for (int i = 0; i < s_per; i++)
                tui_put_char(sf, s_list, s_list.x, y + i, LS_TUI_BLOCK_FULL, TUI_ATTR(HUE | TUI_BRIGHT, TUI_BLACK));
        }
        const uint8_t hue = ls_freq_group_hue(e->group), ink = on ? TUI_WHITE | TUI_BRIGHT : TUI_WHITE;
        tui_put_str(sf, s_list, x0, y, ls_freq_group_tag(e->group), TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
        char text[96], dist[24];
        ls_freq_mhz_text(e->hz, dist, sizeof(dist));
        snprintf(text, sizeof(text), "%9s", dist);
        tui_put_str(sf, s_list, x0 + 4, y, text, TUI_ATTR(on ? HUE | TUI_BRIGHT : TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, name_clip, x0 + 14, y, e->name, TUI_ATTR(ink, TUI_BLACK));
        tui_put_str(sf, s_list, mode_x, y, ls_freq_mode_name(e->mode), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        const bool placed = e->lat_e5 != LS_FREQ_NOWHERE;
        distance_text(e, dist, sizeof(dist));
        if (wide) {
            /* The tags are in THIS ONE beside the list; a column cut to a
               few letters says nothing. */
            snprintf(text, sizeof(text), "%s", placed ? dist : "");
            tui_put_str(sf, s_list, mode_x + 8, y, text, placed && s_fix ? TUI_ATTR(HUE, TUI_BLACK) : LS_ATTR_DIM);
        } else {
            snprintf(text, sizeof(text), "%s%s%s  %s", e->group, placed ? "  " : "", placed ? dist : "",
                     e->tags[0] ? e->tags : e->notes);
            tui_put_str(sf, s_list, x0 + 4, y + 1, text, LS_ATTR_DIM);
        }
    }
    /* The scroll strip: where the window is in the whole list. */
    const int sh = s_scroll.h;
    tui_put_char(sf, panel, s_scroll.x, s_scroll.y, '^', s_first ? TUI_ATTR(HUE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_FAINT);
    tui_put_char(sf, panel, s_scroll.x, s_scroll.y + sh - 1, 'v',
                 s_first + s_rows < s_n ? TUI_ATTR(HUE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_FAINT);
    if (sh > 4 && s_n > s_rows) {
        const int span = sh - 2, top = s_first * span / s_n, len = s_rows * span / s_n;
        for (int i = 0; i <= len && top + i < span; i++)
            tui_put_char(sf, panel, s_scroll.x, s_scroll.y + 1 + top + i, LS_TUI_BLOCK_FULL, TUI_ATTR(HUE, TUI_BLACK));
    }
}

static const char *tune_value(void)
{
    const ls_freq_t *e = selected();
    if (!e) return NULL;
    const ls_freq_route_t r = ls_freq_route(e->mode, e->hz);
    if (r.target == LS_FR_NONE) return "NOT HERE";
    const ls_app_t *a = ls_app_by_id(r.app);
    return a ? a->name : r.app;
}

static void draw(tui_surface *sf, tui_rect a)
{
#if LS_TUI_HAS_COMPACT
    /* In a desk window FREQS is laid out as a desktop program is. */
    if (ls_tui_compact()) { ls_freqs_desk_draw(sf, a); return; }
#endif
    if (a.w < 36 || a.h < 18) {
        ls_panel_notice(sf, a, "FREQS", "Enlarge the pane", "the list keeps its place");
        return;
    }
    refresh();
    /* Side by side needs the width for both: a landscape half-pane is laid
       out as portrait is. */
    const bool wide = a.w > a.h * 2 && a.w >= 100;
    int bar_h = ls_btn_raised_height(a, 4);
    if (wide && bar_h < 3) bar_h = 3;
    const ls_freq_t *e = selected();
    const ls_freq_route_t r = e ? ls_freq_route(e->mode, e->hz) : (ls_freq_route_t){ LS_FR_NONE, NULL, NULL, NULL };
    ls_btn_t bar[4] = {
        { "TUNE", tune_value(), 't', false, !e || r.target == LS_FR_NONE, false },
        { "SAVE", "NEW", 's', false, false, false },
        { "BANK", bank_label(ls_freqs_which()), 'k', false, false, false },
        { "OPTIONS", NULL, 'o', false, false, false },
    };
    const tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - bar_h - 1);
    if (wide) {
        /* Landscape: the header and the filters on the left, the list on the
           right, the keys along the bottom as in portrait. */
        const int lw = body.w * 2 / 5 < 44 ? 44 : body.w * 2 / 5;
        tui_rect left = tui_rect_make(body.x, body.y, lw, body.h);
        int y = header(sf, left, true);
        filters(sf, tui_rect_make(left.x, y, left.w, 4));
        detail(sf, tui_rect_make(left.x, y + 4, left.w, body.y + body.h - y - 4));
        list(sf, tui_rect_make(body.x + lw + 1, body.y, body.w - lw - 1, body.h), true);
    } else {
        int y = header(sf, body, false);
        filters(sf, tui_rect_make(body.x, y, body.w, 4));
        y += 4;
        list(sf, tui_rect_make(body.x, y, body.w, body.y + body.h - y), false);
    }
    ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y + a.h - bar_h - 1, a.w, bar_h), bar, 4, button_focus,
                           LS_BTN_SLOT_SCREEN);
    const bool fresh = s_say[0] && esp_timer_get_time() - s_said_us < 8000000;
    ls_safe_line(sf, a, a.y + a.h - 1, fresh ? s_say : e && r.note ? r.note : "ENTER tunes; tap a row twice to tune it",
                 fresh ? TUI_ATTR(HUE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
}

/* ---------------------------------------------------------------- input -- */

static void tune(void)
{
    const ls_freq_t *e = selected();
    char msg[96];
    if (!e) { say("Nothing selected"); return; }
    ls_freqs_tune(e, msg, sizeof(msg));
    say(msg);
}

static void changed(void) { s_stale = true; s_free = false; store_view(); }

static void band_done(int i) { if (i >= 0 && i < LS_FB__COUNT) { s_f.band = (uint8_t)i; changed(); } }
static void open_band(void)
{
    ls_picker_open("BAND", band_done);
    static const char *const RANGE[LS_FB__COUNT] = { "everything", "below 30 MHz", "30-300 MHz", "300-700 MHz",
                                                     "700-960 MHz", "960 MHz and up" };
    for (int i = 0; i < LS_FB__COUNT; i++) ls_picker_add(ls_freq_band_name(i), RANGE[i]);
    ls_picker_select(s_f.band);
}

/* GROUP lists the groups in what is loaded, with how many of each. */
static EXT_RAM_BSS_ATTR char s_groups[LS_PICKER_MAX][LS_FREQ_GROUP];
static int s_ngroups;
static void group_done(int i)
{
    if (i == 0) s_f.group[0] = 0;
    else if (i > 0 && i <= s_ngroups) snprintf(s_f.group, sizeof(s_f.group), "%s", s_groups[i - 1]);
    else return;
    changed();
}
static void open_group(void)
{
    static EXT_RAM_BSS_ATTR int count[LS_PICKER_MAX];
    s_ngroups = 0;
    for (int i = 0; i < ls_freqs_count(); i++) {
        const ls_freq_t *e = ls_freqs_at(i);
        if (!e) continue;
        const char *g = e->group;
        int k = 0;
        while (k < s_ngroups && strcmp(s_groups[k], g)) k++;
        if (k == s_ngroups) {
            if (s_ngroups >= LS_PICKER_MAX - 1) continue;
            snprintf(s_groups[k], sizeof(s_groups[k]), "%s", g);
            count[k] = 0;
            s_ngroups++;
        }
        count[k]++;
    }
    ls_picker_open("GROUP", group_done);
    char detail[24];
    snprintf(detail, sizeof(detail), "%d", ls_freqs_count());
    ls_picker_add("ALL GROUPS", detail);
    for (int k = 0; k < s_ngroups; k++) {
        snprintf(detail, sizeof(detail), "%s  %d", ls_freq_group_tag(s_groups[k]), count[k]);
        ls_picker_add(s_groups[k][0] ? s_groups[k] : "(none)", detail);
        if (!strcmp(s_groups[k], s_f.group) && s_f.group[0]) ls_picker_select(k + 1);
    }
}

static void find_done(const char *text) { snprintf(s_f.find, sizeof(s_f.find), "%s", text); changed(); }
/* NEAR sorts nearest first and goes to the top, where the nearest is; off
   again, the list goes back to the order it had. */
static uint8_t s_sort_before = LS_FS_LISTED;
static void toggle_near(void)
{
    s_f.near = !s_f.near;
    if (s_f.near) { s_sort_before = s_f.sort; s_f.sort = LS_FS_NEAR; s_sel = s_first = 0; s_top = true; }
    else if (s_f.sort == LS_FS_NEAR) s_f.sort = s_sort_before;
    changed();
    say(!s_f.near ? "NEAR off: everything" : s_fix ? "NEAR: placed within reach, nearest first"
                                                   : "NEAR waits for a GPS fix; showing all");
}

static void filter_action(int i)
{
    if (i == 0) open_band();
    else if (i == 1) open_group();
    else if (i == 2) toggle_near();
    else if (i == 3) {
        if (s_f.find[0]) { s_f.find[0] = 0; changed(); }
        else ls_keyboard_open(LS_TEXT(t_scr_freqs_find_prompt, "FIND: NAME, TAG, GROUP OR MHZ"), "", sizeof(s_f.find) - 1, find_done);
    }
}

/* ---- SAVE: from what FM or P25 is on, typed, or a copy of this one -- */

static EXT_RAM_BSS_ATTR ls_freq_t s_new;
static void saved(void)
{
    char msg[96];
    if (!s_new.name[0]) ls_freq_mhz_text(s_new.hz, s_new.name, sizeof(s_new.name));
    ls_freqs_keep(&s_new, msg, sizeof(msg));
    say(msg);
}
static void name_done(const char *text)
{
    snprintf(s_new.name, sizeof(s_new.name), "%s", text);
    for (char *p = s_new.name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    saved();
}
static void ask_name(void)
{
    char mhz[16];
    ls_freq_mhz_text(s_new.hz, mhz, sizeof(mhz));
    ls_keyboard_open(LS_TEXT(t_scr_freqs_name_prompt, "NAME IT (BLANK: THE FREQUENCY)"), "", LS_FREQ_NAME - 1, name_done);
}
static void mode_done(int i)
{
    if (i < 0 || i >= LS_FM__COUNT) return;
    s_new.mode = (uint8_t)i;
    ask_name();
}
static void ask_mode(void)
{
    ls_picker_open("MODE", mode_done);
    for (int m = 0; m < LS_FM__COUNT; m++) {
        const ls_freq_route_t r = ls_freq_route(m, s_new.hz);
        const ls_app_t *a = r.app ? ls_app_by_id(r.app) : NULL;
        ls_picker_add(ls_freq_mode_name(m), a ? a->name : "listed only");
    }
    ls_picker_select(s_new.mode);
}
static void typed(double mhz)
{
    if (mhz < 0.009 || mhz > 6000) { say("0.009 to 6000 MHz"); return; }
    s_new.hz = (uint32_t)lround(mhz * 1e6);
    ask_mode();
}
static void fresh_entry(void)
{
    memset(&s_new, 0, sizeof(s_new));
    s_new.lat_e5 = s_new.lon_e5 = LS_FREQ_NOWHERE;
    snprintf(s_new.group, sizeof(s_new.group), "MINE");
}
/* What FM and P25 are on now, read the way a user app reads them. */
static bool app_freq(const char *path, uint32_t *hz)
{
    ls_val_t v;
    if (!ls_value_read(path, &v, NULL) || v.kind != LS_VAL_FLOAT || v.f < 0.5f) return false;
    *hz = (uint32_t)lround(v.f * 1e6 / 100.0) * 100u;
    return true;
}
static void save_done(int i)
{
    uint32_t hz;
    fresh_entry();
    if (i == 0) {
        ls_numpad_open("FREQUENCY", "MHz", selected() ? selected()->hz / 1e6 : 145.5, typed);
    } else if (i == 1 && app_freq("fm.freq", &hz)) {
        ls_val_t m;
        s_new.hz = hz;
        int mode = ls_value_read("fm.mode", &m, NULL) && m.kind == LS_VAL_TEXT ? ls_freq_mode_parse(m.s) : -1;
        s_new.mode = (uint8_t)(mode >= 0 ? mode : LS_FM_NFM);
        snprintf(s_new.tags, sizeof(s_new.tags), "fm");
        ask_name();
    } else if (i == 2 && app_freq("p25.freq", &hz)) {
        s_new.hz = hz;
        s_new.mode = LS_FM_P25;
        snprintf(s_new.tags, sizeof(s_new.tags), "p25");
        ask_name();
    } else if (i == 3 && selected()) {
        s_new = *selected();
        snprintf(s_new.group, sizeof(s_new.group), "MINE");
        saved();
    }
}
static void open_save(void)
{
    char fm[32] = "not running", p25[32] = "not running", sel[32] = "nothing selected";
    uint32_t hz;
    ls_val_t m;
    if (app_freq("fm.freq", &hz)) {
        ls_freq_mhz_text(hz, fm, sizeof(fm));
        if (ls_value_read("fm.mode", &m, NULL) && m.kind == LS_VAL_TEXT && m.s) {
            const size_t k = strlen(fm);
            snprintf(fm + k, sizeof(fm) - k, " %s", m.s);
        }
    }
    if (app_freq("p25.freq", &hz)) ls_freq_mhz_text(hz, p25, sizeof(p25));
    if (selected()) snprintf(sel, sizeof(sel), "%s", selected()->name);
    ls_picker_open(LS_TEXT(t_scr_freqs_save_title, "SAVE INTO MINE"), save_done);
    ls_picker_add("TYPE A FREQUENCY", "then its mode and name");
    ls_picker_add("WHAT FM IS ON", fm);
    ls_picker_add("WHAT P25 IS ON", p25);
    ls_picker_add("COPY THIS ONE", sel);
}

/* ---- BANK: which bank, and getting files in and out ----------------- */

static EXT_RAM_BSS_ATTR char s_files[24][96];
static int s_nfiles;
static void open_bank(void);
static void import_done(int i)
{
    if (i < 0 || i >= s_nfiles) return;
    char msg[96];
    if (ls_freqs_import(s_files[i], msg, sizeof(msg))) {
        /* Straight to what came in. */
        const int b = ls_freqs_bank_find(ls_freqs_imported());
        ls_freqs_load(b >= 0 ? b : LS_FREQS_ALL);
        store_view();
    }
    say(msg);
}
static void add_files(const char *dir, bool csv_too)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s_nfiles < 24) {
        const char *n = de->d_name, *dot = strrchr(n, '.');
        if (n[0] == '.' || !dot || strlen(dir) + strlen(n) + 2 > sizeof(s_files[0])) continue;
        if (strcasecmp(dot, ".lscan") && strcasecmp(dot, ".json") && strcasecmp(dot, ".txt") &&
            !(csv_too && !strcasecmp(dot, ".csv"))) continue;
        snprintf(s_files[s_nfiles++], sizeof(s_files[0]), "%s/%s", dir, n);
    }
    closedir(d);
}
static void open_import(void)
{
    char dir[96];
    s_nfiles = 0;
    snprintf(dir, sizeof(dir), "%s/import", ls_freqs_root());
    add_files(dir, true);
    add_files("/sdcard", true);
    ls_picker_open(LS_TEXT(t_scr_freqs_import_title, "IMPORT INTO A NEW BANK"), import_done);
    ls_picker_back(open_bank);
    ls_picker_empty_reason(LS_TEXT(t_scr_freqs_import_none, "Put .csv (CHIRP, gqrx), .json (SDR++) or .lscan files in /sdcard/freqs/import"));
    for (int i = 0; i < s_nfiles; i++) {
        const char *base = strrchr(s_files[i], '/') + 1;
        ls_picker_add(base, strncmp(s_files[i], "/sdcard/freqs", 13) ? "card root" : "freqs/import");
    }
}
static void bank_done(int i)
{
    const int banks = ls_freqs_banks();
    if (i == 0) ls_freqs_load(LS_FREQS_ALL);
    else if (i >= 1 && i <= banks) ls_freqs_load(i - 1);
    else if (i == banks + 1) ls_freqs_load(LS_FREQS_BUILTIN);
    else if (i == banks + 2) { open_import(); return; }
    else return;
    s_stale = true;
    store_view();
}
static void open_bank(void)
{
    const bool card = ls_freqs_scan();
    ls_picker_open("BANK", bank_done);
    ls_picker_add("ALL BANKS", card ? "every file, merged" : "no card: the built-in set");
    for (int i = 0; i < ls_freqs_banks(); i++) {
        char detail[40];
        snprintf(detail, sizeof(detail), "%s.csv", ls_freqs_bank(i));
        ls_picker_add(bank_label(i), detail);
    }
    ls_picker_add("BUILT-IN", "the starter set, no card needed");
    ls_picker_add("IMPORT A FILE", "CHIRP, SDR++, gqrx, .lscan");
    const int w = ls_freqs_which();
    ls_picker_select(w == LS_FREQS_ALL ? 0 : w == LS_FREQS_BUILTIN ? ls_freqs_banks() + 1 : w + 1);
}

/* ---- OPTIONS -------------------------------------------------------- */

enum { O_SORT, O_RENAME, O_MODE, O_FREQ, O_COPY, O_DELETE, O_SCANNER, O_CSV, O_LSCAN, O_RELOAD, O_CLEAR, O__N };
static bool armed(int which)
{
    return s_armed == which && esp_timer_get_time() - s_armed_us < 6000000;
}
static void arm(int which, const char *what)
{
    s_armed = which;
    s_armed_us = esp_timer_get_time();
    say(what);
}
static void edit_with(const ls_freq_t *with)
{
    char msg[96] = "Nothing selected";
    const ls_freq_t *was = selected();
    if (was && ls_freqs_rewrite(s_idx[s_sel], with, msg, sizeof(msg))) {
        /* The cursor follows the edit, or after a DELETE the one below. */
        const ls_freq_t *next = s_sel + 1 < s_n ? ls_freqs_at(s_idx[s_sel + 1]) : s_sel > 0 ? ls_freqs_at(s_idx[s_sel - 1]) : NULL;
        want(with ? with : next);
        ls_freqs_load(ls_freqs_which());
        s_want_pending = s_want_hz != 0;
        s_built_sel = s_sel;
    }
    say(msg);
}
static void rename_done(const char *text)
{
    if (!selected() || !text[0]) return;
    ls_freq_t e = *selected();
    snprintf(e.name, sizeof(e.name), "%s", text);
    edit_with(&e);
}
static void remode_done(int i)
{
    if (!selected() || i < 0 || i >= LS_FM__COUNT) return;
    ls_freq_t e = *selected();
    e.mode = (uint8_t)i;
    edit_with(&e);
}
static void refreq_done(double mhz)
{
    if (!selected() || mhz < 0.009 || mhz > 6000) return;
    ls_freq_t e = *selected();
    e.hz = (uint32_t)lround(mhz * 1e6);
    edit_with(&e);
}
static void export_view(bool lscan)
{
    char path[128], msg[96];
    snprintf(path, sizeof(path), "%s/export/%s.%s", ls_freqs_root(),
             ls_freqs_which() >= 0 ? ls_freqs_bank(ls_freqs_which()) : ls_freqs_which() == LS_FREQS_ALL ? "all" : "builtin",
             lscan ? "lscan" : "csv");
    ls_freqs_export(s_idx, s_n, path, lscan, msg, sizeof(msg));
    say(msg);
}
/* LOAD INTO SCANNER: the NFM and P25 entries in view become the FM
   scanner's channel list, through the same .lscan the scanner imports. */
static void load_scanner(void)
{
    char path[128], msg[96];
    snprintf(path, sizeof(path), "%s/export/scanner.lscan", ls_freqs_root());
    if (!ls_freqs_export(s_idx, s_n, path, true, msg, sizeof(msg))) { say(msg); return; }
    scan_import_file(path, msg, sizeof(msg));
    say(msg);
}
static void options_done(int i)
{
    const ls_freq_t *e = selected();
    switch (i) {
    case O_SORT: s_f.sort = (uint8_t)((s_f.sort + 1) % LS_FS__COUNT); changed(); break;
    case O_RENAME: if (e) ls_keyboard_open("NAME", e->name, LS_FREQ_NAME - 1, rename_done); break;
    case O_MODE:
        if (!e) break;
        ls_picker_open("MODE", remode_done);
        for (int m = 0; m < LS_FM__COUNT; m++) ls_picker_add(ls_freq_mode_name(m), "");
        ls_picker_select(e->mode);
        break;
    case O_FREQ: if (e) ls_numpad_open("FREQUENCY", "MHz", e->hz / 1e6, refreq_done); break;
    case O_COPY: if (e) { s_new = *e; snprintf(s_new.group, sizeof(s_new.group), "MINE"); saved(); } break;
    case O_DELETE:
        if (!e) break;
        if (armed(O_DELETE)) { s_armed = -1; edit_with(NULL); }
        else arm(O_DELETE, LS_TEXT(t_scr_freqs_delete_again, "DELETE again within 6 s removes it from its file"));
        break;
    case O_SCANNER:
        if (armed(O_SCANNER)) { s_armed = -1; load_scanner(); }
        else arm(O_SCANNER, LS_TEXT(t_scr_freqs_scanner_again, "Again to REPLACE the FM scanner's list with the NFM/P25 in view"));
        break;
    case O_CSV: export_view(false); break;
    case O_LSCAN: export_view(true); break;
    case O_RELOAD: ls_freqs_scan(); ls_freqs_load(ls_freqs_which()); say("Reading the card again"); break;
    case O_CLEAR:
        s_f.band = LS_FB_ALL; s_f.group[0] = s_f.find[0] = 0; s_f.near = false;
        changed();
        break;
    default: break;
    }
}
static void open_options(void)
{
    const ls_freq_t *e = selected();
    const bool file = e && e->bank != LS_FREQ_BANK_BUILTIN;
    const char *no = e ? "built-in: COPY first" : "nothing selected";
    ls_picker_open("OPTIONS", options_done);
    ls_picker_add("SORT", ls_freq_sort_name(s_f.sort));
    ls_picker_add("RENAME", file ? e->name : no);
    ls_picker_add("CHANGE MODE", file ? ls_freq_mode_name(e->mode) : no);
    ls_picker_add("CHANGE FREQUENCY", file ? "type it" : no);
    ls_picker_add("COPY TO MINE", e ? "keep it in mine.csv" : no);
    ls_picker_add("DELETE", file ? "from its file, asks twice" : no);
    ls_picker_add("LOAD INTO SCANNER", "replaces FM's scan list");
    ls_picker_add("EXPORT VIEW: CSV", "freqs/export");
    ls_picker_add("EXPORT VIEW: SCANNER", "freqs/export, .lscan");
    ls_picker_add("RELOAD THE CARD", "after editing files");
    ls_picker_add("CLEAR FILTERS", "band, group, near, find");
}

static void action(int i)
{
    if (i == 0) tune();
    else if (i == 1) open_save();
    else if (i == 2) open_bank();
    else if (i == 3) open_options();
}

static void move(int d)
{
    s_free = false;
    s_sel += d;
    if (s_sel < 0) s_sel = 0;
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
}

static bool key(ls_tk_t k, char ch)
{
#if LS_TUI_HAS_COMPACT
    if (ls_tui_compact()) return ls_freqs_desk_key(k, ch);
#endif
    /* The wheel is the desk layout's; here it is the arrows it always was. */
    if (k == LS_TK_WHEEL_UP) k = LS_TK_UP;
    if (k == LS_TK_WHEEL_DOWN) k = LS_TK_DOWN;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if (k == LS_TK_UP) { move(-1); button_focus = -1; return true; }
    if (k == LS_TK_DOWN) { move(1); button_focus = -1; return true; }
    if (k == LS_TK_F5) { move(-(s_rows > 1 ? s_rows - 1 : 1)); return true; }
    if (k == LS_TK_F6) { move(s_rows > 1 ? s_rows - 1 : 1); return true; }
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT || k == LS_TK_TAB) return ls_btn_navigate(k, &button_slot, &button_focus, false);
    if (k == LS_TK_ENTER) {
        if (button_focus >= 0) { if (ls_btn_enabled(0, button_focus)) action(button_focus); }
        else tune();
        return true;
    }
    if (k == LS_TK_ESC && s_f.find[0]) { s_f.find[0] = 0; changed(); return true; }
    if (k != LS_TK_CHAR) return false;
    const char *p = strchr("tsko", ch);
    if (p) { action((int)(p - "tsko")); return true; }
    p = strchr("bgnf", ch);
    if (p) { filter_action((int)(p - "bgnf")); return true; }
    return false;
}

static bool inside(tui_rect r, int col, int row)
{
    return col >= r.x && col < r.x + r.w && row >= r.y && row < r.y + r.h;
}

static bool touch(int col, int row)
{
#if LS_TUI_HAS_COMPACT
    if (ls_tui_compact()) return ls_freqs_desk_touch(col, row);
#endif
    int b = ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
    if (b >= 0) { if (ls_btn_enabled(0, b)) action(b); return true; }
    b = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
    if (b >= 0) { filter_action(b); return true; }
    if (inside(s_scroll, col, row)) {
        move(row < s_scroll.y + s_scroll.h / 2 ? -(s_rows > 1 ? s_rows - 1 : 1) : (s_rows > 1 ? s_rows - 1 : 1));
        return true;
    }
    if (inside(s_list, col, row)) {
        if (ls_drag_scroll_tap_is_lift(&s_drag)) return true;   /* the end of a drag, not a choice */
        s_free = false;
        const int k = s_first + (row - s_list.y) / (s_per ? s_per : 1);
        if (k >= 0 && k < s_n) {
            if (k == s_sel) tune();
            else s_sel = k;
        }
        return true;
    }
    /* The big frequency is the entry: tapping it tunes it. */
    if (inside(s_head, col, row) && row > s_head.y + 1 && row < s_head.y + 7) { tune(); return true; }
    return true;
}

static void enter(void)
{
    button_focus = -1;
    button_slot = 0;
    s_armed = -1;
    ls_freqs_scan();
    if (!s_restored) { s_restored = true; restore_view(); }
    else if (ls_freqs_is_dirty() || !ls_freqs_count()) ls_freqs_load(ls_freqs_which());
    s_stale = true;
#if LS_TUI_HAS_COMPACT
    ls_freqs_desk_enter();
#endif
}

const ls_tui_screen_t ls_scr_freqs = {
    .name = "FREQS",
    .hint = "T tune  S save  K bank  O options  B band  G group  N near  F find",
    .enter = enter, .draw = draw, .key = key, .touch = touch,
    .wheel = true,
};
