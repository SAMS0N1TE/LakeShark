#include "ls_test.h"
#include "ls_df_cal.h"
#include <math.h>

static ls_df_estimate_t circle(float bearing)
{
    return (ls_df_estimate_t){ .valid = true, .bearing = bearing, .spread = 20, .contrast = 12, .coverage = 355 };
}

LS_CASE(three_agreeing_circles_give_the_offset_across_north)
{
    ls_df_cal_t c; ls_df_cal_begin(&c);
    LS_EQ_INT(c.phase, LS_DF_CAL_AIM);
    ls_df_cal_mark(&c, 355);
    /* The lobe peaks 12 degrees clockwise of the beacon, either side of north. */
    LS_CHECK(ls_df_cal_circle(&c, &(ls_df_estimate_t){ .valid = true, .bearing = 5, .contrast = 12, .coverage = 350 }));
    LS_CHECK(ls_df_cal_circle(&c, &(ls_df_estimate_t){ .valid = true, .bearing = 9, .contrast = 12, .coverage = 350 }));
    ls_df_cal_result_t r; ls_df_cal_result(&c, &r);
    LS_CHECK(!r.ready);
    LS_CHECK(ls_df_cal_circle(&c, &(ls_df_estimate_t){ .valid = true, .bearing = 7, .contrast = 12, .coverage = 350 }));
    ls_df_cal_result(&c, &r);
    LS_CHECK(r.ready); LS_EQ_INT(r.circles, 3);
    LS_NEAR(r.offset, 12, 0.05);
    LS_CHECK(r.spread < 2);
    /* Taken off a later estimate, across north the other way. */
    LS_NEAR(ls_df_cal_apply(8, r.offset), 356, 0.05);
}

LS_CASE(a_partial_or_flat_circle_is_not_taken)
{
    ls_df_cal_t c; ls_df_cal_begin(&c);
    LS_CHECK(!ls_df_cal_circle(&c, &(ls_df_estimate_t){ .valid = true, .bearing = 10, .contrast = 12, .coverage = 360 }));
    ls_df_cal_mark(&c, 90);
    ls_df_estimate_t e = circle(100);
    e.coverage = 200; LS_CHECK(!ls_df_cal_circle(&c, &e));
    e = circle(100); e.contrast = 3; LS_CHECK(!ls_df_cal_circle(&c, &e));
    e = circle(100); e.valid = false; LS_CHECK(!ls_df_cal_circle(&c, &e));
    LS_EQ_INT(c.count, 0);
}

LS_CASE(circles_that_disagree_cannot_be_saved)
{
    ls_df_cal_t c; ls_df_cal_begin(&c); ls_df_cal_mark(&c, 0);
    const float b[] = { 10, 40, -25, 30 };
    for (int i = 0; i < 4; i++) { ls_df_estimate_t e = circle(b[i]); LS_CHECK(ls_df_cal_circle(&c, &e)); }
    ls_df_cal_result_t r; ls_df_cal_result(&c, &r);
    LS_CHECK(!r.ready); LS_CHECK(r.spread > LS_DF_CAL_AGREE);
}

LS_CASE(the_newest_circles_replace_the_oldest)
{
    ls_df_cal_t c; ls_df_cal_begin(&c); ls_df_cal_mark(&c, 0);
    for (int i = 0; i < LS_DF_CAL_CIRCLES; i++) { ls_df_estimate_t e = circle(60); ls_df_cal_circle(&c, &e); }
    for (int i = 0; i < LS_DF_CAL_CIRCLES; i++) { ls_df_estimate_t e = circle(4); ls_df_cal_circle(&c, &e); }
    ls_df_cal_result_t r; ls_df_cal_result(&c, &r);
    LS_EQ_INT(r.circles, LS_DF_CAL_CIRCLES);
    LS_NEAR(r.offset, 4, 0.05); LS_CHECK(r.ready);
}

LS_CASE(no_correction_leaves_the_bearing_alone)
{
    LS_NEAR(ls_df_cal_apply(123, NAN), 123, 0.001);
    LS_CHECK(isnan(ls_df_cal_apply(NAN, 5)));
    ls_df_cal_t c; ls_df_cal_begin(&c); ls_df_cal_mark(&c, NAN);
    LS_EQ_INT(c.phase, LS_DF_CAL_AIM);
}
