/* Display Properties, as Windows 95 had it: Background, Appearance,
   Effects and Settings, each previewed live from a copy of the choices that
   only OK or Apply put on the desk (and in NVS). Cancel forgets them. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>

enum { TAB_BG, TAB_LOOK, TAB_FX, TAB_SET, TAB_N };
static const char *const TABS[TAB_N] = {"Background", "Appearance", "Effects", "Settings"};
enum {
    H_TAB = 1, H_OK, H_CANCEL, H_APPLY, H_PATTERN, H_WALL, H_FIT, H_SCHEME, H_ITEM, H_FG, H_BG, H_RESET,
    H_LIVE, H_ANIM, H_LABELS,
};

static dk_prefs_t s_edit;
static int s_tab, s_item;
static bool s_dirty;
typedef struct { tui_rect r; uint8_t kind; int16_t arg; } hit_t;
static hit_t s_hits[72];
static int s_nh;
static char s_files[8][40];
static const char *s_fnames[8];
static uint32_t s_fids[8];
static int s_nfiles;

void dk_props_tab(int tab) { if (tab >= 0 && tab < TAB_N) s_tab = tab; }

void dk_props_open(void)
{
    s_edit = *dk_prefs();
    s_edit.wall = dk_wall_current();
    s_tab = TAB_BG;
    s_item = DK_IT_DESKTOP;
    s_dirty = false;
    s_nfiles = dk_wall_files(s_fnames, s_fids, 8);
    for (int i = 0; i < s_nfiles; i++) {
        snprintf(s_files[i], sizeof(s_files[i]), "%s", s_fnames[i]);
        s_fnames[i] = s_files[i];
    }
}

static void hit(tui_rect r, int kind, int arg)
{
    if (s_nh < (int)(sizeof(s_hits) / sizeof(s_hits[0]))) s_hits[s_nh++] = (hit_t){r, (uint8_t)kind, (int16_t)arg};
}

/* ------------------------------------------------------------ drawing -- */

static void tabs(tui_surface *sf, tui_rect c, tui_rect clip)
{
    const uint8_t hi = dk_attr(R_HILITE), lo = dk_attr(R_SHADOW);
    int x = c.x + 1;
    /* the panel's top line, open under the chosen tab */
    dk_hline(sf, clip, c.x, c.y + 2, c.w, LS_BOX_H, hi);
    for (int t = 0; t < TAB_N; t++) {
        const int w = (int)strlen(TABS[t]) + 4;
        const bool on = t == s_tab;
        const int top = c.y + (on ? 0 : 1);
        ls_box_put(sf, clip, x, top, LS_BOX_ARC_DR, hi);
        dk_hline(sf, clip, x + 1, top, w - 2, LS_BOX_H, hi);
        ls_box_put(sf, clip, x + w - 1, top, LS_BOX_ARC_DL, lo);
        for (int y = top + 1; y <= c.y + 1; y++) {
            ls_box_put(sf, clip, x, y, LS_BOX_V, hi);
            ls_box_put(sf, clip, x + w - 1, y, LS_BOX_V, lo);
        }
        dk_text_accel(sf, clip, x + 2, c.y + 1, TABS[t], on ? dk_attr(R_STRONG) : dk_attr(R_FACE), dk_attr(R_ACCEL));
        if (on) {
            dk_hline(sf, clip, x + 1, c.y + 2, w - 2, ' ', dk_attr(R_FACE));
            for (int i = 1; i < w - 1; i++) tui_put_char(sf, clip, x + i, c.y + 2, ' ', dk_attr(R_FACE));
            ls_box_put(sf, clip, x, c.y + 2, LS_BOX_UL, hi);
            ls_box_put(sf, clip, x + w - 1, c.y + 2, LS_BOX_UR, hi);
        } else {
            ls_box_put(sf, clip, x, c.y + 2, LS_BOX_UH, hi);
            ls_box_put(sf, clip, x + w - 1, c.y + 2, LS_BOX_UH, hi);
        }
        hit(tui_rect_make(x, c.y, w, 3), H_TAB, t);
        x += w;
    }
}

/* A radio row: its dot, its label, and where a click on it goes. */
static void radio_row(tui_surface *sf, tui_rect clip, int x, int y, int w, bool on, const char *label, int kind, int arg)
{
    dk_radio(sf, clip, x, y, on, NULL);
    char t[40];
    snprintf(t, sizeof(t), "%s", label);
    dk_text(sf, tui_rect_intersect(clip, tui_rect_make(x + 4, y, w - 4, 1)), x + 4, y, t,
            on ? dk_attr(R_STRONG) : dk_attr(R_FACE));
    hit(tui_rect_make(x, y, w, 1), kind, arg);
}

/* A monitor drawn in lines, with the desk it would show on its screen. */
static void monitor(tui_surface *sf, tui_rect clip, int x, int y, int sw, int sh)
{
    const uint8_t case_ = dk_attr(R_HILITE), dark = dk_attr(R_SHADOW);
    ls_box_put(sf, clip, x, y, LS_BOX_ARC_DR, case_);
    dk_hline(sf, clip, x + 1, y, sw + 2, LS_BOX_H, case_);
    ls_box_put(sf, clip, x + sw + 3, y, LS_BOX_ARC_DL, dark);
    for (int r = 1; r <= sh + 1; r++) {
        ls_box_put(sf, clip, x, y + r, LS_BOX_V, case_);
        ls_box_put(sf, clip, x + sw + 3, y + r, LS_BOX_V, dark);
    }
    ls_box_put(sf, clip, x, y + sh + 2, LS_BOX_ARC_UR, case_);
    dk_hline(sf, clip, x + 1, y + sh + 2, sw + 2, LS_BOX_H, dark);
    ls_box_put(sf, clip, x + sw + 3, y + sh + 2, LS_BOX_ARC_UL, dark);
    ls_box_put(sf, clip, x + sw + 1, y + sh + 1, LS_SHAPE_DOT, (uint8_t)((dk_attr(R_FACE) & 0xF0) | (TUI_GREEN | TUI_BRIGHT)));
    /* the stand */
    const int mid = x + (sw + 4) / 2;
    ls_box_put(sf, clip, mid - 2, y + sh + 3, LS_BOX_DIAG_R, dark);
    ls_box_put(sf, clip, mid + 2, y + sh + 3, LS_BOX_DIAG_L, dark);
    dk_hline(sf, clip, mid - 4, y + sh + 4, 9, LS_BOX_H, dark);
    /* the screen, with the edited choices */
    const dk_prefs_t now = *dk_prefs();
    dk_prefs_set(&s_edit);
    dk_wall_preview(sf, tui_rect_make(x + 2, y + 1, sw, sh), &s_edit);
    dk_prefs_set(&now);
}

static void background(tui_surface *sf, tui_rect c, tui_rect clip)
{
    const int px = c.x + 2, py = c.y + 4;
    monitor(sf, clip, px, py, 22, 8);
    /* how a picture fits, under the monitor */
    static const char *const FIT[DK_FIT_COUNT] = {"Fill", "Tile", "Centre", "Stretch"};
    dk_text(sf, clip, px, py + 14, "Picture:", dk_attr(R_FACE));
    for (int f = 0; f < DK_FIT_COUNT; f++)
        radio_row(sf, clip, px + (f % 2) * 13, py + 15 + f / 2, 12, s_edit.fit == f, FIT[f], H_FIT, f);
    /* the pattern and the wallpaper, side by side */
    const int lx = c.x + 31, wx = c.x + 47;
    dk_text_accel(sf, clip, lx, c.y + 4, "Pattern", dk_attr(R_FACE), dk_attr(R_ACCEL));
    for (int i = 0; i < dk_wall_pattern_count(); i++)
        radio_row(sf, clip, lx, c.y + 6 + i, 15, s_edit.pattern == i, dk_wall_pattern_name(i), H_PATTERN, i);
    dk_text_accel(sf, clip, wx, c.y + 4, "Wallpaper", dk_attr(R_FACE), dk_attr(R_ACCEL));
    int y = c.y + 6;
    radio_row(sf, clip, wx, y++, c.x + c.w - wx - 1, s_edit.wall == 0 || (s_edit.wall >= 1 && s_edit.wall <= 4), "(None)", H_WALL, 0);
    radio_row(sf, clip, wx, y++, c.x + c.w - wx - 1, s_edit.wall == 5, dk_wall_builtin_name(5), H_WALL, 5);
    radio_row(sf, clip, wx, y++, c.x + c.w - wx - 1, s_edit.wall == 6, dk_wall_builtin_name(6), H_WALL, 6);
    for (int i = 0; i < s_nfiles && y < c.y + c.h - 5; i++)
        radio_row(sf, clip, wx, y++, c.x + c.w - wx - 1, s_edit.wall == s_fids[i], s_fnames[i], H_WALL, 100 + i);
    if (!s_nfiles) {
        dk_text(sf, clip, wx, y + 1, "Pictures: .bmp .png .jpg", dk_attr(R_DISABLED));
        dk_text(sf, clip, wx, y + 2, "in SD desk/wallpapers", dk_attr(R_DISABLED));
    }
}

/* The Windows 95 preview: two windows, a menu, a message box, all in the
   edited colours. */
static void look_preview(tui_surface *sf, tui_rect clip, tui_rect r)
{
    const dk_prefs_t now = *dk_prefs();
    /* sunk into the panel, in the page's own colours */
    dk_frame(sf, clip, tui_rect_make(r.x - 1, r.y - 1, r.w + 2, r.h + 2), false);
    dk_prefs_set(&s_edit);
    dk_fill(sf, r, dk_attr(R_DESK));
    /* inactive window */
    const tui_rect a = tui_rect_make(r.x + 2, r.y + 1, 30, 5);
    dk_fill(sf, a, dk_attr(R_FACE));
    dk_frame(sf, clip, a, true);
    dk_fill(sf, tui_rect_make(a.x + 1, a.y + 1, a.w - 2, 1), dk_attr(R_TITLE_OFF));
    dk_text(sf, clip, a.x + 2, a.y + 1, "Inactive Window", dk_attr(R_TITLE_OFF));
    /* active window, a menu bar and text */
    const tui_rect b = tui_rect_make(r.x + 6, r.y + 3, 36, 6);
    dk_fill(sf, b, dk_attr(R_FACE));
    dk_frame(sf, clip, b, true);
    dk_fill(sf, tui_rect_make(b.x + 1, b.y + 1, b.w - 2, 1), dk_attr(R_TITLE_ON));
    dk_text(sf, clip, b.x + 2, b.y + 1, "Active Window", dk_attr(R_TITLE_ON));
    dk_key(sf, clip, b.x + b.w - 4, b.y + 1, LS_SHAPE_TIMES, dk_attr(R_CLOSE));
    const uint8_t mf = dk_attr(R_MENU);
    dk_fill(sf, tui_rect_make(b.x + 1, b.y + 2, b.w - 2, 1), mf);
    dk_text(sf, clip, b.x + 2, b.y + 2, "Normal", mf);
    dk_text(sf, clip, b.x + 10, b.y + 2, "Disabled", (uint8_t)((mf & 0xF0) | (dk_attr(R_DISABLED) & 0x0F)));
    dk_text(sf, clip, b.x + 20, b.y + 2, " Selected ", dk_attr(R_SELECT));
    dk_text(sf, clip, b.x + 2, b.y + 4, "Window Text", dk_attr(R_FACE));
    /* a message box with its button */
    const tui_rect m = tui_rect_make(r.x + r.w - 22, r.y + 5, 20, 5);
    if (m.x > b.x + 20) {
        dk_fill(sf, m, dk_attr(R_FACE));
        dk_frame(sf, clip, m, true);
        dk_fill(sf, tui_rect_make(m.x + 1, m.y + 1, m.w - 2, 1), dk_attr(R_TITLE_ON));
        dk_text(sf, clip, m.x + 2, m.y + 1, "Message Box", dk_attr(R_TITLE_ON));
        dk_text(sf, clip, m.x + 2, m.y + 2, "Message Text", dk_attr(R_FACE));
        dk_text(sf, clip, m.x + 13, m.y + 3, " OK ", dk_attr(R_KEY));
    }
    dk_text(sf, clip, r.x + 2, r.y + r.h - 1, " Tooltip ", dk_attr(R_TIP));
    dk_prefs_set(&now);
}

static void spinner(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *label, const char *value, int kind)
{
    dk_text_accel(sf, clip, x, y, label, dk_attr(R_FACE), dk_attr(R_ACCEL));
    const int fx = x + 8;
    dk_key(sf, clip, fx, y, LS_SHAPE_LEFT, 0);
    dk_fill(sf, tui_rect_make(fx + 3, y, w, 1), TUI_ATTR(dk_light_face() ? TUI_BLACK : (TUI_WHITE | TUI_BRIGHT),
                                                       dk_light_face() ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK));
    dk_text(sf, tui_rect_make(fx + 3, y, w, 1), fx + 4, y, value,
            TUI_ATTR(dk_light_face() ? TUI_BLACK : (TUI_WHITE | TUI_BRIGHT), dk_light_face() ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK));
    dk_key(sf, clip, fx + 3 + w, y, LS_SHAPE_RIGHT, 0);
    hit(tui_rect_make(fx, y, 3, 1), kind, -1);
    hit(tui_rect_make(fx + 3, y, w, 1), kind, 1);
    hit(tui_rect_make(fx + 3 + w, y, 3, 1), kind, 1);
}

static void swatches(tui_surface *sf, tui_rect clip, int x, int y, const char *label, uint8_t cur, int kind)
{
    dk_text(sf, clip, x, y, label, dk_attr(R_FACE));
    const uint8_t face = TUI_ATTR_BG(dk_attr(R_FACE));
    for (int i = 0; i < 16; i++) {
        const int sx = x + 8 + i * 3;
        if (i == cur) {
            /* the chosen one stands full height, bracketed and ticked */
            const uint8_t mark = (i & 8) || i == TUI_WHITE || i == TUI_CYAN || i == TUI_YELLOW || i == TUI_GREEN
                                 ? TUI_BLACK : (TUI_WHITE | TUI_BRIGHT);
            tui_put_char(sf, clip, sx, y, ' ', TUI_ATTR(i, i));
            ls_box_put(sf, clip, sx + 1, y, LS_SHAPE_CHECK, TUI_ATTR(mark, i));
            tui_put_char(sf, clip, sx - 1, y, '[', dk_attr(R_STRONG));
            tui_put_char(sf, clip, sx + 2, y, ']', dk_attr(R_STRONG));
        } else {
            /* the rest are half-height chips, so the two rows read apart */
            const char half = LS_TUI_QUAD(0, 0, 1, 1);
            const uint8_t a = i == face ? TUI_ATTR(dk_attr(R_SHADOW) & 0x0F, face) : TUI_ATTR(i, face);
            tui_put_char(sf, clip, sx, y, i == face ? '_' : half, a);
            tui_put_char(sf, clip, sx + 1, y, i == face ? '_' : half, a);
        }
        hit(tui_rect_make(sx, y, 2, 1), kind, i);
    }
}

static void appearance(tui_surface *sf, tui_rect c, tui_rect clip)
{
    look_preview(sf, clip, tui_rect_make(c.x + 2, c.y + 4, c.w - 4, 10));
    char v[40];
    snprintf(v, sizeof(v), "%s%s", dk_scheme_name(s_edit.scheme), s_edit.mask ? " (changed)" : "");
    spinner(sf, clip, c.x + 2, c.y + 16, 26, "Scheme", v, H_SCHEME);
    spinner(sf, clip, c.x + 2, c.y + 18, 26, "Item", dk_item_name(s_item), H_ITEM);
    if (s_edit.mask & (1u << s_item)) {
        dk_text(sf, clip, c.x + 50, c.y + 18, "[Reset]", dk_attr(R_KEY));
        hit(tui_rect_make(c.x + 50, c.y + 18, 7, 1), H_RESET, 0);
    }
    uint8_t fg, bg;
    dk_item_colors(&s_edit, s_item, &fg, &bg);
    swatches(sf, clip, c.x + 2, c.y + 19, "Text", fg, H_FG);
    swatches(sf, clip, c.x + 2, c.y + 20, "Colour", bg, H_BG);
}

static void effects(tui_surface *sf, tui_rect c, tui_rect clip)
{
    const int x = c.x + 3;
    dk_text(sf, clip, x, c.y + 4, "Visual effects", dk_attr(R_STRONG));
    dk_check(sf, clip, x, c.y + 6, s_edit.live_drag, "Show window contents while dragging");
    hit(tui_rect_make(x, c.y + 6, 40, 1), H_LIVE, 0);
    dk_check(sf, clip, x, c.y + 7, s_edit.anim, "Animate windows, menus and tooltips");
    hit(tui_rect_make(x, c.y + 7, 40, 1), H_ANIM, 0);
    dk_check(sf, clip, x, c.y + 8, s_edit.labels, "Back icon labels so any wallpaper reads");
    hit(tui_rect_make(x, c.y + 8, 42, 1), H_LABELS, 0);
    static const char *const NOTE[] = {
        "Animations play when you do something or the board's",
        "state changes, and stop; nothing moves while nothing",
        "happens. Off, every change shows at once.",
        "",
        "With contents off, a window being moved or sized is",
        "an outline that marches as the pointer goes.",
    };
    for (unsigned i = 0; i < sizeof(NOTE) / sizeof(NOTE[0]); i++) dk_text(sf, clip, x, c.y + 10 + (int)i, NOTE[i], dk_attr(R_FACE));
    /* a small window with its motion lines, as Windows 95's pictures did */
    const int ax = c.x + c.w - 14, ay = c.y + 5;
    const tui_rect w = tui_rect_make(ax, ay, 10, 4);
    dk_fill(sf, w, dk_attr(R_FACE));
    dk_frame(sf, clip, w, true);
    dk_fill(sf, tui_rect_make(ax + 1, ay + 1, 8, 1), dk_attr(R_TITLE_ON));
    for (int i = 0; i < 3; i++) dk_hline(sf, clip, ax - 6 + i, ay + 1 + i, 4 - i, LS_BOX_DASH_H, dk_attr(R_SHADOW));
}

static void settings_tab(tui_surface *sf, tui_rect c, tui_rect clip)
{
    int cols = 0, rows = 0, cw = 0, ch = 0;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    monitor(sf, clip, c.x + 2, c.y + 4, 22, 8);
    const int x = c.x + 31;
    char line[64];
    dk_text(sf, clip, x, c.y + 4, "Character size", dk_attr(R_STRONG));
    static const int SIZES[3][2] = {{10, 17}, {8, 14}, {7, 13}};
    for (int i = 0; i < 3; i++) {
        snprintf(line, sizeof(line), "%dx%d%s", SIZES[i][0], SIZES[i][1], i == 0 ? "  (default)" : "");
        dk_radio(sf, clip, x, c.y + 6 + i, cw == SIZES[i][0] && ch == SIZES[i][1], NULL);
        dk_text(sf, clip, x + 4, c.y + 6 + i, line, dk_attr(R_FACE));
    }
    dk_text(sf, clip, x, c.y + 10, "The page picks the size:", dk_attr(R_FACE));
    dk_text(sf, clip, x, c.y + 11, "its gear, then SCREEN.", dk_attr(R_FACE));
    snprintf(line, sizeof(line), "Desktop: %d x %d cells", cols, rows);
    dk_text(sf, clip, x, c.y + 13, line, dk_attr(R_STRONG));
    snprintf(line, sizeof(line), "%d x %d pixels", cols * cw, rows * ch);
    dk_text(sf, clip, x, c.y + 14, line, dk_attr(R_FACE));
    snprintf(line, sizeof(line), "Scheme: %s, 16 colours", dk_scheme_name(s_edit.scheme));
    dk_text(sf, clip, x, c.y + 16, line, dk_attr(R_FACE));
}

void dk_props_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    s_nh = 0;
    tabs(sf, c, clip);
    /* the panel's sides and bottom, raised */
    const int bottom = c.y + c.h - 4;
    for (int y = c.y + 3; y < bottom; y++) {
        ls_box_put(sf, clip, c.x, y, LS_BOX_V, dk_attr(R_HILITE));
        ls_box_put(sf, clip, c.x + c.w - 1, y, LS_BOX_V, dk_attr(R_SHADOW));
    }
    ls_box_put(sf, clip, c.x, c.y + 2, LS_BOX_DR, dk_attr(R_HILITE));
    ls_box_put(sf, clip, c.x + c.w - 1, c.y + 2, LS_BOX_DL, dk_attr(R_SHADOW));
    ls_box_put(sf, clip, c.x, bottom, LS_BOX_UR, dk_attr(R_HILITE));
    dk_hline(sf, clip, c.x + 1, bottom, c.w - 2, LS_BOX_H, dk_attr(R_SHADOW));
    ls_box_put(sf, clip, c.x + c.w - 1, bottom, LS_BOX_UL, dk_attr(R_SHADOW));
    const tui_rect pc = tui_rect_intersect(clip, tui_rect_make(c.x + 1, c.y + 3, c.w - 2, bottom - c.y - 3));
    switch (s_tab) {
    case TAB_BG: background(sf, c, pc); break;
    case TAB_LOOK: appearance(sf, c, pc); break;
    case TAB_FX: effects(sf, c, pc); break;
    default: settings_tab(sf, c, pc); break;
    }
    /* OK, Cancel, Apply */
    static const char *const B[3] = {"OK", "Cancel", "Apply"};
    for (int i = 0; i < 3; i++) {
        const tui_rect r = tui_rect_make(c.x + c.w - 3 * 13 + i * 13, c.y + c.h - 3, 12, 3);
        dk_button(sf, clip, r, B[i], i == 0, false);
        if (i == 2 && !s_dirty)
            dk_text(sf, clip, r.x + (r.w - 5) / 2, r.y + 1, B[i], dk_attr(R_DISABLED));
        hit(r, H_OK + i, 0);
    }
}

/* -------------------------------------------------------------- input -- */

static void set_item(uint8_t fg, uint8_t bg)
{
    s_edit.mask |= (uint8_t)(1u << s_item);
    s_edit.fg[s_item] = fg;
    s_edit.bg[s_item] = bg;
    s_dirty = true;
}

static void act(int kind, int arg)
{
    uint8_t fg, bg;
    switch (kind) {
    case H_TAB: s_tab = arg; break;
    case H_OK: ls_desk_apply_prefs(&s_edit); ls_desk_close(dk_front_win() ? dk_front_win()->id : -1); return;
    case H_CANCEL: ls_desk_close(dk_front_win() ? dk_front_win()->id : -1); return;
    case H_APPLY: if (s_dirty) ls_desk_apply_prefs(&s_edit); s_dirty = false; return;
    case H_PATTERN: s_edit.pattern = (uint8_t)arg; s_dirty = true; break;
    case H_WALL:
        s_edit.wall = arg >= 100 ? s_fids[arg - 100] : (uint32_t)arg;
        s_dirty = true;
        break;
    case H_FIT: s_edit.fit = (uint8_t)arg; s_dirty = true; break;
    case H_SCHEME:
        s_edit.scheme = (uint8_t)((s_edit.scheme + dk_scheme_count() + arg) % dk_scheme_count());
        s_edit.mask = 0;                              /* a new scheme starts clean */
        s_dirty = true;
        break;
    case H_ITEM: s_item = (s_item + DK_IT_COUNT + arg) % DK_IT_COUNT; break;
    case H_FG: dk_item_colors(&s_edit, s_item, &fg, &bg); set_item((uint8_t)arg, bg); break;
    case H_BG: dk_item_colors(&s_edit, s_item, &fg, &bg); set_item(fg, (uint8_t)arg); break;
    case H_RESET: s_edit.mask &= (uint8_t)~(1u << s_item); s_dirty = true; break;
    case H_LIVE: s_edit.live_drag = !s_edit.live_drag; s_dirty = true; break;
    case H_ANIM: s_edit.anim = !s_edit.anim; s_dirty = true; break;
    case H_LABELS: s_edit.labels = !s_edit.labels; s_dirty = true; break;
    default: break;
    }
    ls_tui_invalidate();
}

bool dk_props_tap(tui_rect c, int col, int row)
{
    (void)c;
    for (int i = s_nh - 1; i >= 0; i--)
        if (tui_rect_contains(s_hits[i].r, col, row)) { act(s_hits[i].kind, s_hits[i].arg); return true; }
    return false;
}

bool dk_props_key(ls_tk_t k, char ch)
{
    (void)ch;
    switch (k) {
    case LS_TK_TAB: s_tab = (s_tab + 1) % TAB_N; return true;
    case LS_TK_ENTER: act(H_OK, 0); return true;
    case LS_TK_ESC: act(H_CANCEL, 0); return true;
    case LS_TK_LEFT: if (s_tab == TAB_LOOK) act(H_SCHEME, -1); else s_tab = (s_tab + TAB_N - 1) % TAB_N; return true;
    case LS_TK_RIGHT: if (s_tab == TAB_LOOK) act(H_SCHEME, 1); else s_tab = (s_tab + 1) % TAB_N; return true;
    default: return false;
    }
}
