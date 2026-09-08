/*LS-740*/
/* Offline raster tile renderer.
 *
 * Reads the z/x/y JPEG tree written by host/tilepack.py off the SD card and
 * decodes it with the P4's HARDWARE JPEG engine, not a software decoder. That
 * is the whole reason tilepack.py emits JPEG: this runs on the same CPU that
 * is demodulating P25, and inflating a PNG per tile in software would show up
 * as audio dropouts. Decode is a peripheral operation here.
 *
 * The working set is bounded at 9 tiles (3x3). At 256x256 RGB565 that is
 * 128 KB each, 1.15 MB total, in PSRAM - trivial against 32 MB, and it means
 * a pan never has to read more than three new tiles.
 */

#include "map_tiles.h"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

static const char *TAG = "maptile";

#ifndef BSP_SD_MOUNT_POINT
#define BSP_SD_MOUNT_POINT "/sdcard"
#endif

#define TILE_PX   256
#define TILE_RGB  (TILE_PX * TILE_PX * 2)      /* RGB565 */
#define MAX_JPEG  (96 * 1024)                  /* a 256px topo tile is ~10-30 KB */

static jpeg_decoder_handle_t s_jpeg = NULL;
static uint8_t *s_in   = NULL;                 /* DMA-capable input staging */
static size_t   s_in_cap = 0;

typedef struct {
    int      z, x, y;        /* what is currently decoded here, z<0 = empty */
    uint8_t *rgb;
} slot_t;

static slot_t s_slot[MAP_TILE_SLOTS];
static bool   s_ready = false;
static char   s_root[64] = BSP_SD_MOUNT_POINT "/tiles";

/*LS-763*/
/* Release everything map_tiles_init acquired and put the module back into the
   uninitialised state. Called on every failure path in init - the previous
   version returned early after each failed allocation, leaking the JPEG
   engine and any earlier slot buffers, and left s_ready false so the next
   caller happily allocated another partial set on top. LS-746 also releases
   these resources when the foreground MAP app closes. */
static void map_tiles_release_all(void)
{
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        if (s_slot[i].rgb) heap_caps_free(s_slot[i].rgb);
        s_slot[i].rgb = NULL;
        s_slot[i].z   = -1;
    }
    if (s_in) heap_caps_free(s_in);
    s_in     = NULL;
    s_in_cap = 0;
    if (s_jpeg) jpeg_del_decoder_engine(s_jpeg);
    s_jpeg = NULL;
    s_ready = false;
}

void map_tiles_deinit(void) { map_tiles_release_all(); }

bool map_tiles_init(void)
{
    if (s_ready) return true;

    jpeg_decode_engine_cfg_t cfg = { .intr_priority = 0, .timeout_ms = 1000 };
    if (jpeg_new_decoder_engine(&cfg, &s_jpeg) != ESP_OK) {
        ESP_LOGE(TAG, "no JPEG decode engine - tiles disabled");
        s_jpeg = NULL;              /* engine call may leave garbage on fail */
        return false;
    }

    jpeg_decode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    s_in = (uint8_t *)jpeg_alloc_decoder_mem(MAX_JPEG, &in_cfg, &s_in_cap);
    if (!s_in) {
        ESP_LOGE(TAG, "no input buffer");
        map_tiles_release_all();
        return false;
    }

    jpeg_decode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        size_t got = 0;
        s_slot[i].rgb = (uint8_t *)jpeg_alloc_decoder_mem(TILE_RGB, &out_cfg, &got);
        s_slot[i].z = -1;
        if (!s_slot[i].rgb) {
            ESP_LOGE(TAG, "slot %d alloc failed", i);
            map_tiles_release_all();
            return false;
        }
    }
    s_ready = true;
    ESP_LOGW(TAG, "tile renderer up: %d slots x %d KB, root %s",
             MAP_TILE_SLOTS, TILE_RGB / 1024, s_root);
    return true;
}

bool map_tiles_ready(void) { return s_ready; }

/*LS-740*/
/* Slippy-map tile maths, the same convention tilepack.py writes. Kept here
   rather than shared with AppMap's flat-earth projection on purpose: that one
   is a local approximation for range/bearing, this one must match the tile
   grid exactly or the map slides against its own overlay. */
double map_lon2tilex(double lon, int z)
{
    return (lon + 180.0) / 360.0 * (double)(1 << z);
}

double map_lat2tiley(double lat, int z)
{
    double r = lat * M_PI / 180.0;
    return (1.0 - asinh(tan(r)) / M_PI) / 2.0 * (double)(1 << z);
}

static bool tile_path(int z, int x, int y, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%d/%d/%d.jpg", s_root, z, x, y);
    struct stat st;
    return stat(out, &st) == 0 && st.st_size > 0;
}

bool map_tiles_have_zoom(int z)
{
    char p[128];
    snprintf(p, sizeof(p), "%s/%d", s_root, z);
    struct stat st;
    return stat(p, &st) == 0;
}

/*LS-764*/
bool map_tiles_have_sd(void)
{
    struct stat st;
    return stat(BSP_SD_MOUNT_POINT, &st) == 0;
}

/*LS-764*/
bool map_tiles_have_pack(void)
{
    struct stat st;
    return stat(s_root, &st) == 0;
}

/*LS-764*/
bool map_tiles_have(int z, int x, int y)
{
    char p[160];
    return tile_path(z, x, y, p, sizeof(p));
}

/* Find the slot holding (z,x,y), or the least useful slot to reuse. */
static int slot_for(int z, int x, int y, bool *hit)
{
    for (int i = 0; i < MAP_TILE_SLOTS; i++)
        if (s_slot[i].z == z && s_slot[i].x == x && s_slot[i].y == y) {
            *hit = true;
            return i;
        }
    *hit = false;
    for (int i = 0; i < MAP_TILE_SLOTS; i++)
        if (s_slot[i].z < 0) return i;
    /* All full: reuse slot 0 rotationally. A pan only ever needs three new
       tiles, so a trivial policy is enough and keeps this readable. */
    static int rr = 0;
    rr = (rr + 1) % MAP_TILE_SLOTS;
    return rr;
}

const uint8_t *map_tiles_get(int z, int x, int y)
{
    if (!s_ready) return NULL;
    if (x < 0 || y < 0 || z < 0) return NULL;
    int n = 1 << z;
    if (x >= n || y >= n) return NULL;

    bool hit = false;
    int  s = slot_for(z, x, y, &hit);
    if (hit) return s_slot[s].rgb;

    char path[160];
    if (!tile_path(z, x, y, path, sizeof(path))) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t got = fread(s_in, 1, s_in_cap, f);
    fclose(f);
    if (got == 0) return NULL;

    jpeg_decode_cfg_t dc = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_len = 0;
    esp_err_t e = jpeg_decoder_process(s_jpeg, &dc, s_in, got,
                                       s_slot[s].rgb, TILE_RGB, &out_len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "decode %d/%d/%d: %s", z, x, y, esp_err_to_name(e));
        s_slot[s].z = -1;
        return NULL;
    }
    s_slot[s].z = z; s_slot[s].x = x; s_slot[s].y = y;
    return s_slot[s].rgb;
}

void map_tiles_invalidate(void)
{
    for (int i = 0; i < MAP_TILE_SLOTS; i++) s_slot[i].z = -1;
}
