#include "carto/geom.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double carto_lon_to_norm(double lon) {
    return (lon + 180.0) / 360.0;
}

double carto_lat_to_norm(double lat) {
    double r = lat * M_PI / 180.0;
    return (1.0 - asinh(tan(r)) / M_PI) / 2.0;
}

void carto_tile_center(int tx, int ty, int tz, double *lat, double *lon) {
    double n = ldexp(1.0, tz);
    if (lon) *lon = (tx + 0.5) / n * 360.0 - 180.0;
    if (lat) {
        double yn = (ty + 0.5) / n;
        *lat = atan(sinh(M_PI * (1.0 - 2.0 * yn))) * 180.0 / M_PI;
    }
}
