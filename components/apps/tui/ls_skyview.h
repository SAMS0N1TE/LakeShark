#ifndef LS_SKYVIEW_H
#define LS_SKYVIEW_H
#include "ls_tui_ui.h"
#include "ls_gps.h"
#ifdef __cplusplus
extern "C" {
#endif
/* ASCII sky instrument shared by GPS and FALLS. Selected is an input only. */
void ls_skyview_draw(tui_surface *sf, tui_rect area, const ls_gps_state_t *gps, int selected);
#ifdef __cplusplus
}
#endif
#endif
