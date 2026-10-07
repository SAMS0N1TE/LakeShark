#ifndef FM_APRS_VIEW_H
#define FM_APRS_VIEW_H
#include "../../ls_tui_screen.h"
#include "../../ls_options.h"
extern const ls_opt_ctx_t fm_aprs_options;
void fm_aprs_draw(tui_surface *sf, tui_rect area);
bool fm_aprs_key(ls_tk_t key);
bool fm_aprs_touch(int col, int row);
#endif
