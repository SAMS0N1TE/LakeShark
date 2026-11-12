#ifndef LS_CATCH_UI_H
#define LS_CATCH_UI_H
#include "ls_tui_screen.h"
#include "ls_options.h"
/* CATCH as a page of REC: the body only. The shared bar carries its
   actions, OPTIONS carries the band. */
void ls_catch_body_draw(tui_surface *,tui_rect);
bool ls_catch_body_touch(int,int);
bool ls_catch_key(ls_tk_t,char);
void ls_catch_toggle(void);
void ls_catch_save_selected(void);
void ls_catch_stop(void);
void ls_catch_enter(void);                 /* load the saved channel windows */
void ls_catch_watch_cycle(void);           /* WATCH key: ALL <-> CHOSEN */
const char *ls_catch_watch_label(bool *on);
bool ls_catch_running(void);
const ls_opt_ctx_t *ls_catch_options(void);
/* The burst picked in the list, for SUB-GHZ BAND's LISTEN HERE: its index
   (-1 for none), its frequency (0 for none), and whether a cell is on the
   list rather than the slice strip. */
int ls_catch_selected_index(void);
uint32_t ls_catch_selected_hz(void);
bool ls_catch_list_contains(int x,int y);
#endif
