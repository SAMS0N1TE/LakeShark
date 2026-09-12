/* LS_TEST_SOURCES: ${FW}/components/apps/map_gui/map_wpt_select.c */
#include "ls_test.h"
#include "map_gui/map_wpt_select.h"

#include <stdint.h>

/**/

/* Match MAP_MAX_WPT in AppMap.hpp; changing that requires updating the
   AppMap.cpp callback's stack buffer too, so pin the value here. */
#define WPT_N 16

/* Model of the LVGL row index the app passes in.  The uint16_t "no cell"
   sentinel from lv_table_get_selected_cell must survive its cast to int. */
#define TABLE_CELL_NONE 0xFFFF

static void seed(uint8_t used[WPT_N], int a, int b)
{
    for (int i = 0; i < WPT_N; i++) used[i] = 0;
    if (a >= 0 && a < WPT_N) used[a] = 1;
    if (b >= 0 && b < WPT_N) used[b] = 1;
}

LS_CASE(header_row_is_not_actionable)
{
    /* Row 0 holds "NAME", "BRG / DIST", "POSITION".  A tap there must not
       overwrite _wsel or the user would find themselves navigating to
       whatever slot happens to be at index -1. */
    uint8_t used[WPT_N];
    seed(used, 0, 1);
    LS_EQ_INT(map_wpt_row_to_slot(0, used, WPT_N), -1);
}

LS_CASE(empty_row_is_not_actionable)
{

    uint8_t used[WPT_N];
    seed(used, 0, 1);
    LS_EQ_INT(map_wpt_row_to_slot(3, used, WPT_N), -1);   /* slot 2 empty */
    LS_EQ_INT(map_wpt_row_to_slot(WPT_N, used, WPT_N), -1); /* last, empty */
}

LS_CASE(populated_row_maps_to_its_slot)
{
    /* The done-when case: load two waypoints and select the second by
       tapping its row.  Row 2 in the LVGL table is slot 1 in s_wpt. */
    uint8_t used[WPT_N];
    seed(used, 0, 1);
    LS_EQ_INT(map_wpt_row_to_slot(1, used, WPT_N), 0);
    LS_EQ_INT(map_wpt_row_to_slot(2, used, WPT_N), 1);
}

LS_CASE(fresh_boot_gotos_the_second_stored_waypoint)
{

    uint8_t used[WPT_N];
    seed(used, 0, 1);

    int wsel = 0;                             /* AppMap ctor default */
    int slot = map_wpt_row_to_slot(2, used, WPT_N);
    if (slot >= 0) wsel = slot;               /* callback body */
    LS_EQ_INT(wsel, 1);

    /* Now the wptGotoCb precondition (see AppMap.cpp:wptGotoCb) uses
       _wsel to reach s_wpt[_wsel].  Prove the slot it lands on is the
       second populated waypoint, not slot 0. */
    LS_CHECK(wsel >= 0 && wsel < WPT_N);
    LS_CHECK(used[wsel]);
    LS_EQ_INT(wsel, 1);
}

LS_CASE(lv_table_cell_none_is_treated_as_no_selection)
{
    /* lv_table_get_selected_cell writes 0xFFFF into the out params when
       nothing is selected.  That value cast to int is 65535, well beyond
       any real row, and the classifier must reject it silently rather
       than sign-extending into an accidental slot. */
    uint8_t used[WPT_N];
    seed(used, 0, 1);
    LS_EQ_INT(map_wpt_row_to_slot(TABLE_CELL_NONE, used, WPT_N), -1);
}

LS_CASE(rows_past_the_slot_table_are_rejected)
{
    /* buildWptTab sets row_cnt to MAP_MAX_WPT + 1 so the LVGL row index is
       bounded, but the classifier still gets to see it and must not walk
       off the used[] array. */
    uint8_t used[WPT_N];
    seed(used, 0, 1);
    LS_EQ_INT(map_wpt_row_to_slot(WPT_N + 1, used, WPT_N), -1);
    LS_EQ_INT(map_wpt_row_to_slot(100000, used, WPT_N), -1);
}

LS_CASE(negative_row_is_rejected)
{
    /* Defensive: a caller that ever forwards a signed -1 (e.g. a future
       refactor that mixes int and the uint16_t LVGL uses) must not turn
       into an under-flow into slot -2. */
    uint8_t used[WPT_N];
    seed(used, 0, 1);
    LS_EQ_INT(map_wpt_row_to_slot(-1, used, WPT_N), -1);
    LS_EQ_INT(map_wpt_row_to_slot(-1234, used, WPT_N), -1);
}

LS_CASE(null_used_and_zero_len_are_rejected)
{
    /* The AppMap callback always passes a real buffer, but the classifier
       is the only guard against a caller that mis-wires it - fail closed. */
    LS_EQ_INT(map_wpt_row_to_slot(1, NULL, WPT_N), -1);
    uint8_t used[WPT_N] = {0};
    LS_EQ_INT(map_wpt_row_to_slot(1, used, 0), -1);
}
