#include "ls_map_motion.h"
#include <math.h>

static double wrap(double lon)
{
    lon = fmod(lon + 180, 360);
    if (lon < 0) lon += 360;
    return lon - 180;
}
static void position(const ls_map_motion_t *t, int64_t now, double *lat, double *lon)
{
    double progress = (now - t->since) / 750000.0;
    if (progress < 0) progress = 0;
    if (progress > 1) progress = 1;
    progress = progress * progress * (3 - 2 * progress);
    *lat = t->from_lat + (t->lat - t->from_lat) * progress;
    *lon = wrap(t->from_lon + wrap(t->lon - t->from_lon) * progress);
}
void ls_map_motion_position(ls_map_motion_t *t, uint32_t id, int64_t stamp,
                            double lat, double lon, int64_t now, double *out_lat, double *out_lon)
{
    if (!t->valid || t->id != id || stamp < t->stamp || now < t->since || stamp - t->stamp > 10000000) {
        *t = (ls_map_motion_t){.id=id,.stamp=stamp,.since=now,.from_lat=lat,.from_lon=lon,.lat=lat,.lon=lon,.valid=true};
    } else if (stamp != t->stamp) {
        position(t, now, &t->from_lat, &t->from_lon);
        t->lat = lat; t->lon = lon; t->stamp = stamp; t->since = now;
    }
    position(t, now, out_lat, out_lon);
}
