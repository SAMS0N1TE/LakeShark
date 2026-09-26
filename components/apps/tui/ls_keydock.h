/* A touch keyboard docked under an editor. Unlike the ls_keyboard overlay,
   which collects a whole string and hands it back, this sends each press to
   the screen as the key it stands for, so the text being typed into is the
   screen's own and the cursor can be anywhere in it. */

#ifndef LS_KEYDOCK_H
#define LS_KEYDOCK_H

#include <stdbool.h>
#include "ls_tui_screen.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rows the dock wants inside an area of this size; 0 when it cannot fit
   keys a finger can hit. */
int  ls_keydock_height(tui_rect area);
void ls_keydock_draw(tui_surface *sf, tui_rect area);
/* A tap. True when it landed on a key, with that key in *key / *ch
   (LS_TK_CHAR and the character, or an arrow, ENTER or BACKSPACE). Layer
   and shift keys are handled here and return true with LS_TK_NONE. */
bool ls_keydock_touch(int col, int row, ls_tk_t *key, char *ch);
/* Fresh text starts capitalised, as the overlay does. */
void ls_keydock_reset(bool capital);

#ifdef __cplusplus
}
#endif
#endif
