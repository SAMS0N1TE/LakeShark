#ifndef MAP_TILES_H
#define MAP_TILES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**/
/* 3x3 around the view centre. A pan of one tile only ever needs three new
   decodes, which is what keeps this off the audio path. */
#define MAP_TILE_SLOTS 9
#define MAP_TILE_PX    256

bool map_tiles_init(void);
void map_tiles_deinit(void);
bool map_tiles_ready(void);

/* Decoded RGB565, MAP_TILE_PX square, owned by the cache - do not free.
   NULL when the tile is not on the card or will not decode. */
const uint8_t *map_tiles_get(int z, int x, int y);

bool map_tiles_have_zoom(int z);
void map_tiles_invalidate(void);

/**/
/* Cheap file-system probes for the status overlay.  They stat only, they do
   not decode, they do not touch the JPEG engine - so it is safe to call them
   every frame from updateTiles without pushing anything onto the audio path.
   The distinction between "no card" and "no pack" matters at the UI layer
   because they suggest different corrective actions. */
bool map_tiles_have_sd(void);
bool map_tiles_have_pack(void);
bool map_tiles_have(int z, int x, int y);

/* Slippy-map projection, matching host/tilepack.py exactly. Fractional, so the
   integer part is the tile and the remainder is the offset within it. */
double map_lon2tilex(double lon, int z);
double map_lat2tiley(double lat, int z);

#ifdef __cplusplus
}
#endif

#endif
