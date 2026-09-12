/* A number, typed. */

#ifndef LS_NUMPAD_H
#define LS_NUMPAD_H

#include <stdbool.h>

#include "ls_tui.h"
/* ls_tk_t, which is the router's key enum and lives with the router. */
#include "ls_tui_screen.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ls_numpad_done_t)(double value);

/* Open it. */

void ls_numpad_open(const char *title, const char *unit, double initial,
                    ls_numpad_done_t on_done);

void ls_numpad_close(void);
bool ls_numpad_active(void);

bool ls_numpad_key(ls_tk_t key, char ch);
bool ls_numpad_touch(int col, int row);
void ls_numpad_draw(tui_surface *sf, tui_rect area);

#ifdef __cplusplus
}
#endif

#endif /* LS_NUMPAD_H */
