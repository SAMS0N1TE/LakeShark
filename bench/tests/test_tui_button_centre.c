/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_tui_ui.c */
/* Button labels sit where they should, mechanically, at every geometry.

   This exists because "the labels drifted again" was only ever caught by
   looking at the panel, and a screenshot is not a regression test. The hit
   table already records where each button was drawn, so the check is: render
   a bar, read the rects back, and measure the lettering inside each one.

   What is deliberately NOT checked: the border rows. Shortcut badges ([K]),
   state badges ([*], [+]) and the focus caret are drawn on the frame on
   purpose, off to one side. Only the interior - where the label and its value
   live - has to be centred. */

#include "ls_test.h"
#include "ls_tui_ui.h"
#include "tui_core.h"

#include <ctype.h>
#include <string.h>

/* ls_tui_ui.c asks the router about orientation and the panel about its
   rounded corner. The shared stub pins wide to true; this test needs both, so
   it brings its own. */
static bool g_wide = true;
bool ls_tui_is_wide(void) { return g_wide; }
int  ls_tui_corner_pad(int row) { (void)row; return 0; }

#define MAX_W 160
#define MAX_H  40

static tui_cell s_back[MAX_W * MAX_H];
static tui_cell s_front[MAX_W * MAX_H];
static tui_surface s_sf;

static void surface(void)
{
    tui_surface_setup(&s_sf, s_back, s_front, MAX_W, MAX_H);
    tui_frame_begin(&s_sf);
}

/* Lettering, as opposed to the dither texture (shade glyphs), the border
   (- = | +) and the badge brackets. */
static bool is_ink(char c) { return isalnum((unsigned char)c) != 0; }

static char cell_at(int x, int y)
{
    if (x < 0 || y < 0 || x >= s_sf.w || y >= s_sf.h) return ' ';
    return s_sf.back[(size_t)y * s_sf.w + x].ch;
}

static bool ink_span(int x, int y, int w, int *first, int *last)
{
    int f = -1, l = -1;
    for (int i = 0; i < w; i++) {
        if (!is_ink(cell_at(x + i, y))) continue;
        if (f < 0) f = i;
        l = i;
    }
    if (f < 0) return false;
    *first = f;
    *last  = l;
    return true;
}

/* Gaps either side of the lettering differ by at most one, because an odd
   remainder cannot be split evenly and one cell is invisible. */
static int check_interior(const char *where)
{
    int seen = 0;
    const int n = ls_btn_count_slot(LS_BTN_SLOT_SCREEN);
    for (int i = 0; i < n; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, i, &x, &y, &w, &h));

        /* Rows the frame does not own. */
        const int r0 = h >= 3 ? y + 1 : y;
        const int r1 = h >= 3 ? y + h - 2 : y + h - 1;

        for (int r = r0; r <= r1; r++) {
            int first = 0, last = 0;
            if (!ink_span(x, r, w, &first, &last)) continue;
            const int left = first, right = w - 1 - last;
            const int skew = left > right ? left - right : right - left;
            seen++;
            LS_CHECK_MSG(skew <= 1,
                         "%s: button %d of %d at x=%d w=%d h=%d, row %d has "
                         "ink in [%d,%d] - %d of gap left, %d right",
                         where, i, n, x, w, h, r - y, first, last, left, right);
        }
    }
    return seen;
}

static const ls_btn_t MIXED[] = {
    { .label = "REF" },   { .label = "RANGE" }, { .label = "AVG" },
    { .label = "PAL" },   { .label = "GRAIN" }, { .label = "SPLIT" },
    { .label = "PEAK" },  { .label = "HOLD" },  { .label = "SRC" },
    { .label = "MK" },
};
#define MIXED_N ((int)(sizeof(MIXED) / sizeof(MIXED[0])))

/* ------------------------------------------------------------------------ */

LS_CASE(labels_are_centred_in_portrait_where_there_are_no_badges)
{
    g_wide = false;
    int seen = 0;
    for (int count = 1; count <= MIXED_N; count++)
        for (int w = 40; w <= 130; w += 3)
            for (int h = 2; h <= 6; h++) {
                surface();
                ls_btn_bar(&s_sf, tui_rect_make(0, 0, w, h), MIXED, count, -1);
                seen += check_interior("portrait");
            }
    LS_CHECK_MSG(seen > 100, "only %d label rows examined", seen);
}

LS_CASE(labels_are_centred_in_landscape_alongside_shortcut_badges)
{
    /* Same sweep with key hints on. The badge lives on the frame, so the
       interior should read exactly as it does in portrait. */
    g_wide = true;
    const ls_btn_t keyed[] = {
        { .label = "DECODE", .key = '1' }, { .label = "SIGNAL", .key = '2' },
        { .label = "LOG",    .key = '3' }, { .label = "SET",    .key = '4' },
    };
    int seen = 0;
    for (int w = 40; w <= 130; w += 3)
        for (int h = 3; h <= 6; h++) {
            surface();
            ls_btn_bar(&s_sf, tui_rect_make(0, 0, w, h), keyed, 4, -1);
            seen += check_interior("landscape");
        }
    LS_CHECK_MSG(seen > 40, "only %d label rows examined", seen);
}

LS_CASE(a_raised_bar_centres_its_labels_too)
{
    g_wide = true;
    const ls_btn_t vals[] = {
        { .label = "GAIN", .value = "28.0", .key = 'g' },
        { .label = "SQL",  .value = "15",   .key = 's' },
        { .label = "VOL",  .value = "60",   .key = 'v' },
    };
    int seen = 0;
    for (int w = 40; w <= 130; w += 3) {
        const int h = ls_btn_raised_height(tui_rect_make(0, 0, w, 12), 3);
        if (h < 3) continue;
        surface();
        ls_btn_bar_raised(&s_sf, tui_rect_make(0, 0, w, h), vals, 3, -1);
        seen += check_interior("raised");
    }
    LS_CHECK_MSG(seen > 20, "only %d label rows examined", seen);
}

LS_CASE(lettering_never_lands_on_the_frame)
{
    /* A two-line button only has room for two lines inside a three-row box if
       the frame is not counted. Getting this wrong writes the label over the
       top border, which reads as a broken box rather than a shifted word. */
    g_wide = false;
    const ls_btn_t valued[] = {
        { .label = "GAIN", .value = "28.0" },
        { .label = "SQL",  .value = "15" },
    };
    for (int w = 30; w <= 120; w += 3)
        for (int h = 3; h <= 6; h++) {
            surface();
            ls_btn_bar(&s_sf, tui_rect_make(0, 0, w, h), valued, 2, -1);
            const int n = ls_btn_count_slot(LS_BTN_SLOT_SCREEN);
            for (int i = 0; i < n; i++) {
                int x = 0, y = 0, bw = 0, bh = 0;
                LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, i,
                                          &x, &y, &bw, &bh));
                if (bh < 3) continue;
                int f = 0, l = 0;
                LS_CHECK_MSG(!ink_span(x + 1, y, bw - 2, &f, &l),
                             "w=%d h=%d button %d: lettering on the top frame "
                             "row at [%d,%d]", w, bh, i, f, l);
                LS_CHECK_MSG(!ink_span(x + 1, y + bh - 1, bw - 2, &f, &l),
                             "w=%d h=%d button %d: lettering on the bottom "
                             "frame row at [%d,%d]", w, bh, i, f, l);
            }
        }
}

LS_CASE(buttons_never_overlap_or_leave_the_bar)
{
    g_wide = true;
    for (int w = 30; w <= 130; w += 3)
        for (int h = 2; h <= 8; h++) {
            surface();
            const tui_rect bar = tui_rect_make(2, 1, w, h);
            ls_btn_bar(&s_sf, bar, MIXED, MIXED_N, -1);
            const int n = ls_btn_count_slot(LS_BTN_SLOT_SCREEN);
            for (int i = 0; i < n; i++) {
                int ax = 0, ay = 0, aw = 0, ah = 0;
                LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, i,
                                          &ax, &ay, &aw, &ah));
                LS_CHECK(ax >= bar.x && ax + aw <= bar.x + bar.w);
                LS_CHECK(ay >= bar.y && ay + ah <= bar.y + bar.h);
                for (int j = i + 1; j < n; j++) {
                    int bx = 0, by = 0, bw = 0, bh = 0;
                    LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, j,
                                              &bx, &by, &bw, &bh));
                    const bool x_hit = ax < bx + bw && bx < ax + aw;
                    const bool y_hit = ay < by + bh && by < ay + ah;
                    LS_CHECK(!(x_hit && y_hit));
                }
            }
        }
}

LS_CASE(a_tap_anywhere_in_a_button_finds_that_button)
{
    /* The rects this file measures are the ones touch uses, so if centring
       work ever moves a box without moving its hit rect, this fails rather
       than the panel going quietly dead. */
    g_wide = true;
    surface();
    ls_btn_bar(&s_sf, tui_rect_make(0, 0, 60, 3), MIXED, 3, -1);
    const int n = ls_btn_count_slot(LS_BTN_SLOT_SCREEN);
    LS_EQ_INT(3, n);
    for (int i = 0; i < n; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, i, &x, &y, &w, &h));
        LS_EQ_INT(i, ls_btn_hit_slot(x, y, LS_BTN_SLOT_SCREEN));
        LS_EQ_INT(i, ls_btn_hit_slot(x + w - 1, y + h - 1, LS_BTN_SLOT_SCREEN));
    }
}


/* The grid as text, so a test can ask whether a word survived the layout. */
static bool grid_has(const char *want)
{
    char line[MAX_W + 1];
    for (int y = 0; y < s_sf.h; y++) {
        for (int x = 0; x < s_sf.w; x++) line[x] = cell_at(x, y);
        line[s_sf.w] = 0;
        if (strstr(line, want)) return true;
    }
    return false;
}

/* WHETHER THE SHORT BAR CAN BE AFFORDED.

   Three rows is a frame plus one usable line, so a button carrying a value
   writes "LABEL VALUE" into it rather than giving the value a row of its own.
   That is what buys a landscape pane its height back, and what cuts a word in
   half when the pane is not wide enough. ls_btn_compact_fits is the question
   asked before the height is chosen, and it has to answer with the SAME
   numbers the renderer lays out with - a disagreement either wastes rows or
   clips a label, and the second one is silent.

   SUB-GHZ's own bar is the worked example: a full landscape pane gives each
   of five buttons sixteen columns and thirteen for the text, and the longest
   pair - SETUP CAPTURE - is exactly thirteen. The half-width split gives
   eleven and eight, and it does not. */
LS_CASE(the_compact_bar_is_offered_only_where_the_longest_pair_fits)
{
    const ls_btn_t bar[] = {
        {"TUNE","MHz",'f',false,false},
        {"SETUP","CAPTURE",'s',false,false},
        {"WATCH","OFF",'w',false,false},
        {"SCAN","OFF",'n',false,false},
        {"CAPTURE","MENU",'c',false,false},
    };
    LS_CHECK(ls_btn_compact_fits(tui_rect_make(1,2,113,3),bar,5));
    LS_CHECK(!ls_btn_compact_fits(tui_rect_make(1,2,56,3),bar,5));
    /* And what the renderer does with each answer, measured rather than
       assumed: the width it says yes at puts the pair on one line, and the
       width it says no at would have cut it. */
    for (int w = 40; w <= 130; w++) {
        const bool fits = ls_btn_compact_fits(tui_rect_make(1,2,w,3),bar,5);
        surface();
        ls_btn_bar_raised(&s_sf, tui_rect_make(0,0,w,3), bar, 5, -1);
        const bool drawn = grid_has("SETUP CAPTURE");
        LS_CHECK_MSG(fits == drawn,
                     "w=%d: compact_fits said %d, the bar drew %d",
                     w, (int)fits, (int)drawn);
    }
}

/* A button with no value needs no room for one, and an empty or impossible
   bar answers no rather than dividing by something. */
LS_CASE(the_compact_bar_refuses_what_it_should_not_get)
{
    const ls_btn_t labels[] = {
        {"HOME",NULL,'h',false,false},
        {"BACK",NULL,'b',false,false},
    };
    LS_CHECK(ls_btn_compact_fits(tui_rect_make(0,0,40,3),labels,2));
    /* Nine characters plus a frame needs twelve columns a side. */
    LS_CHECK(!ls_btn_compact_fits(tui_rect_make(0,0,12,3),labels,2));
    LS_CHECK(!ls_btn_compact_fits(tui_rect_make(0,0,113,3),NULL,5));
    LS_CHECK(!ls_btn_compact_fits(tui_rect_make(0,0,113,3),labels,0));
    LS_CHECK(!ls_btn_compact_fits(tui_rect_make(0,0,2,3),labels,2));
}
