#ifndef MAP_WPT_SELECT_H
#define MAP_WPT_SELECT_H

/*LS-762*/
/* Row-to-slot classifier for the Map app's waypoint table.  The table has a
   header at row 0 and one row per waypoint slot after it.  Prior to LS-762
   AppMap::buildWptTab left _wpt_tbl without a click callback, so _wsel was
   frozen at 0 unless MARK HERE happened to write into slot 0.  After a fresh
   boot with waypoints already stored, GOTO and DEL could only ever act on
   slot 0, which is a nav aid that lies about which point you have picked.

   Extracting the decision here keeps the LVGL surface in AppMap.cpp thin and
   lets the bench prove the invariants: header and empty rows are never
   actionable, LV_TABLE_CELL_NONE (0xFFFF) is treated as "no selection", and
   a populated row maps 1:1 to its slot.  The AppMap event callback calls
   this and updates _wsel only when the answer is non-negative. */

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
