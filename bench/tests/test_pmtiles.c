/* LS_TEST_SOURCES: ZeroMesh's PMTiles reader, and a tile out of it into libcarto */
/* The tile source, and the seam between it and the renderer. */

#include "ls_test.h"

#include "zeromesh_pmtiles.h"

#include "carto/carto.h"
#include "carto/geom.h"
#include "carto/raster.h"
#include "carto/style.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PMTILES_FIXTURE
#define PMTILES_FIXTURE "fixtures/carto/franklin_z12.pmtiles"
#endif

#define LAT 43.4445
#define LON (-71.6473)

static PmTiles *g_pm;

static bool open_archive(void)
{
    if (!g_pm) g_pm = pmtiles_open(PMTILES_FIXTURE);
    return g_pm != NULL;
}

/* Slippy-map tile for a coordinate, which is the arithmetic every caller of
   pmtiles_get_tile has to do and none of it is in the reader. */
static void tile_for(double lat, double lon, int z, uint32_t *x, uint32_t *y)
{
    const double n = (double)(1u << z);
    double fx = (lon + 180.0) / 360.0;
    double s = sin(lat * 3.14159265358979323846 / 180.0);
    double fy = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * 3.14159265358979323846);
    if (fx < 0) fx = 0;
    if (fx > 1) fx = 1;
    if (fy < 0) fy = 0;
    if (fy > 1) fy = 1;
    *x = (uint32_t)(fx * n);
    *y = (uint32_t)(fy * n);
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(the_archive_opens_and_says_what_it_holds)
{
    LS_CHECK_MSG(open_archive(), "cannot open %s", PMTILES_FIXTURE);
    if (!g_pm) return;

    const uint8_t zmin = pmtiles_min_zoom(g_pm);
    const uint8_t zmax = pmtiles_max_zoom(g_pm);
    const uint32_t n = pmtiles_tile_count(g_pm);

    LS_CHECK_MSG(zmin <= zmax, "zoom range is %u..%u", zmin, zmax);
    LS_CHECK_MSG(zmax <= 22, "max zoom %u is not a slippy zoom", zmax);
    LS_CHECK_MSG(n > 0, "the archive reports no tiles");
    printf("    %s: z%u..z%u, %lu tiles, largest %lu bytes\n",
           PMTILES_FIXTURE, zmin, zmax, (unsigned long)n,
           (unsigned long)pmtiles_max_tile_len(g_pm));
}

LS_CASE(a_tile_that_is_there_comes_back_and_a_tile_that_is_not_does_not)
{
    if (!open_archive()) { LS_CHECK_MSG(false, "no archive"); return; }

    const int z = pmtiles_max_zoom(g_pm);
    uint32_t x = 0, y = 0;
    tile_for(LAT, LON, z, &x, &y);

    LS_CHECK_MSG(pmtiles_has_tile(g_pm, (uint8_t)z, x, y),
                 "z%d %lu/%lu is not in the archive", z,
                 (unsigned long)x, (unsigned long)y);

    /* The far side of the world at the same zoom. An archive of one town
       cannot hold it, and a reader that answers yes to everything would pass
       every other case here. */
    LS_CHECK_MSG(!pmtiles_has_tile(g_pm, (uint8_t)z, 1, 1),
                 "the archive claims to hold z%d 1/1", z);
}

LS_CASE(a_tile_is_read_whole)
{
    if (!open_archive()) { LS_CHECK_MSG(false, "no archive"); return; }

    const int z = pmtiles_max_zoom(g_pm);
    uint32_t x = 0, y = 0;
    tile_for(LAT, LON, z, &x, &y);

    const uint32_t cap = pmtiles_max_tile_len(g_pm);
    LS_CHECK_MSG(cap > 0 && cap < 4u * 1024 * 1024,
                 "largest tile is %lu bytes", (unsigned long)cap);
    if (!cap) return;

    uint8_t *buf = (uint8_t *)malloc(cap);
    LS_CHECK(buf != NULL);
    if (!buf) return;

    size_t len = 0;
    const bool ok = pmtiles_get_tile(g_pm, (uint8_t)z, x, y, buf, cap, &len);
    LS_CHECK_MSG(ok, "reading z%d %lu/%lu failed", z,
                 (unsigned long)x, (unsigned long)y);
    LS_CHECK_MSG(len > 0 && len <= cap,
                 "tile length %u against a %lu cap",
                 (unsigned)len, (unsigned long)cap);

    /* An MVT is a protobuf whose layers are field 3, so the first byte of a
       non-empty tile is 0x1A. This is the check that catches a short read:
       a truncated tile still has a valid first byte, but a tile read from
       the wrong offset does not. */
    if (len)
        LS_CHECK_MSG(buf[0] == 0x1A,
                     "tile starts with 0x%02X, not a protobuf layer field",
                     buf[0]);

    if (len > 1) {
        size_t len2 = 0;
        memset(buf, 0, cap);
        const bool ok2 = pmtiles_get_tile(g_pm, (uint8_t)z, x, y,
                                          buf, len - 1, &len2);
        LS_CHECK_MSG(!ok2, "a %u byte tile was accepted into %u bytes",
                     (unsigned)len, (unsigned)(len - 1));
    }

    free(buf);
}

LS_CASE(every_zoom_the_archive_claims_has_a_tile_under_the_town)
{
    /* An archive cut for z10..z12 that only actually holds z12 would pass
       every case above, and the map would go blank on a zoom out. */
    if (!open_archive()) { LS_CHECK_MSG(false, "no archive"); return; }

    for (int z = pmtiles_min_zoom(g_pm); z <= pmtiles_max_zoom(g_pm); z++) {
        uint32_t x = 0, y = 0;
        tile_for(LAT, LON, z, &x, &y);
        LS_CHECK_MSG(pmtiles_has_tile(g_pm, (uint8_t)z, x, y),
                     "z%d %lu/%lu is missing from a z%u..z%u archive",
                     z, (unsigned long)x, (unsigned long)y,
                     pmtiles_min_zoom(g_pm), pmtiles_max_zoom(g_pm));
    }
}

/* ------------------------------------------------------- the two halves -- */

#define FB_W 256
#define FB_H 256
#define ARENA_BYTES (1024u * 1024u)
#define GUARD 64
#define GUARD_BYTE 0x5A

LS_CASE(a_tile_out_of_the_archive_renders_through_libcarto)
{
    /* The seam. Everything above proves the reader returns bytes and
       test_carto_render proves the renderer draws a tile; neither says the
       bytes one produces are the bytes the other wants. */
    if (!open_archive()) { LS_CHECK_MSG(false, "no archive"); return; }

    const int z = pmtiles_max_zoom(g_pm);
    uint32_t tx = 0, ty = 0;
    tile_for(LAT, LON, z, &tx, &ty);

    const uint32_t cap = pmtiles_max_tile_len(g_pm);
    uint8_t *tile = (uint8_t *)malloc(cap);
    size_t len = 0;
    if (!tile || !pmtiles_get_tile(g_pm, (uint8_t)z, tx, ty, tile, cap, &len)) {
        LS_CHECK_MSG(false, "could not read the tile to render");
        free(tile);
        return;
    }

    const size_t px = carto_fb_row_bytes(FB_W, CARTO_FMT_RGB565) * FB_H;
    uint8_t *block = (uint8_t *)malloc(px + 2 * GUARD);
    uint8_t *arena_buf = (uint8_t *)malloc(ARENA_BYTES);
    memset(block, GUARD_BYTE, px + 2 * GUARD);
    uint8_t *pixels = block + GUARD;

    carto_framebuffer fb;
    carto_arena arena;
    carto_style style;
    carto_fb_init(&fb, FB_W, FB_H, CARTO_FMT_RGB565, pixels);
    carto_arena_init(&arena, arena_buf, ARENA_BYTES);
    carto_style_default(&style);

    double lat = 0, lon = 0;
    carto_tile_center((int)tx, (int)ty, z, &lat, &lon);

    carto_viewport vp;
    memset(&vp, 0, sizeof(vp));
    vp.lat = lat; vp.lon = lon; vp.zoom = z;
    vp.fb_w = FB_W; vp.fb_h = FB_H; vp.tile_px = FB_W;

    carto_ctx *ctx = carto_begin(&arena, &fb, &vp, &style);
    LS_CHECK_MSG(ctx != NULL, "carto_begin failed");
    if (ctx) {
        const int rc = carto_render_tile(ctx, tile, len, (int)tx, (int)ty, z);
        carto_end(ctx);
        LS_CHECK_MSG(rc >= 0, "carto_render_tile returned %d", rc);

        const uint16_t bg = carto_rgb565(style.bg);
        const uint16_t *p = (const uint16_t *)pixels;
        int painted = 0;
        for (int i = 0; i < FB_W * FB_H; i++) if (p[i] != bg) painted++;
        LS_CHECK_MSG(painted > FB_W * FB_H / 200,
                     "a tile from the archive drew only %d of %d pixels",
                     painted, FB_W * FB_H);
        printf("    z%d %lu/%lu: %u bytes -> %d of %d pixels, arena %u\n",
               z, (unsigned long)tx, (unsigned long)ty, (unsigned)len,
               painted, FB_W * FB_H, (unsigned)carto_arena_peak(&arena));
    }

    for (int i = 0; i < GUARD; i++) {
        LS_CHECK_MSG(block[i] == GUARD_BYTE, "wrote before the framebuffer");
        LS_CHECK_MSG(block[GUARD + px + i] == GUARD_BYTE,
                     "wrote past the framebuffer");
    }

    free(tile);
    free(block);
    free(arena_buf);
}
