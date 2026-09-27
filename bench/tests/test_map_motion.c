#include "ls_test.h"
#include "ls_map_motion.h"
LS_CASE(aircraft_eases_between_fixes_without_extrapolating)
{
    ls_map_motion_t t={0}; double lat,lon;
    ls_map_motion_position(&t,1,1000000,42,-71,1000000,&lat,&lon);
    ls_map_motion_position(&t,1,2000000,43,-70,2000000,&lat,&lon);
    LS_NEAR(lat,42,1e-8);
    ls_map_motion_position(&t,1,2000000,43,-70,2375000,&lat,&lon);
    LS_NEAR(lat,42.5,1e-8);
    ls_map_motion_position(&t,1,2000000,43,-70,9000000,&lat,&lon);
    LS_NEAR(lat,43,1e-8);
}
LS_CASE(aircraft_crosses_dateline_and_new_slot_does_not_inherit_motion)
{
    ls_map_motion_t t={0}; double lat,lon;
    ls_map_motion_position(&t,1,1000000,42,179.9,1000000,&lat,&lon);
    ls_map_motion_position(&t,1,2000000,42,-179.9,2000000,&lat,&lon);
    ls_map_motion_position(&t,1,2000000,42,-179.9,2375000,&lat,&lon);
    LS_NEAR(fabs(lon),180,1e-8);
    ls_map_motion_position(&t,2,2000000,51,0,2375000,&lat,&lon);
    LS_NEAR(lat,51,1e-8); LS_NEAR(lon,0,1e-8);
}

LS_CASE(glide_slides_onto_a_corrected_track_instead_of_jumping)
{
    ls_map_glide_t g = {0};
    double lat, lon;
    ls_map_glide(&g, 7, 1000000, 43.0, -71.0, 1000000, &lat, &lon);
    LS_NEAR(lat, 43.0, 1e-9);
    /* The estimate moves on its own between reports. */
    ls_map_glide(&g, 7, 1000000, 43.001, -71.0, 1500000, &lat, &lon);
    LS_NEAR(lat, 43.001, 1e-9);
    /* A report lands 0.002 degrees further on: the symbol starts where it
       was shown and reaches the report after the glide. */
    ls_map_glide(&g, 7, 2000000, 43.003, -71.0, 2000000, &lat, &lon);
    LS_NEAR(lat, 43.001, 1e-9);
    ls_map_glide(&g, 7, 2000000, 43.003, -71.0, 2450000, &lat, &lon);
    LS_CHECK(lat > 43.001 && lat < 43.003);
    ls_map_glide(&g, 7, 2000000, 43.003, -71.0, 3000000, &lat, &lon);
    LS_NEAR(lat, 43.003, 1e-9);
}

LS_CASE(glide_jumps_for_a_new_aircraft_or_a_far_correction)
{
    ls_map_glide_t g = {0};
    double lat, lon;
    ls_map_glide(&g, 7, 1000000, 43.0, -71.0, 1000000, &lat, &lon);
    ls_map_glide(&g, 8, 1000000, 50.0, 0.0, 1100000, &lat, &lon);
    LS_NEAR(lat, 50.0, 1e-9);
    ls_map_glide(&g, 8, 2000000, 50.5, 0.0, 2000000, &lat, &lon);
    LS_NEAR(lat, 50.5, 1e-9);
}

LS_CASE(dead_reckoning_flies_the_reported_speed_and_then_holds)
{
    double lat, lon;
    /* 360 kt north for ten seconds is one nautical mile, a minute of arc. */
    ls_map_dead_reckon(43.0, -71.0, 360, 0, 0, 10000000, 20000000, &lat, &lon);
    LS_NEAR(lat, 43.0 + 1.0 / 60.0, 1e-9);
    LS_NEAR(lon, -71.0, 1e-9);
    /* Past the limit it stops guessing. */
    ls_map_dead_reckon(43.0, -71.0, 360, 0, 0, 60000000, 20000000, &lat, &lon);
    LS_NEAR(lat, 43.0 + 2.0 / 60.0, 1e-9);
    /* East at 60 degrees north covers twice the longitude. */
    ls_map_dead_reckon(60.0, 10.0, 0, 360, 0, 10000000, 20000000, &lat, &lon);
    LS_NEAR(lon, 10.0 + 2.0 / 60.0, 1e-6);
}
