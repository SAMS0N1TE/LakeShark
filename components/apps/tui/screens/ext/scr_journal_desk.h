/* JOURNAL in a desk window: scr_journal.c hands its calls here while the desk
   is drawing it (ls_tui_compact) and neither the live attachments nor the
   handheld layout is up. A build without the desk has none of this. */
#ifndef SCR_JOURNAL_DESK_H
#define SCR_JOURNAL_DESK_H

#include "../../ls_tui_ui.h"
#include "../../ls_field.h"

#if LS_TUI_HAS_COMPACT

/* scr_journal_desk.c */
void ls_journal_desk_enter(void);
void ls_journal_desk_draw(tui_surface *sf, tui_rect a, const ls_field_state_t *st);
/* False for a key that is not the table's: N R C V E G then do what they
   always did. */
bool ls_journal_desk_key(ls_tk_t k, char ch);
bool ls_journal_desk_touch(int col, int row);
bool ls_journal_desk_classic(void);
bool ls_journal_desk_classic_key(ls_tk_t k, char ch);
void ls_journal_desk_export_dir(const char *dir);

/* scr_journal.c */
int  ls_scr_journal_selected(void);                 /* the entry the handheld's keys act on */
void ls_scr_journal_select(int index);
void ls_scr_journal_action(int i);                  /* 0 new, 1 radio, 2 record, 3 sensors, 4 edit, 5 map */
const char *ls_scr_journal_feedback(void);          /* what the last request came to; "" for nothing */
const char *ls_scr_journal_radio(void);             /* the radio attached to new entries */

#endif
#endif /* SCR_JOURNAL_DESK_H */
