/* The compass, drawn: a gimballed card in a metal bezel, seen from above
   and in front, ray cast into the character grid at two by two dots a
   cell. The card stays level in the world, so tilting the board tilts the
   picture of it. Markers on the bezel show a target, the sun, a locked
   bearing and a direction-finding estimate; a signal lobe can lie on the
   card itself. */

#ifndef LS_COMPASS_ART_H
#define LS_COMPASS_ART_H

#include <stdbool.h>
#include <stdint.h>
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A hit on the card: where it was heard, how loud (0..1) and how old
   (0 new, 1 about to go). */
typedef struct { float bearing, strength, age; } ls_compass_blip_t;
/* Another channel's estimate, and the character it is tagged with. */
typedef struct { float bearing; char tag; } ls_compass_mark_t;

#define LS_COMPASS_BLIPS 16
#define LS_COMPASS_MARKS 16
#define LS_COMPASS_PEAKS 4

/* How the signal lobe is filled inside the card. Its edge is drawn in all
   but OFF; FULL dithers the inside densely in the lobe's colours, LIGHT
   sparsely in dim blue. */
enum { LS_COMPASS_FILL_FULL, LS_COMPASS_FILL_LIGHT, LS_COMPASS_FILL_EDGE, LS_COMPASS_FILL_OFF, LS_COMPASS_FILL_COUNT };

typedef struct {
    bool valid;                 /* false draws the card dimmed and still */
    float heading;              /* degrees the card is turned: 0 is north up */
    float pitch, roll;          /* board tilt, degrees */
    float target;               /* true bearings, NAN when absent */
    float sun;
    float lock;
    float df;                   /* direction-finding estimate */
    float df_spread;
    const float *lobe;          /* 72 levels 0..1 by true bearing, or NULL */
    float seconds;              /* animation clock */
    bool simple;                /* the flat ring of characters instead */
    bool facing;                /* a green line where the board's top points: FIND is hearing now */
    const float *recent;        /* 72 levels 0..1 by true bearing: how lately each was heard */
    const ls_compass_blip_t *blips; int blip_count;
    const ls_compass_mark_t *marks; int mark_count;
    const float *peaks; int peak_count;   /* lesser lobes of the shown channel */
    uint8_t fill;               /* LS_COMPASS_FILL_* */
    bool white_letters;         /* direction letters white; otherwise orange, N red */
} ls_compass_scene_t;

/* The direction letters' colour: N red, the rest orange unless white. */
uint8_t ls_compass_letter_ink(bool north, bool white);

/* One dot of a picture drawn at two by three dots a cell. */
typedef struct { uint8_t colour; bool set; } ls_dot_t;
/* Six dots to a cell: the two most common colours in each cell are kept,
   as a sextant's ink and paper. */
void ls_dots_blit(tui_surface *sf, tui_rect area, const ls_dot_t *dots, int dots_w);

void ls_compass_art_draw(tui_surface *sf, tui_rect area, const ls_compass_scene_t *sc);

/* A damped spring: the card swings a little past and settles, the way a
   card floating in fluid does. Angles in degrees, dt in seconds. */
typedef struct { float angle, velocity; bool started; } ls_compass_spring_t;
float ls_compass_spring_step(ls_compass_spring_t *s, float target, float dt);

#ifdef __cplusplus
}
#endif
#endif
