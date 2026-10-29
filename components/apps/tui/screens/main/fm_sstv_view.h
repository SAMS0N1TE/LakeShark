/* FM's PICTURE page: SSTV as it arrives. */
#ifndef FM_SSTV_VIEW_H
#define FM_SSTV_VIEW_H

#include "../../ls_options.h"
#include "../../ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

void fm_sstv_draw(tui_surface *sf, tui_rect area);
/* Any frame the page is not drawn: give the grid its own colours back. */
void fm_sstv_view_idle(void);
/* OPTIONS row: SSTV PICTURES on/off. */
int  fm_sstv_opt_get(const ls_opt_t *o);
void fm_sstv_opt_set(const ls_opt_t *o, int v);

/* The picture's own sixteen colours for its cells (ls_tui_basemap). In its
   own file so a host build without the panel can stand it in. */
void fm_sstv_palette(tui_surface *sf, tui_rect cells, const uint16_t palette[16]);

#ifdef __cplusplus
}
#endif

#endif
