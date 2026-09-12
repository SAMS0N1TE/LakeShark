/* Where something is, relative to where you are. */

#ifndef LS_GEO_H
#define LS_GEO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bearing in degrees true (0 = north, clockwise) and range in metres, from
   (lat0, lon0) to (lat1, lon1). Either output may be NULL. */
void ls_geo_bearing_range(double lat0, double lon0, double lat1, double lon1,
                          double *bearing_deg, double *range_m);

/* The eight-point compass name for a bearing: "N", "NE", ... Always a valid
   string, whatever the bearing, including one outside 0..360. */
const char *ls_geo_compass(double bearing_deg);

#define LS_GEO_M_PER_NM   1852.0
#define LS_GEO_M_PER_MILE 1609.344

#ifdef __cplusplus
}
#endif

#endif /* LS_GEO_H */
