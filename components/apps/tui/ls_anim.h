/* Per-app opening animations. */

#ifndef LS_ANIM_H
#define LS_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ls_tui.h"

/* Begin an opening for `name`, drawn from `icon` in `hue`. Replaces any
   opening already running - switching apps twice quickly should show the
   second one, not queue the first. */
void ls_anim_start(int icon, const char *name, uint8_t hue);

/* True while there are frames left. */
bool ls_anim_active(void);

/* Draw the current frame and advance. Call once per frame from the router,
   before the screen draws, and skip the screen while it returns true. */
bool ls_anim_draw(tui_surface *sf, tui_rect area);

/* Stop now. A key press should always be able to skip an animation; a UI
   that cannot be interrupted is a UI that is in the way. */
void ls_anim_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_ANIM_H */
