/* Letters, typed. */

#ifndef LS_KEYBOARD_H
#define LS_KEYBOARD_H

#include <stdbool.h>

#include "ls_tui.h"
/* ls_tk_t, which is the router's key enum and lives with the router. */
#include "ls_tui_screen.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The longest thing anyone types here: a mesh message is 64 and a channel
   PSK is base64 of 32 bytes, which is 44. */
#define LS_KEYBOARD_MAX 96

/* Called with the finished text when OK is pressed, and not at all when it
   is cancelled. The pointer is only valid for the duration of the call. */
typedef void (*ls_keyboard_done_t)(const char *text);

/* Open it. */

void ls_keyboard_open(const char *title, const char *initial, int max_len,
                      ls_keyboard_done_t on_done);

/* Start masked and lowercase, with a SHOW/HIDE control. Visibility resets
 * on every open; the callback receives the original text. */
void ls_keyboard_open_secret(const char *title, int max_len,
                             ls_keyboard_done_t on_done);

void ls_keyboard_close(void);
bool ls_keyboard_active(void);

/* The router calls these; a screen does not. Both return true when the
   overlay consumed the input, which it does for everything while it is up. */
bool ls_keyboard_key(ls_tk_t key, char ch);
bool ls_keyboard_touch(int col, int row);
void ls_keyboard_draw(tui_surface *sf, tui_rect area);

#ifdef __cplusplus
}
#endif

#endif /* LS_KEYBOARD_H */
