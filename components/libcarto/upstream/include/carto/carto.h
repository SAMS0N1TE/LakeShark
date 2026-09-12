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

int  carto_scratch_peak(void);
void carto_scratch_peak_reset(void);

carto_ctx *carto_begin(carto_arena *arena, carto_framebuffer *fb,
                       carto_viewport *vp, const carto_style *style);

void carto_set_label_sink(carto_ctx *ctx, carto_label_sink *sink);

int carto_render_tile(carto_ctx *ctx, const uint8_t *mvt, size_t len,
                      int tile_x, int tile_y, int tile_z);
int carto_render_overlay(carto_ctx *ctx, const carto_overlay_point *pts, int n);
void carto_end(carto_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif
