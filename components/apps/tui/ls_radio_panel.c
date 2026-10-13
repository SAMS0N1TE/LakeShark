#include "ls_radio_panel.h"
#include "ls_numpad.h"
#include "ls_scan_options.h"
#include "scan_engine.h"
#include "scan_channels.h"
#include <stdio.h>
#include <string.h>

static const uint8_t cyan = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
static const uint8_t amber = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
static const uint8_t white = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

static void line(tui_surface *sf, tui_rect a, int row, const char *text, uint8_t attr)
{
    tui_rect clip = tui_rect_make(a.x + 1, a.y, a.w - 2, a.h);
    tui_put_str(sf, clip, a.x + 2, a.y + row, text, attr);
}

void ls_radio_frequency_draw(tui_surface *sf, tui_rect a, int row, uint32_t hz,
                             uint8_t attr)
{
    static const uint8_t digits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};
    char text[20];
    snprintf(text, sizeof(text), "%lu.%04lu", (unsigned long)(hz / 1000000),
             (unsigned long)((hz % 1000000) / 100));
    int width = (int)strlen(text) * 4 - 2;
    if (a.w < width + 2) {
        line(sf, a, row, text, attr);
        return;
    }
    int x = a.x + (a.w - width) / 2;
    for (const char *p = text; *p; ++p) {
        if (*p == '.') {
            tui_put_char(sf, a, x, a.y + row + 4, LS_TUI_BLOCK_FULL, attr);
            x += 2;
        } else {
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 3; c++)
                    if (digits[*p - '0'][r] & (4 >> c))
                        tui_put_char(sf, a, x + c, a.y + row + r, LS_TUI_BLOCK_FULL, attr);
            x += 4;
        }
    }
}

/* ===================================================================== */
/* THE SCANNER'S GLASS AND DECK                                          */
/*                                                                       */
/* The scanner used to be a stack of boxes with its volume and squelch   */
/* printed in one of them as text: the screen you actually run a scan     */
/* from could not turn the audio down or open the gate without leaving    */
/* for the VFO. It is now the same instrument the VFO page is - one piece */
/* of ASCII glass with a deck of filled stepper cards under it - because  */
/* it is the same receiver, and a handheld does not change its face when  */
/* you press SCAN.                                                       */
/*                                                                       */
/* Everything here is fixed storage: the cards, their hit rects, the      */
/* ninety-six byte activity ring and one float of peak hold. Drawing the  */
/* scanner allocates nothing, and the only state it keeps between frames  */
/* is that ring, the peak and the key flash.                             */
/* ===================================================================== */

/* The deck, in the VFO deck's order so a thumb learns one layout. */
enum { CARD_VOL, CARD_SQL, CARD_HANG, CARD_THRESH, N_CARD };

static tui_rect s_card[N_CARD], s_card_down[N_CARD], s_card_up[N_CARD],
                s_card_val[N_CARD], s_card_bar[N_CARD];
static int      s_cards;          /* how many the last draw laid out      */
static uint8_t  s_card_id[N_CARD];/* which setting each drawn slot is     */
static tui_rect s_gate;           /* the squelch slider under the meter   */

/* A tap on a +/- key lights it for a few frames, the acknowledgement the
   VFO's own cards give: the value lands a frame or two later, and a key that
   looks the same before and after the touch is one a thumb cannot tell it
   reached. */
#define FLASH_FRAMES 6
static int  s_flash = -1;
static bool s_flash_up;
static int  s_flash_ttl;

/* The activity trace: the last readings of the same quieting the squelch is
   already measuring, scrolling right to left. No acquisition, no FFT, no
   buffer beyond these bytes. */
#define TRACE_N 96
static uint8_t s_trace[TRACE_N];
static int     s_trace_head, s_trace_len;
static float   s_hold;            /* decaying peak on the signal meter    */

/* The channel the options list acts on: whatever the list is showing. */
static int s_sel_ch = -1;

/* The screen's own setters, picked up from the view each draw. One panel
   draws at a time, so the keypad trampolines can reach them from here
   without the panel ever learning which receiver it is driving. */
static void (*s_set_vol)(int);
static void (*s_set_sql)(int);
static void numpad_vol(double v)    { if (s_set_vol) s_set_vol((int)(v + 0.5)); }
static void numpad_sql(double v)    { if (s_set_sql) s_set_sql((int)(v + 0.5)); }
static void numpad_hang(double s)   { scan_engine_set_hang_ms((int)(s * 1000.0 + 0.5)); }
static void numpad_thresh(double p) { scan_engine_set_threshold_pct((int)(p + 0.5)); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void trace_push(float sig, bool have)
{
    const float s = have ? (sig < 0.0f ? 0.0f : sig > 1.0f ? 1.0f : sig) : 0.0f;
    s_trace[s_trace_head] = (uint8_t)(s * 255.0f);
    s_trace_head = (s_trace_head + 1) % TRACE_N;
    if (s_trace_len < TRACE_N) s_trace_len++;
}

/* LINES, NOT SLABS: LS_TUI_TRACE is a two pixel line standing in the middle
   of the cell, so each reading is its own lit filament instead of a hundred
   readings running together into one coloured rectangle. */
static void trace_draw(tui_surface *sf, tui_rect box, int x0, int y, int w, int h,
                       float gate)
{
    if (w <= 0 || h <= 0) return;
    if (w > TRACE_N) w = TRACE_N;
    const int gate_q = (int)(gate * (float)(h * 8));
    for (int i = 0; i < w; ++i) {
        const int age = w - 1 - i;
        int v = 0;
        if (age < s_trace_len) {
            const int idx = ((s_trace_head - 1 - age) % TRACE_N + TRACE_N) % TRACE_N;
            v = s_trace[idx];
        }
        const int q = v * h * 8 / 255;
        for (int r = 0; r < h; ++r) {
            const int base = (h - 1 - r) * 8;
            const int fill = q - base;
            if (fill < 1) continue;
            const uint8_t hue = q < gate_q      ? TUI_CYAN
                              : base >= 6 * h   ? (TUI_RED | TUI_BRIGHT)
                              : base >= 3 * h   ? (TUI_YELLOW | TUI_BRIGHT)
                                                : (TUI_GREEN | TUI_BRIGHT);
            tui_put_char(sf, box, x0 + i, y + r,
                         LS_TUI_TRACE(fill > 8 ? 8 : fill), TUI_ATTR(hue, TUI_BLACK));
        }
    }
}

/* An annunciator: a lit field with a word on it, the same vocabulary as the
   buttons below it. Answers where the next one starts. */
int ls_radio_chip(tui_surface *sf, tui_rect box, int x, int y, const char *text,
                  bool on, uint8_t hue)
{
    const int n = (int)strlen(text);
    if (x + n + 2 > box.x + box.w) return x;
    tui_rect field = tui_rect_make(x, y, n + 2, 1);
    ls_fill_dither(sf, field, on ? LS_DITHER_HEAVY : LS_DITHER_LIGHT,
                   on ? hue : LS_FAINT_FG);
    /* A lit chip is a filled field with the word ON it, not a word with two
       coloured crumbs beside it: the letters carry the hue as their ground. */
    tui_put_str(sf, box, x + 1, y, text,
                on ? TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT)
                   : TUI_ATTR(LS_DIM_FG, TUI_BLACK));
    return x + n + 3;
}

/* One lit filament of a bargraph, full cell height. */
static void filament(tui_surface *sf, tui_rect box, int x, int y, int rows, uint8_t hue)
{
    for (int r = 0; r < rows; ++r)
        tui_put_char(sf, box, x, y + r, LS_TUI_TRACE(8), TUI_ATTR(hue, TUI_BLACK));
}

/* The bezel: a ruled frame, heavier than a panel outline, so the glass does
   not read as one more card in a stack of cards. */
void ls_radio_bezel(tui_surface *sf, tui_rect r, const char *title)
{
    const uint8_t edge = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    for (int x = 0; x < r.w; ++x) {
        tui_put_char(sf, r, r.x + x, r.y, '=', edge);
        tui_put_char(sf, r, r.x + x, r.y + r.h - 1, '=', edge);
    }
    for (int y = 1; y < r.h - 1; ++y) {
        tui_put_char(sf, r, r.x, r.y + y, '|', edge);
        tui_put_char(sf, r, r.x + r.w - 1, r.y + y, '|', edge);
    }
    if (title && r.w > (int)strlen(title) + 8)
        tui_put_str(sf, r, r.x + 3, r.y, title, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
}

/* The signal meter and, under it, the squelch gate: a caret on the same
   travel, and the row it sits on is the slider that sets it. Above the caret
   is audio, below it is silence. */
static void meter(tui_surface *sf, tui_rect box, int x0, int y, int w,
                  const ls_radio_view_t *v, bool gate_row)
{
    tui_put_str(sf, box, x0, y, "SIG", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    const int bx = x0 + 4;
    const int bw = w - 10;
    if (bw < 6) return;
    const int segs = bw / 2;
    if (segs < 2) return;
    if (!v->has_squelch) {
        tui_put_str(sf, box, bx, y, "--", LS_ATTR_DIM);
    } else {
        const float sig = v->signal < 0.0f ? 0.0f : v->signal > 1.0f ? 1.0f : v->signal;
        if (sig >= s_hold) s_hold = sig;
        else               s_hold -= (s_hold - sig) * 0.06f;
        const int lit  = (int)(sig * (float)segs + 0.5f);
        const int hold = (int)(s_hold * (float)segs);
        for (int s = 0; s < segs; ++s) {
            const bool on = s < lit;
            uint8_t hue = on ? (s >= segs * 3 / 4 ? (TUI_RED | TUI_BRIGHT)
                              : s >= segs / 2     ? (TUI_YELLOW | TUI_BRIGHT)
                                                  : (TUI_GREEN | TUI_BRIGHT))
                             : LS_FAINT_FG;
            if (!on && s == hold && hold > lit) hue = TUI_WHITE | TUI_BRIGHT;
            filament(sf, box, bx + s * 2, y, 1, hue);
        }
        char pct[8];
        snprintf(pct, sizeof(pct), "%3d%%", (int)(sig * 100.0f + 0.5f));
        tui_put_str(sf, box, bx + segs * 2 + 1, y, pct,
                    TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
    if (!gate_row) return;
    const int g = clampi((int)(v->gate * (float)segs), 0, segs - 1);
    tui_put_str(sf, box, x0, y + 1, "SQL", LS_ATTR_FAINT);
    for (int s = 0; s < segs; ++s)
        tui_put_char(sf, box, bx + s * 2, y + 1, s == g ? '^' : '.',
                     TUI_ATTR(s == g ? (v->squelch_open ? (TUI_GREEN | TUI_BRIGHT)
                                                        : (TUI_CYAN | TUI_BRIGHT))
                                     : LS_FAINT_FG, TUI_BLACK));
    /* The whole row answers a tap, not just the caret's own cell. */
    s_gate = tui_rect_make(bx, y + 1, segs * 2, 1);
}

/* --------------------------------------------------------------- the deck */

typedef struct {
    const char *label, *shortname;
    char        value[12];
    float       frac;
    uint8_t     hue;
    bool        live;
    /* Whether this card can be given a value outright, which is what the
       travel does. Volume and squelch belong to the screen: without its
       setter they can still be stepped, but there is no slider to press,
       and a slider that cannot set is not drawn as a target. */
    bool        settable;
} card_t;

static void card_fill(card_t *c, int id, const ls_radio_view_t *v)
{
    memset(c, 0, sizeof(*c));
    switch (id) {
    case CARD_VOL:
        c->label = "VOLUME"; c->shortname = "VOL"; c->hue = TUI_GREEN;
        /* Volume is on every screen that draws this panel, and the keys the
           card presses are the ones its own quick controls already carry, so
           it is live whether or not the screen handed over a setter. */
        c->live = true;
        c->settable = v->set_volume_pct != NULL;
        c->frac = clampi(v->volume, 0, 100) / 100.0f;
        snprintf(c->value, sizeof(c->value), "%d%%", clampi(v->volume, 0, 100));
        break;
    case CARD_SQL:
        c->label = "SQUELCH"; c->shortname = "SQL"; c->hue = TUI_CYAN;
        c->live = v->set_squelch_pct != NULL || v->has_squelch;
        c->settable = v->set_squelch_pct != NULL;
        c->frac = v->gate < 0.0f ? 0.0f : v->gate > 1.0f ? 1.0f : v->gate;
        if (c->live) snprintf(c->value, sizeof(c->value), "%d%%",
                              (int)(c->frac * 100.0f + 0.5f));
        else snprintf(c->value, sizeof(c->value), "--");
        break;
    case CARD_HANG:
        c->label = "HANG"; c->shortname = "HANG"; c->hue = TUI_YELLOW;
        c->live = c->settable = true;
        c->frac = clampi(scan_engine_get_hang_ms(), 0, 30000) / 30000.0f;
        snprintf(c->value, sizeof(c->value), "%.1fs", scan_engine_get_hang_ms() / 1000.0);
        break;
    default:
        c->label = "OPEN AT"; c->shortname = "OPEN"; c->hue = TUI_MAGENTA;
        c->live = c->settable = true;
        c->frac = clampi(scan_engine_get_threshold_pct(), 0, 100) / 100.0f;
        snprintf(c->value, sizeof(c->value), "%d%%", scan_engine_get_threshold_pct());
        break;
    }
}

/* A card: its own rule, the label and value on one line, the travel under
   them as filaments, and the two keys at the right end of it. The keys keep
   the dithered field every other button on this firmware has - a key with no
   surface is not a key - and the value field is the way to exact entry, so
   the number is never only steppable. */
void ls_radio_card_draw(tui_surface *sf, tui_rect b, ls_radio_card_t *c)
{
    c->down = c->up = c->value_rect = c->bar = tui_rect_make(0, -1, 0, 0);
    if (b.w < 20 || b.h < 5) return;
    const uint8_t h = c->live ? c->hue : LS_FAINT_FG;
    const uint8_t edge = TUI_ATTR(h, TUI_BLACK);
    const uint8_t face = TUI_ATTR(c->live ? (h | TUI_BRIGHT) : LS_DIM_FG, TUI_BLACK);

    for (int x = 0; x < b.w; ++x) {
        tui_put_char(sf, b, b.x + x, b.y, '=', edge);
        tui_put_char(sf, b, b.x + x, b.y + b.h - 1, '=', edge);
    }
    for (int y = 1; y < b.h - 1; ++y) {
        tui_put_char(sf, b, b.x, b.y + y, '|', edge);
        tui_put_char(sf, b, b.x + b.w - 1, b.y + y, '|', edge);
    }

    const int label_y = b.y + 1;
    const int inner_y = label_y + 1;
    const int inner_h = b.h - 3;
    const int right = b.x + b.w - 2;
    const int key_w = b.w >= 30 ? 6 : 5;

    c->up   = tui_rect_make(right - key_w + 1, inner_y, key_w, inner_h);
    c->down = tui_rect_make(right - 2 * key_w, inner_y, key_w, inner_h);

    /* THE HEADER IS THE NUMBER; THE TRAVEL IS THE SETTING.

       These are two targets and they must not overlap. The value field used
       to be given the bar's rows as well, so a tap meant for the travel
       opened the keypad - the one thing a slider must never do. The number
       now wears brackets and keeps row one; the filaments own the rows under
       it and set the value where they are pressed. */
    tui_put_str(sf, b, b.x + 2, label_y, c->label, face);
    char field[20];
    snprintf(field, sizeof(field), "[%s]", c->value);
    const int field_x = b.x + 2 + (int)strlen(c->label) + 1;
    tui_put_str(sf, b, field_x, label_y, field,
                c->live ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    const int field_end = field_x + (int)strlen(field);
    if (c->live && right - 4 > field_end + 1)
        tui_put_str(sf, b, right - 4, label_y, "EDIT", TUI_ATTR(h | TUI_BRIGHT, TUI_BLACK));
    c->value_rect = c->live
        ? tui_rect_make(field_x, label_y, right - field_x + 1, 1)
        : tui_rect_make(0, -1, 0, 0);

    const int bar_x = b.x + 2;
    const int bar_w = c->down.x - 2 - bar_x;
    const int segs = bar_w / 2;
    if (segs >= 2) {
        const int lit = (int)(c->frac * (float)segs + 0.5f);
        for (int s = 0; s < segs; ++s)
            filament(sf, b, bar_x + s * 2, inner_y, inner_h,
                     c->live && s < lit ? (h | TUI_BRIGHT) : LS_FAINT_FG);
        if (c->live && c->settable)
            c->bar = tui_rect_make(bar_x, inner_y, segs * 2 - 1, inner_h);
    }

    for (int k = 0; k < 2; ++k) {
        const tui_rect key = k ? c->up : c->down;
        const bool flash = c->live && (k ? c->flash_up : c->flash_down);

        ls_fill_dither(sf, key,
                       flash ? LS_DITHER_HEAVY : c->live ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT,
                       flash ? (TUI_WHITE | TUI_BRIGHT) : c->live ? h : LS_FAINT_FG);
        for (int x = 0; x < key.w; ++x) {
            tui_put_char(sf, b, key.x + x, key.y, '=', edge);
            tui_put_char(sf, b, key.x + x, key.y + key.h - 1, '=', edge);
        }
        const char sign[2] = { k ? '+' : '-', 0 };
        tui_put_str(sf, b, key.x + (key.w - 1) / 2, key.y + (key.h - 1) / 2, sign,
                    TUI_ATTR(flash ? TUI_BLACK : c->live ? (TUI_WHITE | TUI_BRIGHT) : LS_DIM_FG,
                             flash ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK));
    }
}

/* The scanner's own cards: the shared drawing above, with the panel's hit
   rects and its key flash. One implementation, two screens. */
static void card_draw(tui_surface *sf, int slot, const card_t *c)
{
    ls_radio_card_t card = { .label = c->label, .value = c->value, .frac = c->frac,
                             .hue = c->hue, .live = c->live, .settable = c->settable };
    const bool lit = s_flash == slot && s_flash_ttl > 0;
    card.flash_down = lit && !s_flash_up;
    card.flash_up   = lit && s_flash_up;
    ls_radio_card_draw(sf, s_card[slot], &card);
    s_card_down[slot] = card.down;
    s_card_up[slot]   = card.up;
    s_card_val[slot]  = card.value_rect;
    s_card_bar[slot]  = card.bar;
}

/* A VFD bargraph, the house measurement: thin filaments every other column so
   a reading reads as a reading and not as a coloured slab. */
void ls_radio_bargraph(tui_surface *sf, tui_rect box, int x, int y, int w,
                       int rows, float frac, float peak)
{
    const int segs = w / 2;
    if (segs < 1 || rows < 1) return;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int lit  = (int)(frac * (float)segs + 0.5f);
    const int hold = peak >= 0.0f ? (int)((peak > 1.0f ? 1.0f : peak) * (float)segs) : -1;
    for (int s = 0; s < segs; ++s) {
        const bool on = s < lit;
        uint8_t hue = on ? (s >= segs * 3 / 4 ? (TUI_RED | TUI_BRIGHT)
                          : s >= segs / 2     ? (TUI_YELLOW | TUI_BRIGHT)
                                              : (TUI_GREEN | TUI_BRIGHT))
                         : LS_FAINT_FG;
        if (!on && s == hold && hold > lit) hue = TUI_WHITE | TUI_BRIGHT;
        filament(sf, box, x + s * 2, y, rows, hue);
    }
}

/* A pane with no rows for cards keeps the one-line control: the value, and a
   minus and plus beside it, which still sets everything the card does. */
static void card_draw_line(tui_surface *sf, int slot, const card_t *c)
{
    const tui_rect b = s_card[slot];
    if (b.w < 13 || b.h < 1) return;
    const uint8_t h = c->live ? c->hue : LS_FAINT_FG;
    const int right = b.x + b.w - 1;
    s_card_down[slot] = tui_rect_make(right - 7, b.y, 3, 1);
    s_card_up[slot]   = tui_rect_make(right - 3, b.y, 3, 1);
    tui_put_str(sf, b, b.x, b.y, c->shortname, TUI_ATTR(h | TUI_BRIGHT, TUI_BLACK));
    /* One line has no travel to press, so the bracketed number is the only
       target here and it is exact entry - never a half-slider. */
    char field[20];
    snprintf(field, sizeof(field), "[%s]", c->value);
    const int field_x = b.x + (int)strlen(c->shortname) + 1;
    tui_put_str(sf, b, field_x, b.y, field,
                c->live ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    s_card_val[slot] = c->live
        ? tui_rect_make(field_x, b.y, (int)strlen(field), 1)
        : tui_rect_make(0, -1, 0, 0);
    for (int k = 0; k < 2; ++k) {
        const tui_rect key = k ? s_card_up[slot] : s_card_down[slot];
        const bool flash = c->live && s_flash == slot && s_flash_up == (k != 0) &&
                           s_flash_ttl > 0;
        ls_fill_dither(sf, key, flash ? LS_DITHER_HEAVY : LS_DITHER_LIGHT,
                       flash ? (TUI_WHITE | TUI_BRIGHT) : h);
        tui_put_char(sf, b, key.x + 1, b.y, k ? '+' : '-',
                     TUI_ATTR(flash ? TUI_BLACK : c->live ? (TUI_WHITE | TUI_BRIGHT) : LS_DIM_FG,
                              flash ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK));
    }
}

/* How the deck lays out in the rows it is given: full cards when there is
   height for them, one-line controls across the width when there is not. */
static void deck_plan(tui_rect area, int want, int *n, int *card_h, int *cols)
{
    *n = 0; *card_h = 0; *cols = 0;
    if (want < 1 || area.h < 1 || area.w < 13) return;
    /* More cards beats taller cards: the deck would rather show squelch on a
       five row card than volume alone on a six row one. */
    if (area.w >= 20) {
        for (int ch = 6; ch >= 5; --ch)
            if ((area.h + 1) / (ch + 1) >= want) {
                *n = want; *card_h = ch; *cols = 1; return;
            }
        /* A short, wide pane - landscape with a keyboard under it - stands
           the cards in two columns rather than dropping half of them. */
        if (area.w >= 56) {
            const int rows = (area.h + 1) / 6;
            if (rows >= 1 && rows * 2 >= want) {
                *n = want; *card_h = 5; *cols = 2; return;
            }
        }
        const int fit = (area.h + 1) / 6;
        if (fit >= 1) { *n = fit > want ? want : fit; *card_h = 5; *cols = 1; return; }
    }
    *cols = area.w >= 60 ? 4 : area.w >= 30 ? 2 : 1;
    if (*cols > want) *cols = want;
    const int rows = (want + *cols - 1) / *cols;
    *n = *cols * (area.h < rows ? area.h : rows);
    if (*n > want) *n = want;
    *card_h = 1;
}

static int deck_rows(tui_rect area, int want)
{
    int n, card_h, cols;
    deck_plan(area, want, &n, &card_h, &cols);
    if (!n) return 0;
    const int rows = (n + cols - 1) / cols;
    if (card_h > 1) return rows * card_h + (rows - 1);
    return rows;
}

static void deck_draw(tui_surface *sf, tui_rect area, const ls_radio_view_t *v, int want)
{
    memset(s_card, 0, sizeof(s_card));
    memset(s_card_down, 0, sizeof(s_card_down));
    memset(s_card_up, 0, sizeof(s_card_up));
    memset(s_card_val, 0, sizeof(s_card_val));
    memset(s_card_bar, 0, sizeof(s_card_bar));
    s_cards = 0;
    int n, card_h, cols;
    deck_plan(area, want, &n, &card_h, &cols);
    if (!n) return;
    s_cards = n;
    for (int i = 0; i < n; ++i) {
        s_card_id[i] = (uint8_t)i;
        card_t c;
        card_fill(&c, i, v);
        if (card_h > 1) {
            const int rows = (n + cols - 1) / cols;
            const int used = rows * card_h + (rows - 1);
            const int y0 = area.y + (area.h - used) / 2;
            const int cw = cols > 1 ? area.w / cols : area.w;
            s_card[i] = tui_rect_make(area.x + (i % cols) * cw,
                                      y0 + (i / cols) * (card_h + 1),
                                      cols > 1 ? cw - 1 : area.w, card_h);
            card_draw(sf, i, &c);
        } else {
            const int cw = area.w / cols;
            s_card[i] = tui_rect_make(area.x + (i % cols) * cw, area.y + i / cols,
                                      cw - 1, 1);
            card_draw_line(sf, i, &c);
        }
    }
}

/* --------------------------------------------------------------- the glass */

/* Rows the glass wants here: enough for the rail and the block figures, and
   the meter, the gate and some trace when the pane can pay for them. */
static int glass_rows(tui_rect area, bool wide)
{
    if (wide) return area.h >= 6 ? 6 : area.h;
    if (area.h >= 24) return 14;
    if (area.h >= 18) return 11;
    return area.h >= 9 ? 9 : area.h;
}

static void glass(tui_surface *sf, tui_rect r, const ls_radio_panel_t *p,
                  const ls_radio_view_t *v, bool wide)
{
    s_gate = tui_rect_make(0, -1, 0, 0);
    if (r.h < 6 || r.w < 20) return;
    const bool framed = !wide && r.h >= 12 && r.w >= 34;
    tui_rect in = r;
    if (framed) {
        /* Not "SCANNER": the word SCAN belongs to the button that starts one,
           and a title a thumb cannot press must not wear a button's name. */
        ls_radio_bezel(sf, r, " RECEIVER ");
        in = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    }
    const bool active = scan_engine_active();
    const bool band = scan_engine_get_source() == SCAN_SRC_BAND;
    const scan_channel_t *ch = active && !band ? scan_channel_get(scan_engine_candidate())
                                               : NULL;
    const uint32_t hz = v->receiver.effective_center_known
                            ? (uint32_t)v->receiver.effective_center_hz
                            : v->frequency;

    /* The rail: what the scanner is, and the states a thumb put it in. */
    int x = in.x;
    x = ls_radio_chip(sf, in, x, in.y, band ? "BAND" : "CHANNELS", true,
             band ? TUI_YELLOW : TUI_BLUE);
    x = ls_radio_chip(sf, in, x, in.y, active ? "RUN" : "IDLE", active, TUI_GREEN);
    if (scan_engine_manual_hold()) x = ls_radio_chip(sf, in, x, in.y, "HOLD", true, TUI_RED);
    if (!band && scan_engine_mixed()) x = ls_radio_chip(sf, in, x, in.y, "MIX", true, TUI_MAGENTA);
    if (!band && scan_engine_location()) x = ls_radio_chip(sf, in, x, in.y, "GPS", true, TUI_CYAN);
    {
        char zone[10];
        const int z = scan_engine_get_zone();
        if (z < 0) snprintf(zone, sizeof(zone), "Z ALL");
        else snprintf(zone, sizeof(zone), "Z%d", z);
        x = ls_radio_chip(sf, in, x, in.y, zone, true, TUI_BLUE);
    }
    (void)x;

    /* The carrier, in block figures, and the meter: side by side where the
       pane is wide and short, stacked where it is tall. */
    tui_rect dial = tui_rect_make(in.x, in.y + 1, in.w, in.h - 1);
    tui_rect side = dial;
    if (wide && in.w >= 64) {
        dial.w = 34;
        side.x = dial.x + dial.w;
        side.w = in.w - dial.w;
    } else {
        side = tui_rect_make(in.x, in.y + 6, in.w, in.h - 6);
    }
    ls_radio_frequency_draw(sf, dial, 0, hz, active ? amber : white);

    int row = 0;
    char text[96];
    if (side.h > row) {
        if (ch) snprintf(text, sizeof(text), "%s  Z%d  %s", ch->name, ch->zone,
                         ch->mode == SCAN_MODE_P25 ? "P25" : "FM");
        else snprintf(text, sizeof(text), "MHz  %s", v->mode);
        tui_put_str(sf, side, side.x + (wide ? 1 : 0), side.y + row, text, cyan);
        row++;
    }
    if (side.h > row + 1) {
        meter(sf, side, side.x + (wide ? 1 : 0), side.y + row, side.w - (wide ? 2 : 0),
              v, true);
        row += 2;
    }
    if (side.h > row) {
        tui_put_str(sf, side, side.x + (wide ? 1 : 0), side.y + row,
                    !v->receiver.receiver_streaming        ? "RECEIVER OFFLINE"
                    : v->receiver.tune_state == LS_IQ_RESULT_PENDING ? "TUNE PENDING"
                    : v->receiver.tune_state == LS_IQ_RESULT_FAILED  ? "TUNE FAILED"
                    : v->detail[0][0]                      ? v->detail[0]
                                                            : "RECEIVER READY",
                    LS_ATTR_DIM);
        row++;
    }
    if (side.h > row + 1)
        trace_draw(sf, side, side.x + (wide ? 1 : 0), side.y + row,
                   side.w - (wide ? 2 : 1), side.h - row - (framed ? 0 : 1), v->gate);
    (void)p;
}

bool ls_radio_panel_control_rect(int card, int part, int *x, int *y, int *w, int *h)
{
    tui_rect r = tui_rect_make(0, -1, 0, 0);
    if (card == LS_RADIO_CARD_GATE) r = s_gate;
    else if (card >= 0 && card < s_cards)
        r = part == LS_RADIO_PART_UP      ? s_card_up[card]
          : part == LS_RADIO_PART_VALUE   ? s_card_val[card]
          : part == LS_RADIO_PART_BAR     ? s_card_bar[card]
                                          : s_card_down[card];
    if (r.h <= 0 || r.w <= 0) return false;
    if (x) *x = r.x;
    if (y) *y = r.y;
    if (w) *w = r.w;
    if (h) *h = r.h;
    return true;
}

/* What the deck and the glass do with a tap, before the button bar sees it.
   Priority: the gate slider, then a card's keys, then its value field.
   `out` carries a key back to the screen for the two controls the screen
   owns: a screen that handed over a setter is driven directly, and one that
   has not is driven through the quick controls it already answers, so the
   deck works either way and the keyboard keeps doing what it always did. */
static bool deck_touch(const ls_radio_view_t *v, int x, int y, char *out)
{
    *out = 0;
    const int sql_now = (int)(v->gate * 100.0f + 0.5f);
    /* The gate row is a slider: the column tapped is where the squelch
       opens. Coarse on purpose - exact entry is the value field, which is a
       different target, because a slider that also types is a slider you
       cannot trust. */
    if (s_gate.h > 0 && tui_rect_contains(s_gate, x, y)) {
        if (!v->set_squelch_pct) return true;
        const int pct = s_gate.w > 1 ? (x - s_gate.x) * 100 / (s_gate.w - 1) : 0;
        v->set_squelch_pct(clampi(pct, 0, 100));
        return true;
    }
    for (int i = 0; i < s_cards; ++i) {
        const int id = s_card_id[i];
        const bool down = s_card_down[i].h > 0 && tui_rect_contains(s_card_down[i], x, y);
        const bool up   = s_card_up[i].h > 0 && tui_rect_contains(s_card_up[i], x, y);
        const bool bar  = s_card_bar[i].h > 0 && tui_rect_contains(s_card_bar[i], x, y);
        if (!down && !up && !bar &&
            !(s_card_val[i].h > 0 && tui_rect_contains(s_card_val[i], x, y)))
            continue;
        /* THE TRAVEL SETS, IT DOES NOT TYPE. One write, where it was pressed,
           in the card's own range: the column under the thumb is the value.
           Nothing here runs from a draw and nothing repeats. */
        if (bar) {
            const int span = s_card_bar[i].w > 1 ? s_card_bar[i].w - 1 : 1;
            int pos = x - s_card_bar[i].x;
            if (pos < 0) pos = 0;
            if (pos > span) pos = span;
            switch (id) {
            case CARD_VOL:
                if (v->set_volume_pct) v->set_volume_pct(clampi(pos * 100 / span, 0, 100));
                break;
            case CARD_SQL:
                if (v->set_squelch_pct) v->set_squelch_pct(clampi(pos * 100 / span, 0, 100));
                break;
            case CARD_HANG: {
                /* Half seconds, the step its keys use, so the travel and the
                   keys cannot disagree about what a value is. */
                const int ms = pos * 30000 / span;
                scan_engine_set_hang_ms(clampi((ms + 250) / 500 * 500, 0, 30000));
                break;
            }
            default:
                scan_engine_set_threshold_pct(clampi(1 + pos * 99 / span, 1, 100));
                break;
            }
            return true;
        }
        if (down || up) {
            const int sign = up ? 1 : -1;
            switch (id) {
            case CARD_VOL:
                if (v->set_volume_pct) v->set_volume_pct(clampi(v->volume + sign * 5, 0, 100));
                else *out = up ? '+' : '-';
                break;
            case CARD_SQL:
                if (v->set_squelch_pct) v->set_squelch_pct(clampi(sql_now + sign * 2, 0, 100));
                else if (v->has_squelch) *out = up ? 's' : 'a';
                break;
            case CARD_HANG:
                scan_engine_set_hang_ms(clampi(scan_engine_get_hang_ms() + sign * 500,
                                               0, 30000));
                break;
            default:
                scan_engine_set_threshold_pct(clampi(scan_engine_get_threshold_pct() + sign * 2,
                                                     1, 100));
                break;
            }
            s_flash = i; s_flash_up = up; s_flash_ttl = FLASH_FRAMES;
            return true;
        }
        switch (id) {
        case CARD_VOL:
            if (!v->set_volume_pct) { *out = 'V'; break; }
            s_set_vol = v->set_volume_pct;
            ls_numpad_open("VOLUME", "0 to 100", v->volume, numpad_vol);
            break;
        case CARD_SQL:
            if (!v->set_squelch_pct) { if (v->has_squelch) *out = 'Q'; break; }
            s_set_sql = v->set_squelch_pct;
            ls_numpad_open("SQUELCH", "percent of quieting", sql_now, numpad_sql);
            break;
        case CARD_HANG:
            ls_numpad_open("HANG TIME", "seconds, 0 to 30",
                           scan_engine_get_hang_ms() / 1000.0, numpad_hang);
            break;
        default:
            ls_numpad_open("OPEN AT", "percent of quieting",
                           scan_engine_get_threshold_pct(), numpad_thresh);
            break;
        }
        return true;
    }
    return false;
}

static void buttons(ls_radio_panel_t *p, const ls_radio_view_t *v)
{
    bool active = scan_engine_active(), hold = scan_engine_manual_hold();
    ls_btn_t b[] = {{active ? (hold ? "RESUME" : "HOLD") : (v->fm ? "A / B" : "SCAN"), NULL,
                     active  ? 'H'
                     : v->fm ? 'A'
                             : 'S',
                     hold, false},
                    {active ? "NEXT" : "TUNE", NULL, active ? 'N' : 'T', false, false},
                    {active  ? "STOP"
                     : v->fm ? "SCAN"
                             : "SIGNAL",
                     NULL,
                     active  ? 'X'
                     : v->fm ? 'S'
                             : 'W',
                     active, false},
                    {"LISTS", NULL, 'L', false, false},
                    /* Squelch has a card on the deck now, so the idle slot
                       carries the one thing six buttons cannot: everything
                       else the scanner can be told. 'q' still opens exact
                       squelch entry from the keyboard. */
                    {active  ? "SKIP"
                     : v->fm ? "SETUP"
                             : "SETTINGS",
                     NULL,
                     active  ? 'K'
                     : v->fm ? 'C'
                             : 'P',
                     false, false},
                    {"MORE", NULL, 'M', false, false}};
    if (p->lists) {
        ls_btn_t l[] = {
            {"SAVE FREQ", NULL, 'F', false, false}, {"ENABLE", NULL, 'E', false, p->count == 0},
            {"ZONE", NULL, 'Z', false, false},      {"SCAN TYPE", NULL, 'O', false, false},
            {"RANGE", NULL, 'R', false, false},     {"BACK", NULL, 'M', false, false}};
        memcpy(b, l, sizeof(b));
    }
    if (p->scan_choice) {
        bool band = scan_engine_get_source() == SCAN_SRC_BAND;
        ls_btn_t choices[] = {
            {"CHANNEL LIST", NULL, 'C', !band, false}, {"BAND SCAN", NULL, 'B', band, false},
            {band ? "STEP" : "MIXED AUDIO", NULL, band ? 'I' : 'X', !band && scan_engine_mixed(), false},
            {band ? "RANGE" : "GPS FILTER", NULL, band ? 'R' : 'G', !band && scan_engine_location(), false},
            {"START", NULL, 'S', false, false},        {"BACK", NULL, 'M', false, false}};
        memcpy(b, choices, sizeof(b));
    }
    memcpy(p->buttons, b, sizeof(b));
}

static void channel_list(ls_radio_panel_t *p, const ls_radio_view_t *v, tui_surface *sf, tui_rect a)
{
    p->count = 0;
    for (int i = 0; i < scan_channels_count(); i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (c && (c->mode == (v->fm ? SCAN_MODE_NFM : SCAN_MODE_P25) ||
                  (scan_engine_mixed() && c->mode <= SCAN_MODE_NFM)))
            p->visible[p->count++] = i;
    }
    if (!p->lists && scan_engine_get_source() == SCAN_SRC_CHANNELS) {
        for (int i = 0; i < p->count; i++)
            if (p->visible[i] == scan_engine_candidate()) {
                p->selected = i;
                break;
            }
    }
    if (p->selected >= p->count)
        p->selected = p->count ? p->count - 1 : 0;
    /* What SETUP's per-channel rows act on: whatever the list is showing. */
    s_sel_ch = p->count ? p->visible[p->selected] : -1;
    int rows = (a.h - 7) / 3;
    if (rows < 1)
        rows = 1;
    p->first = (p->selected / rows) * rows;
    p->list_area = a;
    p->previous = tui_rect_make(a.x + 1, a.y + a.h - 4, (a.w - 2) / 2, 3);
    p->next =
        tui_rect_make(p->previous.x + p->previous.w, p->previous.y, a.w - 2 - p->previous.w, 3);
    tui_box(sf, a, p->lists ? "SAVED CHANNELS / UP-DOWN SELECT" : "SCAN LIST", cyan);
    if (p->lists && p->count > rows) {
        tui_box(sf, p->previous, "< PREV", cyan);
        tui_box(sf, p->next, "NEXT >", cyan);
    }
    if (!p->count) {
        line(sf, a, 2, "No channels saved for this mode", white);
        line(sf, a, 4, "LISTS > SAVE FREQ, or choose BAND", LS_ATTR_DIM);
    }
    for (int n = 0; n < rows && p->first + n < p->count; n++) {
        int pos = p->first + n, idx = p->visible[pos];
        const scan_channel_t *c = scan_channel_get(idx);
        if (!c)
            continue;
        char text[64];
        bool current =
            scan_engine_get_source() == SCAN_SRC_CHANNELS && scan_engine_candidate() == idx;
        snprintf(text, sizeof(text), "%c %s %lu.%04lu  Z%d %s",
                 p->lists ? (pos == p->selected ? '>' : ' ') : (current ? '>' : ' '),
                 c->flags & SCAN_FLAG_ENABLED ? "[x]" : "[ ]",
                 (unsigned long)(c->freq_hz / 1000000),
                 (unsigned long)((c->freq_hz % 1000000) / 100), c->zone,
                 c->flags & SCAN_FLAG_LOCKOUT ? "LOCK" : "");
        line(sf, a, 2 + n * 3, text, (p->lists ? pos == p->selected : current) ? amber : white);
        snprintf(text, sizeof(text), "%s %s", c->mode == SCAN_MODE_P25 ? "P25" : "FM", c->name);
        line(sf, a, 3 + n * 3, text, LS_ATTR_DIM);
    }
}

static void scan_choices(tui_surface *sf, tui_rect a, bool wide)
{
    bool band = scan_engine_get_source() == SCAN_SRC_BAND;
    tui_rect list = a, range = a;
    if (wide) {
        list.w = (a.w - 2) / 2;
        range.x = list.x + list.w + 2;
        range.w = a.w - list.w - 2;
    } else {
        list.h = 10;
        range.y = list.y + 11;
        range.h = a.h - 11;
    }
    tui_box(sf, list, band ? "CHANNEL LIST" : "CHANNEL LIST / SELECTED", band ? cyan : amber);
    line(sf, list, 2, "Visit saved frequencies only", white);
    line(sf, list, 4, "Uses your enabled channels and zone", LS_ATTR_DIM);
    line(sf, list, 6, "Manage frequencies with LISTS", LS_ATTR_DIM);
    line(sf, list, 8, scan_engine_location() ? "GPS coverage filter ON" : "GPS coverage filter OFF", cyan);
    tui_box(sf, range, band ? "BAND SCAN / SELECTED" : "BAND SCAN", band ? amber : cyan);
    uint32_t lo, hi, step;
    scan_engine_get_band(&lo, &hi, &step);
    char text[64];
    line(sf, range, 2, "Sweep every frequency step", white);
    snprintf(text, sizeof(text), "FROM %.4f TO %.4f MHz", lo / 1e6, hi / 1e6);
    line(sf, range, 4, text, white);
    snprintf(text, sizeof(text), "STEP %.2f kHz / no list needed", step / 1000.0);
    line(sf, range, 6, text, amber);
    line(sf, range, 8, "RANGE chooses band / STEP changes spacing", LS_ATTR_DIM);
}

/* Large text leaves 34x41 cells in portrait and 79x17 in landscape. Keep
   the same actions and hit testing, but spend fewer rows on decoration. */
static void compact_panel(ls_radio_panel_t *p, const ls_radio_view_t *v,
                          tui_surface *sf, tui_rect a)
{
    bool wide = a.w > 72, active = scan_engine_active();
    a.x++;
    a.w -= 2;
    int bh = wide ? 4 : 10;
    tui_rect controls = tui_rect_make(a.x, a.y + a.h - bh, a.w, bh);
    if (!wide) {
        /* Two columns fit SETTINGS and SCAN TYPE with padding. Letting a
           32-column pane choose three silently truncates both labels. */
        controls.x += (controls.w - 28) / 2;
        controls.w = 28;
    }
    tui_rect body = tui_rect_make(a.x, a.y + 4, a.w, controls.y - a.y - 5);
    tui_rect state = tui_rect_make(a.x, a.y, a.w, 3);
    char text[96];
    if (p->scan_choice) snprintf(text, sizeof(text), "CHOOSE SCAN / THEN START");
    else if (p->lists) snprintf(text, sizeof(text), "SAVED CHANNELS / ZONE %d", scan_engine_get_zone());
    else if (active) scan_engine_status(text, sizeof(text));
    else snprintf(text, sizeof(text), "%s / %s", v->mode,
                  v->receiver.receiver_streaming ? "MANUAL" : "NO USB RECEIVER");
    tui_box(sf, state, NULL, cyan);
    line(sf, state, 1, text, amber);
    if (p->lists && !p->scan_choice) {
        channel_list(p, v, sf, body);
    } else if (p->scan_choice) {
        tui_rect left = body, right = body;
        if (wide) {
            left.w = (body.w - 1) / 2;
            right.x += left.w + 1;
            right.w -= left.w + 1;
        } else {
            left.h = 10;
            right.y += 11;
            right.h -= 11;
        }
        bool band = scan_engine_get_source() == SCAN_SRC_BAND;
        tui_box(sf, left, band ? "CHANNELS" : "CHANNELS / SELECTED", cyan);
        line(sf, left, 2, "Visit saved frequencies", white);
        line(sf, left, 4, "Manage with LISTS", LS_ATTR_DIM);
        line(sf, left, 6, "Mixed audio: P25 I + NFM", LS_ATTR_DIM);
        tui_box(sf, right, band ? "BAND / SELECTED" : "BAND", amber);
        uint32_t lo, hi, step;
        scan_engine_get_band(&lo, &hi, &step);
        snprintf(text, sizeof(text), "%.3f - %.3f MHz", lo / 1e6, hi / 1e6);
        line(sf, right, 2, text, white);
        snprintf(text, sizeof(text), "Step %.2f kHz", step / 1000.0);
        line(sf, right, 4, text, amber);
        line(sf, right, 6, "Every step in this band", LS_ATTR_DIM);
    } else {
        /* The glass and the deck, the same two pieces the VFO page is made
           of. A scan running on the stored list keeps the list in view and
           the deck down to the two controls a scan actually needs. */
        const int gh = glass_rows(body, wide);
        glass(sf, tui_rect_make(body.x, body.y, body.w, gh), p, v, wide);
        tui_rect rest = tui_rect_make(body.x, body.y + gh, body.w, body.h - gh);
        const bool list = active && scan_engine_get_source() == SCAN_SRC_CHANNELS;
        /* Two columns of cards fit a short wide pane, so width buys the
           whole deck back that height could not. */
        const int want = list ? 2 : (rest.h >= 13 || rest.w >= 56) ? 4 : 2;
        const int dh = deck_rows(rest, want);
        if (list && rest.h - dh >= 10) {
            channel_list(p, v, sf, tui_rect_make(rest.x, rest.y, rest.w, rest.h - dh));
            rest.y += rest.h - dh;
            rest.h = dh;
        }
        deck_draw(sf, rest, v, want);
    }
    buttons(p, v);
    if (p->scan_choice) {
        p->buttons[0].label = "CHANNELS";
        p->buttons[1].label = "BAND";
        if (scan_engine_get_source() != SCAN_SRC_BAND) {
            p->buttons[2].label = "MIXED";
            p->buttons[3].label = "GPS";
        }
    }
    ls_btn_bar_raised(sf, controls, p->buttons, 6, p->focus);
}

void ls_radio_panel_draw(ls_radio_panel_t *p, const ls_radio_view_t *v, tui_surface *sf, tui_rect a)
{
    ls_radio_view_t actual = *v;
    if (scan_engine_active() && scan_engine_mixed()) {
        const scan_channel_t *c = scan_channel_get(scan_engine_candidate());
        if (c) {
            actual.fm = c->mode == SCAN_MODE_NFM;
            actual.mode = actual.fm ? "NFM / MIXED" : "P25 I / MIXED";
            actual.frequency = c->freq_hz;
            scan_engine_receiver_status(&actual.receiver);
            v = &actual;
        }
    }
    /* Once a frame, and only here: the activity trace advances by one reading
       and a key lit by a tap fades. Both are animation the panel owns, so
       neither costs the receiver anything. */
    trace_push(v->signal, v->has_squelch);
    if (s_flash_ttl > 0 && --s_flash_ttl == 0) s_flash = -1;
    bool wide = a.w > 72, active = scan_engine_active();
    if (a.w >= 32 && a.h >= (wide ? 14 : 30) && (a.w < 38 || a.h < 20)) {
        compact_panel(p, v, sf, a);
        return;
    }
    if (a.w < 38 || a.h < (wide ? 20 : 34)) {
        ls_panel_notice(sf, a, "RADIO", "Open a larger radio view", "M: detailed controls");
        ls_btn_clear_hits();
        return;
    }
    a.x++;
    a.w -= 2;
    char text[96];
    if (p->scan_choice)
        snprintf(text, sizeof(text), "CHOOSE SCAN TYPE / THEN START");
    else if (active) {
        char state[64];
        scan_engine_status(state, sizeof(state));
        snprintf(text, sizeof(text), "%s / %s",
                 scan_engine_get_source() == SCAN_SRC_BAND ? "BAND SCAN" : "CHANNEL LIST", state);
    } else
        snprintf(text, sizeof(text), "%s / %s", v->mode,
                 v->receiver.receiver_streaming ? "MANUAL" : "NO RECEIVER / CONNECT USB SDR");
    tui_rect status = tui_rect_make(a.x, a.y + 1, a.w, 3);
    tui_box(sf, status, NULL, cyan);
    line(sf, status, 1,
         scan_engine_manual_hold()
             ? (scan_engine_get_source() == SCAN_SRC_BAND ? "BAND SCAN / HOLD"
                                                          : "CHANNEL LIST / HOLD")
             : text,
         amber);
    int bh = wide ? 5 : 10;
    tui_rect controls = tui_rect_make(a.x, a.y + a.h - bh, a.w, bh);
    tui_rect body = tui_rect_make(a.x, a.y + 5, a.w, controls.y - a.y - 6);
    if (p->scan_choice) {
        scan_choices(sf, body, wide);
    } else if (p->lists) {
        uint32_t lo, hi, step;
        scan_engine_get_band(&lo, &hi, &step);
        snprintf(text, sizeof(text), "%s / ZONE %d / %.1fs hang",
                 scan_engine_get_source() == SCAN_SRC_BAND ? "BAND" : "CHANNELS",
                 scan_engine_get_zone(), scan_engine_get_hang_ms() / 1000.0);
        line(sf, body, 0, text, amber);
        snprintf(text, sizeof(text), "%.4f - %.4f MHz / %.1fk", lo / 1e6, hi / 1e6, step / 1000.0);
        line(sf, body, wide ? 1 : 2, text, white);
        line(sf, body, wide ? 2 : 4,
             p->notice[0] ? p->notice : "Zone -1 = all / changes stop scanning", LS_ATTR_DIM);
        body.y += wide ? 3 : 6;
        body.h -= wide ? 3 : 6;
        channel_list(p, v, sf, body);
    } else {
        /* THE GLASS AND THE DECK.

           One instrument, laid out the way the VFO page lays itself out: the
           carrier in block figures under a rail of annunciators, the meter
           and the gate beside or below it, and the controls as cards with
           keys a thumb can hit. A scan running on the stored list keeps the
           list on screen - watching it hop is most of the point - and the
           deck keeps the two controls a scan needs while the rest of them
           wait in SETUP. */
        const int gh = glass_rows(body, wide);
        glass(sf, tui_rect_make(body.x, body.y, body.w, gh), p, v, wide);
        tui_rect rest = tui_rect_make(body.x, body.y + gh, body.w, body.h - gh);
        const bool list = active && scan_engine_get_source() == SCAN_SRC_CHANNELS;
        /* Two columns of cards fit a short wide pane, so width buys the
           whole deck back that height could not. */
        const int want = list ? 2 : (rest.h >= 13 || rest.w >= 56) ? 4 : 2;
        const int dh = deck_rows(rest, want);
        if (list && rest.h - dh >= 10) {
            channel_list(p, v, sf, tui_rect_make(rest.x, rest.y, rest.w, rest.h - dh));
            rest.y += rest.h - dh;
            rest.h = dh;
        } else if (!list && rest.h - dh >= 6) {
            /* Idle, with rows to spare: the band the scan would walk, so
               BAND SCAN is never started blind. */
            uint32_t lo, hi, step;
            scan_engine_get_band(&lo, &hi, &step);
            snprintf(text, sizeof(text), "BAND %.4f - %.4f MHz / %.2fk STEP",
                     lo / 1e6, hi / 1e6, step / 1000.0);
            tui_put_str(sf, rest, rest.x + 1, rest.y, text, LS_ATTR_DIM);
            const int z = scan_engine_get_zone();
            char zone[8];
            if (z < 0) snprintf(zone, sizeof(zone), "ALL");
            else       snprintf(zone, sizeof(zone), "%d", z);
            /* No button's word in here: a line of text wearing a button's
               name is a target a thumb will try and nothing will happen. */
            snprintf(text, sizeof(text), "ZONE %s / %d SAVED CHANNELS / HANG %.1fs",
                     zone, scan_channels_count(), scan_engine_get_hang_ms() / 1000.0);
            tui_put_str(sf, rest, rest.x + 1, rest.y + 1, text, LS_ATTR_DIM);
            rest.y += 3;
            rest.h -= 3;
        }
        deck_draw(sf, rest, v, want);
    }
    buttons(p, v);
    ls_btn_bar_raised(sf, controls, p->buttons, 6, p->focus);
}

/* Band scan presets, cycled by RANGE. A band scan steps lo, lo+step, lo+2*step
   ... so lo and step together fix the raster, and every lo here is a multiple
   of its step counted from 0 Hz: the grid lands on the channel lattice, not on
   a round MHz.

   US VHF land mobile is not on a 12.5 kHz lattice. Its channels sit on a 2.5
   kHz-aligned raster (5, 7.5, 12.5, 15, 25 kHz families), and 154.785 is 2.5
   kHz off a grid that starts at 150.000 and steps 12.5. A 7.5 kHz step from a
   multiple of 7.5 kHz lands every channel that is itself a multiple of 7.5
   kHz (154.785 is step 638 from 150.000) and keeps any other 2.5 kHz-aligned
   channel within 2.5 kHz of a step, which the NFM filter takes; a 12.5 kHz
   step leaves up to 5 kHz. A 2.5 kHz step lands all of them and costs three
   times the pass. NOAA WX is its own 25 kHz preset so it lands exactly. UHF
   450-470 channels are 12.5 kHz centres, so 12.5 kHz from 450.000 lands them;
   press STEP for 6.25 kHz to reach the half-channel centres.

   A step costs about 230 ms (retune, then the NFM squelch's 100 ms settle and
   100 ms window), so the pass time is steps x 0.23 s. */
typedef struct {
    uint32_t lo, hi, step;
    const char *name;
} band_preset_t;

static const band_preset_t band_presets[] = {
    {150000000, 162600000, 7500, "VHF 150-162.6"},   /* 1681 steps, 6.4 min  */
    {136005000, 174000000, 7500, "VHF 136-174"},     /* 5067 steps, 19.4 min */
    {162400000, 162550000, 25000, "NOAA WX"},        /* 7 steps, 1.6 s       */
    {144000000, 148000000, 12500, "2m"},             /* 321 steps, 74 s      */
    {420000000, 450000000, 12500, "70cm"},           /* 2401 steps, 9.2 min  */
    {450000000, 470000000, 12500, "UHF 450-470"},    /* 1601 steps, 6.1 min  */
};
#define BAND_PRESETS ((int)(sizeof(band_presets) / sizeof(band_presets[0])))

/* The preset after the one the band is on now; the first when it is on none. */
static const band_preset_t *band_preset_next(void)
{
    uint32_t lo, hi;
    scan_engine_get_band(&lo, &hi, NULL);
    int next = 0;
    for (int i = 0; i < BAND_PRESETS; i++)
        if (band_presets[i].lo == lo && band_presets[i].hi == hi) {
            next = (i + 1) % BAND_PRESETS;
            break;
        }
    return &band_presets[next];
}

static char action(ls_radio_panel_t *p, const ls_radio_view_t *v, char c)
{
    ls_radio_view_t actual = *v;
    if (scan_engine_active() && scan_engine_mixed()) {
        const scan_channel_t *channel = scan_channel_get(scan_engine_candidate());
        if (channel) {
            actual.fm = channel->mode == SCAN_MODE_NFM;
            scan_engine_receiver_status(&actual.receiver);
            actual.frequency = actual.receiver.effective_center_known ?
                (uint32_t)actual.receiver.effective_center_hz : channel->freq_hz;
            v = &actual;
        }
    }
    if (c >= 'a' && c <= 'z')
        c -= 32;
    if (p->scan_choice) {
        if (c == 'M') {
            p->scan_choice = false;
            return 0;
        }
        if (c == 'C' || c == 'B') {
            scan_engine_stop();
            scan_engine_set_source(c == 'C' ? SCAN_SRC_CHANNELS : SCAN_SRC_BAND);
        } else if (c == 'X' && scan_engine_get_source() == SCAN_SRC_CHANNELS) {
            scan_engine_set_mixed(!scan_engine_mixed());
        } else if (c == 'G' && scan_engine_get_source() == SCAN_SRC_CHANNELS) {
            scan_engine_set_location(!scan_engine_location());
        } else if (c == 'S') {
            p->scan_choice = p->lists = false;
            scan_engine_start();
        } else if (c == 'I' && scan_engine_get_source() == SCAN_SRC_BAND) {
            static const uint32_t steps[] = {5000, 6250, 7500, 10000, 12500, 15000, 20000, 25000, 50000};
            const int n = (int)(sizeof(steps) / sizeof(steps[0]));
            uint32_t lo, hi, step;
            scan_engine_get_band(&lo, &hi, &step);
            int next = 0;
            for (int i = 0; i < n; i++)
                if (steps[i] == step) {
                    next = (i + 1) % n;
                    break;
                }
            scan_engine_set_band(lo, hi, steps[next]);
        } else if (c == 'R' && scan_engine_get_source() == SCAN_SRC_BAND) {
            const band_preset_t *b = band_preset_next();
            scan_engine_set_band(b->lo, b->hi, b->step);
        }
        return 0;
    }
    if (p->lists) {
        if (c == 'M') {
            p->lists = false;
            return 0;
        }
        if (c == 'F') {
            scan_engine_stop();
            int mode = v->fm ? SCAN_MODE_NFM : SCAN_MODE_P25;
            int zone = scan_engine_get_zone();
            if (zone < 0)
                zone = 0;
            if (v->fm && strcmp(v->mode, "NFM")) {
                snprintf(p->notice, sizeof(p->notice), "Select NFM before saving a scan channel");
                return 0;
            }
            char name[16];
            snprintf(name, sizeof(name), "%.4f MHz", v->frequency / 1e6);
            int idx = -1;
            for (int i = 0; i < scan_channels_count(); i++) {
                const scan_channel_t *saved = scan_channel_get(i);
                if (saved && saved->freq_hz == v->frequency && saved->zone == zone &&
                    saved->mode == mode) {
                    idx = i;
                    break;
                }
            }
            if (idx >= 0)
                snprintf(p->notice, sizeof(p->notice), "Already saved in zone %d", zone);
            else {
                idx = scan_channel_add(name, v->frequency, mode, zone);
                snprintf(p->notice, sizeof(p->notice),
                         idx >= 0 ? "Channel saved" : "Could not save channel / list full");
                if (idx >= 0 && v->fm && v->tone_required &&
                    !scan_channel_set_tone(idx, v->tone_required))
                    snprintf(p->notice, sizeof(p->notice), "Could not save receive tone");
            }
        } else if (c == 'E' && p->count) {
            scan_engine_stop();
            int idx = p->visible[p->selected];
            const scan_channel_t *ch = scan_channel_get(idx);
            if (ch)
                scan_channel_set_enabled(idx, !(ch->flags & SCAN_FLAG_ENABLED));
        } else if (c == 'Z') {
            scan_engine_stop();
            int z = scan_engine_get_zone() + 1;
            scan_engine_set_zone(z >= SCAN_MAX_ZONES ? -1 : z);
        } else if (c == 'O') {
            scan_engine_stop();
            p->scan_choice = true;
            p->focus = -1;
        } else if (c == 'R') {
            scan_engine_stop();
            const band_preset_t *b = band_preset_next();
            scan_engine_set_band(b->lo, b->hi, b->step);
            scan_engine_set_source(SCAN_SRC_BAND);
            snprintf(p->notice, sizeof(p->notice), "RANGE: %s / %.1fk step", b->name,
                     b->step / 1000.0);
        }
        return 0;
    }
    if (c == 'C' && v->fm) {
        /* SETUP: hang, the threshold, the priority look, the zone, the band
           edges, the step and the three per-channel flags - everything six
           buttons cannot hold, in the house options list. */
        ls_opt_open(ls_scan_options(v->fm, s_sel_ch));
    } else if (c == 'L') {
        p->lists = true;
        p->notice[0] = 0;
    } else if (c == 'S') {
        if (!scan_engine_active()) {
            p->scan_choice = true;
            p->focus = -1;
        }
    } else if (c == 'X')
        scan_engine_stop();
    else if (c == 'H') {
        if (scan_engine_manual_hold())
            scan_engine_next();
        else
            scan_engine_hold(true);
    } else if (c == 'N')
        scan_engine_next();
    else if (c == 'K')
        scan_engine_skip();
    else
        return c;
    return 0;
}

char ls_radio_panel_key(ls_radio_panel_t *p, const ls_radio_view_t *v, ls_tk_t k, char c)
{
    if (k == LS_TK_ESC) {
        if (p->scan_choice) {
            p->scan_choice = false;
            return 0;
        }
        if (p->lists) {
            p->lists = false;
            return 0;
        }
        return 'M';
    }
    if (p->lists && !p->scan_choice && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        if (p->count)
            p->selected = (p->selected + (k == LS_TK_UP ? p->count - 1 : 1)) % p->count;
        return 0;
    }
    /* Lists and scan setup retain navigation. The receiver owns left/right tuning. */
    if (!p->lists && !p->scan_choice && (k == LS_TK_LEFT || k == LS_TK_RIGHT)) {
        p->focus = -1;
        return k == LS_TK_LEFT ? '[' : ']';
    }
    if (k == LS_TK_CHAR)
        return action(p, v, c);
    if (k == LS_TK_ENTER && p->focus >= 0 && p->focus < 6)
        return action(p, v, p->buttons[p->focus].key);
    ls_btn_navigate(k, &p->slot, &p->focus, false);
    return 0;
}
char ls_radio_panel_touch(ls_radio_panel_t *p, const ls_radio_view_t *v, int x, int y)
{
    /* The deck first: its keys and the gate slider are inside the body, so a
       tap there is never a tap on the bar, and the bar's own hit table only
       covers the rows it drew. */
    if (!p->lists && !p->scan_choice) {
        char pass = 0;
        if (deck_touch(v, x, y, &pass)) return pass;
    }
    if (p->lists && !p->scan_choice &&
        (tui_rect_contains(p->previous, x, y) || tui_rect_contains(p->next, x, y))) {
        int rows = (p->list_area.h - 7) / 3;
        if (rows < 1)
            rows = 1;
        p->selected += tui_rect_contains(p->previous, x, y) ? -rows : rows;
        if (p->selected < 0)
            p->selected = 0;
        if (p->selected >= p->count)
            p->selected = p->count ? p->count - 1 : 0;
        return 0;
    }
    if (p->lists && !p->scan_choice && tui_rect_contains(p->list_area, x, y)) {
        int rows = (p->list_area.h - 7) / 3;
        if (rows < 1) rows = 1;
        int pos = p->first + (y - p->list_area.y - 2) / 3;
        if (y >= p->list_area.y + 2 && pos < p->first + rows && pos < p->count)
            p->selected = pos;
        return 0;
    }
    int hit = ls_btn_hit(x, y);
    return hit >= 0 && hit < 6 ? action(p, v, p->buttons[hit].key) : 0;
}
