/* Router. See ls_tui_screen.h for the contract it enforces. */
#include "ls_tui_screen.h"
#include "ls_keyboard.h"
#include "ls_notify.h"
#include "ls_numpad.h"
#include "ls_picker.h"
#include "ls_tui_chrome.h"
#include "ls_gauge.h"
#include "ls_tui_ui.h"

#include <stdio.h>
#include <string.h>

#include "ls_anim.h"
#include "ls_theme.h"

#define MAX_SCREENS LS_TUI_MAX_SCREENS

static int tab_rows(void)
{
    /* Four rows in portrait, not two. */

    return ls_tui_is_wide() ? 1 : 4;
}

/* Portrait has no hint row. */

static int hint_rows(void)
{
    return ls_tui_is_wide() ? 1 : 0;
}

static const ls_tui_screen_t *s_screens[MAX_SCREENS];
static int  s_count;
/* Screens on the tab strip; see the header. 0 means all of them. */
static int  s_tab_count;

static int8_t s_tab_list[MAX_SCREENS];
static int    s_tab_list_n;

void ls_tui_screen_set_tab_count(int n)
{
    s_tab_count = n;
    s_tab_list_n = 0;               /* a count and a list are alternatives */
}

/* Which screens the strip offers, when it is not just the first few. */

/* Where a screen ended up in the register.

   Needed because the tab list is written in terms of screens and everything
   else in the interface is written in terms of apps, and the two are
   registered together but numbered separately. Looking the pointer up is
   exact; counting registrations by hand is the kind of thing that is right
   until somebody inserts a row. */
int ls_tui_screen_index_of(const ls_tui_screen_t *screen)
{
    if (!screen) return -1;
    for (int i = 0; i < s_count; i++) if (s_screens[i] == screen) return i;
    return -1;
}

void ls_tui_screen_set_tabs(const int *indices, int n)
{
    s_tab_list_n = 0;
    if (!indices) return;
    for (int i = 0; i < n && s_tab_list_n < MAX_SCREENS; i++) {
        if (indices[i] < 0 || indices[i] >= s_count) continue;
        s_tab_list[s_tab_list_n++] = (int8_t)indices[i];
    }
}

static int tab_count(void)
{
    if (s_tab_list_n > 0) return s_tab_list_n;
    if (s_tab_count <= 0 || s_tab_count > s_count) return s_count;
    return s_tab_count;
}

/* Strip position to screen index. The identity when there is no list, which
   is what keeps the "first n" behaviour exactly as it was. */
static int tab_screen(int slot)
{
    if (s_tab_list_n > 0)
        return (slot >= 0 && slot < s_tab_list_n) ? s_tab_list[slot] : 0;
    return slot;
}
static int  s_current;
static char s_status_left[64];
static char s_status_right[48];

/* Help is a router concern, not a screen. */

static bool s_help_open;

/* Where each tab landed this frame, so a tap can be resolved back to a
   screen without recomputing the label widths and getting a different
   answer than the one on the glass. */
static int16_t s_tab_x0[MAX_SCREENS], s_tab_x1[MAX_SCREENS];

/* How much a rotation cost. */

static uint32_t s_rebuilds;
static uint32_t s_rebuild_ms;

void ls_tui_rebuild_note(uint32_t ms) { s_rebuilds++; s_rebuild_ms = ms; }
uint32_t ls_tui_rebuild_count(void)   { return s_rebuilds; }
uint32_t ls_tui_rebuild_ms(void)      { return s_rebuild_ms; }

int ls_tui_screen_register(const ls_tui_screen_t *screen)
{
    if (!screen || !screen->draw || s_count >= MAX_SCREENS) return -1;
    s_screens[s_count] = screen;
    return s_count++;
}

int ls_tui_screen_count(void)   { return s_count; }
int ls_tui_screen_current(void) { return s_current; }
const char *ls_tui_screen_name(int index)
{
    if (index < 0 || index >= s_count || !s_screens[index]) return NULL;
    return s_screens[index]->name;
}

static bool (*s_dispatch_cb)(int);

void ls_tui_screen_set_dispatch_cb(bool (*cb)(int)) { s_dispatch_cb = cb; }

void ls_tui_screen_show(int index)
{
    if (index < 0 || index >= s_count) return;
    /* A console switch must not release a radio during draw. */
    if (s_dispatch_cb && s_dispatch_cb(index)) return;
    if (index == s_current) return;
    if (s_screens[s_current] && s_screens[s_current]->leave)
        s_screens[s_current]->leave();
    s_current = index;
    ls_btn_clear_hits();

    /* The left status belongs to the screen, so it goes with it. */

    s_status_left[0] = 0;
    if (s_screens[s_current] && s_screens[s_current]->enter)
        s_screens[s_current]->enter();

    /* Ask for the receiver this screen needs, or for none. */

    const ls_tui_screen_t *now = s_screens[s_current];
    if (now && now->radio) ls_tui_radio_want(now->radio);
    /* A screen change replaces everything, so do not let the diff try to be
       clever about it - the old screen's cells are not a useful baseline. */
    ls_tui_invalidate();
}

void ls_tui_screen_next(void) { if (s_count) ls_tui_screen_show((s_current + 1) % s_count); }
void ls_tui_screen_prev(void) { if (s_count) ls_tui_screen_show((s_current + s_count - 1) % s_count); }

void ls_tui_status_set(const char *left, const char *right)
{
    if (left)  snprintf(s_status_left,  sizeof(s_status_left),  "%s", left);
    if (right) snprintf(s_status_right, sizeof(s_status_right), "%s", right);
}

bool ls_tui_is_wide(void)
{
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    return cols > rows;
}

/* ------------------------------------------------------------------ chrome */

/* The battery, drawn as a battery. */

static void draw_battery(tui_surface *sf, tui_rect all, int x, uint8_t bar)
{
    ls_gauge_t g;
    if (!ls_gauge_get(&g) || !g.present) return;

    uint8_t hue;
    if (g.charging)        hue = TUI_CYAN   | TUI_BRIGHT;
    else if (g.percent > 40) hue = TUI_GREEN  | TUI_BRIGHT;
    else if (g.percent > 15) hue = TUI_YELLOW | TUI_BRIGHT;
    else                     hue = TUI_RED    | TUI_BRIGHT;

    const uint8_t fill = TUI_ATTR(hue, TUI_BLACK);

    tui_put_char(sf, all, x, 0, '[', bar);
    /* Two cells, four quadrants each, so eight steps across 0..100. */
    const int eighths = (g.percent * 8 + 50) / 100;
    for (int c = 0; c < 2; c++) {
        const int here = eighths - c * 4;
        char glyph;
        if (here >= 4)      glyph = LS_TUI_BLOCK_FULL;
        else if (here == 3) glyph = LS_TUI_QUAD(1, 0, 1, 1);
        else if (here == 2) glyph = LS_TUI_BLOCK_LEFT;
        else if (here == 1) glyph = LS_TUI_QUAD(1, 0, 0, 0);
        else                glyph = ' ';
        tui_put_char(sf, all, x + 1 + c, 0, glyph, glyph == ' ' ? bar : fill);
    }
    tui_put_char(sf, all, x + 3, 0, ']', bar);

    char pct[8];
    snprintf(pct, sizeof(pct), "%2u%%%s", (unsigned)g.percent,
             g.charging ? "+" : "");
    tui_put_str(sf, all, x + 4, 0, pct, bar);
}

/* The status row's words stop short of the corners; its bar does not. */

static void status_span(int cols, int *x0, int *x1)
{
    const int pad = ls_tui_corner_pad(0);
    *x0 = pad;
    *x1 = cols - pad;
}

/* What the clock slot shows. Set by whoever knows the time; empty
   draws nothing, which is what the bench and a board with no clock get. */
static char s_status_clock[LS_TUI_CLOCK_W + 1];

void ls_tui_status_set_clock(const char *text)
{
    if (!text) text = "";
    strncpy(s_status_clock, text, LS_TUI_CLOCK_W);
    s_status_clock[LS_TUI_CLOCK_W] = '\0';
}

static void draw_status(tui_surface *sf, int cols)
{
    const uint8_t bar = TUI_ATTR(TUI_BLACK, TUI_CYAN);
    tui_rect all = tui_surface_rect(sf);
    tui_fill(sf, tui_rect_make(0, 0, cols, 1), ' ', bar);

    int x0, x1;
    status_span(cols, &x0, &x1);

    const int batt_w = 9;
    int right_edge = x1 - 1;
    if (ls_gauge_present()) {
        draw_battery(sf, all, x1 - batt_w, bar);
        right_edge = x1 - batt_w - 1;
    }

    static const char BRAND[] = "LAKESHARK";
    const ls_tui_screen_t *s = s_screens[s_current];
    const char *name = (s && s->name) ? s->name : "";

    ls_tui_status_layout_t l = ls_tui_status_layout_at(
        x0, right_edge + 1 - x0, (int)sizeof(BRAND) - 1, (int)strlen(name),
        (int)strlen(s_status_left), (int)strlen(s_status_right));

    /* The clock where the turn control was (), in the bar's
       own colours because it is information. [?] keeps the inverse because
       it is still a control, and a control should look like one. */
    if (l.clock_x >= 0 && s_status_clock[0])
        tui_put_str(sf, all, l.clock_x, 0, s_status_clock, bar);
    if (l.help_x >= 0) {
        const uint8_t knob = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
        tui_put_str(sf, all, l.help_x, 0, "[?]", knob);
    }

    if (l.brand_x >= 0) tui_put_str(sf, all, l.brand_x, 0, BRAND, bar);
    if (l.name_x  >= 0) tui_put_str(sf, all, l.name_x,  0, name, bar);
    if (l.left_x  >= 0) tui_put_str(sf, all, l.left_x,  0, s_status_left, bar);
    if (l.right_x >= 0) tui_put_str(sf, all, l.right_x, 0, s_status_right, bar);

    /* The unread count, and it outlives the banner. */

    const int unread = ls_notify_unread();
    if (unread > 0 && l.brand_x >= 0) {
        char badge[12];
        snprintf(badge, sizeof(badge), "%d MSG", unread > 99 ? 99 : unread);
        for (int i = 0; i < (int)sizeof(BRAND) - 1; i++)
            tui_put_char(sf, all, l.brand_x + i, 0, ' ', bar);

        const bool inv = ls_notify_badge_inverted();
        tui_put_str(sf, all, l.brand_x, 0, badge,
                    inv ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                        : TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
    }

}

/* The tab strip doubles as the touch target row in portrait, which is why the
   app number is drawn: a finger has nothing else to aim at, and the number is
   also the function key, so the two input methods teach each other. */
/* The strip is made of the same thing every other control is. */

static void tab_box(tui_surface *sf, tui_rect r, bool on,
                    const char *num, const char *name)
{
    if (r.w < 2 || r.h < 1) return;

    const uint8_t hue = on ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;
    /* The number stays white in both states: it is the function key and the
       touch target, and the green is already saying which one is live. */
    const uint8_t face = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    const uint8_t sub = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    ls_panel_box(sf, r, NULL, hue);
    tui_rect f = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    if (f.w <= 0 || f.h <= 0) {
        /* A one or two row strip has no interior. Landscape is that strip,
           and there the name alone on a plain ground is the whole control. */
        if (name && r.w > 2)
            tui_put_str(sf, tui_surface_rect(sf),
                        r.x + (r.w - (int)strlen(name)) / 2, r.y, name, face);
        return;
    }

    ls_fill_dither(sf, f, on ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);

    /* The number and the name, centred together. The number is the function
       key AND the touch target, so it is the part that survives a narrow
       grid; the name is the convenience. */
    const bool named = (name && name[0] && f.h >= 2 && f.w >= 4);
    const int lines = named ? 2 : 1;
    const int ly = f.y + (f.h - lines) / 2;
    if (num) ls_dither_label(sf, f, ly - f.y, num, face);
    if (named) ls_dither_label(sf, f, ly + 1 - f.y, name, sub);
}

static void draw_tabs(tui_surface *sf, int cols, int row, int height)
{
    tui_rect all = tui_surface_rect(sf);
    /* Narrow grids get the number only. The name is a convenience; the number
       is the function key and the touch target, so it is the part that must
       survive. */
    const bool wide = cols >= 60;

    const int ntab = tab_count();

    if (!wide && ntab > 0) {
        const int cw = cols / ntab;
        /* The two lines sit in the middle of the tab, not at its top.

           Portrait gives this strip four rows and it drew the number on the
           first and the name on the second, leaving two rows of coloured
           nothing underneath every tab. That is the same fault as the home
           tiles and the same fault the button bar had: the target grew and
           what is written on it stayed where it was. Anything filled and
           taller than its text centres now. */
        /* Neighbouring tabs SHARE their border column, the way the
           quick controls do. Drawn a column apart the seam reads as "| |",
           which looks like a gap that means something and does not. */
        const int step = (cols - 1) / ntab;
        for (int i = 0; i < ntab; i++) {
            const int x0 = i * step;
            const int w = (i == ntab - 1) ? cols - x0 : step + 1;
            const int scr = tab_screen(i);

            char label[8];
            snprintf(label, sizeof(label), "%d", i + 1);
            char nm[10];
            snprintf(nm, sizeof(nm), "%.*s", (w > 3) ? w - 3 : 1,
                     s_screens[scr]->name);

            tab_box(sf, tui_rect_make(x0, row, w, height),
                    scr == s_current, label, nm);

            s_tab_x0[i] = (int16_t)x0;
            s_tab_x1[i] = (int16_t)((i == ntab - 1) ? cols - 1
                                                    : x0 + step - 1);
        }
        (void)cw;
        (void)all;
        return;
    }

    /* Landscape gets one row, so there is no box to draw and no
       interior to dither. The language survives anyway: black ground like
       every other row on the screen, the current app filled in green because
       green is what this interface means by "this is the live one", and the
       rest plain. What it must NOT be is a blue band, which is the only
       thing it had in common with the old strip. */
    const uint8_t plain = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t here  = TUI_ATTR(TUI_BLACK, TUI_GREEN | TUI_BRIGHT);
    tui_fill(sf, tui_rect_make(0, row, cols, height), ' ',
             TUI_ATTR(TUI_WHITE, TUI_BLACK));
    const int ly = row + (height - 1) / 2;   /* centred, same reason */
    int x = 1;
    for (int i = 0; i < ntab && x < cols - 4; i++) {
        const int scr = tab_screen(i);
        char label[24];
        snprintf(label, sizeof(label), " %d %s ", i + 1, s_screens[scr]->name);
        uint8_t a = (scr == s_current) ? here : plain;
        tui_put_str(sf, all, x, ly, label, a);
        s_tab_x0[i] = (int16_t)x;
        s_tab_x1[i] = (int16_t)(x + (int)strlen(label) - 1);
        x += (int)strlen(label) + 1;
    }
}

static void draw_hints(tui_surface *sf, int cols, int row)
{
    tui_rect all = tui_surface_rect(sf);
    /* The hint row is the one place BR_BLACK is a BACKGROUND, and
       muted white on it is about 3.5:1 - still short for a row of small
       text. Bright white on the same bar is 7.5:1 and the bar still reads
       as a bar. */
    const uint8_t bg  = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, (TUI_BLACK | TUI_BRIGHT));
    const uint8_t key = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, (TUI_BLACK | TUI_BRIGHT));
    tui_fill(sf, tui_rect_make(0, row, cols, 1), ' ', bg);

    const ls_tui_screen_t *s = s_screens[s_current];
    const char *hint = (s && s->hint) ? s->hint : "";

    /* The hint string is a run of "KEY label" pairs separated by two spaces.
       Colouring the key differently from its label is what makes the row
       scannable instead of a wall of text, and it is why the separator is two
       spaces: one would be ambiguous with the space inside a label. */
    /* The stop column comes from the same arithmetic that places the
       tail, so lengthening the tail cannot leave the two overlapping. The
       hardcoded `cols - 11` stopped the hint eight columns past the start of
       an eighteen character tail, and the renderer only pushes changed cells,
       so whichever wrote last stayed on the panel. */

    const int pad = ls_tui_corner_pad(row);
    const int in = pad > 1 ? pad - 1 : 0;
    ls_tui_hint_layout_t hl = ls_tui_hint_layout(cols - 2 * in);

    /* Whole groups, never half a word. See ls_tui_hint_fit. */
    const int fit = ls_tui_hint_fit(hint, hl.hint_end_x - 1);

    int x = 1 + in;
    bool in_key = true;
    for (int i = 0; i < fit; i++) {
        const char *p = hint + i;
        if (p[0] == ' ' && p[1] == ' ') in_key = true;
        else if (p[0] == ' ' && in_key)  in_key = false;
        tui_put_char(sf, all, x++, row, *p, in_key ? key : bg);
    }
    if (hl.tail_x >= 0)
        tui_put_str(sf, all, hl.tail_x + in, row, hl.tail, key);
}

static void draw_help(tui_surface *sf, int cols, int rows)
{
    tui_rect all = tui_surface_rect(sf);
    const uint8_t frame = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t key   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t text  = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    int w = 46, h = 14;
    int x = (cols - w) / 2, y = (rows - h) / 2;
    tui_fill(sf, tui_rect_make(x, y, w, h), ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
    tui_box(sf, tui_rect_make(x, y, w, h), "KEYS", frame);

    static const char *const K[][2] = {
        { "F1..F4",   "the numbered tabs along the top" },
        { "TAB",      "next page in this app" },
        { "SHIFT TAB","previous page" },
        { "UP DOWN",  "move the selection" },
        { "ENTER",    "act on the selection" },
        { "ESC",      "back, or close this" },
        { "F9",       "next colour theme" },
        { "F10",      "this list" },
        { "F11",      "rotate the screen" },
    };
    for (unsigned i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        tui_put_str(sf, all, x + 2, y + 2 + (int)i, K[i][0], key);
        tui_put_str(sf, all, x + 14, y + 2 + (int)i, K[i][1], text);
    }
    tui_put_str(sf, all, x + 2, y + h - 2,
                "turn the board and the screen turns with it", text);
}

void ls_tui_router_draw(tui_surface *sf)
{
    if (!sf || !s_count) return;
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);

    const int th = tab_rows();

    ls_notify_poll(s_current);

    tui_frame_begin(sf);
    ls_btn_clear_hits();
    draw_status(sf, cols);
    draw_tabs(sf, cols, 1, th);

    tui_rect area = tui_rect_make(0, 1 + th, cols, rows - 1 - th - hint_rows());

    /* The opening animation draws instead of the screen, not over it.
       A screen half-visible behind an animation is worse than either, and
       drawing both doubles the cells the diff has to push in the one frame
       where something is already moving. */
    if (area.h > 0 && ls_anim_draw(sf, area)) {
        if (hint_rows()) draw_hints(sf, cols, rows - 1);
        return;
    }

    if (area.h > 0 && s_screens[s_current] && s_screens[s_current]->draw)
        s_screens[s_current]->draw(sf, area);

    if (hint_rows()) draw_hints(sf, cols, rows - 1);
    if (s_help_open) draw_help(sf, cols, rows);
    /* The keypad is drawn last and over the screen's own area, not
       over the chrome: the tab strip stays readable so it is obvious which
       app is being tuned, and the status row keeps saying what the radio is
       doing while a new frequency is typed into it. */
    if (ls_numpad_active()) ls_numpad_draw(sf, area);
    /* The text keyboard, over the screen's own area for the same
       reasons as the number pad: the tab strip stays readable so it is
       obvious which app is being typed into, and the status row keeps
       saying what the radios are doing while a message is written. */
    if (ls_keyboard_active()) ls_keyboard_draw(sf, area);
    /* The place list, over the screen's own area for the same
       reason: the tab strip stays readable so it is obvious which app is
       being driven, and the status row keeps saying what the radios are
       doing while somebody chooses where to go. */
    if (ls_picker_active()) ls_picker_draw(sf, area);
    /* The banner over everything, including the overlays: a message
       that arrives while somebody is typing one is still a message, and the
       overlays are the screens people spend the longest on. */
    if (area.h > 0) ls_notify_draw(sf, area);
}

static void (*s_rotate_cb)(void);

void ls_tui_screen_set_rotate_cb(void (*cb)(void)) { s_rotate_cb = cb; }

static void (*s_regrid_cb)(void);
void ls_tui_screen_set_regrid_cb(void (*cb)(void)) { s_regrid_cb = cb; }
void ls_tui_screen_request_regrid(void) { if (s_regrid_cb) s_regrid_cb(); }
bool ls_tui_screen_can_regrid(void) { return s_regrid_cb != NULL; }

/* A tap that did something should be felt. */

static void (*s_tap_cb)(void);
void ls_tui_set_tap_cb(void (*cb)(void)) { s_tap_cb = cb; }

static bool router_touch_dispatch(int col, int row);

bool ls_tui_router_touch(int col, int row)
{
    const bool took = router_touch_dispatch(col, row);
    if (took && s_tap_cb) s_tap_cb();
    return took;
}

static bool router_touch_dispatch(int col, int row)
{

    /* The banner first, because it is drawn over everything. Only
       taps that land ON it are taken; everything else falls through to what
       it is covering. */
    if (ls_notify_touch(col, row)) return true;
    if (ls_numpad_active()) return ls_numpad_touch(col, row);
    if (ls_keyboard_active()) return ls_keyboard_touch(col, row);
    if (ls_picker_active()) return ls_picker_touch(col, row);

    if (s_help_open) { s_help_open = false; return true; }
    /* A tap during an opening skips it. Anything that cannot be interrupted
       is in the way. */
    if (ls_anim_active()) { ls_anim_cancel(); return true; }

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    const int th = tab_rows();

    if (row == 0) {

        /* From the same span the row was drawn in, so on a panel
           with rounded corners the target moves in with its label rather
           than staying at column 0, where there is now only bar. */
        int x0, x1;
        status_span(cols, &x0, &x1);
        const ls_tui_status_layout_t l = ls_tui_status_layout_at(
            x0, x1 - x0, 9, 0, 0, 0);

        if (l.help_x >= 0 && col >= l.help_x &&
            col < l.help_x + LS_TUI_HELP_W) {
            /* Portrait drew a hint row for the sole purpose of being
               tapped for help. This is that gesture, without the row. */
            s_help_open = true;
        }
        return true;
    }
    if (row >= 1 && row < 1 + th) {
        for (int i = 0; i < tab_count(); i++)
            if (col >= s_tab_x0[i] && col <= s_tab_x1[i]) {
                ls_tui_screen_show(tab_screen(i));
                return true;
            }
        return true;                    /* a miss on the strip is not a tap
                                           that should reach the screen */
    }
    if (hint_rows() && row == rows - 1) { s_help_open = true; return true; }

    const int body_top = 1 + th, body_h = rows - 1 - th - hint_rows();
    if (row < body_top || row >= body_top + body_h) return false;

    /* A screen that drew controls hit-tests its own rects. Only a
       screen that did not gets the fallback below, and the fallback is
       deliberately crude: it is for lists, and anything richer than a list
       should be answering for itself. */
    const ls_tui_screen_t *s = s_screens[s_current];
    if (s && s->touch && s->touch(col, row)) return true;

    if (s && s->key && body_h > 2) {
        int rel = row - body_top;
        ls_tk_t k = rel < body_h / 3        ? LS_TK_UP
                  : rel > body_h * 2 / 3    ? LS_TK_DOWN
                                            : LS_TK_ENTER;
        if (s->key(k, 0)) return true;
    }
    return false;
}

bool ls_tui_router_key(ls_tk_t key, char ch)
{
    /* Help swallows everything while it is up, so there is always a way out
       of it and it can never trap a key the screen underneath wanted. */
    /* ESC clears a banner before anything else sees it, and no
       other key is taken. See ls_notify_key. */
    if (ls_notify_key(key)) return true;

    if (s_help_open) {
        if (key == LS_TK_ESC || key == LS_TK_F10 || key == LS_TK_ENTER)
            s_help_open = false;
        return true;
    }
    if (ls_anim_active()) { ls_anim_cancel(); return true; }

    /* The screen gets first refusal. Only what it does not want reaches the
       global bindings, so a screen with a text field can keep TAB. */
    const ls_tui_screen_t *s = s_screens[s_current];
    /* The keypad takes everything while it is up. Before the
       screen, because the screen's own shortcuts are letters and a keypad
       that let 'f' through would open a second one on top of itself. */
    if (ls_numpad_active()) return ls_numpad_key(key, ch);
    /* Before the screen, because every letter is text while it is
       up and a screen that still saw them would be running its own
       shortcuts while somebody wrote a message. */
    if (ls_keyboard_active()) return ls_keyboard_key(key, ch);
    /* Before the screen, because the list filters on letters and a
       screen that still saw them would be running its own shortcuts while
       somebody typed a place name. */
    if (ls_picker_active()) return ls_picker_key(key, ch);

    if (s && s->key && s->key(key, ch)) return true;

    switch (key) {
    case LS_TK_F1: case LS_TK_F2: case LS_TK_F3: case LS_TK_F4:
    case LS_TK_F5: case LS_TK_F6: case LS_TK_F7: case LS_TK_F8: {
        /* The function keys ARE the numbers printed on the strip. */

        const int slot = key - LS_TK_F1;
        if (slot < tab_count()) ls_tui_screen_show(tab_screen(slot));
        return true;
    }
    case LS_TK_F9:
        /* The same step the Theme row takes, and like that row it
           leaves Daylight alone: under Daylight it moves the theme Daylight
           will hand back to. F9 is a keyboard key, so it is pressed in
           landscape, and the landscape status row names that theme beside
           the word Daylight - the press is not silent. */
        ls_tui_set_theme(ls_tui_theme_next(ls_tui_get_theme()));
        return true;
    case LS_TK_F11:
        if (s_rotate_cb) s_rotate_cb();
        return true;
    case LS_TK_F10:
        s_help_open = true;
        return true;
    case LS_TK_TAB:
        ls_tui_screen_next();
        return true;
    case LS_TK_ESC:
        /* ESC that nothing else wanted goes to the directory. Screen 0
           is the directory by registration order, and having one key that
           always lands somewhere known is what makes an unfamiliar screen
           safe to poke at. */
        ls_tui_screen_show(0);
        return true;
    default:
        return false;
    }
}
