/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_theme.c */

#include "ls_test.h"
#include "ls_theme.h"

#include <string.h>

#define PALETTE_LEN 16

LS_CASE(there_are_themes_and_the_index_is_bounded)
{
    int n = ls_tui_theme_count();
    LS_CHECK_MSG(n >= 2, "only %d themes", n);

    for (int i = 0; i < n; i++) LS_CHECK(ls_tui_theme_at(i) != NULL);

    LS_CHECK(ls_tui_theme_at(-1) == NULL);
    LS_CHECK(ls_tui_theme_at(n) == NULL);
    LS_CHECK(ls_tui_theme_at(n + 100) == NULL);
    LS_CHECK(ls_tui_theme_at(-9999) == NULL);
}

LS_CASE(black_is_actually_black_in_every_theme)
{

    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        LS_CHECK_MSG(t->palette[0] == 0x0000,
                     "%s has black as 0x%04X", t->name, t->palette[0]);
    }
}

LS_CASE(every_theme_fills_its_whole_palette)
{
    /* A short initialiser is not a compile error, it is a zero fill, and a
       zero entry is black. A theme missing its last four colours would draw
       bright text as invisible text and look like a rendering bug. */
    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        int zeros = 0;
        for (int c = 1; c < PALETTE_LEN; c++) if (t->palette[c] == 0) zeros++;
        LS_CHECK_MSG(zeros == 0,
                     "%s has %d unset palette entries after black",
                     t->name, zeros);
    }
}

LS_CASE(text_is_legible_against_its_own_background)
{
    /* Not a contrast model, just the pairs the UI actually relies on: normal
       text on the default background, and the dim colour used for inactive
       text, which is the one most likely to be tuned into invisibility. */
    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        LS_CHECK_MSG(t->palette[7] != t->palette[0],
                     "%s: white equals black", t->name);
        LS_CHECK_MSG(t->palette[8] != t->palette[0],
                     "%s: bright black equals black, so dim text vanishes",
                     t->name);
        LS_CHECK_MSG(t->palette[15] != t->palette[0],
                     "%s: bright white equals black", t->name);
    }
}

LS_CASE(bright_colours_differ_from_their_base)
{
    /* The attribute byte's high bit is the only thing separating a heading
       from body text in most of the screens. If a theme sets both the same,
       every emphasis in the UI silently stops working. */
    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        for (int c = 1; c < 8; c++)
            LS_CHECK_MSG(t->palette[c] != t->palette[c + 8],
                         "%s: colour %d is identical bright and normal",
                         t->name, c);
    }
}

LS_CASE(every_theme_is_named_and_described)
{
    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        LS_CHECK(t->name && t->name[0]);
        LS_CHECK(t->desc && t->desc[0]);
        /* The status bar shows the active theme beside the keyboard flag, and
           portrait has 48 columns for all of it. */
        LS_CHECK_MSG(strlen(t->name) <= 16,
                     "theme name '%s' is %d characters",
                     t->name, (int)strlen(t->name));
    }
}

LS_CASE(no_two_themes_share_a_name)
{
    /* Lookup returns the first match, so a duplicate makes one theme
       permanently unreachable. */
    for (int i = 0; i < ls_tui_theme_count(); i++)
        for (int j = i + 1; j < ls_tui_theme_count(); j++)
            LS_CHECK_MSG(strcmp(ls_tui_theme_at(i)->name,
                                ls_tui_theme_at(j)->name) != 0,
                         "two themes are called '%s'", ls_tui_theme_at(i)->name);
}

/* ---------------------------------------------------------------- lookup -- */

LS_CASE(every_theme_can_be_found_by_its_own_name)
{
    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        LS_CHECK_MSG(ls_tui_theme_by_name(t->name) == t,
                     "'%s' does not find itself", t->name);
    }
}

LS_CASE(a_name_with_a_space_is_found_however_it_is_typed)
{
    /* The bug this fixes: "Terminal Bay" is two console tokens, so the
       matcher only ever saw "terminal". */
    const ls_tui_theme_t *want = ls_tui_theme_by_name("Terminal Bay");
    LS_CHECK(want != NULL);

    LS_CHECK(ls_tui_theme_by_name("terminal bay") == want);
    LS_CHECK(ls_tui_theme_by_name("TERMINAL BAY") == want);
    LS_CHECK(ls_tui_theme_by_name("TerminalBay")  == want);
    LS_CHECK(ls_tui_theme_by_name("terminalbay")  == want);
    LS_CHECK(ls_tui_theme_by_name("  terminal   bay  ") == want);
}

LS_CASE(a_unique_prefix_is_enough)
{
    /* Typing a full theme name on a thumb keyboard is not the point. */
    const ls_tui_theme_t *want = ls_tui_theme_by_name("Terminal Bay");
    LS_CHECK(ls_tui_theme_by_name("term") == want);
    LS_CHECK(ls_tui_theme_by_name("t")    == want);

    const ls_tui_theme_t *ice = ls_tui_theme_by_name("Ice");
    LS_CHECK(ice != NULL);
    LS_CHECK(ls_tui_theme_by_name("ic") == ice);
}

LS_CASE(an_ambiguous_prefix_matches_nothing)
{

    for (int i = 0; i < ls_tui_theme_count(); i++)
        for (int j = i + 1; j < ls_tui_theme_count(); j++) {
            const char *a = ls_tui_theme_at(i)->name;
            const char *b = ls_tui_theme_at(j)->name;
            /* Longest shared prefix of the two names, ignoring case. */
            char shared[32];
            unsigned n = 0;
            while (n < sizeof(shared) - 1 && a[n] && b[n]) {
                char ca = (a[n] >= 'A' && a[n] <= 'Z') ? (char)(a[n] + 32) : a[n];
                char cb = (b[n] >= 'A' && b[n] <= 'Z') ? (char)(b[n] + 32) : b[n];
                if (ca != cb) break;
                shared[n] = a[n];
                n++;
            }
            if (n == 0) continue;
            shared[n] = 0;
            LS_CHECK_MSG(ls_tui_theme_by_name(shared) == NULL,
                         "'%s' is ambiguous between '%s' and '%s' but resolved",
                         shared, a, b);
        }
}

LS_CASE(nonsense_finds_nothing)
{
    LS_CHECK(ls_tui_theme_by_name(NULL) == NULL);
    LS_CHECK(ls_tui_theme_by_name("") == NULL);
    LS_CHECK(ls_tui_theme_by_name("   ") == NULL);
    LS_CHECK(ls_tui_theme_by_name("zzzz") == NULL);
    /* Longer than any name: a prefix walk must not accept a superstring. */
    LS_CHECK(ls_tui_theme_by_name("Terminal Bay Extended") == NULL);
}

/* ------------------------------------------------------------- daylight -- */

LS_CASE(daylight_is_black_ink_on_a_white_ground)
{
    const ls_tui_theme_t *d = &ls_theme_daylight;
    LS_CHECK_MSG(d->palette[0] == 0xFFFF,
                 "Daylight's ground is 0x%04X, not white", d->palette[0]);
    LS_CHECK_MSG(d->palette[15] == 0x0000,
                 "Daylight's ink is 0x%04X, not black", d->palette[15]);
    /* Everything between is an ink or the furniture grey: a slot equal to
       the ground draws invisible words, and one equal to the ink erases the
       difference between a heading and a colour. */
    for (int c = 1; c < 15; c++)
        LS_CHECK_MSG(d->palette[c] != 0xFFFF && d->palette[c] != 0x0000,
                     "Daylight slot %d is 0x%04X", c, d->palette[c]);
    for (int c = 1; c < 8; c++)
        LS_CHECK_MSG(d->palette[c] != d->palette[c + 8],
                     "Daylight colour %d is identical bright and normal", c);
    LS_CHECK(d->name && d->name[0] && strlen(d->name) <= 16);
    LS_CHECK(d->desc && d->desc[0]);
}

LS_CASE(daylight_is_not_a_theme_the_cycle_can_reach)
{
    /* If it were in the table, stepping the theme would land on white and
       the next step would leave it - with the Daylight box still saying off
       the whole time. */
    for (int i = 0; i < ls_tui_theme_count(); i++)
        LS_CHECK_MSG(ls_tui_theme_at(i) != &ls_theme_daylight,
                     "Daylight is theme %d of the cycle", i);
    LS_EQ_INT(-1, ls_tui_theme_index(&ls_theme_daylight));
    LS_CHECK(ls_tui_theme_by_name("Daylight") == NULL);
    LS_CHECK(ls_tui_theme_by_name("day") == NULL);
}

LS_CASE(the_theme_step_visits_every_theme_once_and_wraps)
{
    /* The Theme row and F9 both step with this, so it is the order both of
       them have. */
    const int n = ls_tui_theme_count();
    const ls_tui_theme_t *t = ls_tui_theme_at(0);
    for (int i = 1; i <= n; i++) {
        t = ls_tui_theme_next(t);
        LS_CHECK_MSG(t == ls_tui_theme_at(i % n),
                     "step %d landed on '%s'", i, t ? t->name : "(null)");
    }
    for (int i = 0; i < n; i++)
        LS_EQ_INT(i, ls_tui_theme_index(ls_tui_theme_at(i)));
    LS_EQ_INT(-1, ls_tui_theme_index(NULL));

    /* From anything outside the cycle, the first theme: a press always lands
       on something the table knows. */
    LS_CHECK(ls_tui_theme_next(NULL) == ls_tui_theme_at(0));
    LS_CHECK(ls_tui_theme_next(&ls_theme_daylight) == ls_tui_theme_at(0));
}

LS_CASE(daylight_covers_the_chosen_theme_and_gives_it_back)
{

    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        LS_CHECK(ls_tui_theme_effective(t, true) == &ls_theme_daylight);
        LS_CHECK_MSG(ls_tui_theme_effective(t, false) == t,
                     "Daylight off over '%s' did not give it back", t->name);
    }
    /* Stepping the theme under Daylight and then turning it off draws the
       stepped theme - the case the Theme row's "after Daylight" is about. */
    const ls_tui_theme_t *chosen = ls_tui_theme_at(0);
    LS_CHECK(ls_tui_theme_effective(chosen, true) == &ls_theme_daylight);
    chosen = ls_tui_theme_next(chosen);
    LS_CHECK(ls_tui_theme_effective(chosen, true) == &ls_theme_daylight);
    LS_CHECK(ls_tui_theme_effective(chosen, false) == ls_tui_theme_at(1));

    LS_CHECK(ls_tui_theme_effective(&ls_theme_daylight, false) == ls_tui_theme_at(0));
    LS_CHECK(ls_tui_theme_effective(NULL, false) == ls_tui_theme_at(0));
    LS_CHECK(ls_tui_theme_effective(NULL, true) == &ls_theme_daylight);
}
