/* The place picker's globe strip. scr_tiles.c draws it where CartoCore is
   built and its definition wins; on other boards with the TUI (the Nano's DSI
   build) the strip stays empty. */
#include "ls_tui_ui.h"

__attribute__((weak)) void ls_tiles_search_globe(tui_surface *sf, tui_rect r, float done,
                                                 double lat, double lon)
{
    (void)sf; (void)r; (void)done; (void)lat; (void)lon;
}
