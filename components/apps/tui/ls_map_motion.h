#ifndef LS_MAP_MOTION_H
#define LS_MAP_MOTION_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t id;
    int64_t stamp, since;
    double from_lat, from_lon, lat, lon;
    bool valid;
} ls_map_motion_t;
void ls_map_motion_position(ls_map_motion_t *track, uint32_t id, int64_t stamp,
                            double lat, double lon, int64_t now, double *out_lat, double *out_lon);

/* A position that moves every frame, not only when a report arrives: the
   caller hands in its own estimate (a dead reckoning from the last report)
   and a new report is blended in over GLIDE_US instead of jumping, so a
   symbol slides onto the corrected track. */
typedef struct {
    uint32_t id;
    int64_t stamp, since;
    double off_lat, off_lon, shown_lat, shown_lon;
    bool valid;
} ls_map_glide_t;
void ls_map_glide(ls_map_glide_t *g, uint32_t id, int64_t stamp,
                  double lat, double lon, int64_t now, double *out_lat, double *out_lon);

/* Where something reported at (lat, lon) at `stamp` should be at `now`,
   flying at the given ground speed components in knots. Holds still past
   `max_us`, which is as far as a guess is worth making. */
void ls_map_dead_reckon(double lat, double lon, int ns_kt, int ew_kt, int64_t stamp,
                        int64_t now, int64_t max_us, double *out_lat, double *out_lon);
#endif
