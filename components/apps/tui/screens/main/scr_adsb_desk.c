/* ADS-B as a desktop program: the aircraft in a table.

       Receiver  View  Aircraft                       1090 LR2021  5 aircraft
       [Radio] [Options] [Follow NEAREST] [Zoom+] [Zoom-] [Full map] ...
       Find [......]  [Alarms first] [Positions only]
       +AIRCRAFT 5 of 5---- ! N911MD EMERGENCY --+ +MAP---------------+
       | ! CALL  ICAO  ALT  V/S SPD HDG  NM BRG  | |  the handheld's  |
       | rows, sorted by any column              | |  map, followed   |
       +TRAFFIC----------------------------------+ +THIS ONE----------+
       | messages a second, the last two minutes | | every field, and |
       +-----------------------------------------+ | its altitude     |
       status                                      +------------------+

   The tracker, the map, the traffic strip, the aircraft's own page and
   every letter key are the handheld's (scr_adsb.c); a row's order and text
   are ls_adsb_desk.c; the widgets are ls_deskui.c. An aircraft declaring an
   emergency is red and, unless told otherwise, at the top. */
#include "scr_adsb_desk.h"

#if LS_TUI_HAS_COMPACT

#include "../../ls_deskui.h"
#include "../../ls_adsb_desk.h"
#include "../../ls_box.h"
#include "../../ls_geo.h"
#include "../../ls_input_ev.h"
#include "../../ls_tui_touch.h"
#include "apps/adsb/adsb_app.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32                   /* the bench on Windows */
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0775)
#endif

#define HUE TUI_CYAN
#define WHITE TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define INK(h) TUI_ATTR((h) | TUI_BRIGHT, TUI_BLACK)
#define SOFT(h) TUI_ATTR((h), TUI_BLACK)

static EXT_RAM_BSS_ATTR ls_adsbd_row_t s_row[ADSB_MAX_TRACKED];
static EXT_RAM_BSS_ATTR uint8_t s_order[ADSB_MAX_TRACKED], s_view[ADSB_MAX_TRACKED];
static EXT_RAM_BSS_ATTR int s_nrow, s_n, s_sel;          /* tracked, shown, the cursor's row in the view (-1 none) */
static EXT_RAM_BSS_ATTR int s_sort;                      /* a column, or -1 for the tracker's own order */
static EXT_RAM_BSS_ATTR bool s_desc, s_alarms_first, s_only_pos;
static EXT_RAM_BSS_ATTR bool s_free;                     /* the wheel moved the window off the cursor */
static EXT_RAM_BSS_ATTR bool s_classic, s_show_map, s_show_detail, s_show_dash;
static EXT_RAM_BSS_ATTR bool s_find_focus;
static EXT_RAM_BSS_ATTR ls_dui_edit_t s_find;
static EXT_RAM_BSS_ATTR uint32_t s_shown;

static const ls_dui_col_t COLS[LS_AC__COUNT] = {
    [LS_AC_ALARM]  = { "!",        1, 1, 0, 0 },
    [LS_AC_CALL]   = { "CALLSIGN", 9, 9, 0, 0 },
    [LS_AC_ICAO]   = { "ICAO",     6, 6, 0, 0 },
    [LS_AC_ALT]    = { "ALT ft",   7, 7, LS_DUI_RIGHT, 0 },
    [LS_AC_VS]     = { "V/S",      6, 6, LS_DUI_RIGHT, 4 },
    [LS_AC_SPD]    = { "KT",       4, 4, LS_DUI_RIGHT, 0 },
    [LS_AC_HDG]    = { "HDG",      4, 4, LS_DUI_RIGHT, 2 },
    [LS_AC_DIST]   = { "NM",       6, 6, LS_DUI_RIGHT, 0 },
    [LS_AC_BRG]    = { "BRG",      3, 3, LS_DUI_RIGHT, 5 },
    [LS_AC_SQUAWK] = { "SQWK",     4, 4, 0, 3 },
    [LS_AC_SRC]    = { "SRC",      5, 5, 0, 7 },
    [LS_AC_AGE]    = { "SEEN",     4, 4, LS_DUI_RIGHT, 1 },
    [LS_AC_MSGS]   = { "MSGS",     6, 6, LS_DUI_RIGHT, 6 },
};

static EXT_RAM_BSS_ATTR ls_dui_table_t s_t;
static EXT_RAM_BSS_ATTR ls_dui_menu_t s_menu;
static EXT_RAM_BSS_ATTR ls_dui_hits_t s_hits;
static EXT_RAM_BSS_ATTR tui_rect s_area, s_r_menu, s_r_tools, s_r_filter, s_r_table, s_r_dash, s_r_map, s_r_detail,
                                 s_r_status, s_r_find;
static EXT_RAM_BSS_ATTR int16_t s_bar_x[6];
static EXT_RAM_BSS_ATTR char s_say[96], s_dir[96];
static EXT_RAM_BSS_ATTR int64_t s_said_us, s_now;

enum {
    C_RADIO, C_OPTIONS, C_FOLLOW, C_ZOOM_IN, C_ZOOM_OUT, C_FULL_MAP, C_HOME, C_MAP_ONLY, C_WEATHER, C_EXPORT,
    C_FIND, C_ALARMS, C_ONLY_POS, C_CLEAR, C_PANE_MAP, C_PANE_DETAIL, C_PANE_DASH, C_CLASSIC,
    C_SORT_TABLE, C_SORT_NEAR, C_SORT_LOW, C_FLIP,
    C_PAGE, C_MAP_AT, C_COPY_CALL, C_COPY_ICAO, C_COPY_POS, C_COPY_ROW,
    C_COL = 100,            /* + column: show or hide */
    C_SORT = 120,           /* + column: sort by it */
};

static const char *const MENUS[] = { "Receiver", "View", "Aircraft" };
#define NMENUS 3

static void run(int cmd);

static void say(const char *text) { snprintf(s_say, sizeof(s_say), "%s", text); s_said_us = esp_timer_get_time(); }

bool ls_adsb_desk_classic(void) { return s_classic; }
void ls_adsb_desk_export_dir(const char *dir) { snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : ""); }

/* ------------------------------------------------------------------ data -- */

static const ls_adsbd_row_t *at(int k) { return k >= 0 && k < s_n ? &s_row[s_view[k]] : NULL; }
static const ls_adsbd_row_t *selected(void) { return at(s_sel); }

static void refresh(int64_t now)
{
    double lat = 0, lon = 0;
    const bool here = ls_scr_adsb_here(&lat, &lon);
    s_now = now;
    s_nrow = 0;
    for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
        const adsb_aircraft_t *a = adsb_state_get(slot);
        if (!a || !a->active) continue;
        ls_adsbd_row_t *r = &s_row[s_nrow++];
        memset(r, 0, sizeof(*r));
        r->slot = (int8_t)slot;
        r->key = adsb_state_key(a);
        r->icao = a->icao;
        snprintf(r->call, sizeof(r->call), "%.8s", a->callsign);
        snprintf(r->src, sizeof(r->src), "%.5s", adsb_state_source_label(a, now));
#define FIELD(bit, valid, ts) do { if (valid) { r->have |= (bit); if (now - (ts) > ADSB_FIELD_STALE_US) r->stale |= (bit); } } while (0)
        FIELD(LS_AF_ALT, a->altitude_valid, a->altitude_ts_us);
        FIELD(LS_AF_SPD, a->velocity_valid, a->velocity_ts_us);
        FIELD(LS_AF_HDG, a->heading_valid, a->heading_ts_us);
        FIELD(LS_AF_VS, a->vert_rate_valid, a->vert_rate_ts_us);
        FIELD(LS_AF_POS, a->pos_valid, a->pos_ts_us);
#undef FIELD
        r->alt = a->altitude;
        r->vs = a->vert_rate;
        r->spd = (int16_t)a->velocity;
        r->hdg = (int16_t)a->heading;
        r->lat = a->lat;
        r->lon = a->lon;
        r->nm = r->brg = -1;
        if (here && a->pos_valid) {
            double brg = 0, m = 0;
            ls_geo_bearing_range(lat, lon, a->lat, a->lon, &brg, &m);
            r->nm = (float)(m / LS_GEO_M_PER_NM);
            r->brg = (float)brg;
        }
        r->squawk = (int16_t)adsb_state_squawk(slot);
        r->alarm = ls_scr_adsb_alarm(slot, now);
        r->age_s = (int32_t)((now - a->last_seen_us) / 1000000);
        if (r->age_s < 0) r->age_s = 0;
        r->msgs = (uint32_t)a->msg_count;
    }
    ls_adsbd_sort(s_order, s_row, s_nrow, s_sort < 0 ? LS_AC__COUNT : s_sort, s_desc, s_alarms_first);
    s_n = 0;
    for (int i = 0; i < s_nrow; i++) {
        const ls_adsbd_row_t *r = &s_row[s_order[i]];
        if (s_only_pos && !(r->have & LS_AF_POS)) continue;
        if (!ls_adsbd_match(r, s_find.text)) continue;
        s_view[s_n++] = s_order[i];
    }
    /* The cursor is the tracker's selection, wherever the order put it. */
    const uint32_t key = adsb_select_get_icao();
    s_sel = -1;
    for (int k = 0; k < s_n && s_sel < 0; k++)
        if (s_row[s_view[k]].key == key) s_sel = k;
}

static void move_to(int k)
{
    if (!s_n) return;
    if (k < 0) k = 0;
    if (k >= s_n) k = s_n - 1;
    s_free = false;
    s_sel = k;
    adsb_select_set_icao(s_row[s_view[k]].key);
}

/* ----------------------------------------------------------------- menus -- */

static void build_menu(int which)
{
    char text[44];
    const ls_adsbd_row_t *r = selected();
    const int none = r ? 0 : LS_DUI_OFF;
    ls_dui_menu_begin(&s_menu);
    switch (which) {
    case 0:
        ls_dui_menu_item(&s_menu, "Radio...", "R", C_RADIO, 0);
        ls_dui_menu_item(&s_menu, "Options...", "O", C_OPTIONS, 0);
        ls_dui_menu_item(&s_menu, "FIS-B weather", "W", C_WEATHER, 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Save the table as CSV", "Ctrl+E", C_EXPORT, s_n ? 0 : LS_DUI_OFF);
        break;
    case 1:
        ls_dui_menu_item(&s_menu, "As tracked", NULL, C_SORT_TABLE, s_sort < 0 ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Nearest first", NULL, C_SORT_NEAR, s_sort == LS_AC_DIST && !s_desc ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Lowest first", NULL, C_SORT_LOW, s_sort == LS_AC_ALT && !s_desc ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Turn the order round", NULL, C_FLIP, s_sort < 0 ? LS_DUI_OFF : 0);
        ls_dui_menu_item(&s_menu, "Emergencies at the top", NULL, C_ALARMS, s_alarms_first ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Only aircraft with a position", NULL, C_ONLY_POS, s_only_pos ? LS_DUI_CHECK : 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Map", NULL, C_PANE_MAP, s_show_map ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Details", NULL, C_PANE_DETAIL, s_show_detail ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Traffic", NULL, C_PANE_DASH, s_show_dash ? LS_DUI_CHECK : 0);
        ls_dui_menu_rule(&s_menu);
        snprintf(text, sizeof(text), "Follow: %s", ls_scr_adsb_follow());
        ls_dui_menu_item(&s_menu, text, "F", C_FOLLOW, 0);
        ls_dui_menu_item(&s_menu, "Zoom in", "+", C_ZOOM_IN, 0);
        ls_dui_menu_item(&s_menu, "Zoom out", "-", C_ZOOM_OUT, 0);
        ls_dui_menu_item(&s_menu, "Map only", "V", C_MAP_ONLY, 0);
        ls_dui_menu_item(&s_menu, "Full MAP at home", "M", C_FULL_MAP, 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Handheld layout", "Ctrl+L", C_CLASSIC, 0);
        break;
    default:
        if (r) { ls_adsbd_cell(r, LS_AC_ICAO, text, sizeof(text)); ls_dui_menu_item(&s_menu, r->call[0] ? r->call : text, NULL, -1, LS_DUI_HEAD); }
        else ls_dui_menu_item(&s_menu, "NO AIRCRAFT CHOSEN", NULL, -1, LS_DUI_HEAD);
        ls_dui_menu_item(&s_menu, "Its own page", "Enter", C_PAGE, none);
        ls_dui_menu_item(&s_menu, "Show on MAP", NULL, C_MAP_AT, r && (r->have & LS_AF_POS) ? 0 : LS_DUI_OFF);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Copy callsign", "Ctrl+C", C_COPY_CALL, r && r->call[0] ? 0 : LS_DUI_OFF);
        ls_dui_menu_item(&s_menu, "Copy ICAO address", NULL, C_COPY_ICAO, none);
        ls_dui_menu_item(&s_menu, "Copy position", NULL, C_COPY_POS, r && (r->have & LS_AF_POS) ? 0 : LS_DUI_OFF);
        ls_dui_menu_item(&s_menu, "Copy the row as CSV", NULL, C_COPY_ROW, none);
        break;
    }
}

static void open_menu(int which)
{
    if (which < 0) which = NMENUS - 1;
    if (which >= NMENUS) which = 0;
    build_menu(which);
    ls_dui_menu_open(&s_menu, s_bar_x[which], s_r_menu.y + 1, which, HUE);
}

static void row_menu(int x, int y)
{
    build_menu(2);
    ls_dui_menu_open(&s_menu, x, y, -1, HUE);
}

static void head_menu(int x, int y)
{
    ls_dui_menu_begin(&s_menu);
    ls_dui_menu_item(&s_menu, "COLUMNS", NULL, -1, LS_DUI_HEAD);
    for (int c = 0; c < LS_AC__COUNT; c++)
        ls_dui_menu_item(&s_menu, c == LS_AC_ALARM ? "! emergency" : COLS[c].title, NULL, C_COL + c,
                         ((s_shown >> c) & 1 ? LS_DUI_CHECK : 0) | (COLS[c].drop ? 0 : LS_DUI_OFF));
    ls_dui_menu_open(&s_menu, x, y, -1, HUE);
}

/* --------------------------------------------------------------- actions -- */

static void copy(const char *text)
{
    char line[96];
    ls_dui_clip_set(text);
    snprintf(line, sizeof(line), "Copied %.80s", text);
    say(line);
}

static void export_csv(void)
{
    char path[160], line[200];
    const char *dir = s_dir[0] ? s_dir : "/sdcard/adsb";
    make_dir(dir);
    snprintf(path, sizeof(path), "%s/aircraft-%lu.csv", dir, (unsigned long)time(NULL));
    FILE *f = fopen(path, "w");
    if (!f) { say("No card to save to"); return; }
    fprintf(f, "%s\n", LS_ADSBD_CSV_HEAD);
    for (int k = 0; k < s_n; k++) {
        ls_adsbd_csv(at(k), line, sizeof(line));
        fprintf(f, "%s\n", line);
    }
    fclose(f);
    snprintf(line, sizeof(line), "Saved %d aircraft to %.40s", s_n, strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    say(line);
}

static void sort_by(int col)
{
    if (s_sort == col) {
        /* A third click on a header is the tracker's own order again. */
        if (s_desc) { s_sort = -1; s_desc = false; }
        else s_desc = true;
    } else { s_sort = col; s_desc = false; }
    ls_scr_adsb_sort_set(s_sort == LS_AC_DIST && !s_desc ? 1 : s_sort == LS_AC_ALT && !s_desc ? 2 : 0);
    s_free = false;
}

static void run(int cmd)
{
    char text[200];
    const ls_adsbd_row_t *r = selected();
    s_find_focus = false;
    if (cmd >= C_SORT) { if (cmd - C_SORT < LS_AC__COUNT) sort_by(cmd - C_SORT); return; }
    if (cmd >= C_COL) { if (cmd - C_COL < LS_AC__COUNT) s_shown ^= 1u << (cmd - C_COL); return; }
    switch (cmd) {
    case C_RADIO: ls_scr_adsb_letter('r'); break;
    case C_OPTIONS: ls_scr_adsb_letter('o'); break;
    case C_FOLLOW: ls_scr_adsb_letter('f'); break;
    case C_ZOOM_IN: ls_scr_adsb_letter('='); break;
    case C_ZOOM_OUT: ls_scr_adsb_letter('-'); break;
    case C_FULL_MAP: ls_scr_adsb_letter('m'); break;
    case C_HOME: ls_scr_adsb_letter('h'); break;
    case C_MAP_ONLY: ls_scr_adsb_letter('v'); break;
    case C_WEATHER: ls_scr_adsb_letter('w'); break;
    case C_EXPORT: export_csv(); break;
    case C_FIND: s_find_focus = true; s_find.caret = s_find.len; break;
    case C_ALARMS: s_alarms_first = !s_alarms_first; break;
    case C_ONLY_POS: s_only_pos = !s_only_pos; break;
    case C_CLEAR: ls_dui_edit_set(&s_find, "", 16); s_only_pos = false; break;
    case C_PANE_MAP: s_show_map = !s_show_map; break;
    case C_PANE_DETAIL: s_show_detail = !s_show_detail; break;
    case C_PANE_DASH: s_show_dash = !s_show_dash; break;
    case C_CLASSIC: s_classic = true; s_menu.pop.open = false; break;
    case C_SORT_TABLE: s_sort = -1; s_desc = false; ls_scr_adsb_sort_set(0); break;
    case C_SORT_NEAR: s_sort = LS_AC_DIST; s_desc = false; ls_scr_adsb_sort_set(1); break;
    case C_SORT_LOW: s_sort = LS_AC_ALT; s_desc = false; ls_scr_adsb_sort_set(2); break;
    case C_FLIP: if (s_sort >= 0) s_desc = !s_desc; break;
    case C_PAGE: if (r) ls_scr_adsb_page(true); break;
    case C_MAP_AT: if (r && (r->have & LS_AF_POS)) ls_scr_adsb_map_at(r->lat, r->lon); break;
    case C_COPY_CALL:
        if (r && r->call[0]) copy(r->call);
        else if (r) { ls_adsbd_cell(r, LS_AC_ICAO, text, sizeof(text)); copy(text); }
        break;
    case C_COPY_ICAO: if (r) { ls_adsbd_cell(r, LS_AC_ICAO, text, sizeof(text)); copy(text); } break;
    case C_COPY_POS: if (r && (r->have & LS_AF_POS)) { snprintf(text, sizeof(text), "%.5f, %.5f", (double)r->lat, (double)r->lon); copy(text); } break;
    case C_COPY_ROW: if (r) { ls_adsbd_csv(r, text, sizeof(text)); copy(text); } break;
    default: break;
    }
}

/* ---------------------------------------------------------------- layout -- */

static void layout(tui_rect a)
{
    const tui_rect none = tui_rect_make(0, 0, 0, 0);
    int y = a.y;
    s_r_menu = tui_rect_make(a.x, y++, a.w, 1);
    s_r_tools = a.h >= 18 ? tui_rect_make(a.x, y++, a.w, 1) : none;
    s_r_filter = tui_rect_make(a.x, y++, a.w, 1);
    s_r_status = tui_rect_make(a.x, a.y + a.h - 1, a.w, 1);
    const tui_rect body = tui_rect_make(a.x, y, a.w, a.y + a.h - 1 - y);
    /* The right-hand column: the map over the details, when there is room
       for either beside a table that can still be read. */
    int rw = 0;
    if ((s_show_map || s_show_detail) && a.w >= 104) { rw = a.w * 2 / 5; if (rw > 70) rw = 70; if (rw < 40) rw = 40; }
    const int lw = body.w - (rw ? rw - 1 : 0);
    int dash_h = s_show_dash && body.h >= 26 ? 6 : 0;
    int strip_h = 0;                    /* no column: the details in a strip under the table */
    if (!rw && s_show_detail && body.h >= 22) { strip_h = 7; dash_h = 0; }
    const int th = body.h - (dash_h ? dash_h - 1 : 0) - (strip_h ? strip_h - 1 : 0);
    s_r_table = tui_rect_make(body.x, body.y, lw, th);
    s_r_dash = dash_h ? tui_rect_make(body.x, body.y + th - 1, lw, dash_h) : none;
    s_r_map = s_r_detail = none;
    if (rw) {
        const int rx = body.x + body.w - rw;
        int dh = s_show_detail ? (s_show_map ? (body.h >= 34 ? 17 : 12) : body.h) : 0;
        if (s_show_map && body.h - dh < 12) dh = 0;
        const int mh = s_show_map ? body.h - (dh ? dh - 1 : 0) : 0;
        if (mh) s_r_map = tui_rect_make(rx, body.y, rw, mh);
        if (dh) s_r_detail = tui_rect_make(rx, mh ? body.y + mh - 1 : body.y, rw, dh);
    } else if (strip_h) s_r_detail = tui_rect_make(body.x, body.y + th - 1, lw, strip_h);
    ls_dui_table_layout(&s_t, tui_rect_make(s_r_table.x + 1, s_r_table.y + 1, s_r_table.w - 2, s_r_table.h - 2), COLS,
                        LS_AC__COUNT, s_shown | (1u << LS_AC_ALARM) | (1u << LS_AC_CALL) | (1u << LS_AC_ICAO) |
                        (1u << LS_AC_ALT) | (1u << LS_AC_SPD) | (1u << LS_AC_DIST));
}

/* ------------------------------------------------------------------ draw -- */

static void draw_menu(tui_surface *sf)
{
    ls_dui_bar_draw(sf, s_r_menu, MENUS, NMENUS, s_menu.pop.open ? s_menu.pop.bar : -1, HUE, s_bar_x);
    char text[80];
    const int n = adsb_state_active_count();
    snprintf(text, sizeof(text), "%s  %d aircraft", adsb_active_source_name(), n);
    const int x = s_r_menu.x + s_r_menu.w - (int)strlen(text) - 1;
    if (x > s_bar_x[NMENUS] + 2) tui_put_str(sf, s_r_menu, x, s_r_menu.y, text, n ? INK(TUI_GREEN) : LS_ATTR_DIM);
}

static void draw_tools(tui_surface *sf)
{
    const tui_rect r = s_r_tools;
    if (!r.h) return;
    char label[32];
    int x = r.x + 1;
    x += ls_dui_tool(sf, r, x, "Radio", C_RADIO, false, false, TUI_GREEN, &s_hits);
    x += ls_dui_tool(sf, r, x, "Options", C_OPTIONS, false, false, TUI_BLUE, &s_hits);
    x += 1;
    snprintf(label, sizeof(label), "Follow %s", ls_scr_adsb_follow());
    x += ls_dui_tool(sf, r, x, label, C_FOLLOW, false, false, TUI_YELLOW, &s_hits);
    x += ls_dui_tool(sf, r, x, "Zoom+", C_ZOOM_IN, false, false, TUI_YELLOW, &s_hits);
    x += ls_dui_tool(sf, r, x, "Zoom-", C_ZOOM_OUT, false, false, TUI_YELLOW, &s_hits);
    x += ls_dui_tool(sf, r, x, "Map only", C_MAP_ONLY, false, false, HUE, &s_hits);
    x += ls_dui_tool(sf, r, x, "Full map", C_FULL_MAP, false, false, HUE, &s_hits);
    x += 1;
    x += ls_dui_tool(sf, r, x, "Weather", C_WEATHER, false, false, TUI_MAGENTA, &s_hits);
    x += ls_dui_tool(sf, r, x, "Save CSV", C_EXPORT, false, !s_n, TUI_GREEN, &s_hits);
}

static void draw_filter(tui_surface *sf)
{
    const tui_rect r = s_r_filter;
    int x = r.x + 1;
    tui_put_str(sf, r, x, r.y, "Find", s_find_focus ? INK(TUI_YELLOW) : LS_ATTR_DIM);
    x += 5;
    s_r_find = tui_rect_make(x, r.y, 18, 1);
    ls_dui_edit_draw(sf, r, x, r.y, 18, &s_find, s_find_focus, TUI_YELLOW);
    x += 20;
    x += ls_dui_tool(sf, r, x, "Emergencies first", C_ALARMS, s_alarms_first, false, TUI_RED, &s_hits);
    x += ls_dui_tool(sf, r, x, "Positions only", C_ONLY_POS, s_only_pos, false, TUI_YELLOW, &s_hits);
    if (s_find.len || s_only_pos) x += ls_dui_tool(sf, r, x, "Clear", C_CLEAR, false, false, TUI_WHITE, &s_hits);
    /* How the table is ordered, said in words at the right. */
    char text[48];
    if (s_sort < 0) snprintf(text, sizeof(text), "as tracked");
    else snprintf(text, sizeof(text), "by %s%s", s_sort == LS_AC_ALARM ? "emergency" : COLS[s_sort].title, s_desc ? ", turned round" : "");
    const int tx = r.x + r.w - (int)strlen(text) - 1;
    if (tx > x + 1) tui_put_str(sf, r, tx, r.y, text, LS_ATTR_DIM);
}

static uint8_t alt_hue(int32_t ft) { return ft < 10000 ? TUI_GREEN : ft < 25000 ? TUI_YELLOW : TUI_CYAN; }

static void draw_table(tui_surface *sf)
{
    char title[40], text[32];
    snprintf(title, sizeof(title), "AIRCRAFT %d of %d", s_n, s_nrow);
    ls_dui_pane(sf, s_r_table, title, "* not heard lately", HUE, !s_find_focus);
    /* An emergency is said on the frame itself, whatever the table shows. */
    for (int i = 0; i < s_nrow; i++) {
        const ls_adsbd_row_t *r = &s_row[s_order[i]];
        if (!r->alarm) continue;
        char who[10];
        if (r->call[0]) snprintf(who, sizeof(who), "%s", r->call);
        else ls_adsbd_cell(r, LS_AC_ICAO, who, sizeof(who));
        snprintf(title, sizeof(title), " ! %s %s ", who, r->alarm);
        const int x = s_r_table.x + 22;
        if (x + (int)strlen(title) < s_r_table.x + s_r_table.w - 22)
            tui_put_str(sf, s_r_table, x, s_r_table.y, title, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED));
        break;
    }
    ls_dui_table_head(sf, &s_t, COLS, LS_AC__COUNT, s_sort, s_desc, HUE);
    if (!s_n) {
        const tui_rect b = s_t.body;
        if (s_nrow) {
            tui_put_str(sf, b, b.x + 2, b.y + 1, "Nothing matches the filters.", LS_ATTR_DIM);
            tui_put_str(sf, b, b.x + 2, b.y + 3, "Esc clears Find; Clear drops every filter.", LS_ATTR_DIM);
            return;
        }
        tui_put_str(sf, b, b.x + 2, b.y + 1, adsb_receiver_status(), LS_ATTR_DIM);
        static const char *const HOW[] = {
            "Aircraft show here as they are heard on 1090 MHz;",
            "the map beside plots the ones that send a position.",
            "",
            "Hear further: a vertical antenna about 6.9 cm long,",
            "as high and as clear of walls as it can go.",
            "Radio picks the receiver; Follow keeps the map on the traffic.",
        };
        for (int i = 0; i < 6 && b.y + 3 + i < b.y + b.h; i++)
            tui_put_str(sf, b, b.x + 2, b.y + 3 + i, HOW[i], i == 5 ? INK(TUI_YELLOW) : TUI_ATTR(TUI_WHITE, TUI_BLACK));
        return;
    }
    if (!s_free && s_sel >= 0) ls_dui_table_follow(&s_t, s_sel, s_n);
    else ls_dui_table_clamp(&s_t, s_n);
    for (int row = 0; row < s_t.rows && s_t.first + row < s_n; row++) {
        const int k = s_t.first + row, y = s_t.body.y + row;
        const ls_adsbd_row_t *r = at(k);
        const bool on = k == s_sel, alarm = r->alarm != NULL, pos = r->have & LS_AF_POS;
        const tui_rect line = tui_rect_make(s_t.body.x, y, s_t.body.w, 1);
        if (alarm) tui_fill(sf, line, ' ', TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED));
        else if (on) ls_fill_dither(sf, line, LS_DITHER_MEDIUM, HUE);
        if (on) ls_box_put(sf, s_t.body, s_t.body.x, y, LS_SHAPE_RIGHT, alarm ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED) : WHITE);
        /* On red everything is white; otherwise each column has its own ink. */
#define CELL(col, attr) do { ls_adsbd_cell(r, (col), text, sizeof(text)); \
        ls_dui_table_cell(sf, &s_t, COLS, (col), row, text, alarm ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED) : (attr)); } while (0)
        CELL(LS_AC_ALARM, INK(TUI_RED));
        CELL(LS_AC_CALL, r->call[0] ? (on || pos ? WHITE : TUI_ATTR(TUI_WHITE, TUI_BLACK)) : LS_ATTR_FAINT);
        CELL(LS_AC_ICAO, on ? INK(HUE) : SOFT(HUE));
        CELL(LS_AC_ALT, r->stale & LS_AF_ALT ? LS_ATTR_DIM : INK(alt_hue(r->alt)));
        CELL(LS_AC_VS, r->vs > 250 ? INK(TUI_GREEN) : r->vs < -250 ? INK(TUI_YELLOW) : LS_ATTR_DIM);
        CELL(LS_AC_SPD, r->stale & LS_AF_SPD ? LS_ATTR_DIM : TUI_ATTR(TUI_WHITE, TUI_BLACK));
        CELL(LS_AC_HDG, r->stale & LS_AF_HDG ? LS_ATTR_DIM : SOFT(TUI_MAGENTA));
        CELL(LS_AC_DIST, r->stale & LS_AF_POS ? LS_ATTR_DIM : INK(TUI_YELLOW));
        CELL(LS_AC_BRG, SOFT(TUI_YELLOW));
        CELL(LS_AC_SQUAWK, INK(TUI_MAGENTA));
        CELL(LS_AC_SRC, SOFT(TUI_BLUE));
        CELL(LS_AC_AGE, r->age_s > 15 ? INK(TUI_YELLOW) : LS_ATTR_DIM);
        CELL(LS_AC_MSGS, LS_ATTR_DIM);
#undef CELL
    }
    ls_dui_table_strip(sf, &s_t, s_n, HUE);
}

static int kv(tui_surface *sf, tui_rect in, int x, int y, const char *label, const char *value, uint8_t at)
{
    if (y >= in.y + in.h) return y;
    tui_put_str(sf, in, x, y, label, LS_ATTR_DIM);
    tui_put_str(sf, in, x + 10, y, value, at);
    return y + 1;
}

static void draw_detail(tui_surface *sf)
{
    const tui_rect r = s_r_detail;
    if (!r.w) return;
    const ls_adsbd_row_t *row = selected();
    const adsb_aircraft_t *a = row ? adsb_state_get(row->slot) : NULL;
    char text[64], cell[24];
    const tui_rect in = ls_dui_pane(sf, r, "THIS ONE", row ? row->src : "", row && row->alarm ? TUI_RED : HUE, false);
    if (!row || !a) {
        tui_put_str(sf, in, in.x + 1, in.y, s_nrow ? "Click a row, or Up and Down." : "No aircraft yet.", LS_ATTR_DIM);
        return;
    }
    /* Two columns of fields when the pane is a strip under the table. */
    const bool two = in.w >= 76 && in.h < 9;
    const int x0 = in.x + 1, x1 = two ? in.x + in.w / 2 : x0;
    int y = in.y;
    ls_adsbd_cell(row, LS_AC_ICAO, cell, sizeof(cell));
    tui_put_str(sf, in, x0, y, row->call[0] ? row->call : "-", row->call[0] ? WHITE : LS_ATTR_DIM);
    tui_put_str(sf, in, x0 + 10, y, cell, INK(HUE));
    if (row->alarm) {
        snprintf(text, sizeof(text), " %s ", row->alarm);
        tui_put_str(sf, in, x0 + 18, y, text, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED));
    } else if (!row->call[0]) tui_put_str(sf, in, x0 + 18, y, "no callsign heard yet", LS_ATTR_DIM);
    y++;
    int yl = y, yr = two ? y : 0;
    if (ls_scr_adsb_category(a, text, sizeof(text))) yl = kv(sf, in, x0, yl, "category", text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    else yl = kv(sf, in, x0, yl, "category", "not sent yet", LS_ATTR_DIM);
    if (row->squawk) { ls_adsbd_cell(row, LS_AC_SQUAWK, text, sizeof(text)); yl = kv(sf, in, x0, yl, "squawk", text, row->alarm ? INK(TUI_RED) : INK(TUI_MAGENTA)); }
    else yl = kv(sf, in, x0, yl, "squawk", "not heard yet", LS_ATTR_DIM);
    if (row->have & LS_AF_ALT) {
        snprintf(text, sizeof(text), "%ld ft%s", (long)row->alt, row->stale & LS_AF_ALT ? " stale" : "");
        yl = kv(sf, in, x0, yl, "altitude", text, INK(alt_hue(row->alt)));
    } else yl = kv(sf, in, x0, yl, "altitude", "unknown", LS_ATTR_DIM);
    if (row->have & LS_AF_VS) {
        const char t = ls_adsbd_trend(row);
        snprintf(text, sizeof(text), "%+ld ft/min %s", (long)row->vs, t == '^' ? "climbing" : t == 'v' ? "descending" : "level");
        yl = kv(sf, in, x0, yl, "v/s", text, t == '^' ? INK(TUI_GREEN) : t == 'v' ? INK(TUI_YELLOW) : TUI_ATTR(TUI_WHITE, TUI_BLACK));
    } else yl = kv(sf, in, x0, yl, "v/s", "unknown", LS_ATTR_DIM);
    int *yy = two ? &yr : &yl;
    if (a->airspeed_valid && (!a->velocity_valid || a->airspeed_ts_us > a->velocity_ts_us))
        snprintf(text, sizeof(text), "%d kt %s", a->airspeed, a->airspeed_type ? "TAS" : "IAS");
    else if (row->have & LS_AF_SPD) snprintf(text, sizeof(text), "%d kt ground%s", row->spd, row->stale & LS_AF_SPD ? " stale" : "");
    else snprintf(text, sizeof(text), "unknown");
    *yy = kv(sf, in, x1, *yy, "speed", text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    if (row->have & LS_AF_HDG) snprintf(text, sizeof(text), "%03d %s%s", row->hdg, ls_geo_compass(row->hdg), row->stale & LS_AF_HDG ? " stale" : "");
    else snprintf(text, sizeof(text), "unknown");
    *yy = kv(sf, in, x1, *yy, a->heading_is_track ? "track" : "heading", text, SOFT(TUI_MAGENTA));
    if (row->have & LS_AF_POS) snprintf(text, sizeof(text), "%.4f, %.4f%s", (double)row->lat, (double)row->lon, row->stale & LS_AF_POS ? " stale" : "");
    else snprintf(text, sizeof(text), "not reported yet");
    *yy = kv(sf, in, x1, *yy, "position", text, row->have & LS_AF_POS ? TUI_ATTR(TUI_WHITE, TUI_BLACK) : LS_ATTR_DIM);
    if (row->nm >= 0) {
        snprintf(text, sizeof(text), "%.1f nm %s (%03d)", (double)row->nm, ls_geo_compass(row->brg), (int)(row->brg + 0.5f) % 360);
        *yy = kv(sf, in, x1, *yy, "from here", text, INK(TUI_YELLOW));
    }
    int tracked = (int)((s_now - a->first_seen_us) / 1000000);
    if (tracked < 0) tracked = 0;
    snprintf(text, sizeof(text), "%lds ago, tracked %ds, %lu msgs", (long)row->age_s, tracked, (unsigned long)row->msgs);
    *yy = kv(sf, in, x1, *yy, "heard", text, row->age_s > 15 ? INK(TUI_YELLOW) : TUI_ATTR(TUI_WHITE, TUI_BLACK));
    /* Its altitude over the reports heard, when the pane has rows left. */
    y = yl > yr ? yl : yr;
    if (!two && in.y + in.h - y >= 4) {
        tui_put_str(sf, in, x0, y + 1, "ALTITUDE, RECENT REPORTS", LS_ATTR_FAINT);
        ls_scr_adsb_trend(sf, tui_rect_make(x0, y + 2, in.w - 2, in.y + in.h - y - 2), a);
    }
}

static void draw_status(tui_surface *sf)
{
    const tui_rect r = s_r_status;
    char text[80];
    int alarms = 0, placed = 0;
    for (int i = 0; i < s_nrow; i++) { alarms += s_row[i].alarm != NULL; placed += (s_row[i].have & LS_AF_POS) != 0; }
    const int k = snprintf(text, sizeof(text), " %d aircraft  %d placed  %d emergenc%s", s_nrow, placed, alarms, alarms == 1 ? "y" : "ies");
    tui_put_str(sf, r, r.x, r.y, text, alarms ? INK(TUI_RED) : INK(HUE));
    const int x = r.x + k + 2;
    const bool mine = s_say[0] && esp_timer_get_time() - s_said_us < 8000000;
    const char *idle = s_find_focus ? "Type a callsign, an address, a squawk or EMERGENCY; Enter keeps it, Esc clears"
                     : "Enter opens its page  click a header to sort  F follow  R radio  O options  right button for more";
    tui_put_str(sf, tui_rect_make(x, r.y, r.x + r.w - x, 1), x, r.y, mine ? s_say : idle, mine ? INK(TUI_YELLOW) : LS_ATTR_DIM);
}

static bool context(int col, int row);

void ls_adsb_desk_draw(tui_surface *sf, tui_rect a, int64_t now)
{
    s_area = a;
    s_hits.n = 0;
    ls_dui_context_claim(context);
    if (a.w < 50 || a.h < 12) {
        ls_panel_notice(sf, a, "ADS-B", "Enlarge the window", "the table keeps its place");
        return;
    }
    refresh(now);
    layout(a);
    draw_menu(sf);
    draw_tools(sf);
    draw_filter(sf);
    draw_table(sf);
    if (s_r_dash.h) ls_scr_adsb_dash(sf, s_r_dash, now);
    if (s_r_map.h) ls_scr_adsb_radar(sf, s_r_map);
    draw_detail(sf);
    draw_status(sf);
    ls_dui_popup_draw(sf, a, &s_menu.pop);
}

/* ----------------------------------------------------------------- input -- */

static void popup_result(int r)
{
    if (r == LS_DUI_PREV) open_menu(s_menu.pop.bar - 1);
    else if (r == LS_DUI_NEXT) open_menu(s_menu.pop.bar + 1);
    else if (r >= 0) run(r);
}

bool ls_adsb_desk_classic_key(ls_tk_t k, char ch)
{
    if (!s_classic) return false;
    if (k == LS_TK_CHAR && (ls_tui_input_mods() & LS_IMOD_CTRL) && (ch == 'l' || ch == 'L')) { s_classic = false; return true; }
    return false;
}

bool ls_adsb_desk_key(ls_tk_t k, char ch)
{
    const uint8_t mods = ls_tui_input_mods();
    const bool ctrl = mods & LS_IMOD_CTRL, alt = mods & LS_IMOD_ALT;
    if (s_menu.pop.open) { popup_result(ls_dui_popup_key(&s_menu.pop, k, ch)); return true; }
    if (k == LS_TK_WHEEL_UP || k == LS_TK_WHEEL_DOWN) {
        int c, r;
        /* Over the map the wheel is its zoom; elsewhere the list goes by. */
        if (ls_tui_pointer_cell(&c, &r) && tui_rect_contains(s_r_map, c, r)) { run(k == LS_TK_WHEEL_UP ? C_ZOOM_IN : C_ZOOM_OUT); return true; }
        s_free = true;
        s_t.first += k == LS_TK_WHEEL_UP ? -3 : 3;
        ls_dui_table_clamp(&s_t, s_n);
        return true;
    }
    char low = ch;
    if (low >= 'A' && low <= 'Z') low = (char)(low + 32);
    if (k == LS_TK_CHAR && alt) {
        const char *p = low ? strchr("rva", low) : NULL;
        if (p) { open_menu((int)(p - "rva")); return true; }
        return true;
    }
    if (k == LS_TK_CHAR && ctrl) {
        switch (low) {
        case 'f': run(C_FIND); break;
        case 'e': run(C_EXPORT); break;
        case 'c': run(C_COPY_CALL); break;
        case 'l': run(C_CLASSIC); break;
        case 'v': if (s_find_focus) ls_dui_edit_paste(&s_find); break;
        default: break;
        }
        return true;
    }
    if (s_find_focus) {
        if (k == LS_TK_ESC) { ls_dui_edit_set(&s_find, "", 16); s_find_focus = false; return true; }
        if (k == LS_TK_ENTER || k == LS_TK_DOWN || k == LS_TK_TAB) { s_find_focus = false; return true; }
        ls_dui_edit_key(&s_find, k, ch);
        return true;
    }
    const int page = s_t.rows > 1 ? s_t.rows - 1 : 1;
    switch (k) {
    case LS_TK_UP: move_to(s_sel < 0 ? 0 : s_sel - 1); return true;
    case LS_TK_DOWN: move_to(s_sel + 1); return true;
    case LS_TK_PGUP: move_to(s_sel < 0 ? 0 : s_sel - page); return true;
    case LS_TK_PGDN: move_to(s_sel + page); return true;
    case LS_TK_HOME: move_to(0); return true;
    case LS_TK_END: move_to(s_n - 1); return true;
    case LS_TK_ENTER: run(C_PAGE); return true;
    case LS_TK_TAB: run(C_FIND); return true;
    case LS_TK_ESC:
        if (s_find.len) { ls_dui_edit_set(&s_find, "", 16); return true; }
        return false;
    case LS_TK_CHAR:
        if (low == '/') { run(C_FIND); return true; }
        return false;               /* F R O V M H W + -: the handheld's */
    default: return false;
    }
}

bool ls_adsb_desk_touch(int col, int row)
{
    if (s_menu.pop.open) {
        const int bar = s_menu.pop.bar;
        const int r = ls_dui_popup_tap(&s_menu.pop, col, row);
        if (r == LS_DUI_CLOSED && row == s_r_menu.y) {
            const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
            if (i >= 0 && i != bar) open_menu(i);
        } else popup_result(r);
        return true;
    }
    if (row == s_r_menu.y) {
        const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
        if (i >= 0) open_menu(i);
        return true;
    }
    if (tui_rect_contains(s_r_find, col, row)) {
        s_find_focus = true;
        ls_dui_edit_click(&s_find, s_r_find.x, s_r_find.w, col);
        return true;
    }
    const int cmd = ls_dui_hits_at(&s_hits, col, row);
    if (cmd >= 0) { run(cmd); return true; }
    s_find_focus = false;
    int what = 0;
    switch (ls_dui_table_hit(&s_t, LS_AC__COUNT, col, row, &what)) {
    case LS_DUI_T_HEAD: sort_by(what); break;
    case LS_DUI_T_STRIP:
        s_free = true;
        s_t.first += what * (s_t.rows > 1 ? s_t.rows - 1 : 1);
        ls_dui_table_clamp(&s_t, s_n);
        break;
    case LS_DUI_T_ROW:
        if (what < s_n) {
            const bool again = what == s_sel;
            move_to(what);
            /* Twice on a row opens its page. */
            if (again && ls_tui_input_clicks() >= 2) run(C_PAGE);
        }
        break;
    default: break;
    }
    return true;
}

static bool context(int col, int row)
{
    s_menu.pop.open = false;
    int what = 0;
    const int part = ls_dui_table_hit(&s_t, LS_AC__COUNT, col, row, &what);
    if (part == LS_DUI_T_ROW && what < s_n) {
        move_to(what);
        row_menu(col, row);
        return true;
    }
    if (part == LS_DUI_T_HEAD) { head_menu(col, row + 1); return true; }
    if (tui_rect_contains(s_r_detail, col, row) && selected()) { row_menu(col, row); return true; }
    return false;
}

void ls_adsb_desk_enter(void)
{
    static EXT_RAM_BSS_ATTR bool once;
    s_menu.pop.open = false;
    s_find_focus = false;
    s_free = false;
    if (!once) {
        once = true;
        ls_dui_edit_set(&s_find, "", 16);
        /* Nothing here has an initialiser: all of it is in PSRAM, where
           the Nano has room, and is set the first time the screen opens. */
        s_alarms_first = s_show_map = s_show_detail = s_show_dash = true;
        s_shown = (1u << LS_AC__COUNT) - 1;
        /* The order the handheld was left in. */
        const int sort = ls_scr_adsb_sort_get();
        s_sort = sort == 1 ? LS_AC_DIST : sort == 2 ? LS_AC_ALT : -1;
    }
}

#endif /* LS_TUI_HAS_COMPACT */
