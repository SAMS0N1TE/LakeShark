/* FOX HUNT: bearings to one transmitter from several places, and where they
   cross.

   COMPASS FIND (and the FOX HUNT screen) turn a slow circle into a bearing
   with ls_df / ls_df_fit. Each bearing kept is filed here with the place it
   was taken from. The crossing is a weighted least squares fit: every
   bearing is a line, its weight how well that bearing is known at the
   distance the transmitter turns out to be (an angular error grows with
   range), refitted until it settles. What the fit does not know comes out
   as an error ellipse, from the covariance of the solution scaled by how
   well the lines actually agree. A line the fit puts the transmitter
   behind is left out: a bearing points one way, not both.

   The hunt also lives on the card as plain CSV, one bearing a line, so a
   hunt survives a restart and opens in a spreadsheet:
     utc_s,lat,lon,bearing_deg,spread_deg,level,source
   Lines starting with '#' are comments.

   Pure: no radio, sensor or storage is touched here. */

#ifndef LS_FOX_H
#define LS_FOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_df.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_FOX_MAX 24

typedef struct {
    bool valid;
    double lat, lon;
    float major_m, minor_m;      /* 1-sigma semi-axes */
    float major_deg;             /* true bearing of the major axis, 0..180 */
    float rms_m;                 /* how far the used lines miss the fix */
    float range_m;               /* from the newest bearing's place */
    float bearing;               /* from the newest bearing's place, true */
    int used, behind;            /* lines in the fit; lines left out */
} ls_fox_fix_t;

/* At least two lines 15 degrees or more apart, in front of their places.
   `spread` of each bearing is read as two sigma (as ls_df_fit reports it),
   never tighter than 1 degree. */
bool ls_fox_fix(const ls_df_bearing_t *b, int n, ls_fox_fix_t *out);

/* Where a line from (lat, lon) along `bearing` ends `metres` out. */
void ls_fox_project(double lat, double lon, float bearing, double metres, double *lat2, double *lon2);

/* One CSV line, with its newline; and back. `utc_s` 0 when the clock was
   not set. Parse answers false for a comment, the header or a bad line. */
int  ls_fox_csv_header(char *out, size_t cap);
int  ls_fox_csv_line(const ls_df_bearing_t *b, uint32_t utc_s, char *out, size_t cap);
bool ls_fox_csv_parse(const char *line, ls_df_bearing_t *b, uint32_t *utc_s);

/* The hunt in RAM: the newest LS_FOX_MAX bearings, oldest first. */
typedef struct {
    int n;
    ls_df_bearing_t b[LS_FOX_MAX];
    uint32_t utc[LS_FOX_MAX];
    uint32_t serial;             /* bumps on every change */
} ls_fox_hunt_t;
void ls_fox_hunt_clear(ls_fox_hunt_t *h);
void ls_fox_hunt_add(ls_fox_hunt_t *h, const ls_df_bearing_t *b, uint32_t utc_s);
bool ls_fox_hunt_drop_last(ls_fox_hunt_t *h);

#ifdef __cplusplus
}
#endif
#endif
