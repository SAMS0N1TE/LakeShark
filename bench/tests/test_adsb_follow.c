/* LS_TEST_SOURCES: ls_follow.c, FOLLOW on the ADS-B mini map: the modes,
   what each puts in view, and the rules that keep the view from jumping */

#include "ls_test.h"
#include "ls_follow.h"

#include <math.h>
#include <stdbool.h>

/* The portrait mini map: 50 cells by 21, three map pixels by five a cell,
   and the tile size a 10-pixel cell gives. */
#define PW   150
#define PH   105
#define TILE 76
#define HOME_LAT 43.4445
#define HOME_LON -71.6473
#define FRAME_US 40000          /* 25 frames a second */
#define FIT  0.70               /* the share of the frame a fit fills */

/* lssim's traffic round Franklin, about five miles across. */
static const ls_follow_pt_t NEAR_HOME[] = {
    { 0xA1B2C3u, 43.490, -71.670 },
    { 0xA4C5D6u, 43.410, -71.690 },
    { 0xAABBCCu, 43.400, -71.600 },
    { 0xA7E001u, 43.455, -71.705 },
    { 0xA5FD03u, 43.425, -71.585 },
    { 0xA77000u, 43.465, -71.625 },
};
#define N_NEAR ((int)(sizeof(NEAR_HOME) / sizeof(NEAR_HOME[0])))

/* Four spread over about fifty miles: a fit well short of the cap. */
static const ls_follow_pt_t SPREAD[] = {
    { 0x000001u, 43.90, -71.65 },
    { 0x000002u, 43.10, -71.20 },
    { 0x000003u, 43.60, -72.10 },
    { 0x000004u, 43.30, -71.80 },
};
#define N_SPREAD ((int)(sizeof(SPREAD) / sizeof(SPREAD[0])))

static ls_follow_in_t input(ls_follow_mode_t mode, const ls_follow_pt_t *air, int n)
{
    return (ls_follow_in_t){
        .mode = mode, .pw = PW, .ph = PH, .tile_px = TILE,
        .zoom_lo = 0, .zoom_hi = 22, .zoom = 8,
        .have_rx = true, .rx_lat = HOME_LAT, .rx_lon = HOME_LON,
        .air = air, .n = n, .now_us = 100000000LL,
    };
}

/* Whether a point lands within `share` of the frame each way of its centre. */
static bool inside(const ls_follow_view_t *v, double lat, double lon, double share)
{
    double dx, dy;
    ls_follow_offset(v, TILE, lat, lon, &dx, &dy);
    return fabs(dx) <= share * PW / 2.0 + 1e-6 && fabs(dy) <= share * PH / 2.0 + 1e-6;
}

static bool all_inside(const ls_follow_view_t *v, const ls_follow_pt_t *p, int n, double share)
{
    for (int i = 0; i < n; i++)
        if (!inside(v, p[i].lat, p[i].lon, share)) return false;
    return true;
}

/* Frames for `ms` with the input as it stands: how many moved the view. */
static int run(ls_follow_view_t *v, ls_follow_in_t *in, int ms)
{
    int moved = 0;
    for (int t = 0; t < ms; t += FRAME_US / 1000) {
        in->now_us += FRAME_US;
        moved += ls_follow_step(v, in) ? 1 : 0;
    }
    return moved;
}

/* ---------------------------------------------------------------- modes -- */

LS_CASE(follow_key_steps_fit_all_selected_nearest_off_and_round_again)
{
    LS_EQ_INT(LS_FOLLOW_DEFAULT, LS_FOLLOW_ALL);
    LS_EQ_INT(ls_follow_next(LS_FOLLOW_OFF), LS_FOLLOW_ALL);
    LS_EQ_INT(ls_follow_next(LS_FOLLOW_ALL), LS_FOLLOW_SELECTED);
    LS_EQ_INT(ls_follow_next(LS_FOLLOW_SELECTED), LS_FOLLOW_NEAREST);
    LS_EQ_INT(ls_follow_next(LS_FOLLOW_NEAREST), LS_FOLLOW_OFF);
    /* A stored number from a later build is not a mode: back to OFF. */
    LS_EQ_INT(ls_follow_next((ls_follow_mode_t)9), LS_FOLLOW_OFF);
    LS_EQ_INT(ls_follow_next((ls_follow_mode_t)-1), LS_FOLLOW_OFF);

    /* Four presses go all the way round. */
    ls_follow_mode_t m = LS_FOLLOW_DEFAULT;
    for (int i = 0; i < LS_FOLLOW_MODES; i++) m = ls_follow_next(m);
    LS_EQ_INT(m, LS_FOLLOW_DEFAULT);

    LS_EQ_STR(ls_follow_name(LS_FOLLOW_OFF), "OFF");
    LS_EQ_STR(ls_follow_name(LS_FOLLOW_ALL), "FIT ALL");
    LS_EQ_STR(ls_follow_name(LS_FOLLOW_SELECTED), "SELECTED");
    LS_EQ_STR(ls_follow_name(LS_FOLLOW_NEAREST), "NEAREST");
    LS_EQ_STR(ls_follow_name((ls_follow_mode_t)7), "OFF");

    LS_CHECK(ls_follow_picks_zoom(LS_FOLLOW_ALL));
    LS_CHECK(ls_follow_picks_zoom(LS_FOLLOW_NEAREST));
    LS_CHECK(!ls_follow_picks_zoom(LS_FOLLOW_OFF));
    LS_CHECK(!ls_follow_picks_zoom(LS_FOLLOW_SELECTED));
}

LS_CASE(off_keeps_the_receiver_at_the_centre_at_the_operators_zoom)
{
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_OFF, SPREAD, N_SPREAD);
    in.zoom = 9;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_CHECK(v.valid);
    LS_NEAR(v.lat, HOME_LAT, 1e-9);
    LS_NEAR(v.lon, HOME_LON, 1e-9);
    LS_EQ_INT(v.zoom, 9);
    LS_CHECK(!v.picked);
    /* ZOOM+ is the map's own, and the view takes it at once. */
    LS_EQ_INT(ls_follow_nudge(&v, 1), 0);
    in.zoom = 10;
    in.now_us += FRAME_US;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_INT(v.zoom, 10);
    LS_EQ_INT(run(&v, &in, 3000), 0);
}

/* -------------------------------------------------------------- fit all -- */

LS_CASE(fit_all_frames_every_aircraft_at_the_closest_zoom_that_holds_them)
{
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, SPREAD, N_SPREAD);
    LS_CHECK(ls_follow_step(&v, &in));
    LS_CHECK(v.valid && v.picked);
    LS_EQ_INT(v.count, N_SPREAD);
    LS_EQ_INT(v.zoom, ls_follow_fit_zoom(SPREAD, N_SPREAD, PW, PH, TILE, FIT, 0, LS_FOLLOW_ZOOM_CAP));
    LS_CHECK_MSG(v.zoom > 4 && v.zoom < LS_FOLLOW_ZOOM_CAP,
                 "fifty miles of traffic fitted at zoom %d", v.zoom);
    /* Every one in, with the margin a callsign needs. */
    LS_CHECK(all_inside(&v, SPREAD, N_SPREAD, FIT));

    /* And the closest that does: one step in, one would not be. */
    ls_follow_view_t closer = v;
    closer.zoom++;
    LS_CHECK(!all_inside(&closer, SPREAD, N_SPREAD, FIT));

    /* Centred on them, not on the receiver: the outermost each way are the
       same distance from the middle. */
    double l = 0, r = 0, t = 0, b = 0;
    for (int i = 0; i < N_SPREAD; i++) {
        double dx, dy;
        ls_follow_offset(&v, TILE, SPREAD[i].lat, SPREAD[i].lon, &dx, &dy);
        if (dx < l) l = dx;
        if (dx > r) r = dx;
        if (dy < t) t = dy;
        if (dy > b) b = dy;
    }
    LS_NEAR(l + r, 0, 0.5);
    LS_NEAR(t + b, 0, 0.5);
}

LS_CASE(fit_all_of_one_stops_at_the_cap_and_of_none_rests_on_the_receiver)
{
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, NEAR_HOME, 1);
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_INT(v.zoom, LS_FOLLOW_ZOOM_CAP);
    LS_NEAR(v.lat, NEAR_HOME[0].lat, 1e-6);
    LS_NEAR(v.lon, NEAR_HOME[0].lon, 1e-6);

    /* A map whose archive starts above the cap is held to the archive. */
    ls_follow_clear(&v);
    in.zoom_lo = 12;
    in.zoom_hi = 15;
    ls_follow_step(&v, &in);
    LS_EQ_INT(v.zoom, 12);

    /* Nothing with a position: the receiver, at the zoom there is. */
    ls_follow_clear(&v);
    in = input(LS_FOLLOW_ALL, NEAR_HOME, 0);
    in.zoom = 9;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_NEAR(v.lat, HOME_LAT, 1e-9);
    LS_NEAR(v.lon, HOME_LON, 1e-9);
    LS_EQ_INT(v.zoom, 9);
    LS_EQ_INT(v.count, 0);
    LS_CHECK(!v.picked);
}

LS_CASE(fit_all_leaves_out_a_position_past_the_horizon)
{
    ls_follow_pt_t air[N_NEAR + 1];
    for (int i = 0; i < N_NEAR; i++) air[i] = NEAR_HOME[i];
    air[N_NEAR] = (ls_follow_pt_t){ 0xBADBADu, 30.0, -90.0 };   /* the Gulf */
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, air, N_NEAR + 1);
    ls_follow_step(&v, &in);
    LS_EQ_INT(v.count, N_NEAR);
    LS_EQ_INT(v.zoom, ls_follow_fit_zoom(NEAR_HOME, N_NEAR, PW, PH, TILE, FIT, 0, LS_FOLLOW_ZOOM_CAP));
    LS_CHECK(all_inside(&v, NEAR_HOME, N_NEAR, FIT));
}

LS_CASE(fit_all_holds_still_while_the_aircraft_move_about_inside_it)
{
    ls_follow_pt_t air[N_NEAR];
    for (int i = 0; i < N_NEAR; i++) air[i] = NEAR_HOME[i];
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, air, N_NEAR);
    ls_follow_step(&v, &in);
    const ls_follow_view_t first = v;

    /* Ten seconds of reports, each a little off the last. */
    ls_rng_t rng;
    ls_rng_seed(&rng, 7);
    int moved = 0;
    for (int f = 0; f < 250; f++) {
        for (int i = 0; i < N_NEAR; i++) {
            air[i].lat = NEAR_HOME[i].lat + 0.004 * ls_rng_noise(&rng);
            air[i].lon = NEAR_HOME[i].lon + 0.004 * ls_rng_noise(&rng);
        }
        in.now_us += FRAME_US;
        moved += ls_follow_step(&v, &in) ? 1 : 0;
    }
    LS_EQ_INT(moved, 0);
    LS_EQ_INT(v.zoom, first.zoom);
    LS_NEAR(v.lat, first.lat, 1e-12);
    LS_NEAR(v.lon, first.lon, 1e-12);
}

LS_CASE(fit_all_keeps_a_departing_aircraft_on_the_map_with_a_few_moves)
{
    ls_follow_pt_t air[N_NEAR];
    for (int i = 0; i < N_NEAR; i++) air[i] = NEAR_HOME[i];
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, air, N_NEAR);
    ls_follow_step(&v, &in);
    const int start = v.zoom;

    /* The first one heads north at 480 knots for four minutes. */
    int moved = 0, outside = 0;
    for (int f = 0; f < 6000; f++) {
        air[0].lat += 480.0 / 3600.0 / 60.0 * FRAME_US / 1e6;
        in.now_us += FRAME_US;
        moved += ls_follow_step(&v, &in) ? 1 : 0;
        if (!all_inside(&v, air, N_NEAR, 1.0)) outside++;
    }
    LS_EQ_INT(outside, 0);
    LS_CHECK_MSG(v.zoom < start, "zoom %d after it flew thirty miles off, %d before", v.zoom, start);
    LS_CHECK(all_inside(&v, air, N_NEAR, 1.0));
    /* Out a step at a time, each a snap: never a move a frame. */
    LS_CHECK_MSG(moved > 0 && moved <= 12, "%d moves in four minutes", moved);
}

LS_CASE(fit_all_zooms_out_for_a_newcomer_and_back_in_only_with_room_to_spare)
{
    ls_follow_pt_t air[N_NEAR + 1];
    for (int i = 0; i < N_NEAR; i++) air[i] = NEAR_HOME[i];
    air[N_NEAR] = (ls_follow_pt_t){ 0xC0FFEEu, 44.00, -71.65 };   /* 33 miles north */
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, air, N_NEAR);
    ls_follow_step(&v, &in);
    const int near_zoom = v.zoom;
    LS_EQ_INT(run(&v, &in, 3000), 0);

    /* It arrives: out, within the moment it takes to look, in one move. */
    in.n = N_NEAR + 1;
    LS_EQ_INT(run(&v, &in, 400), 1);
    LS_CHECK(v.zoom < near_zoom);
    LS_CHECK(all_inside(&v, air, N_NEAR + 1, FIT));
    LS_EQ_INT(run(&v, &in, 5000), 0);

    /* It goes: back in, also in one move, and then still. */
    in.n = N_NEAR;
    LS_EQ_INT(run(&v, &in, 400), 1);
    LS_EQ_INT(v.zoom, near_zoom);
    LS_CHECK(all_inside(&v, NEAR_HOME, N_NEAR, FIT));
    LS_EQ_INT(run(&v, &in, 5000), 0);
}

LS_CASE(a_render_still_arriving_holds_a_move_nobody_asked_for)
{
    ls_follow_pt_t air[N_NEAR + 1];
    for (int i = 0; i < N_NEAR; i++) air[i] = NEAR_HOME[i];
    air[N_NEAR] = (ls_follow_pt_t){ 0xC0FFEEu, 44.00, -71.65 };
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, air, N_NEAR);
    ls_follow_step(&v, &in);
    const int zoom = v.zoom;
    in.n = N_NEAR + 1;
    in.busy = true;
    LS_EQ_INT(run(&v, &in, 3000), 0);
    LS_EQ_INT(v.zoom, zoom);
    in.busy = false;
    LS_CHECK(run(&v, &in, 400) > 0);
    LS_CHECK(v.zoom < zoom);
}

LS_CASE(a_group_over_the_antimeridian_is_one_group)
{
    const ls_follow_pt_t air[] = { { 1, 10.0, 179.95 }, { 2, 10.05, -179.95 } };
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, air, 2);
    in.have_rx = false;
    ls_follow_step(&v, &in);
    LS_CHECK_MSG(v.zoom >= 9, "two aircraft six miles apart fitted at zoom %d", v.zoom);
    LS_CHECK(fabs(v.lon) > 179.9);
    LS_CHECK(all_inside(&v, air, 2, FIT));
}

/* ------------------------------------------------------------- selected -- */

LS_CASE(selected_centres_the_choice_and_moves_only_when_it_leaves_the_middle)
{
    ls_follow_pt_t air[N_NEAR];
    for (int i = 0; i < N_NEAR; i++) air[i] = NEAR_HOME[i];
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_SELECTED, air, N_NEAR);
    in.zoom = 10;
    in.selected = air[2].icao;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_UINT(v.target, air[2].icao);
    LS_NEAR(v.lat, air[2].lat, 1e-9);
    LS_NEAR(v.lon, air[2].lon, 1e-9);
    LS_EQ_INT(v.zoom, 10);
    LS_CHECK(!v.picked);

    /* Moving about the middle third: the view stays. */
    air[2].lat += 0.03;
    LS_EQ_INT(run(&v, &in, 4000), 0);
    LS_CHECK(!inside(&v, air[2].lat, air[2].lon, 0.02));

    /* Past it: back in the middle, once. */
    air[2].lat += 0.05;
    LS_EQ_INT(run(&v, &in, 4000), 1);
    LS_NEAR(v.lat, air[2].lat, 1e-9);
    LS_NEAR(v.lon, air[2].lon, 1e-9);

    /* The operator's zoom is taken as it is, the centre left alone. */
    const double lat = v.lat;
    in.zoom = 12;
    in.now_us += FRAME_US;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_INT(v.zoom, 12);
    LS_NEAR(v.lat, lat, 1e-12);
    LS_EQ_INT(ls_follow_nudge(&v, 1), 0);
}

LS_CASE(selected_goes_to_a_new_choice_at_once_even_over_a_render)
{
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_SELECTED, NEAR_HOME, N_NEAR);
    in.selected = NEAR_HOME[0].icao;
    ls_follow_step(&v, &in);
    in.busy = true;
    in.selected = NEAR_HOME[4].icao;
    in.now_us += FRAME_US;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_UINT(v.target, NEAR_HOME[4].icao);
    LS_NEAR(v.lat, NEAR_HOME[4].lat, 1e-9);
    LS_NEAR(v.lon, NEAR_HOME[4].lon, 1e-9);

    /* One with no position rests on the receiver, also at once. */
    in.selected = 0xA00777u;
    in.now_us += FRAME_US;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_UINT(v.target, 0);
    LS_EQ_INT(v.count, 0);
    LS_NEAR(v.lat, HOME_LAT, 1e-9);
    LS_NEAR(v.lon, HOME_LON, 1e-9);

    /* And nothing selected is the same. */
    in.busy = false;
    in.selected = NEAR_HOME[1].icao;
    in.now_us += FRAME_US;
    ls_follow_step(&v, &in);
    in.selected = 0;
    in.now_us += FRAME_US;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_NEAR(v.lat, HOME_LAT, 1e-9);
}

LS_CASE(selected_picked_on_the_map_stays_put_for_the_second_tap)
{
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_SELECTED, NEAR_HOME, N_NEAR);
    in.zoom = 11;
    in.selected = NEAR_HOME[3].icao;
    ls_follow_step(&v, &in);
    const double lat = v.lat, lon = v.lon;

    /* A tap on another, out past the middle third: no jump under the
       finger... */
    LS_CHECK(!inside(&v, NEAR_HOME[4].lat, NEAR_HOME[4].lon, 1.0 / 3.0));
    ls_follow_hold(&v, NEAR_HOME[4].icao, in.now_us);
    in.selected = NEAR_HOME[4].icao;
    LS_EQ_INT(run(&v, &in, 1500), 0);
    LS_NEAR(v.lat, lat, 1e-12);
    LS_NEAR(v.lon, lon, 1e-12);
    LS_EQ_UINT(v.target, NEAR_HOME[4].icao);

    /* ...then it is followed like any selection: into the middle, once. */
    LS_EQ_INT(run(&v, &in, 1500), 1);
    LS_NEAR(v.lat, NEAR_HOME[4].lat, 1e-9);
    LS_NEAR(v.lon, NEAR_HOME[4].lon, 1e-9);
}

/* -------------------------------------------------------------- nearest -- */

LS_CASE(nearest_frames_the_receiver_and_the_closest_and_does_not_flap)
{
    /* One five miles north, one eight south. */
    ls_follow_pt_t air[] = {
        { 0x0000A1u, HOME_LAT + 5.0 / 60.0, HOME_LON },
        { 0x0000B2u, HOME_LAT - 8.0 / 60.0, HOME_LON },
    };
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_NEAREST, air, 2);
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_UINT(v.target, air[0].icao);
    LS_CHECK(v.picked);
    LS_CHECK(inside(&v, HOME_LAT, HOME_LON, FIT));
    LS_CHECK(inside(&v, air[0].lat, air[0].lon, FIT));

    /* The southern one comes in to 4.8 miles: about level, so the view keeps
       the one it has. */
    air[1].lat = HOME_LAT - 4.8 / 60.0;
    LS_EQ_INT(run(&v, &in, 3000), 0);
    LS_EQ_UINT(v.target, air[0].icao);

    /* At 4.4 it is clearly the nearer, and the view moves to it at once. */
    air[1].lat = HOME_LAT - 4.4 / 60.0;
    in.now_us += FRAME_US;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_UINT(v.target, air[1].icao);
    LS_CHECK(inside(&v, HOME_LAT, HOME_LON, FIT));
    LS_CHECK(inside(&v, air[1].lat, air[1].lon, FIT));

    /* None left: the receiver. */
    in.n = 0;
    run(&v, &in, 3000);
    LS_EQ_UINT(v.target, 0);
    LS_NEAR(v.lat, HOME_LAT, 1e-9);
}

/* ----------------------------------------------------- zoom, the limits -- */

LS_CASE(zoom_steps_on_a_picked_zoom_stop_at_their_limits_and_never_wrap)
{
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, SPREAD, N_SPREAD);
    ls_follow_step(&v, &in);
    const int fit = v.zoom;

    /* In: one step a press up to four, then nothing - never round to the
       far end. */
    int last = v.zoom;
    for (int i = 0; i < 10; i++) {
        const int moved = ls_follow_nudge(&v, 1);
        in.now_us += FRAME_US;
        ls_follow_step(&v, &in);
        LS_CHECK_MSG(v.zoom >= last, "ZOOM+ went from %d to %d", last, v.zoom);
        LS_EQ_INT(moved, v.zoom - last);
        last = v.zoom;
    }
    LS_EQ_INT(v.zoom, fit + LS_FOLLOW_BIAS_MAX);
    LS_EQ_INT(v.bias, LS_FOLLOW_BIAS_MAX);
    /* And it stays there while it follows: the fit does not take it back. */
    LS_EQ_INT(run(&v, &in, 5000), 0);
    LS_EQ_INT(v.zoom, fit + LS_FOLLOW_BIAS_MAX);

    /* Out, the same, to four the other side. */
    last = v.zoom;
    for (int i = 0; i < 20; i++) {
        ls_follow_nudge(&v, -1);
        in.now_us += FRAME_US;
        ls_follow_step(&v, &in);
        LS_CHECK_MSG(v.zoom <= last, "ZOOM- went from %d to %d", last, v.zoom);
        last = v.zoom;
    }
    LS_EQ_INT(v.zoom, fit - LS_FOLLOW_BIAS_MAX);

    /* Held to what the map can show as well. */
    ls_follow_clear(&v);
    in.zoom_hi = fit + 1;
    ls_follow_step(&v, &in);
    LS_EQ_INT(ls_follow_nudge(&v, 1), 1);
    LS_EQ_INT(ls_follow_nudge(&v, 1), 0);
    LS_EQ_INT(v.zoom, fit + 1);

    /* A new mode starts from its own fit. */
    ls_follow_clear(&v);
    LS_EQ_INT(v.bias, 0);
}

LS_CASE(another_frame_is_placed_afresh)
{
    ls_follow_view_t v;
    ls_follow_clear(&v);
    ls_follow_in_t in = input(LS_FOLLOW_ALL, SPREAD, N_SPREAD);
    ls_follow_step(&v, &in);
    const int portrait = v.zoom;
    /* MAP ONLY: twice the room each way is a step closer, straight away. */
    in.pw = PW * 2;
    in.ph = PH * 2;
    in.busy = true;
    in.now_us += FRAME_US;
    LS_CHECK(ls_follow_step(&v, &in));
    LS_EQ_INT(v.zoom, portrait + 1);
}
