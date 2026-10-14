/* C6 updater face and explicit confirmation; all I/O runs on its worker. */
#pragma once
#include "ls_tui_ui.h"
const char *ls_c6_view_hint(void);
void ls_c6_view_enter(void);
void ls_c6_view_leave(void);
void ls_c6_view_draw(tui_surface *sf, tui_rect area);
bool ls_c6_view_key(ls_tk_t key, char ch);
/* true means return to the parent UPDATE page */
bool ls_c6_view_touch(int col, int row);
