/* LS_TEST_SOURCES: ${APP}/rec/rec_scout_span.c */

#include "ls_test.h"
#include "rec_scout_span.h"

LS_CASE(span_ladder_runs_from_native_width_to_narrowest_crop)
{
    LS_EQ_UINT(rec_scout_span_hz(0), 200000u);
    LS_EQ_UINT(rec_scout_span_hz(1), 100000u);
    LS_EQ_UINT(rec_scout_span_hz(2),  50000u);
    LS_EQ_UINT(rec_scout_span_hz(3),  25000u);
}

LS_CASE(zoom_in_narrows_span_and_stops_at_minimum)
{
    int level = 0;
    level = rec_scout_span_zoom_in(level);
    LS_EQ_UINT(rec_scout_span_hz(level), 100000u);
    level = rec_scout_span_zoom_in(level);
    LS_EQ_UINT(rec_scout_span_hz(level), 50000u);
    level = rec_scout_span_zoom_in(level);
    LS_EQ_UINT(rec_scout_span_hz(level), 25000u);
    LS_EQ_INT(rec_scout_span_zoom_in(level), level);
}

LS_CASE(zoom_out_widens_span_and_stops_at_native_width)
{
    int level = REC_SCOUT_SPAN_LEVEL_COUNT - 1;
    level = rec_scout_span_zoom_out(level);
    LS_EQ_UINT(rec_scout_span_hz(level), 50000u);
    level = rec_scout_span_zoom_out(level);
    LS_EQ_UINT(rec_scout_span_hz(level), 100000u);
    level = rec_scout_span_zoom_out(level);
    LS_EQ_UINT(rec_scout_span_hz(level), 200000u);
    LS_EQ_INT(rec_scout_span_zoom_out(level), level);
}

LS_CASE(span_shortcut_still_cycles_the_ladder)
{
    int level = REC_SCOUT_SPAN_LEVEL_COUNT - 1;
    level = rec_scout_span_cycle(level);
    LS_EQ_INT(level, 0);
    LS_EQ_UINT(rec_scout_span_hz(level), 200000u);
}

LS_CASE(out_of_range_levels_are_clamped_before_span_arithmetic)
{
    LS_EQ_UINT(rec_scout_span_hz(-1), 200000u);
    LS_EQ_UINT(rec_scout_span_hz(99), 25000u);
    LS_EQ_INT(rec_scout_span_zoom_out(-1), 0);
    LS_EQ_INT(rec_scout_span_zoom_in(99),
              REC_SCOUT_SPAN_LEVEL_COUNT - 1);
}
