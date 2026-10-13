#ifndef LS_RADIO_PANEL_H
#define LS_RADIO_PANEL_H
#include "ls_tui_ui.h"
#include "iq_app_control.h"

typedef struct {
    bool lists, scan_choice;
    int selected, focus, slot;
    tui_rect list_area, previous, next;
    int visible[64], count, first;
    ls_btn_t buttons[6];
    char notice[64];
} ls_radio_panel_t;

typedef struct {
    bool fm;
    uint32_t frequency, standby;
    const char *mode;
    ls_iq_control_status_t receiver;
    float power;
    /* Quieting, 0 hiss to 1 fully quiet, and where the squelch opens on the
       same travel. The panel draws the meter and the gate from these so it
       stays a view and does not reach into any one receiver's state. */
    int   volume;
    float signal;
    float gate;
    bool  squelch_open;
    bool  has_squelch;
    uint16_t tone_required;
    char detail[4][64];
    /* How this screen sets the two things you reach for while a scan runs.
       The panel is still a view: it reads volume and the gate from the fields
       above and hands a new value back through these, so it never learns
       which receiver it is driving. NULL leaves that control inert and drawn
       as unavailable - which is what a screen with no squelch wants. */
    void (*set_volume_pct)(int pct);
    void (*set_squelch_pct)(int pct);
} ls_radio_view_t;

void ls_radio_panel_draw(ls_radio_panel_t *, const ls_radio_view_t *, tui_surface *, tui_rect);
void ls_radio_frequency_draw(tui_surface *, tui_rect, int row, uint32_t hz,
                             uint8_t attr);
char ls_radio_panel_key(ls_radio_panel_t *, const ls_radio_view_t *, ls_tk_t, char);
char ls_radio_panel_touch(ls_radio_panel_t *, const ls_radio_view_t *, int, int);

/* Where the deck's controls landed in the last draw, so a test can press them
   the way a thumb does rather than guess at a screenshot - the same service
   ls_btn_rect_slot gives for the button bar.

   `card` is the deck card in draw order, or LS_RADIO_CARD_GATE for the
   squelch slider under the meter. `part` is which piece of it. Answers false
   when that control was not drawn, which is itself worth asserting: a pane
   too small for a card must say so rather than hide a live target. */
/* THE SHARED FACE.

   The scanner's glass and deck are drawn with these four, and so is P25's
   receiver page, so the two screens are one instrument rather than two houses
   with similar wallpaper. They are drawing only: no state is kept here, and
   every hit rect is handed back to the caller, which owns it. */

/* A ruled frame, heavier than a panel outline, with its title on the rule. */
void ls_radio_bezel(tui_surface *sf, tui_rect r, const char *title);

/* An annunciator: a lit field with a word on it. Answers where the next one
   starts, so a rail of them is a fold of calls. */
int ls_radio_chip(tui_surface *sf, tui_rect box, int x, int y, const char *text,
                  bool on, uint8_t hue);

/* A VFD bargraph: thin lit filaments every other column, green through amber
   to red, `rows` tall. `peak` >= 0 marks a held peak in white. Lines, not
   slabs - a filled block reads as one shape instead of a measurement. */
void ls_radio_bargraph(tui_surface *sf, tui_rect box, int x, int y, int w,
                       int rows, float frac, float peak);

/* One control: its label and bracketed number on the header row, the travel
   under it, and the two keys at the right end. The caller fills everything
   above `down` and reads the four rects back out.

   THE TRAVEL AND THE NUMBER ARE DIFFERENT TARGETS. `bar` sets the value at
   the column pressed; `value_rect` is the only one that opens exact entry.
   They never overlap, and `bar` is empty when the control cannot be given a
   value outright - a slider that cannot set must not be drawn as one. */
typedef struct {
    const char *label;
    const char *value;
    float       frac;        /* 0..1, where the travel sits                 */
    uint8_t     hue;
    bool        live;        /* false: drawn unavailable, no target at all  */
    bool        settable;    /* false: steps only, no travel to press       */
    bool        flash_down, flash_up;   /* key lit by a tap, caller's timer */
    tui_rect    down, up, value_rect, bar;   /* out */
} ls_radio_card_t;
void ls_radio_card_draw(tui_surface *sf, tui_rect box, ls_radio_card_t *card);

#define LS_RADIO_CARD_GATE (-1)
/* DOWN and UP step; BAR is the travel, which sets the value at the column
   pressed; VALUE is the bracketed number in the card's header, which is the
   only thing that opens exact entry. BAR and VALUE never overlap - a slider
   that also types is a slider you cannot trust. */
enum { LS_RADIO_PART_DOWN = 0, LS_RADIO_PART_UP, LS_RADIO_PART_VALUE,
       LS_RADIO_PART_BAR };
bool ls_radio_panel_control_rect(int card, int part, int *x, int *y, int *w, int *h);

/* THE FREQUENCY KEYPAD: FM's VFO keypad, shared so every receiver that tunes
   inline draws the same keys. It replaces the caller's controls area (never a
   popup); the caller owns the entry text and the rects handed back. */
void ls_freq_key_draw(tui_surface *sf, tui_rect k, const char *cap, char badge,
                      uint8_t hue, bool strong);
void ls_freq_pad_draw(tui_surface *sf, tui_rect area, const char *entry,
                      bool invalid, uint32_t blink, tui_rect keys[12],
                      tui_rect actions[3]);
/* '0'-'9' '.', 'd' delete, 'c' clear, 'x' cancel, 't' tune; 0 for a miss. */
char ls_freq_pad_hit(const tui_rect keys[12], const tui_rect actions[3],
                     int col, int row);
void ls_freq_entry_add(char *entry, size_t cap, char c);
void ls_freq_entry_delete(char *entry);
bool ls_freq_entry_hz(const char *entry, uint32_t *out);
#endif
