#ifndef MAP_WPT_SELECT_H
#define MAP_WPT_SELECT_H

/**/
/* Row-to-slot classifier for the Map app's waypoint table. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the waypoint slot index (0..n_slots-1) that should become the new
   selection when the LVGL table reports `row` as its selected cell.  Returns
   -1 when the tap must NOT change the selection: header row, out-of-range
   row (including LV_TABLE_CELL_NONE cast to int), or an empty slot. */
int map_wpt_row_to_slot(int row, const uint8_t *used, int n_slots);

#ifdef __cplusplus
}
#endif

#endif
