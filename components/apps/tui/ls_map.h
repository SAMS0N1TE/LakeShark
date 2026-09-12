/* The map: a PMTiles archive, libcarto, and a viewport over both. */

#ifndef LS_MAP_H
#define LS_MAP_H

#include <stdbool.h>
#include <stdint.h>

/* carto_label, which ls_map_labels hands back. */
#include "carto/label.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ls_map_begin(int px_w, int px_h);
void ls_map_end(void);

/* Open an archive. The previous one is closed first. False when the file is
   missing or is not a PMTiles v3 archive of MVT tiles; ls_map_status says
   which. Safe to call with no card in, which is the usual case. */
bool ls_map_open(const char *path);

const char *ls_map_status(void);

void ls_map_center(double lat, double lon);

/* Where the map last actually WORKED, and how to get back to it. */

/* Which zoom, if any, actually has a tile over this point. */

int ls_map_zoom_covering(double lat, double lon);

bool ls_map_last_good(double *lat, double *lon, int *zoom);
bool ls_map_go_last_good(void);
void ls_map_get_center(double *lat, double *lon);
void ls_map_zoom_by(int dz);
int  ls_map_zoom(void);

/* Move by whole pixels of the current frame, which is what a swipe and an
   arrow key both mean. */
void ls_map_pan(int dx_px, int dy_px);

/* The rendered frame, redrawn only when the view has moved. */

const uint16_t *ls_map_render(int *w, int *h);

/* Which of the five things a pixel is, so a caller drawing into a character
   grid can pick a colour and a glyph without knowing libcarto's palette.
   GROUND is the background and is what "nothing here" looks like. */
typedef enum {
    LS_MAP_GROUND = 0,
    LS_MAP_WATER,
    LS_MAP_PARK,
    LS_MAP_BUILDING,
    LS_MAP_ROAD,
} ls_map_ink_t;

ls_map_ink_t ls_map_classify(uint16_t px);

/* The same answer for a run of pixels, in one call.

   A caller turning a frame into cells classifies every sub-pixel of it -
   about thirty-six thousand of them for one portrait screen - and paid a
   cross-module function call for each. `out` gets one ls_map_ink_t per
   pixel, as bytes. */
void ls_map_classify_row(const uint16_t *px, uint8_t *out, int n);

/* How many pixels a tile is drawn at.

   A caller rendering into a buffer smaller than the panel has to shrink this
   by the same factor, or the same framebuffer covers proportionally less
   ground and the map is silently zoomed in. 256 is the natural size and the
   default; the map screen sets it from its own sub-cell resolution. */
void ls_map_set_tile_px(int px);
int  ls_map_tile_px(void);

/* The place names in the frame that was last rendered. */

int ls_map_labels(const carto_label **out);

/* A number that changes when, and only when, the pixels did.

   A caller that turns the frame into something else - cells, a thumbnail,
   an overlay - has the same problem ls_map_render had: the expensive part
   is a pure function of a picture that mostly does not change, and there is
   no cheap way to ask whether it did. Comparing centre, zoom and size means
   every such caller keeps its own copy of the view and gets it wrong when a
   new key is added here. One counter answers it for all of them. */
uint32_t ls_map_render_serial(void);

void ls_map_zoom_range(int *lo, int *hi);
int  ls_map_source_zoom(void);

/* Whether the frame buffer got internal RAM or fell back to PSRAM.
   The rasteriser writes every painted pixel here, so which one it is changes
   what a render costs - and a timing taken without knowing which is a
   timing of nothing. */
bool ls_map_frame_is_internal(void);

/* True while a render is part drawn.

   A render is spread over frames now - see the note in ls_map.c - so the
   frame handed back can be a picture with tiles still missing from it. A
   caller that wants to say "there is nothing here" has to know the
   difference between a frame that came back empty and one that has not
   finished arriving, because those are opposite messages. */
bool ls_map_render_busy(void);

/* How much one slice of a render may do. */

void ls_map_set_step_limit(int budget_us, int max_tiles);

/* How the tile store is doing: how many tiles it holds, and how often one was wanted that it already had. */

void ls_map_tile_cache_stats(int *slots, uint32_t *hits, uint32_t *misses,
                             uint32_t *absent);

void ls_map_tile_cache_bytes(uint32_t *held, uint32_t *budget);

/* How much PSRAM the store may spend, in bytes.

   Takes effect at once and empties what is held, because the store's
   contents are exactly what the budget was spent on. Six megabytes by
   default: PSRAM is the one resource this board has spare, and a store
   smaller than the working set is not a small cache but a slow no-cache. */
void ls_map_set_cache_budget(uint32_t bytes);

/* What the last render did, for a diagnostics line and for a test:
   how many tiles the viewport covered, how many the archive actually had,
   and how long the whole frame took. */
typedef struct {
    int      tiles_wanted;
    int      tiles_drawn;
    uint32_t render_us;
    uint32_t arena_peak;
    /* Where the render's time went, split two ways. */

    uint32_t fetch_us;    /* getting tile bytes out of the archive       */
    uint32_t raster_us;   /* parsing and drawing them                    */
    /* The largest ring the geometry scratch had to hold. The scratch
       is 65536 points and lives in PSRAM; this says how much of it a real
       archive ever needs. */
    int      scratch_peak;
} ls_map_stats_t;

void ls_map_stats(ls_map_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LS_MAP_H */
