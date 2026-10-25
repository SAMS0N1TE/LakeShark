/* ls_vt_vt: the remote terminal's renderer and input parser.

   The renderer is checked by reading its own output back through the same
   small set of sequences it is allowed to emit (cursor position, 24-bit
   SGR, UTF-8, mode switches) and comparing the screen that results with
   the cells it was given; then that a one-cell change sends one cell. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_vt.h"
#include "ls_tui_screen.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { COLS = 12, ROWS = 4 };

static char out[1 << 16];
static size_t out_n;
static void sink(void *ctx, const char *b, size_t n) { (void)ctx; memcpy(out + out_n, b, n); out_n += n; }

static int semicolons(const char *p)
{
    int n = 0;
    while (*p) n += *p++ == ';';
    return n;
}

/* Replay the stream onto a screen of terminal cells; returns sequences it did not know. */
static int replay(const char *s, size_t n, ls_vt_cell *scr)
{
    int r = 0, c = 0, unknown = 0;
    static uint32_t fg, bg;               /* a terminal keeps its colours from one frame to the next */
    size_t i = 0;
    while (i < n) {
        const unsigned char x = (unsigned char)s[i];
        if (x == 0x1b && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < n && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
            char p[96] = {0};
            memcpy(p, s + i + 2, j - i - 2 < sizeof(p) - 1 ? j - i - 2 : sizeof(p) - 1);
            const char f = s[j];
            unsigned v[10];
            if (f == 'H' && sscanf(p, "%d;%d", &r, &c) == 2) { r--; c--; }
            else if (f == 'm' && sscanf(p, "38;2;%u;%u;%u;48;2;%u;%u;%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                fg = v[0] << 16 | v[1] << 8 | v[2]; bg = v[3] << 16 | v[4] << 8 | v[5];
            } else if (f == 'm' && semicolons(p) == 4 && sscanf(p, "%u;2;%u;%u;%u", &v[0], &v[1], &v[2], &v[3]) == 4 &&
                       (v[0] == 38 || v[0] == 48)) {
                *(v[0] == 38 ? &fg : &bg) = v[1] << 16 | v[2] << 8 | v[3];
            } else if (p[0] == '?' || f == 't' || (f == 'J' && !strcmp(p, "2")) || (f == 'm' && !strcmp(p, "0"))) {
            } else unknown++;
            i = j + 1;
            continue;
        }
        if (x == 0x1b && i + 1 < n && s[i + 1] == ']') { while (i < n && s[i] != 7) i++; i++; continue; }
        uint32_t cp = x;
        int len = 1;
        if (x >= 0xF0) { cp = x & 7; len = 4; } else if (x >= 0xE0) { cp = x & 15; len = 3; } else if (x >= 0xC0) { cp = x & 31; len = 2; }
        for (int k = 1; k < len; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 63);
        if (r >= 0 && r < ROWS && c >= 0 && c < COLS) scr[r * COLS + c] = (ls_vt_cell){cp, fg, bg};
        c++;
        i += (size_t)len;
    }
    return unknown;
}

/* Equal as seen: a space's foreground and a full block's background never show. */
static bool same_screen(const ls_vt_cell *a, const ls_vt_cell *b)
{
    for (int i = 0; i < COLS * ROWS; i++) {
        if (a[i].cp != b[i].cp) return false;
        if (a[i].cp != ' ' && a[i].fg != b[i].fg) return false;
        if (a[i].cp != 0x2588 && a[i].bg != b[i].bg) return false;
    }
    return true;
}

static void test_render(void)
{
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) pal[i] = (uint16_t)(i * 0x1084);
    pal[15] = 0xFFFF;
    pal[0] = 0x0000;
    tui_cell cells[COLS * ROWS];
    for (int i = 0; i < COLS * ROWS; i++) cells[i] = (tui_cell){'a' + i % 26, TUI_ATTR(15, 0)};
    cells[1].ch = (int16_t)(char)0x8F;                 /* full quadrant */
    cells[2].ch = (int16_t)(char)0xC1;                 /* sextant, top left only */
    cells[3].ch = (int16_t)(char)(0xC0 | 21);          /* sextant left column = left half */
    cells[4].ch = (int16_t)(char)0x91;                 /* 50% shade */
    cells[5].ch = 0x2801;                              /* braille dot 1 */
    cells[6].ch = (int16_t)(char)0xB0;                 /* image cell */
    cells[7].ch = (int16_t)(char)0xA7;                 /* trace, full height */
    cells[8].ch = (int16_t)(char)0xA3;                 /* trace, half */
    cells[9].ch = (int16_t)(char)0xA0;                 /* trace, an eighth */
    uint32_t top[COLS * ROWS] = {0}, bot[COLS * ROWS] = {0};
    top[6] = 0x112233; bot[6] = 0x445566;
    ls_vt_frame f = {.cols = COLS, .rows = ROWS, .cells = cells, .palette = pal,
                       .image = {6, 0, 1, 1}, .img_top = top, .img_bot = bot};
    ls_vt_cell now[COLS * ROWS], shown[COLS * ROWS], scr[COLS * ROWS];
    ls_vt_resolve(&f, false, now);
    CHECK(now[0].cp == 'a' && now[0].fg == 0xFFFFFF && now[0].bg == 0, "plain cell %x %x %x", now[0].cp, now[0].fg, now[0].bg);
    CHECK(now[1].cp == 0x2588, "quadrant %x", now[1].cp);
    CHECK(now[2].cp == 0x1FB00, "sextant %x", now[2].cp);
    CHECK(now[3].cp == 0x258C, "left half %x", now[3].cp);
    CHECK(now[4].cp == ' ' && now[4].bg == 0x7F7F7F, "shade %x bg %06x", now[4].cp, now[4].bg);
    CHECK(now[5].cp == 0x2801, "braille %x", now[5].cp);
    CHECK(now[6].cp == 0x2580 && now[6].fg == 0x112233 && now[6].bg == 0x445566, "image %x", now[6].cp);
    /* The device draws traces upright, so a column of them is a bar, not dashes. */
    CHECK(now[7].cp == 0x2502 && now[8].cp == 0x2577 && now[9].cp == '.',
          "traces %x %x %x", now[7].cp, now[8].cp, now[9].cp);
    ls_vt_resolve(&f, true, now);
    CHECK(now[2].cp == 0x2598, "quadrant fallback %x", now[2].cp);
    ls_vt_resolve(&f, false, now);

    ls_vt_screen s;
    ls_vt_screen_init(&s, COLS, ROWS, shown);
    out_n = 0;
    ls_vt_enter(sink, NULL, COLS, ROWS);
    ls_vt_render(&s, now, sink, NULL);
    memset(scr, 0, sizeof(scr));
    CHECK(replay(out, out_n, scr) == 0, "unknown sequences in the stream");
    CHECK(same_screen(scr, now), "replayed screen differs from the cells");

    out_n = 0;
    CHECK(ls_vt_render(&s, now, sink, NULL) == 0, "an unchanged frame sent %u bytes", (unsigned)out_n);
    cells[COLS * 2 + 3].ch = 'Z';
    ls_vt_resolve(&f, false, now);
    out_n = 0;
    ls_vt_render(&s, now, sink, NULL);
    CHECK(out_n < 80, "one changed cell sent %u bytes", (unsigned)out_n);
    CHECK(replay(out, out_n, scr) == 0 && same_screen(scr, now), "diff frame did not reproduce the cells");

    /* Only the colour that changed goes out, and one nobody can see does not. */
    cells[COLS * 3 + 1].attr = TUI_ATTR(7, 0);
    ls_vt_resolve(&f, false, now);
    out_n = 0;
    ls_vt_render(&s, now, sink, NULL);
    out[out_n] = 0;
    CHECK(out_n < 60 && !strstr(out, "48;2"), "a foreground change sent %u bytes, background too", (unsigned)out_n);
    CHECK(replay(out, out_n, scr) == 0 && same_screen(scr, now), "foreground change did not reproduce");
    cells[COLS * 3 + 2].ch = ' ';
    ls_vt_resolve(&f, false, now);
    ls_vt_render(&s, now, sink, NULL);
    cells[COLS * 3 + 2].attr = TUI_ATTR(9, 0);
    ls_vt_resolve(&f, false, now);
    out_n = 0;
    CHECK(ls_vt_render(&s, now, sink, NULL) == 0, "a space's foreground sent %u bytes", (unsigned)out_n);
}

static int feed(ls_vt_input *in, const char *s, ls_vt_event *ev)
{
    return ls_vt_input_feed(in, (const uint8_t *)s, strlen(s), ev, 16);
}

static void test_input(void)
{
    ls_vt_input in = {0};
    ls_vt_event ev[16];
    int n = feed(&in, "a\r\n\t\x7f", ev);
    CHECK(n == 4 && ev[0].key == LS_TK_CHAR && ev[0].ch == 'a' && ev[1].key == LS_TK_ENTER &&
          ev[2].key == LS_TK_TAB && ev[3].key == LS_TK_BACKSPACE, "plain keys: %d events", n);
    n = feed(&in, "\x1b[A\x1bOP\x1b[15~\x1b[23~", ev);
    CHECK(n == 4 && ev[0].key == LS_TK_UP && ev[1].key == LS_TK_F1 && ev[2].key == LS_TK_F5 && ev[3].key == LS_TK_F11,
          "arrows and function keys: %d", n);
    n = feed(&in, "\x1b[<0;10;5M\x1b[<0;10;5m\x1b[<64;1;1M", ev);
    CHECK(n == 3 && ev[0].type == LS_VT_EV_TAP && ev[0].col == 9 && ev[0].row == 4 &&
          ev[1].type == LS_VT_EV_UP && ev[1].col == 9 &&
          ev[2].type == LS_VT_EV_KEY && ev[2].key == LS_TK_UP, "mouse: %d", n);
    n = feed(&in, "\x1b[<0;3;3M\x1b[<32;7;4M\x1b[<35;8;4M\x1b[<34;8;4M\x1b[<0;7;4m\x1b[<2;7;4m", ev);
    CHECK(n == 3 && ev[0].type == LS_VT_EV_TAP && ev[1].type == LS_VT_EV_DRAG && ev[1].col == 6 && ev[1].row == 3 &&
          ev[2].type == LS_VT_EV_UP, "drag: held motion only, left release only: %d", n);
    n = feed(&in, "\x1b[", ev);
    CHECK(n == 0, "half a sequence gave %d events", n);
    n = feed(&in, "B", ev);
    CHECK(n == 1 && ev[0].key == LS_TK_DOWN, "sequence split across reads");
    n = feed(&in, "\x1b", ev);
    CHECK(n == 0, "lone escape waits");
    n = ls_vt_input_idle(&in, ev, 16);
    CHECK(n == 1 && ev[0].key == LS_TK_ESC, "lone escape after a pause is Escape");
    n = feed(&in, "\x1b\x1b[0n", ev);            /* the web screen's Escape */
    CHECK(n == 1 && ev[0].key == LS_TK_ESC && in.n == 0, "Escape with a no-op report after it is Escape at once");
    n = feed(&in, "\x1d", ev);
    CHECK(n == 1 && ev[0].type == LS_VT_EV_QUIT, "Ctrl-] quits");
    n = feed(&in, "\x0c", ev);
    CHECK(n == 1 && ev[0].type == LS_VT_EV_REDRAW, "Ctrl-L repaints");
    n = feed(&in, "\x1b[<65;3;4M", ev);
    CHECK(n == 1 && ev[0].type == LS_VT_EV_KEY && ev[0].key == LS_TK_DOWN && ev[0].wheel, "the wheel is a key, marked");
    n = feed(&in, "\x1b[B", ev);
    CHECK(n == 1 && !ev[0].wheel, "an arrow is not the wheel");
    /* The answer to ls_vt_ask_size is a size, only when one was asked for:
       otherwise CSI 1;2 R is Shift-F3. */
    in.want_size = true;
    n = feed(&in, "\x1b[27;52R", ev);
    CHECK(n == 1 && ev[0].type == LS_VT_EV_SIZE && ev[0].col == 52 && ev[0].row == 27 && !in.want_size,
          "cursor report: %d events, %dx%d", n, ev[0].col, ev[0].row);
    n = feed(&in, "\x1b[1;2R", ev);
    CHECK(n == 1 && ev[0].type == LS_VT_EV_KEY && ev[0].key == LS_TK_F3, "F3 once the report is in");
}

/* --- a phone: a terminal smaller than the grid --------------------------- */

enum { GC = 52, GR = 70 };                 /* a T-Display-P4 in portrait */

/* Replay onto a tc x tr terminal that, like Termius on iOS, ignores a cursor
   position outside its window and wraps (and so scrolls) when written past
   its right edge. Anything that would have done either counts in *bad. */
static void replay_term(const char *s, size_t n, int tc, int tr, ls_vt_cell *scr, int *bad)
{
    int r = 0, c = 0;
    uint32_t fg = 0, bg = 0;
    size_t i = 0;
    while (i < n) {
        const unsigned char x = (unsigned char)s[i];
        if (x == 0x1b && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < n && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
            char p[96] = {0};
            memcpy(p, s + i + 2, j - i - 2 < sizeof(p) - 1 ? j - i - 2 : sizeof(p) - 1);
            const char f = s[j];
            unsigned v[6];
            int pr = 0, pc = 0;
            if (f == 'H' && sscanf(p, "%d;%d", &pr, &pc) == 2) {
                if (pr < 1 || pc < 1 || pr > tr || pc > tc) (*bad)++;
                else { r = pr - 1; c = pc - 1; }
            } else if (f == 'm' && sscanf(p, "38;2;%u;%u;%u;48;2;%u;%u;%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                fg = v[0] << 16 | v[1] << 8 | v[2]; bg = v[3] << 16 | v[4] << 8 | v[5];
            } else if (f == 'm' && sscanf(p, "%u;2;%u;%u;%u", &v[0], &v[1], &v[2], &v[3]) == 4) {
                *(v[0] == 38 ? &fg : &bg) = v[1] << 16 | v[2] << 8 | v[3];
            } else if (f == 'J') {
                for (int k = 0; k < tc * tr; k++) scr[k] = (ls_vt_cell){' ', 0, 0};
            } else if (f == 'X' && c < tc) {
                scr[r * tc + c] = (ls_vt_cell){' ', fg, bg};
            }
            i = j + 1;
            continue;
        }
        if (x == 0x1b && i + 1 < n && s[i + 1] == ']') { while (i < n && s[i] != 7) i++; i++; continue; }
        uint32_t cp = x;
        int len = 1;
        if (x >= 0xF0) { cp = x & 7; len = 4; } else if (x >= 0xE0) { cp = x & 15; len = 3; } else if (x >= 0xC0) { cp = x & 31; len = 2; }
        for (int k = 1; k < len; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 63);
        if (c >= tc || (r == tr - 1 && c == tc - 1)) (*bad)++;   /* a wrap, or the last cell: a scroll */
        else scr[r * tc + c] = (ls_vt_cell){cp, fg, bg};
        c++;
        i += (size_t)len;
    }
}

/* The terminal's rows 0..h-1 are the grid's from y0, columns from x0. */
static bool shows(const ls_vt_cell *scr, int tc, const ls_vt_cell *grid, int x0, int y0, int w, int h)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            if (scr[r * tc + c].cp != grid[(y0 + r) * GC + x0 + c].cp) return false;
    return true;
}

static void row_text(const ls_vt_cell *scr, int tc, int r, char *t)
{
    for (int c = 0; c < tc; c++) t[c] = scr[r * tc + c].cp < 0x80 ? (char)scr[r * tc + c].cp : '#';
    t[tc] = 0;
}

static void test_phone(void)
{
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) pal[i] = (uint16_t)(i * 0x1084);
    static tui_cell cells[GC * GR];
    for (int r = 0; r < GR; r++)
        for (int c = 0; c < GC; c++)
            cells[r * GC + c] = (tui_cell){'a' + (r * 7 + c) % 26, TUI_ATTR(15, r ? 0 : 5)};
    cells[GC * 40 + 3].ch = (int16_t)(char)0xC1;          /* a sextant, top left only */
    ls_vt_frame f = {.cols = GC, .rows = GR, .cells = cells, .palette = pal};
    static ls_vt_cell now[GC * GR], shown[GC * GR], scr[GC * GR];
    ls_vt_resolve(&f, false, now);

    /* The case on the phone: the board asks for 52 x 70, Termius has 52 x 27. */
    ls_vt_screen s;
    ls_vt_screen_init(&s, GC, GR, shown);
    CHECK(ls_vt_screen_resize(&s, 52, 27), "a smaller window changes what shows");
    int w = 0, h = 0, bad = 0;
    ls_vt_screen_view(&s, &w, &h);
    CHECK(w == 52 && h == 26, "view %dx%d", w, h);
    out_n = 0;
    ls_vt_enter(sink, NULL, GC, GR);
    ls_vt_render(&s, now, sink, NULL);
    replay_term(out, out_n, 52, 27, scr, &bad);
    CHECK(bad == 0, "%d writes outside a 52x27 window", bad);
    CHECK(shows(scr, 52, now, 0, 0, 52, 26), "the top of the grid is not what the phone shows");
    char t[64];
    row_text(scr, 52, 26, t);
    CHECK(strstr(t, "^ UP") && strstr(t, "1-26/70") && strstr(t, "v DOWN"), "bar '%s'", t);
    CHECK(scr[26 * 52].bg == now[0].bg && scr[26 * 52 + 51].bg == now[0].bg, "the bar is not in the status bar's colours");

    /* Taps above the bar are the grid's; DOWN on the bar moves half a view. */
    int gc = -1, gr = -1;
    CHECK(ls_vt_screen_tap(&s, 5, 3, &gc, &gr) && gc == 5 && gr == 3, "a tap at the top went to %d,%d", gc, gr);
    CHECK(!ls_vt_screen_tap(&s, 50, 26, &gc, &gr) && s.y0 == 13, "DOWN left the view at row %d", s.y0);
    CHECK(ls_vt_screen_tap(&s, 5, 3, &gc, &gr) && gc == 5 && gr == 16, "a tap after moving went to %d,%d", gc, gr);
    out_n = 0;
    bad = 0;
    ls_vt_render(&s, now, sink, NULL);
    replay_term(out, out_n, 52, 27, scr, &bad);
    CHECK(bad == 0 && shows(scr, 52, now, 0, 13, 52, 26), "after DOWN: %d bad, or the wrong rows", bad);
    row_text(scr, 52, 26, t);
    CHECK(strstr(t, "14-39/70") != NULL, "bar after DOWN '%s'", t);

    /* The wheel's pan stops at the bottom; what is out of view sends nothing. */
    ls_vt_screen_pan(&s, 0, 100);
    CHECK(s.y0 == GR - 26, "the view went past the bottom: %d", s.y0);
    out_n = 0;
    bad = 0;
    ls_vt_render(&s, now, sink, NULL);
    replay_term(out, out_n, 52, 27, scr, &bad);
    CHECK(bad == 0 && shows(scr, 52, now, 0, 44, 52, 26), "at the bottom: %d bad, or the wrong rows", bad);
    cells[GC * 2 + 4].ch = 'Z';
    ls_vt_resolve(&f, false, now);
    out_n = 0;
    CHECK(ls_vt_render(&s, now, sink, NULL) == 0, "a change out of view sent %u bytes", (unsigned)out_n);
    cells[GC * 50 + 4].ch = 'Z';
    ls_vt_resolve(&f, false, now);
    out_n = 0;
    bad = 0;
    ls_vt_render(&s, now, sink, NULL);
    replay_term(out, out_n, 52, 27, scr, &bad);
    CHECK(out_n < 80 && bad == 0 && scr[6 * 52 + 4].cp == 'Z', "a change in view: %u bytes, %d bad", (unsigned)out_n, bad);

    /* Narrower than the grid too: LEFT and RIGHT join the bar. */
    CHECK(ls_vt_screen_resize(&s, 45, 30), "a new size");
    ls_vt_screen_view(&s, &w, &h);
    CHECK(w == 45 && h == 29 && s.y0 == GR - 29, "view %dx%d from row %d", w, h, s.y0);
    out_n = 0;
    bad = 0;
    ls_vt_render(&s, now, sink, NULL);
    replay_term(out, out_n, 45, 30, scr, &bad);
    CHECK(bad == 0 && shows(scr, 45, now, 0, GR - 29, 45, 29), "45x30: %d bad, or the wrong cells", bad);
    row_text(scr, 45, 29, t);
    CHECK(t[strspn(t, " ")] == '<' && strchr(t, '>') && strchr(t, '^'), "narrow bar '%s'", t);
    CHECK(!ls_vt_screen_tap(&s, 44, 29, &gc, &gr) && s.x0 == GC - 45, "RIGHT left x0 at %d", s.x0);
    CHECK(ls_vt_screen_tap(&s, 0, 0, &gc, &gr) && gc == GC - 45, "a tap after RIGHT went to column %d", gc);

    /* Sextants folded for a font that may lack them. */
    s.quadrants = true;
    ls_vt_screen_pan(&s, -100, -100);
    ls_vt_screen_pan(&s, 0, 30);
    out_n = 0;
    ls_vt_render(&s, now, sink, NULL);
    out[out_n] = 0;
    CHECK(!strstr(out, "\xf0\x9f\xac") && strstr(out, "\xe2\x96\x98"), "a sextant went out as itself");

    /* Big enough, or no size given: the whole grid, as before, and no bar. */
    CHECK(ls_vt_screen_resize(&s, 80, 100) && s.x0 == 0 && s.y0 == 0, "a bigger window");
    ls_vt_screen_view(&s, &w, &h);
    CHECK(w == GC && h == GR, "view %dx%d in a big window", w, h);
    CHECK(ls_vt_screen_tap(&s, 51, 69, &gc, &gr) && gc == 51 && gr == 69, "a tap in a big window");
    CHECK(!ls_vt_screen_tap(&s, 51, 70, &gc, &gr), "a tap under the grid");
    CHECK(ls_vt_screen_resize(&s, 0, 0) && !ls_vt_screen_resize(&s, 0, 0), "back to no size");
}

static void test_telnet(void)
{
    ls_vt_telnet t = {0};
    /* Data around IAC WILL NAWS and its subnegotiation; IAC IAC is a 255. */
    uint8_t b[] = {'a', 0xFF, 0xFB, 0x1F, 0xFF, 0xFA, 0x1F, 0, 52, 0, 27, 0xFF, 0xF0, 'b', 0xFF, 0xFF};
    size_t n = ls_vt_telnet_strip(&t, b, sizeof(b));
    CHECK(n == 3 && b[0] == 'a' && b[1] == 'b' && b[2] == 0xFF, "telnet left %u bytes", (unsigned)n);
    CHECK(t.sized && t.cols == 52 && t.rows == 27, "NAWS %dx%d", t.cols, t.rows);
    /* A size of 255, doubled, cut into single bytes. */
    const uint8_t c[] = {0xFF, 0xFA, 0x1F, 0x00, 0xFF, 0xFF, 0x00, 0x28, 0xFF, 0xF0, 'x'};
    size_t kept = 0;
    for (size_t i = 0; i < sizeof(c); i++) {
        uint8_t one = c[i];
        kept += ls_vt_telnet_strip(&t, &one, 1);
    }
    CHECK(kept == 1 && t.cols == 255 && t.rows == 40, "split NAWS %dx%d, %u kept", t.cols, t.rows, (unsigned)kept);
}

int main(void)
{
    test_render();
    test_input();
    test_phone();
    test_telnet();
    printf("test_term_vt: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
