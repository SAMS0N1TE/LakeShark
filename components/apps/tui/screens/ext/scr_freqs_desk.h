/* FREQS in a desk window: scr_freqs.c hands its four calls here while the
   desk is drawing it (ls_tui_compact). A build without the desk has none of
   this. */
#ifndef SCR_FREQS_DESK_H
#define SCR_FREQS_DESK_H

#include "../../ls_tui_ui.h"
#include "../../ls_freqs.h"

/* scr_freqs.c: the dial with its needle, where an entry sits on the dial. */
void ls_freqs_dial(tui_surface *sf, tui_rect clip, int x, int y, const ls_freq_t *e, bool busy);

#if LS_TUI_HAS_COMPACT
void ls_freqs_desk_enter(void);
void ls_freqs_desk_draw(tui_surface *sf, tui_rect a);
bool ls_freqs_desk_key(ls_tk_t k, char ch);
bool ls_freqs_desk_touch(int col, int row);
#endif

#endif /* SCR_FREQS_DESK_H */
