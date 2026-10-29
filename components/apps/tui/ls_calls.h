#ifndef LS_CALLS_H
#define LS_CALLS_H
#include "ls_options.h"
extern const ls_opt_ctx_t ls_calls_options;
void ls_calls_open(const char *receiver);
void ls_calls_leave(void);
bool ls_calls_active(void);
void ls_calls_draw(tui_surface *sf, tui_rect area);
bool ls_calls_key(ls_tk_t key, char ch);
bool ls_calls_touch(int col, int row);
#define LS_CALLS_MENU { .label = "Call archive", .kind = LS_OPT_MENU, .sub = &ls_calls_options }

/* A receiver's live face inside CALLS. P25 registers one: what is on air
   now, which talkgroups have been busy, and one-tap actions on the live
   talkgroup. A receiver without one (FM) gets the selected recording drawn
   in the same place. Every hook is optional; all run on the UI task. */
typedef struct {
  char label[20];
  bool on, can;
} ls_calls_live_key_t;
typedef struct {
  const char *title;    /* "P25 CALLS"                                */
  uint8_t hue;          /* the receiver's accent, a TUI_* colour      */
  /* Rows the live card wants at width w; 0 when it has nothing.     */
  int (*card_rows)(int w);
  void (*card_draw)(tui_surface *sf, tui_rect r);
  /* Rows the talkgroup strip wants at width w, given at most `most`. */
  int (*strip_rows)(int w, int most);
  void (*strip_draw)(tui_surface *sf, tui_rect r);
  /* The talkgroup under a tap in the strip, or 0.                    */
  uint32_t (*strip_hit)(int col, int row);
  /* Up to two actions on the live talkgroup, drawn as CALLS keys.    */
  int (*keys)(ls_calls_live_key_t out[2]);
  void (*press)(int index);
  /* A talkgroup's name, or NULL.                                     */
  const char *(*alias)(uint32_t talkgroup);
} ls_calls_live_t;
/* Used while CALLS is open for `receiver`; NULL removes it. */
void ls_calls_set_live(const char *receiver, const ls_calls_live_t *live);
/* Show only one talkgroup's recordings; 0 shows them all again. */
void ls_calls_show_talkgroup(uint32_t talkgroup);

/* Shared with the live face so a talkgroup looks the same everywhere. */
/* One stable colour per talkgroup: the same TG is the same hue in the
   live card, the strip and the recordings. Never red (alerts own it). */
uint8_t ls_calls_tg_hue(uint32_t talkgroup);
/* Headline text as a braille dot-matrix sign from the ls_glyph figures:
   LS_CALLS_BIG_ROWS tall, four columns a character, '.' ':' and ' ' two.
   Returns the column after the last character. */
#define LS_CALLS_BIG_ROWS 3
int ls_calls_big_text(tui_surface *sf, tui_rect clip, int x, int y,
                      const char *text, uint8_t attr);
/* Columns ls_calls_big_text needs for `text`. */
int ls_calls_big_width(const char *text);
/* A small glass chip, as the receiver rail draws them. */
int ls_calls_chip(tui_surface *sf, tui_rect clip, int x, int y,
                  const char *text, uint8_t hue);
#endif
