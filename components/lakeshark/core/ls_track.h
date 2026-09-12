/* A GPS track: where the unit went, and when. */

#ifndef LS_TRACK_H
#define LS_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1e7 degrees, which is about 1 cm - the resolution every GPS
   interchange format uses and two orders finer than a consumer receiver
   actually knows. Stored that way because a float degree has 7 significant
   digits total, so at 3 digits of longitude it is down to ~1 m of precision
   BEFORE any arithmetic, and a track is nothing but differences. */
typedef struct {
    uint32_t t;          /* seconds: epoch when LS_TRACK_F_EPOCH, else uptime */
    int32_t  lat_e7;
    int32_t  lon_e7;
    int16_t  alt_m;
    uint8_t  sats;
    uint8_t  flags;
} ls_track_pt_t;

#define LS_TRACK_F_EPOCH  0x01   /* `t` is a real date, not an uptime */

/* Points held on the card. 4096 x 16 bytes is 64 KB. */
#define LS_TRACK_CAPACITY 4096

/* Metres between two points, flat-earth. */

float ls_track_distance_m(int32_t lat_a_e7, int32_t lon_a_e7,
                          int32_t lat_b_e7, int32_t lon_b_e7);

/* Should this fix become a point? */

bool ls_track_should_log(float moved_m, uint32_t since_s,
                         float min_move_m, uint32_t max_gap_s);

/* One GPX trackpoint, as text. */

int ls_track_gpx_point(char *out, size_t cap, const ls_track_pt_t *p);

/* A named waypoint, for something that was at a place. */

int ls_track_gpx_waypoint(char *out, size_t cap, const ls_track_pt_t *p,
                          const char *name, const char *desc);

/* The document, in the order GPX 1.1 requires: header, then any
   waypoints, then the track, then the footer. The schema fixes that order -
   metadata, wpt, rte, trk - so a header that opened <trk> itself would leave
   nowhere legal for a waypoint. Same contract as the rest: bytes written, or
   0 when it would not fit. */
int ls_track_gpx_header(char *out, size_t cap, const char *name);
int ls_track_gpx_trk_open(char *out, size_t cap, const char *name);
int ls_track_gpx_trk_close(char *out, size_t cap);
int ls_track_gpx_footer(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LS_TRACK_H */
