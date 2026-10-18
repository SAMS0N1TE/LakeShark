#include "ls_radio_panel.h"
#include "ls_numpad.h"
#include "ls_glyph.h"
#include "ls_tui.h"
#include "ls_tui_screen.h"
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

void ls_radio_digits_draw(tui_surface *sf, tui_rect a, int row, const char *text,
                          uint8_t attr)
{
    static const uint8_t digits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};
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

void ls_radio_frequency_draw(tui_surface *sf, tui_rect a, int row, uint32_t hz,
                             uint8_t attr)
{
    char text[20];
    snprintf(text,sizeof(text),"%lu.%04lu",(unsigned long)(hz/1000000),
             (unsigned long)((hz%1000000)/100));
    ls_radio_digits_draw(sf,a,row,text,attr);
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
void ls_radio_filament(tui_surface *sf, tui_rect box, int x, int y, int rows, uint8_t hue)
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
            ls_radio_filament(sf, box, bx + s * 2, y, 1, hue);
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
    if (b.w < 20 || b.h < 5 || (c->thumb_keys && (b.w < 28 || b.h < 6))) return;
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
    const int key_w = c->thumb_keys ? 8 : b.w >= 30 ? 6 : 5;

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
    int bar_x = b.x + 2;
    if (c->thumb_keys) {
        /* Exact entry has its own filled thumb key; its travel never overlaps. */
        c->value_rect = c->live
            ? tui_rect_make(b.x + 2, inner_y, 8, inner_h)
            : tui_rect_make(0, -1, 0, 0);
        tui_rect edit = tui_rect_make(b.x + 2, inner_y, 8, inner_h);
        ls_fill_dither(sf, edit, c->live ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, h);
        for (int x = 0; x < edit.w; ++x) {
            tui_put_char(sf, b, edit.x + x, edit.y, '=', edge);
            tui_put_char(sf, b, edit.x + x, edit.y + edit.h - 1, '=', edge);
        }
        tui_put_str(sf, edit, edit.x + 1, edit.y + edit.h / 2, field, face);

        bar_x = edit.x + edit.w + 2;
    } else {
        c->value_rect = c->live
            ? tui_rect_make(field_x, label_y, right - field_x + 1, 1)
            : tui_rect_make(0, -1, 0, 0);
    }
    const int bar_w = c->down.x - 2 - bar_x;
    const int segs = bar_w / 2;
    if (segs >= 2) {
        const int lit = (int)(c->frac * (float)segs + 0.5f);
        for (int s = 0; s < segs; ++s)
            ls_radio_filament(sf, b, bar_x + s * 2, inner_y, inner_h,
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
        ls_radio_filament(sf, box, x + s * 2, y, rows, hue);
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
                    {"BACK", NULL, 'M', false, false}};
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
    } else if (!ls_radio_scan_key(c))
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

/* ------------------------------------------------ the frequency keypad -- */

/* FM's VFO keypad, moved here so P25 draws the same one rather than a copy
   that drifts. Drawing only: the caller owns the entry text and the rects. */
void ls_freq_key_draw(tui_surface *sf, tui_rect k, const char *cap, char badge,
                      uint8_t hue, bool strong)
{
    if (k.w < 3 || k.h < 1) return;
    ls_fill_dither(sf, k, strong ? LS_DITHER_HEAVY : LS_DITHER_LIGHT, hue);
    const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
    const uint8_t face = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    if (k.h >= 3) {
        for (int x = 0; x < k.w; ++x) {
            tui_put_char(sf, k, k.x + x, k.y, '=', edge);
            tui_put_char(sf, k, k.x + x, k.y + k.h - 1, '=', edge);
        }
    }
    const int n = (int)strlen(cap);
    if (n == 1 && k.h >= 5 && k.w >= 9 && ls_glyph_has(cap[0])) {
        ls_glyph_draw(sf, k, cap[0], face);
    } else if (n == 1 && cap[0] == '.' && k.h >= 5) {
        /* A point the size of the point on the figures beside it. */
        const int px = k.x + (k.w - 3) / 2, py = k.y + k.h - 3;
        for (int yy = 0; yy < 2; ++yy)
            for (int xx = 0; xx < 3; ++xx)
                tui_put_char(sf, k, px + xx, py + yy, LS_TUI_SHADE_FULL, face);
    } else {
        const int x = k.x + (k.w - n) / 2, y = k.y + (k.h - 1) / 2;
        if (x - 1 > k.x) tui_put_char(sf, k, x - 1, y, ' ', face);
        if (x + n < k.x + k.w - 1) tui_put_char(sf, k, x + n, y, ' ', face);
        tui_put_str(sf, k, x, y, cap, face);
    }
    if (badge && k.w >= 5)
        tui_put_char(sf, k, k.x + 1, k.y, badge, edge);
}

void ls_freq_pad_draw(tui_surface *sf, tui_rect area, const char *entry,
                      bool invalid, uint32_t blink, tui_rect keys[12],
                      tui_rect actions[3])
{
    static const char *const KEY[12] = { "1", "2", "3", "4", "5", "6",
                                         "7", "8", "9", ".", "0", "DEL" };
    static const char BADGE[12] = { '1','2','3','4','5','6','7','8','9','.','0',0 };
    static const char *const ACT[3] = { "CLEAR", "CANCEL", "TUNE" };
    const uint8_t edge = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    memset(keys, 0, 12 * sizeof(tui_rect));
    memset(actions, 0, 3 * sizeof(tui_rect));

    tui_put_str(sf, area, area.x + 2, area.y, "FREQUENCY / MHz", edge);
    char edit[20];
    snprintf(edit, sizeof(edit), "%s%s", entry, ((blink / 8) & 1) ? "|" : "_");
    tui_put_str(sf, area, area.x + 2, area.y + 1, "MHz", edge);
    tui_put_str(sf, area, area.x + 6, area.y + 1, edit,
                TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    if (invalid && area.w >= 32)
        tui_put_str(sf, area, area.x + area.w - 15, area.y + 1, "INVALID RANGE",
                    TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));

    const int top = area.y + 2;
    const int avail = area.y + area.h - top;
    if (avail < 4 || area.w < 18) return;

    /* A key is only a key if the figure on it is the size of a fingertip,
       and the figure wants five rows. Portrait has the height for four rows
       of three; landscape has none and all the width in the world, so the
       same twelve go two deep and six across with the actions beside them. */
    const bool flat = ls_tui_is_wide() && area.w >= 72;
    const int rows = flat ? 2 : 4;
    const int cols = flat ? 6 : 3;
    /* Row-major slot for each key, so '.' and '0' keep a sane order. */
    static const int FLAT_SLOT[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 9, 11 };
    const int act_w = flat ? area.w / 4 : 0;
    const int grid_w = area.w - act_w;
    const int key_h = flat ? avail / rows
                           : (avail - (avail / 5 < 3 ? 3 : avail / 5)) / rows;
    const int pad_h = key_h * rows;
    if (key_h < 1) return;

    for (int i = 0; i < 12; ++i) {
        const int slot = flat ? FLAT_SLOT[i] : i;
        const int row = slot / cols, col = slot % cols;
        const int x0 = area.x + grid_w * col / cols;
        const int x1 = area.x + grid_w * (col + 1) / cols;
        keys[i] = tui_rect_make(x0, top + row * key_h, x1 - x0 - 1, key_h);
        ls_freq_key_draw(sf, keys[i], KEY[i], BADGE[i], TUI_CYAN, false);
    }

    if (flat) {
        const int ax = area.x + grid_w;
        const int half = act_w / 2;
        actions[0] = tui_rect_make(ax, top, half - 1, key_h);
        actions[1] = tui_rect_make(ax + half, top, act_w - half - 1, key_h);
        actions[2] = tui_rect_make(ax, top + key_h, act_w - 1, pad_h - key_h);
        for (int i = 0; i < 3; ++i)
            ls_freq_key_draw(sf, actions[i], ACT[i], 0, i == 2 ? TUI_GREEN : TUI_CYAN, i == 2);
        return;
    }
    const int act_y = top + pad_h;
    const int act_h = area.y + area.h - act_y;
    if (act_h < 1) return;
    for (int i = 0; i < 3; ++i) {
        const int x0 = area.x + area.w * i / 3;
        const int x1 = area.x + area.w * (i + 1) / 3;
        actions[i] = tui_rect_make(x0, act_y, x1 - x0 - 1, act_h);
        ls_freq_key_draw(sf, actions[i], ACT[i], 0, i == 2 ? TUI_GREEN : TUI_CYAN, i == 2);
    }
}

char ls_freq_pad_hit(const tui_rect keys[12], const tui_rect actions[3],
                     int col, int row)
{
    static const char KEY[12] = { '1','2','3','4','5','6','7','8','9','.','0','d' };
    static const char ACT[3] = { 'c', 'x', 't' };
    for (int i = 0; i < 12; ++i)
        if (keys[i].h > 0 && tui_rect_contains(keys[i], col, row)) return KEY[i];
    for (int i = 0; i < 3; ++i)
        if (actions[i].h > 0 && tui_rect_contains(actions[i], col, row)) return ACT[i];
    return 0;
}

void ls_freq_entry_add(char *entry, size_t cap, char c)
{
    size_t n = strlen(entry);
    if (n + 1 >= cap) return;
    if (c >= '0' && c <= '9' && strchr(entry, '.') &&
        strlen(strchr(entry, '.') + 1) >= 6) return;
    if (c == '.') {
        if (strchr(entry, '.')) return;
        if (!n) entry[n++] = '0';
        if (n + 1 >= cap) return;
    }
    entry[n] = c;
    entry[n + 1] = '\0';
}

void ls_freq_entry_delete(char *entry)
{
    size_t n = strlen(entry);
    if (n) entry[n - 1] = '\0';
}

/* Parse MHz as integer Hz, with at most six fractional places. */
bool ls_freq_entry_hz(const char *entry, uint32_t *out)
{
    uint64_t whole = 0, frac = 0;
    bool dot = false, digit = false;
    unsigned places = 0;
    for (const char *p = entry; *p; ++p) {
        if (*p == '.') { if (dot) return false; dot = true; continue; }
        if (*p < '0' || *p > '9') return false;
        digit = true;
        if (!dot) {
            whole = whole * 10 + (unsigned)(*p - '0');
            if (whole > 1766) return false;
        } else {
            if (++places > 6) return false;
            frac = frac * 10 + (unsigned)(*p - '0');
        }
    }
    if (!digit) return false;
    while (places++ < 6) { frac *= 10; }
    const uint64_t hz = whole * 1000000u + frac;
    if (hz < 24000000u || hz > 1766000000u) return false;
    *out = (uint32_t)hz;
    return true;
}

/* Shared receiver LCD; state belongs to the calling screen. */
uint32_t ls_radio_glass_tick(ls_radio_glass_t *g, uint32_t hz)
{
    if (hz != g->dial_target) {
        g->dial_target = hz;
        g->dial_frames = 10;
        if (g->dial_hz == 0) g->dial_hz = hz;
    }
    if (g->dial_frames > 0) {
        g->dial_frames--;
        const int64_t gap = (int64_t)g->dial_target - (int64_t)g->dial_hz;
        g->dial_hz = (uint32_t)((int64_t)g->dial_hz + gap / 3);
        if (g->dial_frames == 0) g->dial_hz = g->dial_target;
    } else {
        g->dial_hz = g->dial_target;
    }
    return g->dial_hz ? g->dial_hz : hz;
}

#define LCD_FIG_W 4
#define LCD_FIG_H 5
static const char *const LCD_FIG[11][LCD_FIG_H] = {
    {"XXXX","X..X","X..X","X..X","XXXX"},   /* 0 */
    {"..XX","..XX","..XX","..XX","..XX"},   /* 1 */
    {"XXXX","...X","XXXX","X...","XXXX"},   /* 2 */
    {"XXXX","...X","XXXX","...X","XXXX"},   /* 3 */
    {"X..X","X..X","XXXX","...X","...X"},   /* 4 */
    {"XXXX","X...","XXXX","...X","XXXX"},   /* 5 */
    {"XXXX","X...","XXXX","X..X","XXXX"},   /* 6 */
    {"XXXX","...X","...X","...X","...X"},   /* 7 */
    {"XXXX","X..X","XXXX","X..X","XXXX"},   /* 8 */
    {"XXXX","X..X","XXXX","...X","XXXX"},   /* 9 */
    {"....","....","XXXX","....","...."},   /* - */
};

static int lcd_fig(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    return c == '-' ? 10 : -1;
}

static int lcd_char_w(char c, int xs)
{
    if (c == '.') return 2 * xs;
    return lcd_fig(c) >= 0 ? LCD_FIG_W * xs : 0;
}

/* The field a string of figures needs, one blank column between them. */
static int lcd_text_w(const char *s, int xs)
{
    int w = 0;
    for (const char *p = s; *p; ++p) {
        const int cw = lcd_char_w(*p, xs);
        if (cw) w += cw + 1;
    }
    return w > 0 ? w - 1 : 0;
}

static void lcd_char(tui_surface *sf, tui_rect box, int x, int y, char c,
                     int xs, uint8_t attr, char ink)
{
    if (c == '.') {
        for (int i = 0; i < 2 * xs; ++i)
            tui_put_char(sf, box, x + i, y + LCD_FIG_H - 1, ink, attr);
        return;
    }
    const int f = lcd_fig(c);
    if (f < 0) return;
    for (int r = 0; r < LCD_FIG_H; ++r)
        for (int c2 = 0; c2 < LCD_FIG_W; ++c2)
            if (LCD_FIG[f][r][c2] == 'X')
                for (int k = 0; k < xs; ++k)
                    tui_put_char(sf, box, x + c2 * xs + k, y + r, ink, attr);
}

/* `now` is what the dial is showing and `settled` where it is going, so a
   digit still moving is drawn in the moving colour and the retune is visible
   as the thing it is. `pad` is the unlit remainder - every segment of the
   figures not yet typed, faintly, which is what an LCD looks like with half
   a frequency entered and is how you can see how many are left. */
#define LCD_PAD "8888.8888"

static void lcd_number(tui_surface *sf, tui_rect box, int x, int y,
                       const char *now, const char *settled, int xs,
                       uint8_t lit, uint8_t moving, const char *pad)
{
    const uint8_t ghost = TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK);
    int cx = x, n = 0;
    const int settled_n = settled ? (int)strlen(settled) : 0;
    for (const char *p = now; *p; ++p) {
        const int cw = lcd_char_w(*p, xs);
        if (!cw) continue;
        const bool diff = n < settled_n && settled[n] != *p;
        lcd_char(sf, box, cx, y, *p, xs, diff ? moving : lit, LS_TUI_SHADE_FULL);
        cx += cw + 1;
        n++;
    }
    if (!pad) return;
    /* Six decimals can type past the ghost: stop at its end, never read beyond. */
    const int pad_n = (int)strlen(pad);
    for (int i = n; i < pad_n; ++i) {
        const int cw = lcd_char_w(pad[i], xs);
        if (!cw) continue;
        lcd_char(sf, box, cx, y, pad[i], xs, ghost, LS_TUI_SHADE_25);
        cx += cw + 1;
    }
}

/* An annunciator: a lit field with a word on it, exactly as dense as the
   buttons on the bar above, because it is the same vocabulary. */
int ls_radio_glass_chip(tui_surface *sf, tui_rect box, int x, int y,
                    const char *text, bool on, uint8_t hue)
{
    const int n = (int)strlen(text);
    tui_rect chip = tui_rect_make(x, y, n + 2, 1);
    ls_fill_dither(sf, chip, on ? LS_DITHER_HEAVY : LS_DITHER_LIGHT,
                   on ? hue : (TUI_BLACK | TUI_BRIGHT));
    tui_put_str(sf, box, x + 1, y, text,
                TUI_ATTR(on ? (TUI_WHITE | TUI_BRIGHT) : LS_DIM_FG, TUI_BLACK));
    return x + n + 3;
}

/* THE ACTIVITY TRACE.

   The reference radio prints a little band picture inside its glass. This is
   the honest version of it: not a second spectrum - that is what the SPECTRUM
   page is for, and this page asks the receiver for nothing extra - but the
   last hundred frames of the same quieting measurement the meter and the
   squelch already read, scrolling right to left. */
#define GLASS_TRACE_N LS_RADIO_TRACE_N

void ls_radio_glass_sample(ls_radio_glass_t *g, float signal)
{
    if (!(signal >= 0.0f)) signal = 0.0f;
    if (signal > 1.0f) signal = 1.0f;
    g->trace[g->trace_head] = (uint8_t)(signal * 255.0f);
    g->trace_head = (g->trace_head + 1) % LS_RADIO_TRACE_N;
    if (g->trace_n < LS_RADIO_TRACE_N) g->trace_n++;
}

/* LINES, NOT SLABS.

   Filling these cells solid turned the history into one coloured rectangle:
   a hundred readings all touching each other read as a block, not as a hundred
   readings. LS_TUI_TRACE is the house primitive for exactly this - a two pixel
   line standing in the middle of the cell, eighths of a cell tall - so each
   sample is its own lit filament with the cell's other eight pixels left
   black on either side. That is what a vacuum display's bargraph looks like,
   and it is the same ramp of colour it always was. */
void ls_radio_glass_trace(const ls_radio_glass_t *g, tui_surface *sf, tui_rect box,
                          int x0, int y, int w, int h, float threshold)
{
    if (w <= 0 || h <= 0) return;
    const int gate = (int)(threshold * 8 * h);
    for (int i = 0; i < w; ++i) {
        const int idx = g->trace_n - w + i;
        const int v = (idx >= 0 && idx < g->trace_n) ? g->trace[(g->trace_head - g->trace_n + idx + LS_RADIO_TRACE_N) % LS_RADIO_TRACE_N] : 0;
        const int q = v * h * 8 / 255;    /* eighths of a cell, up the column */
        for (int r = 0; r < h; ++r) {
            const int base = (h - 1 - r) * 8;
            const int fill = q - base;
            if (fill < 1) continue;
            const uint8_t hue = q < gate     ? TUI_CYAN
                              : base >= 6 * h ? (TUI_RED | TUI_BRIGHT)
                              : base >= 3 * h ? (TUI_YELLOW | TUI_BRIGHT)
                                              : (TUI_GREEN | TUI_BRIGHT);
            tui_put_char(sf, box, x0 + i, y + r,
                         LS_TUI_TRACE(fill > 8 ? 8 : fill),
                         TUI_ATTR(hue, TUI_BLACK));
        }
    }
}

/* The meter: one filament a segment, the unlit ones left standing in grey so
   the scale is readable with no signal on it at all. Peak hold and the
   squelch gate are the ones from the old panel and mean the same things. */
void ls_radio_glass_meter(ls_radio_glass_t *g, tui_surface *sf, tui_rect box,
                          int x0, int y, int w, int rows, float sig,
                          bool available, bool open, bool gate_row, float threshold,
                          const char *gate_label)
{
    /* Only this row band routes around the camera; the glass stays put. */
    tui_rect hole;
    if (ls_tui_cutout(&hole) && hole.y < y + rows + (gate_row ? 1 : 0) &&
        hole.y + hole.h > y) {
        if (hole.x > x0 && hole.x < x0 + w) w = hole.x - x0 - 1;
        else if (hole.x <= x0 && hole.x + hole.w > x0) {
            const int shift = hole.x + hole.w + 1 - x0;
            x0 += shift; w -= shift;
        }
    }
    const uint8_t lab = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    tui_put_str(sf, box, x0, y, "SIG", lab);
    const int bx = x0 + 4;
    const int bw = w - 10;
    if (bw < 6) return;
    if (!available) {
        tui_put_str(sf, box, bx, y, "--", LS_ATTR_DIM);
        return;
    }
    if (!(sig >= 0.0f)) sig = 0.0f;
    if (sig > 1.0f) sig = 1.0f;
    if (sig >= g->hold) g->hold = sig;
    else                   g->hold -= (g->hold - sig) * 0.06f;

    const int segs = bw / 2;
    if (segs < 2) return;
    const int lit  = (int)(sig * (float)segs + 0.5f);
    const int hold = (int)(g->hold * (float)segs);
    for (int s = 0; s < segs; ++s) {
        const bool on = s < lit;
        uint8_t hue = on ? (s >= segs * 3 / 4 ? (TUI_RED | TUI_BRIGHT)
                          : s >= segs / 2     ? (TUI_YELLOW | TUI_BRIGHT)
                                              : (TUI_GREEN | TUI_BRIGHT))
                         : (TUI_BLACK | TUI_BRIGHT);
        if (!on && s == hold && hold > lit) hue = TUI_WHITE | TUI_BRIGHT;
        ls_radio_filament(sf, box, bx + s * 2, y, rows, hue);
    }
    char pct[6];
    snprintf(pct, sizeof(pct), "%3d%%", (int)(sig * 100.0f + 0.5f));
    tui_put_str(sf, box, bx + segs * 2 + 1, y + rows / 2, pct,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    if (!gate_row) return;

    int gate = (int)(threshold * (float)segs);
    if (gate < 0) gate = 0;
    if (gate >= segs) gate = segs - 1;
    tui_put_str(sf, box, x0, y + rows, gate_label ? gate_label : "SQL",
                TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK));
    tui_put_char(sf, box, bx + gate * 2, y + rows, '^',
                 TUI_ATTR(open ? (TUI_GREEN | TUI_BRIGHT) : (TUI_CYAN | TUI_BRIGHT),
                          TUI_BLACK));
}

int ls_radio_glass_frequency(ls_radio_glass_t *g, tui_surface *sf, tui_rect r,
                             int y, uint32_t hz, const char *entry,
                             uint32_t blink, bool wide)
{
    const int x0 = r.x + 2, iw = r.w - 4, last = r.y + r.h - 2;
    const uint8_t val = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t key = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    char now[24], end[24];
    const uint32_t shown = ls_radio_glass_tick(g, hz);
    snprintf(end, sizeof(end), "%u.%04u", (unsigned)(hz / 1000000u),
             (unsigned)((hz % 1000000u) / 100u));
    snprintf(now, sizeof(now), "%u.%04u", (unsigned)(shown / 1000000u),
             (unsigned)((shown % 1000000u) / 100u));

    const char *figures = entry != NULL ? entry : now;
    const char *against = entry != NULL ? NULL : end;
    const char *pad = entry != NULL ? LCD_PAD : NULL;
    const char *field = pad ? pad : now;

    /* Landscape keeps a column for the readouts; portrait gives the whole
       width to the figures and puts the readouts under them. */
    const int fig_w = wide ? (iw * 2) / 3 : iw;
    int xs = 2;
    while (xs > 1 && lcd_text_w(field, xs) + 5 > fig_w) xs--;
    const bool big = last - y >= LCD_FIG_H &&
                     lcd_text_w(field, 1) + 5 <= fig_w;

    if (big) {
        const int w = lcd_text_w(field, xs);
        const int fx = wide ? x0 : x0 + (fig_w - w - 4) / 2;
        y++;
        lcd_number(sf, r, fx, y, figures, against, xs, val,
                   TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK), pad);
        /* The caret sits under the next figure, blinking, so a half-typed
           frequency says where the next press lands. */
        if (entry != NULL && ((blink / 8) & 1)) {
            const int cx = fx + lcd_text_w(entry, xs) +
                           (entry[0] ? 1 : 0);
            if (cx + LCD_FIG_W * xs <= fx + w)
                for (int i = 0; i < LCD_FIG_W * xs; ++i)
                    tui_put_char(sf, r, cx + i, y + LCD_FIG_H - 1,
                                 LS_TUI_SHADE_75, key);
        }
        tui_put_str(sf, r, fx + w + 2, y + LCD_FIG_H - 2, "MHz", key);
        y += LCD_FIG_H;
    } else if (entry != NULL) {
        /* Too few rows for the figures - a split landscape pane - but the
           glass still has to be where the typing shows up, or the entry is
           only in the echo beside the keys. */
        char line[24];
        snprintf(line, sizeof(line), "%s%s", entry,
                 ((blink / 8) & 1) ? "|" : "_");
        tui_put_str(sf, r, x0, y, "ENT", key);
        tui_put_str(sf, r, x0 + 4, y, line, val);
        y++;
    } else {
        /* The compact readout advances exactly as FM's old draw_dial did. */
        const uint32_t compact = ls_radio_glass_tick(g, hz);
        char line[24], settled[24];
        snprintf(line, sizeof(line), "%u.%04u MHz", (unsigned)(compact / 1000000u),
                 (unsigned)((compact % 1000000u) / 100u));
        snprintf(settled, sizeof(settled), "%u.%04u MHz", (unsigned)(hz / 1000000u),
                 (unsigned)((hz % 1000000u) / 100u));
        const int fx = wide ? x0 : r.x + (r.w - (int)strlen(line)) / 2;
        for (int i = 0; line[i]; ++i)
            tui_put_char(sf, r, fx + i, y, line[i],
                         settled[i] && settled[i] != line[i] ?
                         TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : val);
        y++;
    }

    return y;
}

void ls_radio_glass_wipe(ls_radio_glass_t *g, tui_surface *sf, tui_rect r,
                         int first_y, int y, int mode)
{
    const int last = r.y + r.h - 2;
    const uint8_t key = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    if (!g->mode_known || g->mode != mode) {
        if (g->mode_known) g->wipe_frames = 12;
        g->mode = mode;
        g->mode_known = true;
    }
    if (g->wipe_frames > 0) {
        const int col = (12 - g->wipe_frames) * (r.w - 2) / 12;
        for (int i = first_y; i < y && i <= last; ++i)
            tui_put_char(sf, r, r.x + 1 + col, i, LS_TUI_SHADE_50, key);
        g->wipe_frames--;
    }

}

void ls_radio_glass_bezel(tui_surface *sf, tui_rect r, const char *title)
{
    char cap[32];
    snprintf(cap, sizeof(cap), " %s ", title ? title : "");
    ls_radio_bezel(sf, r, NULL);
    if (title && r.w > (int)strlen(title) + 8)
        tui_put_str(sf, r, r.x + 3, r.y, cap,
                    TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
}

void ls_radio_glass_rail(tui_surface *sf, tui_rect r, int x0, int y, int iw,
                         const ls_radio_chip_t *left, int left_n,
                         const ls_radio_chip_t *right, int right_n)
{
    int x = x0;
    for (int i = 0; i < left_n; ++i) {
        if (!left[i].text) continue;
        if (i && x + (int)strlen(left[i].text) + 3 >= x0 + iw) break;
        x = ls_radio_glass_chip(sf, r, x, y, left[i].text, left[i].on, left[i].hue);
    }
    int width = 0;
    for (int i = 0; i < right_n; ++i) width += (int)strlen(right[i].text) + 3;
    int rx = x0 + iw - width + 1;
    if (rx <= x + 1) return;
    for (int i = 0; i < right_n; ++i)
        rx = ls_radio_glass_chip(sf, r, rx, y, right[i].text, right[i].on, right[i].hue);
}

/* One layout supplies both the unchanged paint and its touch rectangles. */
static void vfo_card_geometry(tui_rect b, ls_radio_card_t *c, char edit[20])
{
    const int right = b.x + b.w - 2;
    const int inner_y = b.y + 2, inner_h = b.h - 3, key_w = 8;
    snprintf(edit, 20, "[%s]", c->value);
    c->up = tui_rect_make(right - key_w + 1, inner_y, key_w, inner_h);
    c->down = tui_rect_make(right - 2 * key_w, inner_y, key_w, inner_h);
    int value_w = (int)strlen(edit);
    if (value_w > b.w - 12) value_w = b.w - 12;
    c->value_rect = tui_rect_make(b.x + 12, b.y + 1, value_w, 1);
    const int bar_x = b.x + 2, bar_w = c->down.x - 2 - bar_x;
    c->bar = bar_w / 2 >= 2 ? tui_rect_make(bar_x, inner_y, bar_w, inner_h)
                           : tui_rect_make(0, -1, 0, 0);
}

/* +/- always win; both the displayed value and bar open exact entry. */
int ls_radio_vfo_card_hit(tui_rect down, tui_rect up, tui_rect value,
                         tui_rect bar, int col, int row)
{
    if (tui_rect_contains(down, col, row)) return LS_RADIO_PART_DOWN;
    if (tui_rect_contains(up, col, row)) return LS_RADIO_PART_UP;
    if (tui_rect_contains(value, col, row) || tui_rect_contains(bar, col, row))
        return LS_RADIO_PART_VALUE;
    return -1;
}

/* FM/P25 VFO cards share the same title, travel and filled +/- keys. */
void ls_radio_vfo_card_draw(tui_surface *sf, tui_rect b, ls_radio_card_t *c)
{
    c->down = c->up = c->value_rect = c->bar = tui_rect_make(0, -1, 0, 0);
    const uint8_t h = c->live ? c->hue : (TUI_BLACK | TUI_BRIGHT);
    const uint8_t edge = TUI_ATTR(h, TUI_BLACK);
    const uint8_t face = TUI_ATTR(c->live ? (h | TUI_BRIGHT) : LS_DIM_FG, TUI_BLACK);
    if (b.w < 24 || b.h < 5) return;

    /* The card's own outline, then the field inside it. */
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

    char edit[20];
    vfo_card_geometry(b, c, edit);
    tui_put_str(sf, b, b.x + 2, label_y, c->label, face);
    tui_put_str(sf, b, c->value_rect.x, c->value_rect.y, edit,
                c->live ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    if (ls_tui_keyboard_mode() && b.w >= 30 && c->key_up) {
        char badge[6] = { '[', c->key_down, '/', c->key_up, ']', 0 };
        tui_put_str(sf, b, right - 5, label_y, badge, edge);
    }
    /* Filaments retain HEAD geometry; the whole bar is an entry target. */
    const int bar_x = c->bar.x;
    const int bar_w = c->bar.w;
    const int segs = bar_w / 2;
    if (segs >= 2) {
        float frac = c->frac;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        const int lit = (int)(frac * (float)segs + 0.5f);
        for (int s = 0; s < segs; ++s)
            ls_radio_filament(sf, b, bar_x + s * 2, inner_y, inner_h,
                    c->live && s < lit ? (h | TUI_BRIGHT) : (TUI_BLACK | TUI_BRIGHT));
    }

    if (!c->live) c->bar = tui_rect_make(0, -1, 0, 0);

    /* The keys: a lit field, a rule top and bottom, and the sign in the
       middle. No '+' corners, so the only plus on the card is the one a
       thumb is meant to press. */
    for (int k = 0; k < 2; ++k) {
        const tui_rect key = k ? c->up : c->down;
        const bool flash = c->live && (k ? c->flash_up : c->flash_down);
        ls_fill_dither(sf, key, flash ? LS_DITHER_HEAVY : c->live ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT,
                       flash ? (TUI_WHITE | TUI_BRIGHT) : c->live ? h : (TUI_BLACK | TUI_BRIGHT));
        for (int x = 0; x < key.w; ++x) {
            tui_put_char(sf, b, key.x + x, key.y, '=', edge);
            tui_put_char(sf, b, key.x + x, key.y + key.h - 1, '=', edge);
        }
        const char sign[2] = { k ? '+' : '-', 0 };
        tui_put_str(sf, b, key.x + (key.w - 1) / 2, key.y + (key.h - 1) / 2,
                    sign, TUI_ATTR(flash ? TUI_BLACK : c->live ? (TUI_WHITE | TUI_BRIGHT) : LS_DIM_FG,
                                   flash ? (TUI_WHITE | TUI_BRIGHT) : TUI_BLACK));
    }
}

/* Scanner and VFO dispatch the same scan actions. Resume advances, as before. */
bool ls_radio_scan_key(int c)
{
    if (c == 'X') scan_engine_stop();
    else if (c == 'H') {
        if (scan_engine_manual_hold()) scan_engine_next();
        else scan_engine_hold(true);
    } else if (c == 'N') scan_engine_next();
    else if (c == 'K') scan_engine_skip();
    else return false;
    return true;
}
