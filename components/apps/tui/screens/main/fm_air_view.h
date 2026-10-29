/* FM's AIRBAND page in AM: the channel scanner (airband, CB). */
#ifndef FM_AIR_VIEW_H
#define FM_AIR_VIEW_H

#include "../../ls_options.h"
#include "../../ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

void fm_air_draw(tui_surface *sf, tui_rect area);
bool fm_air_key(ls_tk_t k, char ch);
bool fm_air_touch(int col, int row);
/* The tab's name: AIRBAND, or CB while a CB preset runs. */
const char *fm_air_tab(void);
/* AM's OPTIONS > CHANNEL SCAN. */
extern const ls_opt_ctx_t fm_air_options;
void fm_air_options_show(const ls_opt_t *o, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
