/* Same renderer, without the GCC-only stack-report flags on the normal bench source. */
#include "../../components/apps/tui/tuilib/tui_core.c"

/* Runtime capability/resize queries in tests without a physical grid. */
void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch) {
    if (cols) *cols = 40;
    if (rows) *rows = 30;
    if (cw) *cw = 10;
    if (ch) *ch = 17;
}
