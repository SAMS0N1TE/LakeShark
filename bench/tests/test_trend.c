/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_trend.c */
/* The bounded history behind every trace on the interface. */

#include "ls_test.h"
#include "ls_trend.h"

LS_CASE(an_empty_ring_reports_nothing_rather_than_zero_samples)
{
    ls_trend_t t;
    ls_trend_reset(&t);
    LS_EQ_INT(0, ls_trend_len(&t));
    /* A caller checks the length; these must not read uninitialised memory
       either way. */
    LS_EQ_INT(0, ls_trend_at(&t, 0));
    LS_EQ_INT(0, ls_trend_min(&t, 0));
    LS_EQ_INT(0, ls_trend_max(&t, 8));
}

LS_CASE(zero_is_the_newest_sample)
{
    /* The direction, which is the half that fails silently: a trace
       drawn from the wrong end is still a line, and still looks like data. */
    ls_trend_t t;
    ls_trend_reset(&t);
    ls_trend_push(&t, 10);
    ls_trend_push(&t, 20);
    ls_trend_push(&t, 30);

    LS_EQ_INT(3, ls_trend_len(&t));
    LS_EQ_INT(30, ls_trend_at(&t, 0));
    LS_EQ_INT(20, ls_trend_at(&t, 1));
    LS_EQ_INT(10, ls_trend_at(&t, 2));
}

LS_CASE(reading_past_the_end_gives_nothing_and_does_not_wrap_round)
{
    /* A caller draws a screen-width of trace against a ring that may hold
       less. Wrapping round would repeat the newest samples at the far end and
       draw a history that never happened. */
    ls_trend_t t;
    ls_trend_reset(&t);
    ls_trend_push(&t, 7);
    ls_trend_push(&t, 9);

    LS_EQ_INT(9, ls_trend_at(&t, 0));
    LS_EQ_INT(7, ls_trend_at(&t, 1));
    LS_EQ_INT(0, ls_trend_at(&t, 2));
    LS_EQ_INT(0, ls_trend_at(&t, 40));
    LS_EQ_INT(0, ls_trend_at(&t, -1));
}

LS_CASE(the_ring_holds_its_depth_and_drops_the_oldest)
{
    ls_trend_t t;
    ls_trend_reset(&t);
    for (int i = 0; i < LS_TREND_MAX; i++)
        ls_trend_push(&t, (uint16_t)(1000 + i));

    LS_EQ_INT(LS_TREND_MAX, ls_trend_len(&t));
    LS_EQ_INT(1000 + LS_TREND_MAX - 1, ls_trend_at(&t, 0));
    LS_EQ_INT(1000, ls_trend_at(&t, LS_TREND_MAX - 1));

    /* One more past full: the newest moves on, the oldest falls off, and the
       length does not grow. */
    ls_trend_push(&t, 5000);
    LS_EQ_INT(LS_TREND_MAX, ls_trend_len(&t));
    LS_EQ_INT(5000, ls_trend_at(&t, 0));
    LS_EQ_INT(1001, ls_trend_at(&t, LS_TREND_MAX - 1));
}

LS_CASE(the_ordering_survives_walking_the_modulo_the_whole_way_round)
{

    ls_trend_t t;
    ls_trend_reset(&t);
    for (int i = 0; i < LS_TREND_MAX * 3 + 7; i++)
        ls_trend_push(&t, (uint16_t)(i & 0xFFFF));

    const int last = LS_TREND_MAX * 3 + 7 - 1;
    for (int back = 0; back < LS_TREND_MAX; back++)
        LS_CHECK_MSG(ls_trend_at(&t, back) == (uint16_t)(last - back),
                     "sample %d back read %u, want %u", back,
                     (unsigned)ls_trend_at(&t, back),
                     (unsigned)(uint16_t)(last - back));
}

LS_CASE(the_extremes_are_over_what_is_held_not_over_the_array)
{
    /* While the ring is filling, the slots past `len` are whatever reset left
       there. An extreme that walked the whole array would report those. */
    ls_trend_t t;
    ls_trend_reset(&t);
    ls_trend_push(&t, 400);
    ls_trend_push(&t, 900);
    ls_trend_push(&t, 650);

    LS_EQ_INT(400, ls_trend_min(&t, 0));
    LS_EQ_INT(900, ls_trend_max(&t, 0));

    /* And once it has wrapped, the dropped samples are gone from both. */
    ls_trend_reset(&t);
    ls_trend_push(&t, 1);                      /* the low, about to fall off */
    for (int i = 0; i < LS_TREND_MAX; i++)
        ls_trend_push(&t, (uint16_t)(500 + i));
    LS_EQ_INT(500, ls_trend_min(&t, 0));
}

LS_CASE(an_extreme_covers_the_newest_samples_and_no_others)
{
    /* What a trace has to scale against, and the reason the count is
       not optional.

       Measured on the board: the health page drew twenty-six columns of free
       internal RAM as a flat line pinned to the bottom of its frame for three
       minutes, because ONE higher sample ninety seconds off the left-hand
       edge owned the top of a scale taken over the whole ring. Every sample
       on the screen looked identical and not one of them was equal. */
    ls_trend_t t;
    ls_trend_reset(&t);
    ls_trend_push(&t, 9000);                 /* the spike, off the edge */
    ls_trend_push(&t, 100);
    ls_trend_push(&t, 140);
    ls_trend_push(&t, 120);

    /* Four columns of trace see the spike and are dominated by it. */
    LS_EQ_INT(9000, ls_trend_max(&t, 4));
    /* Three do not, and get a range the drawn samples actually span. */
    LS_EQ_INT(140, ls_trend_max(&t, 3));
    LS_EQ_INT(100, ls_trend_min(&t, 3));

    /* One sample is its own range, which ls_trend_level then calls flat. */
    LS_EQ_INT(120, ls_trend_min(&t, 1));
    LS_EQ_INT(120, ls_trend_max(&t, 1));

    /* Asking for more than is held is asking for all of it, not a read past
       the end. */
    LS_EQ_INT(9000, ls_trend_max(&t, 999));
    LS_EQ_INT(100, ls_trend_min(&t, 999));
}

LS_CASE(a_flat_history_draws_as_a_flat_line_rather_than_a_wall)
{

    LS_EQ_INT(4, ls_trend_level(700, 700, 700));
    LS_EQ_INT(4, ls_trend_level(0, 0, 0));
    /* An inverted range is not a range either. */
    LS_EQ_INT(4, ls_trend_level(500, 900, 100));
}

LS_CASE(the_scale_spans_the_full_eight_heights_between_the_extremes)
{
    LS_EQ_INT(1, ls_trend_level(100, 100, 800));
    LS_EQ_INT(8, ls_trend_level(800, 100, 800));

    /* Monotonic in between, and every step reachable - a scale that skipped
       heights would draw a staircase where the data is a ramp. */
    int seen[9] = { 0 };
    int last = 0;
    for (int v = 100; v <= 800; v++) {
        const int lvl = ls_trend_level((uint16_t)v, 100, 800);
        LS_CHECK_MSG(lvl >= 1 && lvl <= 8, "value %d gave level %d", v, lvl);
        LS_CHECK_MSG(lvl >= last, "level fell from %d to %d at %d",
                     last, lvl, v);
        last = lvl;
        seen[lvl] = 1;
    }
    for (int i = 1; i <= 8; i++)
        LS_CHECK_MSG(seen[i], "height %d is never used by the scale", i);
}

LS_CASE(a_sample_that_exists_is_never_invisible)
{

    for (int v = 0; v <= 2000; v += 7)
        LS_CHECK_MSG(ls_trend_level((uint16_t)v, 500, 1500) >= 1,
                     "value %d scaled to nothing at all", v);
}
