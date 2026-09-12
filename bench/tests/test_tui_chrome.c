/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_tui_chrome.c */
/* Chrome layout at every width, not just the two the hardware has. */

#include "ls_test.h"
#include "ls_text.h"
#include "ls_tui_chrome.h"
#include "ls_tui_screen.h"
#include "tui_core.h"

#include <string.h>

#define LANDSCAPE 115
#define PORTRAIT   48

/* Every placed field as [start, end). Returns how many were placed. */
static int spans(ls_tui_status_layout_t l, int brand, int name, int left,
                 int right, int (*out)[2])
{
    int n = 0;
    if (l.brand_x >= 0) { out[n][0] = l.brand_x; out[n][1] = l.brand_x + brand; n++; }
    if (l.name_x  >= 0) { out[n][0] = l.name_x;  out[n][1] = l.name_x  + name;  n++; }
    if (l.left_x  >= 0) { out[n][0] = l.left_x;  out[n][1] = l.left_x  + left;  n++; }
    if (l.right_x >= 0) { out[n][0] = l.right_x; out[n][1] = l.right_x + right; n++; }
    return n;
}

static int overlaps(int (*s)[2], int n)
{
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (s[i][0] < s[j][1] && s[j][0] < s[i][1]) return 1;
    return 0;
}

static int out_of_row(int (*s)[2], int n, int cols)
{
    for (int i = 0; i < n; i++)
        if (s[i][0] < 0 || s[i][1] > cols) return 1;
    return 0;
}

LS_CASE(status_fields_never_overlap_at_any_width)
{
    /* Lengths taken from what the running UI actually passes: the wordmark,
       the longest screen name, and the right field as "KBD Terminal Bay". */
    const int brand = 9, name = 4, left = 12, right = 16;

    for (int cols = 1; cols <= 200; cols++) {
        ls_tui_status_layout_t l =
            ls_tui_status_layout(cols, brand, name, left, right);
        int s[4][2];
        int n = spans(l, brand, name, left, right, s);
        LS_CHECK_MSG(!overlaps(s, n), "overlap at cols=%d", cols);
        LS_CHECK_MSG(!out_of_row(s, n, cols), "off-row at cols=%d", cols);
    }
}

LS_CASE(landscape_shows_everything)
{
    const int brand = 9, name = 4, left = 12, right = 16;
    ls_tui_status_layout_t l =
        ls_tui_status_layout(LANDSCAPE, brand, name, left, right);
    LS_CHECK(l.brand_x >= 0);
    LS_CHECK(l.name_x  >= 0);
    LS_CHECK(l.left_x  >= 0);
    LS_CHECK(l.right_x >= 0);
}

LS_CASE(portrait_keeps_the_screen_name_and_the_right_status)
{
    /* The two fields that carry information. The wordmark is the same on
       every screen and is what should go when 48 columns runs out. */
    const int brand = 9, name = 4, left = 12, right = 16;
    ls_tui_status_layout_t l =
        ls_tui_status_layout(PORTRAIT, brand, name, left, right);
    LS_CHECK(l.name_x  >= 0);
    LS_CHECK(l.right_x >= 0);
}

LS_CASE(the_wordmark_is_dropped_before_the_screen_name)
{
    /* Squeeze until only one of the two can fit and assert which survives. */
    const int brand = 9, name = 4, right = 16;
    for (int cols = 24; cols <= 40; cols++) {
        ls_tui_status_layout_t l =
            ls_tui_status_layout(cols, brand, name, 0, right);
        if (l.brand_x >= 0) LS_CHECK_MSG(l.name_x >= 0,
            "wordmark kept while the name was dropped at cols=%d", cols);
    }
}

LS_CASE(a_status_row_with_nothing_to_show_places_nothing)
{
    ls_tui_status_layout_t l = ls_tui_status_layout(LANDSCAPE, 0, 0, 0, 0);
    LS_EQ_INT(-1, l.brand_x);
    LS_EQ_INT(-1, l.name_x);
    LS_EQ_INT(-1, l.left_x);
    LS_EQ_INT(-1, l.right_x);
}

LS_CASE(absurd_widths_do_not_place_anything_off_the_row)
{
    static const int W[] = { 0, 1, 2, 3, -5 };
    for (unsigned i = 0; i < sizeof(W) / sizeof(W[0]); i++) {
        ls_tui_status_layout_t l = ls_tui_status_layout(W[i], 9, 4, 12, 16);
        int s[4][2];
        int n = spans(l, 9, 4, 12, 16, s);
        LS_CHECK_MSG(!out_of_row(s, n, W[i] > 0 ? W[i] : 0),
                     "placed a field on a %d-column row", W[i]);
    }
}

/* ------------------------------------------------------------------ hints */

LS_CASE(hint_text_never_reaches_the_tail_at_any_width)
{
    /* The bug this pins: the tail grew from "F10 help" to
       "F10 help  F11 turn" while the hint's stop column stayed at a
       hardcoded cols-11, so the two overlapped from column 29 in portrait. */
    for (int cols = 1; cols <= 200; cols++) {
        ls_tui_hint_layout_t l = ls_tui_hint_layout(cols);
        LS_CHECK(l.tail != NULL);
        if (l.tail_x < 0) continue;

        int tail_end = l.tail_x + (int)strlen(l.tail);
        LS_CHECK_MSG(l.hint_end_x <= l.tail_x,
                     "hint runs into the tail at cols=%d (%d > %d)",
                     cols, l.hint_end_x, l.tail_x);
        LS_CHECK_MSG(tail_end <= cols,
                     "tail runs off the row at cols=%d", cols);
        LS_CHECK_MSG(l.tail_x >= 1, "tail at column %d", l.tail_x);
    }
}

LS_CASE(both_real_geometries_keep_a_tail)
{
    ls_tui_hint_layout_t land = ls_tui_hint_layout(LANDSCAPE);
    ls_tui_hint_layout_t port = ls_tui_hint_layout(PORTRAIT);
    LS_CHECK(land.tail_x >= 0);
    LS_CHECK(port.tail_x >= 0);

    /* Landscape has room for the spelled-out version. */
    LS_EQ_STR("F10 help  F11 turn", land.tail);

    LS_CHECK_MSG(port.hint_end_x >= 20,
                 "portrait left only %d columns for the hint", port.hint_end_x);
}

LS_CASE(a_row_too_narrow_for_a_tail_gives_the_hint_the_whole_row)
{
    ls_tui_hint_layout_t l = ls_tui_hint_layout(10);
    LS_EQ_INT(-1, l.tail_x);
    LS_CHECK(l.hint_end_x >= 1);
    LS_CHECK(l.hint_end_x <= 10);
}

/* ------------------------------------------------------------------ split */

/* ls_tui_split hands every two-pane screen its rects, and a rect error there
   is the same class of defect as the one that put ADSB's footer above its own
   pane. The properties are cheap to state and worth pinning exactly: the two
   halves must cover the area, must not overlap, and must stay inside it.

   A gap is a column the diff renderer never repaints, so it keeps whatever
   was on it. An overlap is two screens writing the same cell, where the one
   that draws last wins and the result changes with content. */
static void split_at(int w, int h, tui_rect *a, tui_rect *b)
{
    ls_tui_split(tui_rect_make(3, 5, w, h), a, b);
}

LS_CASE(the_two_halves_tile_the_area_exactly)
{
    for (int w = 0; w <= 130; w++)
        for (int h = 0; h <= 70; h += 7) {
            tui_rect a, b;
            split_at(w, h, &a, &b);

            int area_cells = (w > 0 && h > 0) ? w * h : 0;
            int a_cells = (a.w > 0 && a.h > 0) ? a.w * a.h : 0;
            int b_cells = (b.w > 0 && b.h > 0) ? b.w * b.h : 0;
            LS_CHECK_MSG(a_cells + b_cells == area_cells,
                         "%dx%d split into %d + %d cells, area is %d",
                         w, h, a_cells, b_cells, area_cells);
        }
}

LS_CASE(neither_half_leaves_the_area)
{
    for (int w = 0; w <= 130; w++)
        for (int h = 0; h <= 70; h += 7) {
            tui_rect a, b;
            split_at(w, h, &a, &b);
            const tui_rect *r[2] = { &a, &b };
            for (int i = 0; i < 2; i++) {
                if (r[i]->w <= 0 || r[i]->h <= 0) continue;
                LS_CHECK_MSG(r[i]->x >= 3 && r[i]->y >= 5 &&
                             r[i]->x + r[i]->w <= 3 + w &&
                             r[i]->y + r[i]->h <= 5 + h,
                             "half %d left a %dx%d area", i, w, h);
            }
        }
}

LS_CASE(the_halves_never_overlap)
{
    for (int w = 0; w <= 130; w++)
        for (int h = 0; h <= 70; h += 7) {
            tui_rect a, b;
            split_at(w, h, &a, &b);
            if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0) continue;
            bool overlap = a.x < b.x + b.w && b.x < a.x + a.w &&
                           a.y < b.y + b.h && b.y < a.y + a.h;
            LS_CHECK_MSG(!overlap, "halves overlap at %dx%d", w, h);
        }
}

LS_CASE(the_threshold_is_width_and_not_aspect)
{
    /* Pinned because a test once faked this as `w >= h * 2`, which splits a
       56x24 pane the other way from production. 60 is the width at which two
       panes each still hold a label and a value. */
    tui_rect a, b;

    split_at(60, 24, &a, &b);
    LS_EQ_INT(30, a.w);
    LS_EQ_INT(24, a.h);

    split_at(59, 24, &a, &b);
    LS_EQ_INT(59, a.w);
    LS_EQ_INT(12, a.h);

    /* Wide but not tall, and tall but not wide, both by the same rule. */
    split_at(56, 24, &a, &b);
    LS_EQ_INT(56, a.w);
    split_at(113, 24, &a, &b);
    LS_EQ_INT(56, a.w);
}

LS_CASE(a_null_output_pointer_is_allowed)
{
    /* Callers that want only one half pass NULL for the other. */
    tui_rect only;
    ls_tui_split(tui_rect_make(0, 0, 100, 20), &only, NULL);
    LS_EQ_INT(50, only.w);
    ls_tui_split(tui_rect_make(0, 0, 100, 20), NULL, &only);
    LS_EQ_INT(50, only.w);
    ls_tui_split(tui_rect_make(0, 0, 100, 20), NULL, NULL);
}

/* --------------------------------------------------------- hint fitting -- */

static const char *const REAL_HINTS[] = {
    "1 DECODE  2 SIGNAL  TAP mark  H hold",
    "1 VFO  2 PAGES  3 SWEEP  UP/DOWN select",
    "ARROWS pick  ENTER open  F1..F8 jump",
    "ENTER arms and disarms",
    "MIC arms  hold auto  LEFT/RIGHT page  ENTER sends",
    "RED is moving",
    "TAP mark  LEFT/RIGHT move  V source  H hold",
    "UP/DOWN aircraft  ENTER details",
    "UP/DOWN pick  ENTER change",
};
#define N_HINTS ((int)(sizeof(REAL_HINTS) / sizeof(REAL_HINTS[0])))

/* A whole-token match, so "F1" does not match inside "F1..F8". */
static bool contains_token(const char *hay, const char *tok, int n)
{
    const int len = (int)strlen(hay);
    for (int i = 0; i + n <= len; i++) {
        if (strncmp(hay + i, tok, (size_t)n) != 0) continue;
        const bool left  = (i == 0) || hay[i - 1] == ' ';
        const bool right = (i + n == len) || hay[i + n] == ' ';
        if (left && right) return true;
    }
    return false;
}

LS_CASE(a_hint_that_fits_is_drawn_whole)
{
    for (int i = 0; i < N_HINTS; i++) {
        const int len = (int)strlen(REAL_HINTS[i]);
        LS_CHECK_MSG(ls_tui_hint_fit(REAL_HINTS[i], len) == len,
                     "'%s' lost characters at its own length", REAL_HINTS[i]);
        LS_CHECK_MSG(ls_tui_hint_fit(REAL_HINTS[i], len + 40) == len,
                     "'%s' lost characters with room to spare", REAL_HINTS[i]);
    }
}

LS_CASE(a_hint_that_does_not_fit_stops_at_a_group_boundary)
{
    /* The defect: DIAG drew "counters are red once they" at 26 columns, which
       is a sentence cut mid-word and reads as damage. Every width from one
       column up to the string's own length is checked, because the boundary
       arithmetic is where an off-by-one would hide. */
    for (int i = 0; i < N_HINTS; i++) {
        const char *h = REAL_HINTS[i];
        const int len = (int)strlen(h);
        for (int w = 1; w <= len; w++) {
            const int n = ls_tui_hint_fit(h, w);
            LS_CHECK_MSG(n <= w, "'%s' at %d returned %d", h, w, n);
            if (n == 0 || n == len) continue;

            const bool at_boundary = (h[n] == ' ');
            int first_word = 0;
            while (h[first_word] && h[first_word] != ' ') first_word++;
            const bool no_choice = (first_word > w);

            LS_CHECK_MSG(at_boundary || no_choice,
                         "'%s' at %d stopped mid-word after %d: '%.*s'",
                         h, w, n, n, h);
        }
    }
}

LS_CASE(the_kept_part_of_a_hint_never_ends_in_a_space)
{
    /* A trailing space is invisible, so a boundary cut that included the
       separator would silently spend a column and, at the wrong width, would
       let the next group start one place further left than the arithmetic
       above believes. */
    for (int i = 0; i < N_HINTS; i++) {
        const char *h = REAL_HINTS[i];
        for (int w = 1; w <= (int)strlen(h); w++) {
            const int n = ls_tui_hint_fit(h, w);
            if (n > 0)
                LS_CHECK_MSG(h[n - 1] != ' ',
                             "'%s' at %d kept a trailing space", h, w);
        }
    }
}

LS_CASE(hint_fitting_survives_the_arguments_it_should_not_get)
{
    LS_CHECK(ls_tui_hint_fit(NULL, 40) == 0);
    LS_CHECK(ls_tui_hint_fit("", 40) == 0);
    LS_CHECK(ls_tui_hint_fit("anything", 0) == 0);
    LS_CHECK(ls_tui_hint_fit("anything", -5) == 0);
    /* Two spaces at the very start: the first group is empty, so there is
       nothing to keep and nothing to cut mid-word. */
    LS_CHECK(ls_tui_hint_fit("  trailing", 4) == 0);
}

/* Every screen's hint against the row portrait gives it, which is the check
   that would have caught this before it reached the panel. */
LS_CASE(no_real_hint_is_cut_mid_word_in_portrait)
{
    const ls_tui_hint_layout_t l = ls_tui_hint_layout(48);
    const int w = l.hint_end_x - 1;
    LS_CHECK_MSG(w > 0, "portrait leaves the hint %d columns", w);

    for (int i = 0; i < N_HINTS; i++) {
        const char *h = REAL_HINTS[i];
        const int n = ls_tui_hint_fit(h, w);
        if (n == 0 || n == (int)strlen(h)) continue;
        LS_CHECK_MSG(h[n] == ' ',
                     "portrait cuts '%s' mid-word to '%.*s'", h, n, h);
    }
}

/* The format itself, because the row's two colours depend on it.

   ls_tui_screen.c reads a hint as "KEY label" pairs: two spaces mean the next
   token is a key, one space inside a pair means the rest is its label. Six of
   the nine screens had it the other way round and wrote two spaces between a
   key and its own label, so "mark", "pick", "move" and "select aircraft" were
   all painted in the key colour. A legend where everything is a key is a wall
   of text, which is exactly what the two colours exist to prevent. */
LS_CASE(every_hint_separates_its_pairs_the_way_the_row_reads_them)
{
    for (int i = 0; i < N_HINTS; i++) {
        const char *h = REAL_HINTS[i];
        const int len = (int)strlen(h);

        LS_CHECK_MSG(len > 0 && h[0] != ' ' && h[len - 1] != ' ',
                     "'%s' starts or ends on a space", h);

        for (int j = 0; j + 2 < len; j++)
            LS_CHECK_MSG(!(h[j] == ' ' && h[j + 1] == ' ' && h[j + 2] == ' '),
                         "'%s' has three spaces at %d", h, j);

        /* Every pair has a key and something after it. A pair that is one
           token is a key with no label, which draws as a bare "TAP". */
        int start = 0;
        while (start < len) {
            int end = start;
            while (end < len && !(h[end] == ' ' && h[end + 1] == ' ')) end++;

            bool has_label = false;
            for (int j = start; j < end; j++)
                if (h[j] == ' ') { has_label = true; break; }

            LS_CHECK_MSG(has_label,
                         "'%s' has a pair with no label: '%.*s'",
                         h, end - start, h + start);
            start = end + 2;
        }
    }
}

/* The row is 26 columns in portrait. A legend that drops to nothing is worse
   than no legend at all, because the row is still there taking a row. */
LS_CASE(every_hint_keeps_at_least_one_whole_pair_in_portrait)
{
    const ls_tui_hint_layout_t l = ls_tui_hint_layout(48);
    const int w = l.hint_end_x - 1;

    for (int i = 0; i < N_HINTS; i++) {
        const char *h = REAL_HINTS[i];
        const int n = ls_tui_hint_fit(h, w);
        LS_CHECK_MSG(n > 0, "portrait drops all of '%s'", h);

        bool has_label = false;
        for (int j = 0; j < n; j++)
            if (h[j] == ' ') { has_label = true; break; }
        LS_CHECK_MSG(has_label,
                     "portrait keeps only a bare key from '%s': '%.*s'",
                     h, n, h);
    }
}

/* Word wrapping, which had one implementation and needed two callers.

   Pure arithmetic over a string, which is the bench's whole purpose, and the
   defect it exists to stop is silent: a line that does not fit is drawn
   clipped, and clipped text still looks like text. The GPS diagnosis line was
   cut for exactly that long. */
LS_CASE(text_wraps_at_words_and_never_past_the_width)
{
    char lines[4][64];

    /* The GPS sentence that was being cut: fifty-one characters into the
       forty-two the portrait POSITION panel has inside its border. */
    const char *why = "sentences are good, no position yet - antenna or sky";
    const int n = ls_wrap_text(why, 42, (char *)lines, sizeof(lines[0]), 4);
    LS_CHECK(n >= 2);
    for (int i = 0; i < n; i++)
        LS_CHECK_MSG((int)strlen(lines[i]) <= 42,
                     "line %d is %d wide: '%s'", i, (int)strlen(lines[i]), lines[i]);

    /* Nothing is lost. Rejoining with single spaces has to give the original
       back - a wrap that drops a word is worse than one that clips, because
       clipping is visible. */
    char joined[128] = {0};
    for (int i = 0; i < n; i++) {
        if (i) strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        strncat(joined, lines[i], sizeof(joined) - strlen(joined) - 1);
    }
    LS_CHECK_MSG(!strcmp(joined, why), "rejoined to '%s'", joined);

    LS_CHECK(lines[0][strlen(lines[0]) - 1] != ' ');
}

LS_CASE(a_word_longer_than_the_line_is_cut_rather_than_dropped)
{
    char lines[3][64];
    /* No spaces at all, so there is nowhere to break. Cutting keeps the
       information; refusing to emit anything loses it. */
    const int n = ls_wrap_text("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 10,
                               (char *)lines, sizeof(lines[0]), 3);
    LS_CHECK(n > 0);
    LS_EQ_INT(10, (int)strlen(lines[0]));
}

LS_CASE(wrapping_refuses_nonsense_rather_than_writing_somewhere)
{
    char lines[2][64];
    LS_EQ_INT(0, ls_wrap_text(NULL, 40, (char *)lines, sizeof(lines[0]), 2));
    LS_EQ_INT(0, ls_wrap_text("text", 40, NULL, sizeof(lines[0]), 2));
    LS_EQ_INT(0, ls_wrap_text("text", 0, (char *)lines, sizeof(lines[0]), 2));
    LS_EQ_INT(0, ls_wrap_text("text", 40, (char *)lines, sizeof(lines[0]), 0));
}

/* A short string is one line and is not padded or split. */
LS_CASE(text_that_already_fits_is_left_alone)
{
    char lines[3][64];
    LS_EQ_INT(1, ls_wrap_text("short enough", 40, (char *)lines,
                              sizeof(lines[0]), 3));
    LS_CHECK(!strcmp(lines[0], "short enough"));
}

LS_CASE(no_hint_names_a_key_the_tail_already_names)
{
    static const int WIDTHS[] = { 48, 115, 40, 80, 200 };

    for (unsigned wi = 0; wi < sizeof(WIDTHS) / sizeof(WIDTHS[0]); wi++) {
        const ls_tui_hint_layout_t l = ls_tui_hint_layout(WIDTHS[wi]);
        if (!l.tail || l.tail_x < 0) continue;

        /* Every whitespace-separated token of the tail that looks like a key,
           checked against every hint. Keys are the tokens the tail names
           first in each pair, which for every tail this returns is an F-key. */
        for (const char *t = l.tail; *t; ) {
            while (*t == ' ') t++;
            int n = 0;
            while (t[n] && t[n] != ' ') n++;
            if (n == 0) break;

            if (n >= 2 && (t[0] == 'F' || t[0] == 'f') &&
                t[1] >= '0' && t[1] <= '9') {
                for (int i = 0; i < N_HINTS; i++)
                    LS_CHECK_MSG(!contains_token(REAL_HINTS[i], t, n),
                                 "'%s' names %.*s, which the tail '%s' "
                                 "already names", REAL_HINTS[i], n, t, l.tail);
            }
            t += n;
        }
    }
}

/* ---------------------------------------------- padded status -- */

/* On a panel with rounded corners the status row's words start past
   one corner and stop short of the other, while the bar runs the full width.
   What fits is still decided by the width alone - which the cases above
   cover at every width - so the padded row must be exactly the unpadded one
   moved in, and nothing may land outside its span. The router draws and
   hit-tests from this one function; a copy of the shift in either place is
   how [R] and its tap would part company. */
LS_CASE(a_padded_status_row_is_the_same_row_moved_in)
{
    const int brand = 9, name = 4, left = 12, right = 16;
    for (int w = 1; w <= 130; w++)
        for (int x0 = 0; x0 <= 5; x0++) {
            const ls_tui_status_layout_t a =
                ls_tui_status_layout(w, brand, name, left, right);
            const ls_tui_status_layout_t b =
                ls_tui_status_layout_at(x0, w, brand, name, left, right);

            const int was[6] = { a.clock_x, a.help_x, a.brand_x,
                                 a.name_x, a.left_x, a.right_x };
            const int now[6] = { b.clock_x, b.help_x, b.brand_x,
                                 b.name_x, b.left_x, b.right_x };
            for (int i = 0; i < 6; i++)
                LS_CHECK_MSG(was[i] < 0 ? now[i] < 0 : now[i] == was[i] + x0,
                             "field %d at width %d, pad %d: %d became %d",
                             i, w, x0, was[i], now[i]);

            int s[4][2];
            const int n = spans(b, brand, name, left, right, s);
            for (int i = 0; i < n; i++)
                LS_CHECK_MSG(s[i][0] >= x0 && s[i][1] <= x0 + w,
                             "a field left the span [%d,%d) at width %d",
                             x0, x0 + w, w);
            /* The clock took the turn control's slot. */
            if (b.clock_x >= 0)
                LS_CHECK(b.clock_x >= x0 &&
                         b.clock_x + LS_TUI_CLOCK_W <= x0 + w);
            if (b.help_x >= 0)
                LS_CHECK(b.help_x >= x0 &&
                         b.help_x + LS_TUI_HELP_W <= x0 + w);
        }
}
