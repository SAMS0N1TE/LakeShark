#ifndef LS_CATCH_UI_H
#define LS_CATCH_UI_H
#include "ls_tui_screen.h"
void ls_catch_draw(tui_surface *,tui_rect);
bool ls_catch_key(ls_tk_t,char);
/* 2 requests BACK to REC, 1 consumed, 0 unhandled. */
int ls_catch_touch(int,int);
#endif
