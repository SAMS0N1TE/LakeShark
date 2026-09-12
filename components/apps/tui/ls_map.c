/* See ls_map.h. The part neither vendored half has: which tiles a
   viewport covers and where each one lands. */
#include "ls_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "carto/carto.h"
#include "carto/geom.h"
#include "carto/raster.h"
#include "carto/style.h"

#include "zeromesh_pmtiles.h"

/* So a refused centre says which module refused it. */
static const char *TAG = "ls_map";

/* Tile pixels. 256 is what the archives are cut at and what libcarto's
   default style is drawn for; asking for anything else scales the geometry
   and thickens the roads, which on a 10x17 cell grid is the difference
   between a legible map and a smear. */
/* How big a tile is drawn, which is now the caller's business. */

#define TILE_PX_NATURAL 256
static int s_tile_px = TILE_PX_NATURAL;

/* The arena. */

#define ARENA_BYTES (768u * 1024u)

static uint8_t *s_tile;
static uint32_t s_tile_cap;

/* What the cached frame was drawn for. Any difference and it is
   drawn again; no difference and the pixels stand. */

#define LABELS_MAX 64
static carto_label      s_labels[LABELS_MAX];
static carto_label_sink s_label_sink = { s_labels, LABELS_MAX, 0 };

/* Anything that frees what a part-drawn render points at has to
   abandon it first. Declared here because begin, end and open all come
   before the step machinery that defines it. */
static void step_abandon(void);

static uint32_t s_serial;

/* See the note in ls_map_begin: the frame is in PSRAM and internal RAM has
   been measured and ruled out. Kept as a getter so the console readout does
   not have to assume it. */
bool ls_map_frame_is_internal(void) { return false; }

static int  s_tz_used;      /* the zoom the last render actually fetched */

uint32_t ls_map_render_serial(void) { return s_serial; }

int ls_map_labels(const carto_label **out)
{
    if (out) *out = s_labels;
    return s_label_sink.n;
}

static bool   s_cache_valid;
static double s_cache_lat, s_cache_lon;
static int    s_cache_zoom, s_cache_w, s_cache_h;

static uint16_t   *s_pixels;
static uint8_t    *s_arena_buf;
static int         s_w, s_h;

static PmTiles    *s_pm;

/* The tiles a pan did not move off the screen. */

/* Six megabytes, and the size is the whole difference between this working and not. */

/* THE BUDGET BUYS TILES NOW, NOT SLOTS OF THE WORST TILE'S SIZE. */

#define TILE_CACHE_BUDGET (6u * 1024u * 1024u)
#define TILE_CACHE_MAX    256

typedef struct {
    uint8_t *buf;
    uint32_t cap;       /* allocated, 0 for a slot that holds nothing */
    uint32_t len;       /* bytes held */
    uint8_t  z;
    uint32_t x, y;
    uint32_t used;      /* the clock reading when it was last wanted */
    bool     valid;
} tile_slot_t;

/* IN PSRAM, and this was missed first time round.

   Raising the slot count from 32 to 256 put a 32-byte record per slot into
   internal .bss - eight kilobytes on a board whose free internal heap is
   twenty-four, and measured: it fell from 27.7 kB to 24.2 kB across the
   change. The table is touched only from the render path, which is a task
   and never an ISR, so it belongs in the memory this store already spends. */
EXT_RAM_BSS_ATTR static tile_slot_t s_tc[TILE_CACHE_MAX];
static int         s_tc_n;        /* slots currently holding a tile */
static uint32_t    s_tc_bytes;    /* allocated across all of them */
static uint32_t    s_tc_budget = TILE_CACHE_BUDGET;
static uint32_t    s_tc_clock;
static uint32_t    s_tc_hits, s_tc_misses, s_tc_absent;

static void tile_cache_free(void)
{
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        heap_caps_free(s_tc[i].buf);
        s_tc[i].buf = NULL;
        s_tc[i].cap = 0;
        s_tc[i].valid = false;
        s_tc[i].len = 0;
    }
    s_tc_n = 0;
    s_tc_bytes = 0;
    s_tc_hits = s_tc_misses = s_tc_absent = 0;
}

void ls_map_set_cache_budget(uint32_t bytes)
{
    s_tc_budget = bytes;
    tile_cache_free();
}

/* Nothing to take up front any more - the store fills as tiles arrive. Kept
   as the one place that knows a new archive invalidates everything. */
static void tile_cache_begin(void)
{
    tile_cache_free();
}

static tile_slot_t *tile_cache_find(uint8_t z, uint32_t x, uint32_t y)
{
    for (int i = 0; i < TILE_CACHE_MAX; i++)
        if (s_tc[i].valid && s_tc[i].z == z && s_tc[i].x == x && s_tc[i].y == y)
            return &s_tc[i];
    return NULL;
}

/* The occupied slot wanted longest ago. NULL when the store is empty, which
   is the one case where nothing can be freed to make room. */
static tile_slot_t *tile_cache_oldest(void)
{
    tile_slot_t *worst = NULL;
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (!s_tc[i].valid) continue;
        if (!worst || s_tc[i].used < worst->used) worst = &s_tc[i];
    }
    return worst;
}

static void tile_cache_drop(tile_slot_t *t)
{
    if (!t || !t->cap) return;
    s_tc_bytes -= t->cap;
    if (t->valid) s_tc_n--;
    heap_caps_free(t->buf);
    t->buf = NULL;
    t->cap = 0;
    t->len = 0;
    t->valid = false;
}

static void tile_cache_put(uint8_t z, uint32_t x, uint32_t y,
                           const uint8_t *bytes, uint32_t len)
{
    if (!len || len > s_tc_budget) return;

    while (s_tc_bytes + len > s_tc_budget) {
        tile_slot_t *victim = tile_cache_oldest();
        if (!victim) return;               /* nothing left to give */
        tile_cache_drop(victim);
    }

    tile_slot_t *slot = NULL;
    for (int i = 0; i < TILE_CACHE_MAX; i++)
        if (!s_tc[i].valid) { slot = &s_tc[i]; break; }

    if (!slot) {
        slot = tile_cache_oldest();
        if (!slot) return;
        tile_cache_drop(slot);
    }
    if (slot->cap) tile_cache_drop(slot);

    slot->buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* A failure here costs time and not correctness - the caller already
       holds the bytes in the scratch buffer and renders from there. */
    if (!slot->buf) return;

    memcpy(slot->buf, bytes, len);
    slot->cap = len;
    slot->len = len;
    slot->z = z; slot->x = x; slot->y = y;
    slot->used = ++s_tc_clock;
    slot->valid = true;
    s_tc_bytes += len;
    s_tc_n++;
}

/* A tile's bytes, from the store or from the card.

   Returns false the same way pmtiles_get_tile does - the archive does not
   have it - so the caller cannot tell a miss from an absence and does not
   have to. A tile that is absent is NOT cached: absence is cheap to
   rediscover and caching it would spend a slot on nothing. */
static bool tile_bytes(uint8_t z, uint32_t x, uint32_t y,
                       const uint8_t **out, size_t *out_len)
{
    tile_slot_t *hit = tile_cache_find(z, x, y);
    if (hit) {
        hit->used = ++s_tc_clock;
        *out = hit->buf;
        *out_len = hit->len;
        s_tc_hits++;
        return true;
    }
    /* Through the scratch buffer, always. The store cannot size a
       slot before the length is known, and the render happens from here
       either way - what the store keeps is a copy. */
    size_t len = 0;
    if (!pmtiles_get_tile(s_pm, z, x, y, s_tile, s_tile_cap, &len) || !len) {

        s_tc_absent++;
        return false;
    }

    s_tc_misses++;

    tile_cache_put(z, x, y, s_tile, (uint32_t)len);
    *out = s_tile;
    *out_len = len;
    return true;
}

void ls_map_tile_cache_stats(int *slots, uint32_t *hits, uint32_t *misses,
                             uint32_t *absent)
{
    if (slots)  *slots = s_tc_n;
    if (hits)   *hits = s_tc_hits;
    if (misses) *misses = s_tc_misses;
    if (absent) *absent = s_tc_absent;
}

void ls_map_tile_cache_bytes(uint32_t *held, uint32_t *budget)
{
    if (held)   *held = s_tc_bytes;
    if (budget) *budget = s_tc_budget;
}

static char        s_path[64];
static const char *s_why = "no map archive loaded";

static double s_lat = 43.4445, s_lon = -71.6473;   /* until told otherwise */
static int    s_zoom = 12;

/* The last view that drew a tile, so "take me back" always has an
   answer. Seeded with the compiled default, which is inside the archive this
   board ships with and is a better guess than nothing on a fresh card. */
static double s_good_lat = 43.4445, s_good_lon = -71.6473;
static int    s_good_zoom = 12;
static bool   s_have_good;

static ls_map_stats_t s_st;

/* Set only by a begin that asked PSRAM for memory and was refused. */
static bool s_alloc_failed;

/* ------------------------------------------------------------ lifetime -- */

void ls_map_end(void)
{

    step_abandon();
    if (s_pm) { pmtiles_close(s_pm); s_pm = NULL; }
    heap_caps_free(s_pixels);   s_pixels = NULL;
    heap_caps_free(s_arena_buf); s_arena_buf = NULL;
    heap_caps_free(s_tile); s_tile = NULL; s_tile_cap = 0;
    s_w = s_h = 0;
    s_why = "no map archive loaded";
}

bool ls_map_begin(int px_w, int px_h)
{
    if (px_w <= 0 || px_h <= 0) return false;
    if (s_pixels && px_w == s_w && px_h == s_h) return true;

    /* A resize frees the frame and the arena, and a render in progress holds a carto context inside that arena and a framebuffer descriptor pointing at those pixels. */

    step_abandon();

    heap_caps_free(s_pixels);
    heap_caps_free(s_arena_buf);
    s_pixels = NULL;
    s_arena_buf = NULL;

    /* PSRAM, and internal RAM is NOT an option. */

    const size_t px = (size_t)px_w * (size_t)px_h * sizeof(uint16_t);
    s_pixels = heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_arena_buf = heap_caps_malloc(ARENA_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pixels || !s_arena_buf) {
        heap_caps_free(s_pixels);   s_pixels = NULL;
        heap_caps_free(s_arena_buf); s_arena_buf = NULL;
        s_w = s_h = 0;
        s_alloc_failed = true;
        s_why = "no PSRAM for the map";
        return false;
    }

    s_w = px_w;
    s_h = px_h;
    s_alloc_failed = false;
    memset(s_pixels, 0, px);
    if (!s_pm) s_why = "no map archive loaded";
    return true;
}

void ls_map_set_tile_px(int px)
{
    if (px < 16) px = 16;
    if (px > TILE_PX_NATURAL) px = TILE_PX_NATURAL;
    if (px == s_tile_px) return;
    s_tile_px = px;
    s_cache_valid = false;
}

int ls_map_tile_px(void) { return s_tile_px; }

bool ls_map_open(const char *path)
{
    /* A different archive means the tiles a render was part way
       through are the wrong tiles. */
    step_abandon();
    tile_cache_free();
    if (s_pm) { pmtiles_close(s_pm); s_pm = NULL; }
    heap_caps_free(s_tile); s_tile = NULL; s_tile_cap = 0;
    /* The render cache is keyed on the VIEW, which cannot see that the tiles
       under it were replaced. Opening a different archive is the one change
       that key misses, so it is dropped by hand here. */
    s_cache_valid = false;

    if (!path || !*path) { s_why = "no map archive loaded"; return false; }

    s_pm = pmtiles_open(path);
    if (!s_pm) {

        s_why = "cannot read that map archive";
        return false;
    }

    snprintf(s_path, sizeof(s_path), "%s", path);

    s_tile_cap = pmtiles_max_tile_len(s_pm);
    if (s_tile_cap)
        s_tile = heap_caps_malloc(s_tile_cap,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* The store holds this archive's tiles, so it is emptied when
       another one is opened. It takes no memory until a tile arrives. */
    tile_cache_begin();
    if (!s_tile) {
        pmtiles_close(s_pm);
        s_pm = NULL;
        s_why = "no memory for a tile";
        return false;
    }

    const int zmin = pmtiles_min_zoom(s_pm), zmax = pmtiles_max_zoom(s_pm);
    if (s_zoom < zmin) s_zoom = zmin;
    if (s_zoom > zmax) s_zoom = zmax;

    s_why = NULL;
    return true;
}

const char *ls_map_status(void)
{
    /* Not started is not out of memory. */

    if (!s_pixels) return s_alloc_failed ? "no PSRAM for the map"
                                         : "the map has not been started";
    return s_why;
}

/* ------------------------------------------------------------ viewport -- */

/* Which of the five things drew this pixel. */

static uint16_t s_ink_rgb[5];
static bool     s_ink_ready;

static void ink_table(void)
{
    if (s_ink_ready) return;
    carto_style st;
    carto_style_default(&st);
    s_ink_rgb[LS_MAP_GROUND]   = carto_rgb565(st.bg);
    s_ink_rgb[LS_MAP_WATER]    = carto_rgb565(st.water);
    s_ink_rgb[LS_MAP_PARK]     = carto_rgb565(st.park);
    s_ink_rgb[LS_MAP_BUILDING] = carto_rgb565(st.building);
    s_ink_rgb[LS_MAP_ROAD]     = carto_rgb565(st.road_color);
    s_ink_ready = true;
}

/* Nearly every pixel is exactly one of the five, so ask that first. */

static inline ls_map_ink_t classify_exact(uint16_t px)
{
    /* Ordered by how much of a frame each one covers: the background is most
       of it, then water and parks, and roads are a few pixels wide. */
    if (px == s_ink_rgb[LS_MAP_GROUND])   return LS_MAP_GROUND;
    if (px == s_ink_rgb[LS_MAP_WATER])    return LS_MAP_WATER;
    if (px == s_ink_rgb[LS_MAP_PARK])     return LS_MAP_PARK;
    if (px == s_ink_rgb[LS_MAP_BUILDING]) return LS_MAP_BUILDING;
    if (px == s_ink_rgb[LS_MAP_ROAD])     return LS_MAP_ROAD;
    return (ls_map_ink_t)-1;              /* not one of them: search for it */
}

static ls_map_ink_t classify_near(uint16_t px)
{

    const int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
    int best = LS_MAP_GROUND, best_d = 1 << 30;
    for (int i = 0; i < 5; i++) {
        const uint16_t c = s_ink_rgb[i];
        const int dr = r - ((c >> 11) & 0x1F);
        const int dg = g - ((c >> 5) & 0x3F);
        const int db = b - (c & 0x1F);
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (ls_map_ink_t)best;
}

ls_map_ink_t ls_map_classify(uint16_t px)
{
    ink_table();
    const ls_map_ink_t k = classify_exact(px);
    return (k == (ls_map_ink_t)-1) ? classify_near(px) : k;
}

/* A whole row at a time, because the caller wants a whole row.

   Thirty-six thousand calls across a translation unit boundary is thirty-six
   thousand function calls the compiler cannot see through, on top of an
   ink_table() guard checked thirty-six thousand times. Handing over the row
   costs one call, hoists the guard out, and leaves the loop somewhere the
   optimiser can do something with it. */
void ls_map_classify_row(const uint16_t *px, uint8_t *out, int n)
{
    if (!px || !out || n <= 0) return;
    ink_table();

    const uint16_t g0 = s_ink_rgb[LS_MAP_GROUND];
    const uint16_t w0 = s_ink_rgb[LS_MAP_WATER];
    const uint16_t p0 = s_ink_rgb[LS_MAP_PARK];
    const uint16_t b0 = s_ink_rgb[LS_MAP_BUILDING];
    const uint16_t r0 = s_ink_rgb[LS_MAP_ROAD];

    for (int i = 0; i < n; i++) {
        const uint16_t v = px[i];
        if      (v == g0) out[i] = LS_MAP_GROUND;
        else if (v == w0) out[i] = LS_MAP_WATER;
        else if (v == p0) out[i] = LS_MAP_PARK;
        else if (v == b0) out[i] = LS_MAP_BUILDING;
        else if (v == r0) out[i] = LS_MAP_ROAD;
        else              out[i] = (uint8_t)classify_near(v);
    }
}

void ls_map_center(double lat, double lon)
{
    /* AN EXACT 0,0 IS NOT A PLACE, IT IS A ZEROED STRUCT. */

    if (lat == 0.0 && lon == 0.0) {
        ESP_LOGW(TAG, "refused a centre on exactly 0,0 - a caller handed over "
                      "a position it had not filled in");
        return;
    }

    if (lat >  85.0) lat =  85.0;
    if (lat < -85.0) lat = -85.0;
    while (lon >  180.0) lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    s_lat = lat;
    s_lon = lon;
}

bool ls_map_last_good(double *lat, double *lon, int *zoom)
{
    if (lat)  *lat  = s_good_lat;
    if (lon)  *lon  = s_good_lon;
    if (zoom) *zoom = s_good_zoom;
    return s_have_good;
}

bool ls_map_go_last_good(void)
{
    s_lat = s_good_lat;
    s_lon = s_good_lon;
    s_zoom = s_good_zoom;
    return s_have_good;
}

void ls_map_get_center(double *lat, double *lon)
{
    if (lat) *lat = s_lat;
    if (lon) *lon = s_lon;
}

int ls_map_zoom(void) { return s_zoom; }

void ls_map_zoom_by(int dz)
{
    int z = s_zoom + dz;

    const int lo = s_pm ? pmtiles_min_zoom(s_pm) : 0;
    const int hi = s_pm ? pmtiles_max_zoom(s_pm) : 22;
    if (z < lo) z = lo;
    if (z > hi) z = hi;
    s_zoom = z;
}

/* See ls_map.h. The same projection ls_map_pan uses, because a
   second copy of it is a second chance to be off by a tile. */
static bool tile_at(int z, double lat, double lon, uint32_t *tx, uint32_t *ty)
{
    if (z < 0 || z > 22) return false;
    const double n = ldexp(1.0, z);
    double fx = carto_lon_to_norm(lon);
    double fy = carto_lat_to_norm(lat);
    /* Off the top or bottom of the projection is not a tile at any zoom.
       Web Mercator stops at about 85 degrees and a caller may hand over
       anything. */
    if (!(fy >= 0.0) || fy > 1.0) return false;
    while (fx > 1.0) fx -= 1.0;
    while (fx < 0.0) fx += 1.0;
    double x = fx * n, y = fy * n;
    const double last = n - 1.0;
    if (x > last) x = last;
    if (y > last) y = last;
    *tx = (uint32_t)x;
    *ty = (uint32_t)y;
    return true;
}

/* How far the search may wander, and why it is bounded at all. */

#define ZOOM_SEARCH_SPAN 3

int ls_map_zoom_covering(double lat, double lon)
{
    if (!s_pm) return -1;

    const int zmin = pmtiles_min_zoom(s_pm);
    const int zmax = pmtiles_max_zoom(s_pm);
    uint32_t tx = 0, ty = 0;

    if (s_zoom >= zmin && s_zoom <= zmax &&
        tile_at(s_zoom, lat, lon, &tx, &ty) &&
        pmtiles_has_tile(s_pm, (uint8_t)s_zoom, tx, ty))
        return s_zoom;

    const int lo = (s_zoom - ZOOM_SEARCH_SPAN) > zmin
                 ? (s_zoom - ZOOM_SEARCH_SPAN) : zmin;
    const int hi = (s_zoom + ZOOM_SEARCH_SPAN) < zmax
                 ? (s_zoom + ZOOM_SEARCH_SPAN) : zmax;

    for (int z = s_zoom - 1; z >= lo; z--)
        if (tile_at(z, lat, lon, &tx, &ty) &&
            pmtiles_has_tile(s_pm, (uint8_t)z, tx, ty))
            return z;

    for (int z = s_zoom + 1; z <= hi; z++)
        if (tile_at(z, lat, lon, &tx, &ty) &&
            pmtiles_has_tile(s_pm, (uint8_t)z, tx, ty))
            return z;

    return -1;
}

void ls_map_pan(int dx_px, int dy_px)
{
    if (!dx_px && !dy_px) return;

    const double world = ldexp(1.0, s_zoom) * (double)s_tile_px;
    double nx = carto_lon_to_norm(s_lon) * world + (double)dx_px;
    double ny = carto_lat_to_norm(s_lat) * world + (double)dy_px;

    if (ny < 0) ny = 0;
    if (ny > world) ny = world;

    double fx = nx / world;
    double fy = ny / world;
    while (fx > 1.0) fx -= 1.0;
    while (fx < 0.0) fx += 1.0;

    s_lon = fx * 360.0 - 180.0;
    s_lat = atan(sinh(M_PI * (1.0 - 2.0 * fy))) * 180.0 / M_PI;
}

/* -------------------------------------------------------------- render -- */

void ls_map_stats(ls_map_stats_t *out) { if (out) *out = s_st; }

void ls_map_zoom_range(int *lo, int *hi)
{
    if (lo) *lo = s_pm ? pmtiles_min_zoom(s_pm) : 0;
    if (hi) *hi = s_pm ? pmtiles_max_zoom(s_pm) : 0;
}

int ls_map_source_zoom(void) { return s_tz_used; }

/* A render spread over frames, because it cannot be made short. */

#define STEP_BUDGET_US 12000   /* of a 40 ms frame, leaving room for the rest */

static int s_step_budget_us = STEP_BUDGET_US;
static int s_step_max_tiles;          /* 0: no count limit, time governs */

void ls_map_set_step_limit(int budget_us, int max_tiles)
{
    s_step_budget_us = (budget_us > 0) ? budget_us : STEP_BUDGET_US;
    s_step_max_tiles = (max_tiles > 0) ? max_tiles : 0;
}

static bool             s_step_on;
static carto_ctx       *s_step_ctx;
static carto_arena      s_step_arena;
static carto_framebuffer s_step_fb;
static carto_style      s_step_style;
static ls_map_stats_t   s_step_st;
static int64_t          s_step_t0;
static int              s_step_tx0, s_step_tx1, s_step_ty0, s_step_ty1;
static int              s_step_x, s_step_y;      /* where the walk is up to */
static int              s_step_tz;
static double           s_step_lat, s_step_lon;  /* the view being drawn for */
static int              s_step_zoom, s_step_w, s_step_h;

static void step_abandon(void)
{
    if (s_step_ctx) carto_end(s_step_ctx);
    s_step_ctx = NULL;
    s_step_on = false;
}

/* True when the render in progress is for the view we are being asked
   about. Anything else - a pan, a zoom, a resize - makes it stale. */
static bool step_matches(void)
{
    return s_step_on && s_step_lat == s_lat && s_step_lon == s_lon &&
           s_step_zoom == s_zoom && s_step_w == s_w && s_step_h == s_h;
}

bool ls_map_render_busy(void) { return s_step_on; }

/* One step of a render: as many tiles as the budget allows, then hand back
   whatever the frame holds. */
static const uint16_t *step_render(void)
{
    const int64_t deadline = esp_timer_get_time() + s_step_budget_us;
    const int n = 1 << s_step_tz;
    int done_here = 0;

    while (s_step_y <= s_step_ty1) {
        if (s_step_y < 0 || s_step_y >= n) { s_step_y++; s_step_x = s_step_tx0; continue; }

        while (s_step_x <= s_step_tx1) {
            const int tx = s_step_x++;
            /* Longitude wraps, latitude does not. */
            int wx = tx % n;
            if (wx < 0) wx += n;

            s_step_st.tiles_wanted++;

            size_t len = 0;
            const uint8_t *bytes = NULL;
            const int64_t tf = esp_timer_get_time();
            const bool got = tile_bytes((uint8_t)s_step_tz, (uint32_t)wx,
                                        (uint32_t)s_step_y, &bytes, &len);
            s_step_st.fetch_us += (uint32_t)(esp_timer_get_time() - tf);

            if (got && len) {
                /* tx, not wx: the renderer places the tile against the world
                   origin, and at the antimeridian the unwrapped index is the
                   one that lands in the frame. */
                const int64_t tr = esp_timer_get_time();
                const int rc = carto_render_tile(s_step_ctx, bytes, len,
                                                 tx, s_step_y, s_step_tz);
                s_step_st.raster_us += (uint32_t)(esp_timer_get_time() - tr);
                if (rc >= 0) s_step_st.tiles_drawn++;
            }

            /* The picture is republished every few tiles, not every tile. */

            if ((s_step_st.tiles_wanted & 3) == 0) s_serial++;

            /* Whichever limit runs out first. See ls_map_set_step_limit for
               why there are two. */
            done_here++;
            if (s_step_max_tiles && done_here >= s_step_max_tiles)
                return s_pixels;
            if (esp_timer_get_time() >= deadline) return s_pixels;
        }
        s_step_y++;
        s_step_x = s_step_tx0;
    }

    /* Done. Close the context, publish, and let the cache take over. */
    carto_end(s_step_ctx);
    s_step_ctx = NULL;
    s_step_on = false;

    s_step_st.arena_peak = (uint32_t)carto_arena_peak(&s_step_arena);
    s_step_st.scratch_peak = carto_scratch_peak();
    s_step_st.render_us = (uint32_t)(esp_timer_get_time() - s_step_t0);
    s_st = s_step_st;

    s_why = s_st.tiles_drawn ? NULL : "no tiles here at this zoom";

    /* Remember a view that worked. See ls_map.h. */
    if (s_st.tiles_drawn > 0) {
        s_good_lat = s_lat;
        s_good_lon = s_lon;
        s_good_zoom = s_zoom;
        s_have_good = true;
    }

    s_serial++;
    s_cache_valid = true;
    s_cache_lat = s_lat; s_cache_lon = s_lon;
    s_cache_zoom = s_zoom; s_cache_w = s_w; s_cache_h = s_h;
    return s_pixels;
}

const uint16_t *ls_map_render(int *w, int *h)
{
    if (w) *w = s_w;
    if (h) *h = s_h;
    if (!s_pixels || !s_pm || !s_tile) return NULL;

    /* Drop a stale render BEFORE anything else, including before the cache is consulted. */

    if (s_step_on && !step_matches()) step_abandon();

    if (s_cache_valid && s_cache_lat == s_lat && s_cache_lon == s_lon &&
        s_cache_zoom == s_zoom && s_cache_w == s_w && s_cache_h == s_h)
        return s_pixels;

    /* A render already under way for this exact view carries on. */
    if (s_step_on) return step_render();

    /* ---- a new render starts here ---------------------------------- */

    memset(&s_step_st, 0, sizeof(s_step_st));
    s_step_t0 = esp_timer_get_time();
    carto_scratch_peak_reset();

    carto_fb_init(&s_step_fb, s_w, s_h, CARTO_FMT_RGB565, (uint8_t *)s_pixels);
    carto_arena_init(&s_step_arena, s_arena_buf, ARENA_BYTES);
    carto_style_default(&s_step_style);

    /* Sourcing tiles from a coarser zoom was tried and is WORSE. */

    s_step_tz = s_zoom;
    s_tz_used = s_step_tz;

    carto_viewport vp;
    memset(&vp, 0, sizeof(vp));
    vp.lat = s_lat;
    vp.lon = s_lon;
    vp.zoom = s_step_tz;
    vp.fb_w = s_w;
    vp.fb_h = s_h;
    vp.tile_px = s_tile_px;

    /* carto_begin clears the frame to the background and computes the world
       origin; every tile below is placed against that same origin, which is
       what makes the seams line up. */
    s_step_ctx = carto_begin(&s_step_arena, &s_step_fb, &vp, &s_step_style);
    if (!s_step_ctx) {
        s_why = "no memory to draw the map";
        return NULL;
    }

    /* Names are collected across the whole frame and not per tile: a place
       belongs to the view, not to whichever tile happened to carry it. */
    s_label_sink.n = 0;
    carto_set_label_sink(s_step_ctx, &s_label_sink);

    /* Which tiles the frame touches. The origin is the world pixel at the
       frame's top left, so the range is that divided by the tile size,
       inclusive at both ends - a frame wider than a tile touches at least
       two, and a pan almost always leaves part of the frame in a neighbour.
       Drawing only the centre tile is what left a growing band of background
       down one edge and read as a rendering fault. */
    const double tp = (double)vp.tile_px;
    const double world = ldexp(1.0, s_step_tz) * tp;
    const double ox = carto_lon_to_norm(s_lon) * world - s_w / 2.0;
    const double oy = carto_lat_to_norm(s_lat) * world - s_h / 2.0;

    s_step_tx0 = (int)floor(ox / tp);
    s_step_ty0 = (int)floor(oy / tp);
    s_step_tx1 = (int)floor((ox + s_w - 1) / tp);
    s_step_ty1 = (int)floor((oy + s_h - 1) / tp);
    s_step_x = s_step_tx0;
    s_step_y = s_step_ty0;

    /* The view this render belongs to, so a pan can tell it is stale. */
    s_step_lat = s_lat; s_step_lon = s_lon;
    s_step_zoom = s_zoom; s_step_w = s_w; s_step_h = s_h;
    s_step_on = true;

    return step_render();
}
