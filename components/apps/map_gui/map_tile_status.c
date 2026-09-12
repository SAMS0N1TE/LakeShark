#include "map_tile_status.h"

#include <stddef.h>

/**/
map_tile_state_t map_tile_classify(const map_tile_probe_t *p)
{
    if (!p)               return MAP_TILE_STATE_INIT_FAIL;
    if (!p->renderer_ok)  return MAP_TILE_STATE_INIT_FAIL;
    /* Home has to come before SD: without a projection origin the tile grid
       has no defined position, so reporting "no card" then would be a lie
       about the reason the map is blank. */
    if (!p->home_set)     return MAP_TILE_STATE_NO_HOME;
    if (!p->sd_mounted)   return MAP_TILE_STATE_NO_SD;
    if (!p->pack_present) return MAP_TILE_STATE_NO_PACK;
    /* A partial 3x3 (some tiles at the edge of the pack) still counts as OK:
       the user sees real map, and reporting NO_COVERAGE while they're
       looking at eight decoded tiles would be the very confusion this fix
       exists to remove. */
    if (p->drawn > 0)     return MAP_TILE_STATE_OK;
    if (!p->zoom_present) return MAP_TILE_STATE_NO_ZOOM;
    if (p->files_present == 0) return MAP_TILE_STATE_NO_COVERAGE;
    return MAP_TILE_STATE_DECODE_FAIL;
}

const char *map_tile_state_text(map_tile_state_t s)
{
    switch (s) {
    case MAP_TILE_STATE_OK:          return "tiles";
    case MAP_TILE_STATE_INIT_FAIL:   return "tile renderer down";
    case MAP_TILE_STATE_NO_HOME:     return "tiles: home unset";
    case MAP_TILE_STATE_NO_SD:       return "no SD card - tiles unavailable";
    case MAP_TILE_STATE_NO_PACK:     return "no /sdcard/tiles pack";
    case MAP_TILE_STATE_NO_ZOOM:     return "tiles: no data at this zoom";
    case MAP_TILE_STATE_NO_COVERAGE: return "tiles: outside pack coverage";
    case MAP_TILE_STATE_DECODE_FAIL: return "tile decode failed";
    }
    return "tiles: unknown state";
}
