/* The MAP view kept across a restart, and where MAP starts.

   One NVS u64: bit 63 set, zoom in 5 bits, latitude and longitude in
   millionths of a degree (11 cm) offset to be positive. 0 is "none", which
   is what setting a new home writes: the map then opens at that home. */
#ifndef LS_MAP_VIEW_H
#define LS_MAP_VIEW_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

static inline uint64_t ls_map_view_pack(double lat, double lon, int zoom)
{
    if (!isfinite(lat) || !isfinite(lon) || fabs(lat) > 85.0 || fabs(lon) > 180.0 || zoom < 0 || zoom > 22) return 0;
    const uint64_t la = (uint64_t)llround((lat + 90.0) * 1e6), lo = (uint64_t)llround((lon + 180.0) * 1e6);
    return UINT64_C(1) << 63 | (uint64_t)zoom << 58 | la << 29 | lo;
}

static inline bool ls_map_view_unpack(uint64_t v, double *lat, double *lon, int *zoom)
{
    if (!(v >> 63)) return false;
    const double la = (double)((v >> 29) & 0x1FFFFFFFu) / 1e6 - 90.0, lo = (double)(v & 0x1FFFFFFFu) / 1e6 - 180.0;
    const int z = (int)((v >> 58) & 31);
    if (fabs(la) > 85.0 || fabs(lo) > 180.0 || z > 22) return false;
    *lat = la; *lon = lo; *zoom = z;
    return true;
}

/* Where MAP opens when nothing has placed it yet this boot: the view it was
   left at, else the saved home at the zoom it already has, else wherever it
   is. Returns false when there is nothing to apply. */
static inline bool ls_map_view_start(uint64_t saved, bool have_home, double home_lat, double home_lon,
                                     double *lat, double *lon, int *zoom)
{
    if (ls_map_view_unpack(saved, lat, lon, zoom)) return true;
    if (!have_home || !isfinite(home_lat) || !isfinite(home_lon) || fabs(home_lat) > 85.0 || fabs(home_lon) > 180.0) return false;
    *lat = home_lat; *lon = home_lon;
    return true;
}
#endif
