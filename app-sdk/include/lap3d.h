/* lap3d.h: 3D in dots, the way COMPASS and DICE draw (API 1.2).
 *
 * The engine lives in the firmware, compiled for speed, and an app calls it
 * through this header; it costs an app no code size. It draws into the same
 * 2 x 3 dots per cell that lap_dots shows: every surface is lit, then shaded
 * along its material's ramp of theme colours with an ordered dither, so a
 * picture follows the board's colour theme and keeps sharp edges.
 *
 *     static l3_scene *sc;
 *     static int steel;
 *     first draw: sc = l3_create(lap_cols(), lap_rows());
 *            steel = l3_material(sc, L3_RAMP_METAL, 4, 0.6f, 0);
 *            l3_camera(sc, l3v(0, 2, 6), l3v(0, 0, 0), 40);
 *     draw:  l3_begin(sc);
 *            l3_floor(sc, 0, grid);
 *            l3_sphere(sc, l3v(0, 1, 0), 1, steel);
 *            l3_end(sc);
 *            l3_show(sc, 0, 0);
 *     leave: l3_destroy(sc);
 *
 * A frame is a list: draw calls between l3_begin and l3_end are recorded,
 * and l3_end renders them. That lets the engine skip what did not change,
 * split the work over both cores, and keep every call cheap for the app.
 *
 * World axes: +x right, +y up, +z toward the viewer of a camera at +z.
 * Units are whatever the app likes; the camera's distances set the scale.
 * Dots are not square on every board; the engine knows their shape
 * (lap_cell_px), so a sphere is round on every screen.
 */
#ifndef LAP3D_H
#define LAP3D_H

#include "lakeshark_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- maths --------------------------------------------------------------- */

typedef struct { float x, y, z; } l3_vec;
typedef struct { float x, y, z, w; } l3_quat;
/* An affine transform: rotation/scale in m[0..8] row by row, then the
   translation in m[9..11]. p' = M p + t. */
typedef struct { float m[12]; } l3_xform;

static inline l3_vec l3v(float x, float y, float z) { l3_vec v = { x, y, z }; return v; }
static inline l3_vec l3_add(l3_vec a, l3_vec b) { return l3v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline l3_vec l3_sub(l3_vec a, l3_vec b) { return l3v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline l3_vec l3_scale(l3_vec a, float k) { return l3v(a.x * k, a.y * k, a.z * k); }
static inline float  l3_dot(l3_vec a, l3_vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline l3_vec l3_cross(l3_vec a, l3_vec b)
{
    return l3v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

l3_vec   l3_norm(l3_vec a);                              /* unit length; zero stays zero */
l3_xform l3_identity(void);
l3_xform l3_pose(l3_vec pos, l3_quat rot, float scale);  /* scale, then rotate, then move */
l3_xform l3_mul(const l3_xform *a, const l3_xform *b);   /* a after b */
l3_vec   l3_apply(const l3_xform *t, l3_vec p);
l3_quat  l3_quat_axis(l3_vec axis, float deg);
l3_quat  l3_quat_euler(float yaw_deg, float pitch_deg, float roll_deg);
l3_quat  l3_quat_mul(l3_quat a, l3_quat b);              /* a after b */
l3_quat  l3_quat_spin(l3_quat q, l3_vec omega, float dt); /* integrate angular velocity (rad/s) */
l3_quat  l3_quat_slerp(l3_quat a, l3_quat b, float t);
l3_vec   l3_quat_rotate(l3_quat q, l3_vec v);

/* ---- the scene ----------------------------------------------------------- */

typedef struct l3_scene l3_scene;       /* the firmware's; opaque */

/* A scene draws into cells_w x cells_h cells (cells*2 x cells*3 dots).
   NULL when out of memory or dimensions are invalid. Create on the first
   draw (lap_cols/lap_rows are drawing APIs), before l3_begin. Free in leave(). */
l3_scene *l3_create(int cells_w, int cells_h);
void      l3_destroy(l3_scene *sc);
bool      l3_resize(l3_scene *sc, int cells_w, int cells_h);
/* Between frames only. Defaults: 256 commands, 1024 mesh vertices (per mesh),
   2048 clipped triangles (per view). Buffers never grow in a frame. Overflow is reported
   in stats.dropped. Reserve larger capacities before drawing complex scenes.
   Firmware creates its shared PSRAM-stack worker with the first scene. */
bool      l3_reserve(l3_scene *sc, int commands, int vertices, int triangles);
void      l3_size(const l3_scene *sc, int *dots_w, int *dots_h);

/* Perspective camera at `eye` looking at `target`, +y up, vertical field of
   view in degrees. fov_deg 0 is orthographic, `ortho_h` world units tall. */
void l3_camera(l3_scene *sc, l3_vec eye, l3_vec target, float fov_deg);
void l3_camera_ortho(l3_scene *sc, l3_vec eye, l3_vec target, float ortho_h);
void l3_camera_up(l3_scene *sc, l3_vec up);              /* default +y */
void l3_clip(l3_scene *sc, float near, float far);       /* default 0.05 .. 1000 */

/* One sun and up to four lamps. The sun shines along `dir`. ambient 0..1. */
void l3_sun(l3_scene *sc, l3_vec dir, float strength, float ambient);
bool l3_lamp(l3_scene *sc, int slot, l3_vec pos, float strength, float range);  /* strength 0 = off */
/* Shadows from the sun, cast by everything drawn with L3_CAST onto
   everything (default off: they cost one more pass). */
void l3_shadows(l3_scene *sc, bool on);

/* What is behind everything: a dithered ramp from `bottom` ink to `top` ink
   down the screen (the same ink twice for a flat colour), or LAP_DOT_CLEAR
   to leave those dots clear so an app can draw its own background first
   with l3_dots. */
void l3_sky(l3_scene *sc, uint8_t top, uint8_t bottom);
/* Fog toward `ink` from `start` to `end` distance; end <= start turns it off. */
void l3_fog(l3_scene *sc, uint8_t ink, float start, float end);
/* A one-dot outline in `ink` where objects meet the background or each
   other, the way COMPASS edges its bezel. LAP_DOT_CLEAR turns it off. */
void l3_outline(l3_scene *sc, uint8_t ink);

/* ---- materials ----------------------------------------------------------- */

/* Up to 8 inks, dark to light, as lap_canvas.h's ramps. spec 0..1 adds a
   highlight toward the last ink. A scene holds 32 materials; the number
   returned is the material, -1 when full. */
enum {
    L3_FLAT    = 1 << 0,    /* nearest ink, no dither: posterised */
    L3_UNLIT   = 1 << 1,    /* always the last ink: lamps, screens, text */
    L3_GRAIN   = 1 << 2,    /* a fixed per-dot noise: felt, stone, wood */
    L3_CHECKER = 1 << 3,    /* checks of unit size, alternate ones one step darker */
    L3_GLOW    = 1 << 4,    /* also lights its own outline: neon */
    L3_ROUND   = 1 << 5,    /* texels draw as round dots, crisp at any size: pips, rivets, LEDs */
};
int  l3_material(l3_scene *sc, const uint8_t *inks, int n, float spec, uint32_t flags);
/* A picture on a surface: each texel is a material number (or 0xFF for
   "the object's own material"), so lighting still shades it. Boxes take it
   on every face, floors and quads tile it once per unit. A texture six
   times as wide as tall gives a box a tile a face, in the order +x, -x,
   +y, -y, +z, -z of its own axes: one die, six faces. */
int  l3_texture(l3_scene *sc, int w, int h, const uint8_t *texels);
void l3_material_texture(l3_scene *sc, int material, int texture);
/* With L3_ROUND, how big each texel's dot is, in texels (default 0.42; up to
   1.4, when dots may reach past their own texel - a die's pips). */
void l3_texture_dot(l3_scene *sc, int texture, float radius);

static const uint8_t L3_RAMP_METAL[4] = { LAP_BLUE, 8, LAP_WHITE, 15 };
static const uint8_t L3_RAMP_GREY[4]  = { LAP_BLACK, 8, LAP_WHITE, 15 };
static const uint8_t L3_RAMP_GREEN[4] = { LAP_BLACK, LAP_GREEN, 10, 15 };
static const uint8_t L3_RAMP_FIRE[4]  = { LAP_RED, 9, 11, 15 };
static const uint8_t L3_RAMP_WOOD[4]  = { LAP_BLACK, LAP_RED, LAP_YELLOW, 11 };
static const uint8_t L3_RAMP_SEA[4]   = { LAP_BLACK, LAP_BLUE, 12, 14 };
static const uint8_t L3_RAMP_IVORY[4] = { 8, LAP_WHITE, 11, 15 };

/* ---- a frame ------------------------------------------------------------- */

void l3_begin(l3_scene *sc);
/* Renders what was drawn since l3_begin into the scene's dots. */
void l3_end(l3_scene *sc);
/* Shows the dots over cells x, y onward (lap_dots). */
void l3_show(const l3_scene *sc, int cell_x, int cell_y);
/* The dots themselves, (dots_w x dots_h), after l3_end: draw 2D over them
   (lap_canvas.h works directly on them) before l3_show. */
uint8_t *l3_dots(l3_scene *sc);

/* Every draw call takes the current options, set before it. */
enum {
    L3_CAST    = 1 << 0,    /* casts a sun shadow (with l3_shadows on) */
    L3_STATIC  = 1 << 1,    /* has not moved since last frame: may be reused */
    L3_SMOOTH  = 1 << 2,    /* meshes: shade across faces (vertex normals) */
    L3_TWO_SIDED = 1 << 3,  /* meshes, quads: no back-face culling */
    L3_NO_DEPTH = 1 << 4,   /* draws over everything drawn before it */
    L3_WIRE    = 1 << 5,    /* meshes: only the edges, in the material's brightest ink */
};
void l3_flags(l3_scene *sc, uint32_t flags);
/* A number 1..255 that l3_pick reports for the dots this object covers. */
void l3_tag(l3_scene *sc, int tag);

/* ---- shapes -------------------------------------------------------------- */

/* Curved shapes are traced per dot, so they are exactly round at any size;
   only the dots they can cover on screen are looked at. */
void l3_sphere(l3_scene *sc, l3_vec c, float r, int mat);
void l3_ellipsoid(l3_scene *sc, const l3_xform *t, l3_vec radii, int mat);
void l3_box(l3_scene *sc, const l3_xform *t, l3_vec half, int mat);
void l3_rbox(l3_scene *sc, const l3_xform *t, l3_vec half, float round, int mat);  /* rounded edges: dice */
void l3_cylinder(l3_scene *sc, l3_vec a, l3_vec b, float r, int mat);             /* capped */
void l3_capsule(l3_scene *sc, l3_vec a, l3_vec b, float r, int mat);
void l3_cone(l3_scene *sc, l3_vec a, l3_vec b, float ra, float rb, int mat);      /* frustum, capped */
void l3_torus(l3_scene *sc, const l3_xform *t, float ring, float tube, int mat);  /* in its local xz plane */
void l3_disc(l3_scene *sc, l3_vec c, l3_vec normal, float r, int mat);
/* A floor at height y, without end. */
void l3_floor(l3_scene *sc, float y, int mat);
/* A flat four-sided patch a, b, c, d (in order around it). */
void l3_quad(l3_scene *sc, l3_vec a, l3_vec b, l3_vec c, l3_vec d, int mat);

/* ---- meshes -------------------------------------------------------------- */

/* Triangles, counter-clockwise seen from outside. `mats` gives a material
   per triangle (NULL: all `mat`). The engine keeps a pointer to the arrays
   until l3_end, so they must stay put that long. Static mesh data is
   fingerprinted each frame; changing it invalidates the static cache. */
typedef struct {
    const l3_vec  *v;       /* vertices */
    const uint16_t *idx;    /* three per triangle */
    const uint8_t *mats;    /* per triangle, or NULL */
    int nv, ntri;
} l3_mesh;
void l3_draw_mesh(l3_scene *sc, const l3_mesh *m, const l3_xform *t, int mat);

/* Ready-made meshes, built into memory the app owns (free with l3_mesh_free).
   False when out of memory. */
bool l3_mesh_lathe(l3_mesh *out, const float *rz, int n, int segments);  /* profile (r,y) pairs turned about +y */
bool l3_mesh_extrude(l3_mesh *out, const float *xy, int n, float depth); /* a closed outline pushed along z */
bool l3_mesh_heightmap(l3_mesh *out, const float *h, int w, int d, float cell);  /* terrain on xz */
bool l3_mesh_icosphere(l3_mesh *out, int subdivisions);
/* Buildings for a map: n footprints (xz: x east, z south pairs, counts[i]
   points each, either winding) standing from base[i] up to top[i], walls in
   wall_mat and flat roofs in roof_mat. One mesh holds up to 65,535 corners
   (two a footprint point); split a city across meshes, a tile each. */
bool l3_mesh_prisms(l3_mesh *out, const float *xz, const uint16_t *counts, int n, const float *base,
                    const float *top, int wall_mat, int roof_mat);
void l3_mesh_free(l3_mesh *m);
/* Text in the 3 x 5 dot font of lap_canvas.h as a solid, one unit per font
   dot, `depth` thick, centred on the origin in its xy plane. */
bool l3_mesh_text(l3_mesh *out, const char *s, float depth);

/* Models from Blender or any OBJ: app-sdk/tools/obj2l3.py writes a C file
   with an l3_mesh (and a material index per triangle, from usemtl). */

/* ---- worlds ---------------------------------------------------------------- */

/* Terrain from a height map, drawn a column at a time front to back (the way
   Voxel Space drew Comanche), so a hundred thousand height samples cost about
   what the screen's columns cost. heights is w x d bytes (0..255 times
   height_scale), mats a material per sample or NULL for `mat`. The corner
   sample [0] sits at origin; samples are `cell` apart along +x and +z. It
   writes depth, so shapes, meshes and sprites stand on it; it takes sun and
   shadows like everything else. The arrays must live through l3_end. */
void l3_terrain(l3_scene *sc, const uint8_t *heights, const uint8_t *mats, int w, int d,
                float cell, float height_scale, l3_vec origin, int mat);

/* Walls on a grid, the way Wolfenstein 3D drew them: map is w x d bytes, 0
   for open floor, otherwise a material number + 1 for a solid block `cell`
   wide and `height` tall, its corner [0] at origin. One ray a column, so a
   maze costs the same however big it is. Add l3_floor for floor and ceiling,
   and anything else for what stands in the rooms. The map must live through
   l3_end. */
void l3_walls(l3_scene *sc, const uint8_t *map, int w, int d, float cell, float height,
              l3_vec origin);

/* Many small round things at once - sparks, snow, stars, bubbles - each a
   lit ball `size` across in world units (so they shrink with distance). An
   L3_UNLIT material makes them glow. Positions must live through l3_end. */
void l3_particles(l3_scene *sc, const l3_vec *p, int n, float size, int mat);

/* ---- cameras ------------------------------------------------------------------ */

/* Around a point: yaw 0 looks along -z, pitch up from the horizon, degrees. */
void l3_camera_orbit(l3_scene *sc, l3_vec target, float distance, float yaw_deg, float pitch_deg,
                     float fov_deg);
/* From a point, for walking about: yaw 0 looks along -z, pitch up, degrees. */
void l3_camera_look(l3_scene *sc, l3_vec eye, float yaw_deg, float pitch_deg, float fov_deg);

/* A finger dragged across the glass turns yaw and pitch (degrees per pixel),
   the usual way to look around a model. Returns true while a finger is down. */
static inline bool l3_drag_turn(float *yaw_deg, float *pitch_deg, float speed)
{
    static bool down;
    static int16_t x0, y0;
    static float yaw0, pitch0;
    lap_touch_t f;
    if (!lap_touch(&f) || !f.down) { down = false; return false; }
    if (!down) { down = true; x0 = f.x; y0 = f.y; yaw0 = *yaw_deg; pitch0 = *pitch_deg; }
    *yaw_deg = yaw0 - (f.x - x0) * speed;
    *pitch_deg = pitch0 + (f.y - y0) * speed * 0.6f;
    if (*pitch_deg > 85) *pitch_deg = 85;
    if (*pitch_deg < -85) *pitch_deg = -85;
    return true;
}

/* ---- lines, points, sprites ------------------------------------------------ */

void l3_line(l3_scene *sc, l3_vec a, l3_vec b, uint8_t ink);
void l3_point(l3_scene *sc, l3_vec p, float size_dots, uint8_t ink);
/* Sprite row strings, keys, inks and label text must live through l3_end. */
/* lap_canvas.h-style ASCII art standing upright at p, facing the camera,
   `height` world units tall; keys/inks as lc_sprite. */
void l3_sprite(l3_scene *sc, l3_vec p, float height, const char *const *rows, int nrows,
               const char *keys, const uint8_t *inks);
/* A label in the 3 x 5 font at p, always facing the camera, scale in dots
   (it does not shrink with distance). */
void l3_label(l3_scene *sc, l3_vec p, const char *s, int scale, uint8_t ink, uint8_t edge);

/* ---- asking the picture ---------------------------------------------------- */

/* Where p lands on the dots; false when behind the camera or clipped. */
bool l3_project(const l3_scene *sc, l3_vec p, float *dot_x, float *dot_y);
/* The ray through a dot, for aiming and touch. */
void l3_ray(const l3_scene *sc, float dot_x, float dot_y, l3_vec *origin, l3_vec *dir);
/* The l3_tag of what shows at a dot after l3_end, 0 for nothing. */
int  l3_pick(const l3_scene *sc, int dot_x, int dot_y);
/* The distance from the camera at a dot after l3_end, or a negative number. */
float l3_depth(const l3_scene *sc, int dot_x, int dot_y);

/* A ray through the last frame's scene, for games: what it hits first within
   max_t (shapes, meshes, terrain, walls), where, and which way that faces.
   It does not draw anything and works between frames. Arrays passed to meshes,
   terrain, walls and particles must remain valid until the next l3_begin when
   using this query (through l3_end alone suffices when not querying). Calls on
   one scene are serialized with recording, rendering and other queries. */
typedef struct {
    int tag;                /* the l3_tag of what was hit, 0 if it had none */
    float t;                /* distance along dir (dir need not be unit) */
    l3_vec p, n;            /* the point and the surface's outward normal */
} l3_hit;
bool l3_raycast(const l3_scene *sc, l3_vec origin, l3_vec dir, float max_t, l3_hit *out);

typedef struct {
    uint32_t us_total;      /* l3_end, all of it */
    uint32_t us_shadow, us_raster, us_shade, us_resolve;
    uint32_t dots_shaded;   /* dots actually rendered this frame */
    uint32_t dots_reused;   /* dots kept from the last frame */
    uint32_t triangles, shapes;
    uint32_t dropped;       /* commands/triangles rejected by capacity or invalid input */
    uint8_t  cores;         /* 1 or 2 */
} l3_stats_t;
void l3_stats(const l3_scene *sc, l3_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LAP3D_H */
