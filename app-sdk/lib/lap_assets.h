/* Runtime assets, SDK 2.5 games/assets. Pure app code; no new imports.
 * All storage is supplied by the app. No allocation, no hidden cache.
 * PNGs become theme-indexed dots (0..15, 255 transparent), suitable for
 * lap_canvas and lap_game. Use lap_palette() to fill the 16 RGB565 inks.
 * Decode once during loading, never per frame. */
#ifndef LAP_ASSETS_H
#define LAP_ASSETS_H
#include "lap3d.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { LAP_ASSET_EFORMAT=-200, LAP_ASSET_EBUDGET=-201, LAP_ASSET_ESTATE=-202 };
typedef struct { uint8_t *data; size_t size, used; } lap_asset_arena_t;
/* Initialize with an app allocation or static array. The size is a hard budget.
 * Decoders roll used back on error and leave out unchanged. Reset only after
 * all images/meshes in this arena are no longer used (including l3_end). */
void lap_asset_arena_init(lap_asset_arena_t *a, void *memory, size_t bytes);
typedef struct { const uint8_t *pixels; int w, h; } lap_image_t;
/* PNG: noninterlaced, 8-bit grey/grey-alpha/RGB/RGBA; indexed 1/2/4/8-bit,
 * PLTE/tRNS. CRC, zlib Adler, DEFLATE and indices checked. Alpha <128 is
 * clear; other pixels use nearest supplied RGB565 ink. No colour management.
 * Peak arena usage = decoded filtered scanlines + output pixels + compressed
 * IDAT bytes + ~2 KiB DEFLATE workspace. Only output remains charged on success.
 * Input must not overlap arena memory. Max dimension 4096; no Adam7/16-bit.
 * Returns 0, EFORMAT or EBUDGET. Works equally on embedded PNG byte arrays. */
int lap_image_png(lap_image_t *out, const void *png, size_t bytes,
                  lap_asset_arena_t *arena, const uint16_t inks[16]);
/* L3M1: obj2l3.py --binary, little-endian header magic, u16 version=1,
 * u16 flags=1 (triangle material bytes), u32 vertex/triangle counts; then
 * xyz float32 vertices, u16 indices, u8 materials. Indices, finite vertices,
 * exact length and material <32 checked. Output uses the arena, not
 * l3_mesh_free(). Material bytes are scene material IDs; remap before draw
 * if the scene uses a different order. Input must not overlap arena. */
int lap_mesh_load(l3_mesh *out, const void *l3m, size_t bytes, lap_asset_arena_t *arena);
typedef struct {
    uint8_t *data;
    size_t capacity, size, offset;
    int job, state, error;
    char path[160];
} lap_asset_file_t;
/* Async read from /sdcard/apps_data/<app_id>/<name>. Both are plain basenames,
 * no traversal. Buffer must be static/app malloc and remain live until poll
 * finishes, even on hide. Size first, then <=64KiB chunks, no blocking SD.
 * begin returns 0 or error; poll returns 0 pending, 1 ready or negative error.
 * Zero-initialize state. Do not begin again while pending. On app unload the
 * existing SD generation cleanup owns cancellation; no extra worker/handle.
 * Check LAP_HW_SD first; an embedded fallback needs no card. */
int lap_asset_begin(lap_asset_file_t *file, const char *app_id, const char *name,
                    void *buffer, size_t budget);
int lap_asset_poll(lap_asset_file_t *file);
#ifdef __cplusplus
}
#endif
#endif
