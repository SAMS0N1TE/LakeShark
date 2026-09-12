#ifndef CARTO_H
#define CARTO_H

#include "carto/fixedpt.h"
#include "carto/framebuffer.h"
#include "carto/style.h"
#include "carto/arena.h"
#include "carto/label.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARTO_VERSION_MAJOR 0
#define CARTO_VERSION_MINOR 1
#define CARTO_VERSION_PATCH 0

typedef struct {
    double    lat;
    double    lon;
    int       zoom;
    int       fb_w;
    int       fb_h;
    int       tile_px;
    carto_fix scale;
    carto_fix origin_x;
    carto_fix origin_y;
} carto_viewport;

enum {
    CARTO_PT_SELECTED  = 1 << 0,
    CARTO_PT_EMERGENCY = 1 << 1,
    CARTO_PT_HAS_TRACK = 1 << 2
};

typedef struct {
    double      lat;
    double      lon;
    float       track_deg;
    uint16_t    flags;
    const char *label;
} carto_overlay_point;

typedef struct carto_ctx carto_ctx;

/*LS-1067  The largest single geometry ring the scratch has had to hold.

   LS DEVIATION 3 sizes that scratch at 65536 points - half a megabyte, out
   of an arena that lives in PSRAM - and states the number rather than
   justifying it, with the honest note that trimming it blind would show up
   as missing roads rather than as an error. This is how the statement gets
   checked against real archives: measured on the way past, at the only
   moment a ring's point count is final.

   Here rather than in mvt.h because mvt.h is private to the library's own
   sources and the firmware only has include/ on its path. */
int  carto_scratch_peak(void);
void carto_scratch_peak_reset(void);

carto_ctx *carto_begin(carto_arena *arena, carto_framebuffer *fb,
                       carto_viewport *vp, const carto_style *style);
/*LS-1055  Collect place names while rendering, instead of drawing them.

   A label at map resolution needs a bitmap font; on a character grid the
   name is text in cells, which is what the grid is for. So the renderer
   hands names out and the caller places them. NULL collects none, which is
   what a caller that only wants a picture gets. */
void carto_set_label_sink(carto_ctx *ctx, carto_label_sink *sink);

int carto_render_tile(carto_ctx *ctx, const uint8_t *mvt, size_t len,
                      int tile_x, int tile_y, int tile_z);
int carto_render_overlay(carto_ctx *ctx, const carto_overlay_point *pts, int n);
void carto_end(carto_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif
