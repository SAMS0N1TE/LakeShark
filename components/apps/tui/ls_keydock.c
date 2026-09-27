#include "ls_keydock.h"

#include <string.h>
#include "esp_attr.h"
#include "ls_tui_ui.h"

#define COLS    10
#define KEY_MIN 3
#define KEY_MAX 6        /* rows a key may be, gap included */
#define MIN_TEXT_ROWS 6  /* what the editor keeps above the dock */

enum { LOWER, UPPER, SYM, MORE, LAYERS };
static const char *const ROW0[LAYERS] = { "qwertyuiop", "QWERTYUIOP", "1234567890", "[]{}<>\\^|`" };
static const char *const ROW1[LAYERS] = { "asdfghjkl.", "ASDFGHJKL?", "@#$%&*()-_", "0123456789" };
static const char *const ROW2[LAYERS] = { "zxcvbnm,",   "ZXCVBNM!",   "+=/:;'\"~",   ".,!?:;\"'" };

/* Slots: 0-9 row 0, 10-19 row 1, 20 shift, 21-28 row 2, 29 delete, then
   the action row. */
enum { S_SHIFT = 20, S_DEL = 29, S_LAYER = 30, S_LEFT, S_RIGHT, S_SPACE,
       S_UP, S_DOWN, S_ENTER, N_SLOTS };

EXT_RAM_BSS_ATTR static tui_rect s_hit[N_SLOTS];
static int s_layer = UPPER, s_flash = -1, s_flash_frames;
static bool s_once = true, s_drawn;

void ls_keydock_reset(bool capital) { s_layer = capital ? UPPER : LOWER; s_once = capital; }

/* Rows per key row: as tall as a finger wants, taking up to about 45% of
   the area, and never so much that the text above has no room. */
static int key_rows(tui_rect area)
{
    int kh = area.h * 45 / 100 / 4;
    if (kh < KEY_MIN) kh = KEY_MIN;       /* a short landscape body still gets keys */
    if (kh > KEY_MAX) kh = KEY_MAX;
    while (kh >= KEY_MIN && area.h - kh * 4 < MIN_TEXT_ROWS) kh--;
    return kh;
}

int ls_keydock_height(tui_rect area)
{
    const int kh = key_rows(area);
    if (kh < KEY_MIN || area.w < COLS * 3) return 0;
    return kh * 4;
}

static void key(tui_surface *sf, tui_rect a, const char *label, char fig, uint8_t hue, int slot, bool lit)
{
    s_hit[slot] = a;
    const bool flash = slot == s_flash && s_flash_frames > 0;
    ls_fill_dither(sf, a, flash ? LS_DITHER_HEAVY : lit ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT,
                   flash ? TUI_GREEN | TUI_BRIGHT : hue);
    char one[2] = { fig, 0 };
    ls_dither_label(sf, a, (a.h - 1) / 2, label ? label : one, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

/* The keyboard is ten units across and fills the width it is given: each
   key runs from its first unit to its last, less one cell of gap, and the
   units are spread so the row ends flush with the edge. */
static int s_x0, s_span;

static int unit_x(int u) { return s_x0 + u * s_span / COLS; }

static tui_rect key_rect(int u0, int u1, int y, int h)
{
    const int x = unit_x(u0);
    return tui_rect_make(x, y, unit_x(u1) - x - 1, h);
}

void ls_keydock_draw(tui_surface *sf, tui_rect area)
{
    memset(s_hit, 0, sizeof(s_hit));
    s_drawn = false;
    /* `area` is the dock itself, already sized by ls_keydock_height. */
    int kh = area.h / 4;
    if (kh > KEY_MAX) kh = KEY_MAX;
    if (kh < KEY_MIN || area.w < COLS * 3) return;
    const int h = kh * 4, krow = kh - 1;     /* a row of air under each row of keys */
    /* One cell in from each side: the rounded glass is handled by the
       grid's own inset, and anything more is width a thumb could use. */
    s_x0 = area.x + 1;
    s_span = area.w - 1;
    const int y0 = area.y + area.h - h;
    tui_fill(sf, tui_rect_make(area.x, y0, area.w, h), ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
    if (s_flash_frames > 0) s_flash_frames--;
    for (int c = 0; c < COLS; c++) {
        key(sf, key_rect(c, c + 1, y0, krow), NULL, ROW0[s_layer][c], TUI_CYAN, c, false);
        key(sf, key_rect(c, c + 1, y0 + kh, krow), NULL, ROW1[s_layer][c], TUI_CYAN, 10 + c, false);
    }
    const int y2 = y0 + 2 * kh, y3 = y0 + 3 * kh;
    key(sf, key_rect(0, 1, y2, krow),
        s_layer == MORE ? "2/2" : s_layer == SYM ? "1/2" : s_layer == UPPER ? (s_once ? "Shf" : "CAP") : "shf",
        0, TUI_YELLOW, S_SHIFT, s_layer == UPPER);
    for (int c = 0; c < 8; c++)
        key(sf, key_rect(c + 1, c + 2, y2, krow), NULL, ROW2[s_layer][c], TUI_CYAN, 21 + c, false);
    key(sf, key_rect(9, 10, y2, krow), "DEL", 0, TUI_RED, S_DEL, false);
    key(sf, key_rect(0, 1, y3, krow), s_layer >= SYM ? "ABC" : "123", 0, TUI_YELLOW, S_LAYER, s_layer >= SYM);
    key(sf, key_rect(1, 2, y3, krow), "<", 0, TUI_BLUE, S_LEFT, false);
    key(sf, key_rect(2, 3, y3, krow), ">", 0, TUI_BLUE, S_RIGHT, false);
    key(sf, key_rect(3, 7, y3, krow), "SPACE", 0, TUI_CYAN, S_SPACE, false);
    key(sf, key_rect(7, 8, y3, krow), "^", 0, TUI_BLUE, S_UP, false);
    key(sf, key_rect(8, 9, y3, krow), "v", 0, TUI_BLUE, S_DOWN, false);
    key(sf, key_rect(9, 10, y3, krow), "RET", 0, TUI_GREEN, S_ENTER, false);
    s_drawn = true;
}

static bool inside(tui_rect r, int col, int row)
{
    /* The gap after a key, across and below, is part of it: a thumb that
       lands between two keys still types something. */
    return r.w > 0 && col >= r.x && col < r.x + r.w + 1 && row >= r.y && row < r.y + r.h + 1;
}

bool ls_keydock_touch(int col, int row, ls_tk_t *out_key, char *out_ch)
{
    if (!s_drawn) return false;
    int slot = -1;
    for (int i = 0; i < N_SLOTS && slot < 0; i++) if (inside(s_hit[i], col, row)) slot = i;
    if (slot < 0) return false;
    s_flash = slot; s_flash_frames = 3;
    ls_tk_t k = LS_TK_NONE; char ch = 0;
    if (slot < 10) { k = LS_TK_CHAR; ch = ROW0[s_layer][slot]; }
    else if (slot < 20) { k = LS_TK_CHAR; ch = ROW1[s_layer][slot - 10]; }
    else if (slot > S_SHIFT && slot < S_DEL) { k = LS_TK_CHAR; ch = ROW2[s_layer][slot - 21]; }
    else switch (slot) {
    case S_SHIFT:
        if (s_layer == SYM) s_layer = MORE;
        else if (s_layer == MORE) s_layer = SYM;
        else if (s_layer == LOWER) { s_layer = UPPER; s_once = true; }
        else if (s_once) s_once = false;            /* second tap: caps lock */
        else s_layer = LOWER;
        break;
    case S_LAYER: s_layer = s_layer >= SYM ? LOWER : SYM; break;
    case S_DEL:   k = LS_TK_BACKSPACE; break;
    case S_LEFT:  k = LS_TK_LEFT; break;
    case S_RIGHT: k = LS_TK_RIGHT; break;
    case S_UP:    k = LS_TK_UP; break;
    case S_DOWN:  k = LS_TK_DOWN; break;
    case S_SPACE: k = LS_TK_CHAR; ch = ' '; break;
    case S_ENTER: k = LS_TK_ENTER; break;
    }
    /* One shifted letter, then back to lowercase, like every phone. */
    if (k == LS_TK_CHAR && s_layer == UPPER && s_once && ch != ' ') s_layer = LOWER;
    if (out_key) *out_key = k;
    if (out_ch) *out_ch = ch;
    return true;
}
