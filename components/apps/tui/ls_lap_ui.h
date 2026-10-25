/* What a native app's screen shows while it is not running: loading, the
   question before an unsigned or crashed app runs, and why it would not open. */
#ifndef LS_LAP_UI_H
#define LS_LAP_UI_H
#include "ls_lap.h"
#include "tuilib/tui_core.h"
#include "ls_tui_screen.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LS_LAP_UI_NONE = 0, LS_LAP_UI_OPEN, LS_LAP_UI_BACK } ls_lap_ui_act_t;

/* `crashed` is true when the last restart happened inside this app. */
void ls_lap_ui_draw(tui_surface *sf, tui_rect area, const ls_lap_info_t *app,
                    const ls_lap_status_t *st, const char *error, bool crashed);
/* A tap or a key on that screen. */
ls_lap_ui_act_t ls_lap_ui_touch(int col, int row);
ls_lap_ui_act_t ls_lap_ui_key(ls_tk_t k, char ch, const ls_lap_status_t *st);

#ifdef __cplusplus
}
#endif
#endif
