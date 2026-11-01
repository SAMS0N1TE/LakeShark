/* The desk form helpers for tests that link a screen without ls_tui_ui.c:
   never compact, keys as wide as their label. */
#include <string.h>
#include "ls_tui_ui.h"

bool ls_tui_compact(void) { return false; }
int ls_key1_width(const ls_btn_t *b) { return b && b->label ? (int)strlen(b->label) + 4 : 4; }
int ls_key1_draw(tui_surface *sf, tui_rect clip, int x, int y, const ls_btn_t *b, bool focus)
{
    (void)sf; (void)clip; (void)x; (void)y; (void)focus;
    return ls_key1_width(b);
}
