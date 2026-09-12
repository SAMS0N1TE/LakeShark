/* A list you pick one thing out of. */

#ifndef LS_PICKER_H
#define LS_PICKER_H

#include <stdbool.h>

#include "ls_tui.h"
/* ls_tk_t, which is the router's key enum and lives with the router. */
#include "ls_tui_screen.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forty-eight is what a screenful can usefully carry: past that nobody is
   reading a list, they are hunting one, and hunting is what the filter is
   for. The caller's own limit is usually smaller. */
#define LS_PICKER_MAX     48
#define LS_PICKER_TEXT    32
#define LS_PICKER_DETAIL  16

/* The index the item was ADDED at, not its position in the list as filtered
   or sorted for display. The caller's array is the thing it wants back, and
   a filter that renumbered the answer would be a trap laid for whoever adds
   the second caller. */
typedef void (*ls_picker_done_t)(int index);

/* Open it empty, then fill it. */

void ls_picker_open(const char *title, ls_picker_done_t on_done);

bool ls_picker_add(const char *label, const char *detail);

/* What to say when the list came out empty. A picker with no rows and no
   sentence reads as a fault; this is the difference between "there is
   nothing here" and "something went wrong". */
void ls_picker_empty_reason(const char *why);

void ls_picker_close(void);
bool ls_picker_active(void);

bool ls_picker_key(ls_tk_t key, char ch);
bool ls_picker_touch(int col, int row);
void ls_picker_draw(tui_surface *sf, tui_rect area);

#ifdef __cplusplus
}
#endif

#endif /* LS_PICKER_H */
