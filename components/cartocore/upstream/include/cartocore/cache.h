#ifndef CC_CACHE_H
#define CC_CACHE_H
#include "render.h"
/* Fixed-size slots make eviction bounded and preserve caller ownership. Each
   slot includes cells and retained tile-local geometry for viewport labels. */
#define CC_CACHE_SLOT_BYTES (512u*1024u)
typedef struct cc_cache_entry cc_cache_entry;
typedef struct {
    int colors16, active_colors16;
    int terrain_strength,active_terrain_strength;
    int deferred; /* misses return 0 without raster work; host renders directly */
    cc_ctile pending_map; int pending;
    unsigned pending_z; cc_mode pending_mode; uint32_t pending_style;
    int64_t pending_x0,pending_y0,pending_x1,pending_y1;
    cc_decoded_cache *decoded_cache; /* optional neighbor raster decode cache */
    cc_edges edges_mode, active_edges; /* caller option; changed edges clear slots */
    unsigned char *storage; size_t slots; uint64_t serial;
    cc_encoder encoder; size_t hits,misses,evictions; const uint8_t *source;
    const void *source_owner; uint64_t source_generation;
    cc_cell *snapshot; int frame_valid,frame_cols,frame_rows; unsigned frame_z;
    int64_t frame_left,frame_top; cc_mode frame_mode; uint32_t frame_style;
    cc_scene retained_scene; int scene_valid; uint64_t scene_key;
    int64_t scene_left,scene_top;
    int64_t scene_x0,scene_y0,scene_x1,scene_y1;
} cc_cell_cache;
int cc_cell_cache_init(cc_cell_cache *c,void *memory,size_t budget);
void cc_cell_cache_clear(cc_cell_cache *c);
/* Fill the latest deferred viewport, including label margin. Budget is cells,
   rounded down to 128-cell rows; <128 does no work. Returns cells rasterized.
   Scratch is restored. Map bytes/decoded cache must outlive pending work.
   Single-owner API: serialize render/step/clear. A cell budget bounds raster
   area, not wall time (geometry decode is still dataset-dependent). */
size_t cc_cell_cache_step(cc_cell_cache *c,size_t budget_cells,cc_arena *scratch);
/* left must be divisible by 2 and top by 4. Scratch is reset after each call.
   Labels use retained geometry, and stay a per-viewport overlay. */
int cc_cell_cache_render(cc_cell_cache *c,const cc_ctile *map,unsigned z,
    int64_t left,int64_t top,int cols,int rows,cc_mode mode,uint32_t style,int labels,
    cc_arena *scratch,cc_cell *out);
#endif
