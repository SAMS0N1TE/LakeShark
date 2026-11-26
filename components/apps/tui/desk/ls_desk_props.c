/* Display Properties, as Windows 95 had it: Background, Appearance,
   Effects and Settings, a property sheet (ls_desk_sheet.c) whose pictures
   are drawn from a copy of the choices that only OK or Apply put on the
   desk (and in NVS). Cancel forgets them.

   This file is also the sheet window's own: the desk's Display Properties
   window (DK_SHELL_DISPLAY) shows whichever sheet is up, this one or one of
   the Control Panel's (ls_desk_cplsys.c). */

#include "ls_desk_sheet.h"

#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

#include "core/settings.h"
#include "core/display_ctl.h"
#include "../ls_theme.h"
#include "../ls_tui_screen.h"

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static dk_prefs_t s_edit;          /* the choices being made */
static int s_item;                 /* the item whose colours are shown */
EXT_RAM_BSS_ATTR static char s_files[8][40];
EXT_RAM_BSS_ATTR static const char *s_fnames[8];
EXT_RAM_BSS_ATTR static uint32_t s_fids[8];
static int s_nfiles;

static void files_read(void)
{
    s_nfiles = dk_wall_files(s_fnames, s_fids, 8);
    for (int i = 0; i < s_nfiles; i++) {
        snprintf(s_files[i], sizeof(s_files[i]), "%s", s_fnames[i]);
        s_fnames[i] = s_files[i];
    }
}

static void d_load(void) { s_edit = *dk_prefs(); s_edit.wall = dk_wall_current(); }
static void d_commit(void) { ls_desk_apply_prefs(&s_edit); }

/* ---------------------------------------------------------- Background -- */

/* The wallpapers offered: none, the two drawn ones, the pictures on the card. */
static int wall_count(void) { return 3 + s_nfiles; }
static uint32_t wall_id(int i) { return i <= 0 ? 0 : i == 1 ? 5 : i == 2 ? 6 : i - 3 < s_nfiles ? s_fids[i - 3] : 0; }
static int wall_get(void)
{
    for (int i = 1; i < wall_count(); i++) if (wall_id(i) == s_edit.wall) return i;
    return 0;
}
static void wall_set(int v) { s_edit.wall = wall_id(v); }
static void wall_text(int v, char *b, size_t n)
{
    if (v <= 0) snprintf(b, n, "(None)");
    else if (v < 3) snprintf(b, n, "%s", dk_wall_builtin_name(v == 1 ? 5 : 6));
    else snprintf(b, n, "%s", v - 3 < s_nfiles ? s_fnames[v - 3] : "");
}
static const char *const FIT[DK_FIT_COUNT] = {"Fill", "Tile", "Centre", "Stretch"};
static int fit_count(void) { return DK_FIT_COUNT; }
static int fit_get(void) { return s_edit.fit < DK_FIT_COUNT ? s_edit.fit : 0; }
static void fit_set(int v) { s_edit.fit = (uint8_t)v; }
static void fit_text(int v, char *b, size_t n) { snprintf(b, n, "%s", v >= 0 && v < DK_FIT_COUNT ? FIT[v] : ""); }
static bool fit_off(void) { return !(s_edit.wall & DK_WP_FILE); }      /* only a picture has a fit */
static int pat_get(void) { return s_edit.pattern < dk_wall_pattern_count() ? s_edit.pattern : 0; }
static void pat_set(int v) { s_edit.pattern = (uint8_t)v; }
static void pat_text(int v, char *b, size_t n) { snprintf(b, n, "%s", dk_wall_pattern_name(v)); }
static bool have_files(void) { return s_nfiles > 0; }

/* A monitor drawn in lines, with the desk it would show on its screen. */
static void monitor(tui_surface *sf, tui_rect clip, tui_rect r)
{
    const int sw = 22, sh = 8, x = r.x + (r.w - sw - 4) / 2, y = r.y + 1;
    const uint8_t case_ = dk_attr(R_HILITE), dark = dk_attr(R_SHADOW);
    ls_box_put(sf, clip, x, y, LS_BOX_ARC_DR, case_);
    dk_hline(sf, clip, x + 1, y, sw + 2, LS_BOX_H, case_);
    ls_box_put(sf, clip, x + sw + 3, y, LS_BOX_ARC_DL, dark);
    for (int k = 1; k <= sh + 1; k++) {
        ls_box_put(sf, clip, x, y + k, LS_BOX_V, case_);
        ls_box_put(sf, clip, x + sw + 3, y + k, LS_BOX_V, dark);
    }
    ls_box_put(sf, clip, x, y + sh + 2, LS_BOX_ARC_UR, case_);
    dk_hline(sf, clip, x + 1, y + sh + 2, sw + 2, LS_BOX_H, dark);
    ls_box_put(sf, clip, x + sw + 3, y + sh + 2, LS_BOX_ARC_UL, dark);
    ls_box_put(sf, clip, x + sw + 1, y + sh + 1, LS_SHAPE_DOT, dk_art(TUI_GREEN));
    const int mid = x + (sw + 4) / 2;
    ls_box_put(sf, clip, mid - 2, y + sh + 3, LS_BOX_DIAG_R, dark);
    ls_box_put(sf, clip, mid + 2, y + sh + 3, LS_BOX_DIAG_L, dark);
    dk_hline(sf, clip, mid - 4, y + sh + 4, 9, LS_BOX_H, dark);
    /* the screen, with the choices being made */
    const tui_rect scr = tui_rect_intersect(clip, tui_rect_make(x + 2, y + 1, sw, sh));
    if (scr.w != sw || scr.h != sh) return;     /* cut by the window's edge: the picture is whole or not there */
    const dk_prefs_t now = *dk_prefs();
    dk_prefs_set(&s_edit);
    dk_wall_preview(sf, scr, &s_edit);
    dk_prefs_set(&now);
}

static const dk_sh_ctl_t BG_CTL[] = {
    {.kind = SH_GROUP, .label = "Wallpaper"},
    {.kind = SH_DROP, .flags = SHF_OWN, .label = "Pict&ure", .get = wall_get, .set = wall_set, .count = wall_count, .text = wall_text},
    {.kind = SH_DROP, .flags = SHF_OWN, .label = "&Fit", .get = fit_get, .set = fit_set, .count = fit_count, .text = fit_text, .off = fit_off},
    {.kind = SH_GROUP, .label = "Under it"},
    {.kind = SH_DROP, .flags = SHF_OWN, .label = "Pa&ttern", .get = pat_get, .set = pat_set, .count = dk_wall_pattern_count, .text = pat_text},
    {.kind = SH_NOTE, .label = "The screen at the left shows it."},
    {.kind = SH_NOTE, .label = "Your own pictures: .bmp .png .jpg in", .hidden = have_files},
    {.kind = SH_NOTE, .label = "desk/wallpapers on the SD card.", .hidden = have_files},
};

/* ---------------------------------------------------------- Appearance -- */

static const char *const COLOUR[16] = {
    "Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "Silver",
    "Grey", "Bright red", "Bright green", "Bright yellow", "Bright blue", "Bright magenta", "Bright cyan", "White",
};
static int scheme_get(void) { return s_edit.scheme; }
static void scheme_set(int v) { s_edit.scheme = (uint8_t)v; s_edit.mask = 0; }     /* a new scheme starts clean */
static void scheme_text(int v, char *b, size_t n) { snprintf(b, n, "%s%s", dk_scheme_name(v), v == s_edit.scheme && s_edit.mask ? " *" : ""); }
static int item_count(void) { return DK_IT_COUNT; }
static int item_get(void) { return s_item; }
static void item_set(int v) { s_item = v; }
static void item_text(int v, char *b, size_t n) { snprintf(b, n, "%s%s", dk_item_name(v), s_edit.mask & (1u << v) ? " *" : ""); }
static int sixteen(void) { return 16; }
static void colour_text(int v, char *b, size_t n) { snprintf(b, n, "%s", COLOUR[v & 15]); }
static int fg_get(void) { uint8_t fg, bg; dk_item_colors(&s_edit, s_item, &fg, &bg); return fg & 15; }
static int bg_get(void) { uint8_t fg, bg; dk_item_colors(&s_edit, s_item, &fg, &bg); return bg & 15; }
static void item_put(uint8_t fg, uint8_t bg)
{
    s_edit.mask |= (uint8_t)(1u << s_item);
    s_edit.fg[s_item] = fg;
    s_edit.bg[s_item] = bg;
}
static void fg_set(int v) { item_put((uint8_t)v, (uint8_t)bg_get()); }
static void bg_set(int v) { item_put((uint8_t)fg_get(), (uint8_t)v); }
static bool item_plain(void) { return !(s_edit.mask & (1u << s_item)); }
static void item_reset(void) { s_edit.mask &= (uint8_t)~(1u << s_item); dk_sheet_dirty(); }

/* The item's two colours as they will read, and their contrast in words. */
static void sample_draw(tui_surface *sf, tui_rect clip, tui_rect r)
{
    uint8_t fg, bg;
    dk_item_colors(&s_edit, s_item, &fg, &bg);
    dk_text(sf, clip, r.x, r.y, "Sample:", dk_attr(R_FACE));
    dk_text(sf, clip, r.x + 8, r.y, " AaBb 123 ", TUI_ATTR(fg, bg));
    const int cr = dk_contrast(fg, bg);
    dk_text(sf, clip, r.x + 19, r.y, cr >= 45 ? "reads well" : cr >= 30 ? "reads" : "hard to read",
            cr >= 30 ? dk_attr(R_GOOD) : dk_attr(R_BAD));
}

/* The Windows 95 preview: two windows, a menu, a button, a tooltip, all in
   the colours being chosen. A click on a part picks its item. */
EXT_RAM_BSS_ATTR static tui_rect s_part[DK_IT_COUNT + 2];
EXT_RAM_BSS_ATTR static uint8_t s_part_item[DK_IT_COUNT + 2];
static int s_parts;
static void part(tui_rect r, int item)
{
    if (s_parts < COUNT(s_part)) { s_part[s_parts] = r; s_part_item[s_parts++] = (uint8_t)item; }
}

static void look_side(tui_surface *sf, tui_rect clip, tui_rect r)
{
    const dk_prefs_t now = *dk_prefs();
    const tui_rect box = tui_rect_make(r.x, r.y, r.w, r.h > 15 ? 15 : r.h);
    dk_frame(sf, clip, box, false);             /* sunk into the page, in the page's own colours */
    const tui_rect d = dk_inner(box);
    clip = tui_rect_intersect(clip, d);
    s_parts = 0;
    dk_prefs_set(&s_edit);
    dk_fill(sf, clip, dk_attr(R_DESK));
    part(d, DK_IT_DESKTOP);
    dk_text(sf, clip, d.x + d.w - 9, d.y + d.h - 1, "Desktop", dk_attr(R_DESK_TEXT));
    /* the window behind */
    const tui_rect a = tui_rect_make(d.x + 1, d.y + 1, d.w - 8, 4);
    dk_fill(sf, tui_rect_intersect(clip, a), dk_attr(R_FACE));
    dk_frame(sf, clip, a, true);
    dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(a.x + 1, a.y + 1, a.w - 2, 1)), dk_attr(R_TITLE_OFF));
    dk_text(sf, clip, a.x + 2, a.y + 1, "Inactive Window", dk_attr(R_TITLE_OFF));
    part(a, DK_IT_WINDOW);
    part(tui_rect_make(a.x + 1, a.y + 1, a.w - 2, 1), DK_IT_TITLE_OFF);
    /* the window in front: title, menu, text, a button */
    const tui_rect b = tui_rect_make(d.x + 3, d.y + 3, d.w - 4, d.h >= 13 ? 7 : 6);
    dk_fill(sf, tui_rect_intersect(clip, b), dk_attr(R_FACE));
    dk_frame(sf, clip, b, true);
    dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(b.x + 1, b.y + 1, b.w - 2, 1)), dk_attr(R_TITLE_ON));
    dk_text(sf, clip, b.x + 2, b.y + 1, "Active Window", dk_attr(R_TITLE_ON));
    dk_key(sf, clip, b.x + b.w - 4, b.y + 1, LS_SHAPE_TIMES, dk_attr(R_CLOSE));
    const uint8_t mf = dk_attr(R_MENU);
    dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(b.x + 1, b.y + 2, b.w - 2, 1)), mf);
    dk_text(sf, clip, b.x + 2, b.y + 2, "Menu", mf);
    dk_text(sf, clip, b.x + 7, b.y + 2, "Grey", (uint8_t)((mf & 0xF0) | (dk_attr(R_DISABLED) & 0x0F)));
    dk_text(sf, clip, b.x + 12, b.y + 2, " Selected ", dk_attr(R_SELECT));
    dk_text(sf, clip, b.x + 2, b.y + b.h - 3, "Window text", dk_attr(R_FACE));
    dk_text(sf, clip, b.x + b.w - 8, b.y + b.h - 2, "[ OK ]", dk_attr(R_KEY));
    part(b, DK_IT_WINDOW);
    part(tui_rect_make(b.x + 1, b.y + 1, b.w - 2, 1), DK_IT_TITLE_ON);
    part(tui_rect_make(b.x + 1, b.y + 2, b.w - 2, 1), DK_IT_MENU);
    part(tui_rect_make(b.x + 12, b.y + 2, 10, 1), DK_IT_SELECT);
    part(tui_rect_make(b.x + b.w - 8, b.y + b.h - 2, 6, 1), DK_IT_BUTTON);
    dk_text(sf, clip, d.x + 1, d.y + d.h - 2, " Tooltip ", dk_attr(R_TIP));
    part(tui_rect_make(d.x + 1, d.y + d.h - 2, 9, 1), DK_IT_TIP);
    dk_prefs_set(&now);
}

static bool look_tap(tui_rect r, int col, int row)
{
    (void)r;
    for (int i = s_parts - 1; i >= 0; i--)
        if (tui_rect_contains(s_part[i], col, row)) { s_item = s_part_item[i]; return true; }
    return false;
}

static const dk_sh_ctl_t LOOK_CTL[] = {
    {.kind = SH_GROUP, .label = "Scheme"},
    {.kind = SH_DROP, .flags = SHF_OWN, .label = "&Scheme", .get = scheme_get, .set = scheme_set, .count = dk_scheme_count, .text = scheme_text},
    {.kind = SH_GROUP, .label = "Your own colours"},
    {.kind = SH_DROP, .flags = SHF_OWN | SHF_VIEW, .label = "&Item", .get = item_get, .set = item_set, .count = item_count, .text = item_text},
    {.kind = SH_DROP, .flags = SHF_OWN | SHF_SWATCH, .label = "&Text", .get = fg_get, .set = fg_set, .count = sixteen, .text = colour_text},
    {.kind = SH_DROP, .flags = SHF_OWN | SHF_SWATCH, .label = "&Ground", .get = bg_get, .set = bg_set, .count = sixteen, .text = colour_text},
    {.kind = SH_CUSTOM, .rows = 1, .draw = sample_draw},
    {.kind = SH_BUTTON, .label = "&Reset this item to the scheme", .press = item_reset, .off = item_plain},
    {.kind = SH_NOTE, .label = "Click a part of the picture to"},
    {.kind = SH_NOTE, .label = "pick its item. * = changed by you."},
};

/* ------------------------------------------------------------- Effects -- */

static int live_get(void) { return s_edit.live_drag; }
static void live_set(int v) { s_edit.live_drag = v != 0; }
static int anim_get(void) { return s_edit.anim; }
static void anim_set(int v) { s_edit.anim = v != 0; }
static int labels_get(void) { return s_edit.labels; }
static void labels_set(int v) { s_edit.labels = v != 0; }
/* The desktop's gadgets, one check each; kept in the desk's own store. */
static void gadget_put(int bit, int on) { dk_shell_set_gadgets(on ? dk_shell_gadgets() | bit : dk_shell_gadgets() & ~bit); }
#define GADGET(name, bit) static int name##_get(void) { return (dk_shell_gadgets() & (bit)) != 0; } \
                          static void name##_set(int v) { gadget_put(bit, v); }
GADGET(gclock, DKG_CLOCK) GADGET(gradio, DKG_RADIO) GADGET(gmesh, DKG_MESH) GADGET(gboard, DKG_SYSTEM)
#undef GADGET

/* What the three checks do, drawn: a window whole or as its outline, with
   or without motion lines, and an icon's name backed or bare. Each part
   changes only when its check does. */
static void fx_side(tui_surface *sf, tui_rect clip, tui_rect r)
{
    const int ax = r.x + 5, ay = r.y + 2;
    const tui_rect w = tui_rect_make(ax, ay, 10, 4);
    dk_fill(sf, tui_rect_intersect(clip, w), dk_attr(R_FACE));
    if (s_edit.live_drag) {
        dk_frame(sf, clip, w, true);
        dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(ax + 1, ay + 1, 8, 1)), dk_attr(R_TITLE_ON));
    } else dk_frame_dashed(sf, clip, w, dk_attr(R_HILITE));
    if (s_edit.anim)
        for (int i = 0; i < 3; i++) dk_hline(sf, clip, ax - 5 + i, ay + 1 + i, 4 - i, LS_BOX_DASH_H, dk_attr(R_SHADOW));
    const int ix = r.x + 4, iy = r.y + 8;
    const uint8_t ink = dk_art(TUI_CYAN);
    ls_box_put(sf, clip, ix + 3, iy, LS_BOX_DR, ink);
    dk_hline(sf, clip, ix + 4, iy, 2, LS_BOX_H, ink);
    ls_box_put(sf, clip, ix + 6, iy, LS_BOX_DL, ink);
    ls_box_put(sf, clip, ix + 3, iy + 1, LS_BOX_UR, ink);
    dk_hline(sf, clip, ix + 4, iy + 1, 2, LS_BOX_H, ink);
    ls_box_put(sf, clip, ix + 6, iy + 1, LS_BOX_UL, ink);
    dk_text(sf, clip, ix, iy + 2, " An icon ", s_edit.labels ? dk_attr(R_SELECT) : dk_attr(R_FACE));
}

static const dk_sh_ctl_t FX_CTL[] = {
    {.kind = SH_GROUP, .label = "Windows"},
    {.kind = SH_CHECK, .flags = SHF_OWN, .label = "Show window &contents while dragging", .get = live_get, .set = live_set},
    {.kind = SH_CHECK, .flags = SHF_OWN, .label = "Animate &windows, menus and tooltips", .get = anim_get, .set = anim_set},
    {.kind = SH_GROUP, .label = "Desktop"},
    {.kind = SH_CHECK, .flags = SHF_OWN, .label = "Back icon &labels so any wallpaper reads", .get = labels_get, .set = labels_set},
    {.kind = SH_GROUP, .label = "Gadgets on the desktop"},
    {.kind = SH_CHECK, .label = "Cloc&k and GPS", .get = gclock_get, .set = gclock_set},
    {.kind = SH_CHECK, .label = "&Receiver", .get = gradio_get, .set = gradio_set},
    {.kind = SH_CHECK, .label = "&Mesh", .get = gmesh_get, .set = gmesh_set},
    {.kind = SH_CHECK, .label = "B&oard", .get = gboard_get, .set = gboard_set},
    {.kind = SH_NOTE, .label = "Animations play on an action, then stop."},
    {.kind = SH_NOTE, .label = "Contents off: a dragged window is an outline."},
};

/* ------------------------------------------------------------ Settings -- */

static void pct_text(int v, char *b, size_t n) { snprintf(b, n, "%d %%", v); }
static void bri_set(int v)
{
    v = v < 5 ? 5 : v > 100 ? 100 : v;              /* never stepped dark */
    if (v != display_ctl_get_user()) display_ctl_set_user(v);
}
static int dim_get(void) { return display_ctl_autodim_enabled(); }
static void dim_set(int v) { display_ctl_set_autodim(v != 0); }
static const int DIM_STEPS[] = {15, 30, 60, 120, 240};
static int dimt_get(void)
{
    const int cur = display_ctl_autodim_timeout();
    int at = 0;
    for (int i = 0; i < COUNT(DIM_STEPS); i++) if (DIM_STEPS[i] <= cur) at = i;
    return at;
}
static void dimt_set(int v) { if (v >= 0 && v < COUNT(DIM_STEPS)) display_ctl_set_autodim_timeout(DIM_STEPS[v]); }
static void dimt_text(int v, char *b, size_t n)
{
    const int s = DIM_STEPS[v >= 0 && v < COUNT(DIM_STEPS) ? v : 0];
    if (s >= 120) snprintf(b, n, "%d minutes", s / 60);
    else snprintf(b, n, "%d seconds", s);
}
static int day_get(void) { return ls_tui_daylight(); }
static void day_set(int v) { ls_tui_set_daylight(v != 0); settings_set_daylight(v != 0); }
static void grid_show(char *b, size_t n)
{
    int cols = 0, rows = 0;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    snprintf(b, n, "%d x %d cells", cols, rows);
}
static void cell_show(char *b, size_t n)
{
    int cw = 0, ch = 0;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    snprintf(b, n, "%d x %d dots", cw, ch);
}
static void pixels_show(char *b, size_t n)
{
    int cols = 0, rows = 0, cw = 0, ch = 0;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    snprintf(b, n, "%d x %d", cols * cw, rows * ch);
}
static void colours_show(char *b, size_t n)
{
    const char *pal = dk_scheme_palette(dk_prefs()->scheme);
    snprintf(b, n, "16, the %s palette", pal ? pal : "board's");
}

static const dk_sh_ctl_t SET_CTL[] = {
    {.kind = SH_GROUP, .label = "Panel light"},
    {.kind = SH_SLIDER, .flags = SHF_LIVE, .label = "B&rightness", .get = display_ctl_get_user, .set = bri_set, .text = pct_text,
     .min = 5, .max = 100, .step = 5},
    {.kind = SH_CHECK, .label = "&Dim the panel when nothing is touched", .get = dim_get, .set = dim_set},
    {.kind = SH_SPIN, .label = "Dim a&fter", .get = dimt_get, .set = dimt_set, .text = dimt_text, .min = 0, .max = COUNT(DIM_STEPS) - 1, .step = 1},
    {.kind = SH_CHECK, .label = "Da&ylight colours, for sun on the panel", .get = day_get, .set = day_set},
    {.kind = SH_GROUP, .label = "This screen"},
    /* the desk's resolution (shell presets); the page scales it to its window */
    {.kind = SH_DROP, .label = "Screen &area", .get = dk_res_row, .set = dk_res_pick, .count = dk_res_rows, .text = dk_res_row_text},
    {.kind = SH_TEXT, .label = "Desktop", .show = grid_show},
    {.kind = SH_TEXT, .label = "Characters", .show = cell_show},
    {.kind = SH_TEXT, .label = "Pixels", .show = pixels_show},
    {.kind = SH_TEXT, .label = "Colours", .show = colours_show},
    {.kind = SH_NOTE, .label = "The page picks the character size: its gear, then SCREEN."},
};

/* ------------------------------------------------------------ the sheet -- */

static const dk_sh_page_t PAGES[] = {
    {.title = "&Background", .ctl = BG_CTL, .n = COUNT(BG_CTL), .enter = files_read, .side_w = 28, .side_h = 14, .side = monitor},
    {.title = "A&ppearance", .ctl = LOOK_CTL, .n = COUNT(LOOK_CTL), .side_w = 32, .side_h = 15, .side = look_side, .side_tap = look_tap},
    {.title = "&Effects", .ctl = FX_CTL, .n = COUNT(FX_CTL), .side_w = 16, .side_h = 12, .side = fx_side},
    {.title = "Setti&ngs", .ctl = SET_CTL, .n = COUNT(SET_CTL)},
};
static const dk_sheet_t DISPLAY = {
    .title = "Display Properties", .pages = PAGES, .n = COUNT(PAGES), .hue = TUI_CYAN, .w = 74,
    .load = d_load, .commit = d_commit,
};

const dk_sheet_t *dk_display_sheet(void)
{
    files_read();
    s_item = DK_IT_DESKTOP;
    return &DISPLAY;
}

/* ------------------------------------------------ the window's own part -- */

void dk_props_open(void)
{
    if (dk_sheet_now() == &DISPLAY && dk_win_shell(DK_SHELL_DISPLAY)) return;      /* already up: its choices stay */
    dk_sheet_begin(dk_display_sheet(), 0);
}

void dk_props_tab(int tab)
{
    dk_props_open();
    dk_sheet_set_tab(tab);
}

void dk_props_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    if (!dk_sheet_now()) dk_props_open();
    dk_sheet_draw(sf, c, clip);
}

bool dk_props_tap(tui_rect c, int col, int row) { return dk_sheet_tap(c, col, row); }
bool dk_props_key(ls_tk_t k, char ch) { return dk_sheet_key(k, ch); }
