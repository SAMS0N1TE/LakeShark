/* Box lines and shapes as procedural cells (ls_box.h): every one draws at
   every face, its arms reach the cell edges where lines must join, a dump
   prints a readable stand-in, and the terminal text path sends the
   codepoint (or the three substitutes) so lsterm shows them. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_box.h"
#include "ls_vt.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const int SIZES[][2] = { {10, 17}, {9, 16}, {15, 26}, {8, 14}, {7, 13} };

static int ink(const ls_box_mask_t *m)
{
    int n = 0;
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++) n += (m->r[y] >> x) & 1;
    return n;
}
static bool at(const ls_box_mask_t *m, int x, int y) { return (m->r[y] >> x) & 1; }

static bool edge(const ls_box_mask_t *m, int side)
{
    /* 0 top, 1 right, 2 bottom, 3 left */
    for (int i = 0; i < (side & 1 ? m->h : m->w); i++) {
        const int x = side == 1 ? m->w - 1 : side == 3 ? 0 : i;
        const int y = side == 0 ? 0 : side == 2 ? m->h - 1 : i;
        if (at(m, x, y)) return true;
    }
    return false;
}

static void every_glyph_draws_and_has_a_stand_in(void)
{
    int codes[160], n = 0;
    for (int cp = 0x2500; cp <= 0x257F; cp++) codes[n++] = cp;
    for (int i = 0; i < LS_BOX_SHAPE_N; i++) codes[n++] = LS_BOX_SHAPES[i];
    bool used[LS_BOX_ATLAS_GLYPHS] = {0};
    for (int i = 0; i < n; i++) {
        const int cp = codes[i];
        CHECK(ls_box_is(cp), "U+%04X is not a box glyph", cp);
        const char a = ls_box_ascii(cp);
        CHECK(a > 0x20 && a < 0x7F, "U+%04X stands in as %d", cp, a);
        const int g = ls_box_atlas(cp);
        CHECK(g >= LS_BOX_ATLAS_FIRST && g < LS_BOX_ATLAS_GLYPHS && !used[g], "U+%04X atlas slot %d", cp, g);
        if (g >= 0 && g < LS_BOX_ATLAS_GLYPHS) used[g] = true;
        for (unsigned s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); s++) {
            ls_box_mask_t m;
            CHECK(ls_box_mask(cp, SIZES[s][0], SIZES[s][1], &m), "U+%04X no mask", cp);
            CHECK(ink(&m) > 0, "U+%04X draws nothing at %dx%d", cp, SIZES[s][0], SIZES[s][1]);
            for (int y = SIZES[s][1]; y < 32; y++) CHECK(!m.r[y], "U+%04X ink below the cell", cp);
            for (int y = 0; y < SIZES[s][1]; y++) CHECK(!(m.r[y] >> SIZES[s][0]), "U+%04X ink right of the cell", cp);
        }
    }
    CHECK(!ls_box_is('A') && !ls_box_is(0x2800) && !ls_box_is(0x00D7), "non-box codes claimed");
    ls_box_mask_t m;
    CHECK(!ls_box_mask(0x2800, 10, 17, &m), "braille is not a box glyph");
}

/* Arms reach the edge they name and no other, so a frame of these joins. */
static void arms_meet_the_edges(void)
{
    for (int cp = 0x2500; cp <= 0x257F; cp++) {
        if (cp >= LS_BOX_DIAG_R && cp <= LS_BOX_DIAG_X) continue;
        const uint16_t a = LS_BOX_ARMS[cp - 0x2500];
        const int want[4] = {a >> 12 & 15, a >> 8 & 15, a >> 4 & 15, a & 15};   /* U R D L */
        if (want[0] >= 4 || want[1] >= 4) continue;            /* dashes stop short on purpose */
        for (unsigned s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); s++) {
            ls_box_mask_t m;
            ls_box_mask(cp, SIZES[s][0], SIZES[s][1], &m);
            for (int side = 0; side < 4; side++)
                CHECK(edge(&m, side) == (want[side] != 0), "U+%04X at %dx%d side %d", cp, SIZES[s][0], SIZES[s][1], side);
        }
    }
    /* The light frame: one pixel through the middle. */
    ls_box_mask_t m;
    ls_box_mask(LS_BOX_H, 10, 17, &m);
    CHECK(m.r[8] == 0x3FF && ink(&m) == 10, "light horizontal");
    ls_box_mask(LS_BOX_V, 10, 17, &m);
    CHECK(ink(&m) == 17 && at(&m, 5, 0) && at(&m, 5, 16), "light vertical");
    ls_box_mask(LS_BOX_DBL_H, 10, 17, &m);
    CHECK(m.r[7] == 0x3FF && m.r[9] == 0x3FF && !m.r[8], "double horizontal is two lines");
    ls_box_mask(LS_BOX_DBL_DR, 10, 17, &m);
    CHECK(at(&m, 4, 7) && at(&m, 9, 7) && at(&m, 4, 16) && at(&m, 6, 9) && !at(&m, 4, 0) && !at(&m, 0, 7),
          "double top-left corner closes");
}

static void dumps_read_as_ascii(void)
{
    static const struct { int cp; char c; } T[] = {
        {LS_BOX_H, '-'}, {LS_BOX_V, '|'}, {LS_BOX_DR, '+'}, {LS_BOX_UL, '+'}, {LS_BOX_VH, '+'},
        {LS_BOX_DBL_H, '='}, {LS_BOX_DBL_V, '|'}, {LS_BOX_DBL_DR, '+'}, {LS_BOX_DASH_H, '-'},
        {LS_BOX_DASH_V, ':'}, {LS_BOX_DIAG_R, '/'}, {LS_BOX_DIAG_L, '\\'}, {LS_BOX_DIAG_X, 'X'},
        {LS_SHAPE_UP, '^'}, {LS_SHAPE_DOWN, 'v'}, {LS_SHAPE_LEFT, '<'}, {LS_SHAPE_RIGHT, '>'},
        {LS_SHAPE_DOT, 'o'}, {LS_SHAPE_TIMES, 'x'}, {LS_SHAPE_SQUARE, 'O'}, {LS_BOX_HALF_L, '-'},
    };
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        CHECK(ls_box_ascii(T[i].cp) == T[i].c, "U+%04X dumps as '%c', want '%c'", T[i].cp, ls_box_ascii(T[i].cp), T[i].c);
}

static char out[1 << 15];
static size_t out_n;
static void sink(void *ctx, const char *b, size_t n) { (void)ctx; if (out_n + n < sizeof(out)) { memcpy(out + out_n, b, n); out_n += n; } }

static void the_terminal_gets_the_codepoints(void)
{
    enum { C = 9, R = 1 };
    const int16_t cps[C] = {LS_BOX_DR, LS_BOX_H, LS_BOX_DL, LS_SHAPE_TIMES, LS_SHAPE_CHECK,
                            LS_SHAPE_RESTORE, LS_SHAPE_UP, 0x2847, 'A'};
    const uint32_t want[C] = {0x250C, 0x2500, 0x2510, 0x00D7, 0x221A, 0x25A3, 0x25B2, 0x2847, 'A'};
    tui_cell cells[C];
    for (int i = 0; i < C; i++) cells[i] = (tui_cell){cps[i], TUI_ATTR(TUI_WHITE, TUI_BLACK)};
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) pal[i] = (uint16_t)(i * 0x0841);
    ls_vt_frame f = {.cols = C, .rows = R, .cells = cells, .palette = pal};
    ls_vt_cell res[C];
    ls_vt_resolve(&f, false, res);
    for (int i = 0; i < C; i++) CHECK(res[i].cp == want[i], "cell %d sent U+%04X, want U+%04X", i, res[i].cp, want[i]);
    ls_vt_cell shown[C];
    ls_vt_screen scr;
    ls_vt_screen_init(&scr, C, R, shown);
    out_n = 0;
    ls_vt_render(&scr, res, sink, NULL);
    out[out_n] = 0;
    CHECK(strstr(out, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90") != NULL, "the frame is not in the text as UTF-8");
    CHECK(strstr(out, "\xc3\x97") != NULL, "the cross is not U+00D7");
}

int main(void)
{
    every_glyph_draws_and_has_a_stand_in();
    arms_meet_the_edges();
    dumps_read_as_ascii();
    the_terminal_gets_the_codepoints();
    if (fails) { printf("test_tui_box_glyphs: %d failed\n", fails); return 1; }
    printf("test_tui_box_glyphs: ok\n");
    return 0;
}
