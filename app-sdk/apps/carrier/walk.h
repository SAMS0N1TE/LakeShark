/* WALK: on foot, docked. Out of the seat, down the ramp into the hangar,
 * round your ship on the pad and back aboard: the cockpit, the hold with
 * your cargo in it, a cabin when the hull has seats, the engine room.
 *
 * This is the rules side only: the layout as boxes, movement with
 * collision, and the places you can use. Nothing here draws or reads keys;
 * ui_walk.c does that. One walker, one layout at a time.
 *
 * World units are metres. +y is up, the floor of the place you stand in is
 * y = 0, and the ship's nose points along -z (as the hangar renders it). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lap3d.h"

#define WALK_BOXES 128
#define WALK_SPOTS 16
#define WALK_EYE   1.65f      /* eye above the feet */
#define WALK_BODY  .35f       /* the walker's radius against walls */

typedef enum { WALK_HANGAR, WALK_SHIP } walk_place;

/* What a box is, so the screen can pick its look. */
typedef enum {
    WB_FLOOR, WB_CEILING, WB_WALL, WB_RIB, WB_DOOR, WB_WINDOW, WB_LIGHT,
    WB_SEAT, WB_CONSOLE, WB_CRATE, WB_BUNK, WB_TABLE, WB_ENGINE, WB_PIPE,
    WB_RAMP, WB_PAD, WB_CLAMP, WB_HAZARD, WB_KIND_COUNT
} walk_box_kind;

typedef struct {
    l3_vec c, half;           /* centre and half extents, axis aligned */
    uint8_t kind;             /* walk_box_kind */
    bool solid;               /* blocks the walker */
} walk_box;

/* The ship on the pad in the hangar: drawn by the screen from the hull mesh,
 * blocked here by this footprint (an ellipse on the floor). */
typedef struct {
    l3_vec centre;            /* where the hull is drawn; y = its keel height */
    float half_width, half_length, height;
    float scale;              /* hull mesh units to metres */
} walk_hull;

/* Places to use: stand within reach, face it, press USE. */
typedef enum {
    WS_SEAT,                  /* sit down: back in the cockpit */
    WS_RAMP_DOWN,             /* aboard -> hangar */
    WS_RAMP_UP,               /* hangar -> aboard */
    WS_HOLD,                  /* the cargo: opens MARKET goods */
    WS_CONSOLE,               /* ship's console: opens STATUS */
    WS_ENGINES,               /* engine room: opens SYS / repairs */
    WS_CABIN,                 /* passenger cabin: opens BOARD (passengers) */
    WS_DESK,                  /* hangar desk: opens MARKET */
    WS_ARMOURY,               /* hangar rack: opens MARKET arms */
    WS_YARD,                  /* hangar shipyard terminal: opens MARKET ships */
    WS_KIND_COUNT
} walk_spot_kind;

typedef struct {
    l3_vec p;                 /* where it is (floor height + what you look at) */
    float reach;              /* metres from the walker's feet */
    uint8_t kind;             /* walk_spot_kind */
    char label[20];           /* shown when in reach: "SIT DOWN", "THE HOLD" */
} walk_spot;

typedef struct {
    walk_place place;
    l3_vec feet;              /* y follows the ramp when on it */
    float yaw, pitch;         /* degrees: yaw 0 looks along -z, + turns right; pitch + looks up */
    float bob;                /* 0..1 phase of the step, advances with distance walked */
    int spot;                 /* the spot in reach and in front, or -1 */
    float moved;              /* metres walked this step (for footstep sounds) */
} walk_state;

/* What the layout is built from. */
typedef struct {
    float hull_radius;        /* game_ship_stats.radius */
    int cargo_capacity, cargo_used;
    int seats;                /* passenger seats; 0 = no cabin */
    bool has_engines_room;    /* false for the smallest hulls */
} walk_ship;

/* Builds the layout for a place and puts the walker at its entrance:
 * WALK_SHIP starts in front of the seat facing aft, WALK_HANGAR at the foot of
 * the ramp facing away from the ship. Deterministic for the same input. */
void walk_enter(walk_state *w, walk_place place, const walk_ship *ship);

/* The current layout. */
int walk_boxes(const walk_box **out);
int walk_spots(const walk_spot **out);
const walk_hull *walk_hull_on_pad(void);   /* NULL aboard */

/* One frame. fwd/strafe in -1..1 (stick or keys), turn/look in degrees this
 * frame. Moves at walking pace, slides along walls, never ends inside a solid
 * box or the hull footprint, keeps pitch within +-70. Updates spot and bob. */
void walk_step(walk_state *w, float fwd, float strafe, float turn_deg, float look_deg, float dt);

/* USE: acts on the spot in reach. Ramps change place (and the layout) here.
 * Returns the walk_spot_kind used, or -1 when nothing is in reach. */
int walk_use(walk_state *w, const walk_ship *ship);

/* The eye for the camera. */
l3_vec walk_eye(const walk_state *w);
