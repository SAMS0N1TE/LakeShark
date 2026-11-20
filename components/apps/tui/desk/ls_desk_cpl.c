/* The Control Panel: a folder of applets, as Windows 95 had it.

   Two views of the same ten things. Icons: each applet's mark with its name
   under it. Details: a list with a header, one row each, its name, what it
   holds and one live line of where things stand; a click on a header sorts
   by that column. A click selects, a double click or Enter opens the
   applet's property sheet in its own window (ls_desk_sheet.c), and the
   panel stays open behind it. The sheets themselves are in
   ls_desk_cplsys.c; Display is Display Properties (ls_desk_props.c). */

#include "ls_desk_sheet.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"

#include "../ls_tui_screen.h"
#include "../ls_tui_touch.h"

enum { VIEW_ICONS, VIEW_DETAILS };
enum { FOC_LIST, FOC_ICONS, FOC_DETAILS, FOC_N };
enum { COL_NAME, COL_WHAT, COL_NOW, COL_N };
#define CPL_MAX 16
#define ICON_W 19
#define ICON_H 5

static int s_view, s_focus;
static int s_sel, s_top;                    /* the applet chosen; the first row shown */
static bool s_follow = true;                /* bring the chosen one into view at the next draw */
static int s_sort = -1;                     /* a column, or -1: the panel's own order */
static bool s_desc;
EXT_RAM_BSS_ATTR static uint8_t s_order[CPL_MAX];
static int s_n, s_cols = 1, s_fit = 1;
EXT_RAM_BSS_ATTR static tui_rect s_r_list, s_r_view[2], s_r_head[COL_N], s_r_bar;

/* The view and the sort column are kept with the desk's other choices. */
static void view_save(void)
{
    dk_shell_set_word((dk_shell_word() & ~15) | (s_view == VIEW_DETAILS ? 1 : 0) | ((s_sort + 1) & 3) << 1 | (s_desc ? 8 : 0));
}

void dk_cpl_open(int applet)
{
    if (applet >= 0 && applet < dk_cpl_count()) { dk_sheet_open(dk_cpl_sheet(applet), 0); return; }
    if (!dk_win_shell(DK_SHELL_CPL)) {
        const int w = dk_shell_word();
        s_view = w & 1 ? VIEW_DETAILS : VIEW_ICONS;
        s_sort = ((w >> 1) & 3) - 1;
        if (s_sort >= COL_N) s_sort = -1;
        s_desc = (w & 8) != 0;
    }
    s_follow = true;
    dk_open_shell(DK_SHELL_CPL, "Control Panel", 78, 21);
}

static void now_line(int i, char *b, size_t n)
{
    const dk_cpl_applet_t *a = dk_cpl_at(i);
    b[0] = 0;
    if (a && a->summary) a->summary(b, n);
}

static int cmp(int a, int b)
{
    char x[64], y[64];
    int r;
    if (s_sort == COL_NOW) { now_line(a, x, sizeof(x)); now_line(b, y, sizeof(y)); r = strcasecmp(x, y); }
    else if (s_sort == COL_WHAT) r = strcasecmp(dk_cpl_at(a)->what, dk_cpl_at(b)->what);
    else r = strcasecmp(dk_cpl_at(a)->name, dk_cpl_at(b)->name);
    return s_desc ? -r : r;
}

/* The order shown: the panel's own in the icon view, the sorted column's in the list. */
static void order(void)
{
    s_n = dk_cpl_count() < CPL_MAX ? dk_cpl_count() : CPL_MAX;
    for (int i = 0; i < s_n; i++) s_order[i] = (uint8_t)i;
    if (s_view != VIEW_DETAILS || s_sort < 0) return;
    for (int i = 1; i < s_n; i++) {
        const uint8_t v = s_order[i];
        int j = i;
        for (; j > 0 && cmp(s_order[j - 1], v) > 0; j--) s_order[j] = s_order[j - 1];
        s_order[j] = v;
    }
}

static int pos_of(int applet)
{
    for (int i = 0; i < s_n; i++) if (s_order[i] == applet) return i;
    return 0;
}

static int rows_total(void) { return s_view == VIEW_ICONS ? (s_n + s_cols - 1) / s_cols : s_n; }

static void open_sel(void)
{
    if (s_sel >= 0 && s_sel < dk_cpl_count()) dk_sheet_open(dk_cpl_sheet(s_sel), 0);
}

static void set_view(int v)
{
    s_view = v;
    s_top = 0;
    s_follow = true;
    view_save();
}

/* ------------------------------------------------------------- drawing -- */

static tui_rect icon_rect(int pos)
{
    const int pad = (s_r_list.w - s_cols * ICON_W) / 2;
    return tui_rect_make(s_r_list.x + pad + (pos % s_cols) * ICON_W, s_r_list.y + (pos / s_cols - s_top) * ICON_H, ICON_W, ICON_H - 1);
}

static void draw_icons(tui_surface *sf, tui_rect clip)
{
    for (int p = 0; p < s_n; p++) {
        const dk_cpl_applet_t *a = dk_cpl_at(s_order[p]);
        const tui_rect r = icon_rect(p), rc = tui_rect_intersect(clip, r);
        if (r.y + r.h <= s_r_list.y || r.y >= s_r_list.y + s_r_list.h) continue;
        const bool sel = s_order[p] == s_sel, hov = dk_hover(rc);
        for (int k = 0; k < 3; k++) if (a->art[k]) dk_text(sf, rc, r.x + (ICON_W - 6) / 2, r.y + k, a->art[k], dk_art(a->hue));
        const int n = dk_text_len(a->name), x = r.x + (ICON_W - n) / 2;
        const uint8_t la = sel ? dk_attr(s_focus == FOC_LIST ? R_SELECT : R_TASKHOVER) : hov ? dk_attr(R_TASKHOVER) : dk_attr(R_FACE);
        if (sel || hov) dk_fill(sf, tui_rect_intersect(rc, tui_rect_make(x - 1, r.y + 3, n + 2, 1)), la);
        dk_text(sf, rc, x, r.y + 3, a->name, la);
    }
}

static void draw_details(tui_surface *sf, tui_rect clip, bool bar)
{
    static const char *const HEAD[COL_N] = {"Name", "What it holds", "Now"};
    const tui_rect l = s_r_list;
    const int w = l.w - (bar ? 2 : 0);
    const int w0 = 21, w1 = (w - w0) * 60 / 100, x[COL_N + 1] = {l.x, l.x + w0, l.x + w0 + w1, l.x + w};
    for (int c = 0; c < COL_N; c++) {
        s_r_head[c] = tui_rect_make(x[c], l.y, x[c + 1] - x[c], 1);
        const tui_rect hc = tui_rect_intersect(clip, s_r_head[c]);
        const uint8_t a = dk_pressed(s_r_head[c]) ? dk_attr(R_SELECT) : dk_hover(hc) ? dk_attr(R_TASKHOVER) : dk_attr(R_KEY);
        dk_fill(sf, hc, a);
        dk_text(sf, hc, x[c] + 1, l.y, HEAD[c], a);
        if (s_sort == c) ls_box_put(sf, hc, x[c] + 2 + dk_text_len(HEAD[c]), l.y, s_desc ? LS_SHAPE_DOWN : LS_SHAPE_UP, a);
        if (c) ls_box_put(sf, hc, x[c], l.y, LS_BOX_V, a);
    }
    for (int k = 0; k < s_fit && s_top + k < s_n; k++) {
        const int i = s_order[s_top + k], y = l.y + 1 + k;
        const dk_cpl_applet_t *a = dk_cpl_at(i);
        const tui_rect rr = tui_rect_intersect(clip, tui_rect_make(l.x, y, w, 1));
        const bool sel = i == s_sel, hov = dk_hover(rr);
        const uint8_t band = sel ? dk_attr(s_focus == FOC_LIST ? R_SELECT : R_TASKHOVER) : dk_attr(R_TASKHOVER);
        if (sel || hov) dk_fill(sf, rr, band);
        const bool lit = sel || hov;
        char now[64];
        now_line(i, now, sizeof(now));
        ls_box_put(sf, rr, x[0] + 1, y, LS_SHAPE_DOT, lit ? band : dk_art(a->hue));
        dk_text(sf, tui_rect_intersect(rr, tui_rect_make(x[0], y, w0 - 1, 1)), x[0] + 3, y, a->name, lit ? band : dk_attr(R_STRONG));
        dk_text(sf, tui_rect_intersect(rr, tui_rect_make(x[1], y, w1 - 1, 1)), x[1] + 1, y, a->what, lit ? band : dk_attr(R_FACE));
        dk_text(sf, tui_rect_intersect(rr, tui_rect_make(x[2], y, x[3] - x[2], 1)), x[2] + 1, y, now, lit ? band : dk_attr(R_INFO));
    }
}

void dk_cpl_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    if (!dk_cpl_count()) { dk_text(sf, clip, c.x + 2, c.y + 1, "No settings in this build.", dk_attr(R_DISABLED)); return; }
    order();
    if (s_sel < 0 || s_sel >= dk_cpl_count()) s_sel = 0;
    /* the bar: which view, how many */
    int x = c.x + 1 + dk_text(sf, clip, c.x + 1, c.y, "View:", dk_attr(R_FACE)) + 1;
    static const char *const VIEW[2] = {"&Icons", "&Details"};
    for (int v = 0; v < 2; v++) {
        s_r_view[v] = tui_rect_make(x, c.y, dk_label_len(VIEW[v]) + 4, 1);
        dk_pushbtn(sf, clip, s_r_view[v], VIEW[v], (s_view == v ? DK_BS_ON : 0) | (s_focus == FOC_ICONS + v ? DK_BS_FOCUS | DK_BS_DEFAULT : 0));
        x += s_r_view[v].w + 1;
    }
    char line[96];
    snprintf(line, sizeof(line), "%d items", s_n);
    dk_text(sf, clip, c.x + c.w - 1 - dk_text_len(line), c.y, line, dk_attr(R_FACE));
    dk_hline(sf, clip, c.x, c.y + 1, c.w, LS_BOX_H, dk_attr(R_SHADOW));
    /* the list */
    s_r_list = tui_rect_make(c.x + 1, c.y + 2, c.w - 2, c.h - 4);
    if (s_r_list.h < 1) return;
    s_cols = s_view == VIEW_ICONS && s_r_list.w / ICON_W > 0 ? s_r_list.w / ICON_W : 1;
    if (s_cols > s_n) s_cols = s_n;
    s_fit = s_view == VIEW_ICONS ? (s_r_list.h + 1) / ICON_H : s_r_list.h - 1;
    if (s_fit < 1) s_fit = 1;
    const int total = rows_total();
    if (s_follow) {
        const int row = s_view == VIEW_ICONS ? pos_of(s_sel) / s_cols : pos_of(s_sel);
        dk_list_keep(total, s_fit, row, &s_top);
        s_follow = false;
    }
    if (s_top > total - s_fit) s_top = total - s_fit;
    if (s_top < 0) s_top = 0;
    const bool bar = total > s_fit;
    const tui_rect lc = tui_rect_intersect(clip, s_r_list);
    if (s_view == VIEW_ICONS) draw_icons(sf, lc);
    else draw_details(sf, lc, bar);
    s_r_bar = tui_rect_make(0, 0, 0, 0);
    if (bar) {
        const int top = s_r_list.y + (s_view == VIEW_DETAILS ? 1 : 0);
        s_r_bar = tui_rect_make(s_r_list.x + s_r_list.w - 1, top, 1, s_r_list.y + s_r_list.h - top);
        dk_vscroll(sf, lc, s_r_bar.x, s_r_bar.y, s_r_bar.h, total, s_fit, s_top);
    }
    /* the foot: what the chosen one is, and how to open it */
    const int fy = c.y + c.h - 1;
    const dk_cpl_applet_t *a = dk_cpl_at(s_sel);
    dk_hline(sf, clip, c.x, fy - 1, c.w, LS_BOX_H, dk_attr(R_SHADOW));
    ls_box_put(sf, clip, c.x + 1, fy, LS_SHAPE_DOT, dk_art(a->hue));
    int fx = c.x + 3 + dk_text(sf, clip, c.x + 3, fy, a->name, dk_attr(R_STRONG));
    snprintf(line, sizeof(line), ": %s", a->what);
    fx += dk_text(sf, clip, fx, fy, line, dk_attr(R_FACE));
    static const char HINT[] = "Double-click or Enter opens";
    if (fx + 2 + (int)sizeof(HINT) < c.x + c.w) dk_text(sf, clip, c.x + c.w - (int)sizeof(HINT), fy, HINT, dk_attr(R_FACE));
}

/* --------------------------------------------------------------- input -- */

static void move(int d)
{
    int p = pos_of(s_sel) + d;
    p = p < 0 ? 0 : p > s_n - 1 ? s_n - 1 : p;
    s_sel = s_order[p];
    s_follow = true;
}

bool dk_cpl_key(dk_win_t *w, ls_tk_t k, char ch)
{
    if (!dk_cpl_count()) { if (k == LS_TK_ESC || k == LS_TK_ENTER) ls_desk_close(w->id); return true; }
    const bool icons = s_view == VIEW_ICONS, list = s_focus == FOC_LIST;
    const uint8_t mods = ls_tui_input_mods();
    switch (k) {
    case LS_TK_TAB: s_focus = (s_focus + (mods & LS_IMOD_SHIFT ? FOC_N - 1 : 1)) % FOC_N; break;
    case LS_TK_UP: if (list) move(icons ? -s_cols : -1); else s_focus = FOC_LIST; break;
    case LS_TK_DOWN: if (list) move(icons ? s_cols : 1); else s_focus = FOC_LIST; break;
    case LS_TK_LEFT: if (!list) s_focus = FOC_ICONS; else if (icons) move(-1); break;
    case LS_TK_RIGHT: if (!list) s_focus = FOC_DETAILS; else if (icons) move(1); break;
    case LS_TK_PGUP: move(-(icons ? s_cols : 1) * s_fit); break;
    case LS_TK_PGDN: move((icons ? s_cols : 1) * s_fit); break;
    case LS_TK_HOME: move(-CPL_MAX); break;
    case LS_TK_END: move(CPL_MAX); break;
    case LS_TK_WHEEL_UP: s_top--; break;            /* the draw keeps it in range */
    case LS_TK_WHEEL_DOWN: s_top++; break;
    case LS_TK_ENTER:
        if (list) open_sel();
        else set_view(s_focus == FOC_ICONS ? VIEW_ICONS : VIEW_DETAILS);
        break;
    case LS_TK_ESC: ls_desk_close(w->id); break;
    case LS_TK_CHAR:
        if (ch == ' ' && !list) { set_view(s_focus == FOC_ICONS ? VIEW_ICONS : VIEW_DETAILS); break; }
        if ((mods & LS_IMOD_ALT) && (ch | 0x20) == 'i') { set_view(VIEW_ICONS); break; }
        if ((mods & LS_IMOD_ALT) && (ch | 0x20) == 'd') { set_view(VIEW_DETAILS); break; }
        /* a letter goes to the next applet that starts with it */
        for (int d = 1; d <= s_n; d++) {
            const int i = s_order[(pos_of(s_sel) + d) % s_n];
            if ((dk_cpl_at(i)->name[0] | 0x20) == (ch | 0x20)) { s_sel = i; s_follow = true; s_focus = FOC_LIST; break; }
        }
        break;
    default: break;
    }
    return true;
}

bool dk_cpl_tap(dk_win_t *w, tui_rect c, int col, int row)
{
    (void)w; (void)c;
    if (!dk_cpl_count()) return true;
    for (int v = 0; v < 2; v++)
        if (tui_rect_contains(s_r_view[v], col, row)) { s_focus = FOC_ICONS + v; set_view(v); return true; }
    if (s_r_bar.w && tui_rect_contains(s_r_bar, col, row)) {
        dk_vscroll_click(s_r_bar.x, s_r_bar.y, s_r_bar.h, rows_total(), s_fit, &s_top, col, row);
        return true;
    }
    if (!tui_rect_contains(s_r_list, col, row)) return true;
    int hit = -1;
    if (s_view == VIEW_ICONS) {
        for (int p = 0; p < s_n; p++) if (tui_rect_contains(icon_rect(p), col, row)) hit = s_order[p];
    } else {
        for (int k = 0; k < COL_N; k++)
            if (tui_rect_contains(s_r_head[k], col, row)) {
                /* the same header again turns the order round */
                if (s_sort == k) s_desc = !s_desc;
                else { s_sort = k; s_desc = false; }
                view_save();
                s_follow = true;
                return true;
            }
        const int k = row - s_r_list.y - 1;
        if (k >= 0 && k < s_fit && s_top + k < s_n) hit = s_order[s_top + k];
    }
    if (hit < 0) return true;
    s_focus = FOC_LIST;
    s_sel = hit;
    if (dk_click_double()) open_sel();
    return true;
}
