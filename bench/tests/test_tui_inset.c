/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_tui_inset.c */
/* The grid was inset by the corner radius on every side. */
#include "ls_test.h"
#include "ls_tui_inset.h"

#include <math.h>

/* The panel this was measured on, and the two faces the firmware offers. */
#define W   568
#define H  1232

LS_CASE(a_point_past_the_arc_centre_on_either_axis_is_clear)
{
    /* The arc only exists in the radius x radius square at each corner. */
    LS_CHECK(ls_tui_corner_clear(40, 0, 40));
    LS_CHECK(ls_tui_corner_clear(0, 40, 40));
    LS_CHECK(ls_tui_corner_clear(41, 3, 40));
    /* And the origin itself never is. */
    LS_CHECK(!ls_tui_corner_clear(0, 0, 40));
}

LS_CASE(the_diagonal_threshold_is_point_293_of_the_radius)
{
    /* R(1 - 1/sqrt2) = 11.7 for R = 40, so 11 is clipped and 12 is clear. */
    LS_CHECK(!ls_tui_corner_clear(11, 11, 40));
    LS_CHECK(ls_tui_corner_clear(12, 12, 40));
}

LS_CASE(no_radius_means_the_whole_panel)
{
    /* A square panel gives the grid every cell that fits, and the pixels
       that do not make a whole cell are split between the two edges rather
       than left on one - 568 is 56 cells of 10 with 8 px over and 1232 is 72 of 17 with 8 over, so 4 a side either way. */
    int ox = -1, oy = -1;
    ls_tui_corner_inset(W, H, 10, 17, 0, &ox, &oy);
    LS_EQ_INT(56, (W - 2 * ox) / 10);
    LS_EQ_INT(72, (H - 2 * oy) / 17);
    LS_EQ_INT(4, ox);
    LS_EQ_INT(4, oy);
    LS_CHECK(ls_tui_corner_clear(0, 0, 0));
}

LS_CASE(whatever_it_picks_actually_clears_the_arc)
{
    /* The property that matters, over every face and radius in range. */
    const int cells[][2] = { { 10, 17 }, { 9, 16 }, { 8, 8 }, { 16, 32 } };
    for (int f = 0; f < 4; f++) {
        for (int r = 0; r <= 80; r++) {
            int ox = -1, oy = -1;
            ls_tui_corner_inset(W, H, cells[f][0], cells[f][1], r, &ox, &oy);
            LS_CHECK(ox >= 0 && oy >= 0);
            LS_CHECK(ls_tui_corner_clear(ox, oy, r));
        }
    }
}

LS_CASE(the_grid_is_centred_so_opposite_margins_match)
{

    const int cells[][2] = { { 10, 17 }, { 9, 16 } };
    for (int f = 0; f < 2; f++) {
        const int cw = cells[f][0], ch = cells[f][1];
        int ox = 0, oy = 0;
        ls_tui_corner_inset(W, H, cw, ch, 40, &ox, &oy);
        const int cols = (W - 2 * ox) / cw, rows = (H - 2 * oy) / ch;
        /* Whatever is left over after the grid is at most one pixel more on
           the far side, which is what integer halving costs. */
        LS_CHECK(W - (ox + cols * cw) - ox <= 1);
        LS_CHECK(H - (oy + rows * ch) - oy <= 1);
    }
}

LS_CASE(it_prefers_an_even_border_to_a_bigger_grid)
{
    /* Maximising cells picks 56x69 at 4,29 on this panel. That has more
       cells than what this returns and stands seven times further off the
       top than the sides, which is the answer a person would call wrong. */
    int ox = 0, oy = 0;
    ls_tui_corner_inset(W, H, 10, 17, 40, &ox, &oy);
    const int worst = ox > oy ? ox : oy;
    LS_CHECK(worst < 29);
}

static long old_grid(int screen_w, int screen_h, int cw, int ch, int radius)
{
    const int ox = ((radius + cw - 1) / cw) * cw;
    const int oy = ((radius + ch - 1) / ch) * ch;
    return (long)((screen_w - 2 * ox) / cw) * ((screen_h - 2 * oy) / ch);
}

LS_CASE(it_beats_the_figure_it_replaces_on_both_faces)
{
    const int cells[][2] = { { 10, 17 }, { 9, 16 } };
    for (int f = 0; f < 2; f++) {
        const int cw = cells[f][0], ch = cells[f][1];
        int ox = 0, oy = 0;
        ls_tui_corner_inset(W, H, cw, ch, 40, &ox, &oy);
        const long now = (long)((W - 2 * ox) / cw) * ((H - 2 * oy) / ch);
        LS_CHECK(now > old_grid(W, H, cw, ch, 40));
        /* And not by ignoring the arc it exists to clear. */
        LS_CHECK(ls_tui_corner_clear(ox, oy, 40));
    }
}

/* The measured numbers, so a change to the search shows up as a diff rather
   than as a panel that looks different. 10x17 went 48x66 -> 54x70. */
/* The measured numbers, so a change to the search shows up as a diff rather
   than as a panel that looks different. The old line gave 48x66 standing 40
   and 51 px off. */
LS_CASE(the_grid_this_panel_gets)
{
    int ox = 0, oy = 0;
    ls_tui_corner_inset(W, H, 10, 17, 40, &ox, &oy);
    LS_EQ_INT(14, ox);
    LS_EQ_INT(12, oy);
    LS_EQ_INT(54, (W - 2 * ox) / 10);
    LS_EQ_INT(71, (H - 2 * oy) / 17);
}

LS_CASE(a_smaller_radius_never_gives_a_worse_border)
{
    int prev = 1 << 30;
    for (int r = 80; r >= 0; r--) {
        int ox = 0, oy = 0;
        ls_tui_corner_inset(W, H, 10, 17, r, &ox, &oy);
        const int worst = ox > oy ? ox : oy;
        LS_CHECK(worst <= prev);
        prev = worst;
    }
    /* And a square panel gives the grid the whole glass. */
    int ox = -1, oy = -1;
    ls_tui_corner_inset(W, H, 10, 17, 0, &ox, &oy);
    LS_EQ_INT(56, (W - 2 * ox) / 10);
    LS_EQ_INT(72, (H - 2 * oy) / 17);
}

LS_CASE(landscape_is_the_same_panel_turned_over)
{
    int ox = 0, oy = 0;
    ls_tui_corner_inset(H, W, 10, 17, 40, &ox, &oy);
    LS_CHECK(ls_tui_corner_clear(ox, oy, 40));
    LS_CHECK((H - 2 * ox) / 10 > 0);
    LS_CHECK((W - 2 * oy) / 17 > 0);
}

LS_CASE(a_radius_bigger_than_the_panel_does_not_produce_a_negative_grid)
{
    int ox = -1, oy = -1;
    ls_tui_corner_inset(64, 64, 10, 17, 400, &ox, &oy);
    LS_CHECK(ox >= 0 && oy >= 0);
    LS_CHECK(64 - 2 * ox > 0);
    LS_CHECK(64 - 2 * oy > 0);
}

LS_CASE(nonsense_dimensions_are_refused_rather_than_divided_by)
{
    int ox = -1, oy = -1;
    ls_tui_corner_inset(0, 0, 0, 0, 40, &ox, &oy);
    LS_EQ_INT(0, ox);
    LS_EQ_INT(0, oy);
    ls_tui_corner_inset(W, H, -10, 17, 40, &ox, &oy);
    LS_EQ_INT(0, ox);
    LS_EQ_INT(0, oy);
}

/* ============================================ words off the arc == */

/*"the very top bar in portrait needs to have the text pushed in a
   little to avoid the round corners" - after . The status row's first
   and last cells were the grid's corner cells, which that change put 1.8 px
   from the arc while every other cell on an edge stands 12 to 14 px off the
   glass. These cases are the arithmetic that says how far in the words go. */

/* The panels the gate builds, as the firmware holds them: the T-Display-P4's
   rounded 568x1232 and the two square Waveshare panels, each both ways up.
   The square ones declare no radius; they are swept through the same radii
   anyway, so the day one of them declares one its answer is already checked. */
static const struct { int w, h, r; } PANELS[] = {
    {  568, 1232, 40 }, { 1232,  568, 40 },
    {  480,  800,  0 }, {  800,  480,  0 },
    {  720,  720,  0 },
};
#define N_PANELS ((int)(sizeof(PANELS) / sizeof(PANELS[0])))

/* The two faces the firmware offers. */
static const int FACES[][2] = { { 10, 17 }, { 8, 15 } };

/* How far inside the glass a point is, in pixels, negative when it is off
   it: the signed distance to a rounded rectangle, in its textbook form.
   Floating point and a different shape from the firmware's integer corner
   test on purpose, so that the two agreeing means something. */
static double inside(double x, double y, int w, int h, int r)
{
    const double hx = w / 2.0, hy = h / 2.0;
    const double qx = fabs(x - hx) - (hx - r);
    const double qy = fabs(y - hy) - (hy - r);
    const double ex = qx > 0 ? qx : 0, ey = qy > 0 ? qy : 0;
    const double mx = qx > qy ? qx : qy;
    return r - sqrt(ex * ex + ey * ey) - (mx < 0 ? mx : 0);
}

/* The least that any part of a cell is inside the glass. The glass is
   convex, so over a rectangle that minimum is at one of its corners. */
static double cell_inside(int x0, int y0, int cw, int ch, int w, int h, int r)
{
    double least = inside(x0, y0, w, h, r);
    const double c[3] = { inside(x0 + cw, y0, w, h, r),
                          inside(x0, y0 + ch, w, h, r),
                          inside(x0 + cw, y0 + ch, w, h, r) };
    for (int i = 0; i < 3; i++) if (c[i] < least) least = c[i];
    return least;
}

LS_CASE(the_arc_covers_no_cell_of_the_grid_it_was_laid_out_against)
{
    /* Why the padding is not "the cells the arc covers": there are none to
       count. Every cell at both ends of every row, on every panel, face and
       radius, is wholly on the glass - checked with the distance above rather
       than with the corner test the inset search used. So a rule stated in
       terms of coverage would move nothing, on this panel or any other. */
    for (int p = 0; p < N_PANELS; p++)
        for (int f = 0; f < 2; f++)
            for (int r = 0; r <= 80; r += 2) {
                const int w = PANELS[p].w, h = PANELS[p].h;
                const int cw = FACES[f][0], ch = FACES[f][1];
                int ox = 0, oy = 0;
                ls_tui_corner_inset(w, h, cw, ch, r, &ox, &oy);
                const int cols = (w - 2 * ox) / cw, rows = (h - 2 * oy) / ch;
                int off = -1;
                double a = 0, b = 0;
                for (int row = 0; row < rows && off < 0; row++) {
                    const int y0 = oy + row * ch;
                    a = cell_inside(ox, y0, cw, ch, w, h, r);
                    b = cell_inside(ox + (cols - 1) * cw, y0, cw, ch, w, h, r);
                    if (a < -1e-9 || b < -1e-9) off = row;
                }
                LS_CHECK_MSG(off < 0,
                             "%dx%d r=%d %dx%d: row %d has a cell off the "
                             "glass (%.2f, %.2f px)", w, h, r, cw, ch, off, a, b);
            }
}

LS_CASE(every_word_keeps_the_grids_own_standoff_from_the_glass)
{
    /* The property the padding exists for, checked against the independent
       distance: on every row probed, every cell from the padding inward is
       at least the grid's straight-edge margin inside the glass, and the
       last cell padded is not - so the padding is enough and never a cell
       more than enough. Rows 0 and last are the ones the chrome puts words
       on; the others show it is zero where there is no corner. */
    for (int p = 0; p < N_PANELS; p++)
        for (int f = 0; f < 2; f++)
            for (int r = 0; r <= 80; r += 2) {
                const int w = PANELS[p].w, h = PANELS[p].h;
                const int cw = FACES[f][0], ch = FACES[f][1];
                int ox = 0, oy = 0;
                ls_tui_corner_inset(w, h, cw, ch, r, &ox, &oy);
                const int cols = (w - 2 * ox) / cw, rows = (h - 2 * oy) / ch;
                int m = ox;
                if (oy < m) m = oy;
                if (w - ox - cols * cw < m) m = w - ox - cols * cw;
                if (h - oy - rows * ch < m) m = h - oy - rows * ch;

                const int probe[5] = { 0, 1, rows / 2, rows - 2, rows - 1 };
                for (int k = 0; k < 5; k++) {
                    const int row = probe[k];
                    if (row < 0 || row >= rows) continue;
                    const int y0 = oy + row * ch;
                    const int pad =
                        ls_tui_corner_cells(w, h, cw, ch, r, ox, oy, row);
                    LS_CHECK_MSG(pad >= 0 && pad <= cols / 2,
                                 "%dx%d r=%d row %d: pad %d of %d columns",
                                 w, h, r, row, pad, cols);

                    int short_cell = -1;
                    double d = 0;
                    for (int c = pad; c < cols - pad && short_cell < 0; c++) {
                        d = cell_inside(ox + c * cw, y0, cw, ch, w, h, r);
                        if (d < m - 1e-9) short_cell = c;
                    }
                    LS_CHECK_MSG(short_cell < 0,
                                 "%dx%d r=%d %dx%d row %d: cell %d is %.2f px "
                                 "inside against a %d px margin, pad %d",
                                 w, h, r, cw, ch, row, short_cell, d, m, pad);

                    if (pad > 0) {
                        const double a = cell_inside(ox + (pad - 1) * cw, y0,
                                                     cw, ch, w, h, r);
                        const double b = cell_inside(ox + (cols - pad) * cw, y0,
                                                     cw, ch, w, h, r);
                        LS_CHECK_MSG(a < m - 1e-9 || b < m - 1e-9,
                                     "%dx%d r=%d %dx%d row %d: pad %d includes "
                                     "a cell that already has the margin "
                                     "(%.2f, %.2f against %d)",
                                     w, h, r, cw, ch, row, pad, a, b, m);
                    }
                }
            }
}

LS_CASE(the_padding_this_panel_gets)
{

    int ox = 0, oy = 0;
    ls_tui_corner_inset(W, H, 10, 17, 40, &ox, &oy);
    LS_EQ_INT(3, ls_tui_corner_cells(W, H, 10, 17, 40, ox, oy, 0));
    LS_EQ_INT(1, ls_tui_corner_cells(W, H, 10, 17, 40, ox, oy, 1));
    LS_EQ_INT(0, ls_tui_corner_cells(W, H, 10, 17, 40, ox, oy, 35));
    LS_EQ_INT(2, ls_tui_corner_cells(W, H, 10, 17, 40, ox, oy, 70));

    ls_tui_corner_inset(H, W, 10, 17, 40, &ox, &oy);
    LS_EQ_INT(16, ox);
    LS_EQ_INT(12, oy);
    LS_EQ_INT(3, ls_tui_corner_cells(H, W, 10, 17, 40, ox, oy, 0));
    LS_EQ_INT(0, ls_tui_corner_cells(H, W, 10, 17, 40, ox, oy, 1));
    LS_EQ_INT(3, ls_tui_corner_cells(H, W, 10, 17, 40, ox, oy, 31));
}

LS_CASE(a_square_cornered_panel_pads_nothing_anywhere)
{
    /* The two Waveshare panels declare no radius, so nothing moves on them -
       any row, either posture, either face. */
    for (int p = 0; p < N_PANELS; p++) {
        if (PANELS[p].r) continue;
        for (int f = 0; f < 2; f++) {
            const int w = PANELS[p].w, h = PANELS[p].h;
            const int cw = FACES[f][0], ch = FACES[f][1];
            int ox = 0, oy = 0;
            ls_tui_corner_inset(w, h, cw, ch, 0, &ox, &oy);
            const int rows = (h - 2 * oy) / ch;
            int moved = -1;
            for (int row = 0; row < rows && moved < 0; row++)
                if (ls_tui_corner_cells(w, h, cw, ch, 0, ox, oy, row) != 0)
                    moved = row;
            LS_CHECK_MSG(moved < 0, "%dx%d %dx%d: row %d was padded",
                         w, h, cw, ch, moved);
        }
    }
}

LS_CASE(corner_padding_refuses_what_it_should_not_get)
{
    int ox = 0, oy = 0;
    ls_tui_corner_inset(W, H, 10, 17, 40, &ox, &oy);
    LS_EQ_INT(0, ls_tui_corner_cells(W, H, 10, 17, 40, ox, oy, -1));
    LS_EQ_INT(0, ls_tui_corner_cells(W, H, 10, 17, 40, ox, oy, 71));
    LS_EQ_INT(0, ls_tui_corner_cells(0, 0, 10, 17, 40, 0, 0, 0));
    LS_EQ_INT(0, ls_tui_corner_cells(W, H, 0, 17, 40, ox, oy, 0));
    LS_EQ_INT(0, ls_tui_corner_cells(W, H, 10, -17, 40, ox, oy, 0));
    LS_EQ_INT(0, ls_tui_corner_cells(W, H, 10, 17, 40, -1, oy, 0));
    /* A radius far past the panel still leaves half the row for words. */
    LS_CHECK(ls_tui_corner_cells(W, H, 10, 17, 4000, ox, oy, 0) <= 54 / 2);
}
