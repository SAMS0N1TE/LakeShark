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
#endif
