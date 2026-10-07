#ifndef FM_SAME_VIEW_H
#define FM_SAME_VIEW_H
#include "../../ls_tui_screen.h"
#include "../../ls_options.h"
extern const ls_opt_ctx_t fm_same_options;
void fm_same_enter(void);
void fm_same_draw(tui_surface *sf, tui_rect area);
bool fm_same_key(ls_tk_t key);
bool fm_same_touch(int col, int row);
#endif
