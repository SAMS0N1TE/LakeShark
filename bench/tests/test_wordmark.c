/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_wordmark.c */

#include "ls_test.h"
#include "ls_wordmark.h"

#include <string.h>

#define PORTRAIT_COLS  54
#define LANDSCAPE_COLS 120

#define MARK "TERMINAL BAY"

LS_CASE(the_wordmark_is_wider_than_the_portrait_grid)
{

    LS_CHECK(ls_wordmark_width(MARK) > PORTRAIT_COLS);
    LS_CHECK(ls_wordmark_width(MARK) < LANDSCAPE_COLS);
}

LS_CASE(letters_are_proportional_so_the_width_cannot_be_a_multiplication)
{
    /* T is five columns and every other capital is four, which is the whole
       reason the old estimate was out. A width computed as letters times a
       constant gets this wrong by one column per T. */
    LS_CHECK(ls_wordmark_width("T") > ls_wordmark_width("E"));
    LS_EQ_INT(ls_wordmark_width("E"), ls_wordmark_width("A"));
    /* Two letters is two glyphs plus the one column of air between them. */
    LS_EQ_INT(ls_wordmark_width("EE"), ls_wordmark_width("E") * 2 + 1);
}

LS_CASE(a_trailing_letter_carries_no_air_so_centring_is_on_the_ink)
{
    /* If the width included a trailing column, every centred mark would sit
       half a column left of centre - invisible on one string and wrong on
       all of them. */
    const int one = ls_wordmark_width("A");
    LS_CHECK(one > 0);
    LS_EQ_INT(one + 1 + one, ls_wordmark_width("AA"));
}

LS_CASE(portrait_splits_the_mark_and_both_halves_fit)
{
    char a[32], b[32];
    LS_CHECK(ls_wordmark_split(MARK, PORTRAIT_COLS, a, sizeof(a),
                               b, sizeof(b)));
    LS_CHECK(strcmp(a, "TERMINAL") == 0);
    LS_CHECK(strcmp(b, "BAY") == 0);
    /* The property that matters is not where it split but that nothing is
       left hanging off the panel. */
    LS_CHECK(ls_wordmark_width(a) <= PORTRAIT_COLS);
    LS_CHECK(ls_wordmark_width(b) <= PORTRAIT_COLS);
}

LS_CASE(landscape_does_not_split_what_already_fits)
{
    char a[32], b[32];
    LS_CHECK(!ls_wordmark_split(MARK, LANDSCAPE_COLS, a, sizeof(a),
                                b, sizeof(b)));
    LS_CHECK(strcmp(a, MARK) == 0);
    LS_EQ_INT(0, (int)strlen(b));
    LS_CHECK(ls_wordmark_width(a) <= LANDSCAPE_COLS);
}

LS_CASE(a_centred_mark_stays_on_the_panel_at_every_width_it_fits)
{
    /* The actual defect was an x coordinate, so assert the x coordinate:
       left edge at or past zero, right edge at or before the last column,
       for both halves, over every grid between the two real ones. */
    for (int cols = 40; cols <= 140; cols++) {
        char a[32], b[32];
        ls_wordmark_split(MARK, cols - 2, a, sizeof(a), b, sizeof(b));
        const char *halves[2] = { a, b };
        for (int h = 0; h < 2; h++) {
            if (!halves[h][0]) continue;
            const int w = ls_wordmark_width(halves[h]);
            if (w > cols) continue;      /* too narrow for even one word */
            const int x = cols / 2 - w / 2;
            LS_CHECK_MSG(x >= 0, "left edge %d off the panel at %d cols",
                         x, cols);
            LS_CHECK_MSG(x + w <= cols,
                         "right edge %d past %d cols", x + w, cols);
        }
    }
}

LS_CASE(nothing_here_reads_past_its_buffers)
{
    char a[32], b[32];
    /* An empty string, a string of spaces, and one with no space to split
       on - each has been a crash in a text routine somewhere. */
    LS_EQ_INT(0, ls_wordmark_width(""));
    LS_EQ_INT(0, ls_wordmark_width(NULL));
    LS_CHECK(!ls_wordmark_split("", 54, a, sizeof(a), b, sizeof(b)));
    LS_CHECK(!ls_wordmark_split("AAAAAAAAAAAAAAAAAAAA", 20,
                                a, sizeof(a), b, sizeof(b)));
    /* Unsplittable comes back whole for the renderer to clip, not empty. */
    LS_CHECK(strlen(a) > 0);

    LS_CHECK(ls_wordmark_width("12:34") > 0);
    ls_wordmark_split("A 1 B", 54, a, sizeof(a), b, sizeof(b));
}
