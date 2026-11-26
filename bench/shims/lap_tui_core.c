/* Same renderer, without the GCC-only stack-report flags on the normal bench source. */
#include "../../components/apps/tui/tuilib/tui_core.c"

/* Runtime capability/resize queries in tests without a physical grid. */
void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch) {
    if (cols) *cols = 40;
    if (rows) *rows = 30;
    if (cw) *cw = 10;
    if (ch) *ch = 17;
}

/* The lap host draws app boxes as desk group boxes in a desk window; these
   tests have no desk: never compact, and the group box is never reached. */
#include "../../components/apps/tui/ls_tui_ui.h"
bool ls_tui_compact(void) { return false; }
tui_rect ls_dui_group_ink(tui_surface *sf, tui_rect r, const char *caption, uint8_t cap)
{
    (void)sf; (void)caption; (void)cap;
    return tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
}
