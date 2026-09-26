#include "ls_test.h"
#include "ls_sun.h"
#include <math.h>

/* 2026-03-20 12:00 UTC, the day of the March equinox. */
#define EQUINOX_NOON 1774008000

LS_CASE(at_an_equinox_noon_the_sun_is_south_at_ninety_minus_latitude)
{
    ls_sun_t s;
    /* Solar noon at longitude 0 falls a few minutes after 12:00 UTC
       (the equation of time); look at 12:07. */
    ls_sun_position(EQUINOX_NOON + 7 * 60, 45.0, 0.0, &s);
    LS_CHECK_MSG(fabs(s.azimuth - 180) < 2.5, "azimuth %.2f", s.azimuth);
    LS_CHECK_MSG(fabs(s.elevation - 45) < 1.0, "elevation %.2f", s.elevation);
    ls_sun_position(EQUINOX_NOON + 7 * 60, -30.0, 0.0, &s);
    LS_CHECK_MSG(s.azimuth < 3 || s.azimuth > 357, "southern noon sun is north: %.2f", s.azimuth);
    LS_CHECK_MSG(fabs(s.elevation - 60) < 1.0, "elevation %.2f", s.elevation);
}

LS_CASE(at_an_equinox_sunrise_the_sun_is_due_east)
{
    ls_sun_t s;
    /* Six hours before solar noon at longitude 0. */
    ls_sun_position(EQUINOX_NOON + 7 * 60 - 6 * 3600, 43.2, 0.0, &s);
    LS_CHECK_MSG(fabs(s.azimuth - 90) < 2.5, "azimuth %.2f", s.azimuth);
    LS_CHECK_MSG(fabs(s.elevation) < 1.5, "elevation %.2f", s.elevation);
}

LS_CASE(midsummer_noon_in_new_hampshire)
{
    ls_sun_t s;
    /* 2026-06-21, solar noon at 71.65 W is about 16:45 UTC:
       90 - 43.2 + 23.44 = 70.2 degrees up, due south. */
    ls_sun_position(1782060300, 43.2, -71.65, &s);
    LS_CHECK_MSG(fabs(s.elevation - 70.2) < 1.0, "elevation %.2f", s.elevation);
    LS_CHECK_MSG(fabs(s.azimuth - 180) < 4.0, "azimuth %.2f", s.azimuth);
}
