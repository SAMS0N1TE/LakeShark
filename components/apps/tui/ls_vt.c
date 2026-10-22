/* See ls_vt_vt.h. */

#include "ls_vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_tui_screen.h"

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
    s->fg = s->bg = 0xFFFFFFFFu;
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

size_t ls_vt_render(ls_vt_screen *s, const ls_vt_cell *cells, ls_vt_out_fn out, void *ctx)
{
    char buf[2048];
    sink k = {.buf = buf, .cap = sizeof(buf), .out = out, .ctx = ctx};
    int at_r = -1, at_c = -1;
    uint32_t fg = s->full ? 0xFFFFFFFFu : s->fg, bg = s->full ? 0xFFFFFFFFu : s->bg;
    bool any = false;
    char seq[64];
    for (int r = 0; r < s->rows; r++) {
        for (int c = 0; c < s->cols; c++) {
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
                put(&k, seq, (size_t)snprintf(seq, sizeof(seq), "\x1b[%d;%dH", r + 1, c + 1));
            }
            const bool new_fg = fg_shows && x->fg != fg, new_bg = bg_shows && x->bg != bg;
            if (new_fg && new_bg) {
                put(&k, seq, (size_t)snprintf(seq, sizeof(seq), "\x1b[38;2;%u;%u;%u;48;2;%u;%u;%um",
                    (unsigned)(x->fg >> 16), (unsigned)(x->fg >> 8 & 255), (unsigned)(x->fg & 255),
                    (unsigned)(x->bg >> 16), (unsigned)(x->bg >> 8 & 255), (unsigned)(x->bg & 255)));
            } else if (new_fg || new_bg) {
                const uint32_t v = new_fg ? x->fg : x->bg;
                put(&k, seq, (size_t)snprintf(seq, sizeof(seq), "\x1b[%d;2;%u;%u;%um", new_fg ? 38 : 48,
                    (unsigned)(v >> 16), (unsigned)(v >> 8 & 255), (unsigned)(v & 255)));
            }
            if (new_fg) fg = x->fg;
            if (new_bg) bg = x->bg;
            put_utf8(&k, x->cp);
            at_r = r; at_c = c + 1;
            *was = *x;
        }
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
    puts_(&k, "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b]0;LakeShark\x07\x1b[0m\x1b[2J");
    flush(&k);
    return k.total;
}

size_t ls_vt_leave(ls_vt_out_fn out, void *ctx)
{
    char buf[64];
    sink k = {.buf = buf, .cap = sizeof(buf), .out = out, .ctx = ctx};
    puts_(&k, "\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
    flush(&k);
    return k.total;
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
        if (sscanf(params + 1, "%d;%d;%d", &btn, &x, &y) == 3 && final == 'M') {
            if (btn == 64)      key(ev, n, max, LS_TK_UP, 0);
            else if (btn == 65) key(ev, n, max, LS_TK_DOWN, 0);
            else if ((btn & ~(4 | 8 | 16)) == 0)
                event(ev, n, max, (ls_vt_event){.type = LS_VT_EV_TAP, .col = x - 1, .row = y - 1});
        }
        return end + 1;
    }
    const int p1 = params[0] >= '0' && params[0] <= '9' ? atoi(params) : 0;
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
