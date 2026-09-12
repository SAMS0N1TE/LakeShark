/* See ls_numpad.h. The keypad, and the box that shows what you have typed. */
#include "ls_numpad.h"

#include <stdio.h>
#include <string.h>

#include "ls_glyph.h"
#include "ls_tui_ui.h"

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

#define DIGITS_MAX 12

static bool             s_open;
static char             s_title[24];
static char             s_unit[8];
static char             s_buf[DIGITS_MAX + 1];
static int              s_len;
static bool             s_fresh;      /* the first digit replaces the seed */
static ls_numpad_done_t s_done;

/* The pad, as it is laid out and as it is read.

   Four rows of three, then one row of two. The order is a telephone's and
   not a calculator's - 1 at the top left - because every radio, every phone
   and every door lock is a telephone pad, and the one place people meet a
   calculator layout is a calculator. */
static const char *const KEYS[] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    ".", "0", "DEL",
};
#define N_KEYS ((int)(sizeof(KEYS) / sizeof(KEYS[0])))
#define KEY_COLS 3
#define KEY_ROWS 4

/* Where each key landed, so a tap is tested against what was drawn rather
   than against arithmetic repeated in two places. */
static tui_rect s_hit[N_KEYS + 2];
static int      s_hit_n;
#define HIT_CANCEL (N_KEYS)
#define HIT_OK     (N_KEYS + 1)

/* ------------------------------------------------------------------ open -- */

void ls_numpad_open(const char *title, const char *unit, double initial,
                    ls_numpad_done_t on_done)
{
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "VALUE");
    snprintf(s_unit, sizeof(s_unit), "%s", unit ? unit : "");
    s_done = on_done;

    snprintf(s_buf, sizeof(s_buf), "%.4f", initial);
    s_len = (int)strlen(s_buf);
    while (s_len > 1 && s_buf[s_len - 1] == '0') s_buf[--s_len] = 0;
    if (s_len > 1 && s_buf[s_len - 1] == '.') s_buf[--s_len] = 0;

    s_fresh = true;
    s_open = true;
}

void ls_numpad_close(void) { s_open = false; s_done = NULL; }
bool ls_numpad_active(void) { return s_open; }

/* ------------------------------------------------------------------ edit -- */

static void push(char c)
{
    if (s_fresh) { s_len = 0; s_buf[0] = 0; s_fresh = false; }
    if (c == '.' && strchr(s_buf, '.')) return;      /* one point, no more */
    if (s_len >= DIGITS_MAX) return;
    s_buf[s_len++] = c;
    s_buf[s_len] = 0;
}

static void backspace(void)
{

    s_fresh = false;
    if (s_len > 0) s_buf[--s_len] = 0;
}

static void accept(void)
{
    const ls_numpad_done_t cb = s_done;
    double v = 0;
    if (sscanf(s_buf, "%lf", &v) != 1) v = 0;
    ls_numpad_close();
    if (cb) cb(v);
}

/* ------------------------------------------------------------------ draw -- */

/* The digits, at the size of the key they are on. */

#define DIG_H LS_GLYPH_ROWS

static void draw_key(tui_surface *sf, tui_rect a, const char *label,
                     bool loud, int slot)
{
    const uint8_t hue = loud ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;
    const uint8_t text = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    ls_panel_box(sf, a, NULL, hue);
    tui_rect field = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (field.w > 0 && field.h > 0) {
        ls_fill_dither(sf, field, loud ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
        /* A single digit or the point gets the figure; a word - DEL, OK,
           CANCEL - stays lettering, because a word has no figure and does
           not need one to be read. */
        if (label[0] && !label[1] && field.h >= DIG_H)
            ls_glyph_draw(sf, field, label[0], text);
        else
            ls_dither_label(sf, field, (field.h - 1) / 2, label, text);
    }
    if (slot >= 0 && slot < (int)(sizeof(s_hit) / sizeof(s_hit[0]))) {
        s_hit[slot] = a;
        if (slot + 1 > s_hit_n) s_hit_n = slot + 1;
    }
}

void ls_numpad_draw(tui_surface *sf, tui_rect area)
{
    if (!s_open || area.w < 20 || area.h < 14) return;

    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    s_hit_n = 0;

    tui_fill(sf, area, ' ', A(TUI_WHITE, TUI_BLACK));
    ls_panel_box(sf, area, s_title, TUI_CYAN);

    /* --- what you have typed, large and at the top -------------------- */
    tui_rect box = tui_rect_make(area.x + 2, area.y + 2, area.w - 4, 3);
    ls_panel_box(sf, box, NULL, TUI_GREEN | TUI_BRIGHT);
    {
        char line[32];
        snprintf(line, sizeof(line), "%s %s", s_buf, s_unit);
        const int n = (int)strlen(line);
        tui_put_str(sf, box, box.x + (box.w - n) / 2, box.y + 1, line,
                    A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));

        if (!s_len)
            tui_put_char(sf, box, box.x + box.w / 2, box.y + 1,
                         LS_TUI_BLOCK_FULL, A(TUI_GREEN, TUI_BLACK));
    }

    /* --- the pad ------------------------------------------------------ */
    const int pad_y = box.y + 4;
    const int pad_h = (area.y + area.h - 1) - pad_y - 1;
    if (pad_h < 8) return;

    /* Five rows of keys: four of digits and one of actions. Every key gets
       the same height so the pad reads as a grid, which is what makes it
       possible to hit one without looking. */
    int kh = pad_h / (KEY_ROWS + 1);
    if (kh > 8) kh = 8;
    if (kh < 3) kh = 3;
    const int kw = (area.w - 2) / KEY_COLS;
    if (kw < 5) return;

    const int x0 = area.x + 1 + ((area.w - 2) - kw * KEY_COLS) / 2;
    const int y0 = pad_y + (pad_h - kh * (KEY_ROWS + 1)) / 2;

    for (int i = 0; i < N_KEYS; i++) {
        const int r = i / KEY_COLS, c = i % KEY_COLS;
        draw_key(sf, tui_rect_make(x0 + c * kw, y0 + r * kh, kw - 1, kh),
                 KEYS[i], false, i);
    }

    /* CANCEL is wide and OK is loud: one of them undoes nothing and the
       other changes what the radio is doing. */
    const int ay = y0 + KEY_ROWS * kh;
    const int aw = (kw * KEY_COLS) * 2 / 3;
    draw_key(sf, tui_rect_make(x0, ay, aw - 1, kh), "CANCEL", false, HIT_CANCEL);
    draw_key(sf, tui_rect_make(x0 + aw, ay, kw * KEY_COLS - aw - 1, kh),
             "OK", true, HIT_OK);
    s_hit_n = HIT_OK + 1;

    if (area.h > 20)
        tui_put_str(sf, area, area.x + 2, area.y + area.h - 2,
                    "type it, or ESC to go back", dim);
}

/* ----------------------------------------------------------------- input -- */

static void activate(int slot)
{
    if (slot < 0) return;
    if (slot == HIT_OK)     { accept(); return; }
    if (slot == HIT_CANCEL) { ls_numpad_close(); return; }
    if (slot >= N_KEYS)     return;
    if (!strcmp(KEYS[slot], "DEL")) backspace();
    else                            push(KEYS[slot][0]);
}

bool ls_numpad_key(ls_tk_t key, char ch)
{
    if (!s_open) return false;

    if (key == LS_TK_CHAR) {
        if (ch >= '0' && ch <= '9') { push(ch); return true; }
        if (ch == '.' || ch == ',') { push('.'); return true; }
        return true;                  /* everything else is swallowed */
    }
    switch (key) {
    case LS_TK_ENTER:     accept(); return true;
    case LS_TK_ESC:       ls_numpad_close(); return true;
    case LS_TK_BACKSPACE: backspace(); return true;
    default: return true;
    }
}

bool ls_numpad_touch(int col, int row)
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
