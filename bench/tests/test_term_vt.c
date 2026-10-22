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
    CHECK(n == 2 && ev[0].type == LS_VT_EV_TAP && ev[0].col == 9 && ev[0].row == 4 &&
          ev[1].type == LS_VT_EV_KEY && ev[1].key == LS_TK_UP, "mouse: %d", n);
    n = feed(&in, "\x1b[", ev);
    CHECK(n == 0, "half a sequence gave %d events", n);
    n = feed(&in, "B", ev);
    CHECK(n == 1 && ev[0].key == LS_TK_DOWN, "sequence split across reads");
    n = feed(&in, "\x1b", ev);
    CHECK(n == 0, "lone escape waits");
    n = ls_vt_input_idle(&in, ev, 16);
    CHECK(n == 1 && ev[0].key == LS_TK_ESC, "lone escape after a pause is Escape");
    n = feed(&in, "\x1d", ev);
    CHECK(n == 1 && ev[0].type == LS_VT_EV_QUIT, "Ctrl-] quits");
    n = feed(&in, "\x0c", ev);
    CHECK(n == 1 && ev[0].type == LS_VT_EV_REDRAW, "Ctrl-L repaints");
}

int main(void)
{
    test_render();
    test_input();
    printf("test_term_vt: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
