/* NOTES: a field notebook. A list of notes, a reader that shows the data
   lines a note carries as data (a live map for a MAP line, a tick box for a
   checklist item), and an editor with a real cursor, the docked keyboard
   when no physical one is attached, and an INSERT menu that writes live
   readings in at the cursor. The notes are Markdown files; see ls_notes.h. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_notes.h"
#include "../../ls_note_blocks.h"
#include "../../ls_textbuf.h"
#include "../../ls_keydock.h"
#include "../../ls_keyboard.h"
#include "../../ls_picker.h"
#include "../../ls_field.h"
#include "../../ls_app.h"
#include "../../ls_map.h"
#include "../../ls_compass_live.h"
#include "ls_keypad.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "core/ls_time.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef enum { V_LIST, V_READ, V_EDIT, V_MAP } view_t;

#define AUTOSAVE_US 20000000LL
#define MAP_ROWS    14

EXT_RAM_BSS_ATTR static char s_text[LS_NOTE_TEXT_MAX];
EXT_RAM_BSS_ATTR static ls_note_info_t s_info;
EXT_RAM_BSS_ATTR static char s_filter[40];
static ls_textbuf_t s_tb;
static view_t s_view;
static int s_sel, s_first, s_read_scroll, s_read_rows;
static int button_focus = -1, button_slot;
static bool s_dock, s_new, s_loaded;
static int64_t s_last_edit_us, s_trash_armed_us;
EXT_RAM_BSS_ATTR static char s_feedback[80];
static tui_rect s_list_area, s_text_area, s_dock_area;
EXT_RAM_BSS_ATTR static int s_row_index[80];         /* list row -> index, for taps */
static int s_row_count;
/* Reader: which text line is on each screen row, so a tap can find a
   checklist item. -1 for rows that are not the start of a line. */
EXT_RAM_BSS_ATTR static int s_read_line_at[120];
static tui_rect s_read_area;
/* Map picture capture. */
static double s_map_lat, s_map_lon;

static void say(const char *text) { snprintf(s_feedback, sizeof(s_feedback), "%s", text); }

/* ------------------------------------------------------------ helpers -- */

static bool matches(const ls_note_info_t *n)
{
    if (!s_filter[0]) return true;
    char a[LS_NOTE_TITLE], b[sizeof(s_filter)];
    size_t i;
    for (i = 0; n->title[i] && i < sizeof(a) - 1; i++) a[i] = (char)tolower((unsigned char)n->title[i]);
    a[i] = 0;
    for (i = 0; s_filter[i] && i < sizeof(b) - 1; i++) b[i] = (char)tolower((unsigned char)s_filter[i]);
    b[i] = 0;
    return strstr(a, b) != NULL;
}

/* Filtered position -> index into ls_notes. */
static int visible_count(void)
{
    int n = 0;
    ls_note_info_t info;
    for (int i = 0; ls_notes_at(i, &info); i++) if (matches(&info)) n++;
    return n;
}

static int visible_index(int k)
{
    ls_note_info_t info;
    for (int i = 0, n = 0; ls_notes_at(i, &info); i++)
        if (matches(&info) && n++ == k) return i;
    return -1;
}

static void stamp_of(const char *stem, char *out, size_t cap)
{
    /* "0012_2026-09-25T14-21-00Z" -> "2026-09-25 14:21" */
    const char *u = strchr(stem, '_');
    out[0] = 0;
    if (!u || strlen(u) < 17 || !isdigit((unsigned char)u[1])) return;
    snprintf(out, cap, "%.10s %.2s:%.2s", u + 1, u + 12, u + 15);
}

static bool fresh_place(double *lat, double *lon)
{
    ls_field_sample_t p;
    ls_field_sample_snapshot(&p);
    if (!p.gps_valid) return false;
    *lat = p.lat; *lon = p.lon;
    return true;
}

static void open_app(const char *id)
{
    for (int k = 0; k < ls_app_count(); k++) {
        const ls_app_t *app = ls_app_at(k);
        if (app->id && !strcmp(app->id, id)) { ls_app_open(k); return; }
    }
}

/* -------------------------------------------------------------- saving -- */

static void save_now(void)
{
    if (!s_loaded || !s_tb.dirty) return;
    bool ok;
    if (s_new) {
        ok = ls_notes_create(s_text, s_info.stem, sizeof(s_info.stem));
        if (ok) s_new = false;
    } else ok = ls_notes_save(s_info.stem, s_text);
    if (ok) { ls_textbuf_mark_clean(&s_tb); ls_notes_summarize(s_text, &s_info); }
    say(ok ? "Saving to the SD card" : "Not saved yet: the card is busy, will retry");
}

static void open_note(int index)
{
    if (!ls_notes_at(index, &s_info)) return;
    /* The editor owns the buffer; the file is read into it after, because
       starting the editor empties it. */
    ls_textbuf_init(&s_tb, s_text, sizeof(s_text), NULL);
    if (!ls_notes_load(s_info.stem, s_text, sizeof(s_text))) {
        s_text[0] = 0;
        say("Could not read that note from the card"); return;
    }
    s_tb.len = (int)strlen(s_text);
    s_loaded = true; s_new = false;
    s_view = V_READ; s_read_scroll = 0; s_trash_armed_us = 0;
}

static void new_note(void)
{
    char stamp[LS_TIME_STAMP_MAX], seed[96];
    ls_time_render_stamp(stamp, sizeof(stamp));
    snprintf(seed, sizeof(seed), "# \n%s\n\n", stamp);
    ls_textbuf_init(&s_tb, s_text, sizeof(s_text), seed);
    s_tb.cursor = 2;                   /* type the title first */
    memset(&s_info, 0, sizeof(s_info));
    s_loaded = true; s_new = true;
    s_view = V_EDIT;
    s_dock = !ls_keypad_present();
    ls_keydock_reset(true);
    s_last_edit_us = esp_timer_get_time();
}

static void edit_note(void)
{
    s_view = V_EDIT;
    s_tb.cursor = s_tb.len; s_tb.want_col = -1;
    s_dock = !ls_keypad_present();
    ls_keydock_reset(false);
    s_last_edit_us = esp_timer_get_time();
}

static void leave_editor(void)
{
    save_now();
    s_view = V_READ; s_read_scroll = 0;
}

/* -------------------------------------------------------------- insert -- */

static void insert_line(const char *line)
{
    if (!line || !line[0]) { say("Nothing live to insert right now"); return; }
    /* Data lines start on a line of their own. */
    if (s_tb.cursor > 0 && s_text[s_tb.cursor - 1] != '\n') ls_textbuf_insert_char(&s_tb, '\n');
    if (!ls_textbuf_insert(&s_tb, line)) say("The note is full");
    s_last_edit_us = esp_timer_get_time();
}

enum { INS_TIME, INS_GPS, INS_HEADING, INS_MAP, INS_SENSORS, INS_CHECK, INS_HEADER, INS_RULE, INS_RADIO };
static const char *s_radio_group[6];
static int s_radio_n;

static void begin_map_capture(void)
{
    double lat, lon;
    if (!fresh_place(&lat, &lon)) ls_map_get_center(&lat, &lon);
    s_map_lat = lat; s_map_lon = lon;
    ls_map_center(lat, lon);
    s_view = V_MAP;
    say("Pan with the arrows, zoom with + and -, then SAVE");
}

static void insert_done(int i)
{
    char line[400];
    if (i >= INS_RADIO) {
        const int g = i - INS_RADIO;
        if (g < s_radio_n) { ls_note_live_radio(line, sizeof(line), s_radio_group[g]); insert_line(line); }
        return;
    }
    switch (i) {
    case INS_TIME:    ls_note_live_time(line, sizeof(line)); insert_line(line); break;
    case INS_GPS:     ls_note_live_gps(line, sizeof(line)); insert_line(line); break;
    case INS_HEADING: {
        ls_compass_reading_t r; ls_compass_live(&r);
        ls_note_fmt_heading(line, sizeof(line), r.magnetic, r.true_deg, r.tilt, r.calibrated);
        insert_line(line); break; }
    case INS_MAP:     begin_map_capture(); break;
    case INS_SENSORS: {
        ls_field_sample_t p; ls_field_sample_snapshot(&p);
        if (!p.imu_valid) { say("Motion sensor not answering"); break; }
        const float field = sqrtf(p.imu.mx * p.imu.mx + p.imu.my * p.imu.my + p.imu.mz * p.imu.mz);
        ls_note_fmt_sensors(line, sizeof(line), p.imu.ax, p.imu.ay, p.imu.az, field, p.imu.temp_c, NAN);
        insert_line(line); break; }
    case INS_CHECK:   insert_line("- [ ] "); break;
    case INS_HEADER:  insert_line("## "); break;
    case INS_RULE:    insert_line("---\n"); break;
    }
}

static void open_insert(void)
{
    ls_picker_open("INSERT AT CURSOR", insert_done);
    ls_picker_add("Time", "now, UTC when the clock is set");
    ls_picker_add("GPS position", "fix, altitude, accuracy");
    ls_picker_add("Heading", "compass, magnetic and true");
    ls_picker_add("Map picture", "saved beside the note as PNG");
    ls_picker_add("Sensors", "motion, magnetic field, temperature");
    ls_picker_add("Checklist item", "- [ ] tap it in the reader to tick");
    ls_picker_add("Heading line", "## a section title");
    ls_picker_add("Divider", "---");
    const char *labels[6];
    s_radio_n = ls_note_radio_groups(s_radio_group, labels, 6);
    for (int k = 0; k < s_radio_n; k++) {
        char label[32]; snprintf(label, sizeof(label), "Radio: %s", labels[k]);
        ls_picker_add(label, "what that radio is reporting now");
    }
    if (!s_radio_n) ls_picker_add("Radio", "no radio is reporting yet");
}

static void save_map_picture(void)
{
    if (ls_map_render_busy()) { say("Map still drawing; SAVE again in a moment"); return; }
    int w = 0, h = 0;
    const uint16_t *px = ls_map_render(&w, &h);
    char leaf[LS_NOTE_STEM + 16] = "";
    if (s_new) save_now();          /* a picture needs the note's name */
    if (px && w > 0 && h > 0 && s_info.stem[0]) {
        ls_notes_picture_leaf(s_info.stem, leaf, sizeof(leaf));
        char file[LS_NOTE_STEM + 24];
        snprintf(file, sizeof(file), "%s.png", leaf);
        double lat, lon; ls_map_get_center(&lat, &lon);
        char line[200];
        if (ls_notes_save_png(leaf, px, w, h)) {
            ls_note_fmt_map(line, sizeof(line), lat, lon, ls_map_zoom(), file);
            s_view = V_EDIT; insert_line(line); say("Map picture saved beside the note");
            return;
        }
        say("A picture is still being written; try again");
        return;
    }
    double lat, lon; ls_map_get_center(&lat, &lon);
    char line[200];
    ls_note_fmt_map(line, sizeof(line), lat, lon, ls_map_zoom(), NULL);
    s_view = V_EDIT; insert_line(line);
    say("No map archive on the card: position saved without a picture");
}

/* -------------------------------------------------------------- drawing -- */

static uint8_t tag_hue(const char *tag)
{
    if (!strncmp(tag, "GPS", 3) || !strncmp(tag, "MAP", 3)) return TUI_GREEN;
    if (!strncmp(tag, "RADIO", 5)) return TUI_MAGENTA;
    if (!strncmp(tag, "BEARING", 7) || !strncmp(tag, "FIX", 3) || !strncmp(tag, "HEADING", 7)) return TUI_YELLOW;
    return TUI_CYAN;
}

/* One source line, wrapped, as data where it is data. Returns rows used. */
static int draw_line(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *line, int n, int max_rows)
{
    char buf[400];
    int k = n < (int)sizeof(buf) - 1 ? n : (int)sizeof(buf) - 1;
    memcpy(buf, line, (size_t)k); buf[k] = 0;
    const uint8_t text = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    int indent = 0; uint8_t attr = text;
    const char *body = buf;
    if (buf[0] == '#') {
        int level = 0; while (buf[level] == '#') level++;
        body = buf + level; while (*body == ' ') body++;
        attr = TUI_ATTR((level == 1 ? TUI_CYAN : TUI_GREEN) | TUI_BRIGHT, TUI_BLACK);
    } else if (buf[0] == '>' ) {
        const char *tag = buf + 1; while (*tag == ' ') tag++;
        int tl = 0; while (tag[tl] && tag[tl] != ' ') tl++;
        char chip[16]; snprintf(chip, sizeof(chip), " %.*s ", tl > 10 ? 10 : tl, tag);
        const uint8_t hue = tag_hue(tag);
        tui_put_str(sf, clip, x, y, chip, TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT));
        indent = (int)strlen(chip) + 1;
        body = tag + tl; while (*body == ' ') body++;
        attr = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
    } else if (!strncmp(buf, "- [ ]", 5) || !strncmp(buf, "- [x]", 5) || !strncmp(buf, "- [X]", 5)) {
        const bool done = buf[3] != ' ';
        tui_put_str(sf, clip, x, y, done ? "[x]" : "[ ]",
                    TUI_ATTR((done ? TUI_GREEN : TUI_YELLOW) | TUI_BRIGHT, TUI_BLACK));
        indent = 4; body = buf + 5; while (*body == ' ') body++;
        attr = done ? LS_ATTR_DIM : text;
    } else if (!strcmp(buf, "---")) {
        for (int i = 0; i < w; i++) tui_put_char(sf, clip, x + i, y, '-', LS_ATTR_DIM);
        return 1;
    }
    const int bw = w - indent;
    if (bw < 4) return 1;
    int rows = 0, len = (int)strlen(body), start = 0;
    if (!len) return 1;
    while (start < len && rows < max_rows) {
        int end, next = ls_textbuf_line(body, len, start, bw, &end);
        char seg[400]; int sl = end - start;
        memcpy(seg, body + start, (size_t)sl); seg[sl] = 0;
        tui_put_str(sf, clip, x + indent, y + rows, seg, attr);
        rows++;
        if (next <= start) break;
        start = next;
    }
    return rows ? rows : 1;
}

static void draw_list(tui_surface *sf, tui_rect a)
{
    const int total = visible_count();
    if (s_sel >= total) s_sel = total ? total - 1 : 0;
    char find[48]; snprintf(find, sizeof(find), "%s", s_filter[0] ? s_filter : "ALL");
    ls_btn_t buttons[] = {{"NEW", "NOTE", 'n', false, false}, {"FIND", find, 'f', s_filter[0] != 0, false},
                          {"OPEN", NULL, 'o', false, !total}, {"COMPASS", "APP", 'c', false, false}};
    const int bar_h = ls_btn_raised_height(a, 4);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), buttons, 4, button_focus);
    tui_rect panel = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h - 1);
    char title[40]; snprintf(title, sizeof(title), "NOTES  %d", ls_notes_count());
    ls_panel_box(sf, panel, title, TUI_CYAN);
    s_list_area = tui_rect_make(panel.x + 2, panel.y + 2, panel.w - 4, panel.h - 3);
    const int per = 3, rows = s_list_area.h / per;
    s_row_count = 0;
    if (!total) {
        const char *lines[] = { ls_notes_scanning() ? "Reading the SD card..." : s_filter[0] ? "No note title matches." :
                                "A notebook for the field.", "", "NEW starts a note. INSERT drops in",
                                "your position, heading, a map", "picture or what a radio hears.",
                                "", "Notes are plain text files in", "/sdcard/notes on the card." };
        for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
            tui_put_str(sf, panel, s_list_area.x, s_list_area.y + 1 + (int)i, lines[i], LS_ATTR_DIM);
    }
    if (s_sel < s_first) s_first = s_sel;
    if (rows > 0 && s_sel >= s_first + rows) s_first = s_sel - rows + 1;
    for (int r = 0; r < rows && s_first + r < total; r++) {
        const int k = s_first + r, idx = visible_index(k);
        ls_note_info_t n;
        if (idx < 0 || !ls_notes_at(idx, &n)) break;
        const int y = s_list_area.y + r * per;
        if (s_row_count < (int)(sizeof(s_row_index) / sizeof(s_row_index[0]))) s_row_index[s_row_count++] = k;
        if (k == s_sel) ls_fill_dither(sf, tui_rect_make(s_list_area.x, y, s_list_area.w, 2), LS_DITHER_LIGHT, TUI_CYAN);
        tui_put_str(sf, panel, s_list_area.x + 1, y, n.title, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        char sub[120], when[24];
        if (n.when[0]) snprintf(when, sizeof(when), "%s", n.when);
        else stamp_of(n.stem, when, sizeof(when));
        snprintf(sub, sizeof(sub), "%s%s%s%s%s%s", when[0] ? when : n.stem,
                 n.badges & LS_NOTE_HAS_GPS ? "  GPS" : "", n.badges & LS_NOTE_HAS_MAP ? "  MAP" : "",
                 n.badges & LS_NOTE_HAS_RADIO ? "  RADIO" : "", n.badges & LS_NOTE_HAS_BEARING ? "  BEARING" : "",
                 n.badges & LS_NOTE_HAS_SENSORS ? "  SENSORS" : "");
        tui_put_str(sf, panel, s_list_area.x + 1, y + 1, sub, LS_ATTR_DIM);
        if (n.checks_open + n.checks_done) {
            char c[24]; snprintf(c, sizeof(c), "[%u/%u]", n.checks_done, n.checks_open + n.checks_done);
            const int cx = s_list_area.x + s_list_area.w - (int)strlen(c) - 1;
            tui_put_str(sf, panel, cx, y, c, TUI_ATTR((n.checks_open ? TUI_YELLOW : TUI_GREEN) | TUI_BRIGHT, TUI_BLACK));
        }
    }
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback[0] ? s_feedback : ls_notes_status(), LS_ATTR_DIM);
}

static void draw_read(tui_surface *sf, tui_rect a)
{
    const bool armed = s_trash_armed_us && esp_timer_get_time() - s_trash_armed_us < 4000000;
    ls_btn_t buttons[] = {{"EDIT", NULL, 'e', false, false}, {"MAP", "PLACE", 'm', false, !s_info.has_place},
                          {"GO TO", "COMPASS", 'g', false, !s_info.has_place},
                          {armed ? "SURE?" : "TRASH", armed ? "TAP AGAIN" : NULL, 't', armed, false},
                          {"BACK", "LIST", 'b', false, false}};
    const int bar_h = ls_btn_raised_height(a, 5);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), buttons, 5, button_focus);
    tui_rect panel = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h - 1);
    ls_panel_box(sf, panel, NULL, TUI_CYAN);
    s_read_area = tui_rect_make(panel.x + 2, panel.y + 1, panel.w - 4, panel.h - 2);
    for (unsigned i = 0; i < sizeof(s_read_line_at) / sizeof(s_read_line_at[0]); i++) s_read_line_at[i] = -1;
    int row = -s_read_scroll, total = 0;
    bool map_shown = false;
    for (const char *line = s_text; *line;) {
        const char *eol = strchr(line, '\n');
        const int n = eol ? (int)(eol - line) : (int)strlen(line);
        const int y = s_read_area.y + row;
        const int space = s_read_area.y + s_read_area.h - y;
        int used;
        if (row >= 0 && space > 0) {
            if (row < (int)(sizeof(s_read_line_at) / sizeof(s_read_line_at[0])))
                s_read_line_at[row] = (int)(line - s_text);
            used = draw_line(sf, s_read_area, s_read_area.x, y, s_read_area.w, line, n, space);
        } else {
            char tmp[400]; int k = n < 399 ? n : 399; memcpy(tmp, line, (size_t)k); tmp[k] = 0;
            used = 1; int e, s = 0, bw = s_read_area.w - (tmp[0] == '>' ? 12 : tmp[0] == '-' ? 4 : 0);
            if (bw < 4) bw = 4;
            while ((s = ls_textbuf_line(tmp, k, s, bw, &e)) < k) used++;
        }
        row += used; total += used;
        double lat, lon; int zoom; char file[64];
        if (!map_shown && ls_notes_parse_map(line, &lat, &lon, &zoom, file, sizeof(file))) {
            /* One live map per screen: the pane has a single picture slot. */
            map_shown = true;
            const int y0 = s_read_area.y + row;
            if (row >= 0 && y0 + MAP_ROWS <= s_read_area.y + s_read_area.h) {
                tui_rect box = tui_rect_make(s_read_area.x, y0, s_read_area.w, MAP_ROWS);
                ls_map_preview(sf, box, lat, lon);
                int px, py;
                if (ls_map_preview_point(lat, lon, box, &px, &py))
                    tui_put_char(sf, box, px, py, '+', TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
            }
            row += MAP_ROWS; total += MAP_ROWS;
        }
        if (!eol) break;
        line = eol + 1;
    }
    s_read_rows = total;
    if (!map_shown) ls_map_preview_leave();
    char foot[160];
    snprintf(foot, sizeof(foot), "%s%s", s_feedback[0] ? s_feedback : ls_notes_status(),
             s_info.checks_open ? "  |  tap [ ] to tick" : "");
    ls_safe_line(sf, a, a.y + a.h - 1, foot, LS_ATTR_DIM);
}

static void draw_edit(tui_surface *sf, tui_rect a)
{
    ls_btn_t buttons[] = {{"DONE", s_tb.dirty ? "SAVE" : "SAVED", 's', false, false},
                          {"INSERT", "DATA", 'i', false, false},
                          {"KEYS", s_dock ? "ON" : "OFF", 'k', s_dock, false}};
    const int bar_h = ls_btn_raised_height(a, 3);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), buttons, 3, button_focus);
    tui_rect body = tui_rect_make(a.x, a.y + bar_h, a.w, a.h - bar_h - 1);
    int dock_h = s_dock ? ls_keydock_height(body) : 0;
    s_dock_area = tui_rect_make(body.x, body.y + body.h - dock_h, body.w, dock_h);
    tui_rect panel = tui_rect_make(body.x, body.y, body.w, body.h - dock_h);
    ls_panel_box(sf, panel, s_new ? "NEW NOTE" : "EDIT", TUI_GREEN);
    s_text_area = tui_rect_make(panel.x + 2, panel.y + 1, panel.w - 4, panel.h - 2);
    const int w = s_text_area.w, rows = s_text_area.h;
    if (w < 4 || rows < 1) return;
    ls_textbuf_keep_visible(&s_tb, w, rows);
    /* Draw by visual row, colouring a data line's tag so the raw text still
       reads as what it is. */
    int start = ls_textbuf_row_start(&s_tb, w, s_tb.scroll);
    for (int r = 0; r < rows && start <= s_tb.len; r++) {
        int end, next = ls_textbuf_line(s_text, s_tb.len, start, w > 1 ? w - 1 : 1, &end);
        const bool line_start = start == 0 || s_text[start - 1] == '\n';
        uint8_t attr = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        if (line_start && s_text[start] == '#') attr = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
        else if (line_start && s_text[start] == '>') attr = TUI_ATTR(tag_hue(s_text + start + 2) | TUI_BRIGHT, TUI_BLACK);
        for (int i = start; i < end; i++)
            tui_put_char(sf, s_text_area, s_text_area.x + i - start, s_text_area.y + r, s_text[i], attr);
        if (next == start || (next >= s_tb.len && end >= s_tb.len)) break;
        start = next;
    }
    int crow, ccol;
    ls_textbuf_locate(&s_tb, w, &crow, &ccol);
    const bool blink = (esp_timer_get_time() / 500000) % 2 == 0;
    if (crow >= s_tb.scroll && crow < s_tb.scroll + rows && blink) {
        const int cx = s_text_area.x + ccol, cy = s_text_area.y + crow - s_tb.scroll;
        const char under = s_tb.cursor < s_tb.len && s_text[s_tb.cursor] != '\n' ? s_text[s_tb.cursor] : ' ';
        tui_put_char(sf, s_text_area, cx, cy, under, TUI_ATTR(TUI_BLACK, TUI_GREEN | TUI_BRIGHT));
    }
    if (dock_h) ls_keydock_draw(sf, s_dock_area);
    char foot[100];
    snprintf(foot, sizeof(foot), "%s  %d/%d", s_feedback[0] ? s_feedback : (s_tb.dirty ? "unsaved changes" : ls_notes_status()),
             s_tb.len, LS_NOTE_TEXT_MAX - 1);
    ls_safe_line(sf, a, a.y + a.h - 1, foot, LS_ATTR_DIM);
}

static void draw_map(tui_surface *sf, tui_rect a)
{
    ls_btn_t buttons[] = {{"SAVE", "PICTURE", 's', false, ls_map_render_busy()}, {"ZOOM", "-", '-', false, false},
                          {"ZOOM", "+", '+', false, false}, {"HERE", "GPS", 'h', false, false},
                          {"CANCEL", NULL, 'x', false, false}};
    const int bar_h = ls_btn_raised_height(a, 5);
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y, a.w, bar_h), buttons, 5, button_focus);
    tui_rect box = tui_rect_make(a.x + 1, a.y + bar_h + 1, a.w - 2, a.h - bar_h - 3);
    double lat, lon; ls_map_get_center(&lat, &lon);
    ls_map_preview(sf, box, lat, lon);
    int px, py;
    if (ls_map_preview_point(lat, lon, box, &px, &py))
        tui_put_char(sf, box, px, py, '+', TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
    char line[100];
    snprintf(line, sizeof(line), "%.5f, %.5f  z%d  %s", lat, lon, ls_map_zoom(), ls_map_render_busy() ? "drawing..." : ls_map_status());
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback[0] ? s_feedback : line, LS_ATTR_DIM);
}

static void draw(tui_surface *sf, tui_rect a)
{
    if (a.h < 16 || a.w < 30) {
        ls_panel_box(sf, a, "NOTES", TUI_CYAN);
        tui_put_str(sf, a, a.x + 1, a.y + 1, "Enlarge the pane", LS_ATTR_DIM);
        return;
    }
    if (s_view == V_EDIT && s_tb.dirty && esp_timer_get_time() - s_last_edit_us > AUTOSAVE_US) save_now();
    switch (s_view) {
    case V_LIST: draw_list(sf, a); break;
    case V_READ: draw_read(sf, a); break;
    case V_EDIT: draw_edit(sf, a); break;
    case V_MAP:  draw_map(sf, a); break;
    }
}

/* ---------------------------------------------------------------- input -- */

static void filter_done(const char *text) { snprintf(s_filter, sizeof(s_filter), "%s", text); s_sel = s_first = 0; }

static void toggle_check_at(int offset)
{
    if (offset < 0 || offset + 5 > s_tb.len || strncmp(s_text + offset, "- [", 3)) return;
    char *mark = s_text + offset + 3;
    if (*mark == ' ') *mark = 'x'; else if (*mark == 'x' || *mark == 'X') *mark = ' '; else return;
    s_tb.dirty = true; s_tb.len = (int)strlen(s_text);
    save_now();
    ls_notes_summarize(s_text, &s_info);
}

static void list_action(int i)
{
    s_feedback[0] = 0;
    if (i == 0) new_note();
    else if (i == 1) {
        if (s_filter[0]) { s_filter[0] = 0; s_sel = s_first = 0; }
        else ls_keyboard_open("FIND BY TITLE", "", sizeof(s_filter) - 1, filter_done);
    } else if (i == 2) { int idx = visible_index(s_sel); if (idx >= 0) open_note(idx); }
    else if (i == 3) open_app("compass");
}

static void read_action(int i)
{
    s_feedback[0] = 0;
    if (i == 0) edit_note();
    else if (i == 1 && s_info.has_place) { ls_map_center(s_info.lat, s_info.lon); open_app("map"); }
    else if (i == 2 && s_info.has_place) {
        ls_compass_set_target(s_info.lat, s_info.lon, s_info.title);
        open_app("compass");
    } else if (i == 3) {
        const int64_t now = esp_timer_get_time();
        if (s_trash_armed_us && now - s_trash_armed_us < 4000000) {
            if (ls_notes_trash(s_info.stem)) { s_view = V_LIST; s_loaded = false; say("Moved to notes/trash"); }
            s_trash_armed_us = 0;
        } else s_trash_armed_us = now;
    } else if (i == 4) { s_view = V_LIST; s_loaded = false; }
}

static void edit_action(int i)
{
    s_feedback[0] = 0;
    if (i == 0) leave_editor();
    else if (i == 1) open_insert();
    else if (i == 2) s_dock = !s_dock;
}

static void map_action(int i)
{
    s_feedback[0] = 0;
    if (i == 0) save_map_picture();
    else if (i == 1) ls_map_zoom_by(-1);
    else if (i == 2) ls_map_zoom_by(1);
    else if (i == 3) { double lat, lon; if (fresh_place(&lat, &lon)) ls_map_center(lat, lon); else say("No GPS fix"); }
    else if (i == 4) { s_view = V_EDIT; ls_map_center(s_map_lat, s_map_lon); }
}

static void action(int i)
{
    switch (s_view) {
    case V_LIST: list_action(i); break;
    case V_READ: read_action(i); break;
    case V_EDIT: edit_action(i); break;
    case V_MAP:  map_action(i); break;
    }
}

static bool edit_key(ls_tk_t k, char ch)
{
    const int w = s_text_area.w > 1 ? s_text_area.w : 40, rows = s_text_area.h > 0 ? s_text_area.h : 10;
    bool edited = false;
    switch (k) {
    case LS_TK_CHAR:      edited = ls_textbuf_insert_char(&s_tb, ch); if (!edited) say("The note is full"); break;
    case LS_TK_ENTER:     edited = ls_textbuf_insert_char(&s_tb, '\n'); break;
    case LS_TK_BACKSPACE: edited = ls_textbuf_backspace(&s_tb); break;
    case LS_TK_TAB:       edited = ls_textbuf_insert(&s_tb, "  "); break;
    case LS_TK_LEFT:      ls_textbuf_move(&s_tb, LS_TB_LEFT, w, rows); break;
    case LS_TK_RIGHT:     ls_textbuf_move(&s_tb, LS_TB_RIGHT, w, rows); break;
    case LS_TK_UP:        ls_textbuf_move(&s_tb, LS_TB_UP, w, rows); break;
    case LS_TK_DOWN:      ls_textbuf_move(&s_tb, LS_TB_DOWN, w, rows); break;
    case LS_TK_F5:        ls_textbuf_move(&s_tb, LS_TB_PAGE_UP, w, rows); break;
    case LS_TK_F6:        ls_textbuf_move(&s_tb, LS_TB_PAGE_DOWN, w, rows); break;
    case LS_TK_F1:        open_insert(); break;
    case LS_TK_F2:        save_now(); break;
    case LS_TK_ESC:       leave_editor(); break;
    default: return false;
    }
    if (edited) { s_last_edit_us = esp_timer_get_time(); s_feedback[0] = 0; }
    return true;
}

static bool key(ls_tk_t k, char ch)
{
    if (s_view == V_EDIT) {
        if (k == LS_TK_CHAR || k == LS_TK_ENTER || k == LS_TK_BACKSPACE || k == LS_TK_TAB ||
            k == LS_TK_LEFT || k == LS_TK_RIGHT || k == LS_TK_UP || k == LS_TK_DOWN ||
            k == LS_TK_ESC || k == LS_TK_F1 || k == LS_TK_F2 || k == LS_TK_F5 || k == LS_TK_F6)
            return edit_key(k, ch);
        return false;
    }
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT || k == LS_TK_TAB)
        return ls_btn_navigate(k, &button_slot, &button_focus, false);
    if (k == LS_TK_ENTER && button_focus >= 0) { if (ls_btn_enabled(0, button_focus)) action(button_focus); return true; }
    if (k == LS_TK_UP || k == LS_TK_DOWN) button_focus = -1;
    if (s_view == V_MAP) {
        if (k == LS_TK_UP) { ls_map_pan(0, -40); return true; }
        if (k == LS_TK_DOWN) { ls_map_pan(0, 40); return true; }
        if (k == LS_TK_BACKSPACE || k == LS_TK_ESC) { map_action(4); return true; }
        if (k == LS_TK_CHAR) { const char *p = strchr("s-+hx", ch); if (p) { map_action((int)(p - "s-+hx")); return true; } }
        return false;
    }
    if (s_view == V_READ) {
        if (k == LS_TK_UP) { if (s_read_scroll) s_read_scroll--; return true; }
        if (k == LS_TK_DOWN) { if (s_read_scroll + s_read_area.h < s_read_rows) s_read_scroll++; return true; }
        if (k == LS_TK_BACKSPACE || k == LS_TK_ESC) { read_action(4); return true; }
        if (k == LS_TK_CHAR) { const char *p = strchr("emgtb", tolower((unsigned char)ch)); if (p) { read_action((int)(p - "emgtb")); return true; } }
        return false;
    }
    const int total = visible_count();
    if (k == LS_TK_UP) { if (s_sel) s_sel--; return true; }
    if (k == LS_TK_DOWN) { if (s_sel + 1 < total) s_sel++; return true; }
    if (k == LS_TK_ENTER) { list_action(2); return true; }
    if (k == LS_TK_CHAR) { const char *p = strchr("nfoc", tolower((unsigned char)ch)); if (p) { list_action((int)(p - "nfoc")); return true; } }
    return false;
}

static bool touch(int col, int row)
{
    const int b = ls_btn_hit(col, row);
    if (b >= 0) { action(b); return true; }
    if (s_view == V_EDIT) {
        ls_tk_t k; char ch;
        if (s_dock && ls_keydock_touch(col, row, &k, &ch)) { if (k != LS_TK_NONE) edit_key(k, ch); return true; }
        if (col >= s_text_area.x && col < s_text_area.x + s_text_area.w &&
            row >= s_text_area.y && row < s_text_area.y + s_text_area.h) {
            s_tb.cursor = ls_textbuf_index_at(&s_tb, s_text_area.w, s_tb.scroll + row - s_text_area.y, col - s_text_area.x);
            s_tb.want_col = -1;
        }
        return true;
    }
    if (s_view == V_READ) {
        const int r = row - s_read_area.y;
        if (r >= 0 && r < (int)(sizeof(s_read_line_at) / sizeof(s_read_line_at[0])) && s_read_line_at[r] >= 0 &&
            col < s_read_area.x + 4)
            toggle_check_at(s_read_line_at[r]);
        return true;
    }
    if (s_view == V_LIST && col >= s_list_area.x && col < s_list_area.x + s_list_area.w && row >= s_list_area.y) {
        const int r = (row - s_list_area.y) / 3;
        if (r >= 0 && r < s_row_count) {
            const int k = s_row_index[r];
            if (k == s_sel) { int idx = visible_index(k); if (idx >= 0) open_note(idx); }
            else s_sel = k;
        }
    }
    return true;
}

/* Journal entries from before NOTES become notes, once. The old files stay
   where they were. */
static void import_journal(void)
{
    char marker[160];
    snprintf(marker, sizeof(marker), "%s/.journal-imported", ls_notes_directory());
    struct stat st;
    if (stat(marker, &st) == 0) return;
    /* The marker first, in a folder made for it: an import that cannot
       record itself would run again on the next visit and double every
       entry. No marker, no import. */
#ifdef _WIN32
    mkdir(ls_notes_directory());
#else
    mkdir(ls_notes_directory(), 0775);
#endif
    FILE *m = fopen(marker, "wb");
    if (!m) return;
    fputs("importing\n", m);
    fclose(m);
    EXT_RAM_BSS_ATTR static ls_journal_entry_t e;
    EXT_RAM_BSS_ATTR static char body[LS_JOURNAL_TEXT + 400];
    int imported = 0;
    for (int i = LS_JOURNAL_CAP - 1; i >= 0; i--) {
        if (!ls_field_entry(i, &e)) continue;
        /* Dated as the entry was, not as the import is. */
        char when[40];
        if (e.sample.utc[0]) snprintf(when, sizeof(when), "%s", e.sample.utc);
        else snprintf(when, sizeof(when), "from JOURNAL, uptime %.0f s", e.sample.time_us / 1e6);
        int n = snprintf(body, sizeof(body), "# %s\n%s\n\n%s\n", e.title, when, e.text);
        if (e.sample.gps_valid && n > 0 && (size_t)n < sizeof(body))
            n += snprintf(body + n, sizeof(body) - (size_t)n, "> GPS %.6f, %.6f  alt %.0f m\n", e.sample.lat, e.sample.lon, e.sample.alt_m);
        if (e.sample.signal_valid && n > 0 && (size_t)n < sizeof(body))
            snprintf(body + n, sizeof(body) - (size_t)n, "> RADIO %s  %.4f MHz  %.0f dBm\n",
                     ls_field_source_name(e.sample.source), e.sample.frequency / 1e6, e.sample.rssi);
        if (ls_notes_create(body, NULL, 0)) imported++;
    }
    FILE *f = fopen(marker, "wb");
    if (f) { fprintf(f, "%d\n", imported); fclose(f); }
}

static void enter(void)
{
    button_focus = -1; button_slot = 0; s_feedback[0] = 0;
    ls_field_start(); ls_field_watch(true);
    ls_notes_start();
    import_journal();
    if (s_view == V_MAP) s_view = V_EDIT;
}

static void leave(void)
{
    if (s_view == V_EDIT || s_view == V_MAP) save_now();
    ls_map_preview_leave();
    ls_field_watch(false);
}

const ls_tui_screen_t ls_scr_notes = {
    .name = "NOTES", .hint = "N new  F find  ENTER open  F1 insert  ESC done",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
};
