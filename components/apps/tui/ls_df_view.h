/* FIND's other two pictures of the same sweeps.

   RADAR looks down on the board, its top up: each channel's estimate is a
   cone as wide as its spread and as long as it stands over its noise, the
   shown channel's lobe lies under them, and hits are blips. The green line
   is where the board points while a receiver is heard, and the green glow
   the directions heard in the last few seconds: nothing on it moves unless
   the board or the signal does. HEAT is a grid
   of bearing against channel, or against time for one channel: each cell
   is how far that direction stood over the noise, as a character and a
   colour, so a real source is a column that stays and a false one is a
   speck that does not.

   Both are drawn in the character grid; neither touches a radio. */

#ifndef LS_DF_VIEW_H
#define LS_DF_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include "ls_compass_art.h"
#include "ls_df.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DF_VIEW_SCALE_DB 30.0f      /* the outer ring, and the top of HEAT's ramp */

typedef struct {
    bool valid;
    float bearing, spread;             /* true degrees */
    float over_db;                     /* how far it stands over its noise */
    uint8_t colour;
    char tag;
    bool shown;                        /* the channel whose lobe is drawn */
} ls_df_cone_t;

typedef struct {
    float heading;                     /* true heading of the board's top, NAN none */
    bool facing;                       /* a receiver is being heard now */
    const float *recent;               /* 72 levels 0..1: how lately each bearing was heard */
    int cones;
    ls_df_cone_t cone[16];
    const float *lobe;                 /* 72 levels 0..1 by true bearing, or NULL */
    const ls_compass_blip_t *blips; int blip_count;
    float pick;                        /* a picked hit's bearing, NAN none */
} ls_df_radar_t;

void ls_df_radar_draw(tui_surface *sf, tui_rect area, const ls_df_radar_t *r);

/* A heat grid: rows of LS_DF_BINS cells, 0 unheard, else 1 + dB over the
   noise scaled so 255 is LS_DF_VIEW_SCALE_DB. */
typedef struct {
    const char *title;
    int rows;
    const uint8_t *cells;              /* rows * LS_DF_BINS */
    const char *const *labels;         /* one per row */
    int selected;                      /* row drawn marked, -1 none */
    float heading;                     /* true, for the caret, NAN none */
    float pick;
} ls_df_heat_t;

/* Draws into `area` and returns the rows used. */
int ls_df_heat_draw(tui_surface *sf, tui_rect area, const ls_df_heat_t *h);
/* dB over the noise as a heat cell. */
uint8_t ls_df_heat_cell(float over_db, bool heard);

#ifdef __cplusplus
}
#endif
#endif
