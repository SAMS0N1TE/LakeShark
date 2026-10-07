/* Bounded route geometry and streaming SD readers; no allocation or hardware. */
#ifndef LS_ROUTE_H
#define LS_ROUTE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_ROUTE_CAP 1024
#define LS_ROUTE_FILE_MAX (8u * 1024u * 1024u)
#define LS_ROUTE_INPUT_MAX 200000u
typedef struct {
    double lat, lon;
    double along_m;
    bool start; /* A segment starts here; never draw or measure across a GPX gap. */
} ls_route_point_t;
typedef struct {
    ls_route_point_t *point;
    size_t cap, n, next;
    double length_m, progress_m, walked_m, cross_m, remaining_m, bearing;
    double off_m, arrival_m;
    unsigned outside, inside, near;
    bool valid, off_route, arrived, located, reversed;
} ls_route_t;
typedef enum { LS_ROUTE_NONE, LS_ROUTE_OFF, LS_ROUTE_REJOIN, LS_ROUTE_ARRIVE } ls_route_event_t;
void ls_route_init(ls_route_t *r, ls_route_point_t *points, size_t cap);
void ls_route_clear(ls_route_t *r);
/* On failure the route is empty. Both readers retain endpoints and evenly sample
   long input in two bounded passes; FILE must be seekable. */
bool ls_route_gpx(ls_route_t *r, FILE *f);
bool ls_route_track(ls_route_t *r, FILE *f);
void ls_route_reverse(ls_route_t *r);
ls_route_event_t ls_route_update(ls_route_t *r, bool fresh, double lat, double lon);
#ifdef __cplusplus
}
#endif
#endif
