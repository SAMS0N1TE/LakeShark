/* FOLLOW on the ADS-B mini map: which part of the map it shows.

   Pure: no map, no clock and no aircraft table. The screen hands in the
   frame the preview will draw, the receiver, the aircraft with a position and
   the time, and gets back a centre and a zoom. Kept apart so the rules that
   decide when the view moves can be tested without a map under them.

   The view only moves when what it follows no longer sits well in it: a
   target leaving the middle, an aircraft past the edge, a zoom that could be
   a step closer. When it does move it snaps there, as the full map's FOLLOW
   does, because every move restarts a render that may not have finished. */

#ifndef LS_FOLLOW_H
#define LS_FOLLOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_FOLLOW_OFF = 0,     /* the receiver at the centre, zoom by hand       */
    LS_FOLLOW_ALL,         /* every aircraft with a position, zoom picked    */
    LS_FOLLOW_SELECTED,    /* the selected aircraft at the centre, by hand   */
    LS_FOLLOW_NEAREST,     /* the receiver and its nearest aircraft, picked  */
    LS_FOLLOW_MODES
} ls_follow_mode_t;

/* What a board that has never been told shows. */
#define LS_FOLLOW_DEFAULT LS_FOLLOW_ALL

/* The closest a picked zoom goes: one aircraft alone, or one passing over the
   receiver, is about fifteen miles across rather than a street map. */
#define LS_FOLLOW_ZOOM_CAP 11

/* How far ZOOM+ and ZOOM- move a picked zoom from the one that fits. */
#define LS_FOLLOW_BIAS_MAX 4

/* The next mode for the FOLLOW key, after NEAREST back to OFF. */
ls_follow_mode_t ls_follow_next(ls_follow_mode_t m);
/* "OFF", "FIT ALL", "SELECTED", "NEAREST". */
const char *ls_follow_name(ls_follow_mode_t m);
/* FIT ALL and NEAREST choose the zoom; OFF and SELECTED leave it alone. */
bool ls_follow_picks_zoom(ls_follow_mode_t m);

typedef struct {
    uint32_t icao;
    double lat, lon;
} ls_follow_pt_t;

typedef struct {
    ls_follow_mode_t mode;
    /* The frame ls_map_preview draws, in map pixels, and a tile's size in
       them: ls_map_preview_frame. */
    int pw, ph, tile_px;
    /* The zooms the map can show, and the one it is at: OFF and SELECTED
       keep that one. */
    int zoom_lo, zoom_hi, zoom;
    /* A render is still arriving. Moving now would throw it away, so only a
       move somebody asked for is made. */
    bool busy;
    bool have_rx;
    double rx_lat, rx_lon;
    /* The aircraft with a position, where they are drawn. */
    const ls_follow_pt_t *air;
    int n;
    uint32_t selected;     /* 0 for none */
    int64_t now_us;
} ls_follow_in_t;

typedef struct {
    bool valid;            /* a view has been placed                          */
    bool force;            /* place the next one at once                      */
    bool picked;           /* the zoom on show was picked: ZOOM+ nudges it    */
    double lat, lon;       /* the centre                                      */
    int zoom;
    int fit;               /* the zoom that fits, before the bias             */
    int bias;              /* ZOOM+ and ZOOM- on a picked zoom                */
    uint32_t target;       /* SELECTED's or NEAREST's aircraft, 0 for none    */
    uint32_t seen;         /* the selection last step, to see it change       */
    int count;             /* aircraft the mode put in view                   */
    int pw, ph;            /* the frame it was placed in                      */
    int zoom_lo, zoom_hi;
    int64_t moved_us;      /* when it last moved                              */
    int64_t looked_us;     /* when it last asked whether to move              */
} ls_follow_view_t;

/* The modes' names, for an OPTIONS row. */
extern const char *const ls_follow_names[LS_FOLLOW_MODES];

/* Place the view afresh on the next step: a new mode, a new screen. The bias
   is kept; ls_follow_clear forgets that too. */
void ls_follow_reset(ls_follow_view_t *v);
void ls_follow_clear(ls_follow_view_t *v);

/* SELECTED: the operator picked `icao` on the map itself, where it already
   is. The view stays put for a moment instead of snapping to it, so the
   second tap that opens it lands where it was; then it is followed as any
   selection is. */
void ls_follow_hold(ls_follow_view_t *v, uint32_t icao, int64_t now_us);

/* ZOOM+ or ZOOM- while the zoom is picked: moves it by dz steps, at most
   LS_FOLLOW_BIAS_MAX from the one that fits and never past the map's zooms.
   The offset is kept while the picked zoom follows the aircraft. Answers how
   far the zoom moved, 0 at a limit or when the zoom is not picked. */
int ls_follow_nudge(ls_follow_view_t *v, int dz);

/* The closest zoom, from lo to hi, at which every point fits in `fill` of
   the frame each way. One point, or none, is hi. */
int ls_follow_fit_zoom(const ls_follow_pt_t *p, int n, int pw, int ph,
                       int tile_px, double fill, int lo, int hi);

/* Where a coordinate lands relative to the view's centre, in map pixels at
   the view's zoom: right and down are positive. */
void ls_follow_offset(const ls_follow_view_t *v, int tile_px,
                      double lat, double lon, double *dx, double *dy);

/* One frame: move the view if the mode says it must. True when the centre
   or the zoom changed. */
bool ls_follow_step(ls_follow_view_t *v, const ls_follow_in_t *in);

#ifdef __cplusplus
}
#endif

#endif /* LS_FOLLOW_H */
