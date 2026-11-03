/* The globe: world countries and US states as outlines on an orthographic
   globe, drawn with the map's sextant dot canvas, with a cell fill per
   region that the caller chooses (what is installed, what is selected).

   The outlines come from /sdcard/maps/outlines.lso (LSOUT1, written by
   bench/maps/make_outlines.py from Natural Earth and fetched with the map
   state list), loaded into PSRAM by a one-shot task so the UI never waits on
   the card. Projection is redone only when the view moves; fills are redone
   a few rows per frame, the last view's still drawn meanwhile, so they lag a
   move instead of blanking.
   UI task only, except the loader. */
#ifndef LS_GLOBE_H
#define LS_GLOBE_H
#include <stdbool.h>
#include <stdint.h>
#include "tuilib/tui_core.h"
#ifdef __cplusplus
extern "C" {
#endif

#define LS_GLOBE_PATH "/sdcard/maps/outlines.lso"

enum { LS_GLOBE_NONE = 0, LS_GLOBE_LOADING, LS_GLOBE_READY, LS_GLOBE_FAILED };

/* Start loading `path` (NULL: LS_GLOBE_PATH) if it is not loaded or is
   newer than what is; returns the state. Cheap to call every frame. */
int  ls_globe_load(const char *path);
int  ls_globe_state(void);
/* Outlines are in memory and drawable. Stays true while a newer file loads:
   the old shapes are kept until the new ones are swapped in. */
bool ls_globe_has_shapes(void);

/* Where the globe is looking: latitude, longitude of the centre and a zoom,
   1 the whole globe in the rectangle, larger closer. The drawn view eases
   toward it over a few frames, so a change of view is seen, not jumped. */
void ls_globe_target(float lat, float lon, float zoom);
void ls_globe_view(float *lat, float *lon, float *zoom);

/* The fill for a state, by code ("US-NH"): a TUI colour, or 0 for none.
   `level` 1 asks about US states; countries are outline only. */
typedef uint8_t (*ls_globe_fill_fn)(const char *code, void *ctx);

/* Draw into `area`. `selected` (a code or NULL) is outlined bright. Call
   ls_globe_refill() when the fills' answers change (a download landed). */
void ls_globe_draw(tui_surface *sf, tui_rect area, ls_globe_fill_fn fill, void *ctx,
                   const char *selected);
void ls_globe_refill(void);
/* True while the view is still easing toward its target. */
bool ls_globe_moving(void);

/* What is under a cell of the last draw: 1 and the state's code, 0 and the
   country's code, or -1 (sea, space). lat/lon may be NULL. */
int  ls_globe_hit(int col, int row, char code[8], float *lat, float *lon);
/* A region's name and centre by code; false when unknown. */
bool ls_globe_region(const char *code, char *name, int name_cap, float *lat, float *lon);

/* The outlines flat in Web Mercator over `a`, for MAP where the card has no
   tiles: centred on lat/lon at zoom `z` (256 px tiles), `ppx` x `ppy` world
   pixels a cell. A graticule, countries, states; regions `fill` colours are
   stippled and named. False until outlines are loaded. UI task only. */
bool ls_globe_flat(tui_surface *sf, tui_rect a, double lat, double lon, unsigned z, double ppx, double ppy,
                   ls_globe_fill_fn fill, void *ctx);
/* The same under a map that is moving: the last outlines shifted with a pan,
   none during a zoom, a full draw once the view has been still 250 ms (only
   when full_ok: false keeps it to cheap shifts, as while MAP opens). */
bool ls_globe_flat_lazy(tui_surface *sf, tui_rect a, double lat, double lon, unsigned z, double ppx, double ppy, bool full_ok,
                        ls_globe_fill_fn fill, void *ctx);
/* The filled region (one on the card) whose centre is nearest lat/lon;
   `inside` (may be NULL) says lat/lon is within it. */
bool ls_globe_nearest(double lat, double lon, ls_globe_fill_fn fill, void *ctx, char code[8], float *rlat, float *rlon,
                      bool *inside);

#ifdef __cplusplus
}
#endif
#endif
