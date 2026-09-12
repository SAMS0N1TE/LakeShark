/* LS_TEST_SOURCES: display_dim.h alone */

#include "ls_test.h"
#include "display_dim.h"

LS_CASE(off_never_dims_however_long_it_sits)
{
    LS_CHECK(!display_dim_due(false, 15, 0u));
    LS_CHECK(!display_dim_due(false, 15, 60u * 60u * 1000u));
}

LS_CASE(on_dims_only_after_the_whole_timeout)
{
    LS_CHECK(!display_dim_due(true, 120, 0u));
    LS_CHECK(!display_dim_due(true, 120, 119999u));
    LS_CHECK(!display_dim_due(true, 120, 120000u));
    LS_CHECK(display_dim_due(true, 120, 120001u));
    LS_CHECK(display_dim_due(true, 15, 15001u));
}

LS_CASE(a_zero_timeout_means_never_not_at_once)
{
    LS_CHECK(!display_dim_due(true, 0, 5000u));
    LS_CHECK(!display_dim_due(true, -1, 5000u));
}

LS_CASE(the_fade_down_lands_on_the_dim_level)
{
    /* 89 is where an 80 percent slider lands through the perceptual curve. */
    int level = 89, steps = 0;
    while (level != 10 && steps < 100) {
        const int next = display_dim_step(level, 10, 4);
        LS_CHECK(next < level);
        LS_CHECK(next >= 10);
        level = next;
        steps++;
    }
    LS_EQ_INT(level, 10);
    LS_EQ_INT(steps, 20);        /* 19 steps of 4, then the last 3 */
}

LS_CASE(the_fade_up_lands_on_the_slider_level)
{
    int level = 10, steps = 0;
    while (level != 89 && steps < 100) {
        const int next = display_dim_step(level, 89, 4);
        LS_CHECK(next > level);
        LS_CHECK(next <= 89);
        level = next;
        steps++;
    }
    LS_EQ_INT(level, 89);
}

LS_CASE(an_unknown_level_goes_straight_to_the_target)
{
    LS_EQ_INT(display_dim_step(-1, 89, 4), 89);
    LS_EQ_INT(display_dim_step(-1, 10, 4), 10);
}

LS_CASE(at_the_target_it_stays_put)
{
    LS_EQ_INT(display_dim_step(10, 10, 4), 10);
    LS_EQ_INT(display_dim_step(89, 89, 4), 89);
}
