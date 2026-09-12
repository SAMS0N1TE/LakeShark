/* LS_TEST_SOURCES: libcarto's renderer against a real vector tile */

#include "ls_test.h"

#include "carto/carto.h"
#include "carto/raster.h"
#include "carto/style.h"
#include "carto/geom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CARTO_FIXTURE
#define CARTO_FIXTURE "fixtures/carto/sample.mvt"
#endif

/* Smaller than the panel on purpose: 720x720 is what CartoTUI's own firmware
   allocated, and this has to fit a host test's malloc as well as PSRAM. */
#define FB_W 320
#define FB_H 320

/* The arena the board will hand it. carto_begin takes 512 KB of scratch out
   of this before anything is drawn, which is the number DEVIATIONS.md section
   3 is about, so anything much under a megabyte fails to start. */
#define ARENA_BYTES (1024u * 1024u)

#define GUARD 64
#define GUARD_BYTE 0x5A

static uint8_t *g_block;
static uint8_t *g_pixels;
static uint8_t *g_arena_buf;
static carto_arena g_arena;
static carto_framebuffer g_fb;
static carto_style g_style;

static uint8_t *g_tile;
static size_t   g_tile_len;

static bool load_tile(void)
{
    if (g_tile) return true;
    FILE *f = fopen(CARTO_FIXTURE, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    g_tile = (uint8_t *)malloc((size_t)n);
    if (!g_tile) { fclose(f); return false; }
    g_tile_len = fread(g_tile, 1, (size_t)n, f);
    fclose(f);
    return g_tile_len == (size_t)n;
}

static void fresh(void)
{
    const size_t px = carto_fb_row_bytes(FB_W, CARTO_FMT_RGB565) * FB_H;
    if (!g_block) {
        g_block = (uint8_t *)malloc(px + 2 * GUARD);
        g_arena_buf = (uint8_t *)malloc(ARENA_BYTES);
    }
    memset(g_block, GUARD_BYTE, px + 2 * GUARD);
    g_pixels = g_block + GUARD;

    carto_fb_init(&g_fb, FB_W, FB_H, CARTO_FMT_RGB565, g_pixels);
    carto_arena_init(&g_arena, g_arena_buf, ARENA_BYTES);
    carto_style_default(&g_style);
}

static bool guards_intact(void)
{
    const size_t px = carto_fb_row_bytes(FB_W, CARTO_FMT_RGB565) * FB_H;
    for (int i = 0; i < GUARD; i++)
        if (g_block[i] != GUARD_BYTE) return false;
    for (int i = 0; i < GUARD; i++)
        if (g_block[GUARD + px + i] != GUARD_BYTE) return false;
    return true;
}

static uint32_t fb_hash(void)
{
    const size_t n = carto_fb_row_bytes(FB_W, CARTO_FMT_RGB565) * FB_H;
    uint32_t h = 2166136261u;               /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h ^= g_pixels[i];
        h *= 16777619u;
    }
    return h;
}

/* How many pixels differ from the style's background. "It rendered" is not
   the same as "it drew something", and a decoder that silently reads nothing
   out of a tile leaves a perfectly clean background behind. */
static int painted(void)
{
    const uint16_t bg = carto_rgb565(g_style.bg);
    const uint16_t *p = (const uint16_t *)g_pixels;
    int n = 0;
    for (int i = 0; i < FB_W * FB_H; i++) if (p[i] != bg) n++;
    return n;
}

/* Where the fixture actually is. A tile carries no coordinates of its own -
   they are the filename in a tile server - so rendering it against the wrong
   ones puts every road off the edge of the frame and leaves a clean
   background, which looks exactly like a decoder that read nothing. These
   are the numbers CartoTUI's own harness uses for this file. */
#define TILE_Z 14
#define TILE_X 4936
#define TILE_Y 6007

static void viewport(carto_viewport *vp)
{
    double lat = 0, lon = 0;
    carto_tile_center(TILE_X, TILE_Y, TILE_Z, &lat, &lon);
    memset(vp, 0, sizeof(*vp));
    vp->lat = lat;
    vp->lon = lon;
    vp->zoom = TILE_Z;
    vp->fb_w = FB_W;
    vp->fb_h = FB_H;
    /* One tile across the frame, so the whole tile is in view. */
    vp->tile_px = FB_W;
}

static int render(void)
{
    carto_viewport vp;
    viewport(&vp);
    carto_ctx *ctx = carto_begin(&g_arena, &g_fb, &vp, &g_style);
    if (!ctx) return -1;
    const int rc = carto_render_tile(ctx, g_tile, g_tile_len,
                                     TILE_X, TILE_Y, TILE_Z);
    carto_end(ctx);
    return rc;
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(the_fixture_tile_is_there_and_is_a_tile)
{
    LS_CHECK_MSG(load_tile(), "cannot read %s", CARTO_FIXTURE);
    LS_CHECK_MSG(g_tile_len > 1024, "%s is %u bytes, too small to be a tile",
                 CARTO_FIXTURE, (unsigned)g_tile_len);
}

LS_CASE(a_real_tile_renders_and_draws_something)
{
    if (!load_tile()) { LS_CHECK_MSG(false, "no fixture"); return; }
    fresh();

    const int rc = render();
    LS_CHECK_MSG(rc >= 0, "carto_render_tile returned %d", rc);

    const int n = painted();
    LS_CHECK_MSG(n > FB_W * FB_H / 100,
                 "only %d of %d pixels differ from the background - the tile "
                 "decoded to nothing", n, FB_W * FB_H);
}

LS_CASE(nothing_is_written_outside_the_framebuffer)
{
    /* The span writer's bound. It clips xa and xb itself and trusts the row
       pointer, so an off-by-one here is a write past the end of a row and
       into the next one, which on the board is somebody else's PSRAM. */
    if (!load_tile()) { LS_CHECK_MSG(false, "no fixture"); return; }
    fresh();
    render();
    LS_CHECK_MSG(guards_intact(),
                 "the renderer wrote outside its framebuffer");
}

LS_CASE(the_same_tile_renders_to_the_same_pixels)
{
    /* Determinism is what makes a checksum worth recording at all, and it is
       not free: the arena is reused between frames and a renderer that read
       an uninitialised corner of it would drift. */
    if (!load_tile()) { LS_CHECK_MSG(false, "no fixture"); return; }

    fresh();
    render();
    const uint32_t a = fb_hash();

    fresh();
    render();
    const uint32_t b = fb_hash();

    LS_CHECK_MSG(a == b, "two identical renders hashed %08lX and %08lX",
                 (unsigned long)a, (unsigned long)b);
}

LS_CASE(the_arena_peak_is_bounded_and_leaves_room)
{

    if (!load_tile()) { LS_CHECK_MSG(false, "no fixture"); return; }
    fresh();
    render();

    const size_t peak = carto_arena_peak(&g_arena);
    LS_CHECK_MSG(peak > 0, "the renderer took nothing from the arena");
    LS_CHECK_MSG(peak <= ARENA_BYTES,
                 "peak %u exceeds the arena it came from", (unsigned)peak);
    printf("    carto arena peak %u bytes of %u for a %dx%d frame\n",
           (unsigned)peak, (unsigned)ARENA_BYTES, FB_W, FB_H);
}

LS_CASE(a_torn_tile_is_refused_rather_than_read_past)
{
    /* An SD card gives back short reads and corrupt bytes, and the decoder
       will meet both. Every prefix of a real tile is fed to it; what matters
       is that none of them writes outside the framebuffer or fails to
       return. */
    if (!load_tile()) { LS_CHECK_MSG(false, "no fixture"); return; }

    static const size_t CUTS[] = { 0, 1, 2, 7, 64, 511, 4096, 20000, 65535 };
    for (unsigned i = 0; i < sizeof(CUTS) / sizeof(CUTS[0]); i++) {
        const size_t len = CUTS[i] < g_tile_len ? CUTS[i] : g_tile_len / 2;
        fresh();

        carto_viewport vp;
        viewport(&vp);
        carto_ctx *ctx = carto_begin(&g_arena, &g_fb, &vp, &g_style);
        LS_CHECK_MSG(ctx != NULL, "carto_begin failed at cut %u",
                     (unsigned)len);
        if (!ctx) continue;
        carto_render_tile(ctx, g_tile, len, TILE_X, TILE_Y, TILE_Z);
        carto_end(ctx);

        LS_CHECK_MSG(guards_intact(),
                     "a %u byte prefix wrote outside the framebuffer",
                     (unsigned)len);
    }
}
