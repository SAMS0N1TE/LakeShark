/* lap3d internals: PSRAM recording, geometry and row-band raster jobs. */
#ifndef L3_PRIVATE_H
#define L3_PRIVATE_H
#include "../../../../app-sdk/include/lap3d.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define L3_PI 3.14159265358979323846f
#define L3_INF 1.0e30f
#define L3_TEXTURES 32
#define L3_SHADOW 128
typedef struct {
    int x0, y0, x1, y1;
} l3_rect;
typedef struct {
    uint8_t ink[8], n;
    float spec;
    uint32_t flags;
    int texture;
} l3_mat;
typedef struct {
    int w, h;
    uint8_t *data;
    float dot;                  /* L3_ROUND dot radius in texels */
} l3_tex;
typedef struct {
    float depth;
    uint8_t mat : 5, object : 1;
    uint8_t light, tag, ink;
} l3_pixel;
typedef struct {
    l3_vec eye, right, up, forward;
    float fy, fx, inv_fy, inv_fx, near_z, far_z;
    int w, h;
    bool ortho;
} l3_view;
enum {
    S_SPHERE,
    S_ELLIPSOID,
    S_BOX,
    S_RBOX,
    S_CYLINDER,
    S_CAPSULE,
    S_CONE,
    S_TORUS,
    S_DISC,
    S_FLOOR,
    S_QUAD,
    S_MESH,
    S_TERRAIN,
    S_WALLS,
    S_PARTICLES,
    S_LINE,
    S_POINT,
    S_SPRITE,
    S_LABEL
};
typedef struct {
    int kind, mat;
    uint32_t flags;
    uint8_t tag, ink, edge;
    int scale;
    l3_xform t, inv;
    l3_vec a, b, c, d, half;
    float r, r2, bound, shadow_bound;
    l3_vec center;
    l3_rect rect;
    int first, count;
    l3_mesh mesh;
    const char *text, *keys;
    const char *const *rows;
    const uint8_t *inks;
    int nrows;
    const uint8_t *map, *map_mats;
    const l3_vec *points;
    int map_w, map_d, np;
} l3_cmd;
typedef struct {
    l3_vec p, n;
} l3_vertex;
typedef struct {
    float x, y, z;
    l3_vec p, n;
} l3_corner;
typedef struct {
    l3_corner v[3];
    int32_t x[3], y[3];
    int32_t area;
    float weight[3];
    l3_rect rect;
    int cmd, mat;
    float sun;
    bool flat;
    uint8_t edges;
} l3_tri;
/* Transient visible-surface record; never copied to the static colour cache. */
typedef struct {
    float u, v;
    int16_t nx, ny, nz;
    uint16_t command;
} l3_surface;
/* Narrow neighbour key. Exact depths remain available for threshold ties and
 * the public depth API; quantization never decides an ambiguous outline. */
typedef struct {
    uint16_t depth;
    uint8_t tag, mat;
} l3_key;
typedef struct {
    uint32_t candidates, edges, intersections, steps, writes, lights, shadows, resolves;
    uint32_t triangle_reciprocals, plane_reciprocals;
    uint64_t bytes;
    uint32_t us_raster, us_shade, us_cache, us_resolve;
    uint32_t kind_us[7];
} l3_counts;
typedef struct { l3_vec p; float sun; } l3_lighting;
typedef struct { uint32_t entry; uint16_t first, last; } l3_bin;
/* Full-frame PSRAM visibility layer, gradient normal packed in two bytes. */
typedef struct { float depth; uint16_t command; uint8_t mat; int8_t nx, nz; } l3_land;

typedef struct { float t, u, plane, lo, hi; l3_vec n; int mat; bool hit; } l3_wall_column;
typedef struct { l3_vec p; float x, y, z, rx, ry; l3_rect rect; int cmd; } l3_particle;
typedef struct {
    l3_vec o, dir, p, n, local, ld, lp, ln;
    float hit, u, v, sun, lod;
    int material;
    l3_lighting *lighting;
    bool depth_only, column_query;
    float beam_lo, beam_hi;
    l3_pixel *pixels;
    l3_surface *surface;
    int y0, key_first;
    l3_key *keys;
    bool full_dirty;
#ifdef L3_PROFILE
    l3_counts *counts;
#endif
} l3_work;
struct l3_scene {
    int w, h, ncmd, cmdcap, nvert, vertcap, ntri, tricap, nmat, ntex;
    bool recording, valid, shadows, shadow_pass;
    uint32_t flags, revision, previous_revision;
    uint8_t tag, sky_top, sky_bottom, outline, fog_ink;
    float fog_start, fog_end, sun_strength, ambient;
    l3_vec eye, target, up, sun;
    float fov, ortho_h, near_z, far_z;
    struct {
        l3_vec pos;
        float strength, range;
    } lamp[4];
    l3_mat material[32];
    l3_tex texture[L3_TEXTURES];
    l3_cmd *cmd;
    l3_vertex *vertices;
    l3_tri *tri;
    l3_pixel *pixels, *cache;
    l3_key *keys;
    uint16_t *shadow;
    float shadow_scale, shadow_inv;
    bool shadow_internal;
    size_t lighting_internal;
    l3_lighting *lighting[2];
    l3_land *land;
    l3_bin *bin, *bin_group;
    int bin_count;
    int bin_bands, bin_stride;
    l3_particle *particle;
    int nparticle, ncolumn, columncap;
    l3_wall_column *column;
    l3_vec shape_v[192], shape_n[192];
    uint16_t shape_idx[192 * 3];
    void *scratch[2];
    bool scratch_internal[2];
    uint8_t *dots, *dirty_mask, *previous_mask;
    l3_view camera, view, sunview;
    l3_vec ortho_half;
    l3_rect dirty, previous_dynamic, current_dynamic;
    uint64_t static_hash, previous_hash;
    bool reuse, shadow_valid, shadow_changed;
    bool ordered, cacheable, partial, have_frame;
    bool land_active, walls_first;
    uint8_t lamp_mask[2];
    float lamp_inv[4];
    uint64_t shadow_hash, static_hash_all;
    l3_counts counts[2];
    l3_stats_t stats;
    /* Scratch belongs to the scene, never the TUI stack. */
    l3_work work[2], query;
    l3_counts query_counts;
    l3_corner clip_a[12], clip_b[12];
    l3_vec bounds_min, bounds_max;
    atomic_int next_band, completed;
    atomic_uchar *tile_ready;
    int bands, pass;
    uint32_t band_dots[2];
    size_t internal_bytes, psram_bytes;
    uint64_t frame_bytes;
    uint32_t profile_frames;
    uint64_t profile_us[5], profile_kind[7];
};
static inline float l3_clamp(float v, float a, float b) { return fminf(b, fmaxf(a, v)); }
/* Same representable-int domain as existing planar checker coordinates. */
static inline int l3_ifloor(float v) {
    int i = (int)v;
    return i - (v < (float)i);
}
/* Positive finite squared distance. Two Newton steps keep lamp N.L error
 * below 5 ppm while avoiding a square root and division for each lamp. */
static inline float l3_rsqrt(float x) {
    uint32_t bits; memcpy(&bits,&x,sizeof bits);
    bits = 0x5f375a86u-(bits>>1);
    float r; memcpy(&r,&bits,sizeof r);
    r *= 1.5f-.5f*x*r*r;
    return r*(1.5f-.5f*x*r*r);
}
static inline float l3_length(l3_vec v) { return sqrtf(l3_dot(v, v)); }
static inline l3_vec l3_dir(const l3_xform *t, l3_vec p) {
    return l3v(t->m[0] * p.x + t->m[1] * p.y + t->m[2] * p.z,
               t->m[3] * p.x + t->m[4] * p.y + t->m[5] * p.z,
               t->m[6] * p.x + t->m[7] * p.y + t->m[8] * p.z);
}
static inline l3_vec l3_unit(l3_vec a) {
    float n = l3_length(a);
    return n > 1.0e-20f ? l3_scale(a, 1.0f / n) : l3v(0, 0, 0);
}
static inline l3_vec l3_transform(const l3_xform *t, l3_vec p) {
    return l3_add(l3_dir(t, p), l3v(t->m[9], t->m[10], t->m[11]));
}
static inline l3_vec l3_normal(const l3_xform *inv, l3_vec n) {
    return l3_unit(l3v(inv->m[0] * n.x + inv->m[3] * n.y + inv->m[6] * n.z,
                       inv->m[1] * n.x + inv->m[4] * n.y + inv->m[7] * n.z,
                       inv->m[2] * n.x + inv->m[5] * n.y + inv->m[8] * n.z));
}
/* Internal rays use view z as t. Public l3_ray remains normalized. */
static inline void l3_zray(const l3_view *v, float x, float y, l3_vec *o, l3_vec *d) {
    l3_vec p = l3_add(l3_scale(v->right, (x + 0.5f - v->w * 0.5f) * v->inv_fx),
                      l3_scale(v->up, (v->h * 0.5f - y - 0.5f) * v->inv_fy));
    *o = v->ortho ? l3_add(v->eye, p) : v->eye;
    *d = v->ortho ? v->forward : l3_add(v->forward, p);
}
#ifdef L3_PROFILE
#define L3_COUNT(s, worker, name) ((s)->counts[worker].name++)
#define L3_BYTES(s, worker, n) ((s)->counts[worker].bytes += (size_t)(n))
#else
#define L3_COUNT(s, worker, name) ((void)0)
#define L3_BYTES(s, worker, n) ((void)0)
#endif
/* Raster/shade traffic stays internal unless that worker's allocation fell
 * back to PSRAM. Counts cover logical frame-array accesses, not cache misses. */
#define L3_LOCAL_BYTES(s, worker, n) \
    do { if (!(s)->scratch_internal[worker]) L3_BYTES(s, worker, n); } while (0)
static inline l3_pixel *l3_work_pixel(l3_scene *s, l3_work *w, int x, int y) {
    return w->pixels + (y - w->y0) * s->w + x;
}
static inline uint16_t l3_key_depth(float z) {
    uint32_t bits;
    memcpy(&bits, &z, sizeof bits);
    uint16_t key = (uint16_t)(bits >> 16);
    return z > 0 && !key ? 1 : key;
}
void *l3_alloc(size_t n);
uint64_t l3_clock(void);
bool l3_inverse(l3_xform *out, const l3_xform *in);
void l3_view_setup(l3_view *v, l3_vec eye, l3_vec target, l3_vec up, float fov, float height, int w,
                   int h, float aspect, float near_z, float far_z);
void l3_view_ray(const l3_view *v, float x, float y, l3_vec *o, l3_vec *d);
bool l3_view_project(const l3_view *v, l3_vec p, float *x, float *y);
l3_cmd *l3_record(l3_scene *s, int kind, int mat);
void l3_prepare(l3_scene *s);
void l3_bins_prepare(l3_scene *s);
void l3_terrain_shadow(l3_scene *s, int command);
void l3_columns(l3_scene *s, int x, int worker);
void l3_walls_prepare(l3_scene *s, int command);
void l3_particles_prepare(l3_scene *s, int command);
void l3_flat_prepare(l3_scene *s, int command);
void l3_submit_triangle(l3_scene *s, int command, int mat);
void l3_world_bounds(l3_cmd *c);
bool l3_world_hit(const l3_cmd *c, l3_work *w, float min_t, float max_t);
bool l3_ray_triangle(l3_vec a, l3_vec b, l3_vec c, l3_work *w, float min_t, float max_t, bool two_sided);
void l3_mesh_prepare(l3_scene *s, int command);
bool l3_intersect(const l3_cmd *c, l3_work *w, float min_t, float max_t);
void l3_raster_band(l3_scene *s, int band, int worker);
void l3_run(l3_scene *s, int pass);
void l3_resolve(l3_scene *s);
void l3_resolve_publish(l3_scene *s, int tile, int worker);
void l3_resolve_try(l3_scene *s, int tile, int worker, bool local);
void l3_resolve_band(l3_scene *s, int band, int worker);
void l3_profile_frame(l3_scene *s);
uint16_t l3_glyph(char c);
void l3_worker_acquire(void);
void l3_worker_release(void);
void l3_worker_start(l3_scene *s);
void l3_worker_finish(l3_scene *s);
#endif
