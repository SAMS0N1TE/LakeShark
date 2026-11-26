/* See ls_tui_ui.h for why buttons are blocks of cells. */
#include "ls_tui_ui.h"

#include <stdio.h>
#include <string.h>

#include "ls_icons.h"
#include "ls_i18n.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "ls_box.h"
#include "ls_deskui.h"

/* Shared out of line: avoid duplicating corner clipping at every label. */
void ls_safe_line(tui_surface *sf, tui_rect area, int row,
                  const char *text, uint8_t attr)
{
    if (!sf || !text || row < area.y || row >= area.y + area.h) return;
    const int cols = tui_surface_rect(sf).w;
    const int pad = ls_tui_corner_pad(row) + 1;
    int left = area.x + 1, right = area.x + area.w - 1;
    if (left < pad) left = pad;
    if (right > cols - pad) right = cols - pad;
    if (right <= left) return;
    tui_rect line = tui_rect_make(left, row, right - left, 1);
    tui_put_str(sf, line, left, row, text, attr);
}

#define MAX_HITS 24

typedef struct { int16_t x0, y0, x1, y1; } hit_t;

#define BTN_SLOTS 3
/* A button stops getting wider. At file scope because
   ls_btn_compact_fits has to answer for the same button the renderer
   will draw, and a second copy of this number is a second chance for
   the two to disagree about whether a word will fit. */
#define BTN_MAX_W 16
EXT_RAM_BSS_ATTR static hit_t s_btn_hit[BTN_SLOTS][MAX_HITS];
static int   s_btn_n[BTN_SLOTS];
static bool s_btn_enabled[BTN_SLOTS][MAX_HITS];
static char s_btn_keys[BTN_SLOTS][MAX_HITS];
EXT_RAM_BSS_ATTR static uint32_t s_btn_value[BTN_SLOTS][MAX_HITS];
EXT_RAM_BSS_ATTR static int64_t s_btn_change[BTN_SLOTS][MAX_HITS];
/* Where the bar that last drew each key stood. An app may draw two bars in
   one frame (a tab strip and its keys); each then overwrites the other's
   record, and only a change within the same bar is a change. */
EXT_RAM_BSS_ATTR static int32_t s_btn_at[BTN_SLOTS][MAX_HITS];

/* Records key i's face; true while it shows a fresh change (a press, a new value). */
static bool btn_changed(int slot, int i, uint32_t token, tui_rect bar, int64_t now)
{
    const int32_t at = (int32_t)(((uint32_t)(uint16_t)bar.y << 16) | (uint16_t)bar.x);
    if (s_btn_value[slot][i] && s_btn_value[slot][i] != token && s_btn_at[slot][i] == at)
        s_btn_change[slot][i] = now;
    s_btn_value[slot][i] = token;
    s_btn_at[slot][i] = at;
    return s_btn_change[slot][i] && now - s_btn_change[slot][i] < 240000;
}

EXT_RAM_BSS_ATTR static hit_t s_tile_hit[MAX_HITS];
static int   s_tile_n, s_tile_cols, s_tile_rows;

static void hit_clear(hit_t *h, int *n) { *n = 0; (void)h; }

static int hit_find(const hit_t *h, int n, int col, int row)
{
    for (int i = 0; i < n; i++)
        if (col >= h[i].x0 && col <= h[i].x1 && row >= h[i].y0 && row <= h[i].y1)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ button */

/* A button is a filled block, not a bracketed label.

   `[SPLIT]` reads as text and invites a tap on the word; a filled rectangle
   reads as a target and invites a tap anywhere inside it. On a panel where
   the difference between hitting and missing is four millimetres that is not
   decoration, it is the whole affordance - and it costs the same number of
   cells either way, because the background was going to be painted regardless. */
/* The "something new to download" marker: a drip falling through ASCII,
   . : v V v :, five frames a second off the same clock as the LIVE spinner. */
static char drip_glyph(void)
{
    return 'v';   /* steady: it falls only when there is something new, not on a clock */
}

/* A label that does not fit is shortened on purpose, not cut mid-word.

   Whole word if it fits; else the first part of a hyphenated or spaced one
   (RTL-SDR becomes RTL); else the vowels leave from the back (SPECTRUM becomes
   SPCTRM); and only then the plain cut. Returns true when it came to that. */
static bool fit_label(char *dst, size_t cap, const char *src, int w)
{
    if (cap == 0) return false;
    if (w < 1) w = 1;
    if ((size_t)w > cap - 1) w = (int)cap - 1;
    snprintf(dst, cap, "%s", src ? src : "");
    int n = (int)strlen(dst);
    if (n <= w) return false;
    int head = 0;
    while (dst[head] && dst[head] != '-' && dst[head] != ' ' && dst[head] != '/') head++;
    if (head >= 3 && head < n && head <= w) { dst[head] = 0; return false; }
    for (int i = n - 2; i >= 1 && n > w; i--) {
        const char c = dst[i];
        if (c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' ||
            c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') {
            memmove(dst + i, dst + i + 1, (size_t)(n - i));
            n--;
        }
    }
    if (n <= w) return false;
    dst[w] = 0;
    return true;
}

/* How many columns a button's lettering wants, frame and gap included. */
static int button_want(const ls_btn_t *b, int row_h)
{
    const int lab = b->label ? (int)strlen(b->label) : 0;
    const int val = b->value ? (int)strlen(b->value) : 0;
    int text = lab;
    if (row_h == 3 && b->value) text = lab + 1 + val;
    else if (row_h > 1 && b->value && val > lab) text = val;
    return text + (row_h >= 3 ? 3 : 1);
}

/* The buttons in the interface language: a copy whose label and value are
   ls_tr()'d, or the caller's own array in English. */
#define TR_BTN_MAX 32
static const ls_btn_t *tr_buttons(const ls_btn_t *btn, int n, ls_btn_t *copy)
{
    if (ls_i18n_lang() == LS_LANG_EN || !btn || n <= 0 || n > TR_BTN_MAX) return btn;
    for (int i = 0; i < n; i++) {
        copy[i] = btn[i];
        copy[i].label = ls_tr(btn[i].label);
        copy[i].value = ls_tr(btn[i].value);
    }
    return copy;
}

/* ------------------------------------------------------------ desk compact */

#if LS_TUI_HAS_COMPACT
static bool s_compact;
bool ls_tui_compact(void) { return s_compact; }
void ls_tui_set_compact(bool on) { s_compact = on; }
/* Colour roles (ls_tui_ui.h): filled by the desk for the frame it draws. */
uint8_t ls_ink_tab[LS_INK_COUNT];
bool ls_ink_on;
EXT_RAM_BSS_ATTR static ls_ink_keep_t s_ink_keep;
void ls_ink_set(const uint8_t *tab)
{
    ls_ink_on = tab != NULL;
    if (tab) memcpy(ls_ink_tab, tab, sizeof(ls_ink_tab));
    s_ink_keep.n = 0;
}
void ls_ink_mark(tui_rect r, int kind)
{
    if (!ls_ink_on || r.w < 1 || r.h < 1) return;
    /* The last rect of this kind, right beside it or right under it with the
       same span: one rect (a column of fields, a row of keys). */
    for (int i = s_ink_keep.n - 1; i >= 0 && i >= s_ink_keep.n - 2; i--) {
        if (s_ink_keep.kind[i] != kind) continue;
        int16_t *q = &s_ink_keep.r[i].x;
        if (q[0] == r.x && q[1] == r.y && q[2] == r.w && q[3] == r.h && i == s_ink_keep.n - 1) return;
        if (kind == LS_INK_K_RING) continue;              /* two rings are two frames */
        if (i < s_ink_keep.n - 1) {                       /* joining past a later rect must not put r under it */
            const int16_t *l = &s_ink_keep.r[s_ink_keep.n - 1].x;
            if (r.x < l[0] + l[2] && l[0] < r.x + r.w && r.y < l[1] + l[3] && l[1] < r.y + r.h) continue;
        }
        if (q[0] == r.x && q[2] == r.w && q[1] + q[3] == r.y) { q[3] = (int16_t)(q[3] + r.h); return; }
        if (q[1] == r.y && q[3] == r.h && q[0] + q[2] == r.x) { q[2] = (int16_t)(q[2] + r.w); return; }
    }
    if (s_ink_keep.n >= LS_INK_KEEP_MAX) return;
    const int i = s_ink_keep.n++;
    s_ink_keep.r[i].x = (int16_t)r.x; s_ink_keep.r[i].y = (int16_t)r.y;
    s_ink_keep.r[i].w = (int16_t)r.w; s_ink_keep.r[i].h = (int16_t)r.h;
    s_ink_keep.kind[i] = (uint8_t)kind;
}
void ls_ink_keep(tui_rect r) { ls_ink_mark(r, LS_INK_K_GLASS); }
ls_ink_keep_t *ls_ink_kept(void) { return &s_ink_keep; }
/* What is left of x0..x1, y0..y1 outside h, as up to four rects. */
static void unkeep_piece(int x0, int y0, int x1, int y1, int k, tui_rect h)
{
    if (h.x >= x1 || h.y >= y1 || h.x + h.w <= x0 || h.y + h.h <= y0) {
        ls_ink_mark(tui_rect_make(x0, y0, x1 - x0, y1 - y0), k);
        return;
    }
    const int cx0 = h.x > x0 ? h.x : x0, cy0 = h.y > y0 ? h.y : y0;
    const int cx1 = h.x + h.w < x1 ? h.x + h.w : x1, cy1 = h.y + h.h < y1 ? h.y + h.h : y1;
    if (cy0 > y0) ls_ink_mark(tui_rect_make(x0, y0, x1 - x0, cy0 - y0), k);
    if (y1 > cy1) ls_ink_mark(tui_rect_make(x0, cy1, x1 - x0, y1 - cy1), k);
    if (cx0 > x0) ls_ink_mark(tui_rect_make(x0, cy0, cx0 - x0, cy1 - cy0), k);
    if (x1 > cx1) ls_ink_mark(tui_rect_make(cx1, cy0, x1 - cx1, cy1 - cy0), k);
}
/* A popup over a scope is not scope: its rect is cut out of what the frame
   keeps black (and of every other ground it named), each rect it crosses
   becoming the strips around it. In place: no second list on the stack. */
void ls_ink_unkeep(tui_rect h)
{
    if (!ls_ink_on || h.w < 1 || h.h < 1 || !s_ink_keep.n) return;
    const int was = s_ink_keep.n;
    int keep = 0;
    int16_t cut[LS_INK_KEEP_MAX][5];     /* the rects crossed, cut later */
    int ncut = 0;
    for (int i = 0; i < was; i++) {
        const int x0 = s_ink_keep.r[i].x, y0 = s_ink_keep.r[i].y;
        const int x1 = x0 + s_ink_keep.r[i].w, y1 = y0 + s_ink_keep.r[i].h;
        if (h.x >= x1 || h.y >= y1 || h.x + h.w <= x0 || h.y + h.h <= y0) {
            s_ink_keep.r[keep] = s_ink_keep.r[i];
            s_ink_keep.kind[keep++] = s_ink_keep.kind[i];
            continue;
        }
        cut[ncut][0] = (int16_t)x0; cut[ncut][1] = (int16_t)y0; cut[ncut][2] = (int16_t)x1; cut[ncut][3] = (int16_t)y1;
        cut[ncut++][4] = s_ink_keep.kind[i];
    }
    s_ink_keep.n = (uint8_t)keep;
    for (int i = 0; i < ncut; i++) {
        const int x0 = cut[i][0], y0 = cut[i][1], x1 = cut[i][2], y1 = cut[i][3], kind = cut[i][4];
        if (kind != LS_INK_K_RING) { unkeep_piece(x0, y0, x1, y1, kind, h); continue; }
        /* A ring cut by a popup is its four sides, each cut. */
        unkeep_piece(x0, y0, x1, y0 + 1, LS_INK_K_FILL, h);
        if (y1 - 1 > y0) unkeep_piece(x0, y1 - 1, x1, y1, LS_INK_K_FILL, h);
        if (y1 - 1 > y0 + 1) {
            unkeep_piece(x0, y0 + 1, x0 + 1, y1 - 1, LS_INK_K_FILL, h);
            if (x1 - 1 > x0) unkeep_piece(x1 - 1, y0 + 1, x1, y1 - 1, LS_INK_K_FILL, h);
        }
    }
}

#else
void ls_tui_set_compact(bool on) { (void)on; }
#endif

/* How much of a key's lettering survives a short row. 0 everything; 1 the
   label to eight and the value to six; 2 the label to five and the value to
   four. A key is never cut to nothing: the hint row names it anyway. */
static void key1_text(const ls_btn_t *b, int fit, char *lab, size_t lcap,
                      char *val, size_t vcap)
{
    static const int LAB[3] = {24, 8, 5}, VAL[3] = {16, 6, 4};
    if (fit < 0) fit = 0;
    if (fit > 2) fit = 2;
    fit_label(lab, lcap, b->label ? b->label : "", LAB[fit]);
    val[0] = 0;
    if (b->value && b->value[0]) fit_label(val, vcap, b->value, VAL[fit]);
}

static int key1_width_fit(const ls_btn_t *b, int fit)
{
    char lab[32], val[24];
    key1_text(b, fit, lab, sizeof(lab), val, sizeof(val));
    const int l = (int)strlen(lab), v = (int)strlen(val);
    return 4 + l + (v ? v + 1 : 0);
}

int ls_key1_width(const ls_btn_t *b) { return b ? key1_width_fit(b, 0) : 0; }

int ls_btn_bar_width(const ls_btn_t *btn, int n)
{
    /* One translated key at a time: no array copy on the UI stack. */
    int w = 0;
    for (int i = 0; btn && i < n; i++) {
        ls_btn_t one = btn[i];
        one.label = ls_tr(btn[i].label);
        one.value = ls_tr(btn[i].value);
        w += key1_width_fit(&one, 0) + (i ? 1 : 0);
    }
    return w;
}

/* Each key names its own bevel rect, unless a bar named the whole row. */
static bool s_key1_mark = true;

/* The cap colours: KEY (black on white) at rest, green when engaged, the
   selection blue when focused or just changed, and no cap at all when the
   control is unavailable - its word stays readable in the one grey that may
   carry words. */
static int key1_paint(tui_surface *sf, tui_rect clip, int x, int y,
                      const ls_btn_t *b, int fit, bool focus, bool changed)
{
    char lab[32], val[24];
    key1_text(b, fit, lab, sizeof(lab), val, sizeof(val));
    uint8_t cap_bg, ink, vink, accel;
    if (b->dim)                { cap_bg = TUI_BLACK; ink = LS_DIM_FG; vink = LS_DIM_FG; accel = LS_DIM_FG; }
    else if (focus || changed) { cap_bg = TUI_BLUE; ink = TUI_WHITE | TUI_BRIGHT; vink = TUI_YELLOW | TUI_BRIGHT; accel = TUI_YELLOW | TUI_BRIGHT; }
    else if (b->on)            { cap_bg = TUI_GREEN; ink = TUI_BLACK; vink = TUI_WHITE | TUI_BRIGHT; accel = TUI_RED; }
    else                       { cap_bg = TUI_WHITE; ink = TUI_BLACK; vink = TUI_BLUE; accel = TUI_RED; }
    if (ls_ink_on && !b->dim) {
        /* In a desk window: the scheme's Selected Items, title and Button colours. */
        const bool lit = focus || changed;
        const uint8_t a = ls_ink(lit ? LS_INK_SELECT : b->on ? LS_INK_KEY_ON : LS_INK_KEY, 0);
        cap_bg = TUI_ATTR_BG(a);
        ink = vink = TUI_ATTR_FG(a);
        accel = lit || b->on || cap_bg != TUI_BLACK ? ink : TUI_ATTR_FG(ls_ink(LS_INK_ACCEL, 0));
    }
    const uint8_t cap = TUI_ATTR(ink, cap_bg);
    /* In a desk window the cap is a toolbar button: a light line at its
       left end and a dark one at its right (the reverse while latched on,
       pressed in), the brackets' cells, so it is just as wide. */
    const bool desk = ls_dui_desk();
    int cx = x;
    if (desk) cx++;
    else tui_put_char(sf, clip, cx++, y, '[', TUI_ATTR(b->dim ? LS_FAINT_FG : (TUI_WHITE | TUI_BRIGHT), TUI_BLACK));
    tui_put_char(sf, clip, cx++, y, ' ', cap);
    /* The shortcut, picked out in the label the way an accelerator was. */
    int hot = -1;
    if (b->key && !b->dim) {
        const char k = (char)(b->key >= 'a' && b->key <= 'z' ? b->key - 32 : b->key);
        for (int i = 0; lab[i]; i++) {
            const char c = (char)(lab[i] >= 'a' && lab[i] <= 'z' ? lab[i] - 32 : lab[i]);
            if (c == k) { hot = i; break; }
        }
    }
    for (int i = 0; lab[i]; i++)
        tui_put_char(sf, clip, cx++, y, lab[i], i == hot ? TUI_ATTR(accel, cap_bg) : cap);
    if (val[0]) {
        tui_put_char(sf, clip, cx++, y, ' ', cap);
        for (int i = 0; val[i]; i++) tui_put_char(sf, clip, cx++, y, val[i], TUI_ATTR(vink, cap_bg));
    }
    /* Something new inside: the drip takes the cap's closing space. */
    tui_put_char(sf, clip, cx++, y, b->badge ? drip_glyph() : ' ',
                 b->badge ? TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, cap_bg) : cap);
    if (desk) {
        cx++;
        ls_box_put(sf, clip, x, y, LS_EDGE_L, ls_ink(b->on && !b->dim ? LS_INK_SHADOW : LS_INK_HILITE, 0));
        ls_box_put(sf, clip, cx - 1, y, LS_EDGE_R, ls_ink(b->on && !b->dim ? LS_INK_HILITE : LS_INK_SHADOW, 0));
        if (s_key1_mark) ls_ink_mark(tui_rect_make(x, y, cx - x, 1), LS_INK_K_FILL);
    } else tui_put_char(sf, clip, cx++, y, ']', TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
    return cx - x;
}

int ls_key1_draw(tui_surface *sf, tui_rect clip, int x, int y,
                 const ls_btn_t *b, bool focus)
{
    if (!sf || !b) return 0;
    ls_btn_t one = *b;
    one.label = ls_tr(b->label);
    one.value = ls_tr(b->value);
    return key1_paint(sf, clip, x, y, &one, 0, focus, false);
}

/* Rows a compact bar needs at a fit level, keys flowing left to right. */
static int key1_rows(const ls_btn_t *btn, int n, int w, int fit)
{
    int rows = 1, x = 0;
    for (int i = 0; i < n; i++) {
        const int kw = key1_width_fit(&btn[i], fit);
        if (x > 0 && x + kw > w) { rows++; x = 0; }
        x += kw + 1;
    }
    return rows;
}

/* The desk form of every button bar: one-row keys from the bar's left edge,
   wrapping onto the bar's next row when there is one, shortening their
   lettering when there is not. Hits are the keys as drawn. */
static void compact_bar(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                        int focus, int slot)
{
    int fit = 0;
    while (fit < 2 && key1_rows(btn, n, bar.w, fit) > bar.h) fit++;
    int x = bar.x, y = bar.y;
    /* One bevel rect for the bar's rows, not one a key. */
    if (ls_dui_desk()) ls_ink_mark(tui_rect_make(bar.x, bar.y, bar.w, key1_rows(btn, n, bar.w, fit) < bar.h ? key1_rows(btn, n, bar.w, fit) : bar.h), LS_INK_K_FILL);
    s_key1_mark = false;
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < n && s_btn_n[slot] < MAX_HITS; i++) {
        const int kw = key1_width_fit(&btn[i], fit);
        if (x > bar.x && x + kw > bar.x + bar.w) { y++; x = bar.x; }
        if (y >= bar.y + bar.h) break;
        uint32_t token = 2166136261u;
        const char *parts[] = {btn[i].label, btn[i].value};
        for (int p = 0; p < 2; p++) if (parts[p])
            for (const char *c = parts[p]; *c; c++) token = (token ^ (unsigned char)*c) * 16777619u;
        token ^= (btn[i].on ? 1u : 0u) | (btn[i].dim ? 2u : 0u);
        const bool changed = btn_changed(slot, i, token, bar, now);
        const int used = key1_paint(sf, bar, x, y, &btn[i], fit, i == focus, changed);
        const int k = s_btn_n[slot];
        s_btn_enabled[slot][k] = !btn[i].dim;
        s_btn_keys[slot][k] = btn[i].key;
        s_btn_hit[slot][k] = (hit_t){(int16_t)x, (int16_t)y, (int16_t)(x + used - 1), (int16_t)y};
        s_btn_n[slot]++;
        x += used + 1;
    }
    s_key1_mark = true;
}

void ls_field1_draw(tui_surface *sf, tui_rect clip, int x, int y, int w,
                    const char *text, bool caret, uint8_t ink)
{
    if (!sf || w < 3) return;
    const uint8_t well = TUI_ATTR(ink, TUI_BLACK);
    /* A desk window: a sunken edit on the contrast ground. */
    if (ls_dui_desk()) ls_dui_field(sf, clip, x, y, w);
    else {
        tui_put_char(sf, clip, x, y, '[', TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
        tui_put_char(sf, clip, x + w - 1, y, ']', TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
    const int room = w - 2;
    const char *t = text ? text : "";
    int n = (int)strlen(t);
    /* The tail is what is being typed: a long entry scrolls left. */
    const int keep = caret ? room - 1 : room;
    if (n > keep && keep > 0) { t += n - keep; n = keep; }
    for (int i = 0; i < room; i++)
        tui_put_char(sf, clip, x + 1 + i, y, i < n ? t[i] : ' ', well);
    /* The caret is an inverse cell, steady: nothing blinks on a timer. */
    if (caret && n < room)
        tui_put_char(sf, clip, x + 1 + n, y, ' ', ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, ink & 0x07)));
}

void ls_dialog1_frame(tui_surface *sf, tui_rect r, const char *title, uint8_t hue)
{
    ls_ink_unkeep(r);                    /* over a scope a dialog is still the scheme's */
    if (!sf || r.w < 4 || r.h < 3) return;
    if (ls_dui_desk()) {
        /* A desk dialog: the face inside a raised frame, its title in the
           active title bar's colours across the top row. */
        tui_fill(sf, r, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
        const uint8_t hi = ls_ink(LS_INK_HILITE, 0), lo = ls_ink(LS_INK_SHADOW, 0);
        const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
        for (int y = r.y + 1; y < y1; y++) {
            ls_box_put(sf, r, r.x, y, LS_BOX_V, hi);
            ls_box_put(sf, r, x1, y, LS_BOX_V, lo);
        }
        for (int x = r.x + 1; x < x1; x++) ls_box_put(sf, r, x, y1, LS_BOX_H, lo);
        ls_box_put(sf, r, r.x, y1, LS_BOX_UR, hi);
        ls_box_put(sf, r, x1, y1, LS_BOX_UL, lo);
        const uint8_t bar = ls_ink(LS_INK_TITLE, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLUE));
        tui_fill(sf, tui_rect_make(r.x, r.y, r.w, 1), ' ', bar);
        if (title && title[0]) {
            char t[64];
            snprintf(t, sizeof(t), "%s", ls_tr(title));
            if ((int)strlen(t) > r.w - 3) t[r.w - 3 > 0 ? r.w - 3 : 0] = 0;
            tui_put_str(sf, r, r.x + 1, r.y, t, bar);
        }
        ls_ink_bevel(r);
        return;
    }
    const uint8_t hi = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t lo = TUI_ATTR(LS_FAINT_FG, TUI_BLACK);
    tui_fill(sf, r, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (int x = r.x + 1; x < x1; x++) {
        ls_box_put(sf, r, x, r.y, LS_BOX_H, hi);
        ls_box_put(sf, r, x, y1, LS_BOX_H, lo);
    }
    for (int y = r.y + 1; y < y1; y++) {
        ls_box_put(sf, r, r.x, y, LS_BOX_V, hi);
        ls_box_put(sf, r, x1, y, LS_BOX_V, lo);
    }
    ls_box_put(sf, r, r.x, r.y, LS_BOX_DR, hi);
    ls_box_put(sf, r, x1, r.y, LS_BOX_DL, hi);
    ls_box_put(sf, r, r.x, y1, LS_BOX_UR, hi);
    ls_box_put(sf, r, x1, y1, LS_BOX_UL, lo);
    if (title && title[0]) {
        char t[48];
        snprintf(t, sizeof(t), " %s ", ls_tr(title));
        if ((int)strlen(t) > r.w - 4) t[r.w - 4 > 0 ? r.w - 4 : 0] = 0;
        tui_put_str(sf, r, r.x + 2, r.y, t, TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
    }
}

void ls_slider1_draw(tui_surface *sf, tui_rect r, const char *label,
                     const char *value, float frac, uint8_t hue, bool live,
                     tui_rect *down, tui_rect *up, tui_rect *value_rect,
                     tui_rect *bar)
{
    const tui_rect none = tui_rect_make(0, -1, 0, 0);
    if (down) *down = none;
    if (up) *up = none;
    if (value_rect) *value_rect = none;
    if (bar) *bar = none;
    if (!sf || r.w < 16 || r.h < 1) return;
    const int y = r.y;
    const uint8_t lab = live ? TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM;
    label = ls_tr(label ? label : "");
    int x = r.x;
    tui_put_str(sf, r, x, y, label, lab);
    x += (int)strlen(label) + 1;
    char v[20];
    snprintf(v, sizeof(v), "[%s]", value ? value : "");
    const int vw = (int)strlen(v);
    const bool desk = ls_dui_desk();
    if (desk) {
        /* A desk window: the value in a sunken field, the keys raised caps,
           the travel a groove with its lit part heavy and a thumb. */
        const tui_rect in = ls_dui_field(sf, r, x, y, vw);
        tui_put_str(sf, tui_rect_intersect(r, in), in.x, y, value ? value : "",
                    live ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    } else
    tui_put_str(sf, r, x, y, v, live ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    if (value_rect) *value_rect = tui_rect_make(x, y, vw, 1);
    x += vw + 1;
    /* The keys at the right end, then the travel between. */
    const int right = r.x + r.w;
    const tui_rect kd = tui_rect_make(right - 6, y, 3, 1), ku = tui_rect_make(right - 3, y, 3, 1);
    const uint8_t kcap = live ? ls_ink(LS_INK_KEY, TUI_ATTR(TUI_BLACK, TUI_WHITE)) : LS_ATTR_DIM;
    for (int k = 0; k < 2; k++) {
        const tui_rect key = k ? ku : kd;
        if (desk) {
            tui_put_char(sf, r, key.x + 1, y, k ? '+' : '-', kcap);
            ls_dui_cap_ends(sf, r, key.x, y, 3, false);
            continue;
        }
        tui_put_char(sf, r, key.x, y, '[', TUI_ATTR(live ? (TUI_WHITE | TUI_BRIGHT) : LS_FAINT_FG, TUI_BLACK));
        tui_put_char(sf, r, key.x + 1, y, k ? '+' : '-', kcap);
        tui_put_char(sf, r, key.x + 2, y, ']', TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
    }
    if (live) {
        if (down) *down = kd;
        if (up) *up = ku;
    }
    const int tw = kd.x - 1 - x;
    if (tw < 6) return;
    /* |----=====#------| : the travel in the shadow grey, the part that is
       on in the control's own colour, one solid cell for the thumb. */
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int inner = tw - 2;
    const int pos = (int)(frac * (float)(inner - 1) + 0.5f);
    const uint8_t track = TUI_ATTR(LS_FAINT_FG, TUI_BLACK);
    const uint8_t lit = TUI_ATTR(live ? (hue | TUI_BRIGHT) : LS_DIM_FG, TUI_BLACK);
    if (desk) {
        const uint8_t groove = ls_ink(LS_INK_RULE, track);
        tui_put_char(sf, r, x, y, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
        for (int i = 0; i < inner; i++) {
            if (live && i == pos) tui_put_char(sf, r, x + 1 + i, y, ' ', ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, TUI_WHITE)));
            else ls_box_put(sf, r, x + 1 + i, y, i < pos ? 0x2501 : LS_BOX_H, i < pos ? lit : groove);
        }
        tui_put_char(sf, r, x + tw - 1, y, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
        if (bar && live) *bar = tui_rect_make(x + 1, y, inner, 1);
        return;
    }
    ls_box_put(sf, r, x, y, LS_BOX_VR, track);
    for (int i = 0; i < inner; i++) {
        if (live && i == pos) tui_put_char(sf, r, x + 1 + i, y, LS_TUI_BLOCK_FULL, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        else if (i < pos) ls_box_put(sf, r, x + 1 + i, y, LS_BOX_DBL_H, lit);
        else ls_box_put(sf, r, x + 1 + i, y, LS_BOX_H, track);
    }
    ls_box_put(sf, r, x + tw - 1, y, LS_BOX_VL, track);
    if (bar && live) *bar = tui_rect_make(x + 1, y, inner, 1);
}

static int s_force_rows;
/* A key grid lays its rows out full width and fits the corners after (grid_fit_corners). */
static bool s_grid_fit;
static const uint8_t *s_btn_hues;
static void button_bar(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                        int focus, int slot, bool raised, bool stretch, bool single_row)
{
    ls_btn_t tr_copy[TR_BTN_MAX];
    btn = tr_buttons(btn, n, tr_copy);
    if (slot < 0 || slot >= BTN_SLOTS) slot = 0;
    hit_clear(s_btn_hit[slot], &s_btn_n[slot]);
    if (!sf || n <= 0 || bar.w < 6 || bar.h < 1) return;
    if (ls_tui_compact()) { compact_bar(sf, bar, btn, n, focus, slot); return; }
    const bool key_hints = ls_tui_keyboard_mode();

    /* Rows follow the height the caller gave us, columns follow from that.
       Portrait passes two or four rows and gets fat targets without this
       file knowing which orientation it is in. */
    int per_row = n;
    int corner = ls_tui_corner_pad(bar.y);
    int bottom_corner = ls_tui_corner_pad(bar.y + bar.h - 1);
    if (bottom_corner > corner) corner = bottom_corner;
    if (corner > 0 && !stretch) {
        int right = bar.x + bar.w;
        int cols = tui_surface_rect(sf).w;
        if (bar.x < corner) bar.x = corner;
        if (right > cols - corner) right = cols - corner;
        bar.w = right - bar.x;
        if (bar.w < 3) return;
    }
    int rows = 1;
    while (!single_row && bar.h >= rows * 2 && bar.w / per_row < (raised ? 10 : 8) && per_row > 1) {
        rows++;
        per_row = (n + rows - 1) / rows;
    }
    if (s_force_rows > 0) rows = s_force_rows;
    if (rows > bar.h) rows = bar.h;
    if (rows < 1) rows = 1;
    per_row = (n + rows - 1) / rows;
    if (per_row < 1) per_row = 1;

    /* A button is as tall as it needs to be, not as tall as the rect. */

    int row_h = bar.h / rows;
    const int max_h = raised ? 7 : 5;
    if (row_h > max_h) row_h = max_h;
    int cell_w = bar.w / per_row;
    if (cell_w > BTN_MAX_W) cell_w = BTN_MAX_W;
    if (cell_w < 3 || row_h < 1) return;

    for (int i = 0; i < n && i < MAX_HITS; i++) {
        const int r = i / per_row, c = i % per_row;
        if (r >= rows) break;

        const int count = n - r * per_row < per_row ? n - r * per_row : per_row;
        /* Full-width transport rows respect their own rounded corners.
           The bottom row must not shrink every row above it. */
        int row_x=bar.x,row_w=bar.w;
        if(stretch) {
            int pad=s_grid_fit?0:ls_tui_corner_pad(bar.y+r*row_h);
            int end_pad=s_grid_fit?0:ls_tui_corner_pad(bar.y+(r+1)*row_h-1);
            if(end_pad>pad)pad=end_pad;
            int right=bar.x+bar.w,cols=tui_surface_rect(sf).w;
            if(row_x<pad)row_x=pad;
            if(right>cols-pad)right=cols-pad;
            row_w=right-row_x;
            cell_w=row_w/count;
            if(cell_w<3)continue;
        }
        /* Cells are even unless a word needs more than its share: then the
           buttons with room to spare lend columns to the ones without. */
        int cw[MAX_HITS];
        const int first = r * per_row;
        for (int k = 0; k < count && k < MAX_HITS; k++) cw[k] = cell_w;
        /* A button is capped at BTN_MAX_W, which can leave the row short of
           the bar: that slack goes to the words first. */
        int free_cols = row_w - count * cell_w;
        if (free_cols < 0) free_cols = 0;
        for (int pass = 0; pass < MAX_HITS * 4; pass++) {
            int needy = -1, donor = -1, best = 0;
            for (int k = 0; k < count && k < MAX_HITS; k++) {
                const int want = button_want(&btn[first + k], row_h);
                if (want > cw[k] && needy < 0) needy = k;
                const int spare = cw[k] - (want > 6 ? want : 6);
                if (spare > best) { best = spare; donor = k; }
            }
            if (needy < 0) break;
            if (free_cols > 0) { free_cols--; cw[needy]++; continue; }
            if (donor < 0 || donor == needy) break;
            cw[donor]--; cw[needy]++;
        }
        int xoff = 0, used = 0;
        for (int k = 0; k < count && k < MAX_HITS; k++) { if (k < c) xoff += cw[k]; used += cw[k]; }
        const int x0 = row_x + (row_w - (used - 1)) / 2 + xoff;
        const int y0 = bar.y + r * row_h;
        const int w = c < MAX_HITS ? cw[c] : cell_w;
        const int h = row_h;

        /* A dithered field and a border, not a slab of colour. */

        const bool sel = (i == focus);
        uint32_t token = 2166136261u;
        const char *parts[] = {btn[i].label, btn[i].value};
        for (int p = 0; p < 2; p++) if (parts[p])
            for (const char *c = parts[p]; *c; c++) token = (token ^ (unsigned char)*c) * 16777619u;
        token ^= (btn[i].on ? 1u : 0u) | (btn[i].dim ? 2u : 0u);
        int64_t now = esp_timer_get_time();
        bool changed = btn_changed(slot, i, token, bar, now);
        uint8_t hue;
        int density;
        /* A key with a colour of its own (HOME's groups) fills in it: bright
           when it is the open one, its darker shade when not. */
        const uint8_t own = s_btn_hues ? s_btn_hues[i] & 7 : 0;
        const bool lit = sel || changed || btn[i].on;
        if (btn[i].dim)      { hue = TUI_BLACK | TUI_BRIGHT; density = LS_DITHER_LIGHT; }
        else if (own)        { hue = lit ? own | TUI_BRIGHT : own; density = lit ? LS_DITHER_HEAVY : LS_DITHER_LIGHT; }
        else if (sel || changed) { hue = TUI_CYAN | TUI_BRIGHT; density = LS_DITHER_HEAVY; }
        else if (btn[i].on)  { hue = TUI_GREEN | TUI_BRIGHT; density = LS_DITHER_HEAVY; }
        /* Cyan, not blue. Blue was the last of the old palette left
           on a control, and it put a navy field under every page button
           while the panel around it was cyan - which is the same "belongs to
           a different program" the navigation strip had. */
        else                 { hue = TUI_CYAN;                density = LS_DITHER_LIGHT; }

        /* Text on black, so the glyph is not fighting the texture. The one
           exception is a dim button, whose whole point is to recede. */
        /* An unavailable control still has to say what it is - a
           label you cannot read is not a disabled button, it is a blank. */
        /* A coloured key's words sit on its colour: dark on the bright
           face, light on the dark one. */
        const uint8_t face = own && !btn[i].dim
                           ? (lit ? TUI_ATTR(TUI_BLACK, hue) : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, hue))
                           : TUI_ATTR(btn[i].dim ? LS_DIM_FG : (TUI_WHITE | TUI_BRIGHT), TUI_BLACK);

        tui_rect box = tui_rect_make(x0, y0, w - 1, h);
        ls_fill_dither(sf, box, density, hue);
        const bool tall = raised && h >= 5 && box.w >= 5;

        /* A border when there is height for one. Two rows is a strip and
           gets none; three or more is a key and looks like one. */
        if (h >= 3) {
            const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
            tui_hline(sf, box, box.x, box.y, box.w, tall ? '=' : '-', edge);
            tui_hline(sf, box, box.x, box.y + h - 1, box.w, tall ? '=' : '-', edge);
            tui_vline(sf, box, box.x, box.y + 1, h - 2, '|', edge);
            tui_vline(sf, box, box.x + box.w - 1, box.y + 1, h - 2, '|', edge);
            tui_put_char(sf, box, box.x, box.y, '+', edge);
            tui_put_char(sf, box, box.x + box.w - 1, box.y, '+', edge);
            tui_put_char(sf, box, box.x, box.y + h - 1, '+', edge);
            tui_put_char(sf, box, box.x + box.w - 1, box.y + h - 1, '+', edge);
        }

        const char *lab = btn[i].label ? btn[i].label : "";
        char compact[48];
        /* Three rows is a frame plus one usable line, so a label and a value
           cannot both have their own row - they go on one line or they land
           on the border. The raised bar already knew this; the plain one did
           not, and wrote its label over the top edge. */
        const bool compact_value = h == 3 && btn[i].value;
        if (compact_value) { snprintf(compact,sizeof(compact),"%s %s",lab,btn[i].value); lab=compact; }
        const int text_w = h >= 3 ? box.w - 2 : box.w;
        char cut[48];
        if (compact_value && (int)strlen(lab) > text_w) {
            /* "LABEL VALUE" on one line: the value gives way first, down to
               three columns, then the label. */
            const int ll = (int)strlen(btn[i].label ? btn[i].label : "");
            const int vl = (int)strlen(btn[i].value);
            int vw = text_w - ll - 1;
            if (vw < 3) vw = vl < 3 ? vl : 3;
            int lwid = text_w - 1 - vw;
            char a[24], b[24];
            fit_label(a, sizeof(a), btn[i].label, lwid);
            fit_label(b, sizeof(b), btn[i].value, vw);
            snprintf(cut, sizeof(cut), "%s %s", a, b);
        } else fit_label(cut, sizeof(cut), lab, text_w);
        int lw = (int)strlen(cut);
        if (lw > text_w) { cut[text_w] = 0; lw = text_w; }

        const int lines = (h > 1 && btn[i].value && !compact_value) ? 2 : 1;
        /* Centre inside the frame, not inside the box. Rows 0 and h-1 belong
           to the border when there is one, so counting them pushed the
           lettering a row high - onto the top edge for the tightest boxes. */
        const int inner_y = h >= 3 ? box.y + 1 : box.y;
        const int inner_h = h >= 3 ? h - 2 : h;
        const int ly = inner_y + (inner_h - lines) / 2;
        const int lx = box.x + (box.w - lw) / 2;
        /* A space either side: the field under the lettering is texture, and
           a word butting into it loses its first and last letter. */
        if (lx - 1 > box.x) tui_put_char(sf, box, lx - 1, ly, ' ', face);
        if (lx + lw < box.x + box.w - 1)
            tui_put_char(sf, box, lx + lw, ly, ' ', face);
        tui_put_str(sf, box, lx, ly, cut, face);

        if (lines == 2) {
            char vc[48];
            const bool hard = fit_label(vc, sizeof(vc), btn[i].value, text_w);
            int vw = (int)strlen(vc);
            if (tall && hard && vw > 0) vc[vw - 1] = '>';
            const uint8_t vattr = TUI_ATTR(btn[i].dim ? LS_DIM_FG
                                                      : (TUI_YELLOW | TUI_BRIGHT),
                                           TUI_BLACK);
            const int vx = box.x + (box.w - vw) / 2;
            if (vx - 1 > box.x) tui_put_char(sf, box, vx - 1, ly + 1, ' ', vattr);
            if (vx + vw < box.x + box.w - 1)
                tui_put_char(sf, box, vx + vw, ly + 1, ' ', vattr);
            tui_put_str(sf, box, vx, ly + 1, vc, vattr);
        }

        if (tall) {
            const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
            if (key_hints && btn[i].key) {
                char badge[] = {'[', btn[i].key, ']', 0};
                tui_put_str(sf, box, box.x + (box.w - 3) / 2, box.y, badge, face);
            }
            if (!btn[i].dim && (btn[i].on || sel)) {
                tui_put_str(sf, box, box.x + (box.w - 3) / 2, box.y + h - 1,
                            ls_tr(btn[i].on ? "[*]" : "[+]"), edge);
            }
        } else if (key_hints && btn[i].key && h >= 3 && box.w >= 5) {
            char badge[] = {'[', btn[i].key, ']', 0};
            tui_put_str(sf, box, box.x + box.w - 4, box.y, badge, face);
        } else if (key_hints && btn[i].key && box.w >= lw + 2) {
            tui_put_char(sf, box, box.x + box.w - 1, ly, btn[i].key, face);
        }

        if (sel) tui_put_char(sf,box,box.x,box.y+h/2,'>',face);
        /* Something new inside: a magenta drip a cell clear of the label's
           right end, on the label's own row. The label does not move; with no
           room beside it nothing is drawn. */
        if (btn[i].badge) {
            const int dx = lx + lw + 1;
            if (dx <= box.x + box.w - 2) {
                tui_put_char(sf, box, dx, ly, drip_glyph(), TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
                if (dx + 1 <= box.x + box.w - 2) tui_put_char(sf, box, dx + 1, ly, ' ', face);
            }
        }
        s_btn_enabled[slot][s_btn_n[slot]] = !btn[i].dim;
        s_btn_keys[slot][s_btn_n[slot]] = btn[i].key;
        hit_t *hit = &s_btn_hit[slot][s_btn_n[slot]];
        hit->x0 = (int16_t)box.x;
        hit->y0 = (int16_t)box.y;
        hit->x1 = (int16_t)(box.x + box.w - 1);
        hit->y1 = (int16_t)(box.y + h - 1);
        s_btn_n[slot]++;
    }
}

void ls_btn_bar_slot(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                     int focus, int slot)
{
    button_bar(sf, bar, btn, n, focus, slot, false, false, false);
}

/* Whether a three-row bar can put every label and its value on one line.

   Three rows is a frame plus one usable line, so a button carrying a value
   writes "LABEL VALUE" into box.w-2 columns rather than giving the value a
   row of its own. That compact form is what gives a landscape pane its
   height back - and what quietly cuts a word in half when the pane is not
   wide enough for it. SUB-GHZ on a half-width landscape split is the case:
   five buttons in fifty-six columns leaves eight for the text, and SETUP
   CAPTURE is thirteen.

   So the caller asks instead of assuming, and takes the four-row form when
   the answer is no. The arithmetic below is button_bar's own, for the
   single-row case a three-row bar always is. */
bool ls_btn_compact_fits(tui_rect bar, const ls_btn_t *btn, int n)
{
    if (!btn || n < 1 || bar.w < 6) return false;
    if (ls_tui_compact()) return true;     /* one-row keys shorten, never split */
    ls_btn_t tr_copy[TR_BTN_MAX];
    btn = tr_buttons(btn, n, tr_copy);
    /* The corners take their cells off both ends before anything is laid
       out, exactly as button_bar does it. */
    const int corner = ls_tui_corner_pad(bar.y);
    const int w = bar.w - 2 * corner;
    if (w < 6) return false;
    /* Words borrow columns from buttons with room to spare, so the question
       is whether the row as a whole has them: each button wants its lettering
       plus a frame and a gap, and never gives up below six columns. */
    int total = 0;
    for (int i = 0; i < n; i++) {
        const char *lab = btn[i].label ? btn[i].label : "";
        int want = (int)strlen(lab);
        if (btn[i].value) want += 1 + (int)strlen(btn[i].value);
        want += 3;
        total += want > 6 ? want : 6;
    }
    if (total > w) return false;
    return true;
}

int ls_btn_raised_height(tui_rect area, int n)
{
    if (n < 1 || area.w < 1 || area.h < 1) return 0;
    /* One-row keys average about twelve columns with their gap. */
    if (ls_tui_compact()) return area.w >= n * 12 || area.h < 8 ? 1 : 2;
    int columns = area.w / 10;
    if (columns < 1) columns = 1;
    const int rows = (n + columns - 1) / columns;
    /* Landscape without a keyboard is driven by thumbs alone, so its keys
       get the row a keyboard user does not need. */
    const bool wide = area.w > area.h * 2;
    int height = wide ? (!ls_tui_keyboard_mode() && area.h >= 20 ? 4 : 3)
                      : area.h >= 48 ? 7 : 5;
    if (height * rows > area.h / 3) height = area.h / (3 * rows);
    if (height < 1) height = 1;
    return height * rows < area.h ? height * rows : area.h;
}

void ls_btn_bar_raised(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                       int focus)
{
    button_bar(sf, bar, btn, n, focus, LS_BTN_SLOT_SCREEN, true, false, false);
}

void ls_btn_bar_raised_hued(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                            int focus, const uint8_t *hues)
{
    s_btn_hues = hues;
    button_bar(sf, bar, btn, n, focus, LS_BTN_SLOT_SCREEN, true, false, false);
    s_btn_hues = NULL;
}

/* A fixed row for screens that budget labels and large touch targets together. */
void ls_btn_bar_raised_row(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus)
{
    button_bar(sf,bar,btn,n,focus,LS_BTN_SLOT_SCREEN,true,false,true);
}

void ls_btn_bar_rows_slot(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus,int slot,int rows)
{
    s_force_rows=rows;
    button_bar(sf,bar,btn,n,focus,slot,false,false,false);
    s_force_rows=0;
}

/* A key grid down into the panel's rounded corners: on each row the curve
   reaches, an outer key's side steps in by the cells the glass lacks there,
   and what lies under the curve is left empty. The hit rects stay whole (a
   finger on the curve has no glass to land on anyway). */
static void grid_fit_corners(tui_surface *sf, tui_rect bar, int slot)
{
    if (!sf || slot < 0 || slot >= BTN_SLOTS) return;
    const tui_cell blank = { ' ', TUI_ATTR(TUI_BLACK, TUI_BLACK) };
    for (int y = bar.y; y < bar.y + bar.h && y < sf->h; y++) {
        const int p = ls_tui_corner_pad(y);
        if (y < 0 || p <= 0) continue;
        tui_cell *row = &sf->back[(size_t)y * sf->w];
        for (int i = 0; i < s_btn_n[slot]; i++) {
            const hit_t *r = &s_btn_hit[slot][i];
            if (y < r->y0 || y > r->y1) continue;
            if (r->x0 < p && p < r->x1 - 2) {
                row[p] = row[r->x0 < 0 ? 0 : r->x0];
                for (int x = r->x0 < 0 ? 0 : r->x0; x < p; x++) row[x] = blank;
            }
            const int lim = sf->w - 1 - p;
            if (r->x1 > lim && lim > r->x0 + 2) {
                row[lim] = row[r->x1 < sf->w ? r->x1 : sf->w - 1];
                for (int x = lim + 1; x <= r->x1 && x < sf->w; x++) row[x] = blank;
            }
        }
    }
}

void ls_btn_bar_grid_slot(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus,int slot,int rows,
                          const uint8_t *hues)
{
    s_force_rows=rows;
    s_btn_hues=hues;
    s_grid_fit=true;
    button_bar(sf,bar,btn,n,focus,slot,true,true,false);
    s_grid_fit=false;
    s_btn_hues=NULL;
    s_force_rows=0;
    grid_fit_corners(sf,bar,slot);
}

void ls_btn_bar_transport(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus)
{
    button_bar(sf,bar,btn,n,focus,LS_BTN_SLOT_SCREEN,true,true,false);
}

void ls_btn_bar_transport_slot(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus,int slot)
{
    button_bar(sf,bar,btn,n,focus,slot,true,true,false);
}

void ls_btn_bar_raised_slot(tui_surface *sf, tui_rect bar, const ls_btn_t *btn,
                            int n, int focus, int slot)
{
    button_bar(sf, bar, btn, n, focus, slot, true, false, false);
}

void ls_btn_bar(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                int focus)
{
    ls_btn_bar_slot(sf, bar, btn, n, focus, LS_BTN_SLOT_SCREEN);
}

int ls_btn_hit_slot(int col, int row, int slot)
{
    if (slot < 0 || slot >= BTN_SLOTS) return -1;
    int i = hit_find(s_btn_hit[slot], s_btn_n[slot], col, row);
    if (i < 0 || !s_btn_enabled[slot][i]) return -1;
    s_btn_change[slot][i] = esp_timer_get_time();
    return i;
}

int ls_btn_count_slot(int slot)
{
    if (slot < 0 || slot >= BTN_SLOTS) return 0;
    return s_btn_n[slot];
}

bool ls_btn_rect_slot(int slot, int i, int *x, int *y, int *w, int *h)
{
    if (slot < 0 || slot >= BTN_SLOTS) return false;
    if (i < 0 || i >= s_btn_n[slot]) return false;
    const hit_t *r = &s_btn_hit[slot][i];
    if (x) *x = r->x0;
    if (y) *y = r->y0;
    if (w) *w = r->x1 - r->x0 + 1;
    if (h) *h = r->y1 - r->y0 + 1;
    return true;
}

int ls_btn_shortcut(char ch, int slot)
{
    if (!ch || slot < 0 || slot >= BTN_SLOTS) return -1;
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    for (int i = 0; i < s_btn_n[slot]; i++) {
        char key = s_btn_keys[slot][i];
        if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
        if (key == ch && s_btn_enabled[slot][i]) {
            s_btn_change[slot][i] = esp_timer_get_time();
            return i;
        }
    }
    return -1;
}

void ls_btn_clear_hits(void)
{
    memset(s_btn_n,0,sizeof(s_btn_n));
}

int ls_btn_hit(int col, int row)
{
    return ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
}

bool ls_btn_enabled(int slot, int focus)
{ return slot>=0 && slot<BTN_SLOTS && focus>=0 && focus<s_btn_n[slot] && s_btn_enabled[slot][focus]; }

bool ls_btn_navigate(ls_tk_t key, int *slot, int *focus, bool two_bars)
{
    if (!slot || !focus || (key!=LS_TK_LEFT && key!=LS_TK_RIGHT && key!=LS_TK_UP && key!=LS_TK_DOWN && key!=LS_TK_TAB)) return false;
    int slots=two_bars?2:1, best_slot=-1,best=-1,best_score=0x7fffffff;
    bool have=ls_btn_enabled(*slot,*focus);
    hit_t origin=have?s_btn_hit[*slot][*focus]:(hit_t){0};
    for(int s=0;s<slots;s++)for(int i=0;i<s_btn_n[s];i++) {
        if(!ls_btn_enabled(s,i))continue;
        if(!have){*slot=s;*focus=i;return true;}
        if(s==*slot && i==*focus)continue;
        hit_t h=s_btn_hit[s][i];
        int dx=h.x0+h.x1-origin.x0-origin.x1,dy=h.y0+h.y1-origin.y0-origin.y1;
        int along=(key==LS_TK_LEFT)?-dx:(key==LS_TK_UP)?-dy:(key==LS_TK_DOWN)?dy:dx;
        int cross=(key==LS_TK_UP || key==LS_TK_DOWN)?dx:dy;
        if(key==LS_TK_TAB) { along=(s-*slot)*MAX_HITS+i-*focus;cross=0; }
        if(along<=0)continue;
        if(cross<0)cross=-cross;
        int score=cross*1000+along;
        if(score<best_score){best_score=score;best_slot=s;best=i;}
    }
    if(best>=0){*slot=best_slot;*focus=best;}
    else if(key==LS_TK_TAB) {*focus=-1;return ls_btn_navigate(key,slot,focus,two_bars);}
    return true;
}

int ls_btn_key(char ch, const ls_btn_t *btn, int n)
{
    if (!ch || !btn) return -1;
    char lo = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    for (int i = 0; i < n; i++) {
        char k = btn[i].key;
        if (!k) continue;
        if (k >= 'A' && k <= 'Z') k = (char)(k + 32);
        if (k == lo) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------- tiles */

void ls_tile_grid(tui_surface *sf, tui_rect area, const ls_tile_t *tile,
                  int n, int sel)
{
    hit_clear(s_tile_hit, &s_tile_n);
    s_tile_cols = s_tile_rows = 0;
    if (!sf || n <= 0 || area.w < 12 || area.h < 4) return;

    /* A tile is 14 columns and 7 rows at its most comfortable - an
       icon, a name and a subtitle with air around them. Both are minimums,
       not literals: the count comes from dividing the rect, so a 48-column
       portrait grid gets three across and a 115-column landscape one gets
       eight, and neither is written down anywhere. */
    /* A tile is also as wide as its longest name needs: a name cut at
       thirteen characters is a name nobody can read back. */
    int longest = 0;
    for (int i = 0; i < n; i++) {
        const int len = tile[i].name ? (int)strlen(tile[i].name) : 0;
        if (len > longest) longest = len;
    }
    const int pitch = longest + 2 > 15 ? longest + 2 : 15;
    int cols = area.w / pitch;
    if (cols < 2) cols = 2;
    if (cols > 6) cols = 6;
    if (cols > n) cols = n;
    int rows = (n + cols - 1) / cols;

    /* A tile stops getting taller. */

#define TILE_MAX_H 16

    /* THE GRID IS CHOSEN SO THE ART FITS, not cropped to fit the grid. */

    {
        /* Two borders, the art, the name, and the row of air that separates
           one tile from the next. */
        const int want_h = 2 + LS_ICON_ROWS + 1 + 1;
        const int fits_rows = area.h / want_h;
        if (fits_rows >= 1 && rows > fits_rows) {
            const int want_cols = (n + fits_rows - 1) / fits_rows;
            /* The art has a width as well as a height, and a tile too narrow
               for it is the same fault turned ninety degrees. */
            /* The art outranks the name: a long name folds onto the
               subtitle row below, cropped art has nowhere to go. */
            if (want_cols <= MAX_HITS && area.w / want_cols >= LS_ICON_COLS + 3) {
                cols = want_cols;
                rows = fits_rows;
            }
        }
    }

    int tw = area.w / cols;
    int th = area.h / rows;
    if (th > TILE_MAX_H) th = TILE_MAX_H;
    if (th < 4) {

        rows = area.h / 4;
        if (rows < 1) rows = 1;
        cols = (n + rows - 1) / rows;
        if (cols < 1) cols = 1;
        tw = area.w / cols;
        th = area.h / rows;
    }
    if (tw < 8 || th < 3) return;

    /* Whatever the cap leaves over goes at BOTH ends. */

    const int pad_x = (area.w - cols * tw) / 2;
    const int pad_y = (area.h - rows * th) / 2;

    s_tile_cols = cols;
    s_tile_rows = rows;

    for (int i = 0; i < n && i < MAX_HITS; i++) {
        const int c = i % cols, r = i / cols;
        if (r >= rows) break;
        const int x0 = area.x + pad_x + c * tw;
        const int y0 = area.y + pad_y + r * th;
        const int w = tw - 1, h = th - 1;
        if (w < 6 || h < 3) continue;

        const bool sel_now = (i == sel);
        tui_rect box = tui_rect_make(x0, y0, w, h);

        uint8_t hue = tile[i].hue ? tile[i].hue : (uint8_t)(TUI_CYAN);
        uint8_t frame = TUI_ATTR(sel_now ? (hue | TUI_BRIGHT) : hue, TUI_BLACK);
        const uint8_t sel_at = ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, hue));   /* the chosen tile */
        const uint8_t sel_bg = TUI_ATTR_BG(sel_at);

        tui_fill(sf, box, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
        tui_box(sf, box, NULL, frame);
        if(ls_tui_keyboard_mode() && i<9) tui_put_char(sf,box,box.x+2,box.y,'1'+i,frame);

        if (sel_now)
            tui_fill(sf, tui_rect_make(box.x + 1, box.y + 1, box.w - 2, box.h - 2),
                     ' ', sel_at);

        const uint8_t on_tile = sel_now ? sel_at
                                        : TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
        const uint8_t name_at = sel_now ? sel_at
                                        : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        /* The subtitle under every tile on the directory - the
           single most-looked-at grey in the interface. */
        const uint8_t sub_at  = sel_now ? sel_at
                                        : LS_ATTR_DIM;

        /* The contents sit in the middle of the tile. */

        int text_h = (tile[i].name ? 1 : 0) + (tile[i].sub ? 1 : 0);
        const int interior = h - 2;              /* rows between the borders */
        if (interior < LS_ICON_ROWS + text_h && text_h > 1) text_h = 1;
        int icon_h = interior - text_h;
        if (icon_h < 0) icon_h = 0;
        if (icon_h > LS_ICON_ROWS) icon_h = LS_ICON_ROWS;

        int top = box.y + 1 + (interior - (icon_h + text_h)) / 2;
        if (top < box.y + 1) top = box.y + 1;

        int text_y = top;
        if (icon_h > 0) {
            const tui_rect icon_clip = tui_rect_make(box.x, top, box.w, icon_h);
            ls_icon_draw(sf, icon_clip, box.x + (box.w - LS_ICON_COLS) / 2,
                        top, tile[i].icon, on_tile);
            text_y = top + icon_h;
        }

        const char *tile_name = ls_tr(tile[i].name);
        const char *tile_sub = ls_tr(tile[i].sub);
        /* A name too long for the tile folds at its last space onto the
           subtitle's row, which it takes over: "AVIATION PULSES" reads as
           two words, not as "AVIATION PULS". */
        if (tile_name) {
            int nw = (int)strlen(tile_name);
            if (nw > box.w - 2) {
                int at = box.w - 2;
                while (at > 0 && tile_name[at] != ' ') at--;
                if (at > 0) { tile_sub = tile_name + at + 1; nw = at; }
                else nw = box.w - 2;
            }
            char cut[24];
            snprintf(cut, sizeof(cut), "%.*s", nw, tile_name);
            tui_put_str(sf, box, box.x + (box.w - nw) / 2, text_y, cut, name_at);
        }
        /* text_h == 1 means the subtitle was the thing dropped to make room
           - tile[i].sub is still non-NULL, but no row was budgeted for it,
           so drawing it anyway would land on whatever the icon or the
           border owns there. */
        if (tile_sub && text_h >= 2 && text_y + 1 < box.y + box.h) {
            int sw = (int)strlen(tile_sub);
            if (sw > box.w - 2) sw = box.w - 2;
            char cut[24];
            snprintf(cut, sizeof(cut), "%.*s", sw, tile_sub);
            tui_put_str(sf, box, box.x + (box.w - sw) / 2, text_y + 1, cut, sub_at);
        }

        /* A folder is a stack: the next card peeks out in the row of air
           below, and the count sits in the bottom border. The column of air
           beside a tile is one cell, too narrow for an edge of its own. */
        if (tile[i].more) {
            const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
            tui_hline(sf, area, box.x + 2, box.y + box.h, box.w - 4, '-', edge);
            tui_put_char(sf, area, box.x + 1, box.y + box.h, '\'', edge);
            tui_put_char(sf, area, box.x + box.w - 2, box.y + box.h, '\'', edge);
            char tag[8];
            snprintf(tag, sizeof(tag), "[+%u]", (unsigned)tile[i].more);
            if (box.w >= 10)
                tui_put_str(sf, box, box.x + box.w - 6, box.y + box.h - 1, tag,
                            TUI_ATTR(hue == TUI_YELLOW ? TUI_WHITE | TUI_BRIGHT : TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        }
        /* An app from the card says so at the other end of that border. */
        if (tile[i].sd && box.w >= 12)
            tui_put_str(sf, box, box.x + 2, box.y + box.h - 1, "[SD]",
                        TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

        /* A live app says so in words, in its top border. */

        if (tile[i].live && box.w >= 10) {
            tui_put_char(sf, box, box.x + box.w - 8, box.y, '*',
                         TUI_ATTR(TUI_GREEN | TUI_BRIGHT,TUI_BLACK));
            tui_put_str(sf, box, box.x + box.w - 6, box.y, ls_tr("LIVE"),
                        sel_now ? TUI_ATTR(TUI_BLACK, TUI_GREEN | TUI_BRIGHT)
                                : TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        }
        else if (tile[i].live)
            tui_put_char(sf, box, box.x + box.w - 2, box.y, LS_TUI_BLOCK_FULL,
                         TUI_ATTR(TUI_GREEN | TUI_BRIGHT,
                                  sel_now ? sel_bg : TUI_BLACK));

        /* Something new in there, in the same place and the same way as LIVE,
           in the colour of the update pop-up. */
        if (tile[i].badge && !tile[i].live) {
            const uint8_t dot = TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, sel_now ? sel_bg : TUI_BLACK);
            const char g = drip_glyph();
            if (box.w >= 10) {
                char tag[16];
                snprintf(tag, sizeof(tag), "%c %s %c", g, ls_tr("NEW"), g);
                tui_put_str(sf, box, box.x + box.w - 2 - (int)strlen(tag), box.y, tag, dot);
            } else {
                tui_put_char(sf, box, box.x + box.w - 2, box.y, g, dot);
            }
        }

        s_tile_hit[s_tile_n].x0 = (int16_t)box.x;
        s_tile_hit[s_tile_n].y0 = (int16_t)box.y;
        s_tile_hit[s_tile_n].x1 = (int16_t)(box.x + box.w);
        s_tile_hit[s_tile_n].y1 = (int16_t)(box.y + box.h);
        s_tile_n++;
    }
}

int ls_tile_hit(int col, int row) { return hit_find(s_tile_hit, s_tile_n, col, row); }

void ls_tile_shape(int *cols, int *rows)
{
    if (cols) *cols = s_tile_cols;
    if (rows) *rows = s_tile_rows;
}

/* --------------------------------------------------------------- fragments */

void ls_fill_dither(tui_surface *sf, tui_rect r, int density, uint8_t hue)
{
    const char g = (density >= LS_DITHER_HEAVY)  ? LS_TUI_SHADE_75
                 : (density == LS_DITHER_MEDIUM) ? LS_TUI_SHADE_50
                                                 : LS_TUI_SHADE_25;
    const uint8_t at = TUI_ATTR(hue, TUI_BLACK);
    tui_fill(sf, r, g, at);   /* clipped to r and the surface once, not per cell */
}

void ls_dither_label(tui_surface *sf, tui_rect r, int row, const char *text,
                     uint8_t attr)
{
    if (!text || !*text) return;
    text = ls_tr(text);
    /* Measured in cells: UTF-8 names have more bytes than letters. */
    int n = tui_utf8_cells(text, strlen(text));
    if (n > r.w) n = r.w;
    const int x = r.x + (r.w - n) / 2;
    const int y = r.y + row;

    if (x - 1 >= r.x) tui_put_char(sf, r, x - 1, y, ' ', attr);
    if (x + n < r.x + r.w) tui_put_char(sf, r, x + n, y, ' ', attr);
    const char *p = text;
    for (int i = 0; i < n && *p; i++)
        tui_put_char(sf, r, x + i, y, tui_str_cell(&p), attr);
}

__attribute__((noinline, optimize("Os"))) void ls_rframe(tui_surface *sf, tui_rect clip, tui_rect r, char h, uint8_t at)
{
    if (r.w < 2 || r.h < 2) return;
    /* Solid line glyphs: rounded and light, or square and double for '='. */
    const bool dbl = h == '=';
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (int x = r.x + 1; x < x1; x++) {
        ls_box_put(sf, clip, x, r.y, dbl ? LS_BOX_DBL_H : LS_BOX_H, at);
        ls_box_put(sf, clip, x, y1, dbl ? LS_BOX_DBL_H : LS_BOX_H, at);
    }
    for (int y = r.y + 1; y < y1; y++) {
        ls_box_put(sf, clip, r.x, y, dbl ? LS_BOX_DBL_V : LS_BOX_V, at);
        ls_box_put(sf, clip, x1, y, dbl ? LS_BOX_DBL_V : LS_BOX_V, at);
    }
    ls_box_put(sf, clip, r.x, r.y, dbl ? LS_BOX_DBL_DR : LS_BOX_ARC_DR, at);
    ls_box_put(sf, clip, x1, r.y, dbl ? LS_BOX_DBL_DL : LS_BOX_ARC_DL, at);
    ls_box_put(sf, clip, r.x, y1, dbl ? LS_BOX_DBL_UR : LS_BOX_ARC_UR, at);
    ls_box_put(sf, clip, x1, y1, dbl ? LS_BOX_DBL_UL : LS_BOX_ARC_UL, at);
}

void ls_panel_box(tui_surface *sf, tui_rect r, const char *title, uint8_t hue)
{
    /* In a desk window a panel is an etched group box, its caption in the
       panel's own hue on the face. */
    if (ls_dui_desk()) { ls_dui_group_ink(sf, r, title, TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK)); return; }
    tui_box(sf, r, NULL, TUI_ATTR(hue, TUI_BLACK));
    title = ls_tr(title);
    if (title && r.w > 6)
        tui_put_str(sf, r, r.x + 2, r.y, title,
                    ls_ink(LS_INK_CAPTION, TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT)));
}

/* An instrument with nothing to show says so in a panel its own size, in the middle of the space. */

void ls_panel_notice(tui_surface *sf, tui_rect area, const char *title,
                     const char *reason, const char *remedy)
{
    if (!sf || area.w < 12 || area.h < 4) return;
    reason = ls_tr(reason);
    remedy = ls_tr(remedy);

    const int lines = (reason ? 1 : 0) + (remedy ? 1 : 0);
    int h = lines + 4;                      /* border, air, lines, air, border */
    if (h > area.h) h = area.h;

    int w = 0;
    if (reason && (int)strlen(reason) > w) w = (int)strlen(reason);
    if (remedy && (int)strlen(remedy) > w) w = (int)strlen(remedy);
    w += 6;
    if (title && (int)strlen(title) + 6 > w) w = (int)strlen(title) + 6;
    if (w > area.w) w = area.w;

    tui_rect box = tui_rect_make(area.x + (area.w - w) / 2,
                                 area.y + (area.h - h) / 2, w, h);
    ls_panel_box(sf, box, title, TUI_CYAN);

    int y = box.y + 2;
    if (reason) {
        const int n = (int)strlen(reason);
        tui_put_str(sf, box, box.x + (w - n) / 2, y, reason,
                    TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        y++;
    }
    if (remedy) {
        const int n = (int)strlen(remedy);
        tui_put_str(sf, box, box.x + (w - n) / 2, y, remedy, LS_ATTR_DIM);
    }
}

void ls_kv(tui_surface *sf, tui_rect r, int row, const char *label,
           const char *value, uint8_t vattr)
{
    label = ls_tr(label);
    value = ls_tr(value);
    if (ls_dui_desk() && label) {
        /* A form's row: "Label:" in the label column, the value right
           aligned in a sunken field at the right, all the panel's fields one
           column wide so they line up (and the desk joins them). */
        const int y = r.y + row, lx = r.x + 2;
        const int ln = (int)strlen(label);
        tui_put_str(sf, r, lx, y, label, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_put_char(sf, r, lx + ln, y, ':', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        if (!value) return;
        const int vw = (int)strlen(value);
        const int right = r.x + r.w - 1;              /* the field's last cell sits inside the border */
        int fw = (r.w - 4) * 2 / 5;
        if (fw < vw + 3) fw = vw + 3;
        int fx = right - fw;
        if (fx < lx + ln + 2) { fx = lx + ln + 2; fw = right - fx; }
        if (fw < 3) return;
        const tui_rect in = ls_dui_field(sf, r, fx, y, fw);
        int tx = in.x + in.w - 1 - vw;
        if (tx < in.x) tx = in.x;
        tui_put_str(sf, tui_rect_intersect(r, in), tx, y, value, vattr);
        return;
    }
    tui_put_str(sf, r, r.x + 2, r.y + row, label,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (!value) return;
    int vw = (int)strlen(value);
    int x = r.x + r.w - 2 - vw;
    /* Right-aligned until it would collide with the label, then left-aligned
       at a fixed column. Two alignments is one more than ideal and still
       better than a value that overwrites its own label. */
    int min_x = r.x + 2 + (int)strlen(label) + 1;
    if (x < min_x) x = min_x;
    tui_put_str(sf, r, x, r.y + row, value, vattr);
}

tui_rect ls_kv_col(tui_surface *sf, tui_rect r, int row, const char *label,
                   const char *value, uint8_t vattr, int label_w)
{
    if (!ls_dui_desk() || !label) {
        ls_kv(sf, r, row, label, value, vattr);
        return tui_rect_make(0, 0, 0, 0);
    }
    label = ls_tr(label);
    const int y = r.y + row, lx = r.x + 2, ln = (int)strlen(label);
    const uint8_t cap = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    tui_put_str(sf, r, lx, y, label, cap);
    tui_put_char(sf, r, lx + ln, y, ':', cap);
    int fx = lx + label_w;
    if (fx < lx + ln + 2) fx = lx + ln + 2;
    const int fw = r.x + r.w - 1 - fx;     /* to the cell inside the border */
    if (fw < 3) return tui_rect_make(0, 0, 0, 0);
    const tui_rect in = ls_dui_field(sf, r, fx, y, fw);
    if (value) tui_put_str(sf, tui_rect_intersect(r, in), in.x, y, ls_tr(value), vattr);
    return in;
}

void ls_meter_ascii(tui_surface *sf, tui_rect clip, int x, int y, int w,
                    float fraction, uint8_t hue)
{
    if (w < 3) return;
    if (!(fraction > 0)) fraction = 0;
    if (fraction > 1) fraction = 1;
    const int span = w - 2, lit = (int)(fraction * span);
    const uint8_t ink = TUI_ATTR(hue, TUI_BLACK);
    tui_put_char(sf, clip, x, y, '[', ink);
    for (int i = 0; i < span; ++i) {
        bool tick = i == span/4 || i == span/2 || i == span*3/4;
        char ch = i < lit ? (tick ? '|' : '=') : (tick ? '+' : '-');
        if (i == lit-1 && lit < span) ch = '>';
        tui_put_char(sf, clip, x+1+i, y, ch, i < lit ? ink : LS_ATTR_FAINT);
    }
    tui_put_char(sf, clip, x+w-1, y, ']', ink);
}

void ls_bar(tui_surface *sf, tui_rect r, int row, int x, int w, float v)
{
    if (w <= 0) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    int lit = (int)(v * (float)w);
    for (int i = 0; i < w; i++) {
        uint8_t c = i < lit ? (i > w * 3 / 4 ? TUI_RED | TUI_BRIGHT
                             : i > w / 2     ? TUI_YELLOW | TUI_BRIGHT
                                             : TUI_GREEN | TUI_BRIGHT)
                            : (TUI_BLACK | TUI_BRIGHT);
        tui_put_char(sf, r, x + i, r.y + row,
                     i < lit ? LS_TUI_SHADE_FULL : LS_TUI_SHADE_25,
                     TUI_ATTR(c, TUI_BLACK));
    }
}

/* ------------------------------------------------- the desk kit's parts -- */

/* The primitives of ls_deskui.h's kit that the shared widgets above use
   themselves (a field, a cap's ends, a group box, the well's frame, the
   VFD's segments). Here, beside the widgets, so everything that draws a
   widget has them; the rest of the kit is in ls_deskui.c. */

bool ls_dui_desk(void) { return ls_tui_compact() && ls_ink_on; }

#define KIT_TEXT  TUI_ATTR(TUI_WHITE, TUI_BLACK)
static uint8_t kit_hi(void) { return ls_ink(LS_INK_HILITE, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)); }
static uint8_t kit_lo(void) { return ls_ink(LS_INK_SHADOW, TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK)); }

void ls_dui_edge(tui_surface *sf, tui_rect r, bool sunken)
{
    if (!sf || r.w < 2 || r.h < 2) return;
    const uint8_t tl = sunken ? kit_lo() : kit_hi(), br = sunken ? kit_hi() : kit_lo();
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (int x = r.x + 1; x < x1; x++) {
        ls_box_put(sf, r, x, r.y, LS_EDGE_B, tl);
        ls_box_put(sf, r, x, y1, LS_EDGE_T, br);
    }
    for (int y = r.y + 1; y < y1; y++) {
        ls_box_put(sf, r, r.x, y, LS_EDGE_R, tl);
        ls_box_put(sf, r, x1, y, LS_EDGE_L, br);
    }
    ls_box_put(sf, r, r.x, r.y, LS_EDGE_DOT_BR, tl);
    ls_box_put(sf, r, x1, r.y, LS_EDGE_DOT_BL, tl);
    ls_box_put(sf, r, r.x, y1, LS_EDGE_DOT_TR, tl);
    ls_box_put(sf, r, x1, y1, LS_EDGE_DOT_TL, br);
    ls_ink_bevel(r);
}

tui_rect ls_dui_group_ink(tui_surface *sf, tui_rect r, const char *caption, uint8_t cap)
{
    if (!sf || r.w < 2 || r.h < 2) return tui_rect_make(r.x, r.y, 0, 0);
    caption = ls_tr(caption);
    if (cap == kit_hi() || cap == kit_lo()) cap = ls_ink(LS_INK_TEXT, KIT_TEXT);
    /* Etched: the groove's dark side top and left, its light side bottom and right. */
    const uint8_t lo = kit_lo(), hi = kit_hi();
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (int x = r.x + 1; x < x1; x++) {
        ls_box_put(sf, r, x, r.y, LS_BOX_H, lo);
        ls_box_put(sf, r, x, y1, LS_BOX_H, hi);
    }
    for (int y = r.y + 1; y < y1; y++) {
        ls_box_put(sf, r, r.x, y, LS_BOX_V, lo);
        ls_box_put(sf, r, x1, y, LS_BOX_V, hi);
    }
    ls_box_put(sf, r, r.x, r.y, LS_BOX_DR, lo);
    ls_box_put(sf, r, x1, r.y, LS_BOX_DL, lo);
    ls_box_put(sf, r, r.x, y1, LS_BOX_UR, lo);
    ls_box_put(sf, r, x1, y1, LS_BOX_UL, hi);
    if (caption && caption[0] && r.w > 6) {
        const tui_rect top = tui_rect_make(r.x + 1, r.y, r.w - 3, 1);
        const int n = (int)strlen(caption);
        tui_put_char(sf, top, r.x + 1, r.y, ' ', KIT_TEXT);
        tui_put_str(sf, top, r.x + 2, r.y, caption, cap);
        tui_put_char(sf, top, r.x + 2 + n, r.y, ' ', KIT_TEXT);
    }
    ls_ink_bevel(r);
    return tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
}

tui_rect ls_dui_field(tui_surface *sf, tui_rect clip, int x, int y, int w)
{
    if (!sf || w < 3) return tui_rect_make(x, y, 0, 0);
    const tui_rect in = tui_rect_make(x + 1, y, w - 2, 1);
    tui_fill(sf, tui_rect_intersect(clip, in), ' ', KIT_TEXT);
    if (!ls_dui_desk()) {
        tui_put_char(sf, clip, x, y, '[', LS_ATTR_FAINT);
        tui_put_char(sf, clip, x + w - 1, y, ']', TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        return in;
    }
    ls_box_put(sf, clip, x, y, LS_EDGE_R, kit_lo());
    ls_box_put(sf, clip, x + w - 1, y, LS_EDGE_L, kit_hi());
    ls_ink_mark(tui_rect_make(x, y, w, 1), LS_INK_K_FILL);
    ls_ink_well(in);
    return in;
}

void ls_dui_cap_ends(tui_surface *sf, tui_rect clip, int x, int y, int w, bool down)
{
    if (!sf || w < 2) return;
    ls_box_put(sf, clip, x, y, LS_EDGE_L, down ? kit_lo() : kit_hi());
    ls_box_put(sf, clip, x + w - 1, y, LS_EDGE_R, down ? kit_hi() : kit_lo());
    ls_ink_mark(tui_rect_make(x, y, w, 1), LS_INK_K_FILL);
}

/* Segments a b c d e f g as bits 0..6, the way a seven-segment part numbers them. */
static uint8_t seg_mask(char c)
{
    static const uint8_t DIG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    if (c >= '0' && c <= '9') return DIG[c - '0'];
    if (c == '-') return 0x40;
    if (c == ' ') return 0;
    return 0xFF;                            /* not a figure */
}
bool ls_dui_seg_has(char c) { return seg_mask(c) != 0xFF; }

/* Heavy box glyph for arms up, right, down, left (bits 3..0). */
static int seg_glyph(unsigned arms)
{
    static const uint16_t G[16] = {
        ' ',    0x2578, 0x257B, 0x2513, 0x257A, 0x2501, 0x250F, 0x2533,
        0x2579, 0x251B, 0x2503, 0x252B, 0x2517, 0x253B, 0x2523, 0x254B,
    };
    return G[arms & 15];
}

/* The arms of cell (cx, cy) of a w x h figure whose lit segments are m. */
static unsigned seg_arms(uint8_t m, int cx, int cy, int w, int h)
{
    const int mid = h / 2, last = h - 1, right = w - 1;
    unsigned a = 0;                         /* U R D L = 8 4 2 1 */
    const bool row_a = cy == 0 && (m & 0x01), row_g = cy == mid && (m & 0x40), row_d = cy == last && (m & 0x08);
    if (row_a || row_g || row_d) {
        if (cx > 0) a |= 1;
        if (cx < right) a |= 4;
    }
    if (cx == 0 || cx == right) {
        const uint8_t up = cx == 0 ? 0x20 : 0x02, dn = cx == 0 ? 0x10 : 0x04;   /* f/b above the middle, e/c below */
        if ((m & up) && cy > 0 && cy <= mid) a |= 8;
        if ((m & up) && cy < mid) a |= 2;
        if ((m & dn) && cy > mid) a |= 8;
        if ((m & dn) && cy >= mid && cy < last) a |= 2;
    }
    return a;
}

bool ls_dui_seg(tui_surface *sf, tui_rect clip, int x, int y, int w, int h, char c, uint8_t lit, uint8_t ghost)
{
    const uint8_t m = seg_mask(c);
    if (m == 0xFF || !sf || w < 3 || (h != 3 && h != 5)) return false;
    for (int cy = 0; cy < h; cy++)
        for (int cx = 0; cx < w; cx++) {
            const unsigned on = seg_arms(m, cx, cy, w, h);
            if (on) { ls_box_put(sf, clip, x + cx, y + cy, seg_glyph(on), lit); continue; }
            const unsigned off = ghost ? seg_arms(0x7F, cx, cy, w, h) : 0;
            if (off) ls_box_put(sf, clip, x + cx, y + cy, seg_glyph(off), ghost);
        }
    return true;
}

/* Lit: the hue, bright. Ghost: the hue's quiet weight; in a desk window the
   desk keeps it as dim as the palette allows (a palette whose quiet cyan is
   nearly as bright as its lit one gets dark grey ghosts). */
void ls_dui_vfd_inks(int flags, uint8_t *lit, uint8_t *ghost)
{
    const int k = flags & 3;
    const uint8_t hue = k == LS_DUI_VFD_GREEN ? TUI_GREEN : k == LS_DUI_VFD_AMBER ? TUI_YELLOW : TUI_CYAN;
    if (lit) *lit = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
    if (ghost) *ghost = TUI_ATTR(hue, TUI_BLACK);
}

/* The rest of the kit: sections, the VFD readout, the status bar. */

static int kit_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

tui_rect ls_dui_group(tui_surface *sf, tui_rect r, const char *caption)
{
    if (!sf || r.w < 2 || r.h < 2) return tui_rect_make(r.x, r.y, 0, 0);
    if (!ls_dui_desk()) {
        ls_panel_box(sf, r, caption, TUI_CYAN);
        return tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    }
    /* The caption in the scheme's information hue; never one of the bevel's inks. */
    uint8_t cap = ls_ink(LS_INK_ACCENT, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (cap == kit_hi() || cap == kit_lo()) cap = ls_ink(LS_INK_TEXT, KIT_TEXT);
    return ls_dui_group_ink(sf, r, caption, cap);
}

static tui_rect kit_sunk(tui_surface *sf, tui_rect r, const char *caption, bool glass)
{
    if (!sf || r.w < 3 || r.h < 3) return tui_rect_make(r.x, r.y, 0, 0);
    caption = ls_tr(caption);
    const tui_rect in = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    if (!ls_dui_desk()) {
        tui_box(sf, r, NULL, glass ? TUI_ATTR(TUI_CYAN, TUI_BLACK) : LS_ATTR_FAINT);
        tui_fill(sf, in, ' ', KIT_TEXT);
    } else {
        tui_fill(sf, in, ' ', KIT_TEXT);
        ls_dui_edge(sf, r, true);
        if (glass) ls_ink_keep(in);
        else ls_ink_well(in);
    }
    if (caption && caption[0] && r.w > 6)
        tui_put_str(sf, tui_rect_make(r.x + 1, r.y, r.w - 2, 1), r.x + 1, r.y, caption,
                    ls_dui_desk() ? ls_ink(LS_INK_TEXT, KIT_TEXT) : TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    return in;
}

tui_rect ls_dui_well(tui_surface *sf, tui_rect r, const char *caption) { return kit_sunk(sf, r, caption, false); }
tui_rect ls_dui_glass(tui_surface *sf, tui_rect r) { return kit_sunk(sf, r, NULL, true); }

/* A tab strip on the body's top edge: ls_dui_tabs' Win98 tab control in one
   row. Every tab is a raised cap on the face (light edge left, dark right);
   the chosen one (btn.on) has its words strong and opens into the page (no
   line under it), the others are quieter and stand on the base line, which
   runs on under the rest of the row as the page's top edge. No selection
   band: a tab is not a list row. Hits as ls_btn_bar_slot records them. Off
   the desk it is ls_btn_bar_slot. */
void ls_btn_tabs_slot(tui_surface *sf, tui_rect row, const ls_btn_t *btn, int n, int focus, int slot)
{
    if (!ls_tui_compact() || !ls_dui_desk()) { ls_btn_bar_slot(sf, row, btn, n, focus, slot); return; }
    ls_btn_t tr_copy[TR_BTN_MAX];
    btn = tr_buttons(btn, n, tr_copy);
    if (slot < 0 || slot >= BTN_SLOTS) slot = 0;
    hit_clear(s_btn_hit[slot], &s_btn_n[slot]);
    if (!sf || n <= 0 || row.w < 6 || row.h < 1) return;
    row.h = 1;
    int fit = 0;
    while (fit < 2 && key1_rows(btn, n, row.w - 2, fit) > 1) fit++;
    const uint8_t hi = kit_hi(), lo = kit_lo();
    const uint8_t strong = ls_ink(LS_INK_STRONG, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    const uint8_t text = ls_ink(LS_INK_TEXT, KIT_TEXT);
    const uint8_t quiet = ls_ink(LS_INK_DIM, LS_ATTR_DIM);
    tui_fill(sf, row, ' ', KIT_TEXT);
    int x = row.x + 1;
    ls_box_put(sf, row, row.x, row.y, LS_EDGE_B, hi);
    for (int i = 0; i < n && s_btn_n[slot] < MAX_HITS; i++) {
        char lab[32], val[24];
        key1_text(&btn[i], fit, lab, sizeof(lab), val, sizeof(val));
        const int lw = (int)strlen(lab), vw = (int)strlen(val);
        const int w = 4 + lw + (vw ? vw + 1 : 0);
        if (x + w > row.x + row.w) break;
        const bool on = btn[i].on;
        /* The chosen tab's words strong; the keyboard's tab in the scheme's
           information hue; a tab that cannot be had greyed; the rest plain. */
        const uint8_t ink = btn[i].dim ? quiet : on ? strong
                          : i == focus ? ls_ink(LS_INK_ACCENT, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)) : text;
        /* The chosen tab stands raised (light edge left); the others sit
           behind it on the base line, a dark divider between them. */
        if (on) ls_box_put(sf, row, x, row.y, LS_EDGE_L, hi);
        else ls_box_put(sf, row, x, row.y, LS_EDGE_B, hi);
        int cx = x + 1;
        /* Its padding: open into the page when chosen, else the base line. */
        if (on) tui_put_char(sf, row, cx, row.y, ' ', KIT_TEXT);
        else ls_box_put(sf, row, cx, row.y, LS_EDGE_B, hi);
        cx++;
        int hot = -1;
        if (btn[i].key && !btn[i].dim) {
            const char k = (char)(btn[i].key >= 'a' && btn[i].key <= 'z' ? btn[i].key - 32 : btn[i].key);
            for (int j = 0; lab[j]; j++) {
                const char c = (char)(lab[j] >= 'a' && lab[j] <= 'z' ? lab[j] - 32 : lab[j]);
                if (c == k) { hot = j; break; }
            }
        }
        const uint8_t accel = ls_ink(LS_INK_ACCEL, TUI_ATTR(TUI_RED, TUI_BLACK));
        for (int j = 0; lab[j]; j++) tui_put_char(sf, row, cx++, row.y, lab[j], j == hot && !on ? accel : ink);
        if (vw) {
            tui_put_char(sf, row, cx++, row.y, ' ', ink);
            for (int j = 0; val[j]; j++) tui_put_char(sf, row, cx++, row.y, val[j], on ? strong : quiet);
        }
        if (on) tui_put_char(sf, row, cx, row.y, ' ', KIT_TEXT);
        else ls_box_put(sf, row, cx, row.y, LS_EDGE_B, hi);
        cx++;
        ls_box_put(sf, row, cx, row.y, LS_EDGE_R, lo);
        const int k = s_btn_n[slot];
        s_btn_enabled[slot][k] = !btn[i].dim;
        s_btn_keys[slot][k] = btn[i].key;
        s_btn_hit[slot][k] = (hit_t){(int16_t)x, (int16_t)row.y, (int16_t)(x + w - 1), (int16_t)row.y};
        s_btn_n[slot]++;
        x += w;
    }
    /* The page's top edge runs on under the rest of the row. */
    for (; x < row.x + row.w; x++) ls_box_put(sf, row, x, row.y, LS_EDGE_B, hi);
    ls_ink_mark(row, LS_INK_K_FILL);
}

void ls_dui_status(tui_surface *sf, tui_rect row, const char *const *panes, const int *w, int n)
{
    if (!sf || row.w < 4 || n < 1) return;
    row.h = 1;
    const bool desk = ls_dui_desk();
    const uint8_t text = ls_ink(LS_INK_TEXT, KIT_TEXT);
    tui_fill(sf, row, ' ', KIT_TEXT);
    int fixed = 0, flex = 0;
    for (int i = 0; i < n; i++) { if (w && w[i] > 0) fixed += w[i]; else flex++; }
    const int gaps = n - 1;
    int rest = row.w - fixed - gaps;
    if (rest < 0) rest = 0;
    int x = row.x;
    for (int i = 0; i < n && x < row.x + row.w; i++) {
        int pw = w && w[i] > 0 ? w[i] : (flex ? rest / flex + (i == n - 1 ? rest % flex : 0) : 0);
        if (x + pw > row.x + row.w) pw = row.x + row.w - x;
        if (pw < 3) { x += pw + 1; continue; }
        const tui_rect in = tui_rect_make(x + 1, row.y, pw - 2, 1);
        if (desk) {
            ls_box_put(sf, row, x, row.y, LS_EDGE_R, kit_lo());
            ls_box_put(sf, row, x + pw - 1, row.y, LS_EDGE_L, kit_hi());
        } else if (i) tui_put_char(sf, row, x - 1, row.y, '|', LS_ATTR_FAINT);
        const char *t = panes ? ls_tr(panes[i]) : NULL;
        if (t) tui_put_str(sf, tui_rect_intersect(row, in), in.x + 1, row.y, t, text);
        x += pw + 1;
    }
    if (desk) ls_ink_mark(row, LS_INK_K_FILL);
}

/* A tab control: the tabs on r's first two rows, the page under them. */
tui_rect ls_dui_tabs(tui_surface *sf, tui_rect r, const char *const *names, int n, int sel,
                     int16_t *tx, int16_t *tw)
{
    for (int t = 0; t < n; t++) { if (tx) tx[t] = -1; if (tw) tw[t] = 0; }
    if (!sf || r.w < 8 || r.h < 4 || n < 1) return tui_rect_make(r.x, r.y, 0, 0);
    const uint8_t hi = kit_hi(), lo = kit_lo();
    const uint8_t text = ls_ink(LS_INK_TEXT, KIT_TEXT), strong = ls_ink(LS_INK_STRONG, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    const int base = r.y + 2, x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    tui_fill(sf, tui_rect_make(r.x, r.y, r.w, 2), ' ', KIT_TEXT);
    /* The page: raised, its top edge the tabs' base line. */
    for (int x = r.x + 1; x < x1; x++) {
        ls_box_put(sf, r, x, base, LS_BOX_H, hi);
        ls_box_put(sf, r, x, y1, LS_BOX_H, lo);
    }
    for (int y = base + 1; y < y1; y++) {
        ls_box_put(sf, r, r.x, y, LS_BOX_V, hi);
        ls_box_put(sf, r, x1, y, LS_BOX_V, lo);
    }
    ls_box_put(sf, r, r.x, base, LS_BOX_DR, hi);
    ls_box_put(sf, r, x1, base, LS_BOX_DL, lo);
    ls_box_put(sf, r, r.x, y1, LS_BOX_UR, hi);
    ls_box_put(sf, r, x1, y1, LS_BOX_UL, lo);
    int x = r.x, right = r.x;
    for (int t = 0; t < n; t++) {
        const char *name = ls_tr(names[t] ? names[t] : "");
        const int w = kit_len(name) + 4;
        if (x + w > x1) break;
        const bool on = t == sel;
        const int top = on ? r.y : r.y + 1;
        ls_box_put(sf, r, x, top, LS_BOX_ARC_DR, hi);
        for (int i = 1; i < w - 1; i++) ls_box_put(sf, r, x + i, top, LS_BOX_H, hi);
        ls_box_put(sf, r, x + w - 1, top, LS_BOX_ARC_DL, lo);
        if (on) {
            ls_box_put(sf, r, x, r.y + 1, LS_BOX_V, hi);
            ls_box_put(sf, r, x + w - 1, r.y + 1, LS_BOX_V, lo);
            /* the chosen tab opens into its page */
            for (int i = 1; i < w - 1; i++) tui_put_char(sf, r, x + i, base, ' ', KIT_TEXT);
            ls_box_put(sf, r, x, base, x == r.x ? LS_BOX_V : LS_BOX_UL, hi);
            ls_box_put(sf, r, x + w - 1, base, LS_BOX_UR, hi);
        }
        tui_put_str(sf, r, x + 2, r.y + 1, name, on ? strong : text);
        if (tx) tx[t] = (int16_t)x;
        if (tw) tw[t] = (int16_t)w;
        x += w;
        right = x;
    }
    if (ls_dui_desk()) {
        ls_ink_mark(tui_rect_make(r.x, r.y, right - r.x, 3), LS_INK_K_FILL);
        ls_ink_bevel(tui_rect_make(r.x, base, r.w, r.h - 2));
    }
    return tui_rect_make(r.x + 1, base + 1, r.w - 2, r.h - 4);
}

/* ------------------------------------------------------------------ VFD -- */

/* Columns the text takes as figures cw wide, one blank after each. */
static int vfd_width(const char *s, int cw)
{
    int w = 0;
    for (; *s; s++) w += (*s == '.' || *s == ':') ? 2 : ls_dui_seg_has(*s) ? cw + 1 : 1;
    return w > 0 ? w - 1 : 0;
}

void ls_dui_vfd(tui_surface *sf, tui_rect r, const char *text, int flags)
{
    if (!sf || r.w < 1 || r.h < 1) return;
    if (!text) text = "";
    uint8_t lit, ghost;
    ls_dui_vfd_inks(flags, &lit, &ghost);
    tui_fill(sf, r, ' ', KIT_TEXT);
    ls_ink_mark(r, LS_INK_K_VFD);
    int h = !(flags & LS_DUI_VFD_SMALL) && r.h >= 5 ? 5 : !(flags & LS_DUI_VFD_SMALL) && r.h >= 3 ? 3 : 1;
    int cw = h == 5 ? 4 : 3;
    while (h > 1 && vfd_width(text, cw) + 2 > r.w) {
        if (cw > 3) cw--;
        else if (h == 5) h = 3;
        else h = 1;
    }
    if (h == 1) {
        const int n = kit_len(text);
        const int x = flags & LS_DUI_VFD_LEFT ? r.x + 1 : r.x + (r.w - n) / 2;
        tui_put_str(sf, r, x, r.y + (r.h - 1) / 2, text, lit);
        return;
    }
    const int tw = vfd_width(text, cw);
    int x = flags & LS_DUI_VFD_LEFT ? r.x + 1 : r.x + (r.w - tw) / 2;
    const int y = r.y + (r.h - h) / 2;
    for (const char *p = text; *p; p++) {
        if (*p == '.') {
            ls_box_put(sf, r, x, y + h - 1, LS_SHAPE_DOT, lit);
            x += 2;
        } else if (*p == ':') {
            ls_box_put(sf, r, x, y + h / 2 - (h == 5 ? 1 : 0), LS_SHAPE_DOT, lit);
            ls_box_put(sf, r, x, y + h - 1 - (h == 5 ? 1 : 0), LS_SHAPE_DOT, lit);
            x += 2;
        } else if (ls_dui_seg(sf, r, x, y, cw, h, *p, lit, ghost)) {
            x += cw + 1;
        } else {
            tui_put_char(sf, r, x, y + h - 1, *p, lit);
            x += 1;
        }
    }
}
