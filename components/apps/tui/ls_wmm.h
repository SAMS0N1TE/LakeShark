/* The World Magnetic Model, WMM2025: what the Earth's field should be at a
   place and date. Declination turns a magnetic heading into a true one; the
   expected strength and dip are what a compass reading is checked against
   to say whether something nearby is bending it. */

#ifndef LS_WMM_H
#define LS_WMM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double declination;   /* degrees, east positive */
    double inclination;   /* degrees, down positive */
    double total_nt;      /* F */
    double horizontal_nt; /* H */
    double north_nt, east_nt, down_nt;
} ls_wmm_field_t;

/* Geodetic latitude/longitude in degrees, height above the ellipsoid in
   metres, decimal year. False outside the model's 2025.0-2030.0 span. */
bool ls_wmm_field(double lat, double lon, double height_m, double year, ls_wmm_field_t *out);
/* Decimal year from a calendar date. */
double ls_wmm_year(int year, int month, int day);

#ifdef __cplusplus
}
#endif
#endif
