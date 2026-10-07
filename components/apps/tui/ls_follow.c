/* See ls_follow.h. The projection is the map's own Web Mercator, so a point
   this file says is in the frame is one ls_map_preview draws in it. */
#include "ls_follow.h"

#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* A fit gives the aircraft this share of the frame's width and height. The
   rest is margin for their symbols and the callsigns beside them. */
#define FIT_FILL    0.70
/* A picked zoom steps closer only when everything would fill no more than
   this at the closer zoom. The gap between the two is what stops it stepping
   in and straight back out. */
#define CLOSER_FILL 0.50
/* Further from the centre than this share of the half-frame is the edge,
   where a callsign is cut off and the aircraft is about to leave. */
#define EDGE_FILL   0.90
/* After a move the view stays put this long, and between moves it looks
   again this often: the projection is double precision, which this chip does
   in software. */
#define HOLD_US     2000000LL
#define LOOK_US     200000LL
/* Further than this from the receiver is a bad position, not an aircraft:
   1090 MHz does not carry past the horizon, about 250 miles from a jet at
   altitude, and one bad decode would zoom FIT ALL out to a continent. */
#define RANGE_NM    400.0
/* NEAREST moves to another aircraft only when it is within this share of the
   followed one's range. Two at about the same range would otherwise trade
   places, and the view with them. */
#define NEARER      0.92
/* At most this many aircraft are framed: the table holds sixteen, and the
   arrays below are on the UI task's stack. */
#define MAX_POINTS  16

const char *const ls_follow_names[LS_FOLLOW_MODES] = {
    "OFF", "FIT ALL", "SELECTED", "NEAREST",
};

ls_follow_mode_t ls_follow_next(ls_follow_mode_t m)
{
    const int i = (int)m + 1;
    return i > 0 && i < LS_FOLLOW_MODES ? (ls_follow_mode_t)i : LS_FOLLOW_OFF;
}

const char *ls_follow_name(ls_follow_mode_t m)
{
    return (int)m >= 0 && m < LS_FOLLOW_MODES ? ls_follow_names[m] : ls_follow_names[0];
}

bool ls_follow_picks_zoom(ls_follow_mode_t m)
{
    return m == LS_FOLLOW_ALL || m == LS_FOLLOW_NEAREST;
}

/* ---------------------------------------------------------- projection -- */

/* Across and down the world, 0 to 1. */
typedef struct { double x, y; } merc_t;

static double merc_x(double lon) { return (lon + 180.0) / 360.0; }

static double merc_y(double lat)
{
    if (lat >  85.05) lat =  85.05;
    if (lat < -85.05) lat = -85.05;
    const double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0;
}

static merc_t merc(double lat, double lon) { return (merc_t){ merc_x(lon), merc_y(lat) }; }

/* The short way round, across the antimeridian. */
static double wrap_half(double d)
{
    while (d >= 0.5) d -= 1.0;
    while (d < -0.5) d += 1.0;
    return d;
}

static double lon_at(double x)
{
    x -= floor(x);
    return x * 360.0 - 180.0;
}

static double lat_at(double y)
{
    if (y < 0) y = 0;
    if (y > 1) y = 1;
    return atan(sinh(M_PI * (1.0 - 2.0 * y))) * 180.0 / M_PI;
}

/* The world's width in map pixels at a zoom. */
static double world(int tile_px, int zoom)
{
    return (double)(tile_px > 0 ? tile_px : 256) * ldexp(1.0, zoom);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* What the points cover: x relative to the first one, so a group over the
   antimeridian is one group and not the whole world. */
typedef struct { double ref, x0, x1, y0, y1; } box_t;

static void box_of(const merc_t *m, int n, box_t *b)
{
    *b = (box_t){ 0 };
    if (n <= 0) return;
    b->ref = m[0].x;
    b->x0 = b->x1 = 0;
    b->y0 = b->y1 = m[0].y;
    for (int i = 1; i < n; i++) {
        const double x = wrap_half(m[i].x - b->ref);
        if (x < b->x0) b->x0 = x;
        if (x > b->x1) b->x1 = x;
        if (m[i].y < b->y0) b->y0 = m[i].y;
        if (m[i].y > b->y1) b->y1 = m[i].y;
    }
}

static int box_fit(const box_t *b, int pw, int ph, int tile_px, double fill, int lo, int hi)
{
    if (hi < lo) hi = lo;
    const double w = b->x1 - b->x0, h = b->y1 - b->y0;
    for (int z = hi; z > lo; z--) {
        const double s = world(tile_px, z);
        if (w * s <= fill * pw && h * s <= fill * ph) return z;
    }
    return lo;
}

int ls_follow_fit_zoom(const ls_follow_pt_t *p, int n, int pw, int ph,
                       int tile_px, double fill, int lo, int hi)
{
    if (hi < lo) hi = lo;
    if (!p || n <= 0) return hi;
    if (pw <= 0 || ph <= 0) return lo;
    if (n > MAX_POINTS) n = MAX_POINTS;
    merc_t m[MAX_POINTS] = { { 0, 0 } };
    for (int i = 0; i < n; i++) m[i] = merc(p[i].lat, p[i].lon);
    box_t b;
    box_of(m, n, &b);
    return box_fit(&b, pw, ph, tile_px, fill, lo, hi);
}

static void offset_of(merc_t centre, double scale, merc_t m, double *dx, double *dy)
{
    *dx = wrap_half(m.x - centre.x) * scale;
    *dy = (m.y - centre.y) * scale;
}

void ls_follow_offset(const ls_follow_view_t *v, int tile_px,
                      double lat, double lon, double *dx, double *dy)
{
    double x = 0, y = 0;
    if (v) offset_of(merc(v->lat, v->lon), world(tile_px, v->zoom), merc(lat, lon), &x, &y);
    if (dx) *dx = x;
    if (dy) *dy = y;
}

/* Nautical miles on a flat earth, `k` the cosine of the first latitude. It
   only ranks and screens aircraft within a few hundred miles. */
static double flat_nm(double k, double lat0, double lon0, double lat1, double lon1)
{
    double dx = lon1 - lon0;
    while (dx >= 180.0) dx -= 360.0;
    while (dx < -180.0) dx += 360.0;
    dx *= k;
    const double dy = lat1 - lat0;
    return 60.0 * sqrt(dx * dx + dy * dy);
}

/* ---------------------------------------------------------------- view -- */

void ls_follow_reset(ls_follow_view_t *v)
{
    if (!v) return;
    v->valid = false;
    v->force = true;
    v->picked = false;
    v->target = 0;
}

void ls_follow_clear(ls_follow_view_t *v)
{
    if (!v) return;
    *v = (ls_follow_view_t){ .force = true };
}

void ls_follow_hold(ls_follow_view_t *v, uint32_t icao, int64_t now_us)
{
    if (!v || !v->valid) return;
    v->seen = icao;
    v->target = icao;
    v->moved_us = now_us;
}

int ls_follow_nudge(ls_follow_view_t *v, int dz)
{
    if (!v || !dz || !v->valid || !v->picked) return 0;
    /* Measured from the zoom on show, which a view resting between two
       fits may hold a step back from the one that fits. */
    int to = clampi(v->zoom + dz, v->zoom_lo, v->zoom_hi);
    const int bias = clampi(to - v->fit, -LS_FOLLOW_BIAS_MAX, LS_FOLLOW_BIAS_MAX);
    to = clampi(v->fit + bias, v->zoom_lo, v->zoom_hi);
    const int moved = to - v->zoom;
    if (!moved) return 0;
    v->bias = bias;
    v->zoom = to;
    v->force = true;
    return moved;
}

static bool place(ls_follow_view_t *v, double lat, double lon, int zoom, int64_t now)
{
    const bool moved = !v->valid || v->lat != lat || v->lon != lon || v->zoom != zoom;
    v->valid = true;
    v->force = false;
    v->lat = lat;
    v->lon = lon;
    v->zoom = zoom;
    if (moved) v->moved_us = now;
    return moved;
}

/* Whether a move nobody asked for may be made now: not over a render still
   arriving, not straight after the last move, and not every frame. */
static bool may_look(ls_follow_view_t *v, const ls_follow_in_t *in)
{
    const int64_t now = in->now_us;
    if (in->busy) return false;
    if (now >= v->moved_us && now - v->moved_us < HOLD_US) return false;
    if (now >= v->looked_us && now - v->looked_us < LOOK_US) return false;
    v->looked_us = now;
    return true;
}

/* Nothing to follow, or FOLLOW OFF: the receiver at the centre, at the zoom
   the map has, which is the operator's. */
static bool rest(ls_follow_view_t *v, const ls_follow_in_t *in, bool at_once)
{
    v->count = 0;
    v->picked = false;
    if (!in->have_rx || (!at_once && !may_look(v, in))) {
        /* The centre waits; the zoom is the operator's at once. */
        const bool zoomed = v->valid && v->zoom != in->zoom;
        if (v->valid) v->zoom = in->zoom;
        return zoomed;
    }
    return place(v, in->rx_lat, in->rx_lon, in->zoom, in->now_us);
}

/* Every point in view at the closest zoom that holds them all. The caller
   has already decided this frame may move. */
static bool frame(ls_follow_view_t *v, const ls_follow_in_t *in,
                  const merc_t *m, int n, bool at_once)
{
    const int lo = in->zoom_lo, hi = in->zoom_hi < lo ? lo : in->zoom_hi;
    const int top = clampi(LS_FOLLOW_ZOOM_CAP, lo, hi);
    box_t b;
    box_of(m, n, &b);
    v->fit = box_fit(&b, in->pw, in->ph, in->tile_px, FIT_FILL, lo, top);
    const int want = clampi(v->fit + v->bias, lo, hi);
    const merc_t mid = { b.ref + (b.x0 + b.x1) / 2.0, (b.y0 + b.y1) / 2.0 };
    const double lat = lat_at(mid.y), lon = lon_at(mid.x);

    if (at_once || !v->valid) return place(v, lat, lon, want, in->now_us);
    /* One no longer fits: out at once. */
    if (want < v->zoom) return place(v, lat, lon, want, in->now_us);
    /* Closer only with room to spare. */
    const int closer = clampi(box_fit(&b, in->pw, in->ph, in->tile_px, CLOSER_FILL, lo, top) + v->bias, lo, hi);
    if (closer > v->zoom) return place(v, lat, lon, want, in->now_us);

    /* The same zoom: back to the middle when they have drifted a sixth of the
       frame off it, or when one is at the edge and the move is worth a
       render. */
    const merc_t centre = merc(v->lat, v->lon);
    const double scale = world(in->tile_px, v->zoom);
    double dx, dy;
    offset_of(centre, scale, mid, &dx, &dy);
    const bool drift = fabs(dx) > in->pw / 6.0 || fabs(dy) > in->ph / 6.0;
    const bool worth = fabs(dx) > in->pw / 12.0 || fabs(dy) > in->ph / 12.0;
    bool edge = false;
    for (int i = 0; i < n && !edge; i++) {
        double ex, ey;
        offset_of(centre, scale, m[i], &ex, &ey);
        edge = fabs(ex) > EDGE_FILL * in->pw / 2.0 || fabs(ey) > EDGE_FILL * in->ph / 2.0;
    }
    if (drift || (edge && worth)) return place(v, lat, lon, v->zoom, in->now_us);
    return false;
}

static bool in_range(const ls_follow_in_t *in, double k, const ls_follow_pt_t *p)
{
    return !in->have_rx || flat_nm(k, in->rx_lat, in->rx_lon, p->lat, p->lon) <= RANGE_NM;
}

static bool fit_all(ls_follow_view_t *v, const ls_follow_in_t *in, bool at_once)
{
    const double k = in->have_rx ? cos(in->rx_lat * M_PI / 180.0) : 1.0;
    int n = 0;
    for (int i = 0; in->air && i < in->n && n < MAX_POINTS; i++)
        if (in_range(in, k, &in->air[i])) n++;
    v->target = 0;
    if (!n) return rest(v, in, at_once);
    /* The first to appear, after resting on the receiver: go to it. */
    if (!v->picked) at_once = true;
    v->count = n;
    v->picked = true;
    if (!at_once && !may_look(v, in)) return false;
    merc_t m[MAX_POINTS] = { { 0, 0 } };
    n = 0;
    for (int i = 0; in->air && i < in->n && n < MAX_POINTS; i++)
        if (in_range(in, k, &in->air[i])) m[n++] = merc(in->air[i].lat, in->air[i].lon);
    return frame(v, in, m, n, at_once);
}

static bool nearest(ls_follow_view_t *v, const ls_follow_in_t *in, bool at_once)
{
    int best = -1, cur = -1;
    double bd = 0, cd = 0;
    if (in->have_rx) {
        const double k = cos(in->rx_lat * M_PI / 180.0);
        for (int i = 0; in->air && i < in->n; i++) {
            const double d = flat_nm(k, in->rx_lat, in->rx_lon, in->air[i].lat, in->air[i].lon);
            if (d > RANGE_NM) continue;
            if (best < 0 || d < bd) { best = i; bd = d; }
            if (in->air[i].icao == v->target) { cur = i; cd = d; }
        }
    }
    if (best < 0) {
        v->target = 0;
        return rest(v, in, at_once);
    }
    /* The one followed stays until another is clearly nearer. */
    const int pick = cur >= 0 && bd > cd * NEARER ? cur : best;
    if (in->air[pick].icao != v->target) {
        v->target = in->air[pick].icao;
        at_once = true;
    }
    v->count = 1;
    v->picked = true;
    if (!at_once && !may_look(v, in)) return false;
    const merc_t two[2] = { merc(in->rx_lat, in->rx_lon),
                            merc(in->air[pick].lat, in->air[pick].lon) };
    return frame(v, in, two, 2, at_once);
}

static bool selected(ls_follow_view_t *v, const ls_follow_in_t *in, bool at_once)
{
    v->picked = false;
    int i = -1;
    for (int k = 0; in->selected && in->air && k < in->n; k++)
        if (in->air[k].icao == in->selected) i = k;
    if (i < 0) {
        v->target = 0;
        return rest(v, in, at_once);
    }
    const ls_follow_pt_t *p = &in->air[i];
    v->count = 1;
    if (p->icao != v->target) {
        v->target = p->icao;
        at_once = true;
    }
    if (at_once || !v->valid) return place(v, p->lat, p->lon, in->zoom, in->now_us);

    /* The zoom is the operator's, taken as it is. */
    const bool zoomed = v->zoom != in->zoom;
    v->zoom = in->zoom;
    if (!may_look(v, in)) return zoomed;
    /* Back to the middle once it leaves the middle third, as the full map's
       FOLLOW does. */
    double dx, dy;
    ls_follow_offset(v, in->tile_px, p->lat, p->lon, &dx, &dy);
    if (fabs(dx) > in->pw / 6.0 || fabs(dy) > in->ph / 6.0)
        return place(v, p->lat, p->lon, v->zoom, in->now_us) || zoomed;
    return zoomed;
}

bool ls_follow_step(ls_follow_view_t *v, const ls_follow_in_t *in)
{
    if (!v || !in) return false;
    bool at_once = v->force || !v->valid;
    /* Another frame is another fit: turned round, or MAP ONLY. */
    if (in->pw != v->pw || in->ph != v->ph) at_once = true;
    /* So is the operator choosing another aircraft to follow. */
    if (in->mode == LS_FOLLOW_SELECTED && in->selected != v->seen) at_once = true;
    v->seen = in->selected;
    v->pw = in->pw;
    v->ph = in->ph;
    v->zoom_lo = in->zoom_lo;
    v->zoom_hi = in->zoom_hi < in->zoom_lo ? in->zoom_lo : in->zoom_hi;
    if (in->pw <= 0 || in->ph <= 0) return false;

    switch (in->mode) {
    case LS_FOLLOW_ALL:      return fit_all(v, in, at_once);
    case LS_FOLLOW_SELECTED: return selected(v, in, at_once);
    case LS_FOLLOW_NEAREST:  return nearest(v, in, at_once);
    default:
        v->target = 0;
        return rest(v, in, true);
    }
}
