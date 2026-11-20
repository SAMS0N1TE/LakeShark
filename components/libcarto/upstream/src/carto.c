#include "carto/carto.h"
#include "carto/raster.h"
#include "carto/geom.h"
#include "mvt.h"
#include <math.h>

struct carto_ctx {
    carto_arena *arena;
    carto_framebuffer *fb;
    const carto_style *style;
    int zoom;
    double tile_px;
    double origin_x, origin_y;
    carto_ipt *scratch;
    int scratch_cap;

    const uint8_t **val_ptr;
    int            *val_len;
    int            *val_num;
    int             val_cap;

    carto_label_sink *labels;
};

/* One layer's distinct tag values. Four thousand is what the renderer has
   always assumed; the number moved out of mvt.c so the allocation and the
   bound stay in the same place. */
#define CARTO_MVT_VALUES 4096

int  carto_scratch_peak(void)       { return mvt_scratch_peak(); }
void carto_scratch_peak_reset(void) { mvt_scratch_peak_reset(); }

carto_ctx *carto_begin(carto_arena *arena, carto_framebuffer *fb,
                       carto_viewport *vp, const carto_style *style) {
    if (!arena || !fb || !vp || !style) return NULL;

    carto_ctx *c = (carto_ctx *)carto_arena_alloc(arena, sizeof(carto_ctx), 8);
    if (!c) return NULL;

    c->arena = arena;
    c->fb = fb;
    c->style = style;
    c->zoom = vp->zoom;
    c->tile_px = (vp->tile_px > 0) ? (double)vp->tile_px : 256.0;

    double n = ldexp(1.0, vp->zoom);
    double world = n * c->tile_px;
    c->origin_x = carto_lon_to_norm(vp->lon) * world - fb->width / 2.0;
    c->origin_y = carto_lat_to_norm(vp->lat) * world - fb->height / 2.0;

    /* LS DEVIATION 3: the scratch is 65536 points of 8 bytes, which is
       512 KB. Kept at that size and stated rather than inherited: it comes
       out of the arena, the arena is in PSRAM, and a tile whose geometry
       exceeds the cap is truncated rather than refused - so trimming this to
       save PSRAM would show up as missing roads and not as an error. */
    c->scratch_cap = 1 << 16;
    c->scratch = (carto_ipt *)carto_arena_alloc(arena,
        (size_t)c->scratch_cap * sizeof(carto_ipt), 4);
    if (!c->scratch) return NULL;

    c->val_cap = CARTO_MVT_VALUES;
    c->val_ptr = (const uint8_t **)carto_arena_alloc(arena,
        (size_t)c->val_cap * sizeof(const uint8_t *), 4);
    c->val_len = (int *)carto_arena_alloc(arena,
        (size_t)c->val_cap * sizeof(int), 4);
    c->val_num = (int *)carto_arena_alloc(arena,
        (size_t)c->val_cap * sizeof(int), 4);
    if (!c->val_ptr || !c->val_len || !c->val_num) return NULL;
    c->labels = NULL;

    vp->scale = carto_fix_from_float((float)(c->tile_px / (double)CARTO_TILE_EXTENT));
    vp->origin_x = carto_fix_from_float((float)c->origin_x);
    vp->origin_y = carto_fix_from_float((float)c->origin_y);

    carto_fb_clear(fb, carto_rgb565(style->bg));
    return c;
}

void carto_set_label_sink(carto_ctx *ctx, carto_label_sink *sink)
{
    if (ctx) ctx->labels = sink;
}

int carto_render_tile(carto_ctx *ctx, const uint8_t *mvt, size_t len,
                      int tile_x, int tile_y, int tile_z) {
    (void)tile_z;
    if (!ctx || !mvt || len == 0) return -1;

    double ox = (double)tile_x * ctx->tile_px - ctx->origin_x;
    double oy = (double)tile_y * ctx->tile_px - ctx->origin_y;

    static const carto_layer_kind order[6] = {
        CARTO_LAYER_WATER, CARTO_LAYER_LANDUSE, CARTO_LAYER_BUILDING,
        CARTO_LAYER_BOUNDARY, CARTO_LAYER_ROAD, CARTO_LAYER_PLACE
    };
    for (int i = 0; i < 6; ++i) {
        carto_mvt_render_category(ctx->fb, ctx->style, mvt, len, order[i],
                                  ox, oy, ctx->tile_px, ctx->zoom,
                                  ctx->scratch, ctx->scratch_cap,
                                  ctx->val_ptr, ctx->val_len, ctx->val_num,
                                  ctx->val_cap, ctx->labels);
    }
    return 0;
}

/* LS DEVIATION 5: a coarser tile standing in for a missing one. Drawn `shift`
   zooms enlarged and only inside the missing tile's own square, so it cannot
   paint over the neighbours that did arrive. */
int carto_render_tile_under(carto_ctx *ctx, const uint8_t *mvt, size_t len,
                            int parent_x, int parent_y, int shift, int tile_x, int tile_y) {
    if (!ctx || !mvt || len == 0 || shift < 1 || shift > 6) return -1;
    carto_framebuffer *fb = ctx->fb;
    if (fb->format != CARTO_FMT_RGB565) return -1;
    const double big = ctx->tile_px * (double)(1 << shift);
    const double fx0 = (double)tile_x * ctx->tile_px - ctx->origin_x;
    const double fy0 = (double)tile_y * ctx->tile_px - ctx->origin_y;
    int x0 = (int)floor(fx0), y0 = (int)floor(fy0);
    int x1 = (int)ceil(fx0 + ctx->tile_px), y1 = (int)ceil(fy0 + ctx->tile_px);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->width) x1 = fb->width;
    if (y1 > fb->height) y1 = fb->height;
    if (x1 <= x0 || y1 <= y0) return 0;
    carto_framebuffer part = *fb;
    part.width = x1 - x0;
    part.height = y1 - y0;
    part.pixels = fb->pixels + (size_t)y0 * fb->stride + (size_t)x0 * 2;
    const double ox = (double)parent_x * big - ctx->origin_x - x0;
    const double oy = (double)parent_y * big - ctx->origin_y - y0;
    const int had = ctx->labels ? ctx->labels->n : 0;
    static const carto_layer_kind order[6] = {
        CARTO_LAYER_WATER, CARTO_LAYER_LANDUSE, CARTO_LAYER_BUILDING,
        CARTO_LAYER_BOUNDARY, CARTO_LAYER_ROAD, CARTO_LAYER_PLACE
    };
    for (int i = 0; i < 6; ++i)
        carto_mvt_render_category(&part, ctx->style, mvt, len, order[i], ox, oy, big,
                                  ctx->zoom, ctx->scratch, ctx->scratch_cap,
                                  ctx->val_ptr, ctx->val_len, ctx->val_num,
                                  ctx->val_cap, ctx->labels);
    /* Names were placed in the part's own pixels. */
    if (ctx->labels)
        for (int i = had; i < ctx->labels->n; ++i) { ctx->labels->at[i].x += x0; ctx->labels->at[i].y += y0; }
    return 0;
}

int carto_render_overlay(carto_ctx *ctx, const carto_overlay_point *pts, int n) {
    if (!ctx || !pts) return -1;
    double world = ldexp(1.0, ctx->zoom) * ctx->tile_px;
    for (int i = 0; i < n; ++i) {
        int x = (int)(carto_lon_to_norm(pts[i].lon) * world - ctx->origin_x);
        int y = (int)(carto_lat_to_norm(pts[i].lat) * world - ctx->origin_y);
        carto_rgb col = ctx->style->aircraft_color;
        if (pts[i].flags & CARTO_PT_EMERGENCY) col = ctx->style->aircraft_emergency_color;
        else if (pts[i].flags & CARTO_PT_SELECTED) col = ctx->style->aircraft_selected_color;
        carto_fill_triangle(ctx->fb, x, y - 8, x - 7, y + 8, x + 7, y + 8, col);
    }
    return 0;
}

void carto_end(carto_ctx *ctx) {
    (void)ctx;
}
