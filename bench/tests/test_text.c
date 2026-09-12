/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_text.c */

#include "ls_test.h"
#include "ls_text.h"

#include <string.h>

static const char *age(uint32_t then, uint32_t now)
{
    static char buf[8];
    memset(buf, 0, sizeof(buf));
    ls_age_str(buf, sizeof(buf), then, now);
    return buf;
}

LS_CASE(an_age_uses_the_largest_unit_that_leaves_a_whole_number)
{
    LS_EQ_STR(" 0s", age(100, 100));
    LS_EQ_STR("59s", age(1, 60));
    LS_EQ_STR(" 1m", age(0 + 1, 61));
    LS_EQ_STR("59m", age(1, 1 + 3599));
    LS_EQ_STR(" 1h", age(1, 1 + 3600));
    LS_EQ_STR("23h", age(1, 1 + 86399));
    LS_EQ_STR(" 1d", age(1, 1 + 86400));
}

LS_CASE(days_are_reported_as_days)
{
    /* The mesh peer table stopped at hours, so a node last heard on
       Tuesday read "97h" - a number the reader has to divide before it means
       anything, in a column four characters wide precisely so it can be read
       at a glance. */
    LS_EQ_STR(" 4d", age(1, 1 + 4 * 86400));
    LS_EQ_STR("30d", age(1, 1 + 30 * 86400));
}

LS_CASE(never_heard_is_not_an_age)
{
    /* A zero timestamp is "no observation", and the screens that call this
       hold rows for peers and talkgroups that have not been heard yet. If
       that rendered as an age it would read as "heard just now", which is
       the opposite of the truth and indistinguishable from a live contact. */
    LS_EQ_STR(" -- ", age(0, 5000));
    LS_EQ_STR(" -- ", age(0, 0));
}

LS_CASE(a_timestamp_from_the_future_is_refused_rather_than_wrapped)
{
    /* and both turn on the same trap: these timestamps come
       from a clock that is epoch seconds once the RTC is set and uptime
       before, so a record written under one and read under the other is
       ahead of "now". Unsigned subtraction would turn that into an age of
       about a hundred and thirty-six years, and the column would render its
       last two digits as a perfectly ordinary-looking number of days. */
    LS_EQ_STR(" -- ", age(5000, 4999));
    LS_EQ_STR(" -- ", age(0xFFFFFFFFu, 1));
}

LS_CASE(an_age_never_writes_past_the_buffer_it_was_given)
{
    /* Callers hand this a fixed four-or-five byte field on the stack. */
    char tight[5];
    memset(tight, 'Z', sizeof(tight));
    ls_age_str(tight, 5, 1, 1 + 30 * 86400);
    LS_CHECK(strlen(tight) < 5);

    /* Too small to hold anything is a refusal, not a truncation of an
       already-short string into something that reads as a different age. */
    char none[1];
    none[0] = 'Z';
    ls_age_str(none, sizeof(none), 1, 100);
    LS_EQ_INT('Z', none[0]);
}

/* ------------------------------------------------------------- wrapping -- */

LS_CASE(wrapping_breaks_at_words_and_keeps_them_whole)
{
    char lines[4][32];
    const int n = ls_wrap_text("the quick brown fox jumps over it", 12,
                               (char *)lines, sizeof(lines[0]), 4);
    LS_CHECK_MSG(n >= 2, "a 33 character sentence fitted on %d lines of 12", n);
    for (int i = 0; i < n; i++)
        LS_CHECK_MSG((int)strlen(lines[i]) <= 12,
                     "line %d is %d characters wide: [%s]",
                     i, (int)strlen(lines[i]), lines[i]);

    /* Every word survives somewhere, which is the property a reader cares
       about - a wrapper that drops one is worse than one that cuts. */
    char joined[128] = {0};
    for (int i = 0; i < n; i++) {
        strncat(joined, lines[i], sizeof(joined) - strlen(joined) - 1);
        strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
    }
    LS_CHECK(strstr(joined, "quick"));
    LS_CHECK(strstr(joined, "brown"));
    LS_CHECK(strstr(joined, "jumps"));
}

LS_CASE(a_word_wider_than_the_line_is_cut_rather_than_dropped)
{
    /* Settled in the POCSAG detail view and carried over verbatim: a cut word
       still carries information and a missing one does not. */
    char lines[3][32];
    const int n = ls_wrap_text("aaaaaaaaaaaaaaaaaaaaaaaa end", 10,
                               (char *)lines, sizeof(lines[0]), 3);
    LS_CHECK_MSG(n >= 1, "a long word produced no lines at all");
    LS_CHECK(strlen(lines[0]) > 0);
    for (int i = 0; i < n; i++)
        LS_CHECK((int)strlen(lines[i]) <= 10);
}

LS_CASE(wrapping_refuses_the_shapes_it_cannot_serve)
{
    char lines[2][16];
    LS_EQ_INT(0, ls_wrap_text(NULL, 10, (char *)lines, sizeof(lines[0]), 2));
    LS_EQ_INT(0, ls_wrap_text("text", 0, (char *)lines, sizeof(lines[0]), 2));
    LS_EQ_INT(0, ls_wrap_text("text", 10, NULL, sizeof(lines[0]), 2));
    LS_EQ_INT(0, ls_wrap_text("text", 10, (char *)lines, sizeof(lines[0]), 0));
}
