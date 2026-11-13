/* The Control Panel: every setting the board has, in one window.

   The first page is a tile for each applet: its mark, its name and one live
   line of where things stand. A tile opens its page: rows of settings, each
   a value that reads, a level with < and >, a switch, or a way into the app
   that has the rest. Esc or Back returns; nothing is more than two levels
   down. The rows themselves, and what they call, are in ls_desk_cplsys.c. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>

#include "../ls_tui_screen.h"

static int s_page = -1;           /* -1 the tiles, else an applet */
static int s_sel, s_top;

static uint8_t on_face(uint8_t fg) { return (uint8_t)((dk_attr(R_FACE) & 0xF0) | (fg & 0x0F)); }
static uint8_t hue_on_face(uint8_t hue)
{
    uint8_t c = hue & 7;
    if (!dk_light_face()) return on_face((uint8_t)(c == TUI_BLACK ? TUI_WHITE : c | TUI_BRIGHT));
    if (c == TUI_YELLOW) c = TUI_RED;
    if (c == TUI_CYAN) c = TUI_BLUE;
    if (c == TUI_WHITE) c = TUI_BLACK;
    return on_face(c);
}

#define BTN_W 14
static tui_rect btn(tui_rect c, int from_right) { return tui_rect_make(c.x + c.w - (BTN_W + 1) * (from_right + 1), c.y + c.h - 3, BTN_W, 3); }

void dk_cpl_open(int applet)
{
    s_page = applet >= 0 && applet < dk_cpl_count() ? applet : -1;
    s_sel = 0;
    s_top = 0;
    if (s_page >= 0 && dk_cpl_at(s_page)->enter) dk_cpl_at(s_page)->enter();
    dk_open_shell(DK_SHELL_CPL, "Control Panel", 78, 27);
}

static void page(int p)
{
    const int from = s_page;
    s_page = p;
    s_sel = p < 0 && from >= 0 ? from : 0;
    s_top = 0;
    if (p >= 0 && dk_cpl_at(p)->enter) dk_cpl_at(p)->enter();
}

/* An applet's mark: three rows of six cells; box and braille codepoints as
   UTF-8, drawn in its colour. */
static void art(tui_surface *sf, tui_rect clip, int x, int y, const dk_cpl_applet_t *a)
{
    for (int r = 0; r < 3; r++) if (a->art[r]) dk_text(sf, clip, x, y + r, a->art[r], hue_on_face(a->hue));
}

/* ---------------------------------------------------------------- tiles -- */

#define TILE_H 4
static tui_rect tile_rect(tui_rect c, int i)
{
    const int n = dk_cpl_count(), per = (n + 1) / 2, w = (c.w - 4) / 2;
    return tui_rect_make(c.x + 2 + (i / per) * w, c.y + 2 + (i % per) * TILE_H, w - 1, TILE_H - 1);
}

static void draw_tiles(tui_surface *sf, tui_rect c, tui_rect clip)
{
    dk_text(sf, clip, c.x + 2, c.y, "Every setting the board has. Pick one; each value shown is live.", dk_attr(R_FACE));
    for (int i = 0; i < dk_cpl_count(); i++) {
        const dk_cpl_applet_t *a = dk_cpl_at(i);
        const tui_rect r = tile_rect(c, i);
        const tui_rect t = tui_rect_intersect(clip, r);
        const bool sel = i == s_sel;
        art(sf, t, r.x + 1, r.y, a);
        const int x = r.x + 9;
        if (sel) dk_fill(sf, tui_rect_intersect(t, tui_rect_make(x - 1, r.y, r.w - 9, 1)), dk_attr(R_SELECT));
        dk_text(sf, t, x, r.y, a->name, sel ? dk_attr(R_SELECT) : hue_on_face(a->hue));
        dk_text(sf, t, x, r.y + 1, a->what, dk_attr(R_FACE));
        char line[64] = "";
        if (a->summary) a->summary(line, sizeof(line));
        dk_text(sf, t, x, r.y + 2, line, dk_attr(R_INFO));
    }
    dk_button(sf, clip, btn(c, 1), "Open", true, dk_pressed(btn(c, 1)));
    dk_button(sf, clip, btn(c, 0), "Close", false, dk_pressed(btn(c, 0)));
}

/* ----------------------------------------------------------------- page -- */

#define LABEL_W 22
#define METER_W 24

static int vis_rows(const dk_cpl_applet_t *a, int *idx, int cap)
{
    int n = 0;
    for (int i = 0; i < a->n && n < cap; i++)
        if (!a->rows[i].hidden || !a->rows[i].hidden()) idx[n++] = i;
    return n;
}

static int rows_y(tui_rect c) { return c.y + 5; }
static int rows_fit(tui_rect c) { const int f = c.h - 10; return f > 1 ? f : 1; }
static int value_x(tui_rect c) { return c.x + 4 + LABEL_W; }

static void draw_row(tui_surface *sf, tui_rect clip, tui_rect c, int y, const dk_cpl_row_t *r, bool sel)
{
    const int lx = c.x + 3, vx = value_x(c);
    if (sel) dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(lx - 1, y, LABEL_W + 1, 1)), dk_attr(R_SELECT));
    dk_text(sf, tui_rect_intersect(clip, tui_rect_make(lx, y, LABEL_W, 1)), lx, y, r->label, sel ? dk_attr(R_SELECT) : dk_attr(R_FACE));
    char v[72] = "";
    if (r->show) r->show(v, sizeof(v));
    const tui_rect vc = tui_rect_intersect(clip, tui_rect_make(vx, y, c.x + c.w - 2 - vx, 1));
    const uint8_t arrow = sel ? dk_attr(R_ACCEL) : dk_attr(R_SHADOW);
    switch (r->kind) {
    case CR_LEVEL:
        ls_box_put(sf, vc, vx, y, LS_SHAPE_LEFT, arrow);
        dk_meter(sf, vc, vx + 2, y, METER_W, (r->pct ? r->pct() : 0) * 10, TUI_GREEN);
        ls_box_put(sf, vc, vx + 3 + METER_W, y, LS_SHAPE_RIGHT, arrow);
        dk_text(sf, vc, vx + 6 + METER_W, y, v, dk_attr(R_STRONG));
        break;
    case CR_CHOICE:
        ls_box_put(sf, vc, vx, y, LS_SHAPE_LEFT, arrow);
        dk_text(sf, vc, vx + 2, y, v, dk_attr(R_STRONG));
        ls_box_put(sf, vc, vx + 3 + dk_text_len(v), y, LS_SHAPE_RIGHT, arrow);
        break;
    case CR_SWITCH: {
        const bool on = r->on && r->on();
        dk_check(sf, vc, vx, y, on, NULL);
        dk_text(sf, vc, vx + 4, y, v, on ? (r->warn ? dk_attr(R_BAD) : dk_attr(R_GOOD)) : dk_attr(R_FACE));
        break;
    }
    case CR_GO:
        dk_text(sf, vc, vx, y, v, dk_attr(R_INFO));
        ls_box_put(sf, vc, vx + dk_text_len(v) + 1, y, LS_SHAPE_RIGHT, arrow);
        break;
    case CR_ACTION:
        tui_put_char(sf, vc, vx, y, '[', dk_attr(R_HILITE));
        dk_text(sf, vc, vx + 2, y, v, dk_attr(R_STRONG));
        tui_put_char(sf, vc, vx + 3 + dk_text_len(v), y, ']', dk_attr(R_SHADOW));
        break;
    default:
        dk_text(sf, vc, vx, y, v, dk_attr(R_STRONG));
        break;
    }
}

static void draw_page(tui_surface *sf, tui_rect c, tui_rect clip)
{
    const dk_cpl_applet_t *a = dk_cpl_at(s_page);
    art(sf, clip, c.x + 3, c.y + 1, a);
    dk_text(sf, clip, c.x + 12, c.y + 1, a->name, hue_on_face(a->hue));
    dk_text(sf, clip, c.x + 12, c.y + 2, a->what, dk_attr(R_FACE));
    dk_hline(sf, clip, c.x + 2, c.y + 4, c.w - 4, LS_BOX_H, dk_attr(R_SHADOW));
    int idx[32];
    const int n = vis_rows(a, idx, 32), fit = rows_fit(c);
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < 0) s_sel = 0;
    dk_list_keep(n, fit, s_sel, &s_top);
    for (int k = 0; k < fit && s_top + k < n; k++)
        draw_row(sf, clip, c, rows_y(c) + k, &a->rows[idx[s_top + k]], s_top + k == s_sel);
    if (n > fit) {
        char more[48];
        snprintf(more, sizeof(more), "%d-%d of %d", s_top + 1, s_top + fit < n ? s_top + fit : n, n);
        dk_text(sf, clip, c.x + c.w - 2 - dk_text_len(more), c.y + 3, more, dk_attr(R_DISABLED));
    }
    /* what the keys do on the selected row, said once */
    const dk_cpl_row_t *r = n ? &a->rows[idx[s_sel]] : NULL;
    const char *hint = !r ? "" : r->kind == CR_LEVEL || r->kind == CR_CHOICE ? "Left and Right change it"
                     : r->kind == CR_SWITCH ? "Enter or Space switches it" : r->kind == CR_GO ? "Enter opens it"
                     : r->kind == CR_ACTION ? "Enter does it" : "";
    dk_text(sf, clip, c.x + 3, c.y + c.h - 4, hint, dk_attr(R_DISABLED));
    dk_button(sf, clip, btn(c, 2), "Back", false, dk_pressed(btn(c, 2)));
    if (a->app) {
        const ls_app_t *app = ls_app_by_id(a->app);
        char label[20];
        snprintf(label, sizeof(label), "%.12s", app && app->name ? app->name : a->app);
        if (app && app->screen) dk_button(sf, clip, btn(c, 1), label, false, dk_pressed(btn(c, 1)));
    }
    dk_button(sf, clip, btn(c, 0), "Close", true, dk_pressed(btn(c, 0)));
}

void dk_cpl_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    if (!dk_cpl_count()) { dk_text(sf, clip, c.x + 2, c.y + 1, "No settings in this build.", dk_attr(R_DISABLED)); return; }
    if (s_page < 0) draw_tiles(sf, c, clip);
    else draw_page(sf, c, clip);
}

/* ---------------------------------------------------------------- input -- */

static void go(const char *where)
{
    if (!where) return;
    if (!strcmp(where, "@display")) dk_do(A_DISPLAY, 0);
    else if (!strcmp(where, "@tasks")) dk_tasks_open();
    else if (!strcmp(where, "@about")) ls_desk_open_about();
    else if (!strcmp(where, "@folder")) dk_folder_open(NULL, -1, -1);
    else if (ls_desk_open_app(where) < 0) dk_msg("Control Panel", where, "That app is not in this build.");
}

static void act(const dk_cpl_row_t *r, int dir)
{
    switch (r->kind) {
    case CR_LEVEL: case CR_CHOICE: if (r->step) r->step(dir); break;
    case CR_SWITCH: case CR_ACTION: if (r->step) r->step(+1); break;
    case CR_GO: go(r->go); break;
    default: break;
    }
    ls_tui_invalidate();
}

bool dk_cpl_key(dk_win_t *w, ls_tk_t k, char ch)
{
    const int count = dk_cpl_count();
    if (!count) { if (k == LS_TK_ESC || k == LS_TK_ENTER) ls_desk_close(w->id); return true; }
    if (s_page < 0) {
        const int per = (count + 1) / 2;
        switch (k) {
        case LS_TK_UP: case LS_TK_WHEEL_UP: if (s_sel > 0) s_sel--; break;
        case LS_TK_DOWN: case LS_TK_WHEEL_DOWN: case LS_TK_TAB: if (s_sel < count - 1) s_sel++; break;
        case LS_TK_RIGHT: if (s_sel + per < count) s_sel += per; break;
        case LS_TK_LEFT: if (s_sel >= per) s_sel -= per; break;
        case LS_TK_HOME: s_sel = 0; break;
        case LS_TK_END: s_sel = count - 1; break;
        case LS_TK_ENTER: page(s_sel); break;
        case LS_TK_ESC: ls_desk_close(w->id); break;
        case LS_TK_CHAR:
            /* a letter goes to the next applet that starts with it */
            for (int d = 1; d <= count; d++) {
                const int i = (s_sel + d) % count;
                if ((dk_cpl_at(i)->name[0] | 0x20) == (ch | 0x20)) { s_sel = i; break; }
            }
            break;
        default: break;
        }
        return true;
    }
    const dk_cpl_applet_t *a = dk_cpl_at(s_page);
    int idx[32];
    const int n = vis_rows(a, idx, 32);
    const dk_cpl_row_t *r = n && s_sel < n ? &a->rows[idx[s_sel]] : NULL;
    switch (k) {
    case LS_TK_UP: case LS_TK_WHEEL_UP: if (s_sel > 0) s_sel--; break;
    case LS_TK_DOWN: case LS_TK_WHEEL_DOWN: case LS_TK_TAB: if (s_sel < n - 1) s_sel++; break;
    case LS_TK_HOME: s_sel = 0; break;
    case LS_TK_END: s_sel = n - 1; break;
    case LS_TK_LEFT: if (r && (r->kind == CR_LEVEL || r->kind == CR_CHOICE)) act(r, -1); break;
    case LS_TK_RIGHT: if (r && (r->kind == CR_LEVEL || r->kind == CR_CHOICE)) act(r, +1); break;
    case LS_TK_ENTER: if (r) act(r, +1); break;
    case LS_TK_CHAR:
        if (ch == ' ' && r) act(r, +1);
        else if ((ch == '-' || ch == ',' || ch == '<') && r && (r->kind == CR_LEVEL || r->kind == CR_CHOICE)) act(r, -1);
        else if ((ch == '+' || ch == '=' || ch == '.' || ch == '>') && r && (r->kind == CR_LEVEL || r->kind == CR_CHOICE)) act(r, +1);
        break;
    case LS_TK_ESC: case LS_TK_BACKSPACE: page(-1); break;
    default: break;
    }
    return true;
}

bool dk_cpl_tap(dk_win_t *w, tui_rect c, int col, int row)
{
    const int count = dk_cpl_count();
    if (tui_rect_contains(btn(c, 0), col, row)) { ls_desk_close(w->id); return true; }
    if (!count) return true;
    if (s_page < 0) {
        if (tui_rect_contains(btn(c, 1), col, row)) { page(s_sel); return true; }
        for (int i = 0; i < count; i++)
            if (tui_rect_contains(tile_rect(c, i), col, row)) {
                /* one click picks, a second (or a double) opens */
                if (s_sel == i || dk_click_double()) page(i);
                else s_sel = i;
                return true;
            }
        return true;
    }
    const dk_cpl_applet_t *a = dk_cpl_at(s_page);
    if (tui_rect_contains(btn(c, 2), col, row)) { page(-1); return true; }
    if (a->app && tui_rect_contains(btn(c, 1), col, row)) { go(a->app); return true; }
    int idx[32];
    const int n = vis_rows(a, idx, 32), k = row - rows_y(c);
    if (k < 0 || k >= rows_fit(c) || s_top + k >= n || col < c.x + 2 || col >= c.x + c.w - 2) return true;
    s_sel = s_top + k;
    const dk_cpl_row_t *r = &a->rows[idx[s_sel]];
    const int vx = value_x(c);
    if (col < vx) return true;                          /* the label only selects */
    if (r->kind == CR_LEVEL) {
        const int mx = vx + 3, mw = METER_W - 2;        /* inside the meter's brackets */
        if (col <= vx + 1) act(r, -1);
        else if (col >= vx + 2 + METER_W && col <= vx + 4 + METER_W) act(r, +1);
        else if (col >= mx && col < mx + mw && r->set) { r->set((col - mx) * 100 / (mw - 1)); ls_tui_invalidate(); }
    } else if (r->kind == CR_CHOICE) {
        act(r, col <= vx + 1 ? -1 : +1);
    } else {
        act(r, +1);
    }
    return true;
}
