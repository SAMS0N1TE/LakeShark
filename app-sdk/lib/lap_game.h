/* 2D games, SDK 2.5. Dots, downward y; no allocation or firmware imports.
 * Caller owns every array and its capacity. Drawing clips to the canvas.
 * Valid maps/sheets have arrays covering their declared dimensions. */
#ifndef LAP_GAME_H
#define LAP_GAME_H
#include "lap_canvas.h"
#include "lap_assets.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { LAP_SPRITE_FLIP_X=1, LAP_SPRITE_FLIP_Y=2 };
typedef struct { lap_image_t image; int frame_w, frame_h; } lap_sprite_t;
/* Nearest-neighbour integer scale 1..16, optional 16-entry palette remap.
 * frame runs row-major through the sheet; invalid frames do nothing.
 * Transparency is tested before palette remapping. x/y are top-left dots. */
void lap_sprite_draw(lap_canvas_t *c, const lap_sprite_t *s, int frame,
                      int x, int y, int scale, unsigned flags, const uint8_t *palette);
typedef struct { int first, count, frame; uint32_t period_ms, elapsed; bool loop, done; } lap_animation_t;
/* Initialize with {first,count,0,period_ms,0,loop,false}. Returns sheet frame.
 * A large dt advances arithmetically, with no catch-up loop. */
int lap_animation_step(lap_animation_t *a, uint32_t dt_ms);
typedef struct {
    const lap_sprite_t *sheet;
    const uint16_t *tiles; /* width*height frame IDs; 0xffff is empty */
    const uint8_t *flags;  /* width*height per-cell flags, optional */
    int width,height;
} lap_tilemap_t;
/* Only visible cells are visited. Camera origin can be negative. */
void lap_tilemap_draw(lap_canvas_t *c, const lap_tilemap_t *m, int camera_x, int camera_y);
uint8_t lap_tile_flags(const lap_tilemap_t *m, int tile_x, int tile_y);
typedef struct { float x,y,w,h; } lap_aabb_t;
bool lap_collision_aabb(lap_aabb_t a, lap_aabb_t b); /* touching edges don't overlap */
bool lap_collision_circle(float ax,float ay,float ar,float bx,float by,float br);
/* True on overlap with any cell carrying mask. Outside the map is empty. */
bool lap_collision_tiles(const lap_tilemap_t *m, lap_aabb_t box, uint8_t mask);
typedef struct { float x,y,vx,vy,life; uint8_t ink; } lap_particle_t;
typedef struct { lap_particle_t *items; unsigned capacity; } lap_particles_t;
/* Inserts in a dead slot, false if full; life seconds >0. */
bool lap_particle_emit(lap_particles_t *p, lap_particle_t particle);
void lap_particles_step(lap_particles_t *p, float dt, float gravity);
void lap_particles_draw(lap_canvas_t *c, const lap_particles_t *p, int camera_x,int camera_y);
typedef struct { uint32_t left_ms, duration_ms, seed; int amplitude,x,y; } lap_shake_t;
/* Set left_ms=duration_ms, amplitude in dots, seed !=0 to start. Decays to 0. */
void lap_shake_step(lap_shake_t *s, uint32_t dt_ms);
/* Ordered-dither fade, amount 0..1, to any theme ink. */
void lap_fade(lap_canvas_t *c, uint8_t ink, float amount);
#ifdef __cplusplus
}
#endif
#endif
