#include "ls_sun.h"

#include <math.h>

#define RAD (M_PI / 180.0)

void ls_sun_position(time_t utc, double lat, double lon, ls_sun_t *out)
{
    if (!out) return;
    const double n = (double)utc / 86400.0 + 2440587.5 - 2451545.0;
    const double L = fmod(280.460 + 0.9856474 * n, 360.0);
    const double g = fmod(357.528 + 0.9856003 * n, 360.0) * RAD;
    const double lambda = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * RAD;
    const double eps = (23.439 - 0.0000004 * n) * RAD;
    const double ra = atan2(cos(eps) * sin(lambda), cos(lambda));
    const double dec = asin(sin(eps) * sin(lambda));
    const double gmst = fmod(18.697374558 + 24.06570982441908 * n, 24.0);
    const double ha = (gmst * 15.0 + lon) * RAD - ra;
    const double phi = lat * RAD;
    double el = asin(sin(phi) * sin(dec) + cos(phi) * cos(dec) * cos(ha));
    double az = atan2(-sin(ha), tan(dec) * cos(phi) - sin(phi) * cos(ha));
    el /= RAD; az /= RAD;
    if (az < 0) az += 360.0;
    /* Bennett's refraction, which lifts a low sun by up to half a degree. */
    if (el > -1.0) el += 1.02 / tan((el + 10.3 / (el + 5.11)) * RAD) / 60.0;
    out->azimuth = az;
    out->elevation = el;
}
