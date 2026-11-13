/* JOURNAL as a desktop program: the entries in a table, the one chosen
   beside it.

       Journal  View  Entry                      RECORDING  12 entries
       [New] [Edit] [Record ON] [Radio GPS] [Sensors] [Map]  Find [....]
       +ENTRIES 12 of 12------------------+ +BRIDGE SURVEY--------------+
       | TITLE       WHEN     RADIO  MHz  | | 2026-11-12 14:02:11Z      |
       | rows, sorted by any column       | | where, what radio, signal |
       |                                  | | the note, wrapped; PgUp   |
       +----------------------------------+ | and PgDn read it          |
       CSV status, the card, the last thing said

   The entries, recording and every action are the handheld's (scr_journal.c
   and the field worker, ls_field.h); a row's order and text are
   ls_journal_desk.c; the widgets are ls_deskui.c. New and Edit open the
   same NOTE fields the handheld does. */
#include "scr_journal_desk.h"

#if LS_TUI_HAS_COMPACT

#include "../../ls_deskui.h"
#include "../../ls_journal_desk.h"
#include "../../ls_box.h"
#include "../../ls_input_ev.h"
#include "../../ls_tui_touch.h"
#include "core/ls_time.h"
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

static EXT_RAM_BSS_ATTR ls_jrnd_row_t s_row[LS_JOURNAL_CAP];
static EXT_RAM_BSS_ATTR uint8_t s_order[LS_JOURNAL_CAP], s_view[LS_JOURNAL_CAP];
static EXT_RAM_BSS_ATTR ls_journal_entry_t s_e;            /* the one being read */
static EXT_RAM_BSS_ATTR ls_field_state_t s_st;
static EXT_RAM_BSS_ATTR int s_nrow, s_n, s_sel, s_sort, s_scroll, s_lines, s_text_rows;
static EXT_RAM_BSS_ATTR uint32_t s_sel_id, s_shown;
static EXT_RAM_BSS_ATTR int64_t s_built_us, s_said_us;
static EXT_RAM_BSS_ATTR bool s_have, s_desc, s_free, s_classic, s_find_focus, s_show_read;
static EXT_RAM_BSS_ATTR ls_dui_edit_t s_find;
static EXT_RAM_BSS_ATTR char s_built_find[40];
static EXT_RAM_BSS_ATTR ls_dui_table_t s_t;
static EXT_RAM_BSS_ATTR ls_dui_menu_t s_menu;
static EXT_RAM_BSS_ATTR ls_dui_hits_t s_hits;
static EXT_RAM_BSS_ATTR tui_rect s_area, s_r_menu, s_r_tools, s_r_table, s_r_read, s_r_status, s_r_find;
static EXT_RAM_BSS_ATTR int16_t s_bar_x[6];
static EXT_RAM_BSS_ATTR char s_say[96], s_dir[96];

static const ls_dui_col_t COLS[LS_JC__COUNT] = {
    [LS_JC_TITLE]  = { "TITLE",    24, 10, LS_DUI_FLEX, 0 },
    [LS_JC_WHEN]   = { "WHEN",     20, 20, 0, 0 },
    [LS_JC_SOURCE] = { "RADIO",    8,  8, 0, 2 },
    [LS_JC_FREQ]   = { "MHz",      9,  9, LS_DUI_RIGHT, 3 },
    [LS_JC_RSSI]   = { "dBm",      4,  4, LS_DUI_RIGHT, 4 },
    [LS_JC_PLACE]  = { "PLACE",    17, 17, 0, 1 },
    [LS_JC_SIZE]   = { "SIZE",     4,  4, LS_DUI_RIGHT, 6 },
    [LS_JC_KEPT]   = { "ON",       3,  3, 0, 5 },
};

enum {
    C_NEW, C_EDIT, C_RECORD, C_RADIO, C_SENSORS, C_MAP, C_FIND, C_EXPORT, C_CLASSIC, C_PANE_READ,
    C_COPY_TITLE, C_COPY_TEXT, C_COPY_PLACE,
    C_COL = 100, C_SORT = 120,
};

static const char *const MENUS[] = { "Journal", "View", "Entry" };
#define NMENUS 3

static void say(const char *text) { snprintf(s_say, sizeof(s_say), "%s", text); s_said_us = esp_timer_get_time(); }

bool ls_journal_desk_classic(void) { return s_classic; }
void ls_journal_desk_export_dir(const char *dir) { snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : ""); }

/* ------------------------------------------------------------------ data -- */

static const ls_jrnd_row_t *at(int k) { return k >= 0 && k < s_n ? &s_row[s_view[k]] : NULL; }
static const ls_jrnd_row_t *selected(void) { return at(s_sel); }

/* The rows, read off the worker: when the count moves, Find changes, or a
   second has gone by (an edit keeps the count). Each entry is copied once
   into the one scratch entry, which is where Find reads its note. */
static void rebuild(bool force)
{
    const int64_t now = esp_timer_get_time();
    if (!force && s_nrow == s_st.journal_count && !strcmp(s_built_find, s_find.text) && now - s_built_us < 1000000) return;
    s_built_us = now;
    snprintf(s_built_find, sizeof(s_built_find), "%.39s", s_find.text);
    s_nrow = 0;
    static EXT_RAM_BSS_ATTR uint8_t hit[LS_JOURNAL_CAP];
    for (int i = 0; i < s_st.journal_count && s_nrow < LS_JOURNAL_CAP; i++) {
        if (!ls_field_entry(i, &s_e)) break;
        ls_jrnd_row_t *r = &s_row[s_nrow];
        memset(r, 0, sizeof(*r));
        r->index = (uint16_t)i;
        r->id = s_e.id;
        snprintf(r->title, sizeof(r->title), "%s", s_e.title);
        r->time_us = s_e.sample.time_us;
        r->clock = s_e.sample.utc[0] != 0;
        if (r->clock) snprintf(r->when, sizeof(r->when), "%s", s_e.sample.utc);
        else {
            char stamp[LS_TIME_STAMP_MAX];
            ls_time_render_stamp_at(stamp, sizeof(stamp), 0, s_e.sample.time_us);
            snprintf(r->when, sizeof(r->when), "%.23s", stamp);
        }
        snprintf(r->source, sizeof(r->source), "%s", ls_field_source_name(s_e.sample.source));
        r->gps = s_e.sample.gps_valid;
        r->imu = s_e.sample.imu_valid;
        r->signal = s_e.sample.signal_valid;
        r->saved = s_e.saved;
        r->lat = s_e.sample.lat;
        r->lon = s_e.sample.lon;
        r->freq = s_e.sample.radio_valid || s_e.sample.signal_valid ? s_e.sample.frequency : 0;
        r->rssi = s_e.sample.rssi;
        r->chars = (uint16_t)strlen(s_e.text);
        hit[s_nrow] = ls_jrnd_holds(s_e.title, s_find.text) || ls_jrnd_holds(s_e.text, s_find.text) ||
                      ls_jrnd_holds(r->source, s_find.text) || ls_jrnd_holds(r->when, s_find.text);
        s_nrow++;
    }
    ls_jrnd_sort(s_order, s_row, s_nrow, s_sort, s_desc);
    s_n = 0;
    for (int i = 0; i < s_nrow; i++)
        if (hit[s_order[i]]) s_view[s_n++] = s_order[i];
    s_have = false;
}

/* The cursor is the entry the handheld's keys act on, wherever the order put it. */
static void place_cursor(void)
{
    s_sel = -1;
    for (int k = 0; k < s_n && s_sel < 0; k++)
        if (s_sel_id && s_row[s_view[k]].id == s_sel_id) s_sel = k;
    if (s_sel < 0 && s_n) {
        /* Nothing chosen yet, or it went: the handheld's own selection. */
        const int want = ls_scr_journal_selected();
        for (int k = 0; k < s_n && s_sel < 0; k++)
            if (s_row[s_view[k]].index == want) s_sel = k;
        if (s_sel < 0) s_sel = 0;
        s_sel_id = s_row[s_view[s_sel]].id;
    }
    const ls_jrnd_row_t *r = selected();
    if (r) ls_scr_journal_select(r->index);
    if (r && (!s_have || s_e.id != r->id)) { s_have = ls_field_entry(r->index, &s_e); }
    else if (!r) s_have = false;
}

static void move_to(int k)
{
    if (!s_n) return;
    if (k < 0) k = 0;
    if (k >= s_n) k = s_n - 1;
    s_free = false;
    s_sel = k;
    s_sel_id = s_row[s_view[k]].id;
    s_scroll = 0;
    s_have = false;
}

/* ----------------------------------------------------------------- menus -- */

static void build_menu(int which)
{
    char text[44];
    const ls_jrnd_row_t *r = selected();
    const int none = r ? 0 : LS_DUI_OFF;
    ls_dui_menu_begin(&s_menu);
    switch (which) {
    case 0:
        ls_dui_menu_item(&s_menu, "New note...", "N", C_NEW, 0);
        ls_dui_menu_item(&s_menu, s_st.recording ? "Stop recording" : "Record a walk (CSV)", "C", C_RECORD, 0);
        snprintf(text, sizeof(text), "Attach radio: %.20s", ls_scr_journal_radio());
        ls_dui_menu_item(&s_menu, text, "R", C_RADIO, 0);
        ls_dui_menu_item(&s_menu, "Live attachments", "V", C_SENSORS, 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Save the list as text", "Ctrl+E", C_EXPORT, s_n ? 0 : LS_DUI_OFF);
        break;
    case 1:
        ls_dui_menu_item(&s_menu, "Newest first", NULL, C_SORT + LS_JC_WHEN, s_sort == LS_JC_WHEN && !s_desc ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "By title", NULL, C_SORT + LS_JC_TITLE, s_sort == LS_JC_TITLE ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "By radio", NULL, C_SORT + LS_JC_SOURCE, s_sort == LS_JC_SOURCE ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Strongest signal first", NULL, C_SORT + LS_JC_RSSI, s_sort == LS_JC_RSSI ? LS_DUI_CHECK : 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Reading pane", NULL, C_PANE_READ, s_show_read ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Handheld layout", "Ctrl+L", C_CLASSIC, 0);
        break;
    default:
        ls_dui_menu_item(&s_menu, r ? (r->title[0] ? r->title : "(no title)") : "NO ENTRY CHOSEN", NULL, -1, LS_DUI_HEAD);
        ls_dui_menu_item(&s_menu, "Edit the note...", "E", C_EDIT, none);
        ls_dui_menu_item(&s_menu, "Show on MAP", "G", C_MAP, r && r->gps ? 0 : LS_DUI_OFF);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Copy title", "Ctrl+C", C_COPY_TITLE, none);
        ls_dui_menu_item(&s_menu, "Copy the note's first line", NULL, C_COPY_TEXT, r && r->chars ? 0 : LS_DUI_OFF);
        ls_dui_menu_item(&s_menu, "Copy position", NULL, C_COPY_PLACE, r && r->gps ? 0 : LS_DUI_OFF);
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

static void head_menu(int x, int y)
{
    ls_dui_menu_begin(&s_menu);
    ls_dui_menu_item(&s_menu, "COLUMNS", NULL, -1, LS_DUI_HEAD);
    for (int c = 0; c < LS_JC__COUNT; c++)
        ls_dui_menu_item(&s_menu, COLS[c].title, NULL, C_COL + c, ((s_shown >> c) & 1 ? LS_DUI_CHECK : 0) | (COLS[c].drop ? 0 : LS_DUI_OFF));
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

/* Every entry shown, in the order shown, as a text file a PC reads. */
static void export_text(void)
{
    char path[160], line[120];
    const char *dir = s_dir[0] ? s_dir : "/sdcard/journal";
    make_dir(dir);
    snprintf(path, sizeof(path), "%s/journal-%lu.txt", dir, (unsigned long)time(NULL));
    FILE *f = fopen(path, "w");
    if (!f) { say("No card to save to"); return; }
    int n = 0;
    for (int k = 0; k < s_n; k++) {
        const ls_jrnd_row_t *r = at(k);
        if (!ls_field_entry(r->index, &s_e)) continue;
        fprintf(f, "== %s\n%s  %s", s_e.title, r->when, r->source);
        if (r->freq) fprintf(f, "  %.4f MHz", r->freq / 1e6);
        if (r->signal) fprintf(f, "  %.0f dBm", (double)r->rssi);
        if (r->gps) fprintf(f, "  %.6f, %.6f", r->lat, r->lon);
        fprintf(f, "\n\n%s\n\n", s_e.text);
        n++;
    }
    fclose(f);
    s_have = false;
    snprintf(line, sizeof(line), "Saved %d entr%s to %.40s", n, n == 1 ? "y" : "ies", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    say(line);
}

static void run(int cmd)
{
    char text[96];
    const ls_jrnd_row_t *r = selected();
    s_find_focus = false;
    if (cmd >= C_SORT) {
        const int c = cmd - C_SORT;
        if (c >= LS_JC__COUNT) return;
        if (s_sort == c) s_desc = !s_desc;
        else { s_sort = c; s_desc = false; }
        s_free = false;
        rebuild(true);
        return;
    }
    if (cmd >= C_COL) { if (cmd - C_COL < LS_JC__COUNT) s_shown ^= 1u << (cmd - C_COL); return; }
    switch (cmd) {
    case C_NEW: ls_scr_journal_action(0); break;
    case C_RADIO: ls_scr_journal_action(1); break;
    case C_RECORD: ls_scr_journal_action(2); break;
    case C_SENSORS: ls_scr_journal_action(3); break;
    case C_EDIT: if (r) ls_scr_journal_action(4); else say("Choose an entry first"); break;
    case C_MAP: if (r && r->gps) ls_scr_journal_action(5); else say("That entry has no position"); break;
    case C_FIND: s_find_focus = true; s_find.caret = s_find.len; break;
    case C_EXPORT: export_text(); break;
    case C_CLASSIC: s_classic = true; s_menu.pop.open = false; break;
    case C_PANE_READ: s_show_read = !s_show_read; break;
    case C_COPY_TITLE: if (r) copy(r->title); break;
    case C_COPY_TEXT: if (r && s_have) { ls_jrnd_wrap(s_e.text, 0, 90, text, sizeof(text)); copy(text); } break;
    case C_COPY_PLACE: if (r && r->gps) { snprintf(text, sizeof(text), "%.6f, %.6f", r->lat, r->lon); copy(text); } break;
    default: break;
    }
}

/* ---------------------------------------------------------------- layout -- */

static void layout(tui_rect a)
{
    const tui_rect none = tui_rect_make(0, 0, 0, 0);
    int y = a.y;
    s_r_menu = tui_rect_make(a.x, y++, a.w, 1);
    s_r_tools = tui_rect_make(a.x, y++, a.w, 1);
    s_r_status = tui_rect_make(a.x, a.y + a.h - 1, a.w, 1);
    const tui_rect body = tui_rect_make(a.x, y, a.w, a.y + a.h - 1 - y);
    s_r_read = none;
    int rw = 0, rh = 0;
    if (s_show_read) {
        if (a.w >= 100) { rw = a.w * 2 / 5; if (rw > 76) rw = 76; }
        else if (body.h >= 20) rh = body.h / 2;
    }
    s_r_table = tui_rect_make(body.x, body.y, body.w - (rw ? rw - 1 : 0), body.h - (rh ? rh - 1 : 0));
    if (rw) s_r_read = tui_rect_make(body.x + body.w - rw, body.y, rw, body.h);
    else if (rh) s_r_read = tui_rect_make(body.x, body.y + body.h - rh, body.w, rh);
    ls_dui_table_layout(&s_t, tui_rect_make(s_r_table.x + 1, s_r_table.y + 1, s_r_table.w - 2, s_r_table.h - 2), COLS,
                        LS_JC__COUNT, s_shown | (1u << LS_JC_TITLE) | (1u << LS_JC_WHEN));
}

/* ------------------------------------------------------------------ draw -- */

static void draw_menu(tui_surface *sf)
{
    ls_dui_bar_draw(sf, s_r_menu, MENUS, NMENUS, s_menu.pop.open ? s_menu.pop.bar : -1, HUE, s_bar_x);
    char text[64];
    snprintf(text, sizeof(text), "%d entr%s", s_st.journal_count, s_st.journal_count == 1 ? "y" : "ies");
    int x = s_r_menu.x + s_r_menu.w - (int)strlen(text) - 1;
    if (x <= s_bar_x[NMENUS] + 2) return;
    tui_put_str(sf, s_r_menu, x, s_r_menu.y, text, INK(HUE));
    const char *word = s_st.record_errors ? "WRITE FAILED" : s_st.recording ? "RECORDING" : "IDLE";
    x -= (int)strlen(word) + 2;
    if (x > s_bar_x[NMENUS] + 2)
        tui_put_str(sf, s_r_menu, x, s_r_menu.y, word, s_st.record_errors ? INK(TUI_RED) : s_st.recording ? INK(TUI_GREEN) : LS_ATTR_DIM);
}

static void draw_tools(tui_surface *sf)
{
    const tui_rect r = s_r_tools;
    char label[40];
    const ls_jrnd_row_t *row = selected();
    int x = r.x + 1;
    x += ls_dui_tool(sf, r, x, "New", C_NEW, false, false, TUI_GREEN, &s_hits);
    x += ls_dui_tool(sf, r, x, "Edit", C_EDIT, false, !row, HUE, &s_hits);
    x += ls_dui_tool(sf, r, x, s_st.recording ? "Record ON" : "Record OFF", C_RECORD, s_st.recording, false, TUI_RED, &s_hits);
    snprintf(label, sizeof(label), "Radio %.16s", ls_scr_journal_radio());
    x += ls_dui_tool(sf, r, x, label, C_RADIO, false, false, TUI_MAGENTA, &s_hits);
    x += ls_dui_tool(sf, r, x, "Sensors", C_SENSORS, false, false, TUI_YELLOW, &s_hits);
    x += ls_dui_tool(sf, r, x, "Map", C_MAP, false, !row || !row->gps, TUI_YELLOW, &s_hits);
    x += 1;
    s_r_find = tui_rect_make(0, 0, 0, 0);
    if (x + 26 < r.x + r.w) {
        tui_put_str(sf, r, x, r.y, "Find", s_find_focus ? INK(TUI_YELLOW) : LS_ATTR_DIM);
        s_r_find = tui_rect_make(x + 5, r.y, 18, 1);
        ls_dui_edit_draw(sf, r, x + 5, r.y, 18, &s_find, s_find_focus, TUI_YELLOW);
        x += 25;
    }
    x += ls_dui_tool(sf, r, x, "Save as text", C_EXPORT, false, !s_n, TUI_GREEN, &s_hits);
}

static void draw_table(tui_surface *sf)
{
    char title[40], text[40];
    snprintf(title, sizeof(title), "ENTRIES %d of %d", s_n, s_nrow);
    ls_dui_pane(sf, s_r_table, title, "", s_st.recording ? TUI_GREEN : HUE, !s_find_focus);
    ls_dui_table_head(sf, &s_t, COLS, LS_JC__COUNT, s_sort, s_desc, HUE);
    const tui_rect b = s_t.body;
    if (!s_n) {
        if (s_nrow) {
            tui_put_str(sf, b, b.x + 2, b.y + 1, "Nothing matches Find.", LS_ATTR_DIM);
            tui_put_str(sf, b, b.x + 2, b.y + 3, "Esc clears it.", LS_ATTR_DIM);
            return;
        }
        tui_put_str(sf, b, b.x + 2, b.y + 1, "A notebook for the field.", LS_ATTR_DIM);
        tui_put_str(sf, b, b.x + 2, b.y + 3, "New writes a note where you are; Record keeps a walk", TUI_ATTR(TUI_WHITE, TUI_BLACK));
        tui_put_str(sf, b, b.x + 2, b.y + 4, "with the sensors. A radio's reading is attached if one", TUI_ATTR(TUI_WHITE, TUI_BLACK));
        tui_put_str(sf, b, b.x + 2, b.y + 5, "is picked with Radio; it is optional.", TUI_ATTR(TUI_WHITE, TUI_BLACK));
        return;
    }
    if (!s_free && s_sel >= 0) ls_dui_table_follow(&s_t, s_sel, s_n);
    else ls_dui_table_clamp(&s_t, s_n);
    for (int row = 0; row < s_t.rows && s_t.first + row < s_n; row++) {
        const int k = s_t.first + row, y = b.y + row;
        const ls_jrnd_row_t *r = at(k);
        const bool on = k == s_sel;
        if (on) ls_fill_dither(sf, tui_rect_make(b.x, y, b.w, 1), LS_DITHER_MEDIUM, HUE);
        if (on) ls_box_put(sf, b, b.x, y, LS_SHAPE_RIGHT, WHITE);
        ls_jrnd_cell(r, LS_JC_TITLE, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_TITLE, row, text, on ? WHITE : r->title[0] ? INK(HUE) : LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_WHEN, row, r->when, r->clock ? TUI_ATTR(TUI_WHITE, TUI_BLACK) : LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_SOURCE, row, r->source, SOFT(TUI_MAGENTA));
        ls_jrnd_cell(r, LS_JC_FREQ, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_FREQ, row, text, INK(TUI_YELLOW));
        ls_jrnd_cell(r, LS_JC_RSSI, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_RSSI, row, text, INK(r->rssi >= -90 ? TUI_GREEN : r->rssi >= -105 ? TUI_YELLOW : TUI_RED));
        ls_jrnd_cell(r, LS_JC_PLACE, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_PLACE, row, r->gps ? text : "-", r->gps ? SOFT(TUI_YELLOW) : LS_ATTR_FAINT);
        ls_jrnd_cell(r, LS_JC_SIZE, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_SIZE, row, text, LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, COLS, LS_JC_KEPT, row, r->saved ? "SD" : "RAM", r->saved ? SOFT(TUI_GREEN) : INK(TUI_YELLOW));
    }
    ls_dui_table_strip(sf, &s_t, s_n, HUE);
}

static void draw_read(tui_surface *sf)
{
    const tui_rect r = s_r_read;
    if (!r.w) return;
    const ls_jrnd_row_t *row = selected();
    char text[96], line[128];
    if (!row || !s_have) {
        const tui_rect in = ls_dui_pane(sf, r, "NO ENTRY", "", HUE, false);
        tui_put_str(sf, in, in.x + 1, in.y, s_nrow ? "Click a row, or Up and Down." : "Nothing written yet.", LS_ATTR_DIM);
        return;
    }
    snprintf(text, sizeof(text), "%.40s", s_e.title[0] ? s_e.title : "(no title)");
    const tui_rect in = ls_dui_pane(sf, r, text, row->saved ? "on the card" : "in RAM, not saved yet", HUE, false);
    int y = in.y;
    tui_put_str(sf, in, in.x + 1, y++, row->when, row->clock ? WHITE : LS_ATTR_DIM);
    /* What was attached when it was written, a chip each. */
    int x = in.x + 1;
    static const uint8_t CHIP_HUE[] = { TUI_MAGENTA, TUI_YELLOW, TUI_BLUE, TUI_GREEN };
    const char *chips[4];
    int nchips = 0;
    chips[nchips++] = row->source;
    if (row->gps) chips[nchips++] = "GPS";
    if (row->imu) chips[nchips++] = "IMU";
    if (row->signal) chips[nchips++] = "SIGNAL";
    for (int i = 0; i < nchips; i++) {
        if (!chips[i][0]) continue;
        const int w = (int)strlen(chips[i]) + 2;
        if (x + w > in.x + in.w) break;
        tui_fill(sf, tui_rect_make(x, y, w, 1), ' ', TUI_ATTR(TUI_BLACK, CHIP_HUE[i]));
        tui_put_str(sf, in, x + 1, y, chips[i], TUI_ATTR(TUI_BLACK, CHIP_HUE[i]));
        x += w + 1;
    }
    y++;
    if (row->gps) {
        snprintf(text, sizeof(text), "%.6f, %.6f  %.0f m", row->lat, row->lon, (double)s_e.sample.alt_m);
        tui_put_str(sf, in, in.x + 1, y++, text, SOFT(TUI_YELLOW));
    }
    if (row->freq || row->signal) {
        int k = 0;
        if (row->freq) k += snprintf(text + k, sizeof(text) - (size_t)k, "%.4f MHz  ", row->freq / 1e6);
        if (row->signal) k += snprintf(text + k, sizeof(text) - (size_t)k, "%.1f dBm", (double)row->rssi);
        tui_put_str(sf, in, in.x + 1, y++, text, INK(TUI_YELLOW));
    }
    for (int cx = in.x; cx < in.x + in.w; cx++) ls_box_put(sf, in, cx, y, 0x2500, LS_ATTR_FAINT);
    y++;
    /* The note. */
    const int w = in.w - 2, rows = in.y + in.h - y;
    s_text_rows = rows;
    s_lines = ls_jrnd_lines(s_e.text, w);
    if (s_scroll > s_lines - rows) s_scroll = s_lines - rows;
    if (s_scroll < 0) s_scroll = 0;
    if (!s_lines) tui_put_str(sf, in, in.x + 1, y, "(no text)", LS_ATTR_DIM);
    int off = 0;
    for (int l = 0; l < s_lines && l < s_scroll + rows; l++) {
        off = ls_jrnd_wrap(s_e.text, off, w, line, sizeof(line));
        if (l >= s_scroll) tui_put_str(sf, in, in.x + 1, y + l - s_scroll, line, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    }
    if (s_lines > rows) {
        snprintf(text, sizeof(text), " %d-%d of %d: PgUp PgDn ", s_scroll + 1, s_scroll + rows, s_lines);
        tui_put_str(sf, r, r.x + r.w - (int)strlen(text) - 2, r.y + r.h - 1, text, SOFT(HUE));
    }
}

static void draw_status(tui_surface *sf)
{
    const tui_rect r = s_r_status;
    char text[120];
    const int k = snprintf(text, sizeof(text), " CSV %s  %lu rows  %lu errors", s_st.record_errors ? "WRITE FAILED" : s_st.recording ? "SAVING" : "STOPPED",
                           (unsigned long)s_st.record_rows, (unsigned long)s_st.record_errors);
    tui_put_str(sf, r, r.x, r.y, text, s_st.record_errors ? INK(TUI_RED) : s_st.recording ? INK(TUI_GREEN) : INK(HUE));
    const int x = r.x + k + 2;
    const bool mine = s_say[0] && esp_timer_get_time() - s_said_us < 8000000;
    const char *fb = ls_scr_journal_feedback();
    const char *idle = s_find_focus ? "Type part of a title, a note, a radio or a date; Enter keeps it, Esc clears"
                     : s_st.storage[0] ? s_st.storage : "N new  E edit  C record  G map  click a header to sort  right button for more";
    tui_put_str(sf, tui_rect_make(x, r.y, r.x + r.w - x, 1), x, r.y, mine ? s_say : fb[0] ? fb : idle,
                mine || fb[0] ? INK(TUI_YELLOW) : LS_ATTR_DIM);
}

static bool context(int col, int row);

void ls_journal_desk_draw(tui_surface *sf, tui_rect a, const ls_field_state_t *st)
{
    s_area = a;
    s_st = *st;
    s_hits.n = 0;
    ls_dui_context_claim(context);
    if (a.w < 50 || a.h < 12) {
        ls_panel_notice(sf, a, "JOURNAL", "Enlarge the window", "the list keeps its place");
        return;
    }
    rebuild(false);
    place_cursor();
    layout(a);
    draw_menu(sf);
    draw_tools(sf);
    draw_table(sf);
    draw_read(sf);
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

bool ls_journal_desk_classic_key(ls_tk_t k, char ch)
{
    if (!s_classic) return false;
    if (k == LS_TK_CHAR && (ls_tui_input_mods() & LS_IMOD_CTRL) && (ch == 'l' || ch == 'L')) { s_classic = false; return true; }
    return false;
}

bool ls_journal_desk_key(ls_tk_t k, char ch)
{
    const uint8_t mods = ls_tui_input_mods();
    const bool ctrl = mods & LS_IMOD_CTRL, alt = mods & LS_IMOD_ALT;
    if (s_menu.pop.open) { popup_result(ls_dui_popup_key(&s_menu.pop, k, ch)); return true; }
    char low = ch;
    if (low >= 'A' && low <= 'Z') low = (char)(low + 32);
    if (k == LS_TK_CHAR && alt) {
        const char *p = low ? strchr("jve", low) : NULL;
        if (p) open_menu((int)(p - "jve"));
        return true;
    }
    if (k == LS_TK_CHAR && ctrl) {
        switch (low) {
        case 'f': run(C_FIND); break;
        case 'e': run(C_EXPORT); break;
        case 'c': run(C_COPY_TITLE); break;
        case 'n': run(C_NEW); break;
        case 'l': run(C_CLASSIC); break;
        case 'v': if (s_find_focus) ls_dui_edit_paste(&s_find); break;
        default: break;
        }
        return true;
    }
    if (s_find_focus) {
        if (k == LS_TK_ESC) { ls_dui_edit_set(&s_find, "", 32); s_find_focus = false; return true; }
        if (k == LS_TK_ENTER || k == LS_TK_DOWN || k == LS_TK_TAB) { s_find_focus = false; return true; }
        ls_dui_edit_key(&s_find, k, ch);
        return true;
    }
    const int page = s_t.rows > 1 ? s_t.rows - 1 : 1;
    switch (k) {
    case LS_TK_UP: move_to(s_sel - 1); return true;
    case LS_TK_DOWN: move_to(s_sel + 1); return true;
    case LS_TK_HOME: move_to(0); return true;
    case LS_TK_END: move_to(s_n - 1); return true;
    /* The page keys read the note when it runs past its pane, else the list. */
    case LS_TK_PGUP: if (s_lines > s_text_rows && s_r_read.w) s_scroll -= s_text_rows - 1; else move_to(s_sel - page); return true;
    case LS_TK_PGDN: if (s_lines > s_text_rows && s_r_read.w) s_scroll += s_text_rows - 1; else move_to(s_sel + page); return true;
    case LS_TK_ENTER: case LS_TK_F2: run(C_EDIT); return true;
    case LS_TK_INSERT: run(C_NEW); return true;
    case LS_TK_TAB: run(C_FIND); return true;
    case LS_TK_LEFT: case LS_TK_RIGHT: return true;       /* the handheld's button walk has nothing to walk here */
    case LS_TK_ESC:
        if (s_find.len) { ls_dui_edit_set(&s_find, "", 32); return true; }
        return false;
    case LS_TK_CHAR:
        if (low == '/') { run(C_FIND); return true; }
        return false;               /* N R C V E G: the handheld's */
    default: return false;
    }
}

bool ls_journal_desk_touch(int col, int row)
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
    if (tui_rect_contains(s_r_read, col, row)) {
        /* The upper half reads back, the lower on. */
        if (s_lines > s_text_rows) s_scroll += row < s_r_read.y + s_r_read.h / 2 ? -(s_text_rows - 1) : s_text_rows - 1;
        return true;
    }
    int what = 0;
    switch (ls_dui_table_hit(&s_t, LS_JC__COUNT, col, row, &what)) {
    case LS_DUI_T_HEAD: run(C_SORT + what); break;
    case LS_DUI_T_STRIP:
        s_free = true;
        s_t.first += what * (s_t.rows > 1 ? s_t.rows - 1 : 1);
        ls_dui_table_clamp(&s_t, s_n);
        break;
    case LS_DUI_T_ROW:
        if (what < s_n) {
            const bool again = what == s_sel;
            move_to(what);
            if (again && ls_tui_input_clicks() >= 2) run(C_EDIT);
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
    const int part = ls_dui_table_hit(&s_t, LS_JC__COUNT, col, row, &what);
    if (part == LS_DUI_T_ROW && what < s_n) {
        move_to(what);
        place_cursor();
        build_menu(2);
        ls_dui_menu_open(&s_menu, col, row, -1, HUE);
        return true;
    }
    if (part == LS_DUI_T_HEAD) { head_menu(col, row + 1); return true; }
    if (tui_rect_contains(s_r_read, col, row) && selected()) {
        build_menu(2);
        ls_dui_menu_open(&s_menu, col, row, -1, HUE);
        return true;
    }
    return false;
}

void ls_journal_desk_enter(void)
{
    static EXT_RAM_BSS_ATTR bool once;
    s_menu.pop.open = false;
    s_find_focus = false;
    s_free = false;
    s_built_us = 0;
    s_nrow = -1;                        /* read the entries afresh */
    if (!once) {
        once = true;
        /* Nothing here has an initialiser: all of it is in PSRAM, where
           the Nano has room, and is set the first time the screen opens. */
        ls_dui_edit_set(&s_find, "", 32);
        s_sort = LS_JC_WHEN;
        s_shown = (1u << LS_JC__COUNT) - 1;
        s_show_read = true;
    }
}

#endif /* LS_TUI_HAS_COMPACT */
