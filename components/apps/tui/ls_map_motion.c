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

#define GLIDE_US 900000

void ls_map_glide(ls_map_glide_t *g, uint32_t id, int64_t stamp,
                  double lat, double lon, int64_t now, double *out_lat, double *out_lon)
{
    if (!g->valid || g->id != id || stamp < g->stamp || stamp - g->stamp > 60000000) {
        *g = (ls_map_glide_t){ .id = id, .stamp = stamp, .since = now,
                               .shown_lat = lat, .shown_lon = lon, .valid = true };
    } else if (stamp != g->stamp) {
        g->off_lat = g->shown_lat - lat;
        g->off_lon = wrap(g->shown_lon - lon);
        /* A correction bigger than a few miles is a new track, not a
           wobble: jump to it. */
        if (fabs(g->off_lat) > 0.05 || fabs(g->off_lon) > 0.05) g->off_lat = g->off_lon = 0;
        g->stamp = stamp;
        g->since = now;
    }
    double k = 1.0 - (double)(now - g->since) / GLIDE_US;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    k = k * k * (3 - 2 * k);
    *out_lat = lat + g->off_lat * k;
    *out_lon = wrap(lon + g->off_lon * k);
    g->shown_lat = *out_lat;
    g->shown_lon = *out_lon;
}

void ls_map_dead_reckon(double lat, double lon, int ns_kt, int ew_kt, int64_t stamp,
                        int64_t now, int64_t max_us, double *out_lat, double *out_lon)
{
    int64_t age = now - stamp;
    if (age < 0) age = 0;
    if (age > max_us) age = max_us;
    const double hours = (double)age / 3600e6;
    /* One knot for an hour is one nautical mile, a minute of latitude. */
    double la = lat + ns_kt * hours / 60.0;
    if (la > 85) la = 85;
    if (la < -85) la = -85;
    const double c = cos(lat * M_PI / 180.0);
    *out_lat = la;
    *out_lon = wrap(lon + (c > 0.01 ? ew_kt * hours / 60.0 / c : 0.0));
}
