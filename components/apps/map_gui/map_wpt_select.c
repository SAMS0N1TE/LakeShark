#include "map_wpt_select.h"

/**/
int map_wpt_row_to_slot(int row, const uint8_t *used, int n_slots)
{
    if (!used || n_slots <= 0) return -1;
    /* Row 0 is the header the app writes in buildWptTab.  LVGL reports
       LV_TABLE_CELL_NONE (0xFFFF) when no cell is selected; casting that
       through int lands well past n_slots and is caught by the upper
       bound.  Both cases must be silent - the caller leaves _wsel alone. */
    if (row < 1 || row > n_slots) return -1;
    int slot = row - 1;
    if (!used[slot]) return -1;
    return slot;
}
