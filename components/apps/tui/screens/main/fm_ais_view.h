#ifndef FM_AIS_VIEW_H
#define FM_AIS_VIEW_H
#include "../../ls_tui_screen.h"
#include "../../ls_options.h"
extern const ls_opt_ctx_t fm_ais_options;
void fm_ais_draw(tui_surface *sf, tui_rect area);
bool fm_ais_key(ls_tk_t key);
bool fm_ais_touch(int col, int row);
#endif
