#ifndef CC_RENDER_H
#define CC_RENDER_H
#include "ctile.h"
#include "out.h"
/* Owns no storage: arena and cell buffers remain the caller's property.
   The map must be immutable for the lifetime of this state. */
typedef struct {
    int colors16, active_colors16;
    int terrain_strength,active_terrain_strength;
    cc_edges edges_mode, active_edges; /* caller option; resolved retained state */
    cc_arena *arena; cc_planes planes; cc_scene scene; cc_encoder encoder;
    const cc_ctile *map; size_t mark; int valid,labels; unsigned z;
    int64_t left,top; cc_mode mode; cc_cell *saved;
    cc_decoded_cache *decoded_cache; /* optional; supplied and owned by caller */
    cc_cell *output_row;
    int scene_ready;
} cc_renderer;
int cc_renderer_init(cc_renderer *r,cc_arena *arena,int cols,int rows,cc_cell *saved);
size_t cc_renderer_hot_bytes(int cols,int rows);
int cc_renderer_init_split(cc_renderer *r,cc_arena *hot,cc_arena *cold,int cols,int rows,cc_cell *saved);
/* Force real raster/encode work, retaining the prepared scene. Caller must not
   reset/use its arenas or decoded cache between this call and cc_render. */
void cc_renderer_reraster(cc_renderer *r);
int cc_render(cc_renderer *r,const cc_ctile *map,unsigned z,int64_t left,int64_t top,
              cc_mode mode,int labels,cc_cell *cells);
void cc_renderer_invalidate(cc_renderer *r);
#endif
