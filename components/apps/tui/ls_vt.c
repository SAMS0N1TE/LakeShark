/* See ls_vt_vt.h. */

#include "ls_vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_tui_screen.h"
#include "ls_box.h"
#include "ls_textpack.h"

static uint32_t rgb888(uint16_t c)
{
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 + 15) / 31) << 16 | ((g * 255 + 31) / 63) << 8 | ((b * 255 + 15) / 31);
}

/* a over b by alpha/4 */
static uint32_t blend(uint32_t a, uint32_t b, int quarters)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const uint32_t x = (a >> sh) & 255, y = (b >> sh) & 255;
        out |= ((x * (uint32_t)quarters + y * (uint32_t)(4 - quarters)) / 4) << sh;
    }
    return out;
}

static uint32_t mean2(uint32_t a, uint32_t b) { return blend(a, b, 2); }

/* Quadrant byte bits: 1 top left, 2 top right, 4 bottom left, 8 bottom right. */
static const uint16_t QUAD[16] = {
    0x0020, 0x2598, 0x259D, 0x2580, 0x2596, 0x258C, 0x259E, 0x259B,
    0x2597, 0x259A, 0x2590, 0x259C, 0x2584, 0x2599, 0x259F, 0x2588,
};

/* Sextant bits are (row * 2 + col), the same order Unicode numbers its
   sextants in; the four that are already block elements are not repeated
   in U+1FB00..1FB3B. */
static uint32_t sextant(int bits, bool quadrants_only)
{
    bits &= 63;
    if (quadrants_only) {
        const int q = ((bits & 5) ? 1 : 0) | ((bits & 10) ? 2 : 0) |
                      ((bits & 16) ? 4 : 0) | ((bits & 32) ? 8 : 0);
        return QUAD[q];
    }
    if (bits == 0) return 0x20;
    if (bits == 21) return 0x258C;
    if (bits == 42) return 0x2590;
    if (bits == 63) return 0x2588;
    return 0x1FB00u + (uint32_t)bits - 1 - (bits > 21) - (bits > 42);
}

/* The device draws a trace as a filament: two pixels wide, centred, rising
   n/8 of the cell from the bottom, so a column of them is a bar (VOLUME,
   SIG, the activity trace). Upright, then: a dot at the foot, the lower half
   of a vertical line, or the whole of one. */
static uint32_t trace(int eighths)
{
    if (eighths <= 2) return '.';
    if (eighths <= 5) return 0x2577;
    return 0x2502;
}

void ls_vt_resolve(const ls_vt_frame *f, bool quadrants_only, ls_vt_cell *out)
{
    uint32_t pal[16];
    for (int i = 0; i < 16; i++) pal[i] = rgb888(f->palette[i]);
    for (int r = 0; r < f->rows; r++) {
        for (int c = 0; c < f->cols; c++) {
            const size_t i = (size_t)r * f->cols + c;
            const tui_cell *cell = &f->cells[i];
            int ch = cell->ch;
            if (ch < 0) ch &= 0xFF;
            ls_vt_cell o = {0x20, pal[TUI_ATTR_FG(cell->attr)], pal[TUI_ATTR_BG(cell->attr)]};
            const bool in_img = f->image.w > 0 && f->img_top && tui_rect_contains(f->image, c, r);
            if (ch == 0xB0) {
                if (in_img) { o.cp = 0x2580; o.fg = f->img_top[i]; o.bg = f->img_bot[i]; }
                out[i] = o;
                continue;
            }
            if (in_img && f->glass && (f->glass[i >> 3] >> (i & 7) & 1))
                o.bg = mean2(f->img_top[i], f->img_bot[i]);
            if (ch >= 0x20 && ch < 0x7F)                 o.cp = (uint32_t)ch;
            else if (ch >= 0x80 && ch <= 0x8F)           o.cp = QUAD[ch & 15];
            else if (ch >= 0x90 && ch <= 0x93)           o.bg = blend(o.fg, o.bg, ch - 0x90 + 1);
            else if (ch >= 0xA0 && ch <= 0xA7)           o.cp = trace(ch - 0xA0 + 1);
            else if (ch >= 0xC0 && ch <= 0xFF)           o.cp = sextant(ch, quadrants_only);
            else if (ch >= 0x2800 && ch <= 0x28FF)       o.cp = (uint32_t)ch;
            else if (ch >= 0x2000 && ls_box_is(ch))      o.cp = ls_box_vt(ch);
            else if (ch >= 0x80) {
                const int slot = tui_cyr_slot(ch);
                o.cp = slot >= 0 ? tui_cyr_slot_codepoint(slot) : '?';
            }
            out[i] = o;
        }
    }
}

static uint32_t pixel(const uint16_t *src, int w, int h, int x0, int y0, int x, int y)
{
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    return rgb888(src[(size_t)((y + y0) % h) * w + (x + x0) % w]);
}

/* Mean of four points in [xa,xb) x [ya,yb). */
static uint32_t area(const uint16_t *src, int w, int h, int x0, int y0, int xa, int xb, int ya, int yb)
{
    const int xs[2] = {xa + (xb - xa) / 4, xa + (3 * (xb - xa)) / 4};
    const int ys[2] = {ya + (yb - ya) / 4, ya + (3 * (yb - ya)) / 4};
    uint32_t sum[3] = {0, 0, 0};
    for (int j = 0; j < 2; j++)
        for (int k = 0; k < 2; k++) {
            const uint32_t p = pixel(src, w, h, x0, y0, xs[k], ys[j]);
            sum[0] += p >> 16; sum[1] += (p >> 8) & 255; sum[2] += p & 255;
        }
    return (sum[0] / 4) << 16 | (sum[1] / 4) << 8 | (sum[2] / 4);
}

void ls_vt_sample_image(tui_rect cells, int cols, const uint16_t *src, int w, int h,
                          int x0, int y0, uint32_t *img_top, uint32_t *img_bot)
{
    if (!src || w <= 0 || h <= 0 || cells.w <= 0 || cells.h <= 0) return;
    for (int cy = 0; cy < cells.h; cy++) {
        const int ya = cy * h / cells.h, yb = (cy + 1) * h / cells.h;
        const int ym = ya + (yb - ya + 1) / 2;
        for (int cx = 0; cx < cells.w; cx++) {
            const int xa = cx * w / cells.w, xb = (cx + 1) * w / cells.w;
            const size_t i = (size_t)(cells.y + cy) * cols + cells.x + cx;
            img_top[i] = area(src, w, h, x0, y0, xa, xb > xa ? xb : xa + 1, ya, ym > ya ? ym : ya + 1);
            img_bot[i] = area(src, w, h, x0, y0, xa, xb > xa ? xb : xa + 1, ym, yb > ym ? yb : ym + 1);
        }
    }
}

void ls_vt_screen_init(ls_vt_screen *s, int cols, int rows, ls_vt_cell *shown)
{
    s->cols = cols;
    s->rows = rows;
    s->shown = shown;
    s->full = true;
    s->clear = false;
    s->fg = s->bg = 0xFFFFFFFFu;
    s->term_cols = s->term_rows = 0;
    s->x0 = s->y0 = 0;
    s->quadrants = false;
    s->bar_fg = s->bar_bg = 0xFFFFFFFFu;
}

/* --- the view: a terminal smaller than the grid ------------------------ */

static bool cropped(const ls_vt_screen *s)
{
    return s->term_cols > 0 && (s->term_cols < s->cols || s->term_rows < s->rows);
}

void ls_vt_screen_view(const ls_vt_screen *s, int *w, int *h)
{
    const bool crop = cropped(s);
    *w = crop && s->term_cols < s->cols ? s->term_cols : s->cols;
    *h = crop && s->term_rows <= s->rows ? s->term_rows - 1 : s->rows;   /* the bar's row comes off */
}

static void clamp_view(ls_vt_screen *s)
{
    int w, h;
    ls_vt_screen_view(s, &w, &h);
    if (s->x0 > s->cols - w) s->x0 = s->cols - w;
    if (s->y0 > s->rows - h) s->y0 = s->rows - h;
    if (s->x0 < 0) s->x0 = 0;
    if (s->y0 < 0) s->y0 = 0;
}

bool ls_vt_screen_resize(ls_vt_screen *s, int term_cols, int term_rows)
{
    if (term_cols < 10 || term_rows < 3) term_cols = term_rows = 0;   /* not a size anyone can use */
    if (term_cols == s->term_cols && term_rows == s->term_rows) return false;
    s->term_cols = term_cols;
    s->term_rows = term_rows;
    clamp_view(s);
    s->clear = s->full = true;
    return true;
}

void ls_vt_screen_pan(ls_vt_screen *s, int dcols, int drows)
{
    const int x0 = s->x0, y0 = s->y0;
    s->x0 += dcols;
    s->y0 += drows;
    clamp_view(s);
    if (s->x0 != x0 || s->y0 != y0) s->full = true;
}

/* The bar's buttons, left to right, an equal share of its width each. */
enum { BAR_LEFT, BAR_UP, BAR_WHERE, BAR_DOWN, BAR_RIGHT };

static int bar_buttons(const ls_vt_screen *s, int *b)
{
    int w, h, n = 0;
    ls_vt_screen_view(s, &w, &h);
    const bool across = w < s->cols, down = h < s->rows;
    if (across) b[n++] = BAR_LEFT;
    if (down) b[n++] = BAR_UP;
    b[n++] = BAR_WHERE;
    if (down) b[n++] = BAR_DOWN;
    if (across) b[n++] = BAR_RIGHT;
    return n;
}

/* The bar as text, w ASCII characters: ASCII so that no font lacks it and
   no terminal takes a character in it for two cells. */
static void bar_text(const ls_vt_screen *s, int w, int h, char *t)
{
    int b[5];
    const int n = bar_buttons(s, b);
    memset(t, ' ', (size_t)w);
    for (int i = 0; i < n; i++) {
        char label[24];
        switch (b[i]) {
        case BAR_LEFT:  snprintf(label, sizeof(label), "<"); break;
        case BAR_RIGHT: snprintf(label, sizeof(label), ">"); break;
        case BAR_UP:    snprintf(label, sizeof(label), n > 3 ? "^" : "^ UP"); break;
        case BAR_DOWN:  snprintf(label, sizeof(label), n > 3 ? "v" : "v DOWN"); break;
        default:
            if (h < s->rows) snprintf(label, sizeof(label), "%d-%d/%d", s->y0 + 1, s->y0 + h, s->rows);
            else             snprintf(label, sizeof(label), "%d-%d/%d", s->x0 + 1, s->x0 + w, s->cols);
            break;
        }
        const int a = i * w / n, z = (i + 1) * w / n;
        int len = (int)strlen(label);
        if (len > z - a) len = z - a;
        memcpy(t + a + (z - a - len) / 2, label, (size_t)len);
    }
}

bool ls_vt_screen_tap(ls_vt_screen *s, int col, int row, int *gcol, int *grow)
{
    int w, h;
    ls_vt_screen_view(s, &w, &h);
    if (col < 0 || row < 0 || col >= w) return false;
    if (row == h && cropped(s)) {
        int b[5];
        const int n = bar_buttons(s, b);
        const int step_r = h / 2 > 1 ? h / 2 : 1, step_c = w / 2 > 1 ? w / 2 : 1;
        switch (b[col * n / w]) {
        case BAR_LEFT:  ls_vt_screen_pan(s, -step_c, 0); break;
        case BAR_RIGHT: ls_vt_screen_pan(s, step_c, 0); break;
        case BAR_UP:    ls_vt_screen_pan(s, 0, -step_r); break;
        case BAR_DOWN:  ls_vt_screen_pan(s, 0, step_r); break;
        default: break;
        }
        return false;
    }
    if (row >= h) return false;
    *gcol = col + s->x0;
    *grow = row + s->y0;
    return true;
}

/* The buffer is the caller's: a whole screen wants 2 KB, but entering and
   leaving run on the console task (a 'term off', or a quit key fed through
   'term in'), whose 4 KB stack a 2 KB buffer under printf overflowed. */
typedef struct {
    char *buf;
    size_t cap, n, total;
    ls_vt_out_fn out;
    void *ctx;
} sink;

static void flush(sink *k)
{
    if (k->n) k->out(k->ctx, k->buf, k->n);
    k->total += k->n;
    k->n = 0;
}

static void put(sink *k, const char *s, size_t n)
{
    if (k->n + n > k->cap) flush(k);
    if (n > k->cap) {                 /* longer than the whole buffer: straight through */
        k->out(k->ctx, s, n);
        k->total += n;
        return;
    }
    memcpy(k->buf + k->n, s, n);
    k->n += n;
}

static void puts_(sink *k, const char *s) { put(k, s, strlen(s)); }

static void put_utf8(sink *k, uint32_t cp)
{
    char u[4];
    size_t n;
    if (cp < 0x80)         { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800)   { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 63));
                             u[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else                   { u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 63));
                             u[2] = (char)(0x80 | ((cp >> 6) & 63)); u[3] = (char)(0x80 | (cp & 63)); n = 4; }
    put(k, u, n);
}

/* U+1FB00..1FB3B as the quadrant ls_vt_resolve would have chosen. */
static uint32_t fold(uint32_t cp)
{
    if (cp < 0x1FB00u || cp > 0x1FB3Bu) return cp;
    int bits = (int)(cp - 0x1FB00u) + 1;
    bits += bits >= 21;
    bits += bits >= 42;
    return sextant(bits, true);
}

static void put_colours(sink *k, bool new_fg, bool new_bg, uint32_t f, uint32_t b)
{
    char seq[64];
    if (new_fg && new_bg) {
        put(k, seq, (size_t)LS_SNPRINTF(t_ls_vt_38_2_u_u, seq, sizeof(seq), "\x1b[38;2;%u;%u;%u;48;2;%u;%u;%um",
            (unsigned)(f >> 16), (unsigned)(f >> 8 & 255), (unsigned)(f & 255),
            (unsigned)(b >> 16), (unsigned)(b >> 8 & 255), (unsigned)(b & 255)));
    } else if (new_fg || new_bg) {
        const uint32_t v = new_fg ? f : b;
        put(k, seq, (size_t)snprintf(seq, sizeof(seq), "\x1b[%d;2;%u;%u;%um", new_fg ? 38 : 48,
            (unsigned)(v >> 16), (unsigned)(v >> 8 & 255), (unsigned)(v & 255)));
    }
}

size_t ls_vt_render(ls_vt_screen *s, const ls_vt_cell *cells, ls_vt_out_fn out, void *ctx)
{
    char buf[2048];
    sink k = {.buf = buf, .cap = sizeof(buf), .out = out, .ctx = ctx};
    int at_r = -1, at_c = -1;
    bool any = false;
    if (s->clear) {
        puts_(&k, "\x1b[?2026h\x1b[0m\x1b[2J");
        any = true;
        s->clear = false;
        s->full = true;
    }
    uint32_t fg = s->full ? 0xFFFFFFFFu : s->fg, bg = s->full ? 0xFFFFFFFFu : s->bg;
    char seq[64];
    int vw, vh;
    ls_vt_screen_view(s, &vw, &vh);
    for (int r = s->y0; r < s->y0 + vh; r++) {
        for (int c = s->x0; c < s->x0 + vw; c++) {
            const size_t i = (size_t)r * s->cols + c;
            const ls_vt_cell *x = &cells[i];
            ls_vt_cell *was = &s->shown[i];
            /* Colour is most of the bytes on the wire: a space shows only its
               background and a full block only its foreground, so the other
               one neither makes the cell differ nor gets sent. */
            const bool fg_shows = x->cp != 0x20, bg_shows = x->cp != 0x2588;
            if (!s->full && was->cp == x->cp && (!fg_shows || was->fg == x->fg) &&
                (!bg_shows || was->bg == x->bg)) continue;
            if (!any) { puts_(&k, "\x1b[?2026h"); any = true; }
            if (r != at_r || c != at_c) {
                put(&k, seq, (size_t)snprintf(seq, sizeof(seq), "\x1b[%d;%dH", r - s->y0 + 1, c - s->x0 + 1));
            }
            const bool new_fg = fg_shows && x->fg != fg, new_bg = bg_shows && x->bg != bg;
            put_colours(&k, new_fg, new_bg, x->fg, x->bg);
            if (new_fg) fg = x->fg;
            if (new_bg) bg = x->bg;
            put_utf8(&k, s->quadrants ? fold(x->cp) : x->cp);
            at_r = r; at_c = c + 1;
            *was = *x;
        }
    }
    /* The bar, in the colours of the grid's top line (the status bar). */
    if (cropped(s) && (s->full || cells[0].fg != s->bar_fg || cells[0].bg != s->bar_bg)) {
        char t[256];
        const int w = vw < (int)sizeof(t) ? vw : (int)sizeof(t);
        bar_text(s, w, vh, t);
        if (!any) { puts_(&k, "\x1b[?2026h"); any = true; }
        put(&k, seq, (size_t)snprintf(seq, sizeof(seq), "\x1b[%d;1H", vh + 1));
        const bool new_fg = cells[0].fg != fg, new_bg = cells[0].bg != bg;
        put_colours(&k, new_fg, new_bg, cells[0].fg, cells[0].bg);
        fg = cells[0].fg;
        bg = cells[0].bg;
        /* The terminal's last cell is erased in the bar's colour, not written:
           a terminal that wraps as soon as it is written scrolls the screen. */
        const bool corner = vh + 1 == s->term_rows && w == s->term_cols;
        put(&k, t, (size_t)(corner ? w - 1 : w));
        if (corner) puts_(&k, "\x1b[X");
        s->bar_fg = fg;
        s->bar_bg = bg;
    }
    s->full = false;
    s->fg = fg;
    s->bg = bg;
    if (any) puts_(&k, "\x1b[?2026l");
    flush(&k);
    return k.total;
}

size_t ls_vt_enter(ls_vt_out_fn out, void *ctx, int cols, int rows)
{
    char buf[128];
    sink k = {.buf = buf, .cap = sizeof(buf), .out = out, .ctx = ctx};
    char seq[48];
    /* Ask for the grid's size; terminals that allow it resize, the rest ignore it. */
    put(&k, seq, (size_t)snprintf(seq, sizeof(seq), "\x1b[8;%d;%dt", rows, cols));
    puts_(&k, LS_TEXT(t_ls_vt_1049h_25l_7l_1000h, "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b]0;LakeShark\x07\x1b[0m\x1b[2J"));
    flush(&k);
    return k.total;
}

size_t ls_vt_leave(ls_vt_out_fn out, void *ctx)
{
    char buf[64];
    sink k = {.buf = buf, .cap = sizeof(buf), .out = out, .ctx = ctx};
    puts_(&k, LS_TEXT(t_ls_vt_1006l_1002l_1000l_0m, "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l"));
    flush(&k);
    return k.total;
}

size_t ls_vt_ask_size(ls_vt_out_fn out, void *ctx)
{
    /* Forward and down, not a cursor position: those stop at the edge in
       every terminal, where one sent past it is not held there by all.
       The cursor goes back where it was once the report is asked for. */
    static const char ask[] = "\x1b" "7\x1b[999C\x1b[999B\x1b[6n\x1b" "8";
    out(ctx, ask, sizeof(ask) - 1);
    return sizeof(ask) - 1;
}

/* --- telnet ------------------------------------------------------------ */

enum { TN_DATA, TN_IAC, TN_VERB, TN_SB, TN_SB_IAC };

size_t ls_vt_telnet_strip(ls_vt_telnet *t, uint8_t *b, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t x = b[i];
        switch (t->state) {
        case TN_DATA:
            if (x == 0xFF) t->state = TN_IAC; else b[o++] = x;
            break;
        case TN_IAC:
            if (x == 0xFF)      { b[o++] = x; t->state = TN_DATA; }
            else if (x == 0xFA) { t->sb_n = 0; t->state = TN_SB; }
            else                t->state = x >= 0xFB ? TN_VERB : TN_DATA;
            break;
        case TN_VERB:
            t->state = TN_DATA;
            break;
        case TN_SB:
            if (x == 0xFF) t->state = TN_SB_IAC;
            else if (t->sb_n < (int)sizeof(t->sb)) t->sb[t->sb_n++] = x;
            break;
        case TN_SB_IAC:
            if (x == 0xFF) {                       /* a 255 inside it, doubled */
                if (t->sb_n < (int)sizeof(t->sb)) t->sb[t->sb_n++] = x;
                t->state = TN_SB;
                break;
            }
            if (t->sb_n >= 5 && t->sb[0] == 31) {  /* NAWS: width and height, 16 bits each */
                t->cols = t->sb[1] << 8 | t->sb[2];
                t->rows = t->sb[3] << 8 | t->sb[4];
                t->sized = true;
            }
            t->state = TN_DATA;
            break;
        }
    }
    return o;
}

/* --- input ------------------------------------------------------------ */

static int key(ls_vt_event *ev, int *n, int max, int k, char ch)
{
    if (*n < max) ev[(*n)++] = (ls_vt_event){.type = LS_VT_EV_KEY, .key = k, .ch = ch};
    return 1;
}

static void event(ls_vt_event *ev, int *n, int max, ls_vt_event e)
{
    if (*n < max) ev[(*n)++] = e;
}

/* Bytes taken from the front of buf, 0 when the sequence is still incomplete. */
static int take(ls_vt_input *in, ls_vt_event *ev, int *n, int max)
{
    const uint8_t *b = in->buf;
    const int len = in->n;
    if (b[0] != 0x1B) {
        const uint8_t x = b[0], last = in->last;
        in->last = x;
        if (x == 0x1D || x == 0x03) event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_QUIT});
        else if (x == 0x0C)         event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_REDRAW});
        else if (x == '\r')         key(ev, n, max, LS_TK_ENTER, 0);
        else if (x == '\n')         { if (last != '\r') key(ev, n, max, LS_TK_ENTER, 0); }
        else if (x == '\t')         key(ev, n, max, LS_TK_TAB, 0);
        else if (x == 0x7F || x == 0x08) key(ev, n, max, LS_TK_BACKSPACE, 0);
        else if (x >= 0x20 && x < 0x7F) key(ev, n, max, LS_TK_CHAR, (char)x);
        return 1;                                  /* UTF-8 and other controls are dropped */
    }
    in->last = 0x1B;
    if (len < 2) return 0;
    if (b[1] == 'O') {                             /* SS3: F1-F4, and arrows in application mode */
        if (len < 3) return 0;
        static const char ss3[] = "PQRSABCDH";
        static const int ss3k[] = {LS_TK_F1, LS_TK_F2, LS_TK_F3, LS_TK_F4,
                                   LS_TK_UP, LS_TK_DOWN, LS_TK_RIGHT, LS_TK_LEFT, LS_TK_F1};
        const char *p = strchr(ss3, b[2]);
        if (p && b[2]) key(ev, n, max, ss3k[p - ss3], 0);
        return 3;
    }
    if (b[1] != '[') {                             /* Alt+key: Escape, then the key */
        key(ev, n, max, LS_TK_ESC, 0);
        return 1;
    }
    int end = 2;
    while (end < len && !(b[end] >= 0x40 && b[end] <= 0x7E)) end++;
    if (end >= len) return len >= (int)sizeof(in->buf) ? len : 0;
    const char final = (char)b[end];
    char params[32];
    const int pn = end - 2 < (int)sizeof(params) - 1 ? end - 2 : (int)sizeof(params) - 1;
    memcpy(params, b + 2, (size_t)pn);
    params[pn] = 0;
    if (params[0] == '<' && (final == 'M' || final == 'm')) {   /* SGR mouse */
        int btn = 0, x = 0, y = 0;
        if (sscanf(params + 1, "%d;%d;%d", &btn, &x, &y) == 3 && final == 'm') {
            if ((btn & 3) == 0 && !(btn & 64))
                event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_UP, .col = x - 1, .row = y - 1});
        } else if (sscanf(params + 1, "%d;%d;%d", &btn, &x, &y) == 3 && final == 'M') {
            if ((btn & 32) && (btn & 3) == 0)          /* motion with the left button held */
                event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_DRAG, .col = x - 1, .row = y - 1});
            else if (btn & 32) {}                      /* motion with no button, or another one */
            else if (btn == 64 || btn == 65)
                event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_KEY, .key = btn == 64 ? LS_TK_UP : LS_TK_DOWN,
                                                .wheel = true, .col = x - 1, .row = y - 1});
            else if ((btn & ~(4 | 8 | 16)) == 0)
                event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_TAP, .col = x - 1, .row = y - 1});
        }
        return end + 1;
    }
    const int p1 = params[0] >= '0' && params[0] <= '9' ? atoi(params) : 0;
    int rows = 0, cols = 0;
    if (final == 'R' && in->want_size && sscanf(params, "%d;%d", &rows, &cols) == 2) {
        in->want_size = false;                     /* the cursor report: where it stopped is the size */
        event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_SIZE, .col = cols, .row = rows});
        return end + 1;
    }
    switch (final) {
    case 'A': key(ev, n, max, LS_TK_UP, 0); break;
    case 'B': key(ev, n, max, LS_TK_DOWN, 0); break;
    case 'C': key(ev, n, max, LS_TK_RIGHT, 0); break;
    case 'D': key(ev, n, max, LS_TK_LEFT, 0); break;
    case 'H': key(ev, n, max, LS_TK_F1, 0); break;
    case 'P': case 'Q': case 'R': case 'S': key(ev, n, max, LS_TK_F1 + (final - 'P'), 0); break;
    case 'Z': key(ev, n, max, LS_TK_TAB, 0); break;
    case '~':
        if (p1 == 1 || p1 == 7)           key(ev, n, max, LS_TK_F1, 0);
        else if (p1 == 3)                 key(ev, n, max, LS_TK_BACKSPACE, 0);
        else if (p1 >= 11 && p1 <= 15)    key(ev, n, max, LS_TK_F1 + p1 - 11, 0);
        else if (p1 >= 17 && p1 <= 21)    key(ev, n, max, LS_TK_F6 + p1 - 17, 0);
        else if (p1 == 23)                key(ev, n, max, LS_TK_F11, 0);
        else if (p1 == 5)                 key(ev, n, max, LS_TK_UP, 0);
        else if (p1 == 6)                 key(ev, n, max, LS_TK_DOWN, 0);
        break;
    default: break;
    }
    return end + 1;
}

int ls_vt_input_feed(ls_vt_input *in, const uint8_t *bytes, size_t len,
                       ls_vt_event *ev, int max)
{
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (in->n < (int)sizeof(in->buf)) in->buf[in->n++] = bytes[i];
        for (;;) {
            if (!in->n) break;
            const int used = take(in, ev, &n, max);
            if (!used) break;
            memmove(in->buf, in->buf + used, (size_t)(in->n - used));
            in->n -= used;
        }
    }
    return n;
}

int ls_vt_input_idle(ls_vt_input *in, ls_vt_event *ev, int max)
{
    int n = 0;
    if (in->n == 1 && in->buf[0] == 0x1B) key(ev, &n, max, LS_TK_ESC, 0);
    in->n = 0;                                     /* a sequence left half sent is noise */
    return n;
}
