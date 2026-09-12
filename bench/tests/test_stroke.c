/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_stroke.c */

#include "ls_test.h"
#include "ls_stroke.h"

#include <string.h>

LS_CASE(empty_ink_draws_nothing_at_all)
{

    LS_EQ_INT(0, ls_stroke_glyph(false, false, false, false));
}

LS_CASE(a_side_by_side_pair_is_a_horizontal_run)
{
    LS_EQ_INT('-', ls_stroke_glyph(true, true, false, false));
    /* And the low pair is an underscore, so ink sitting at different heights
       in neighbouring cells still joins up instead of stepping. */
    LS_EQ_INT('_', ls_stroke_glyph(false, false, true, true));
}

LS_CASE(a_stacked_pair_is_a_vertical_run)
{
    LS_EQ_INT('|', ls_stroke_glyph(true, false, true, false));
    LS_EQ_INT('|', ls_stroke_glyph(false, true, false, true));
}

LS_CASE(opposite_corners_are_a_diagonal_the_right_way_round)
{
    /* Top-left to bottom-right descends to the right, which is a backslash.
       Getting these two swapped mirrors every diagonal road on the map and
       looks entirely plausible - the same failure the sky plot is guarded
       against. */
    LS_EQ_INT('\\', ls_stroke_glyph(true, false, false, true));
    LS_EQ_INT('/',  ls_stroke_glyph(false, true, true, false));
}

LS_CASE(three_or_four_quarters_is_a_junction)
{
    LS_EQ_INT('+', ls_stroke_glyph(true, true, true, false));
    LS_EQ_INT('+', ls_stroke_glyph(true, true, false, true));
    LS_EQ_INT('+', ls_stroke_glyph(true, false, true, true));
    LS_EQ_INT('+', ls_stroke_glyph(false, true, true, true));
    LS_EQ_INT('+', ls_stroke_glyph(true, true, true, true));
}

LS_CASE(a_single_quarter_claims_no_direction)
{
    /* A road clipping the corner of a cell has no direction to report, and
       inventing one would draw a diagonal that is not there. */
    LS_EQ_INT('.', ls_stroke_glyph(true, false, false, false));
    LS_EQ_INT('.', ls_stroke_glyph(false, true, false, false));
    LS_EQ_INT('.', ls_stroke_glyph(false, false, true, false));
    LS_EQ_INT('.', ls_stroke_glyph(false, false, false, true));
}

LS_CASE(no_pattern_ever_comes_out_as_a_block)
{

    for (int i = 0; i < 16; i++) {
        const char g = ls_stroke_glyph(i & 1, (i >> 1) & 1,
                                       (i >> 2) & 1, (i >> 3) & 1);
        if (!g) continue;                      /* empty is allowed */
        const unsigned char u = (unsigned char)g;
        LS_CHECK_MSG(u >= 0x20 && u < 0x7F,
                     "pattern %d gave 0x%02X, which the blitter draws as a "
                     "filled rectangle", i, (unsigned)u);
    }
}

LS_CASE(every_non_empty_pattern_draws_something)
{
    /* Fifteen of the sixteen have ink in them, and every one of those has to
       put a mark on the screen - a pattern that silently drew nothing would
       punch holes in a road. */
    int drawn = 0;
    for (int i = 0; i < 16; i++) {
        const char g = ls_stroke_glyph(i & 1, (i >> 1) & 1,
                                       (i >> 2) & 1, (i >> 3) & 1);
        if (g) drawn++;
    }
    LS_EQ_INT(15, drawn);
}
