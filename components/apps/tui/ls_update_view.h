/* The UPDATE app's page: updates over WiFi, drawn as a scene. */

#ifndef LS_UPDATE_VIEW_H
#define LS_UPDATE_VIEW_H

#include <stdbool.h>

#include "ls_tui.h"
#include "ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opening the page starts a check when nothing is under way. */
void ls_update_view_enter(void);
void ls_update_view_draw(tui_surface *sf, tui_rect area);
/* ENTER steps the update; every other key is left to the router. */
bool ls_update_view_key(ls_tk_t k, char ch);
/* True when the tap was the page's BACK. A tap on the big button, or on
   READY, does what ENTER does. */
bool ls_update_view_touch(int col, int row);

#ifdef __cplusplus
}
#endif

#endif /* LS_UPDATE_VIEW_H */
