/* P25 in a desk window: on the VFO view scr_p25.c hands the middle of the
   window here (ls_tui_compact), between its own view bar and its tuning
   deck, unless the receiver view was asked for. A build without the desk
   has none of this. */
#ifndef SCR_P25_DESK_H
#define SCR_P25_DESK_H

#include "../../ls_tui_ui.h"

#if LS_TUI_HAS_COMPACT

/* scr_p25_desk.c */
void ls_p25_desk_enter(void);
void ls_p25_desk_draw(tui_surface *sf, tui_rect a);
/* False for a key that is not the table's: tuning, the views and the
   handheld's letters then do what they always did. */
bool ls_p25_desk_key(ls_tk_t k, char ch);
/* False for a click outside the area it drew in. */
bool ls_p25_desk_touch(int col, int row);
/* The receiver view (the frequency glass) was asked for instead. */
bool ls_p25_desk_classic(void);
bool ls_p25_desk_classic_key(ls_tk_t k, char ch);
void ls_p25_desk_export_dir(const char *dir);

/* scr_p25.c and p25_calls_live.c: the handheld's own pieces. */
void ls_scr_p25_health(tui_surface *sf, tui_rect r);        /* RECEIVER HEALTH / TOTALS */
const char *ls_scr_p25_receive_label(void);                 /* "CONTROL LOCKED", "VOICE"... */
const char *ls_scr_p25_voice_status(void);
void p25_calls_live_card(tui_surface *sf, tui_rect r);      /* the call on air, or the last one; ten rows */

#endif
#endif /* SCR_P25_DESK_H */
