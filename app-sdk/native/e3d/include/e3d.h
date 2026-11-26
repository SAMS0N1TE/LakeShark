/*
 * e3d — a small software 3D engine for the ESP32-P4, ported from an old
 * PowerPC demo engine (ReferenceCode/3d.c) and rebuilt for a 16-bit frame.
 *
 * World axes are the original engine's:
 *
 *        y
 *        |   x   (the camera looks along +x: x is depth)
 *        |  /
 *        | /
 *        |/_____ z   (right on screen)
 *
 * What it keeps from the original: the object text format and loader, the
 * move/scale/rotate matrix calls that accumulate into an object
 * (transformObject), the heading/pitch/tilt camera, a point light with
 * ambient + diffuse + specular, Gouraud and flat shading, a floor drawn as
 * the background, planar shadows on the floor, fog, a light glow and
 * wireframe.
 *
 * What is new: objects are instances of shared, immutable meshes (a model
 * matrix instead of rewriting the vertices, so nothing drifts), smooth
 * normals per corner with a crease angle, winding repair at load, near-plane
 * clipping, perspective-correct depth (1/x z-buffer), sub-pixel rasterising
 * and a second rasteriser thread on the other core.
 *
 * A frame:
 *   e3d_begin(e, pixels, stride);       target: RGB565, stride in pixels
 *   e3d_draw(e, &obj);                  any number of objects
 *   e3d_draw_light(e);                  optional glow at the light
 *   e3d_end(e);                         rasterise everything
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Matrices ────────────────────────────────────────────────────────────────
 * v' = M v with v = (x, y, z, 1): row i gives output i, column 3 is the
 * translation. Angles are degrees, as in the original. */
typedef struct { float m[4][4]; } e3d_mat;

void e3d_identity(e3d_mat *m);
void e3d_move(e3d_mat *m, float dx, float dy, float dz);
void e3d_scale(e3d_mat *m, float sx, float sy, float sz);           /* 0 means 1 */
void e3d_rotate(e3d_mat *m, float ax, float ay, float az);          /* X, then Y, then Z */
void e3d_mul(e3d_mat *a, const e3d_mat *b);                         /* a = a * b: b acts first */
void e3d_apply(const e3d_mat *m, float v[3]);                       /* v = M v */
/* Rotate by deg around the axis (ax, ay, az) through the point b. */
void e3d_rotate_about(e3d_mat *m, const float b[3], float ax, float ay, float az, float deg);
/* Scale around the point b (0 means 1, as in the original scaleArbitrary). */
void e3d_scale_about(e3d_mat *m, const float b[3], float sx, float sy, float sz);

/* ── Meshes ──────────────────────────────────────────────────────────────────
 * The original text format, one file holding several objects:
 *   <objects>
 *   per object:  <triangles>
 *                <v0> <v1> <v2> <R> <G> <B>      (one line per triangle)
 *                <vertices>
 *                <x> <y> <z>                     (one line per vertex)
 * Outward normals are (v0 - v1) x (v2 - v1). Meshes are never changed after
 * loading, so any number of objects can share one. */
typedef struct {
    uint16_t v[3];
    uint8_t  r, g, b;
    union { uint8_t texture; uint8_t pad; }; /* 0 for RGB, otherwise a 1-based texture index */
    uint16_t n[3];              /* corner normals: index into nrm[] */
} e3d_tri;

typedef struct {
    uint16_t width, height;
    const uint8_t *indices;
    const uint16_t *palette;     /* 256 RGB565 colors */
    bool masked;                 /* index 255 is transparent */
} e3d_texture;

typedef struct {
    uint16_t width, height;
    const uint8_t *rgb;          /* Row-major RGB888 luxels, width * height * 3 bytes. */
    int32_t min_u, min_v;        /* First luxel in unwrapped texture texels, aligned to 16. */
    const uint16_t *baked_texels; /* Optional borrowed RGB565 cache from e3d_lightmap_bake. */
    const e3d_texture *baked_texture;
} e3d_lightmap;

/* Bake the opaque texture at its original texel resolution. The cache must stay
 * immutable with the texture and lightmap through the end of frames that use it. */
size_t e3d_lightmap_texel_count(const e3d_lightmap *lightmap);
bool e3d_lightmap_bake(e3d_lightmap *lightmap, const e3d_texture *texture,
                      uint16_t *output, size_t output_texels);

typedef struct {
    int      nverts, ntris;
    float  (*pos)[3];
    e3d_tri *tris;
    /* Corner normals, smoothed across edges below the crease angle. Corners
     * with the same vertex and normal share an entry, so the renderer lights
     * each one once. */
    int       nnorm;
    float   (*nrm)[3];
    uint16_t *nrm_vert;          /* the vertex of each entry (in vertex order) */
    int16_t  *qn;                /* nrm in Q14 as x[], y[], z[], each nnorm rounded up to 8 */
    float    center[3];         /* mean of the vertices (the original OBJECT.pos) */
    float    radius;            /* bounding sphere around center */
    bool     open;              /* has holes or edges shared by more than two triangles */
    /* Optional borrowed arrays for textured opaque objects. UVs are in texels.
     * Texture triangles use r as a brightness multiplier, 255 for full color. */
    const float (*uv)[2];
    const e3d_texture *textures;
    uint16_t texture_count;
    /* Optional borrowed array of ntris pointers. A lightmap replaces the texture
     * triangle's r multiplier and is sampled every 16 texels. The array,
     * descriptors and RGB data must remain valid and read-only through e3d_end(). */
    const e3d_lightmap *const *lightmaps;
} e3d_mesh;

/* Parse objects from text in the format above. Returns the count (array in
 * *out, free with e3d_meshes_free), 0 on a format error. */
int  e3d_load_text(const char *text, e3d_mesh **out);
int  e3d_load_file(const char *path, e3d_mesh **out);
void e3d_meshes_free(e3d_mesh *m, int count);

/* Build a mesh in code: create, fill pos[] and tris[].v / .r .g .b, finish.
 * Up to 65535 vertices and 21845 triangles. */
e3d_mesh *e3d_mesh_create(int nverts, int ntris);
void      e3d_mesh_finish(e3d_mesh *m, float crease_deg);  /* normals, bounds, winding */
void      e3d_mesh_free(e3d_mesh *m);
e3d_mesh *e3d_mesh_sphere(float radius, int slices, int stacks, int r, int g, int b);
/* A floor at height y over [x0,x1] x [z0,z1], n x n tiles in two colours. */
e3d_mesh *e3d_mesh_floor(float x0, float z0, float x1, float z1, float y, int n,
                         uint32_t rgb_a, uint32_t rgb_b);

/* ── Objects ─────────────────────────────────────────────────────────────── */
enum {
    E3D_FLOOR        = 1u << 0,   /* background layer: no depth test, receives shadows */
    E3D_DOUBLE_SIDED = 1u << 1,
    E3D_NO_SHADOW    = 1u << 2,
    E3D_FLAT         = 1u << 3,   /* one colour per triangle */
    E3D_WIRE         = 1u << 4,   /* edges only */
    E3D_HIDDEN       = 1u << 5,
    E3D_UNLIT        = 1u << 6,   /* the triangle colours as they are */
};

typedef struct {
    const e3d_mesh *mesh;
    e3d_mat  m;                   /* model -> world */
    uint32_t flags;               /* E3D_* */
    int32_t  color;               /* 0xRRGGBB for the whole object, -1 = mesh colours */
} e3d_object;

void e3d_object_init(e3d_object *o, const e3d_mesh *mesh);
/* World-space transform applied on top of what the object has (transformObject). */
void e3d_transform(e3d_object *o, const e3d_mat *mat);
void e3d_object_pos(const e3d_object *o, float out[3]);   /* current centre */
void e3d_object_color(e3d_object *o, int r, int g, int b);

/* ── Camera, light, renderer ─────────────────────────────────────────────── */
typedef struct {
    float pos[3];
    float heading;                /* around y, from +x towards +z */
    float pitch;                  /* up from the xz plane */
    float tilt;                   /* bank around the view direction */
} e3d_camera;

typedef struct {
    float pos[3];
    float r, g, b;                /* 0..255 */
} e3d_light;

typedef struct {
    float ambient;                /* share of the surface colour lit by nothing */
    float diffuse;
    float specular;               /* white-ish highlight, scaled by the light colour */
    float shininess;              /* whole numbers are fastest */
    float shadow_y;               /* plane the shadows fall on */
    float shadow_dark;            /* 0 = black shadows .. 1 = none */
    float fog_density;            /* 0 = off; fog = exp(-depth * density) */
    uint16_t fog_rgb565;
    uint16_t clear_rgb565;        /* when there is no background image */
    bool  shadows;
    bool  fog_keep_bg;            /* true: pixels nothing was drawn on (a painted sky) keep their
                                     colours; false (default): they get all fog */
} e3d_params;

typedef struct e3d e3d_t;

/* width x height target. threads: 1, or 2 to rasterise half the rows on core 1. */
e3d_t *e3d_create(int width, int height, int threads);
void   e3d_destroy(e3d_t *e);

e3d_camera *e3d_cam(e3d_t *e);
e3d_light  *e3d_light_get(e3d_t *e);
e3d_params *e3d_params_get(e3d_t *e);
/* Relative camera move, as the original transformCamera. */
void e3d_camera_move(e3d_t *e, float dx, float dy, float dz, float dheading, float dpitch, float dtilt);
/* Lens: pixels per unit at depth 1 (the original: width / 42.44 * 35). */
void e3d_set_focal(e3d_t *e, float f);
/* A width x height RGB565 image copied under each frame, or NULL for clear_rgb565. */
void e3d_set_background(e3d_t *e, const uint16_t *img);
/* Or a function that fills row y (w pixels) of the background; it writes into
 * internal SRAM, so it is much faster than an image in PSRAM. Called from both
 * cores at once: it may only write row. */
typedef void (*e3d_row_fn)(uint16_t *row, int y, int w, void *ctx);
void e3d_set_background_rows(e3d_t *e, e3d_row_fn fn, void *ctx);

void e3d_begin(e3d_t *e, uint16_t *pixels, int stride);
void e3d_draw(e3d_t *e, const e3d_object *o);
void e3d_draw_light(e3d_t *e);
void e3d_end(e3d_t *e);

typedef struct {
    int objects, tris_in, tris_drawn, shadow_tris;
    int t_geom_us, t_raster_us;
} e3d_stats;
const e3d_stats *e3d_get_stats(e3d_t *e);

/* 5x7 text for overlays: A-Z 0-9 and . : - / % + space. scale >= 1. */
void e3d_text(uint16_t *pixels, int stride, int w, int h, int x, int y, const char *s,
              uint16_t color, int scale);

#ifdef __cplusplus
}
#endif
