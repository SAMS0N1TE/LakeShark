#ifndef MAP_TILE_STATUS_H
#define MAP_TILE_STATUS_H

/**/
/* Why the map is blank right now, in one word for the overlay.  Before this,
   AppMap::updateTiles noticed `drawn == 0` and toggled a static bool nobody
   read, so a card that was never inserted looked identical to a decoder that
   had wedged - a distinction that costs an afternoon in the field.  The
   classifier is a pure function so the bench can exercise every state; the
   AppMap side is only a probe and a label update. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MAP_TILE_STATE_OK = 0,        /* at least one tile drew successfully */
    MAP_TILE_STATE_INIT_FAIL,     /* renderer never came up (no engine/PSRAM) */
    MAP_TILE_STATE_NO_HOME,       /* home unset; nothing to centre the grid on */
    MAP_TILE_STATE_NO_SD,         /* SD mount point is not present */
    MAP_TILE_STATE_NO_PACK,       /* SD mounted but /sdcard/tiles missing */
    MAP_TILE_STATE_NO_ZOOM,       /* pack exists but not the wanted zoom */
    MAP_TILE_STATE_NO_COVERAGE,   /* zoom exists but no tile files at this x,y */
    MAP_TILE_STATE_DECODE_FAIL,   /* tile files present at this x,y but none decoded */
} map_tile_state_t;

typedef struct {
    bool renderer_ok;   /* map_tiles_init succeeded */
    bool home_set;      /* the app has a home position to project onto */
    bool sd_mounted;    /* /sdcard exists on the host filesystem */
    bool pack_present;  /* /sdcard/tiles directory is present */
    bool zoom_present;  /* /sdcard/tiles/<z> is present */
    int  files_present; /* how many of the 3x3 tiles exist as JPEG files */
    int  drawn;         /* how many of those actually decoded and were shown */
} map_tile_probe_t;

/* Pick the state that best explains what the user sees.  Order matters: the
   most upstream reason wins so INIT_FAIL trumps NO_SD trumps NO_PACK, etc.,
   and OK only wins when at least one tile drew (partial coverage still
   counts as OK - the user is looking at real map). */
map_tile_state_t map_tile_classify(const map_tile_probe_t *p);

/* One-line label text for the overlay.  Kept short because it lives inside
   the plot corner and must not eat the aircraft rings. */
const char *map_tile_state_text(map_tile_state_t s);

#ifdef __cplusplus
}
#endif

#endif
