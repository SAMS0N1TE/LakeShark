/* See ls_keyboard.h. The keyboard, and the box that shows what you have
   typed into it. */
#include "ls_keyboard.h"

#include <stdio.h>
#include <string.h>

#include "ls_tui_ui.h"

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

static bool               s_open;
static bool               s_secret;
static bool               s_reveal;
static int                s_flash_slot = -1, s_flash_frames;
static char               s_title[24];
static char               s_buf[LS_KEYBOARD_MAX + 1];
static int                s_len;
static int                s_max;
static int                s_layer;
static int                s_blink;
static ls_keyboard_done_t s_done;

enum { LAYER_LOWER = 0, LAYER_UPPER, LAYER_SYM, LAYER_MORE, LAYER__COUNT };

/* Four character layers, ten columns wide. */

static const char *const LAYER_ROW0[LAYER__COUNT] = {
    "qwertyuiop", "QWERTYUIOP", "1234567890", "[]{}<>\\^|`",
};
static const char *const LAYER_ROW1[LAYER__COUNT] = {
    "asdfghjkl.", "ASDFGHJKL?", "@#$%&*()-_", "0123456789",
};
static const char *const LAYER_ROW2[LAYER__COUNT] = {
    "zxcvbnm,",   "ZXCVBNM!",   "+=/:;'\"~", ".,!?:;\"'",
};

#define KEY_COLS 10

/* The slots, in the order they are drawn and hit-tested. */
#define SLOT_ROW0   0           /* 0..9   */
#define SLOT_ROW1   10          /* 10..19 */
#define SLOT_SHIFT  20
#define SLOT_ROW2   21          /* 21..28 */
#define SLOT_DEL    29
#define SLOT_LAYER  30
#define SLOT_SPACE  31
#define SLOT_CANCEL 32
#define SLOT_OK     33
#define SLOT_REVEAL 34
#define N_SLOTS     35

static tui_rect s_hit[N_SLOTS];
static int      s_hit_n;

/* ------------------------------------------------------------------ open -- */

void ls_keyboard_open(const char *title, const char *initial, int max_len,
                      ls_keyboard_done_t on_done)
{
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "TEXT");
    s_secret = false;
    s_reveal = false;
    s_flash_slot = -1;
    s_flash_frames = 0;
    s_done = on_done;

    s_max = (max_len > 0 && max_len < LS_KEYBOARD_MAX) ? max_len : LS_KEYBOARD_MAX;
    snprintf(s_buf, sizeof(s_buf), "%s", initial ? initial : "");
    s_len = (int)strlen(s_buf);
    if (s_len > s_max) { s_len = s_max; s_buf[s_len] = 0; }

    /* A fresh message starts with a capital, an edit of an existing string
       does not: the first thing you type into an empty box is the start of a
       sentence, and the first thing you type into a full one is a fix. */
    s_layer = s_len ? LAYER_LOWER : LAYER_UPPER;
    s_open = true;
}

static void clear_text(char *text, size_t size)
{
    volatile char *p = text;
    while (size--) *p++ = 0;
}

/* Wireless credentials use the same keyboard without showing secrets. */
void ls_keyboard_open_secret(const char *title, int max_len,
                             ls_keyboard_done_t on_done)
{
    ls_keyboard_open(title, "", max_len, on_done);
    s_secret = true;
    s_layer = LAYER_LOWER;
}

void ls_keyboard_close(void)
{
    s_open = false;
    s_done = NULL;
    clear_text(s_buf, sizeof(s_buf));
    s_len = 0;
    s_secret = false;
    s_reveal = false;
    s_flash_slot = -1;
    s_flash_frames = 0;
}
bool ls_keyboard_active(void) { return s_open; }

/* ------------------------------------------------------------------ edit -- */

static void push(char c)
{
    if (c < 0x20 || c >= 0x7F) return;
    if (s_len >= s_max) return;
    s_buf[s_len++] = c;
    s_buf[s_len] = 0;
    /* One capital, then back to lower case. A shift that stayed on would
       have to be turned off by hand after every proper noun. */
    if (s_layer == LAYER_UPPER) s_layer = LAYER_LOWER;
}

static void backspace(void)
{
    if (s_len > 0) s_buf[--s_len] = 0;
}

static void accept(void)
{
    const ls_keyboard_done_t cb = s_done;
    char out[LS_KEYBOARD_MAX + 1];
    snprintf(out, sizeof(out), "%s", s_buf);
    ls_keyboard_close();
    if (cb) cb(out);
    clear_text(out, sizeof(out));
}

/* Which character a slot produces, or 0 when the slot is not a character. */
static char slot_char(int slot)
{
    if (slot >= SLOT_ROW0 && slot < SLOT_ROW0 + 10)
        return LAYER_ROW0[s_layer][slot - SLOT_ROW0];
    if (slot >= SLOT_ROW1 && slot < SLOT_ROW1 + 10)
        return LAYER_ROW1[s_layer][slot - SLOT_ROW1];
    if (slot >= SLOT_ROW2 && slot < SLOT_ROW2 + 8)
        return LAYER_ROW2[s_layer][slot - SLOT_ROW2];
    if (slot == SLOT_SPACE) return ' ';
    return 0;
}

static void activate(int slot)
{
    /* acknowledge accepted taps while the next key is being aimed.
       The keyboard runs a faster frame loop; twelve frames remain visible
       for roughly a fingertip's lift without delaying another keystroke. */
    s_flash_slot = slot;
    s_flash_frames = 12;
    switch (slot) {
    case SLOT_REVEAL: s_reveal = !s_reveal; return;
    case SLOT_SHIFT:
        if (s_layer >= LAYER_SYM) {
            s_layer = s_layer == LAYER_SYM ? LAYER_MORE : LAYER_SYM;
            return;
        }
        s_layer = (s_layer == LAYER_UPPER) ? LAYER_LOWER : LAYER_UPPER;
        return;
    case SLOT_LAYER:
        s_layer = (s_layer >= LAYER_SYM) ? LAYER_LOWER : LAYER_SYM;
        return;
    case SLOT_DEL:    backspace(); return;
    case SLOT_CANCEL: ls_keyboard_close(); return;
    case SLOT_OK:     accept(); return;
    default: break;
    }
    const char c = slot_char(slot);
    if (c) push(c);
}

/* ------------------------------------------------------------------ draw -- */

static void draw_key(tui_surface *sf, tui_rect a, const char *label, char fig,
                     int density, uint8_t hue, int slot)
{
    if (slot >= 0 && slot < N_SLOTS) {
        s_hit[slot] = a;
        if (slot + 1 > s_hit_n) s_hit_n = slot + 1;
    }
    if (a.w <= 0 || a.h <= 0) return;

    /* A single-cell letter has no centre cell in an even-width key. Keep
       the full touch target, but paint an odd-width tile around its label. */
    if ((a.w & 1) == 0) { a.x++; a.w--; }

    const bool hit = slot == s_flash_slot && s_flash_frames > 0;
    ls_fill_dither(sf, a, hit ? LS_DITHER_HEAVY : density,
                   hit ? TUI_GREEN | TUI_BRIGHT : hue);
    const uint8_t text = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    /* The letter at ONE CELL, in bright white, not as a stroke figure. */

    char one[2] = { fig, 0 };
    const char *what = label ? label : one;
    ls_dither_label(sf, a, (a.h - 1) / 2, what, text);
}

/* The text so far, wrapped into the rows the box has, newest tail kept. */
static void draw_text(tui_surface *sf, tui_rect box)
{
    const int w = box.w - 4;
    const int rows = box.h - 2;
    if (w < 4 || rows < 1) return;

    const int cap = w * rows;
    const int from = s_len > cap ? s_len - cap : 0;

    for (int r = 0; r < rows; r++) {
        const int off = from + r * w;
        if (off >= s_len) break;
        char line[192];
        snprintf(line, sizeof(line), "%.*s", w, s_buf + off);
        if (s_secret && !s_reveal) memset(line, '*', strlen(line));
        tui_put_str(sf, box, box.x + 2, box.y + 1 + r, line,
                    A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
    /* A block cursor that blinks, so an empty box reads as waiting rather
       than as broken. */
    if ((s_blink / 12) & 1) {
        const int used = s_len - from;
        const int cy = box.y + 1 + (used / w);
        const int cx = box.x + 2 + (used % w);
        if (cy < box.y + box.h - 1)
            tui_put_char(sf, box, cx, cy, LS_TUI_BLOCK_FULL,
                         A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }
}

void ls_keyboard_draw(tui_surface *sf, tui_rect full)
{
    if (!s_open) return;
    s_blink++;
    if (full.w < 20 || full.h < 8) return;

    s_hit_n = 0;
    memset(s_hit, 0, sizeof(s_hit));

    /* --- how tall this has to be -------------------------------------- */
    /* A key is KEY_MIN rows, and that is the whole of the floor. */

#define KEY_MIN 3
#define KEY_MAX 6
    int kh = (full.h - 9) / 4;
    if (kh > KEY_MAX) kh = KEY_MAX;
    if (kh < KEY_MIN) kh = KEY_MIN;

    const int text_w = full.w - 6;
    int lines = text_w > 0 ? (s_max + text_w - 1) / text_w : 1;
    if (lines < 1) lines = 1;
    if (lines > 6) lines = 6;
    const int tbox_h = lines + 2;

    /* The overlay takes the BOTTOM of the screen, not all of it. */

    /* Two more rows in portrait than the contents strictly need. */

    const int reveal_h = s_secret ? 4 : 0;
    const int need = tbox_h + 4 * kh + 4 + reveal_h;
    const int margin = (full.h - need) >= 8 ? 2 : 0;
    const int want = need + margin;
    tui_rect area = full;
    if (want < full.h) {
        area.y = full.y + full.h - want;
        area.h = want;
    }
    if (kh > (area.h - tbox_h - 4 - reveal_h) / 4)
        kh = (area.h - tbox_h - 4 - reveal_h) / 4;

    /* Black behind the overlay's own rows, so nothing of the screen shows
       through the keys themselves. */
    tui_fill(sf, area, ' ', A(TUI_WHITE, TUI_BLACK));
    ls_panel_box(sf, area, s_title, TUI_CYAN);

    /* --- what you have typed ------------------------------------------ */
    tui_rect box = tui_rect_make(area.x + 2, area.y + 2, area.w - 4, tbox_h);
    ls_panel_box(sf, box, NULL, TUI_GREEN | TUI_BRIGHT);
    draw_text(sf, box);
    {
        char count[16];
        snprintf(count, sizeof(count), "%d/%d", s_len, s_max);
        const int n = (int)strlen(count);
        if (box.w > n + 4)
            tui_put_str(sf, box, box.x + box.w - n - 2, box.y + box.h - 1,
                        count, A(DIM_FG, TUI_BLACK));
    }

    /* --- the keys ------------------------------------------------------ */
    if (s_secret) {
        draw_key(sf, tui_rect_make(area.x + 2, box.y + box.h + 1,
                                  area.w - 4, 3),
                 s_reveal ? "HIDE PASSWORD" : "SHOW PASSWORD", 0,
                 s_reveal ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT,
                 s_reveal ? TUI_GREEN : TUI_CYAN, SLOT_REVEAL);
    }
    const int pad_y = box.y + box.h + 1 + reveal_h;
    const int pad_h = (area.y + area.h - 1) - pad_y;

    /* Four rows: three of characters and one of actions. Below KEY_MIN rows
       each they are targets nobody can hit, and half a keyboard is worse
       than none. */
    if (pad_h < 4 * KEY_MIN) {

        tui_put_str(sf, area, area.x + 2, area.y + area.h - 2,
                    "type it, ENTER to send, ESC to go back", A(DIM_FG, TUI_BLACK));
        return;
    }

    if (kh > pad_h / 4) kh = pad_h / 4;

    /* The gap between keys comes out of the key, and only when there is a
       row to spare: at the floor a gap would trade a hittable key for a tidy
       one. */
    const int krow = (kh > KEY_MIN) ? kh - 1 : kh;

    int kw = (area.w - 2) / KEY_COLS;
    if (kw > 9) kw = 9;                 /* landscape: wider is not better */
    if (kw < 3) return;                 /* a letter and its gap */

    const int x0 = area.x + 1 + ((area.w - 2) - kw * KEY_COLS) / 2;
    /* Anchored to the BOTTOM of the overlay, not centred in it. A keyboard
       lives under the thing being typed on every device anyone has held, and
       on a 1232 pixel portrait panel the difference between centred and
       bottom is about two centimetres of thumb travel. Centred also put the
       action row - the one with OK on it - in the middle of the screen,
       which is the last place a hand looks for it. */
    int y0 = area.y + area.h - 1 - kh * 4;
    if (y0 < pad_y) y0 = pad_y;
    const int inner = kw - 1;           /* the gap between keys */

    const uint8_t letter_hue = TUI_CYAN;

    for (int c = 0; c < KEY_COLS; c++) {
        const tui_rect a = tui_rect_make(x0 + c * kw, y0, inner, krow);
        draw_key(sf, a, NULL, LAYER_ROW0[s_layer][c], LS_DITHER_LIGHT,
                 letter_hue, SLOT_ROW0 + c);
    }
    for (int c = 0; c < KEY_COLS; c++) {
        const tui_rect a = tui_rect_make(x0 + c * kw, y0 + kh, inner, krow);
        draw_key(sf, a, NULL, LAYER_ROW1[s_layer][c], LS_DITHER_LIGHT,
                 letter_hue, SLOT_ROW1 + c);
    }

    const int y2 = y0 + 2 * kh;
    {
        const bool up = (s_layer == LAYER_UPPER);
        draw_key(sf, tui_rect_make(x0, y2, inner, krow),
                 s_layer == LAYER_MORE ? "2/2" : s_layer == LAYER_SYM ? "1/2"
                                                                  : up ? "SHF" : "shf", 0,
                 up ? LS_DITHER_HEAVY : LS_DITHER_LIGHT, TUI_YELLOW, SLOT_SHIFT);
        for (int c = 0; c < 8; c++)
            draw_key(sf, tui_rect_make(x0 + (c + 1) * kw, y2, inner, krow), NULL,
                     LAYER_ROW2[s_layer][c], LS_DITHER_LIGHT, letter_hue,
                     SLOT_ROW2 + c);
        draw_key(sf, tui_rect_make(x0 + 9 * kw, y2, inner, krow), "DEL", 0,
                 LS_DITHER_LIGHT, TUI_RED, SLOT_DEL);
    }

    /* The action row, in units of the character grid so it lines up with it:
       layer 2, space 4, cancel 2, ok 2. */
    const int y3 = y0 + 3 * kh;
    draw_key(sf, tui_rect_make(x0, y3, 2 * kw - 1, krow),
             s_layer >= LAYER_SYM ? "ABC" : "123", 0,
             s_layer >= LAYER_SYM ? LS_DITHER_HEAVY : LS_DITHER_LIGHT,
             TUI_YELLOW, SLOT_LAYER);
    draw_key(sf, tui_rect_make(x0 + 2 * kw, y3, 4 * kw - 1, krow), "SPACE", 0,
             LS_DITHER_LIGHT, letter_hue, SLOT_SPACE);
    draw_key(sf, tui_rect_make(x0 + 6 * kw, y3, 2 * kw - 1, krow), "CANCEL", 0,
             LS_DITHER_LIGHT, TUI_RED, SLOT_CANCEL);
    /* OK is loud: it is the one key here that changes what the radio does. */
    draw_key(sf, tui_rect_make(x0 + 8 * kw, y3, 2 * kw - 1, krow), "OK", 0,
             LS_DITHER_MEDIUM, TUI_GREEN | TUI_BRIGHT, SLOT_OK);

    s_hit_n = N_SLOTS;
    /* Visual gutters belong to the adjacent key, instead of swallowing a
       near-centre tap. Expand only into actual gaps, never another row. */
    for (int i = 0; i < SLOT_REVEAL; ++i) {
        s_hit[i].w++;
        s_hit[i].h += kh - krow;
    }
    if (s_flash_frames > 0) s_flash_frames--;
}

/* ----------------------------------------------------------------- input -- */

bool ls_keyboard_key(ls_tk_t key, char ch)
{
    if (!s_open) return false;

    /* A physical keyboard types straight into the same buffer. The
       on-screen keys are for when there is not one, not instead of one. */
    if (key == LS_TK_CHAR) {
        for (int i = 0; i < SLOT_REVEAL; ++i) {
            if (slot_char(i) == ch) { s_flash_slot = i; s_flash_frames = 12; break; }
        }
        push(ch);
        return true;
    }

    switch (key) {
    case LS_TK_ENTER:     accept(); return true;
    case LS_TK_ESC:       ls_keyboard_close(); return true;
    case LS_TK_BACKSPACE: activate(SLOT_DEL); return true;
    case LS_TK_TAB:
        s_layer = (s_layer >= LAYER_SYM) ? LAYER_LOWER : LAYER_SYM;
        return true;
    default: return true;
    }
}

bool ls_keyboard_touch(int col, int row)
{
    if (!s_open) return false;
    for (int i = 0; i < s_hit_n; i++) {
        const tui_rect r = s_hit[i];
        if (r.w <= 0) continue;
        if (col >= r.x && col < r.x + r.w && row >= r.y && row < r.y + r.h) {
            activate(i);
            return true;
        }
    }

    return true;
}
