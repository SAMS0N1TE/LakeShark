/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_skyplot.c */

#include "ls_test.h"
#include "ls_skyplot.h"

/* The panel's own cell, and a plot that fits the portrait grid. */
#define CW 10
#define CH 17
#define ROWS 15
#define COLS 27

static void at(int az, int el, int *r, int *c)
{
    LS_CHECK(ls_sky_project(az, el, ROWS, COLS, CW, CH, r, c));
}

LS_CASE(straight_up_is_the_middle)
{
    int r = -1, c = -1;
    at(0, 90, &r, &c);
    LS_EQ_INT(ROWS / 2, r);
    LS_EQ_INT(COLS / 2, c);
    /* And the azimuth of something directly overhead does not move it. */
    for (int az = 0; az < 360; az += 37) {
        int r2, c2;
        at(az, 90, &r2, &c2);
        LS_EQ_INT(r, r2);
        LS_EQ_INT(c, c2);
    }
}

LS_CASE(north_is_up_and_east_is_right)
{
    /* The whole point. Rows increase downwards, so north is a NEGATIVE row
       offset - the one minus sign that decides whether this plot is a sky
       plot or its mirror image. */
    int r, c;
    const int cy = ROWS / 2, cx = COLS / 2;

    at(0, 0, &r, &c);                       /* due north, on the horizon */
    LS_CHECK_MSG(r < cy, "north came out below centre (row %d of %d)", r, cy);
    LS_EQ_INT(cx, c);

    at(90, 0, &r, &c);                      /* due east */
    LS_CHECK_MSG(c > cx, "east came out left of centre (col %d of %d)", c, cx);
    LS_EQ_INT(cy, r);

    at(180, 0, &r, &c);                     /* due south */
    LS_CHECK(r > cy);
    LS_EQ_INT(cx, c);

    at(270, 0, &r, &c);                     /* due west */
    LS_CHECK(c < cx);
    LS_EQ_INT(cy, r);
}

LS_CASE(the_plot_is_round_in_pixels_not_in_cells)
{
    /* A 17 pixel row against a 10 pixel column: the horizon due east must be
       further out in COLUMNS than the horizon due north is in ROWS, by about
       the cell aspect. Equal radii would make these equal and the plot an
       ellipse. */
    int rn, cn, re, ce;
    at(0, 0, &rn, &cn);
    at(90, 0, &re, &ce);

    const int up    = (ROWS / 2) - rn;      /* rows from centre to the rim */
    const int right = ce - (COLS / 2);      /* columns to the rim          */
    LS_CHECK(up > 0 && right > 0);
    LS_CHECK_MSG(right > up,
                 "east reach %d is not wider than north reach %d - the plot "
                 "is an ellipse", right, up);

    /* 17/10 is 1.7. Allow for the clamp to the half-width and for rounding
       on a small grid. */
    LS_CHECK(right <= up * 2);
}

LS_CASE(elevation_moves_a_satellite_in_from_the_rim)
{
    /* Monotonic: higher is nearer the middle, all the way up. An inverted
       radius draws a sky that is inside out and is not obviously wrong. */
    const int cy = ROWS / 2;
    int prev = -1;
    for (int el = 0; el <= 90; el += 10) {
        int r, c;
        at(0, el, &r, &c);
        const int from_centre = cy - r;
        if (prev >= 0) LS_CHECK(from_centre <= prev);
        prev = from_centre;
    }
    LS_EQ_INT(0, prev);                     /* 90 degrees is the centre */
}

LS_CASE(nothing_lands_outside_the_plot)
{
    /* Every azimuth and every elevation, on the real grid and on some
       awkward ones. The clamp is deliberate - a satellite on the horizon is
       the one worth seeing - so this checks it actually holds. */
    const int grids[][2] = { { 15, 27 }, { 9, 9 }, { 3, 3 }, { 21, 40 } };
    for (int gi = 0; gi < 4; gi++) {
        const int rows = grids[gi][0], cols = grids[gi][1];
        for (int az = -720; az <= 720; az += 7) {
            for (int el = 0; el <= 90; el += 3) {
                int r = -99, c = -99;
                LS_CHECK(ls_sky_project(az, el, rows, cols, CW, CH, &r, &c));
                LS_CHECK_MSG(r >= 0 && r < rows,
                             "row %d outside %d at az %d el %d",
                             r, rows, az, el);
                LS_CHECK_MSG(c >= 0 && c < cols,
                             "col %d outside %d at az %d el %d",
                             c, cols, az, el);
            }
        }
    }
}

LS_CASE(an_empty_satellite_slot_is_refused_rather_than_drawn_due_north)
{
    /* A GSV slot that was never filled is all zeros, which is a
       valid-looking az 0 el 0 - due north on the horizon. Letting those
       through stacks every unused slot on one point of the rim, which reads
       as a cluster of real satellites in one direction: the exact picture
       this plot exists to make meaningful. The caller skips prn 0; this
       refuses the impossible elevations regardless. */
    int r, c;
    LS_CHECK(!ls_sky_project(0, -1, ROWS, COLS, CW, CH, &r, &c));
    LS_CHECK(!ls_sky_project(0, 91, ROWS, COLS, CW, CH, &r, &c));
    /* Zero elevation itself is legal - a satellite really can be on the
       horizon - so it must NOT be refused. */
    LS_CHECK(ls_sky_project(0, 0, ROWS, COLS, CW, CH, &r, &c));
}

LS_CASE(a_wrapped_azimuth_is_the_same_place_as_the_unwrapped_one)
{
    for (int az = 0; az < 360; az += 11) {
        int r1, c1, r2, c2, r3, c3;
        at(az, 30, &r1, &c1);
        at(az + 360, 30, &r2, &c2);
        at(az - 360, 30, &r3, &c3);
        LS_EQ_INT(r1, r2); LS_EQ_INT(c1, c2);
        LS_EQ_INT(r1, r3); LS_EQ_INT(c1, c3);
    }
    /* 360 is north, not a dropped satellite. */
    int rn, cn, r360, c360;
    at(0, 0, &rn, &cn);
    at(360, 0, &r360, &c360);
    LS_EQ_INT(rn, r360); LS_EQ_INT(cn, c360);
}

LS_CASE(an_unlocated_satellite_is_not_due_north_on_the_horizon)
{
    /* A GSV field the receiver has not filled in parses as zero, so
       an unlocated satellite arrives as azimuth 0 elevation 0. Seen on the
       board: nine visible with no fix and one sitting on top of the N
       marker. Same call this project made for a mesh advert at 0,0 and for
       ls_map_center. */
    LS_CHECK(!ls_sky_located(0, 0));

    /* Everything else is a place, including each axis on its own - a
       satellite due north at ten degrees is located, and so is one on the
       horizon to the east. Refusing those would hide real satellites. */
    LS_CHECK(ls_sky_located(0, 10));
    LS_CHECK(ls_sky_located(90, 0));
    LS_CHECK(ls_sky_located(1, 0));
    LS_CHECK(ls_sky_located(0, 1));
    LS_CHECK(ls_sky_located(180, 45));
}

LS_CASE(nonsense_grids_are_refused_rather_than_divided_by)
{
    int r, c;
    LS_CHECK(!ls_sky_project(0, 45, 2, 27, CW, CH, &r, &c));
    LS_CHECK(!ls_sky_project(0, 45, 15, 2, CW, CH, &r, &c));
    LS_CHECK(!ls_sky_project(0, 45, 15, 27, 0, CH, &r, &c));
    LS_CHECK(!ls_sky_project(0, 45, 15, 27, CW, 0, &r, &c));
    LS_CHECK(!ls_sky_project(0, 45, 15, 27, CW, CH, NULL, &c));
}

LS_CASE(the_glyph_ladder_climbs_and_a_seen_satellite_is_not_a_blank)
{
    /* Seen but not tracked reports snr 0, which ls_gps.h calls a real state.
       It must be visible, and it must not look like the strongest. */
    LS_CHECK(ls_sky_glyph(0) != ' ');
    /* And not a full stop, which the plot uses for its 45 degree
       ring - the two differ in colour, and a colour chosen to recede must
       not be the only thing separating furniture from a satellite. */
    LS_CHECK(ls_sky_glyph(0) != '.');
    LS_CHECK(ls_sky_glyph(0) != ls_sky_glyph(45));
    /* Four distinct steps above zero, ordered. */
    LS_CHECK(ls_sky_glyph(10) != ls_sky_glyph(25));
    LS_CHECK(ls_sky_glyph(25) != ls_sky_glyph(35));
    LS_CHECK(ls_sky_glyph(35) != ls_sky_glyph(45));
    /* And a negative reading does not index off the bottom. */
    LS_CHECK(ls_sky_glyph(-5) == ls_sky_glyph(0));
}
