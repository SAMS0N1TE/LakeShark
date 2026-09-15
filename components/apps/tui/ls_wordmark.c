/* See ls_wordmark.h. The font moved here from compact_ui.cpp
   unchanged; the width and the split are new. */
#include "ls_wordmark.h"

#include <stdio.h>
#include <string.h>

/* Five-row block capitals built from cells, so the logo costs nothing
   that a normal screen does not already cost and needs no image asset, no
   decoder and no PSRAM. */
static const char *const BLOCK[26][LS_WORDMARK_ROWS] = {
 {" XX ","X  X","XXXX","X  X","X  X"},{"XXX ","X  X","XXX ","X  X","XXX "},
 {" XXX","X   ","X   ","X   "," XXX"},{"XXX ","X  X","X  X","X  X","XXX "},
 {"XXXX","X   ","XXX ","X   ","XXXX"},{"XXXX","X   ","XXX ","X   ","X   "},
 {" XXX","X   ","X XX","X  X"," XXX"},{"X  X","X  X","XXXX","X  X","X  X"},
 {"XXX "," X  "," X  "," X  ","XXX "},{"   X","   X","   X","X  X"," XX "},
 {"X  X","X X ","XX  ","X X ","X  X"},{"X   ","X   ","X   ","X   ","XXXX"},
 {"X  X","XXXX","XXXX","X  X","X  X"},{"X  X","XX X","X XX","X  X","X  X"},
 {" XX ","X  X","X  X","X  X"," XX "},{"XXX ","X  X","XXX ","X   ","X   "},
 {" XX ","X  X","X  X","X XX"," XXX"},{"XXX ","X  X","XXX ","X X ","X  X"},
 {" XXX","X   "," XX ","   X","XXX "},{"XXXXX","  X  ","  X  ","  X  ","  X  "},
 {"X  X","X  X","X  X","X  X"," XX "},{"X  X","X  X","X  X"," XX ","  X "},
 {"X  X","X  X","XXXX","XXXX","X  X"},{"X  X"," XX "," XX ","X  X","X  X"},
 {"X  X","X  X"," XX ","  X ","  X "},{"XXXX","   X"," XX ","X   ","XXXX"},
};

/* Three columns per letter, including T, for the 34-column portrait grid. */
static const char *const COMPACT[26][LS_WORDMARK_ROWS] = {
 {" X ","X X","XXX","X X","X X"},{"XX ","X X","XX ","X X","XX "},
 {" XX","X  ","X  ","X  "," XX"},{"XX ","X X","X X","X X","XX "},
 {"XXX","X  ","XX ","X  ","XXX"},{"XXX","X  ","XX ","X  ","X  "},
 {" XX","X  ","X X","X X"," XX"},{"X X","X X","XXX","X X","X X"},
 {"XXX"," X "," X "," X ","XXX"},{"  X","  X","  X","X X"," X "},
 {"X X","X X","XX ","X X","X X"},{"X  ","X  ","X  ","X  ","XXX"},
 {"X X","XXX","XXX","X X","X X"},{"X X","XXX","XXX","XXX","X X"},
 {" X ","X X","X X","X X"," X "},{"XX ","X X","XX ","X  ","X  "},
 {" X ","X X","X X","XXX"," XX"},{"XX ","X X","XX ","X X","X X"},
 {" XX","X  "," X ","  X","XX "},{"XXX"," X "," X "," X "," X "},
 {"X X","X X","X X","X X","XXX"},{"X X","X X","X X","X X"," X "},
 {"X X","X X","XXX","XXX","X X"},{"X X","X X"," X ","X X","X X"},
 {"X X","X X"," X "," X "," X "},{"XXX","  X"," X ","X  ","XXX"},
};

/* A space, and the fallback for anything not a letter. */
#define SPACE_W 3

/* Width of one glyph: the widest of its five rows. T is five columns and
   every other letter is four, which is the whole reason this is measured. */
static int glyph_width(char c, bool compact)
{
    const int idx = (c | 0x20) - 'a';
    if (c == ' ' || idx < 0 || idx > 25) return SPACE_W;
    if (compact) return 3;
    int wide = 0;
    for (int r = 0; r < LS_WORDMARK_ROWS; r++) {
        const int len = (int)strlen(BLOCK[idx][r]);
        if (len > wide) wide = len;
    }
    return wide;
}

static int width(const char *text, bool compact)
{
    if (!text || !*text) return 0;
    int w = 0;
    for (const char *p = text; *p; p++) {
        const int gw = glyph_width(*p, compact);
        /* A letter carries one column of air after it; a space does not,
           because it IS the air. Matching ls_wordmark_row exactly - a width
           that disagrees with the renderer by one column per letter is worse
           than no width at all. */
        w += (*p == ' ') ? SPACE_W : gw + 1;
    }
    /* The trailing column of air after the last letter is not part of the
       mark, so a centred string is centred on its ink. */
    if (text[strlen(text) - 1] != ' ') w -= 1;
    return w;
}

static bool split(const char *text, int cols, bool compact,
                       char *a, size_t a_cap, char *b, size_t b_cap)
{
    if (a && a_cap) a[0] = 0;
    if (b && b_cap) b[0] = 0;
    if (!text || !a || !a_cap) return false;

    if (width(text, compact) <= cols) {
        snprintf(a, a_cap, "%s", text);
        return false;
    }

    /* The last space whose left half still fits. Walking from the right
       keeps as much as possible on the first line, which is what stops a
       two-word mark splitting after its first letter when neither
       arrangement is comfortable. */
    const size_t n = strlen(text);
    for (size_t i = n; i-- > 0; ) {
        if (text[i] != ' ') continue;
        char head[64];
        if (i >= sizeof(head)) continue;
        memcpy(head, text, i);
        head[i] = 0;
        if (width(head, compact) > cols) continue;
        if (!b || !b_cap) continue;
        if (width(text + i + 1, compact) > cols) continue;
        snprintf(a, a_cap, "%s", head);
        snprintf(b, b_cap, "%s", text + i + 1);
        return true;
    }

    /* No split helps. Hand back the whole thing and let the renderer clip
       it: one visibly cut word says "this does not fit here", which is
       true, where an empty panel says nothing at all. */
    snprintf(a, a_cap, "%s", text);
    return false;
}

static void draw_row(tui_surface *sf, tui_rect clip, int x, int y, int row, bool compact,
                     const char *text, uint8_t attr)
{
    if (!sf || !text || row < 0 || row >= LS_WORDMARK_ROWS) return;
    int cx = x;
    for (const char *p = text; *p; p++) {
        const int idx = (*p | 0x20) - 'a';
        if (*p == ' ' || idx < 0 || idx > 25) { cx += SPACE_W; continue; }
        const char *pat = compact ? COMPACT[idx][row] : BLOCK[idx][row];
        for (int c = 0; pat[c]; c++)
            if (pat[c] == 'X')
                tui_put_char(sf, clip, cx + c, y + row, LS_TUI_SHADE_FULL,
                             attr);
        cx += glyph_width(*p, compact) + 1;
    }
}

int ls_wordmark_width(const char *text) { return width(text, false); }
int ls_wordmark_compact_width(const char *text) { return width(text, true); }
bool ls_wordmark_split(const char *text, int cols, char *a, size_t ac, char *b, size_t bc)
{ return split(text, cols, false, a, ac, b, bc); }
bool ls_wordmark_compact_split(const char *text, int cols, char *a, size_t ac, char *b, size_t bc)
{ return split(text, cols, true, a, ac, b, bc); }
void ls_wordmark_row(tui_surface *sf, tui_rect clip, int x, int y, int row,
                     const char *text, uint8_t attr)
{ draw_row(sf, clip, x, y, row, false, text, attr); }
void ls_wordmark_compact_row(tui_surface *sf, tui_rect clip, int x, int y, int row,
                             const char *text, uint8_t attr)
{ draw_row(sf, clip, x, y, row, true, text, attr); }
