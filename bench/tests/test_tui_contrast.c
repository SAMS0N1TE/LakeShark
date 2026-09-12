/* LS_TEST_SOURCES: ls_theme.c against the palettes it defines */

#include "ls_test.h"

#include "ls_theme.h"
#include "tuilib/tui_core.h"

#include <math.h>
#include <stdio.h>

/* RGB565 to the 0..1 channel values the luminance formula wants. */
static void unpack(uint16_t c, double *r, double *g, double *b)
{
    *r = (double)((c >> 11) & 0x1F) / 31.0;
    *g = (double)((c >> 5)  & 0x3F) / 63.0;
    *b = (double)( c        & 0x1F) / 31.0;
}

/* WCAG relative luminance: linearise each channel, then weight it. */
static double lin(double v)
{
    return (v <= 0.03928) ? (v / 12.92) : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(uint16_t c)
{
    double r, g, b;
    unpack(c, &r, &g, &b);
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

static double contrast(uint16_t fg, uint16_t bg)
{
    const double a = luminance(fg), b = luminance(bg);
    const double hi = a > b ? a : b, lo = a > b ? b : a;
    return (hi + 0.05) / (lo + 0.05);
}

/* The palette indices the interface names. TUI_WHITE is the muted grey the
   rule sends words to; TUI_BLACK|TUI_BRIGHT is the furniture grey. */
#define IDX_BLACK     0
#define IDX_WHITE     7
#define IDX_BR_BLACK  8
#define IDX_BR_WHITE 15

static int palette_count(void) { return ls_tui_theme_count() + 1; }

static const ls_tui_theme_t *palette_at(int i)
{
    return i < ls_tui_theme_count() ? ls_tui_theme_at(i) : &ls_theme_daylight;
}

/* ----------------------------------------------------------------- cases -- */

LS_CASE(the_grey_that_carries_words_is_readable_in_every_theme)
{
    /* LS_DIM_FG is TUI_WHITE. Every palette has to keep it legible on the
       ground it is drawn on, or the rule in ls_tui_ui.h is a comment rather
       than a guarantee. */
    const int n = palette_count();
    LS_CHECK_MSG(n > 1, "no themes are registered at all");

    for (int i = 0; i < n; i++) {
        const ls_tui_theme_t *t = palette_at(i);
        LS_CHECK(t != NULL);
        if (!t) continue;

        const double c = contrast(t->palette[IDX_WHITE], t->palette[IDX_BLACK]);
        LS_CHECK_MSG(c >= 4.5,
                     "theme '%s': the text grey is %.1f:1 on its own ground - "
                     "4.5:1 is the floor for something a reader has to read",
                     t->name, c);
    }
}

LS_CASE(the_furniture_grey_stays_out_of_the_way_in_every_theme)
{
    /* The other half, and it matters as much: if the faint grey were strong
       enough to read, the unlit half of a meter would compete with the lit
       half and a radar ring would read as a contact. The rule needs BOTH
       colours to keep doing their own job. */
    const int n = palette_count();
    for (int i = 0; i < n; i++) {
        const ls_tui_theme_t *t = palette_at(i);
        if (!t) continue;

        const double faint = contrast(t->palette[IDX_BR_BLACK], t->palette[IDX_BLACK]);
        const double text  = contrast(t->palette[IDX_WHITE],    t->palette[IDX_BLACK]);
        LS_CHECK_MSG(faint < 4.5,
                     "theme '%s': the furniture grey is %.1f:1 - strong enough "
                     "to compete with the things that are meant to be lit",
                     t->name, faint);

        LS_CHECK_MSG(faint >= 2.0,
                     "theme '%s': the furniture grey is %.2f:1 - too close to "
                     "the ground to be seen, so a radar ring reads as nothing",
                     t->name, faint);
        LS_CHECK_MSG(text > faint,
                     "theme '%s': the text grey (%.1f:1) does not stand further "
                     "from the ground than the furniture grey (%.1f:1) - the "
                     "two have swapped jobs",
                     t->name, text, faint);
    }
}

LS_CASE(the_hint_row_reads_on_its_own_bar)
{

    const int n = palette_count();
    for (int i = 0; i < n; i++) {
        const ls_tui_theme_t *t = palette_at(i);
        if (!t) continue;

        const double c = contrast(t->palette[IDX_BR_WHITE], t->palette[IDX_BR_BLACK]);
        LS_CHECK_MSG(c >= 4.5,
                     "theme '%s': the hint row is %.1f:1 - a bar of text "
                     "nobody can read is a row of the screen spent on nothing",
                     t->name, c);
    }
}

LS_CASE(the_grey_this_was_reported_about_really_was_too_dark)
{

    const ls_tui_theme_t *t = ls_tui_theme_by_name("Terminal Bay");
    LS_CHECK_MSG(t != NULL, "Terminal Bay is not registered any more");
    if (!t) return;

    const double c = contrast(t->palette[IDX_BR_BLACK], t->palette[IDX_BLACK]);
    LS_CHECK_MSG(c < 3.0,
                 "BR_BLACK is %.1f:1 on black now - it used to be about 2.1 "
                 "and that is why words were moved off it", c);
}

/* ------------------------------------------------------ the chrome pairs -- */

typedef struct {
    uint8_t     fg, bg;
    const char *where;
} pair_t;

static const pair_t CHROME[] = {
    { TUI_WHITE | TUI_BRIGHT,  TUI_BLACK,               "names and headings" },
    { TUI_YELLOW | TUI_BRIGHT, TUI_BLACK,               "values" },
    { TUI_CYAN | TUI_BRIGHT,   TUI_BLACK,               "the [?] knob" },
    { TUI_BLACK,               TUI_CYAN,                "the status row" },
    { TUI_BLACK,               TUI_YELLOW | TUI_BRIGHT, "the MSG badge" },
    { TUI_BLACK,               TUI_GREEN | TUI_BRIGHT,  "the landscape tab, a tile's LIVE" },
    { TUI_BLACK,               TUI_CYAN | TUI_BRIGHT,   "a selected row" },
    { TUI_YELLOW | TUI_BRIGHT, TUI_BLACK | TUI_BRIGHT,  "the hint row's keys" },
    { TUI_BLACK,               TUI_YELLOW,              "the waterfall's HELD" },
    { TUI_BLACK,               TUI_WHITE,               "the selected DIAG tile" },
    { TUI_BLACK,               TUI_RED | TUI_BRIGHT,    "a red panel title" },
    { TUI_BLACK,               TUI_BLUE | TUI_BRIGHT,   "a blue panel title" },
    { TUI_BLACK,               TUI_MAGENTA | TUI_BRIGHT,"a magenta panel title" },
};

LS_CASE(the_chrome_pairs_read_in_every_palette)
{
    for (int i = 0; i < palette_count(); i++) {
        const ls_tui_theme_t *t = palette_at(i);
        if (!t) continue;
        for (unsigned p = 0; p < sizeof(CHROME) / sizeof(CHROME[0]); p++) {
            const double c = contrast(t->palette[CHROME[p].fg & 0x0F],
                                      t->palette[CHROME[p].bg & 0x0F]);
            LS_CHECK_MSG(c >= 4.5, "theme '%s': %s is %.2f:1",
                         t->name, CHROME[p].where, c);
        }
    }
}

LS_CASE(daylight_carries_words_in_every_colour_both_ways)
{
    /* The mode exists to be read in the sun, so it is held to more
       than the themes are. Any colour may carry words on the white ground,
       and any colour may be the ground under white words - the status row,
       every selected tile, every banner. Contrast is symmetric, so the two
       are one number; both are asked for anyway, so a failure says which
       job the colour is failing. BR_BLACK is furniture and carries neither. */
    const ls_tui_theme_t *d = &ls_theme_daylight;
    for (int c = 1; c < 16; c++) {
        if (c == IDX_BR_BLACK) continue;
        const double ink   = contrast(d->palette[c], d->palette[IDX_BLACK]);
        const double under = contrast(d->palette[IDX_BLACK], d->palette[c]);
        LS_CHECK_MSG(ink >= 4.5, "Daylight slot %d on white is %.2f:1", c, ink);
        LS_CHECK_MSG(under >= 4.5,
                     "white words on Daylight slot %d are %.2f:1", c, under);
    }
}

LS_CASE(daylight_bright_is_the_stronger_ink)
{
    /* The bright bit of an attribute is how the interface says
       "this one": a heading, a selected box, a lit tile. On black that means
       more light. On white it has to mean more ink, or every emphasis in the
       interface would fade toward the ground instead of standing out of it. */
    const ls_tui_theme_t *d = &ls_theme_daylight;
    for (int c = 0; c < 8; c++)
        LS_CHECK_MSG(luminance(d->palette[c + 8]) < luminance(d->palette[c]),
                     "Daylight colour %d is lighter bright than normal", c);
}
