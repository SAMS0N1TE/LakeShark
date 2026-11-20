/* Property sheets (see ls_desk_sheet.h): the inks that read in every
   scheme, the controls, and the one sheet that is up. */

#include "ls_desk_sheet.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "../ls_theme.h"
#include "../ls_tui_screen.h"
#include "../ls_tui_touch.h"

/* Inks and small parts are the shell's (ls_desk_priv.h): the sheet adds the
   controls and keeps the one sheet that is up. */

static bool s_front;            /* the sheet's window is the one in front */
static char key_of(const char *s)
{
    const char k = dk_label_key(s);
    return (char)(k >= 'A' && k <= 'Z' ? k + 32 : k);
}

/* ============================================================ the sheet == */

#define SH_MAX 128              /* controls in a sheet, all its tabs */
#define SH_PAGE 48              /* controls on a tab */
#define SH_TABS 6
enum { FOC_TABS = -1, FOC_OK = -2, FOC_CANCEL = -3, FOC_APPLY = -4 };

typedef struct { tui_rect row, hit, a, b; int16_t ci; } sh_slot_t;

static const dk_sheet_t *s_sh;
static int s_tab, s_focus, s_scroll, s_total, s_bodyh;
static bool s_dirty, s_fit;
EXT_RAM_BSS_ATTR static int16_t s_pend[SH_MAX], s_orig[SH_MAX];
EXT_RAM_BSS_ATTR static uint8_t s_touch[SH_MAX];
EXT_RAM_BSS_ATTR static sh_slot_t s_slot[SH_PAGE];
EXT_RAM_BSS_ATTR static int16_t s_rowy[SH_PAGE];
static int s_ns;
/* UI task only; in PSRAM, as internal RAM is short on the Nano */
EXT_RAM_BSS_ATTR static tui_rect s_r_tab[SH_TABS], s_r_btn[3], s_r_side, s_r_body, s_r_bar, s_r_client;
EXT_RAM_BSS_ATTR static struct { bool open; int ci, sel, top, n, rows; tui_rect box, field; } s_dd;

const dk_sheet_t *dk_sheet_now(void) { return s_sh; }
void dk_sheet_dirty(void) { s_dirty = true; }

static const dk_sh_page_t *page(void) { return &s_sh->pages[s_tab]; }
static int base(int tab) { int b = 0; for (int t = 0; t < tab; t++) b += s_sh->pages[t].n; return b; }
static const dk_sh_ctl_t *ctl(int ci) { return ci >= 0 && ci < page()->n ? &page()->ctl[ci] : NULL; }
static bool shown(const dk_sh_ctl_t *c) { return !c->hidden || !c->hidden(); }
static bool is_off(const dk_sh_ctl_t *c) { return c->off && c->off(); }
static bool editable(const dk_sh_ctl_t *c) { return c->kind == SH_CHECK || c->kind == SH_DROP || c->kind == SH_SPIN || c->kind == SH_SLIDER; }
static bool tracked(const dk_sh_ctl_t *c) { return c->get && !(c->flags & SHF_OWN) && editable(c); }
static bool focusable(const dk_sh_ctl_t *c) { return shown(c) && !is_off(c) && (editable(c) || c->kind == SH_BUTTON); }

static int value(int ci)
{
    const dk_sh_ctl_t *c = ctl(ci);
    if (!c || !c->get) return 0;
    const int gi = base(s_tab) + ci;
    return tracked(c) && gi < SH_MAX ? s_pend[gi] : c->get();
}

int dk_sheet_val(const dk_sh_ctl_t *c)
{
    if (!c || !c->get) return 0;
    if (s_sh && tracked(c))
        for (int t = 0, gi = 0; t < s_sh->n; t++)
            for (int i = 0; i < s_sh->pages[t].n; i++, gi++)
                if (&s_sh->pages[t].ctl[i] == c && gi < SH_MAX) return s_pend[gi];
    return c->get();
}

static void bounds(const dk_sh_ctl_t *c, int *lo, int *hi)
{
    if (c->kind == SH_CHECK) { *lo = 0; *hi = 1; }
    else if (c->kind == SH_DROP) { *lo = 0; *hi = (c->count ? c->count() : 1) - 1; }
    else { *lo = c->min; *hi = c->max; }
    if (*hi < *lo) *hi = *lo;
}

static void change(int ci, int v)
{
    const dk_sh_ctl_t *c = ctl(ci);
    if (!c || !editable(c)) return;
    int lo, hi;
    bounds(c, &lo, &hi);
    v = v < lo ? lo : v > hi ? hi : v;
    const int gi = base(s_tab) + ci;
    if (tracked(c) && gi < SH_MAX) {
        if (s_pend[gi] == v) return;
        s_pend[gi] = (int16_t)v;
        s_touch[gi] = 1;
        s_dirty = true;
        if ((c->flags & SHF_LIVE) && c->set) c->set(v);
    } else if (c->set) {
        if (c->get && c->get() == v) return;
        c->set(v);
        if (!(c->flags & SHF_VIEW)) s_dirty = true;
    }
}

static void step(int ci, int d)
{
    const dk_sh_ctl_t *c = ctl(ci);
    if (c) change(ci, value(ci) + d * (c->kind == SH_SPIN || c->kind == SH_SLIDER ? (c->step ? c->step : 1) : 1));
}

static void refresh(int tab)
{
    const dk_sh_page_t *p = &s_sh->pages[tab];
    for (int i = 0, gi = base(tab); i < p->n && gi < SH_MAX; i++, gi++)
        if (tracked(&p->ctl[i]) && !s_touch[gi]) s_pend[gi] = s_orig[gi] = (int16_t)p->ctl[i].get();
}

static void reload(void)
{
    memset(s_touch, 0, sizeof(s_touch));
    s_dirty = false;
    if (s_sh->load) s_sh->load();
    for (int t = 0; t < s_sh->n; t++) refresh(t);
}

static void apply(void)
{
    for (int t = 0, gi = 0; t < s_sh->n; t++)
        for (int i = 0; i < s_sh->pages[t].n; i++, gi++) {
            const dk_sh_ctl_t *c = &s_sh->pages[t].ctl[i];
            if (gi >= SH_MAX || !s_touch[gi] || !tracked(c) || !c->set) continue;
            if (c->get() != s_pend[gi]) c->set(s_pend[gi]);
        }
    if (s_sh->commit) s_sh->commit();
    reload();               /* what the board took, not what was asked */
}

/* Cancel: what was written as it moved goes back to what it was. */
static void cancel(void)
{
    if (!s_sh) return;
    for (int t = 0, gi = 0; t < s_sh->n; t++)
        for (int i = 0; i < s_sh->pages[t].n; i++, gi++) {
            const dk_sh_ctl_t *c = &s_sh->pages[t].ctl[i];
            if (gi >= SH_MAX || !s_touch[gi] || !tracked(c) || !(c->flags & SHF_LIVE) || !c->set) continue;
            if (c->get() != s_orig[gi]) c->set(s_orig[gi]);
        }
    memset(s_touch, 0, sizeof(s_touch));
    s_dirty = false;
}

/* The window is going, by its title key, Alt+F4 or the taskbar: that is
   Cancel. OK and Cancel have already settled everything by then. */
void dk_sheet_closed(void) { cancel(); s_dd.open = false; }

static void close_win(void)
{
    const dk_win_t *w = dk_win_shell(DK_SHELL_DISPLAY);
    if (w) ls_desk_close(w->id);
}

static int first_focus(void)
{
    for (int i = 0; i < page()->n && i < SH_PAGE; i++) if (focusable(&page()->ctl[i])) return i;
    return FOC_OK;
}

void dk_sheet_set_tab(int tab)
{
    if (!s_sh || tab < 0 || tab >= s_sh->n) return;
    const bool on_tabs = s_focus == FOC_TABS;
    s_tab = tab;
    s_scroll = 0;
    s_dd.open = false;
    s_ns = 0;
    refresh(tab);
    if (page()->enter) page()->enter();
    s_focus = on_tabs ? FOC_TABS : first_focus();
}

void dk_sheet_begin(const dk_sheet_t *sh, int tab)
{
    if (!sh || !sh->n) return;
    /* A sheet still up with choices pending is cancelled (one closed by its
       title key was cancelled then: dk_sheet_closed). */
    if (dk_win_shell(DK_SHELL_DISPLAY)) cancel();
    s_sh = sh;
    s_tab = tab >= 0 && tab < sh->n ? tab : 0;
    s_scroll = 0;
    s_dd.open = false;
    s_ns = 0;
    s_fit = true;
    reload();
    if (page()->enter) page()->enter();
    s_focus = first_focus();
}

/* The rows a tab needs, without drawing it. */
static int page_rows(const dk_sh_page_t *p)
{
    int y = 0;
    bool in = false;
    for (int i = 0; i < p->n; i++) {
        const dk_sh_ctl_t *c = &p->ctl[i];
        if (!shown(c)) continue;
        if (c->kind == SH_GROUP) { y += in ? 2 : 1; in = true; }
        else y += c->kind == SH_CUSTOM ? c->rows : 1;
    }
    return y + (in ? 1 : 0);
}

static int want_h(const dk_sheet_t *sh)
{
    int rows = 10;
    for (int t = 0; t < sh->n; t++) {
        const int r = page_rows(&sh->pages[t]);
        if (r > rows) rows = r;
        if (sh->pages[t].side && sh->pages[t].side_h > rows) rows = sh->pages[t].side_h;
    }
    return rows + 5 > 25 ? 25 : rows + 5;       /* tabs 3, the panel's foot 1, the buttons 1 */
}

void dk_sheet_open(const dk_sheet_t *sh, int tab)
{
    if (!sh) return;
    /* the one already up is brought forward as it is, its choices kept */
    if (sh != s_sh || !dk_win_shell(DK_SHELL_DISPLAY)) dk_sheet_begin(sh, tab);
    dk_open_shell(DK_SHELL_DISPLAY, sh->title, sh->w ? sh->w : 66, want_h(sh));
}

void dk_sheet_go(const char *where)
{
    if (!where) return;
    if (!strcmp(where, "@display")) dk_sheet_open(dk_display_sheet(), 1);
    else if (!strcmp(where, "@tasks")) dk_tasks_open();
    else if (!strcmp(where, "@about")) ls_desk_open_about();
    else if (!strcmp(where, "@folder")) dk_folder_open(NULL, -1, -1);
    else if (ls_desk_open_app(where) < 0) dk_msg("Control Panel", where, "That app is not in this build.");
}

/* ------------------------------------------------------------- drawing -- */

static void tabs(tui_surface *sf, tui_rect c, tui_rect clip)
{
    const uint8_t hi = dk_attr(R_HILITE), lo = dk_attr(R_SHADOW), face = dk_attr(R_FACE);
    int x = c.x + 1;
    dk_hline(sf, clip, c.x, c.y + 2, c.w, LS_BOX_H, hi);
    for (int t = 0; t < s_sh->n && t < SH_TABS; t++) {
        const char *title = s_sh->pages[t].title;
        const int w = dk_label_len(title) + 4;
        const bool on = t == s_tab;
        const int top = c.y + (on ? 0 : 1);
        s_r_tab[t] = tui_rect_make(x, c.y, w, 3);
        ls_box_put(sf, clip, x, top, LS_BOX_ARC_DR, hi);
        dk_hline(sf, clip, x + 1, top, w - 2, LS_BOX_H, hi);
        ls_box_put(sf, clip, x + w - 1, top, LS_BOX_ARC_DL, lo);
        for (int y = top + 1; y <= c.y + 1; y++) {
            ls_box_put(sf, clip, x, y, LS_BOX_V, hi);
            ls_box_put(sf, clip, x + w - 1, y, LS_BOX_V, lo);
        }
        const bool foc = on && s_focus == FOC_TABS, hov = !on && dk_hover(s_r_tab[t]);
        const uint8_t a = foc ? dk_attr(R_SELECT) : hov ? dk_attr(R_TASKHOVER) : dk_attr(on ? R_STRONG : R_FACE);
        if (foc || hov) dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(x + 1, c.y + 1, w - 2, 1)), a);
        dk_label(sf, clip, x + 2, c.y + 1, title, a, !foc && !hov);
        if (on) {
            for (int i = 1; i < w - 1; i++) tui_put_char(sf, clip, x + i, c.y + 2, ' ', face);
            ls_box_put(sf, clip, x, c.y + 2, LS_BOX_UL, hi);
            ls_box_put(sf, clip, x + w - 1, c.y + 2, LS_BOX_UR, hi);
        } else {
            ls_box_put(sf, clip, x, c.y + 2, LS_BOX_UH, hi);
            ls_box_put(sf, clip, x + w - 1, c.y + 2, LS_BOX_UH, hi);
        }
        x += w;
    }
}

static void group_edge(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *title)
{
    const uint8_t hi = dk_attr(R_HILITE), lo = dk_attr(R_SHADOW);
    if (title) {
        ls_box_put(sf, clip, x, y, LS_BOX_DR, hi);
        dk_hline(sf, clip, x + 1, y, w - 2, LS_BOX_H, hi);
        ls_box_put(sf, clip, x + w - 1, y, LS_BOX_DL, lo);
        tui_put_char(sf, clip, x + 2, y, ' ', hi);
        const int n = dk_text(sf, clip, x + 3, y, title, dk_hue(s_sh->hue ? s_sh->hue : TUI_CYAN));
        tui_put_char(sf, clip, x + 3 + n, y, ' ', hi);
    } else {
        ls_box_put(sf, clip, x, y, LS_BOX_UR, hi);
        dk_hline(sf, clip, x + 1, y, w - 2, LS_BOX_H, lo);
        ls_box_put(sf, clip, x + w - 1, y, LS_BOX_UL, lo);
    }
}

/* A field's value between its brackets; lit when the focus or the pointer is on it. */
static void field(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *v, bool foc, bool hov, bool off, int chip)
{
    const uint8_t a = off ? dk_attr(R_DISABLED) : foc ? dk_attr(R_SELECT) : hov ? dk_attr(R_TASKHOVER) : dk_attr(R_STRONG);
    const tui_rect in = tui_rect_intersect(clip, tui_rect_make(x + 1, y, w, 1));
    tui_put_char(sf, clip, x, y, '[', off ? dk_attr(R_DISABLED) : dk_attr(R_SHADOW));
    if (foc || hov) dk_fill(sf, in, a);
    int tx = x + 2;
    if (chip >= 0) {
        tui_put_char(sf, in, tx, y, ' ', TUI_ATTR(chip, chip));
        tui_put_char(sf, in, tx + 1, y, ' ', TUI_ATTR(chip, chip));
        tx += 3;
    }
    dk_text(sf, in, tx, y, v, a);
    tui_put_char(sf, clip, x + 1 + w, y, ']', off ? dk_attr(R_DISABLED) : dk_attr(R_HILITE));
}

static void arrow_key(tui_surface *sf, tui_rect clip, tui_rect r, int cp, bool off)
{
    const tui_rect in = tui_rect_intersect(clip, r);
    const uint8_t a = off ? dk_attr(R_DISABLED) : dk_pressed(r) ? dk_attr(R_SELECT) : dk_hover(r) ? dk_attr(R_TASKHOVER) : dk_attr(R_KEY);
    if (!off) dk_fill(sf, in, a);
    tui_put_char(sf, in, r.x, r.y, '[', a);
    ls_box_put(sf, in, r.x + 1, r.y, cp, a);
    tui_put_char(sf, in, r.x + 2, r.y, ']', a);
}

static void ctl_text(const dk_sh_ctl_t *c, int v, char *b, size_t n)
{
    b[0] = 0;
    if (c->text) c->text(v, b, n);
    else if (c->show) c->show(b, n);
    else snprintf(b, n, "%d", v);
}

static void row_draw(tui_surface *sf, tui_rect clip, int ci, const dk_sh_ctl_t *c, int x, int y, int w, int lw, sh_slot_t *s)
{
    const bool off = is_off(c), foc = s_focus == ci && !off;
    const tui_rect row = tui_rect_make(x, y, w, c->kind == SH_CUSTOM ? c->rows : 1);
    const tui_rect rc = tui_rect_intersect(clip, row);
    const bool hov = !off && !s_dd.open && dk_hover(rc);
    const uint8_t la = off ? dk_attr(R_DISABLED) : hov ? dk_attr(R_STRONG) : dk_attr(R_FACE);
    const int fx = x + lw, fw = w - lw;
    char v[72] = "";
    memset(s, 0, sizeof(*s));
    s->row = rc;
    s->ci = (int16_t)ci;
    if (c->label && c->kind != SH_NOTE && c->kind != SH_CHECK && c->kind != SH_CUSTOM && !(c->kind == SH_BUTTON && !c->cap)) {
        const int n = dk_label(sf, rc, x, y, c->label, la, !off);
        tui_put_char(sf, rc, x + n, y, ':', la);
    }
    switch (c->kind) {
    case SH_NOTE:
        if (c->show) c->show(v, sizeof(v));
        dk_text(sf, rc, x, y, c->show ? v : c->label, dk_attr(R_FACE));
        break;
    case SH_TEXT:
        if (c->show) c->show(v, sizeof(v));
        dk_text(sf, tui_rect_intersect(rc, tui_rect_make(fx, y, fw, 1)), fx, y, v, dk_attr(R_STRONG));
        break;
    case SH_CHECK: {
        const bool on = value(ci) != 0;
        const int n = dk_label_len(c->label);
        tui_put_char(sf, rc, x, y, '[', off ? dk_attr(R_DISABLED) : dk_attr(R_SHADOW));
        if (off) tui_put_char(sf, rc, x + 1, y, on ? 'x' : '-', dk_attr(R_DISABLED));
        else if (on) tui_put_char(sf, rc, x + 1, y, 'x', (c->flags & SHF_WARN) ? dk_attr(R_BAD) : dk_attr(R_STRONG));
        tui_put_char(sf, rc, x + 2, y, ']', off ? dk_attr(R_DISABLED) : dk_attr(R_HILITE));
        const uint8_t a = foc ? dk_attr(R_SELECT) : la;
        if (foc) dk_fill(sf, tui_rect_intersect(rc, tui_rect_make(x + 3, y, n + 2, 1)), a);
        dk_label(sf, rc, x + 4, y, c->label, a, !off && !foc);
        s->hit = tui_rect_intersect(rc, tui_rect_make(x, y, n + 6, 1));
        break;
    }
    case SH_DROP: {
        const int count = c->count ? c->count() : 0, cur = value(ci);
        int longest = 6;
        for (int i = 0; i < count && i < 32; i++) {
            ctl_text(c, i, v, sizeof(v));
            const int n = dk_text_len(v);
            if (n > longest) longest = n;
        }
        const int chip = c->flags & SHF_SWATCH ? 3 : 0;
        int dw = longest + 2 + chip;
        if (dw > fw - 5) dw = fw - 5;
        ctl_text(c, cur, v, sizeof(v));
        field(sf, rc, fx, y, dw, v, foc, hov, off, chip ? (cur & 15) : -1);
        s->a = tui_rect_make(fx + dw + 2, y, 3, 1);
        arrow_key(sf, rc, s->a, LS_SHAPE_DOWN, off);
        s->hit = tui_rect_intersect(rc, tui_rect_make(fx, y, dw + 5, 1));
        s->b = tui_rect_make(fx, y, longest + 5 + chip, 1);        /* the list's width */
        break;
    }
    case SH_SPIN: {
        const int dw = fw - 9 < 12 ? fw - 9 : 12;
        ctl_text(c, value(ci), v, sizeof(v));
        field(sf, rc, fx, y, dw, v, foc, hov, off, -1);
        s->a = tui_rect_make(fx + dw + 2, y, 3, 1);
        s->b = tui_rect_make(fx + dw + 5, y, 3, 1);
        arrow_key(sf, rc, s->a, LS_SHAPE_UP, off);
        arrow_key(sf, rc, s->b, LS_SHAPE_DOWN, off);
        s->hit = tui_rect_intersect(rc, tui_rect_make(fx, y, dw + 2, 1));
        break;
    }
    case SH_SLIDER: {
        int tw = fw - 17;
        tw = tw > 26 ? 26 : tw < 6 ? 6 : tw;
        s->a = tui_rect_make(fx, y, 3, 1);
        s->hit = tui_rect_intersect(rc, tui_rect_make(fx + 3, y, tw, 1));
        s->b = tui_rect_make(fx + 3 + tw, y, 3, 1);
        /* held on the track: the thumb follows the pointer */
        int pc = 0, pr = 0;
        if (!off && s_front && dk_pressed(s->hit) && dk_pointer_cell(&pc, &pr) && c->max > c->min && tw > 1) {
            const int stp = c->step ? c->step : 1;
            const int raw = c->min + ((pc - s->hit.x) * (c->max - c->min) + (tw - 1) / 2) / (tw - 1);
            change(ci, c->min + (raw - c->min + stp / 2) / stp * stp);
        }
        const int val = value(ci), span = c->max > c->min ? c->max - c->min : 1;
        const int at = (val - c->min) * (tw - 1) / span;
        arrow_key(sf, rc, s->a, LS_SHAPE_LEFT, off);
        for (int i = 0; i < tw; i++) {
            const uint8_t a = off ? dk_attr(R_DISABLED) : i == at ? (foc ? dk_attr(R_SELECT) : hov ? dk_attr(R_TASKHOVER) : dk_attr(R_STRONG))
                            : i < at ? dk_art(TUI_ATTR_FG(dk_attr(R_GOOD))) : dk_attr(R_SHADOW);
            ls_box_put(sf, rc, fx + 3 + i, y, i == at ? LS_BOX_DBL_VH : i < at ? LS_BOX_DBL_H : LS_BOX_H, a);
        }
        arrow_key(sf, rc, s->b, LS_SHAPE_RIGHT, off);
        ctl_text(c, val, v, sizeof(v));
        dk_text(sf, rc, fx + 7 + tw, y, v, off ? dk_attr(R_DISABLED) : dk_attr(R_STRONG));
        break;
    }
    case SH_GAUGE: {
        int tw = fw - 16;
        tw = tw > 24 ? 24 : tw < 6 ? 6 : tw;
        const int span = c->max > c->min ? c->max - c->min : 100;
        int val = c->get ? c->get() - c->min : 0;
        val = val < 0 ? 0 : val > span ? span : val;
        const int on = (val * tw + span / 2) / span;
        tui_put_char(sf, rc, fx, y, '[', dk_attr(R_SHADOW));
        for (int i = 0; i < tw; i++)
            tui_put_char(sf, rc, fx + 1 + i, y, i < on ? '=' : '-', i < on ? dk_art(TUI_ATTR_FG(dk_attr(R_INFO))) : dk_attr(R_SHADOW));
        tui_put_char(sf, rc, fx + 1 + tw, y, ']', dk_attr(R_HILITE));
        if (c->show) c->show(v, sizeof(v));
        dk_text(sf, rc, fx + 3 + tw, y, v, dk_attr(R_STRONG));
        break;
    }
    case SH_BUTTON: {
        const char *cap = c->cap ? c->cap : c->label;
        const int bx = c->label && c->cap ? fx : x;
        s->hit = tui_rect_intersect(rc, tui_rect_make(bx, y, dk_label_len(cap) + 4, 1));
        dk_pushbtn(sf, rc, s->hit, cap, (off ? DK_BS_DISABLED : 0) | (foc ? DK_BS_FOCUS : 0));
        break;
    }
    case SH_CUSTOM:
        if (c->draw) c->draw(sf, rc, row);
        s->hit = rc;
        break;
    default: break;
    }
}

static void page_draw(tui_surface *sf, tui_rect clip)
{
    const dk_sh_page_t *p = page();
    const tui_rect body = s_r_body;
    int x0 = body.x + 1, w0 = body.w - 2;
    s_r_side = tui_rect_make(0, 0, 0, 0);
    if (p->side && p->side_w && w0 > p->side_w + 22) {
        s_r_side = tui_rect_make(x0, body.y, p->side_w, body.h);
        p->side(sf, tui_rect_intersect(clip, s_r_side), s_r_side);
        x0 += p->side_w + 1;
        w0 -= p->side_w + 1;
    }
    s_total = page_rows(p);
    s_bodyh = body.h;
    if (s_scroll > s_total - body.h) s_scroll = s_total - body.h;
    if (s_scroll < 0) s_scroll = 0;
    const bool bar = s_total > body.h;
    if (bar) w0 -= 2;
    int lw = 0;
    for (int i = 0; i < p->n; i++) {
        const dk_sh_ctl_t *c = &p->ctl[i];
        if (!shown(c) || !c->label || c->kind == SH_GROUP || c->kind == SH_NOTE || c->kind == SH_CHECK || c->kind == SH_CUSTOM) continue;
        if (c->kind == SH_BUTTON && !c->cap) continue;
        const int n = dk_label_len(c->label) + 2;
        if (n > lw) lw = n;
    }
    if (lw > w0 / 2) lw = w0 / 2;
    s_ns = 0;
    int y = 0;
    bool in = false;
    for (int i = 0; i < p->n && i < SH_PAGE; i++) {
        const dk_sh_ctl_t *c = &p->ctl[i];
        s_rowy[i] = (int16_t)y;
        if (!shown(c)) continue;
        const int sy = body.y + y - s_scroll;
        if (c->kind == SH_GROUP) {
            if (in) { group_edge(sf, clip, x0, sy, w0, NULL); y++; }
            group_edge(sf, clip, x0, body.y + y - s_scroll, w0, c->label);
            y++;
            in = true;
            continue;
        }
        const int rows = c->kind == SH_CUSTOM ? c->rows : 1;
        if (in)
            for (int k = 0; k < rows; k++) {
                ls_box_put(sf, clip, x0, sy + k, LS_BOX_V, dk_attr(R_HILITE));
                ls_box_put(sf, clip, x0 + w0 - 1, sy + k, LS_BOX_V, dk_attr(R_SHADOW));
            }
        sh_slot_t scratch, *s = sy >= body.y && sy + rows <= body.y + body.h && s_ns < SH_PAGE ? &s_slot[s_ns++] : &scratch;
        row_draw(sf, clip, i, c, in ? x0 + 2 : x0, sy, in ? w0 - 4 : w0, lw, s);
        y += rows;
    }
    if (in) group_edge(sf, clip, x0, body.y + y - s_scroll, w0, NULL);
    s_r_bar = tui_rect_make(0, 0, 0, 0);
    if (bar) {
        s_r_bar = tui_rect_make(body.x + body.w - 1, body.y, 1, body.h);
        dk_vscroll(sf, clip, s_r_bar.x, body.y, body.h, s_total, body.h, s_scroll);
    }
}

/* ----------------------------------------------------------- drop-down -- */

static void dd_keep(void)
{
    if (s_dd.sel < 0) s_dd.sel = 0;
    if (s_dd.sel > s_dd.n - 1) s_dd.sel = s_dd.n - 1;
    dk_list_keep(s_dd.n, s_dd.rows, s_dd.sel, &s_dd.top);
}

/* Where the list hangs: under its field, or over it when there is more room
   there. Worked out again at every draw, so it follows its window. */
static bool dd_place(void)
{
    const sh_slot_t *s = NULL;
    for (int i = 0; i < s_ns; i++) if (s_slot[i].ci == s_dd.ci) s = &s_slot[i];
    if (!s) return false;
    const tui_rect cl = s_r_client;
    const int below = cl.y + cl.h - (s->hit.y + 1) - 2, above = s->hit.y - cl.y - 2;
    int rows = s_dd.n > 9 ? 9 : s_dd.n;
    const bool up = rows > below && above > below;
    if (rows > (up ? above : below)) rows = up ? above : below;
    if (rows < 1) rows = 1;
    s_dd.rows = rows;
    int w = s->b.w > s->hit.w ? s->b.w : s->hit.w;
    if (s_dd.n > rows) w += 1;
    int x = s->hit.x;
    if (x + w > cl.x + cl.w) x = cl.x + cl.w - w;
    s_dd.box = tui_rect_make(x, up ? s->hit.y - rows - 2 : s->hit.y + 1, w, rows + 2);
    dk_list_keep(s_dd.n, s_dd.rows, -1, &s_dd.top);
    return true;
}

static void dd_open(int ci)
{
    const dk_sh_ctl_t *c = ctl(ci);
    if (!c || !c->count || c->count() < 1) return;
    s_dd.ci = ci;
    s_dd.n = c->count();
    s_dd.sel = value(ci);
    s_dd.top = 0;
    if (!dd_place()) return;
    s_dd.open = true;
    dd_keep();
}

static void dd_pick(void)
{
    s_dd.open = false;
    change(s_dd.ci, s_dd.sel);
}

static void dd_draw(tui_surface *sf, tui_rect clip)
{
    const dk_sh_ctl_t *c = ctl(s_dd.ci);
    if (!c || !dd_place()) { s_dd.open = false; return; }
    const tui_rect b = s_dd.box, in = tui_rect_intersect(clip, dk_inner(b));
    const uint8_t menu = dk_attr(R_MENU), sel = dk_attr(R_SELECT);
    dk_fill(sf, tui_rect_intersect(clip, b), menu);
    dk_frame(sf, clip, b, true);
    int pc = 0, pr = 0;
    /* the bar follows the pointer when the pointer moves, and the keys otherwise */
    static int was_c = -1, was_r = -1;
    if (s_front && dk_pointer_cell(&pc, &pr) && (pc != was_c || pr != was_r)) {
        was_c = pc;
        was_r = pr;
        if (tui_rect_contains(in, pc, pr) && pc < b.x + b.w - (s_dd.n > s_dd.rows ? 2 : 1)) s_dd.sel = s_dd.top + pr - (b.y + 1);
    }
    if (s_dd.sel > s_dd.n - 1) s_dd.sel = s_dd.n - 1;
    const int tw = b.w - 2 - (s_dd.n > s_dd.rows ? 1 : 0);
    for (int k = 0; k < s_dd.rows && s_dd.top + k < s_dd.n; k++) {
        const int i = s_dd.top + k, y = b.y + 1 + k;
        const tui_rect rr = tui_rect_intersect(in, tui_rect_make(b.x + 1, y, tw, 1));
        const uint8_t a = i == s_dd.sel ? sel : menu;
        dk_fill(sf, rr, a);
        int tx = b.x + 2;
        if (c->flags & SHF_SWATCH) {
            tui_put_char(sf, rr, tx, y, ' ', TUI_ATTR(i & 15, i & 15));
            tui_put_char(sf, rr, tx + 1, y, ' ', TUI_ATTR(i & 15, i & 15));
            tx += 3;
        }
        char v[72];
        ctl_text(c, i, v, sizeof(v));
        dk_text(sf, rr, tx, y, v, a);
    }
    if (s_dd.n > s_dd.rows) dk_vscroll(sf, in, b.x + b.w - 2, b.y + 1, s_dd.rows, s_dd.n, s_dd.rows, s_dd.top);
}

/* ------------------------------------------------------------ the frame -- */

void dk_sheet_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    if (!s_sh) return;
    dk_win_t *w = dk_win_shell(DK_SHELL_DISPLAY);
    s_front = w && dk_front_win() == w;
    if (w && s_fit) {
        /* the window takes the sheet's name and its size, once */
        s_fit = false;
        snprintf(w->title, sizeof(w->title), "%s", s_sh->title);
        snprintf(w->label, sizeof(w->label), "%s", s_sh->title);
        tui_rect o;
        dk_win_rects(w, &o, NULL);
        const int ww = (s_sh->w ? s_sh->w : 66) + o.w - c.w, wh = want_h(s_sh) + o.h - c.h;
        if (w->state == LS_DESK_WIN_NORMAL && (ww != o.w || wh != o.h)) {
            const tui_rect work = dk_work();
            ls_desk_resize(w->id, ww, wh);
            ls_desk_move(w->id, (work.w - ww) / 2 > 0 ? (work.w - ww) / 2 : 0, (work.h - wh) / 3 > 0 ? (work.h - wh) / 3 : 0);
        }
    }
    s_r_client = c;
    if (!s_dirty && s_sh->load) s_sh->load();
    refresh(s_tab);
    const int bh = 1, bw = 12;
    const int bottom = c.y + c.h - bh - 1;
    tabs(sf, c, clip);
    for (int y = c.y + 3; y < bottom; y++) {
        ls_box_put(sf, clip, c.x, y, LS_BOX_V, dk_attr(R_HILITE));
        ls_box_put(sf, clip, c.x + c.w - 1, y, LS_BOX_V, dk_attr(R_SHADOW));
    }
    ls_box_put(sf, clip, c.x, c.y + 2, LS_BOX_DR, dk_attr(R_HILITE));
    ls_box_put(sf, clip, c.x + c.w - 1, c.y + 2, LS_BOX_DL, dk_attr(R_SHADOW));
    ls_box_put(sf, clip, c.x, bottom, LS_BOX_UR, dk_attr(R_HILITE));
    dk_hline(sf, clip, c.x + 1, bottom, c.w - 2, LS_BOX_H, dk_attr(R_SHADOW));
    ls_box_put(sf, clip, c.x + c.w - 1, bottom, LS_BOX_UL, dk_attr(R_SHADOW));
    s_r_body = tui_rect_make(c.x + 1, c.y + 3, c.w - 2, bottom - c.y - 3);
    page_draw(sf, tui_rect_intersect(clip, s_r_body));
    if (s_focus == FOC_APPLY && !s_dirty) s_focus = FOC_OK;
    static const char *const B[3] = {"OK", "Cancel", "&Apply"};
    for (int i = 0; i < 3; i++) {
        s_r_btn[i] = tui_rect_make(c.x + c.w - 3 * (bw + 1) + i * (bw + 1), c.y + c.h - bh, bw, bh);
        dk_pushbtn(sf, clip, s_r_btn[i], B[i], (i == 0 ? DK_BS_DEFAULT : 0) | (s_focus == FOC_OK - i ? DK_BS_FOCUS : 0) | (i == 2 && !s_dirty ? DK_BS_DISABLED : 0));
    }
    if (s_dd.open) dd_draw(sf, tui_rect_intersect(clip, c));
}

/* --------------------------------------------------------------- input -- */

static void press(const dk_sh_ctl_t *c)
{
    if (c->press) c->press();
    else if (c->go) dk_sheet_go(c->go);
}

static void do_ok(void) { apply(); close_win(); }
static void do_cancel(void) { cancel(); close_win(); }

static void focus_show(void)
{
    if (s_focus < 0 || s_focus >= SH_PAGE) return;
    const int y = s_rowy[s_focus];
    if (y - 1 < s_scroll) s_scroll = y - 1;
    if (y + 2 > s_scroll + s_bodyh) s_scroll = y + 2 - s_bodyh;
}

static void focus_step(int d)
{
    int o[SH_PAGE + 4], n = 0;
    if (s_sh->n > 1) o[n++] = FOC_TABS;
    for (int i = 0; i < page()->n && i < SH_PAGE; i++) if (focusable(&page()->ctl[i])) o[n++] = i;
    o[n++] = FOC_OK;
    o[n++] = FOC_CANCEL;
    if (s_dirty) o[n++] = FOC_APPLY;
    int at = 0;
    for (int i = 0; i < n; i++) if (o[i] == s_focus) at = i;
    s_focus = o[(at + d + n) % n];
    focus_show();
}

static void activate(int ci)
{
    const dk_sh_ctl_t *c = ctl(ci);
    if (!c || !focusable(c)) return;
    s_focus = ci;
    if (c->kind == SH_CHECK) change(ci, !value(ci));
    else if (c->kind == SH_BUTTON) press(c);
    else if (c->kind == SH_DROP) dd_open(ci);
}

static bool accel(char ch)
{
    ch = (char)(ch | 0x20);
    if (ch < 'a' || ch > 'z') return false;
    for (int i = 0; i < page()->n && i < SH_PAGE; i++) {
        const dk_sh_ctl_t *c = &page()->ctl[i];
        char key = key_of(c->label);
        if (!key && c->kind == SH_BUTTON) key = key_of(c->cap);
        if (!focusable(c) || key != ch) continue;
        s_focus = i;
        focus_show();
        if (c->kind == SH_CHECK) change(i, !value(i));
        else if (c->kind == SH_BUTTON) press(c);
        return true;
    }
    for (int t = 0; t < s_sh->n; t++)
        if (key_of(s_sh->pages[t].title) == ch) { dk_sheet_set_tab(t); return true; }
    if (ch == 'a' && s_dirty) { apply(); return true; }
    return false;
}

static bool dd_key(ls_tk_t k, char ch)
{
    switch (k) {
    case LS_TK_UP: s_dd.sel--; break;
    case LS_TK_DOWN: s_dd.sel++; break;
    case LS_TK_WHEEL_UP: s_dd.sel--; break;
    case LS_TK_WHEEL_DOWN: s_dd.sel++; break;
    case LS_TK_PGUP: s_dd.sel -= s_dd.rows; break;
    case LS_TK_PGDN: s_dd.sel += s_dd.rows; break;
    case LS_TK_HOME: s_dd.sel = 0; break;
    case LS_TK_END: s_dd.sel = s_dd.n - 1; break;
    case LS_TK_ENTER: dd_pick(); return true;
    case LS_TK_ESC: s_dd.open = false; break;
    case LS_TK_TAB: dd_pick(); focus_step(dk_mods() & LS_IMOD_SHIFT ? -1 : 1); return true;
    case LS_TK_CHAR:
        if (ch == ' ') { dd_pick(); return true; }
        for (int d = 1; d <= s_dd.n; d++) {
            char v[72];
            const int i = (s_dd.sel + d) % s_dd.n;
            ctl_text(ctl(s_dd.ci), i, v, sizeof(v));
            if ((v[0] | 0x20) == (ch | 0x20)) { s_dd.sel = i; break; }
        }
        break;
    default: break;
    }
    dd_keep();
    return true;
}

bool dk_sheet_key(ls_tk_t k, char ch)
{
    if (!s_sh) return false;
    if (k == LS_TK_SHIFT || k == LS_TK_ALT || k == LS_TK_CTRL) return true;
    if (s_dd.open) return dd_key(k, ch);
    const dk_sh_ctl_t *c = s_focus >= 0 ? ctl(s_focus) : NULL;
    const bool ranged = c && (c->kind == SH_SLIDER || c->kind == SH_SPIN), drop = c && c->kind == SH_DROP;
    const int n = s_sh->n;
    switch (k) {
    case LS_TK_TAB: focus_step(dk_mods() & LS_IMOD_SHIFT ? -1 : 1); break;
    case LS_TK_PGDN: dk_sheet_set_tab((s_tab + 1) % n); break;
    case LS_TK_PGUP: dk_sheet_set_tab((s_tab + n - 1) % n); break;
    case LS_TK_ESC: do_cancel(); return true;
    case LS_TK_ENTER:
        if (s_focus == FOC_CANCEL) do_cancel();
        else if (s_focus == FOC_APPLY) apply();
        else if (c && (c->kind == SH_BUTTON || drop)) activate(s_focus);
        else do_ok();
        return true;
    case LS_TK_UP:
        if (ranged) step(s_focus, +1);
        else if (drop) step(s_focus, -1);
        else focus_step(-1);
        break;
    case LS_TK_DOWN:
        if (ranged) step(s_focus, -1);
        else if (drop) step(s_focus, +1);
        else focus_step(+1);
        break;
    case LS_TK_LEFT:
        if (s_focus == FOC_TABS) dk_sheet_set_tab((s_tab + n - 1) % n);
        else if (ranged || drop) step(s_focus, -1);
        else focus_step(-1);
        break;
    case LS_TK_RIGHT:
        if (s_focus == FOC_TABS) dk_sheet_set_tab((s_tab + 1) % n);
        else if (ranged || drop) step(s_focus, +1);
        else focus_step(+1);
        break;
    case LS_TK_HOME: if (ranged) change(s_focus, c->min); else if (drop) change(s_focus, 0); break;
    case LS_TK_END: if (ranged) change(s_focus, c->max); else if (drop) change(s_focus, 32767); break;
    case LS_TK_F4: if (drop) dd_open(s_focus); break;
    case LS_TK_WHEEL_UP: case LS_TK_WHEEL_DOWN: {
        const int d = k == LS_TK_WHEEL_UP ? 1 : -1;
        int pc = 0, pr = 0;
        if (dk_pointer_cell(&pc, &pr))
            for (int i = 0; i < s_ns; i++) {
                const dk_sh_ctl_t *h = ctl(s_slot[i].ci);
                if (h && (h->kind == SH_SLIDER || h->kind == SH_SPIN) && !is_off(h) && tui_rect_contains(s_slot[i].row, pc, pr)) {
                    step(s_slot[i].ci, d);
                    return true;
                }
            }
        s_scroll -= d * 2;          /* page_draw keeps it in range */
        break;
    }
    case LS_TK_CHAR:
        if (ch == ' ') {
            if (s_focus == FOC_OK) do_ok();
            else if (s_focus == FOC_CANCEL) do_cancel();
            else if (s_focus == FOC_APPLY) apply();
            else if (c) activate(s_focus);
            return true;
        }
        if (ranged && (ch == '+' || ch == '=')) step(s_focus, +1);
        else if (ranged && (ch == '-' || ch == '_')) step(s_focus, -1);
        else accel(ch);
        break;
    default: break;
    }
    return true;
}

bool dk_sheet_tap(tui_rect c, int col, int row)
{
    (void)c;
    if (!s_sh) return false;
    if (s_dd.open) {
        const tui_rect in = dk_inner(s_dd.box);
        const bool bar = s_dd.n > s_dd.rows;
        if (bar && col == s_dd.box.x + s_dd.box.w - 2 && row >= in.y && row < in.y + in.h) {
            dk_vscroll_click(col, in.y, s_dd.rows, s_dd.n, s_dd.rows, &s_dd.top, col, row);
            if (s_dd.sel < s_dd.top) s_dd.sel = s_dd.top;
            if (s_dd.sel >= s_dd.top + s_dd.rows) s_dd.sel = s_dd.top + s_dd.rows - 1;
        } else if (tui_rect_contains(in, col, row)) {
            s_dd.sel = s_dd.top + row - in.y;
            if (s_dd.sel < s_dd.n) dd_pick();
        } else s_dd.open = false;
        return true;
    }
    for (int t = 0; t < s_sh->n && t < SH_TABS; t++)
        if (tui_rect_contains(s_r_tab[t], col, row)) { s_focus = FOC_TABS; dk_sheet_set_tab(t); return true; }
    if (tui_rect_contains(s_r_btn[0], col, row)) { do_ok(); return true; }
    if (tui_rect_contains(s_r_btn[1], col, row)) { do_cancel(); return true; }
    if (tui_rect_contains(s_r_btn[2], col, row)) { if (s_dirty) apply(); return true; }
    if (s_r_bar.w && tui_rect_contains(s_r_bar, col, row)) {
        dk_vscroll_click(s_r_bar.x, s_r_bar.y, s_r_bar.h, s_total, s_bodyh, &s_scroll, col, row);
        return true;
    }
    if (s_r_side.w && tui_rect_contains(s_r_side, col, row)) {
        if (page()->side_tap) page()->side_tap(s_r_side, col, row);
        return true;
    }
    if (!tui_rect_contains(s_r_body, col, row)) return true;
    for (int i = 0; i < s_ns; i++) {
        const sh_slot_t *s = &s_slot[i];
        const dk_sh_ctl_t *k = ctl(s->ci);
        if (!k || !tui_rect_contains(s->row, col, row) || is_off(k)) continue;
        const bool on_hit = tui_rect_contains(s->hit, col, row);
        const bool on_a = s->a.w && tui_rect_contains(s->a, col, row), on_b = s->b.w && tui_rect_contains(s->b, col, row);
        if (focusable(k) && (on_hit || on_a || on_b || k->kind == SH_CHECK)) s_focus = s->ci;
        switch (k->kind) {
        case SH_CHECK: if (on_hit) change(s->ci, !value(s->ci)); break;
        case SH_DROP: if (on_hit) dd_open(s->ci); break;
        case SH_SPIN: if (on_a) step(s->ci, +1); else if (on_b) step(s->ci, -1); break;
        case SH_SLIDER:
            if (on_a) step(s->ci, -1);
            else if (on_b) step(s->ci, +1);
            else if (on_hit && s->hit.w > 1 && k->max > k->min) {
                const int stp = k->step ? k->step : 1;
                const int raw = k->min + ((col - s->hit.x) * (k->max - k->min) + (s->hit.w - 1) / 2) / (s->hit.w - 1);
                change(s->ci, k->min + (raw - k->min + stp / 2) / stp * stp);
            }
            break;
        case SH_BUTTON: if (on_hit) press(k); break;
        case SH_CUSTOM: if (k->tap) k->tap(s->row, col, row); break;
        default: break;
        }
        return true;
    }
    return true;
}
