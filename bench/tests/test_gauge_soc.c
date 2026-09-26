#include "ls_test.h"
#include "ls_gauge_soc.h"

LS_CASE(a_full_cell_under_the_board_load_reads_full)
{
    /* 4.15 V at -350 mA rests near 4.185 V: a charged cell under the
       board's load. */
    LS_EQ_INT(ls_gauge_rest_mv(4150, -350), 4185);
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(ls_gauge_rest_mv(4150, -350)), 100);
}

LS_CASE(the_curve_is_monotonic_and_bounded)
{
    int last = -1;
    for (int mv = 3000; mv <= 4400; mv += 5) {
        const int p = ls_gauge_soc_from_rest_mv(mv);
        LS_CHECK(p >= 0 && p <= 100);
        LS_CHECK(p >= last);
        last = p;
    }
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(3300), 0);
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(3790), 50);
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(4180), 100);
}

LS_CASE(charging_current_is_taken_off_not_added)
{
    LS_EQ_INT(ls_gauge_rest_mv(4100, 500), 4050);
    LS_EQ_INT(ls_gauge_rest_mv(3700, 0), 3700);
}
