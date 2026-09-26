/* Where the sun is: true azimuth and elevation for a place and UTC time,
   to about half a degree (the Astronomical Almanac's low-precision
   formulae). Enough to check a compass against, or to find north without
   one. */

#ifndef LS_SUN_H
#define LS_SUN_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { double azimuth, elevation; } ls_sun_t;

/* Elevation includes ordinary refraction near the horizon. */
void ls_sun_position(time_t utc, double lat, double lon, ls_sun_t *out);

#ifdef __cplusplus
}
#endif
#endif
