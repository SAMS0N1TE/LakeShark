#include "scan_geo.h"
#include <math.h>

bool scan_geo_ready(const scan_geo_t *s, int64_t now)
{
    return s && s->have_fix && now >= s->fix_us && now - s->fix_us <= 30000000;
}
bool scan_geo_admits(const scan_geo_t *s, int i, int64_t now)
{
    return i >= 0 && i < SCAN_MAX_CHANNELS && scan_geo_ready(s, now) &&
           ((s->eligible >> i) & 1);
}
void scan_geo_update(scan_geo_t *s, const scan_channel_t *c, int count,
                     bool valid, double lat, double lon, int64_t fix, int64_t now)
{
    if (!s || !c || count < 0 || count > SCAN_MAX_CHANNELS) return;
    if (!valid || !isfinite(lat) || !isfinite(lon) || fabs(lat) > 90 || fabs(lon) > 180 ||
        fix <= 0 || fix > now || now - fix > 5000000) return;
    const double rad = 0.017453292519943295;
    uint64_t mask = 0;
    bool fresh = scan_geo_ready(s, now);
    for (int i = 0; i < count; ++i) {
        if (!c[i].radius_m) continue;
        double a = lat * rad, b = c[i].lat_e7 * 1e-7 * rad;
        double d = (lon - c[i].lon_e7 * 1e-7) * rad;
        double h = sin((a-b)/2) * sin((a-b)/2) + cos(a)*cos(b)*sin(d/2)*sin(d/2);
        h = fmax(0, fmin(1, h));
        double distance = 12742000.0 * asin(sqrt(h));
        double margin = fresh && ((s->eligible >> i) & 1) ? fmax(1000, c[i].radius_m * .1) : 0;
        if (distance <= c[i].radius_m + margin) mask |= UINT64_C(1) << i;
    }
    s->eligible = mask;
    s->fix_us = fix;
    s->have_fix = true;
}
