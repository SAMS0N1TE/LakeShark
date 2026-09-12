/* See ls_geo.h. */
#include "ls_geo.h"

#include <math.h>

/* WGS84 mean radius. The same figure the ADS-B radar used, kept so the two
   do not disagree in the last digit of a range they both print. */
#define EARTH_RADIUS_M 6371000.0

void ls_geo_bearing_range(double lat0, double lon0, double lat1, double lon1,
                          double *bearing_deg, double *range_m)
{
    const double lat_mid = (lat0 + lat1) * 0.5 * M_PI / 180.0;
    const double x = (lon1 - lon0) * M_PI / 180.0 * cos(lat_mid) * EARTH_RADIUS_M;
    const double y = (lat1 - lat0) * M_PI / 180.0 * EARTH_RADIUS_M;
    if (bearing_deg) {
        double brg = atan2(x, y) * 180.0 / M_PI;
        if (brg < 0.0) brg += 360.0;
        *bearing_deg = brg;
    }
    if (range_m) *range_m = sqrt(x * x + y * y);
}

const char *ls_geo_compass(double bearing_deg)
{
    static const char *const NAME[8] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW"
    };
    double b = fmod(bearing_deg, 360.0);
    if (b < 0.0) b += 360.0;

    int i = (int)((b + 22.5) / 45.0);
    if (i < 0 || i > 7) i = 0;
    return NAME[i];
}
