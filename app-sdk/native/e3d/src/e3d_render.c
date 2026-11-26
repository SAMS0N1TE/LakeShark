/*
 * e3d renderer: geometry (transform, light, clip, project) into draw lists,
 * then rasterising into an RGB565 target with a 1/x depth buffer (2.30).
 * From the projection on it is fixed point: each vertex is projected once to
 * a screen record, and triangles whose corners all have one are culled and
 * written with integer work only; the rest are clipped in float.
 *
 * Draw lists are layers, drawn in order:
 *   floor   background: drawn without a depth test, writes depth and marks its
 *           pixels in the stencil (1)
 *   shadow  only over unshadowed floor (stencil 1); darkens it, stencil 2
 *   opaque  depth tested (w > z), the objects and wireframes; the floor's depth
 *           hides what is below it
 *   fx      additive glows, depth tested
 *
 * Rasterising is tiled: the frame is cut into TILE_H-row bands, each drawn in
 * a colour + depth + stencil tile in internal SRAM and copied out once, so
 * PSRAM sees one write per pixel. Primitives are binned by band as they are
 * made, so the rasteriser reads only those of its band. Geometry and
 * rasterising both run on the two cores when there are 2 threads: objects are
 * taken from a shared list, then bands, as the threads come for them.
 */
#include "e3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "e3d_port.h"
#include "e3d_mem.h"

#define now_us     e3d_port_time_us
#define alloc_sram e3d_port_alloc_sram
#define alloc_fast e3d_port_alloc_any
#define free_fast  e3d_port_free_aligned

#define SRAM_CHUNKS  8             /* per thread: 8 x 6 KB */
#define SRAM_RESERVE (40 * 1024)
#define DMA_RESERVE  (48 * 1024)   /* of that, DMA-capable: SD card transfers and Wi-Fi buffers */
#define NEAR    2.0f               /* view-space x of the near plane */
#define TILE_H  16
#define FOG_ZN  2048               /* fog factors by 1/x: 6 mantissa bits of each power of two */

enum { M_FLAT, M_GOURAUD, M_FLOOR, M_SHADOW, M_LINE, M_GLOW, M_TEXTURE };
enum { L_FLOOR, L_SHADOW, L_OPAQUE, L_FX, NLAYERS };

/* A primitive in fixed point, 36 bytes, written as 9 words: thousands are
 * written each frame, so they are kept small. Triangles use all three corners,
 * lines two, the glow corner 0 (x[1]: its radius). */
#define SUB   16                   /* screen positions: 12.4 */
#define WONE  1073741824.f         /* depth 1/x: 2.30 */
#define GB    1800.f               /* guard band: corners stay this close to the centre (x < 2048 in 12.4) */
typedef union {
    struct {
        int32_t w[3];
        int16_t x[3], y[3];
        uint8_t r[3], g[3], b[3];
        uint8_t mode;
    };
    uint32_t u[9];
} prim_t;
_Static_assert(sizeof(prim_t) == 36, "prim_t size");

/* A layer's primitives, in chunks taken from the thread's pool. Chunks in
 * internal SRAM come first in the pool: writing a primitive there is several
 * times cheaper than to PSRAM, where every new cache line is read first. */
#define CHUNK   128
#define MAXCH   128
typedef struct {
    prim_t *ch[MAXCH];
    int n;
} plist_t;

static inline const prim_t *prim_at(const plist_t *l, int i) { return &l->ch[i / CHUNK][i % CHUNK]; }

/* the primitives of one layer that touch one band: indices into the layer's list */
typedef struct {
    uint16_t *i;
    int n, cap;
} bin_t;

typedef struct {
    uint16_t *c;
    int32_t *z;                    /* 1/x in 2.30, 0 = nothing */
    uint8_t *s;                    /* stencil: 1 floor, 2 shadowed floor */
    int y0, y1;
} tile_t;

typedef struct {
    float v[3];                    /* view space */
    float c[3];                    /* colour 0..255 */
} cvert;

typedef struct {
    const e3d_texture *image;
    const e3d_lightmap *lightmap;
    float origin[2], dx[2], dy[2]; /* gradients of u/x and v/x */
} tex_gradient;

/* A vertex on the screen, as primitives take it: 1/x in 2.30, x and y in
 * 12.4. Its clip code says where it is; the screen bits only when not C_NEAR,
 * the position only when neither C_NEAR nor C_GB. */
typedef struct {
    int32_t w;
    int16_t x, y;
} sv_t;
enum { C_L = 1, C_R = 2, C_T = 4, C_B = 8, C_GB = 16, C_NEAR = 32, C_OFF = 64 };
#define C_OUT (C_L | C_R | C_T | C_B | C_NEAR)   /* all corners with one of these: nothing to draw */

/* What one geometry thread owns: its draw lists, binned by band as they are
 * made (bins[band * NLAYERS + layer]), and per-object scratch. */
typedef struct {
    plist_t list[NLAYERS];
    prim_t **pool;                 /* chunks: npool_sram in SRAM, then PSRAM ones */
    int npool, npool_sram, pool_cap, pool_used;
    bin_t *bins;
    void *scratch;                 /* holds the arrays below */
    void *lscratch;                /* holds nl */
    float (*vpos)[3];              /* view space */
    sv_t *sv, *ssv;                /* on the screen: the vertices, their shadows */
    uint8_t *clip, *sclip;         /* their clip codes */
    int nvert_cap;
    float (*lt)[2];                /* lit corner normals: kd, ks */
    uint32_t *ltag;
    int16_t *nl;                   /* batched lighting, per normal: L, H in, then colour factors out (SoA) */
    int16_t *nlb;                  /* the same for back faces (the normals turned round) */
    int nnorm_cap;
    tex_gradient *texture_planes;
    int texture_count, texture_capacity;
    uint32_t tag;
    int tris_drawn, shadow_tris;
} geo_t;

/* an object recorded by e3d_draw, drawn in e3d_end */
typedef struct {
    e3d_object o;
    bool shadow;
} job_t;

struct e3d {
    int w, h;
    uint16_t *fb;
    int stride;
    const uint16_t *bg;
    e3d_row_fn bg_fn;
    void *bg_ctx;
    e3d_camera cam;
    e3d_light light;
    e3d_params par;
    float focal;
    e3d_mat view;
    float vlight[3];               /* the light in view space */
    int shin_n;                    /* shininess as a whole number, 0 if it is not */
    bool floor_drawn;              /* shadows need a floor drawn before them */
    geo_t geo[2];
    job_t *jobs;
    int njobs, jobs_cap;
    int next_job;                  /* the next job to take, shared by the threads */
    int next_band;                 /* and the next band to draw */
    int task;                      /* what the worker does on go: 0 geometry, 1 raster */
    int nbands;
    tile_t tile[2];
    e3d_stats st, st_frame;
    int64_t t0;
    uint8_t fog_lut[257];
    uint8_t fog_z[FOG_ZN];         /* the fog factor by depth: see fog_tile */
    float fog_zk;                  /* the density fog_z was made for */
    int threads;
    bool pie_light;                /* the PIE lighting kernel passed its check */
#if E3D_PORT_THREADS
    e3d_port_thread worker;
    e3d_port_sem go, done;
#endif
    volatile bool quit;
};

/* ── Setup ───────────────────────────────────────────────────────────────── */

static void raster(e3d_t *e, int tid);
static void geo_run(e3d_t *e, int tid);
#if defined(__riscv_xesppie)
static bool light_batch_check(void);
#endif

#if E3D_PORT_THREADS
static int worker_main(void *arg)
{
    e3d_t *e = arg;
    /* only e3d_destroy ends it: leaving on an exit request would leave the
     * thread that draws waiting for its half of a frame */
    while (!e->quit) {
        if (!e3d_port_sem_take(e->go, 100))
            continue;
        if (e->quit)
            break;
        if (e->task)
            raster(e, 1);
        else
            geo_run(e, 1);
        e3d_port_sem_give(e->done);
    }
    return 0;
}
#endif

static bool tile_alloc(tile_t *t, int w)
{
    size_t n = (size_t)w * TILE_H;
    t->c = alloc_fast(n * sizeof(uint16_t));
    t->z = alloc_fast(n * sizeof(int32_t));
    t->s = alloc_fast(n);
    return t->c && t->z && t->s;
}

static void tile_free(tile_t *t)
{
    if (t->c) free_fast(t->c);
    if (t->z) free_fast(t->z);
    if (t->s) free_fast(t->s);
    memset(t, 0, sizeof(*t));
}

e3d_t *e3d_create(int width, int height, int threads)
{
    e3d_t *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->w = width;
    e->h = height;
    e->nbands = (height + TILE_H - 1) / TILE_H;
    e->geo[0].bins = calloc((size_t)e->nbands * NLAYERS, sizeof(bin_t));
    e->geo[1].bins = calloc((size_t)e->nbands * NLAYERS, sizeof(bin_t));
    if (!e->geo[0].bins || !e->geo[1].bins || !tile_alloc(&e->tile[0], width)) {
        e3d_destroy(e);
        return NULL;
    }
    e->focal = (float)width / 42.44f * 35.f;
    e->par = (e3d_params){
        .ambient = 0.35f, .diffuse = 0.95f, .specular = 0.85f, .shininess = 24.f,
        .shadow_y = 1.f, .shadow_dark = 0.45f, .fog_density = 0.f,
        .fog_rgb565 = 0xA514, .clear_rgb565 = 0, .shadows = true,
    };
    e->light = (e3d_light){ .pos = { 0, 1000, 0 }, .r = 240, .g = 240, .b = 220 };
    for (int i = 0; i <= 256; i++)   /* fog factor for depth * density * 32 = i */
        e->fog_lut[i] = (uint8_t)(expf(-(float)i / 32.f) * 32.f + 0.5f);
    e->threads = 1;
#if defined(__riscv_xesppie)
    e->pie_light = light_batch_check();
#endif
#if E3D_PORT_THREADS
    if (threads > 1 && e3d_port_threads_available() && tile_alloc(&e->tile[1], width)) {
        e->go = e3d_port_sem_create();
        e->done = e3d_port_sem_create();
        if (e->go && e->done)
            e->worker = e3d_port_thread_start(worker_main, e, 1, 8192);
        if (e->worker)
            e->threads = 2;
    }
#else
    (void)threads;
#endif
    return e;
}

void e3d_destroy(e3d_t *e)
{
    if (!e)
        return;
#if E3D_PORT_THREADS
    if (e->worker) {
        e->quit = true;
        e3d_port_sem_give(e->go);
        e3d_port_thread_join(e->worker);
    }
    if (e->go) e3d_port_sem_destroy(e->go);
    if (e->done) e3d_port_sem_destroy(e->done);
#endif
    for (int k = 0; k < 2; k++) {
        geo_t *g = &e->geo[k];
        for (int i = 0; i < g->npool; i++) {
            if (i < g->npool_sram)
                free_fast(g->pool[i]);
            else
                free(g->pool[i]);
        }
        free(g->pool);
        if (g->bins)
            for (int i = 0; i < e->nbands * NLAYERS; i++)
                if (g->bins[i].i)
                    free_fast(g->bins[i].i);
        free(g->bins);
        free(g->scratch);
        free(g->lscratch);
        free(g->texture_planes);
    }
    free(e->jobs);
    tile_free(&e->tile[0]);
    tile_free(&e->tile[1]);
    free(e);
}

e3d_camera *e3d_cam(e3d_t *e) { return &e->cam; }
e3d_light *e3d_light_get(e3d_t *e) { return &e->light; }
e3d_params *e3d_params_get(e3d_t *e) { return &e->par; }
void e3d_set_focal(e3d_t *e, float f) { e->focal = f; }
void e3d_set_background(e3d_t *e, const uint16_t *img) { e->bg = img; e->bg_fn = NULL; }
void e3d_set_background_rows(e3d_t *e, e3d_row_fn fn, void *ctx) { e->bg_fn = fn; e->bg_ctx = ctx; e->bg = NULL; }
const e3d_stats *e3d_get_stats(e3d_t *e) { return &e->st; }

void e3d_camera_move(e3d_t *e, float dx, float dy, float dz, float dh, float dp, float dt)
{
    e->cam.pos[0] += dx;
    e->cam.pos[1] += dy;
    e->cam.pos[2] += dz;
    e->cam.heading = fmodf(e->cam.heading + dh, 360.f);
    e->cam.pitch = fmodf(e->cam.pitch + dp, 360.f);
    e->cam.tilt = fmodf(e->cam.tilt + dt, 360.f);
}

/* chunk c into the pool: at the end, or after the SRAM ones when sram */
static bool pool_add(geo_t *g, prim_t *c, bool sram)
{
    if (g->npool == g->pool_cap) {
        int cap = g->pool_cap ? g->pool_cap * 2 : 16;
        prim_t **q = realloc(g->pool, sizeof(*q) * (size_t)cap);
        if (!q)
            return false;
        g->pool = q;
        g->pool_cap = cap;
    }
    int at = sram ? g->npool_sram++ : g->npool;
    memmove(&g->pool[at + 1], &g->pool[at], sizeof(*g->pool) * (size_t)(g->npool - at));
    g->pool[at] = c;
    g->npool++;
    return true;
}

/* the first primitive of a new chunk */
static prim_t *push_chunk(geo_t *g, plist_t *l)
{
    int c = l->n / CHUNK;
    if (c == MAXCH)
        return NULL;
    if (g->pool_used == g->npool) {
        prim_t *q = malloc(sizeof(prim_t) * CHUNK);
        if (!q)
            return NULL;
        if (!pool_add(g, q, false)) {
            free(q);
            return NULL;
        }
    }
    l->ch[c] = g->pool[g->pool_used++];
    l->n++;
    return l->ch[c];
}

static inline prim_t *push(geo_t *g, int layer)
{
    plist_t *l = &g->list[layer];
    unsigned n = (unsigned)l->n;
    if (__builtin_expect(n % CHUNK == 0, 0))
        return push_chunk(g, l);
    l->n = (int)n + 1;
    return &l->ch[n / CHUNK][n % CHUNK];
}

/* bins are small and hot: SRAM when it can */
static bool bin_grow(bin_t *bin)
{
    int cap = bin->cap ? bin->cap * 2 : 64;
    uint16_t *q = alloc_fast(sizeof(*q) * (size_t)cap);
    if (!q)
        return false;
    if (bin->i) {
        memcpy(q, bin->i, sizeof(*q) * (size_t)bin->n);
        free_fast(bin->i);
    }
    bin->i = q;
    bin->cap = cap;
    return true;
}

/* p (the last one pushed to the layer) into the bins of the bands that rows
 * [y0, y1) touch (0 <= y0, y1 <= h) */
static inline void bin_prim(const e3d_t *e, geo_t *g, int layer, const prim_t *p, int y0, int y1)
{
    (void)p;
    if (y1 <= y0)
        return;
    uint16_t idx = (uint16_t)(g->list[layer].n - 1);
    unsigned b = (unsigned)y0 / TILE_H, b1 = (unsigned)(y1 - 1) / TILE_H;
    if (b1 >= (unsigned)e->nbands)
        b1 = (unsigned)e->nbands - 1;
    bin_t *bin = &g->bins[b * NLAYERS + layer];
    for (; b <= b1; b++, bin += NLAYERS) {
        if (__builtin_expect(bin->n == bin->cap, 0) && !bin_grow(bin))
            continue;
        bin->i[bin->n++] = idx;
    }
}

void e3d_begin(e3d_t *e, uint16_t *pixels, int stride)
{
    e->fb = pixels;
    e->stride = stride;
#ifndef E3D_HOST
    /* One more SRAM chunk a frame, for the thread with fewer, while the
     * internal heap keeps SRAM_RESERVE free for the launcher and the app, and
     * DMA_RESERVE of it DMA-capable (without it, SD card reads fail). */
    geo_t *gs = &e->geo[e->threads == 2 && e->geo[1].npool_sram < e->geo[0].npool_sram];
    if (gs->npool_sram < SRAM_CHUNKS &&
        e3d_port_sram_free() > SRAM_RESERVE + sizeof(prim_t) * CHUNK &&
        e3d_port_dma_free() > DMA_RESERVE + sizeof(prim_t) * CHUNK) {
        prim_t *c = alloc_sram(sizeof(prim_t) * CHUNK);
        if (c && !pool_add(gs, c, true))
            free_fast(c);
    }
#endif
    for (int k = 0; k < 2; k++) {
        e->geo[k].pool_used = 0;
        e->geo[k].texture_count = 0;
        for (int i = 0; i < NLAYERS; i++)
            e->geo[k].list[i].n = 0;
        e->geo[k].tris_drawn = e->geo[k].shadow_tris = 0;
        for (int i = 0; i < e->nbands * NLAYERS; i++)
            e->geo[k].bins[i].n = 0;
    }
    e->njobs = 0;
    memset(&e->st_frame, 0, sizeof(e->st_frame));
    e->floor_drawn = false;
    e->t0 = now_us();
    int n = (int)e->par.shininess;
    e->shin_n = ((float)n == e->par.shininess && n > 0 && n <= 1024) ? n : 0;

    /* the original's view: tilt(x) * pitch(z) * heading(y) * move(-pos) */
    e3d_mat r;
    e3d_move(&e->view, -e->cam.pos[0], -e->cam.pos[1], -e->cam.pos[2]);
    e3d_rotate(&r, 0, -e->cam.heading, 0);
    e3d_mul(&r, &e->view);
    e->view = r;
    e3d_rotate(&r, 0, 0, -e->cam.pitch);
    e3d_mul(&r, &e->view);
    e->view = r;
    e3d_rotate(&r, -e->cam.tilt, 0, 0);
    e3d_mul(&r, &e->view);
    e->view = r;
}

/* ── Geometry ────────────────────────────────────────────────────────────── */

static inline float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* 1/sqrt(x) to about 0.2%: no fsqrt or fdiv, which are slow on the P4's FPU */
static inline float rsqrt_fast(float x)
{
    union { float f; uint32_t i; } u = { x };
    u.i = 0x5f375a86u - (u.i >> 1);
    return u.f * (1.5f - 0.5f * x * u.f * u.f);
}

static inline void norm3(float v[3])
{
    float l = dot3(v, v);
    if (l > 0.f) {
        l = rsqrt_fast(l);
        v[0] *= l; v[1] *= l; v[2] *= l;
    }
}

static inline float clampc(float c) { return c < 0.5f ? 0.5f : c > 254.5f ? 254.5f : c; }

static inline float ipow(float s, int n)
{
    float r = 1.f;
    for (; n; n >>= 1, s *= s)
        if (n & 1)
            r *= s;
    return r;
}

/* Lighting at a point with unit normal n, as two factors so it can be cached per
 * corner and used with any surface colour:
 *   colour = base * (ambient + kd * light) + ks * light
 * kd: diffuse (n.L), ks: Blinn-Phong highlight. */
E3D_FAST static void light_terms(const e3d_t *e, const float p[3], const float n[3], float *kd, float *ks)
{
    /* in view space: the camera is at the origin */
    const float *lp = e->vlight;
    float L[3] = { lp[0] - p[0], lp[1] - p[1], lp[2] - p[2] };
    float V[3] = { -p[0], -p[1], -p[2] };
    norm3(L);
    norm3(V);
    float d = dot3(n, L);
    if (d <= 0.f) {
        *kd = *ks = 0.f;
        return;
    }
    float H[3] = { L[0] + V[0], L[1] + V[1], L[2] + V[2] };
    norm3(H);
    float s = dot3(n, H);
    *kd = d * e->par.diffuse * (1.f / 255.f);
    *ks = s <= 0.f ? 0.f : (e->shin_n ? ipow(s, e->shin_n) : powf(s, e->par.shininess)) * e->par.specular;
}

static inline void lit_color(const e3d_t *e, const float base[3], float kd, float ks, float out[3])
{
    out[0] = clampc(base[0] * (e->par.ambient + kd * e->light.r) + ks * e->light.r);
    out[1] = clampc(base[1] * (e->par.ambient + kd * e->light.g) + ks * e->light.g);
    out[2] = clampc(base[2] * (e->par.ambient + kd * e->light.b) + ks * e->light.b);
}

static inline float clampxy(float v) { return v < -1e6f ? -1e6f : v > 1e6f ? 1e6f : v; }

static inline int16_t row_clamp(float y, int h)
{
    return (int16_t)(y < 0.f ? 0 : y > (float)h ? h : (int)y);
}

/* view space (x >= NEAR) -> screen x, y and 1/x */
static inline void project_vert(const e3d_t *e, const float v[3], float out[3])
{
    float w = 1.f / v[0];
    out[0] = clampxy((float)e->w * 0.5f + v[2] * w * e->focal);
    out[1] = clampxy((float)e->h * 0.5f - v[1] * w * e->focal);
    out[2] = w;
}

/* screen position in 12.4, rounded (|v| < GB, so the bias keeps it positive) */
static inline int32_t fix4(float v) { return (int32_t)(v * (float)SUB + 32768.5f) - 32768; }

/* the corners of one primitive, packed into its 9 words: 9 stores, not 19 */
static inline void prim_write(prim_t *p, int mode, const int32_t w[3], const int32_t x[3], const int32_t y[3],
                              const uint8_t rgb[3][3])
{
    uint32_t *u = p->u;
    u[0] = (uint32_t)w[0];
    u[1] = (uint32_t)w[1];
    u[2] = (uint32_t)w[2];
    u[3] = (uint16_t)x[0] | (uint32_t)x[1] << 16;
    u[4] = (uint16_t)x[2] | (uint32_t)y[0] << 16;
    u[5] = (uint16_t)y[1] | (uint32_t)y[2] << 16;
    u[6] = rgb[0][0] | (uint32_t)rgb[1][0] << 8 | (uint32_t)rgb[2][0] << 16 | (uint32_t)rgb[0][1] << 24;
    u[7] = rgb[1][1] | (uint32_t)rgb[2][1] << 8 | (uint32_t)rgb[0][2] << 16 | (uint32_t)rgb[1][2] << 24;
    u[8] = rgb[2][2] | (uint32_t)mode << 8;
}

static void push_gb(e3d_t *e, geo_t *g, int layer, int mode, const float *s[3], const uint8_t rgb[3][3]);

/* a projected triangle (corners s: x, y, 1/x) with corner colours rgb into the layer */
E3D_FAST static void push_proj(e3d_t *e, geo_t *g, int layer, int mode, const float *s0, const float *s1,
                               const float *s2, const uint8_t rgb[3][3])
{
    const float *s[3] = { s0, s1, s2 };
    if ((s0[0] < 0 && s1[0] < 0 && s2[0] < 0) || (s0[1] < 0 && s1[1] < 0 && s2[1] < 0) ||
        (s0[0] > e->w && s1[0] > e->w && s2[0] > e->w) || (s0[1] > e->h && s1[1] > e->h && s2[1] > e->h))
        return;
    float cx = (float)e->w * 0.5f, cy = (float)e->h * 0.5f;
    for (int i = 0; i < 3; i++)
        if (fabsf(s[i][0] - cx) > GB || fabsf(s[i][1] - cy) > GB) {
            push_gb(e, g, layer, mode, s, rgb);
            return;
        }
    prim_t *p = push(g, layer);
    if (!p)
        return;
    int32_t w[3], x[3], y[3];
    float lo = s0[1], hi = s0[1];
    for (int i = 0; i < 3; i++) {
        w[i] = (int32_t)(s[i][2] * WONE);
        x[i] = fix4(s[i][0]);
        y[i] = fix4(s[i][1]);
        lo = s[i][1] < lo ? s[i][1] : lo;
        hi = s[i][1] > hi ? s[i][1] : hi;
    }
    prim_write(p, mode, w, x, y, rgb);
    bin_prim(e, g, layer, p, row_clamp(lo - 1.f, e->h), row_clamp(hi + 2.f, e->h));
    if (layer == L_SHADOW)
        g->shadow_tris++;
    else
        g->tris_drawn++;
}

/* a triangle whose corners are all on the screen records (no clipping needed) */
E3D_FAST static void push_sv(e3d_t *e, geo_t *g, int layer, int mode, const sv_t *a, const sv_t *b, const sv_t *c,
                             const uint8_t rgb[3][3])
{
    /* the rows whose pixel centres it covers, as raster_tri takes them */
    int lo = a->y < b->y ? a->y : b->y, hi = a->y < b->y ? b->y : a->y;
    lo = c->y < lo ? c->y : lo;
    hi = c->y > hi ? c->y : hi;
    lo = (lo + 7) >> 4;
    hi = (hi + 7) >> 4;
    lo = lo < 0 ? 0 : lo;
    hi = hi > e->h ? e->h : hi;
    if (lo >= hi)
        return;
    prim_t *p = push(g, layer);
    if (!p)
        return;
    int32_t w[3] = { a->w, b->w, c->w }, x[3] = { a->x, b->x, c->x }, y[3] = { a->y, b->y, c->y };
    prim_write(p, mode, w, x, y, rgb);
    bin_prim(e, g, layer, p, lo, hi);
    if (layer == L_SHADOW)
        g->shadow_tris++;
    else
        g->tris_drawn++;
}

/* a line between two vertices on the screen records */
E3D_FAST static void push_line_sv(e3d_t *e, geo_t *g, const sv_t *a, const sv_t *b, const uint8_t ca[3],
                                  const uint8_t cb[3])
{
    int lo = a->y < b->y ? a->y : b->y, hi = a->y < b->y ? b->y : a->y;
    lo = (lo >> 4) - 1;
    hi = (hi >> 4) + 2;
    lo = lo < 0 ? 0 : lo;
    hi = hi > e->h ? e->h : hi;
    if (lo >= hi)
        return;
    prim_t *p = push(g, L_OPAQUE);
    if (!p)
        return;
    int32_t w[3] = { a->w, b->w, 0 }, x[3] = { a->x, b->x, 0 }, y[3] = { a->y, b->y, 0 };
    uint8_t rgb[3][3] = { { ca[0], ca[1], ca[2] }, { cb[0], cb[1], cb[2] }, { 0 } };
    prim_write(p, M_LINE, w, x, y, rgb);
    bin_prim(e, g, L_OPAQUE, p, lo, hi);
}

/* twice the signed screen area: > 0 when the corners turn the way of a front face */
static inline int64_t sv_area(const sv_t *a, const sv_t *b, const sv_t *c)
{
    return (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(c->x - a->x) * (b->y - a->y);
}

/* the clip code of a screen point (no branches) */
static inline int screen_code(const e3d_t *e, float x, float y)
{
    float cx = (float)e->w * 0.5f, cy = (float)e->h * 0.5f;
    return (x < 0.f) | (x > (float)e->w) << 1 | (y < 0.f) << 2 | (y > (float)e->h) << 3 |
           ((fabsf(x - cx) > GB) | (fabsf(y - cy) > GB)) << 4;
}

/* A triangle with a corner too far off screen for 12.4: clipped to the guard
 * band in screen space, where 1/x and the colours are linear, then fanned. */
typedef struct { float x, y, w, c[3]; } gbv;

static int gb_clip(const gbv *in, int n, gbv *out, int axis, float lim, float sgn)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const gbv *a = &in[i], *b = &in[(i + 1) % n];
        float da = sgn * ((axis ? a->y : a->x) - lim), db = sgn * ((axis ? b->y : b->x) - lim);
        if (da <= 0.f)
            out[m++] = *a;
        if ((da <= 0.f) != (db <= 0.f)) {
            float t = da / (da - db);
            gbv *o = &out[m++];
            o->x = a->x + (b->x - a->x) * t;
            o->y = a->y + (b->y - a->y) * t;
            o->w = a->w + (b->w - a->w) * t;
            for (int k = 0; k < 3; k++)
                o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
        }
    }
    return m;
}

static void push_gb(e3d_t *e, geo_t *g, int layer, int mode, const float *s[3], const uint8_t rgb[3][3])
{
    gbv a[9], b[9];
    for (int i = 0; i < 3; i++) {
        a[i] = (gbv){ s[i][0], s[i][1], s[i][2], { rgb[i][0], rgb[i][1], rgb[i][2] } };
    }
    float cx = (float)e->w * 0.5f, cy = (float)e->h * 0.5f, lim = GB - 2.f;
    int n = gb_clip(a, 3, b, 0, cx + lim, 1.f);
    n = gb_clip(b, n, a, 0, cx - lim, -1.f);
    n = gb_clip(a, n, b, 1, cy + lim, 1.f);
    n = gb_clip(b, n, a, 1, cy - lim, -1.f);
    for (int i = 1; i + 1 < n; i++) {
        const gbv *v[3] = { &a[0], &a[i], &a[i + 1] };
        float q[3][3];
        uint8_t c[3][3];
        for (int k = 0; k < 3; k++) {
            q[k][0] = v[k]->x;
            q[k][1] = v[k]->y;
            q[k][2] = v[k]->w;
            for (int j = 0; j < 3; j++) {
                float f = v[k]->c[j] + 0.5f;
                c[k][j] = (uint8_t)(f < 0.f ? 0.f : f > 255.f ? 255.f : f);
            }
        }
        push_proj(e, g, layer, mode, q[0], q[1], q[2], c);
    }
}

static void project_push(e3d_t *e, geo_t *g, int layer, int mode, const cvert *a, const cvert *b, const cvert *c)
{
    float s[3][3];
    const cvert *v[3] = { a, b, c };
    uint8_t rgb[3][3];
    for (int i = 0; i < 3; i++) {
        project_vert(e, v[i]->v, s[i]);
        for (int q = 0; q < 3; q++)
            rgb[i][q] = (uint8_t)(v[i]->c[q] + 0.5f);
    }
    push_proj(e, g, layer, mode, s[0], s[1], s[2], rgb);
}

static void lerp_cv(const cvert *a, const cvert *b, float t, cvert *o)
{
    for (int k = 0; k < 3; k++) {
        o->v[k] = a->v[k] + (b->v[k] - a->v[k]) * t;
        o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
    }
}

/* clip against the near plane x = NEAR, then fan */
E3D_FAST static void emit_tri(e3d_t *e, geo_t *g, int layer, int mode, const cvert in[3])
{
    int inside = (in[0].v[0] >= NEAR) + (in[1].v[0] >= NEAR) + (in[2].v[0] >= NEAR);
    if (inside == 3) {
        project_push(e, g, layer, mode, &in[0], &in[1], &in[2]);
        return;
    }
    if (inside == 0)
        return;
    cvert out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const cvert *a = &in[i], *b = &in[(i + 1) % 3];
        bool ia = a->v[0] >= NEAR, ib = b->v[0] >= NEAR;
        if (ia)
            out[n++] = *a;
        if (ia != ib)
            lerp_cv(a, b, (NEAR - a->v[0]) / (b->v[0] - a->v[0]), &out[n++]);
    }
    for (int i = 1; i + 1 < n; i++)
        project_push(e, g, layer, mode, &out[0], &out[i], &out[i + 1]);
}

static void emit_texture(e3d_t *e, geo_t *g, const cvert in[3],
                         const e3d_texture *image, uint8_t brightness,
                         const e3d_lightmap *lightmap)
{
    cvert clipped[4];
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        const cvert *a = &in[i], *b = &in[(i + 1) % 3];
        bool inside_a = a->v[0] >= NEAR, inside_b = b->v[0] >= NEAR;
        if (inside_a) clipped[count++] = *a;
        if (inside_a != inside_b)
            lerp_cv(a, b, (NEAR - a->v[0]) / (b->v[0] - a->v[0]), &clipped[count++]);
    }
    if (count < 3) return;

    gbv a[9], b[9];
    bool outside = false;
    for (int i = 0; i < count; ++i) {
        float screen[3];
        project_vert(e, clipped[i].v, screen);
        outside |= screen[0] < -1 || screen[0] > e->w + 1 || screen[1] < -1 || screen[1] > e->h + 1;
        a[i] = (gbv){screen[0], screen[1], screen[2],
                     {clipped[i].c[0] * screen[2], clipped[i].c[1] * screen[2], 0}};
    }
    if (outside) {
        count = gb_clip(a, count, b, 0, -1, -1);
        count = gb_clip(b, count, a, 0, e->w + 1, 1);
        count = gb_clip(a, count, b, 1, -1, -1);
        count = gb_clip(b, count, a, 1, e->h + 1, 1);
    }
    for (int i = 1; i + 1 < count; ++i) {
        const gbv *v[] = {a, a + i, a + i + 1};
        float ax = v[1]->x - a->x, ay = v[1]->y - a->y;
        float bx = v[2]->x - a->x, by = v[2]->y - a->y;
        float area = ax * by - bx * ay;
        if (fabsf(area) < 1e-3f) continue;
        if (g->texture_count == g->texture_capacity) {
            int capacity = g->texture_capacity ? g->texture_capacity * 2 : 128;
            tex_gradient *planes = realloc(g->texture_planes, capacity * sizeof(*planes));
            if (!planes) return;
            g->texture_planes = planes;
            g->texture_capacity = capacity;
        }
        prim_t *p = push(g, L_OPAQUE);
        if (!p) return;
        unsigned index = g->texture_count++;
        tex_gradient *plane = &g->texture_planes[index];
        plane->image = image;
        plane->lightmap = lightmap;
        for (int k = 0; k < 2; ++k) {
            float va = v[1]->c[k] - a->c[k], vb = v[2]->c[k] - a->c[k];
            plane->dx[k] = (va * by - vb * ay) / area;
            plane->dy[k] = (ax * vb - bx * va) / area;
            plane->origin[k] = a->c[k] - a->x * plane->dx[k] - a->y * plane->dy[k];
        }
        int32_t w[3], x[3], y[3];
        int lo = e->h, hi = 0;
        for (int k = 0; k < 3; ++k) {
            x[k] = fix4(v[k]->x);
            y[k] = fix4(v[k]->y);
            w[k] = v[k]->w * WONE;
            int row = (y[k] + 7) >> 4;
            if (row < lo) lo = row;
            if (row > hi) hi = row;
        }
        const uint8_t colors[3][3] = {{0}};
        prim_write(p, M_TEXTURE, w, x, y, colors);
        p->u[6] = index;
        p->u[7] = brightness;
        bin_prim(e, g, L_OPAQUE, p, row_clamp(lo, e->h), row_clamp(hi, e->h));
        ++g->tris_drawn;
    }
}

static void emit_line(e3d_t *e, geo_t *g, const cvert *a0, const cvert *b0)
{
    cvert a = *a0, b = *b0;
    if (a.v[0] < NEAR && b.v[0] < NEAR)
        return;
    if (a.v[0] < NEAR)
        lerp_cv(&a, &b, (NEAR - a.v[0]) / (b.v[0] - a.v[0]), &a);
    else if (b.v[0] < NEAR)
        lerp_cv(&b, &a, (NEAR - b.v[0]) / (a.v[0] - b.v[0]), &b);
    float sa[3], sb[3];
    project_vert(e, a.v, sa);
    project_vert(e, b.v, sb);
    /* clip to the screen (plus a pixel) in screen space, where 1/x and the colours are linear */
    float t0 = 0.f, t1 = 1.f, d[2] = { sb[0] - sa[0], sb[1] - sa[1] };
    float lo[2] = { -1.f, -1.f }, hi[2] = { (float)e->w + 1.f, (float)e->h + 1.f };
    for (int k = 0; k < 2; k++) {
        if (d[k] == 0.f) {
            if (sa[k] < lo[k] || sa[k] > hi[k])
                return;
            continue;
        }
        float ta = (lo[k] - sa[k]) / d[k], tb = (hi[k] - sa[k]) / d[k];
        if (ta > tb) { float t = ta; ta = tb; tb = t; }
        t0 = ta > t0 ? ta : t0;
        t1 = tb < t1 ? tb : t1;
    }
    if (t0 > t1)
        return;
    prim_t *p = push(g, L_OPAQUE);
    if (!p)
        return;
    int32_t w[3] = { 0 }, x[3] = { 0 }, y[3] = { 0 };
    uint8_t rgb[3][3] = { { 0 } };
    float ts[2] = { t0, t1 };
    for (int i = 0; i < 2; i++) {
        float t = ts[i];
        w[i] = (int32_t)((sa[2] + (sb[2] - sa[2]) * t) * WONE);
        x[i] = fix4(sa[0] + d[0] * t);
        y[i] = fix4(sa[1] + d[1] * t);
        for (int k = 0; k < 3; k++)
            rgb[i][k] = (uint8_t)(a.c[k] + (b.c[k] - a.c[k]) * t + 0.5f);
    }
    prim_write(p, M_LINE, w, x, y, rgb);
    float ya = (float)y[0] / SUB, yb = (float)y[1] / SUB;
    float l = ya < yb ? ya : yb, h = ya < yb ? yb : ya;
    bin_prim(e, g, L_OPAQUE, p, row_clamp(l - 1.f, e->h), row_clamp(h + 2.f, e->h));
}

E3D_FAST static void xform(const e3d_mat *m, const float in[3], float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = m->m[i][0] * in[0] + m->m[i][1] * in[1] + m->m[i][2] * in[2] + m->m[i][3];
}

/* normals: the cofactor matrix of the 3x3 part (inverse transpose up to scale) */
static void normal_matrix(const e3d_mat *m, float n[3][3])
{
    const float (*a)[4] = m->m;
    n[0][0] = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    n[0][1] = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    n[0][2] = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    n[1][0] = a[0][2] * a[2][1] - a[0][1] * a[2][2];
    n[1][1] = a[0][0] * a[2][2] - a[0][2] * a[2][0];
    n[1][2] = a[0][1] * a[2][0] - a[0][0] * a[2][1];
    n[2][0] = a[0][1] * a[1][2] - a[0][2] * a[1][1];
    n[2][1] = a[0][2] * a[1][0] - a[0][0] * a[1][2];
    n[2][2] = a[0][0] * a[1][1] - a[0][1] * a[1][0];
    float det = a[0][0] * n[0][0] + a[0][1] * n[0][1] + a[0][2] * n[0][2];
    if (det < 0.f)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                n[i][j] = -n[i][j];
}

/* Per-object scratch, per thread, in PSRAM: it is reused object after object,
 * so it lives in the cache anyway. (PIE stores reach PSRAM through the cache
 * faster than they reach internal SRAM.) */
static bool ensure_scratch(geo_t *g, int nverts, int nnorm)
{
    if (nverts <= g->nvert_cap && nnorm <= g->nnorm_cap)
        return true;
    if (nverts < g->nvert_cap)
        nverts = g->nvert_cap;
    if (nnorm < g->nnorm_cap)
        nnorm = g->nnorm_cap;
    size_t nv = (size_t)nverts, nn = (size_t)nnorm, np = (nn + 7) & ~(size_t)7;
    size_t base = (nv * (sizeof(float[3]) + 2 * sizeof(sv_t) + 2) + nn * (sizeof(float[2]) + sizeof(uint32_t)) + 15) &
                  ~(size_t)15;
    uint8_t *p = malloc(base);
    uint8_t *q = malloc(np * 12 * sizeof(int16_t));
    if (!p || !q) {
        free(p);
        free(q);
        return false;
    }
    free(g->lscratch);
    g->lscratch = q;
    g->nl = (int16_t *)q;
    g->nlb = g->nl + np * 6;
    free(g->scratch);
    g->scratch = p;
    g->vpos = (float (*)[3])p;
    g->sv = (sv_t *)(g->vpos + nv);
    g->ssv = g->sv + nv;
    g->lt = (float (*)[2])(g->ssv + nv);
    g->ltag = (uint32_t *)(g->lt + nn);
    g->clip = (uint8_t *)(g->ltag + nn);
    g->sclip = g->clip + nv;
    memset(g->ltag, 0, nn * sizeof(uint32_t));
    g->nvert_cap = nverts;
    g->nnorm_cap = nnorm;
    return true;
}

/* every vertex into view space and onto the screen, with its clip code: the
 * rows are written out so the dependent float chains overlap */
E3D_FAST static void xform_verts(const e3d_t *e, geo_t *g, const e3d_mat *vm, const float (*pos)[3], int n)
{
    const float b00 = vm->m[0][0], b01 = vm->m[0][1], b02 = vm->m[0][2], b03 = vm->m[0][3];
    const float b10 = vm->m[1][0], b11 = vm->m[1][1], b12 = vm->m[1][2], b13 = vm->m[1][3];
    const float b20 = vm->m[2][0], b21 = vm->m[2][1], b22 = vm->m[2][2], b23 = vm->m[2][3];
    const float cx = (float)e->w * 0.5f, cy = (float)e->h * 0.5f, f = e->focal;
    float (*vp)[3] = g->vpos;
    sv_t *sv = g->sv;
    uint8_t *cl = g->clip;
    for (int i = 0; i < n; i++) {
        float x = pos[i][0], y = pos[i][1], z = pos[i][2];
        float v0 = b00 * x + b01 * y + b02 * z + b03;
        float v1 = b10 * x + b11 * y + b12 * z + b13;
        float v2 = b20 * x + b21 * y + b22 * z + b23;
        vp[i][0] = v0;
        vp[i][1] = v1;
        vp[i][2] = v2;
        int c = C_NEAR;
        if (v0 >= NEAR) {
            float w = 1.f / v0, k = w * f, X = cx + v2 * k, Y = cy - v1 * k;
            c = screen_code(e, X, Y);
            if (!(c & C_GB)) {
                sv[i].w = (int32_t)(w * WONE);
                sv[i].x = (int16_t)fix4(X);
                sv[i].y = (int16_t)fix4(Y);
            }
        }
        cl[i] = (uint8_t)c;
    }
}

/* is the sphere (view-space centre c, radius r) at least partly in the frustum? */
static bool sphere_visible(const e3d_t *e, const float c[3], float r)
{
    if (c[0] + r < NEAR)
        return false;
    float tx = (float)e->w * 0.5f / e->focal, ty = (float)e->h * 0.5f / e->focal;
    float kx = sqrtf(1.f + tx * tx), ky = sqrtf(1.f + ty * ty);
    if (c[2] - c[0] * tx > r * kx || -c[2] - c[0] * tx > r * kx)
        return false;
    if (c[1] - c[0] * ty > r * ky || -c[1] - c[0] * ty > r * ky)
        return false;
    return true;
}

/* Planar shadows. The light's projection onto y = shadow_y is projective,
 * so with the view and the object's matrix it is one 4x4 matrix: a shadow
 * vertex takes 4 rows and one divide. Its 4th coordinate is the vertex's
 * world y less the light's. Triangles facing the light make the silhouette;
 * their shadows are the ones that turn like front faces seen from above. */
static void shadow_matrix(const e3d_t *e, const e3d_mat *m, e3d_mat *q)
{
    const float *L = e->light.pos;
    float sy = e->par.shadow_y, d = sy - L[1];
    const e3d_mat s = { { { d, L[0], 0.f, -sy * L[0] },
                          { 0.f, sy, 0.f, -sy * L[1] },
                          { 0.f, L[2], d, -sy * L[2] },
                          { 0.f, 1.f, 0.f, -L[1] } } };
    *q = e->view;
    e3d_mul(q, &s);
    e3d_mul(q, m);
}

static inline void shadow_vert(const float (*q)[4], const float p[3], float h[4])
{
    for (int r = 0; r < 4; r++)
        h[r] = q[r][0] * p[0] + q[r][1] * p[1] + q[r][2] * p[2] + q[r][3];
}

E3D_FAST static void draw_shadow(e3d_t *e, geo_t *g, const e3d_object *o)
{
    const e3d_mesh *mesh = o->mesh;
    e3d_mat qm;
    shadow_matrix(e, &o->m, &qm);
    const float (*q)[4] = (const float (*)[4])qm.m;
    const float q00 = q[0][0], q01 = q[0][1], q02 = q[0][2], q03 = q[0][3];
    const float q10 = q[1][0], q11 = q[1][1], q12 = q[1][2], q13 = q[1][3];
    const float q20 = q[2][0], q21 = q[2][1], q22 = q[2][2], q23 = q[2][3];
    const float q30 = q[3][0], q31 = q[3][1], q32 = q[3][2], q33 = q[3][3];
    const float cx = (float)e->w * 0.5f, cy = (float)e->h * 0.5f, f = e->focal;
    const float lo = e->par.shadow_y + 0.01f - e->light.pos[1];   /* the vertex above the floor... */
    const float (*pos)[3] = (const float (*)[3])mesh->pos;
    sv_t *sv = g->ssv;
    uint8_t *cl = g->sclip;
    for (int i = 0; i < mesh->nverts; i++) {
        float x = pos[i][0], y = pos[i][1], z = pos[i][2];
        float hx = q00 * x + q01 * y + q02 * z + q03;
        float hy = q10 * x + q11 * y + q12 * z + q13;
        float hz = q20 * x + q21 * y + q22 * z + q23;
        float hw = q30 * x + q31 * y + q32 * z + q33;
        int c = C_OFF;
        if (hw < -1.f && hw > lo) {                  /* ...and below the light */
            c = C_NEAR;
            if (hx <= NEAR * hw) {                   /* view x = hx / hw >= NEAR (hw < 0) */
                float iw = 1.f / hx, k = f * iw, X = cx + hz * k, Y = cy - hy * k;
                c = screen_code(e, X, Y);
                if (!(c & C_GB)) {
                    sv[i].w = (int32_t)(hw * iw * WONE);
                    sv[i].x = (int16_t)fix4(X);
                    sv[i].y = (int16_t)fix4(Y);
                }
            }
        }
        cl[i] = (uint8_t)c;
    }
    bool all = (o->flags & E3D_DOUBLE_SIDED) != 0;
    bool above = e->cam.pos[1] > e->par.shadow_y;
    const float up[3] = { e->view.m[0][1], e->view.m[1][1], e->view.m[2][1] };   /* world y in view space */
    static const uint8_t black[3][3];
    for (int t = 0; t < mesh->ntris; t++) {
        const e3d_tri *tri = &mesh->tris[t];
        int i0 = tri->v[0], i1 = tri->v[1], i2 = tri->v[2];
        int c0 = cl[i0], c1 = cl[i1], c2 = cl[i2], any = c0 | c1 | c2;
        if ((any & C_OFF) || (c0 & c1 & c2 & C_OUT))
            continue;
        if (!(any & (C_NEAR | C_GB))) {
            if (!all) {
                int64_t a = sv_area(&sv[i0], &sv[i1], &sv[i2]);
                if (above ? a <= 0 : a >= 0)
                    continue;
            }
            push_sv(e, g, L_SHADOW, M_SHADOW, &sv[i0], &sv[i1], &sv[i2], black);
            continue;
        }
        /* near the camera or far off screen: in view space, clipped */
        cvert cv[3];
        memset(cv, 0, sizeof(cv));
        for (int k = 0; k < 3; k++) {
            float h[4];
            shadow_vert(q, pos[tri->v[k]], h);
            float iw = 1.f / h[3];
            cv[k].v[0] = h[0] * iw;
            cv[k].v[1] = h[1] * iw;
            cv[k].v[2] = h[2] * iw;
        }
        if (!all) {
            const float *s0 = cv[0].v, *s1 = cv[1].v, *s2 = cv[2].v;
            float a[3] = { s0[0] - s1[0], s0[1] - s1[1], s0[2] - s1[2] };
            float b[3] = { s2[0] - s1[0], s2[1] - s1[1], s2[2] - s1[2] };
            float n[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
            if (dot3(n, up) <= 0.f)
                continue;
        }
        emit_tri(e, g, L_SHADOW, M_SHADOW, cv);
    }
}

/* Objects are recorded here (copied, so the caller may change them) and drawn
 * in e3d_end, shared between the threads. */
void e3d_draw(e3d_t *e, const e3d_object *o)
{
    if (!o->mesh || (o->flags & E3D_HIDDEN) || !e->fb)
        return;
    if (e->njobs == e->jobs_cap) {
        int cap = e->jobs_cap ? e->jobs_cap * 2 : 64;
        job_t *j = realloc(e->jobs, sizeof(*j) * (size_t)cap);
        if (!j)
            return;
        e->jobs = j;
        e->jobs_cap = cap;
    }
    job_t *j = &e->jobs[e->njobs++];
    j->o = *o;
    bool floor = (o->flags & E3D_FLOOR) != 0;
    j->shadow = e->par.shadows && e->floor_drawn && !floor && !(o->flags & E3D_NO_SHADOW) &&
                e->light.pos[1] > e->par.shadow_y;
    if (floor)
        e->floor_drawn = true;
    e->st_frame.objects++;
    e->st_frame.tris_in += o->mesh->ntris;
}

/* â”€â”€ Batched lighting â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Lights all of an object's corner normals at once, in 16-bit fixed point.
 * The light and half vectors are made per vertex in float, since they need
 * 1/sqrt. Then, per normal, 8 at a time with PIE:
 *   - the normal goes into world space
 *   - n.L and n.H, with the highlight power by repeated squaring
 *   - the colour factors F = ambient + kd * light (Q11) and S = ks * light (Q4)
 * A corner's colour is then base * F + S, in integers.
 *
 * nl holds 6 arrays of np: L x, y, z and H x, y, z per normal on the way in,
 * F r, g, b and S r, g, b on the way out. K is the normal matrix in Q14
 * (m00 m01 m02 m10 m11 m12 m20 m21 | m22), then D r, g, b (diffuse * light
 * / 255, Q11), S r, g, b (specular * light, Q4) and the ambient (Q11). */

static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* the same arithmetic in C: the host build, and the check of the PIE one */
static void light_batch_c(const int16_t *qn, int16_t *nl, int np, const int16_t *K, int shin)
{
    for (int u = 0; u < np; u++) {
        int32_t n[3] = { qn[u], qn[np + u], qn[2 * np + u] }, w[3];
        for (int r = 0; r < 3; r++)
            w[r] = sat16((K[3 * r] * n[0] + K[3 * r + 1] * n[1] + K[3 * r + 2] * n[2]) >> 14);
        int32_t d = sat16((w[0] * nl[u] + w[1] * nl[np + u] + w[2] * nl[2 * np + u]) >> 14);
        int32_t s = sat16((w[0] * nl[3 * np + u] + w[1] * nl[4 * np + u] + w[2] * nl[5 * np + u]) >> 14);
        d = d < 0 ? 0 : d;
        s = d > 0 && s > 0 ? s : 0;
        int32_t p = 16384;
        for (int e = shin; e; e >>= 1, s = (s * s) >> 14)
            if (e & 1)
                p = (p * s) >> 14;
        for (int c = 0; c < 3; c++) {
            nl[c * np + u] = sat16((d * K[9 + c] + 16384 * K[15]) >> 14);
            nl[(3 + c) * np + u] = sat16((p * K[12 + c]) >> 14);
        }
    }
}

#if defined(__riscv_xesppie)
static const int16_t lb_one[8] __attribute__((aligned(16))) = {
    16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384 };

/* PIE takes its integer operands only in x8-x15, hence the register variables. Each step
 * of 8 normals walks the 3 normal arrays and the 6 nl arrays by the stride
 * of one array. */
E3D_FAST static void light_batch(const int16_t *qn, int16_t *nl, int np, const int16_t *K, int shin)
{
    int16_t ds[16] __attribute__((aligned(16)));   /* kd, then the highlight base / power */
    register int sh __asm__("a1") = 14;
    register int st __asm__("a0") = np * (int)sizeof(int16_t);
    __asm__ volatile("esp.movx.w.sar %0" : : "r"(sh));
    for (int i = 0; i < np; i += 8) {
        register const int16_t *pk __asm__("a2") = K;
        register const int16_t *pn __asm__("a3") = qn + i;
        register int16_t *pl __asm__("a4") = nl + i;
        register int16_t *pd __asm__("a5") = ds;
        __asm__ volatile(
            "esp.vld.128.ip q7, %[k], 16\n\t"
            "esp.vld.128.ip q6, %[k], 0\n\t"
            "esp.vld.128.xp q0, %[n], %[st]\n\t"
            "esp.vld.128.xp q1, %[n], %[st]\n\t"
            "esp.vld.128.xp q2, %[n], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q7, 0\n\t"
            "esp.vsmulas.s16.qacc q1, q7, 1\n\t"
            "esp.vsmulas.s16.qacc q2, q7, 2\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q7, 3\n\t"
            "esp.vsmulas.s16.qacc q1, q7, 4\n\t"
            "esp.vsmulas.s16.qacc q2, q7, 5\n\t"
            "esp.srcmb.s16.qacc q4, %[sh], 1\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q7, 6\n\t"
            "esp.vsmulas.s16.qacc q1, q7, 7\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 0\n\t"
            "esp.srcmb.s16.qacc q5, %[sh], 1\n\t"
            "esp.vld.128.xp q0, %[l], %[st]\n\t"
            "esp.vld.128.xp q1, %[l], %[st]\n\t"
            "esp.vld.128.xp q2, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vmulas.s16.qacc q3, q0\n\t"
            "esp.vmulas.s16.qacc q4, q1\n\t"
            "esp.vmulas.s16.qacc q5, q2\n\t"
            "esp.srcmb.s16.qacc q0, %[sh], 1\n\t"
            "esp.vld.128.xp q1, %[l], %[st]\n\t"
            "esp.vld.128.xp q2, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vmulas.s16.qacc q3, q1\n\t"
            "esp.vmulas.s16.qacc q4, q2\n\t"
            "esp.vld.128.xp q1, %[l], %[st]\n\t"
            "esp.vmulas.s16.qacc q5, q1\n\t"
            "esp.srcmb.s16.qacc q1, %[sh], 1\n\t"
            "esp.zero.q q2\n\t"
            "esp.vmax.s16 q0, q0, q2\n\t"
            "esp.vcmp.gt.s16 q3, q0, q2\n\t"
            "esp.vmax.s16 q1, q1, q2\n\t"
            "esp.andq q1, q1, q3\n\t"
            "esp.vst.128.ip q0, %[d], 16\n\t"
            "esp.vst.128.ip q1, %[d], 0"
            : [k] "+r"(pk), [n] "+r"(pn), [l] "+r"(pl), [d] "+r"(pd)
            : [st] "r"(st), [sh] "r"(sh)
            : "memory");
        register const int16_t *po __asm__("a3") = lb_one;
        __asm__ volatile(
            "esp.vld.128.ip q1, %[d], 0\n\t"
            "esp.vld.128.ip q2, %[o], 0\n\t"
            "mv t0, %[n]\n"
            "1:\n\t"
            "andi t1, t0, 1\n\t"
            "beqz t1, 2f\n\t"
            "esp.vmul.s16 q2, q2, q1\n"
            "2:\n\t"
            "srli t0, t0, 1\n\t"
            "beqz t0, 3f\n\t"
            "esp.vmul.s16 q1, q1, q1\n\t"
            "j 1b\n"
            "3:\n\t"
            "esp.vst.128.ip q2, %[d], 0"
            : [d] "+r"(pd), [o] "+r"(po)
            : [n] "r"(shin)
            : "t0", "t1", "memory");
        pk = K + 8;
        pd = ds;
        pl = nl + i;
        __asm__ volatile(
            "esp.vld.128.ip q6, %[k], 0\n\t"
            "esp.vld.128.ip q0, %[d], 16\n\t"
            "esp.vld.128.ip q1, %[d], 0\n\t"
            "esp.vld.128.ip q2, %[o], 0\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q6, 1\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 7\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q6, 2\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 7\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q6, 3\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 7\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q1, q6, 4\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q1, q6, 5\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q1, q6, 6\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]"
            : [k] "+r"(pk), [d] "+r"(pd), [l] "+r"(pl), [o] "+r"(po)
            : [st] "r"(st), [sh] "r"(sh)
            : "memory");
    }
}
#else
#define light_batch light_batch_c
#endif

#if defined(__riscv_xesppie)
/* The PIE kernel against the C one on made-up normals: a difference beyond
 * rounding means the C one is used. */
static bool light_batch_check(void)
{
    enum { N = 64 };
    int16_t *qn = malloc(sizeof(int16_t) * 3 * N), *a = malloc(sizeof(int16_t) * 6 * N), *b = malloc(sizeof(int16_t) * 6 * N);
    bool ok = false;
    if (qn && a && b) {
        uint32_t r = 12345;
        for (int set = 0; set < 3; set++)
            for (int u = 0; u < N; u++) {
                float v[3];
                for (int c = 0; c < 3; c++) {
                    r = r * 1664525u + 1013904223u;
                    v[c] = (float)(int32_t)(r >> 8 & 0xffff) / 32768.f - 1.f;
                }
                norm3(v);
                for (int c = 0; c < 3; c++) {
                    if (set == 0)
                        qn[c * N + u] = (int16_t)(v[c] * 16384.f);
                    else
                        a[((set - 1) * 3 + c) * N + u] = (int16_t)(v[c] * 16384.f);
                }
            }
        e3d_mat m;
        e3d_rotate(&m, 30, 50, 70);
        int16_t K[16] __attribute__((aligned(16)));
        for (int i = 0; i < 9; i++)
            K[i] = (int16_t)lrintf(m.m[i / 3][i % 3] * 16384.f);
        const int16_t rest[7] = { 1831, 1700, 1500, 3264, 3000, 2800, 717 };
        memcpy(K + 9, rest, sizeof(rest));
        for (int u = 1; u < N; u += 2) {   /* H near the turned normal: bright highlights */
            for (int c = 0; c < 3; c++) {
                float h = (K[3 * c] * qn[u] + K[3 * c + 1] * qn[N + u] + K[3 * c + 2] * qn[2 * N + u]) / (16384.f * 16384.f);
                a[(3 + c) * N + u] = (int16_t)(h * (16000.f - 40.f * (float)u));
                a[c * N + u] = (int16_t)(h * 16000.f);   /* and L along it, so n.L > 0 */
            }
        }
        memcpy(b, a, sizeof(int16_t) * 6 * N);
        light_batch(qn, a, N, K, 24);
        light_batch_c(qn, b, N, K, 24);
        int worst = 0;
        for (int i = 0; i < 6 * N; i++) {
            int d = a[i] - b[i];
            d = d < 0 ? -d : d;
            worst = d > worst ? d : worst;
        }
        ok = worst <= 2;
        if (!ok)
            E3D_LOG("PIE lighting differs from C by %d, using C", worst);
    }
    free(qn); free(a); free(b);
    return ok;
}
#endif


/* the colour factors of the object's normals into g->nl (see above), and
 * with back into g->nlb for back faces too (the batch again, with the normal
 * matrix negated); false
 * when the batch does not apply and corners are lit one by one: a scale that
 * is not uniform, or a shininess that is not a whole number */
E3D_FAST static bool light_object(const e3d_t *e, geo_t *g, const e3d_mesh *mesh, const float nm[3][3], int16_t K[16],
                                  bool back)
{
    if (!mesh->qn || !mesh->nnorm || !e->shin_n)
        return false;
    float r0 = dot3(nm[0], nm[0]), r1 = dot3(nm[1], nm[1]), r2 = dot3(nm[2], nm[2]);
    float k2 = (r0 + r1 + r2) * (1.f / 3.f), tol = k2 * 0.01f;
    if (k2 <= 0.f || fabsf(r0 - k2) > tol || fabsf(r1 - k2) > tol || fabsf(r2 - k2) > tol ||
        fabsf(dot3(nm[0], nm[1])) > tol || fabsf(dot3(nm[0], nm[2])) > tol || fabsf(dot3(nm[1], nm[2])) > tol)
        return false;
    float k = 16384.f * rsqrt_fast(k2);
    for (int i = 0; i < 9; i++)
        K[i] = sat16(lrintf(nm[i / 3][i % 3] * k));
    const float lc[3] = { e->light.r, e->light.g, e->light.b };
    for (int c = 0; c < 3; c++) {
        K[9 + c] = sat16(lrintf(e->par.diffuse * (1.f / 255.f) * lc[c] * 2048.f));
        K[12 + c] = sat16(lrintf(e->par.specular * lc[c] * 16.f));
    }
    K[15] = sat16(lrintf(e->par.ambient * 2048.f));

    /* L and H per vertex, written straight into the lanes of its normals
     * (the mesh keeps them in vertex order) */
    const float *lp = e->vlight;   /* view space: the camera is at the origin */
    int nn = mesh->nnorm, np = (nn + 7) & ~7;
    int16_t *nl = g->nl;
    int pv = -1;
    int16_t lx = 0, ly = 0, lz = 0, hx = 0, hy = 0, hz = 0;
    for (int u = 0; u < nn; u++) {
        int v = mesh->nrm_vert[u];
        if (v != pv) {
            pv = v;
            const float *p = g->vpos[v];
            float L[3] = { lp[0] - p[0], lp[1] - p[1], lp[2] - p[2] };
            float V[3] = { -p[0], -p[1], -p[2] };
            /* no branches: they cost more than the arithmetic on the P4 */
            float ll = rsqrt_fast(dot3(L, L) + 1e-6f) * 16384.f, vv = rsqrt_fast(dot3(V, V) + 1e-6f) * 16384.f;
            float H[3] = { L[0] * ll + V[0] * vv, L[1] * ll + V[1] * vv, L[2] * ll + V[2] * vv };
            float hh = rsqrt_fast(dot3(H, H) + 1.f) * 16384.f;
            lx = (int16_t)(L[0] * ll); ly = (int16_t)(L[1] * ll); lz = (int16_t)(L[2] * ll);
            hx = (int16_t)(H[0] * hh); hy = (int16_t)(H[1] * hh); hz = (int16_t)(H[2] * hh);
        }
        nl[u] = lx;
        nl[np + u] = ly;
        nl[2 * np + u] = lz;
        nl[3 * np + u] = hx;
        nl[4 * np + u] = hy;
        nl[5 * np + u] = hz;
    }
    for (int u = nn; u < np; u++)
        for (int c = 0; c < 6; c++)
            nl[c * np + u] = 0;
    int16_t Kb[16] __attribute__((aligned(16)));
    if (back) {
        memcpy(g->nlb, nl, sizeof(int16_t) * 6 * (size_t)np);
        for (int i = 0; i < 16; i++)
            Kb[i] = (int16_t)(i < 9 ? -K[i] : K[i]);
    }
#if defined(__riscv_xesppie)
    if (e->pie_light) {
        light_batch(mesh->qn, nl, np, K, e->shin_n);
        if (back)
            light_batch(mesh->qn, g->nlb, np, Kb, e->shin_n);
        return true;
    }
#endif
    light_batch_c(mesh->qn, nl, np, K, e->shin_n);
    if (back)
        light_batch_c(mesh->qn, g->nlb, np, Kb, e->shin_n);
    return true;
}

/* base * F + S of the batch, rounded, at most 255 (without a branch: c >= 0) */
static inline uint8_t mix8(int base, int f, int s)
{
    int c = (base * f + s * 128 + 1024) >> 11;
    return (uint8_t)(c | (255 - c) >> 31);
}

/* a corner's colour from its normal's factors f (arrays np apart) */
static inline void corner_rgb(const int16_t *f, int np, int r, int g, int b, uint8_t out[3])
{
    out[0] = mix8(r, f[0], f[3 * np]);
    out[1] = mix8(g, f[np], f[4 * np]);
    out[2] = mix8(b, f[2 * np], f[5 * np]);
}

E3D_FAST static void draw_object(e3d_t *e, geo_t *g, const job_t *j)
{
    const e3d_object *o = &j->o;
    const e3d_mesh *mesh = o->mesh;
    bool floor = (o->flags & E3D_FLOOR) != 0;
    bool shadow = j->shadow;

    float c[3], cv[3];
    xform(&o->m, mesh->center, c);
    float sc = 0.f;
    for (int j = 0; j < 3; j++) {
        float l = o->m.m[0][j] * o->m.m[0][j] + o->m.m[1][j] * o->m.m[1][j] + o->m.m[2][j] * o->m.m[2][j];
        if (l > sc)
            sc = l;
    }
    float radius = mesh->radius * sqrtf(sc);
    xform(&e->view, c, cv);
    bool visible = sphere_visible(e, cv, radius);
    if (!visible && !shadow)
        return;
    if (!ensure_scratch(g, mesh->nverts, mesh->nnorm))
        return;

    if (shadow)
        draw_shadow(e, g, o);
    if (!visible)
        return;
    e3d_mat mv = e->view;
    e3d_mul(&mv, &o->m);
    xform_verts(e, g, &mv, (const float (*)[3])mesh->pos, mesh->nverts);

    /* lit in view space (the view is a rotation, so normals turn with it) */
    float nm[3][3];
    normal_matrix(&mv, nm);
    bool two_sided = (o->flags & E3D_DOUBLE_SIDED) || floor;
    bool flat = (o->flags & E3D_FLAT) != 0;
    bool unlit = (o->flags & E3D_UNLIT) != 0;
    bool wire = (o->flags & E3D_WIRE) != 0;
    int layer = floor ? L_FLOOR : L_OPAQUE;
    int mode = floor ? M_FLOOR : flat ? M_FLAT : M_GOURAUD;
    float obj_rgb[3] = { (float)((o->color >> 16) & 255), (float)((o->color >> 8) & 255), (float)(o->color & 255) };
    uint32_t tag = ++g->tag ? g->tag : ++g->tag;   /* lit-corner cache of this draw */
    int16_t K[16] __attribute__((aligned(16)));
    bool back = wire || (o->flags & E3D_DOUBLE_SIDED);   /* back faces drawn often enough to batch */
    bool batch = !unlit && !flat && light_object(e, g, mesh, nm, K, back);
    int np = (mesh->nnorm + 7) & ~7;

    const uint8_t *cl = g->clip;
    const sv_t *sv = g->sv;
    for (int t = 0; t < mesh->ntris; t++) {
        const e3d_tri *tri = &mesh->tris[t];
        int i0 = tri->v[0], i1 = tri->v[1], i2 = tri->v[2];
        int c0 = cl[i0], c1 = cl[i1], c2 = cl[i2];
        if (c0 & c1 & c2 & C_OUT)
            continue;
        /* on the screen records: integer tests, no clipping */
        bool scr = !((c0 | c1 | c2) & (C_NEAR | C_GB)), fast = scr && !wire;
        bool front;
        if (scr) {
            front = sv_area(&sv[i0], &sv[i1], &sv[i2]) > 0;
        } else {
            const float *a = g->vpos[i0], *b = g->vpos[i1], *cc = g->vpos[i2];
            float e1[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
            float e2[3] = { cc[0] - b[0], cc[1] - b[1], cc[2] - b[2] };
            float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            front = dot3(n, b) < 0.f;
        }
        if (!front && !two_sided && !wire)
            continue;
        if (!wire && !floor && mesh->uv && mesh->textures && tri->texture &&
            tri->texture <= mesh->texture_count) {
            const e3d_texture *image = &mesh->textures[tri->texture - 1];
            if (image->indices && image->palette && image->width && image->height) {
                cvert vertices[3];
                for (int k = 0; k < 3; ++k) {
                    unsigned index = tri->v[k];
                    memcpy(vertices[k].v, g->vpos[index], sizeof(vertices[k].v));
                    vertices[k].c[0] = mesh->uv[index][0];
                    vertices[k].c[1] = mesh->uv[index][1];
                    vertices[k].c[2] = 0;
                }
                const e3d_lightmap *lightmap = mesh->lightmaps ? mesh->lightmaps[t] : NULL;
                if (lightmap && (!lightmap->rgb || !lightmap->width || !lightmap->height ||
                                 (size_t)lightmap->width * lightmap->height > SIZE_MAX / 3))
                    lightmap = NULL;
                emit_texture(e, g, vertices, image, tri->r, lightmap);
                continue;
            }
        }
        float sign = front ? 1.f : -1.f;

        float base[3];
        if (o->color >= 0)
            memcpy(base, obj_rgb, sizeof(base));
        else {
            base[0] = tri->r;
            base[1] = tri->g;
            base[2] = tri->b;
        }
        cvert v[3];
        uint8_t rgb[3][3];
        bool batched = batch && (front || back);
        if (batched) {
            int br = (int)base[0], bg = (int)base[1], bb = (int)base[2];
            const int16_t *f = front ? g->nl : g->nlb;
            corner_rgb(f + tri->n[0], np, br, bg, bb, rgb[0]);
            corner_rgb(f + tri->n[1], np, br, bg, bb, rgb[1]);
            corner_rgb(f + tri->n[2], np, br, bg, bb, rgb[2]);
        } else if (unlit) {
            for (int k = 0; k < 3; k++)
                for (int q = 0; q < 3; q++)
                    v[k].c[q] = clampc(base[q]);
        } else if (flat) {
            const float *p0 = g->vpos[i0], *p1 = g->vpos[i1], *p2 = g->vpos[i2];
            float f1[3] = { p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2] };
            float f2[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
            float fn[3] = { sign * (f1[1] * f2[2] - f1[2] * f2[1]), sign * (f1[2] * f2[0] - f1[0] * f2[2]),
                            sign * (f1[0] * f2[1] - f1[1] * f2[0]) };
            norm3(fn);
            float mid[3] = { (p0[0] + p1[0] + p2[0]) / 3.f, (p0[1] + p1[1] + p2[1]) / 3.f, (p0[2] + p1[2] + p2[2]) / 3.f };
            float kd, ks;
            light_terms(e, mid, fn, &kd, &ks);
            lit_color(e, base, kd, ks, v[0].c);
            memcpy(v[1].c, v[0].c, sizeof(v[0].c));
            memcpy(v[2].c, v[0].c, sizeof(v[0].c));
        } else {
            for (int k = 0; k < 3; k++) {
                int u = tri->n[k];
                float kd, ks;
                if (front && g->ltag[u] == tag) {
                    kd = g->lt[u][0];
                    ks = g->lt[u][1];
                } else {
                    const float *ln = mesh->nrm[u];
                    float wn[3];
                    for (int q = 0; q < 3; q++)
                        wn[q] = sign * (nm[q][0] * ln[0] + nm[q][1] * ln[1] + nm[q][2] * ln[2]);
                    norm3(wn);
                    light_terms(e, g->vpos[mesh->nrm_vert[u]], wn, &kd, &ks);
                    if (front) {
                        g->lt[u][0] = kd;
                        g->lt[u][1] = ks;
                        g->ltag[u] = tag;
                    }
                }
                lit_color(e, base, kd, ks, v[k].c);
            }
        }
        if (!batched)
            for (int k = 0; k < 3; k++)
                for (int q = 0; q < 3; q++)
                    rgb[k][q] = (uint8_t)(v[k].c[q] + 0.5f);
        if (wire && scr) {
            /* a closed mesh has each edge twice, once each way: drawn once */
            const uint16_t *vi = tri->v;
            for (int k = 0; k < 3; k++) {
                int a = vi[k], b = vi[k == 2 ? 0 : k + 1];
                if ((mesh->open || a < b) && !(cl[a] & cl[b] & C_OUT))
                    push_line_sv(e, g, &sv[a], &sv[b], rgb[k], rgb[k == 2 ? 0 : k + 1]);
            }
            g->tris_drawn++;
            continue;
        }
        if (fast) {
            push_sv(e, g, layer, mode, &sv[i0], &sv[i1], &sv[i2], rgb);
            continue;
        }
        if (batched)
            for (int k = 0; k < 3; k++)
                for (int q = 0; q < 3; q++)
                    v[k].c[q] = rgb[k][q];
        for (int k = 0; k < 3; k++)
            memcpy(v[k].v, g->vpos[tri->v[k]], sizeof(v[k].v));
        if (wire) {
            emit_line(e, g, &v[0], &v[1]);
            emit_line(e, g, &v[1], &v[2]);
            emit_line(e, g, &v[2], &v[0]);
            g->tris_drawn++;
        } else {
            emit_tri(e, g, layer, mode, v);
        }
    }
}

/* The original's lightDisplay: a glow where the light is, hidden behind objects. */
void e3d_draw_light(e3d_t *e)
{
    float lv[3];
    xform(&e->view, e->light.pos, lv);
    if (lv[0] <= NEAR)
        return;
    float dist = sqrtf(dot3(lv, lv));
    prim_t *p = push(&e->geo[0], L_FX);
    if (!p)
        return;
    float s[3];
    project_vert(e, lv, s);
    float cx = (float)e->w * 0.5f, cy = (float)e->h * 0.5f;
    if (fabsf(s[0] - cx) > GB || fabsf(s[1] - cy) > GB)
        return;
    float r = 16000.f / dist * (float)e->w / 640.f;
    r = r < 4.f ? 4.f : r > (float)e->h ? (float)e->h : r;
    int32_t w[3] = { (int32_t)(s[2] * WONE), 0, 0 }, x[3] = { fix4(s[0]), fix4(r), 0 }, y[3] = { fix4(s[1]), 0, 0 };
    uint8_t rgb[3][3] = { { (uint8_t)clampc(e->light.r), (uint8_t)clampc(e->light.g), (uint8_t)clampc(e->light.b) } };
    prim_write(p, M_GLOW, w, x, y, rgb);
    bin_prim(e, &e->geo[0], L_FX, p, row_clamp(s[1] - r - 1.f, e->h), row_clamp(s[1] + r + 2.f, e->h));
}

/* â”€â”€ Rasteriser (into a tile) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * Integer from the primitive on: 12.4 corners, 2.30 depth, 16.16 edges and
 * colours. Only the per-triangle setup is in float (one reciprocal for the
 * gradients, one divide per edge); pixels are integer. */

#define PACK_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_MAX (255 << 16)

static inline uint16_t pack565(int32_t r, int32_t g, int32_t b)
{
    return (uint16_t)(((r >> 8) & 0xF800) | ((g >> 13) & 0x07E0) | (b >> 19));
}

static inline int32_t fx16(float v)   /* to 16.16, kept in range */
{
    return (int32_t)(v < -32000.f ? -32000.f * 65536.f : v > 32000.f ? 32000.f * 65536.f : v * 65536.f);
}

/* A span's colour channel in 16.16: the start c (float, 16.16 units) and the
 * gradient d. The gradient is used as it is when the whole span stays in
 * 0..255, the common case; else both ends are clamped and the step remade. */
static inline void span_c(float c, int32_t d, int n, int32_t *ci, int32_t *di)
{
    int32_t a = (int32_t)c, b = a + d * (n - 1);
    if ((uint32_t)a <= C_MAX && (uint32_t)b <= C_MAX) {
        *ci = a;
        *di = d;
        return;
    }
    a = c < 0.f ? 0 : c > (float)C_MAX ? C_MAX : a;
    float cb = c + (float)d * (float)(n - 1);
    b = cb < 0.f ? 0 : cb > (float)C_MAX ? C_MAX : (int32_t)cb;
    *ci = a;
    *di = n > 1 ? (b - a) / (n - 1) : 0;
}

/* the span of a row from the two edges, clipped to the tile's width */
#define ROW_SPAN()                                                         \
    if (y == ym && y != ys) {                                              \
        xb = fx16(x1 + ((float)y + 0.5f - y1) * s12);                      \
        dxb = fx16(s12);                                                   \
    }                                                                      \
    int32_t l = xa < xb ? xa : xb, r = xa < xb ? xb : xa;                  \
    int xs = (l + 0x7FFF) >> 16, xe = (r + 0x7FFF) >> 16;                  \
    if (xs < 0) xs = 0;                                                    \
    if (xe > W) xe = W

static inline int floor_texel(float value)
{
    int texel = (int)value;
    return texel - ((float)texel > value);
}

static inline int wrap_texel(int texel, int size, bool power_of_two)
{
    if (power_of_two) return texel & (size - 1);
    texel %= size;
    return texel < 0 ? texel + size : texel;
}

static inline uint16_t modulate_texture(uint16_t color, unsigned red, unsigned green, unsigned blue)
{
    unsigned r = (color >> 11) * red >> 8;
    unsigned g = ((color >> 5) & 63) * green >> 8;
    unsigned b = (color & 31) * blue >> 8;
    return (r << 11) | (g << 5) | b;
}

static inline unsigned lightmap_offset(int texel, int32_t first, unsigned luxels)
{
    if (texel <= first) return 0;
    unsigned offset = (uint32_t)texel - (uint32_t)first;
    unsigned end = (luxels - 1) * 16;
    return offset < end ? offset : end;
}

static inline uint16_t shade_lightmap(const e3d_lightmap *lightmap, int s, int t, uint16_t color)
{
    unsigned u = lightmap_offset(s, lightmap->min_u, lightmap->width);
    unsigned v = lightmap_offset(t, lightmap->min_v, lightmap->height);
    unsigned uf = u & 15, vf = v & 15;
    size_t stride = (size_t)lightmap->width * 3;
    const uint8_t *a = lightmap->rgb + (v >> 4) * stride + (u >> 4) * 3;
    // At an exact luxel, the neighbor has zero weight and can be outside the lightmap.
    size_t next_column = uf ? 3 : 0;
    const uint8_t *b = a + next_column;
    const uint8_t *c = a + (vf ? stride : 0);
    const uint8_t *d = c + next_column;
    unsigned wa = (16 - uf) * (16 - vf), wb = uf * (16 - vf);
    unsigned wc = (16 - uf) * vf, wd = uf * vf;
    unsigned r = (wa * a[0] + wb * b[0] + wc * c[0] + wd * d[0]) >> 8;
    unsigned g = (wa * a[1] + wb * b[1] + wc * c[1] + wd * d[1]) >> 8;
    unsigned blue = (wa * a[2] + wb * b[2] + wc * c[2] + wd * d[2]) >> 8;
    return modulate_texture(color, r + 1, g + 1, blue + 1);
}

size_t e3d_lightmap_texel_count(const e3d_lightmap *lightmap)
{
    if (!lightmap || !lightmap->rgb || !lightmap->width || !lightmap->height) return 0;
    size_t width = (lightmap->width - 1) * 16u + 1;
    size_t height = (lightmap->height - 1) * 16u + 1;
    if (width > SIZE_MAX / height / sizeof(uint16_t) ||
        (int64_t)lightmap->min_u + (int64_t)width - 1 > INT32_MAX ||
        (int64_t)lightmap->min_v + (int64_t)height - 1 > INT32_MAX) return 0;
    return width * height;
}

bool e3d_lightmap_bake(e3d_lightmap *lightmap, const e3d_texture *image,
                       uint16_t *output, size_t output_texels)
{
    size_t count = e3d_lightmap_texel_count(lightmap);
    if (!count || count > output_texels || !output || !image || image->masked ||
        !image->indices || !image->palette || !image->width || !image->height) return false;

    int width = (lightmap->width - 1) * 16 + 1;
    int height = (lightmap->height - 1) * 16 + 1;
    bool pow2x = !(image->width & (image->width - 1));
    bool pow2y = !(image->height & (image->height - 1));
    uint16_t *pixel = output;
    for (int y = 0; y < height; ++y) {
        int raw_t = lightmap->min_v + y;
        size_t t = wrap_texel(raw_t, image->height, pow2y);
        const uint8_t *source = image->indices + t * image->width;
        for (int x = 0; x < width; ++x) {
            int raw_s = lightmap->min_u + x;
            int s = wrap_texel(raw_s, image->width, pow2x);
            *pixel++ = shade_lightmap(lightmap, raw_s, raw_t, image->palette[source[s]]);
        }
    }
    lightmap->baked_texels = output;
    lightmap->baked_texture = image;
    return true;
}

E3D_FAST static void raster_tri(const e3d_t *e, const geo_t *geo, const prim_t *p, tile_t *tl)
{
    int i0 = 0, i1 = 1, i2 = 2, t;
    if (p->y[i1] < p->y[i0]) { t = i0; i0 = i1; i1 = t; }
    if (p->y[i2] < p->y[i0]) { t = i0; i0 = i2; i2 = t; }
    if (p->y[i2] < p->y[i1]) { t = i1; i1 = i2; i2 = t; }
    int Y0 = p->y[i0], Y1 = p->y[i1], Y2 = p->y[i2];
    /* rows whose pixel centre is in [y0, y2) */
    int ys = (Y0 + 7) >> 4, ye = (Y2 + 7) >> 4, ym = (Y1 + 7) >> 4;
    if (ys < tl->y0) ys = tl->y0;
    if (ye > tl->y1) ye = tl->y1;
    if (ys >= ye)
        return;
    const float k4 = 1.f / SUB;
    float x0 = p->x[i0] * k4, y0 = Y0 * k4, x1 = p->x[i1] * k4, y1 = Y1 * k4, x2 = p->x[i2] * k4, y2 = Y2 * k4;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (!(fabsf(area) > 1e-3f))
        return;

    /* edges in 16.16 at the first row's pixel centre, stepped per row */
    float py = (float)ys + 0.5f;
    float sl = (x2 - x0) / (y2 - y0);
    float s01 = y1 > y0 ? (x1 - x0) / (y1 - y0) : 0.f;
    float s12 = y2 > y1 ? (x2 - x1) / (y2 - y1) : 0.f;
    int32_t xa = fx16(x0 + (py - y0) * sl), dxa = fx16(sl), xb, dxb;
    if (ys < ym) {
        xb = fx16(x0 + (py - y0) * s01);
        dxb = fx16(s01);
    } else {
        xb = fx16(x1 + (py - y1) * s12);
        dxb = fx16(s12);
    }
    int mode = p->mode, W = e->w;
    int row0 = (ys - tl->y0) * W;

    if (mode == M_SHADOW) {   /* edges only */
        int dark = (int)(e->par.shadow_dark * 32.f);
        for (int y = ys; y < ye; y++, xa += dxa, xb += dxb, row0 += W) {
            ROW_SPAN();
            uint16_t *fb = tl->c + row0;
            uint8_t *sb = tl->s + row0;
            for (int i = xs; i < xe; i++) {
                if (sb[i] == 1) {
                    sb[i] = 2;
                    uint16_t c = fb[i];
                    int cr = ((c >> 11) * dark) >> 5, cg = (((c >> 5) & 63) * dark) >> 5, cb = ((c & 31) * dark) >> 5;
                    fb[i] = (uint16_t)((cr << 11) | (cg << 5) | cb);
                }
            }
        }
        return;
    }

    /* the gradients; the row values are kept in float (the FPU's registers
     * are free, the integer ones are not) */
    float ia = 1.f / area;
    float ex1 = (y2 - y0) * ia, ex2 = (y1 - y0) * ia;   /* d/dx: (a1-a0)*ex1 - (a2-a0)*ex2 */
    float ey1 = (x2 - x0) * ia, ey2 = (x1 - x0) * ia;   /* d/dy: (a2-a0)*ey2 - (a1-a0)*ey1 */
#define GRADS(A, S, a0, dx, dy) \
    float a0 = (float)A[i0] * S, dx = (float)(A[i1] - A[i0]) * S * ex1 - (float)(A[i2] - A[i0]) * S * ex2, \
          dy = (float)(A[i2] - A[i0]) * S * ey2 - (float)(A[i1] - A[i0]) * S * ey1
    GRADS(p->w, 1.f, w0, dwdx, dwdy);
    int32_t dw = (int32_t)dwdx;
    float wr = w0 + dwdy * (py - y0);
    if (mode == M_TEXTURE) {
        const tex_gradient *plane = &geo->texture_planes[p->u[6]];
        const e3d_texture *image = plane->image;
        const e3d_lightmap *lightmap = plane->lightmap;
        const uint16_t *baked = lightmap && lightmap->baked_texture == image && !image->masked ?
                                lightmap->baked_texels : NULL;
        unsigned baked_width = baked ? (lightmap->width - 1) * 16u + 1 : 0;
        unsigned baked_height = baked ? (lightmap->height - 1) * 16u + 1 : 0;
        unsigned brightness = p->u[7] + 1;
        int width = image->width, height = image->height;
        bool pow2x = !(width & (width - 1)), pow2y = !(height & (height - 1));
        static const float step_scale[] = {0, 1, 0.5f, 1.f / 3, 0.25f, 0.2f, 1.f / 6, 1.f / 7, 0.125f};
        for (int y = ys; y < ye; y++, xa += dxa, xb += dxb, row0 += W, wr += dwdy) {
            ROW_SPAN();
            uint16_t *fb = tl->c + row0;
            int32_t *zb = tl->z + row0;
            float x = xs + .5f, fy = y + .5f;
            int32_t w = (int32_t)(wr + dwdx * (x - x0));
            float u = plane->origin[0] + plane->dx[0] * x + plane->dy[0] * fy;
            float v = plane->origin[1] + plane->dx[1] * x + plane->dy[1] * fy;
            for (int i = xs; i < xe;) {
                int n = xe - i < 8 ? xe - i : 8;
                float end_w = w + (float)dw * n;
                float depth = WONE / (w > 0 ? w : 1), end_depth = WONE / (end_w > 0 ? end_w : 1);
                float su = u * depth, tv = v * depth;
                u += plane->dx[0] * n;
                v += plane->dx[1] * n;
                // Correct perspective at each block endpoint, then step across its texels.
                float du = (u * end_depth - su) * step_scale[n];
                float dv = (v * end_depth - tv) * step_scale[n];
                int past = i + n;
                if (baked) {
                    for (; i < past; ++i, w += dw, su += du, tv += dv) {
                        if (w <= zb[i]) continue;
                        int raw_s = floor_texel(su), raw_t = floor_texel(tv);
                        unsigned u_local = (unsigned)raw_s - (unsigned)lightmap->min_u;
                        unsigned v_local = (unsigned)raw_t - (unsigned)lightmap->min_v;
                        if (u_local < baked_width && v_local < baked_height) {
                            fb[i] = baked[v_local * baked_width + u_local];
                        } else {
                            int s = wrap_texel(raw_s, width, pow2x), t = wrap_texel(raw_t, height, pow2y);
                            uint8_t index = image->indices[t * width + s];
                            fb[i] = shade_lightmap(lightmap, raw_s, raw_t, image->palette[index]);
                        }
                        zb[i] = w;
                    }
                    continue;
                }
                for (; i < past; ++i, w += dw, su += du, tv += dv) {
                    if (w <= zb[i]) continue;
                    int raw_s = floor_texel(su), raw_t = floor_texel(tv);
                    int s = wrap_texel(raw_s, width, pow2x), t = wrap_texel(raw_t, height, pow2y);
                    uint8_t index = image->indices[t * width + s];
                    if (image->masked && index == 255) continue;
                    uint16_t color = image->palette[index];
                    fb[i] = lightmap ? shade_lightmap(lightmap, raw_s, raw_t, color) :
                                      modulate_texture(color, brightness, brightness, brightness);
                    zb[i] = w;
                }
            }
        }
        return;
    }
    if (mode == M_FLAT) {
        uint16_t flat = PACK_RGB(p->r[i0], p->g[i0], p->b[i0]);
        for (int y = ys; y < ye; y++, xa += dxa, xb += dxb, row0 += W, wr += dwdy) {
            ROW_SPAN();
            uint16_t *fb = tl->c + row0;
            int32_t *zb = tl->z + row0;
            int32_t w = (int32_t)(wr + dwdx * ((float)xs + 0.5f - x0));
            for (int i = xs; i < xe; i++, w += dw)
                if (w > zb[i]) {
                    zb[i] = w;
                    fb[i] = flat;
                }
        }
        return;
    }
    GRADS(p->r, 65536.f, r0, drdx, drdy);
    GRADS(p->g, 65536.f, g0, dgdx, dgdy);
    GRADS(p->b, 65536.f, b0, dbdx, dbdy);
#undef GRADS
    int32_t dr = (int32_t)drdx, dg = (int32_t)dgdx, db = (int32_t)dbdx;
    float rr = r0 + drdy * (py - y0), gr = g0 + dgdy * (py - y0), br = b0 + dbdy * (py - y0);

    for (int y = ys; y < ye; y++, xa += dxa, xb += dxb, row0 += W, wr += dwdy, rr += drdy, gr += dgdy, br += dbdy) {
        ROW_SPAN();
        int n = xe - xs;
        if (n <= 0)
            continue;
        uint16_t *fb = tl->c + row0 + xs;
        int32_t *zb = tl->z + row0 + xs;
        float dx = (float)xs + 0.5f - x0;
        int32_t w = (int32_t)(wr + dwdx * dx);
        int32_t cr, cg, cb, sr, sg, sb2;
        span_c(rr + drdx * dx, dr, n, &cr, &sr);
        span_c(gr + dgdx * dx, dg, n, &cg, &sg);
        span_c(br + dbdx * dx, db, n, &cb, &sb2);
        if (mode == M_FLOOR) {
            uint8_t *sb = tl->s + row0 + xs;
            for (int i = 0; i < n; i++, w += dw, cr += sr, cg += sg, cb += sb2) {
                zb[i] = w;
                sb[i] = 1;
                fb[i] = pack565(cr, cg, cb);
            }
        } else {
            for (int i = 0; i < n; i++, w += dw, cr += sr, cg += sg, cb += sb2)
                if (w > zb[i]) {
                    zb[i] = w;
                    fb[i] = pack565(cr, cg, cb);
                }
        }
    }
}
#undef ROW_SPAN

/* a line, one pixel per step along its longer axis: position, depth and
 * colours stepped in 16.16 (depth in its own units) */
E3D_FAST static void raster_line(const e3d_t *e, const prim_t *p, tile_t *tl)
{
    int dx4 = p->x[1] - p->x[0], dy4 = p->y[1] - p->y[0];
    int ax = dx4 < 0 ? -dx4 : dx4, ay = dy4 < 0 ? -dy4 : dy4;
    int steps = ((ax > ay ? ax : ay) >> 4) + 1;
    int32_t sx = (dx4 << 12) / steps, sy = (dy4 << 12) / steps, sw = (p->w[1] - p->w[0]) / steps;
    int32_t sr = ((p->r[1] - p->r[0]) << 16) / steps, sg = ((p->g[1] - p->g[0]) << 16) / steps,
            sb = ((p->b[1] - p->b[0]) << 16) / steps;
    /* only the steps whose rows are in the band (a step either side to spare) */
    int i0 = 0, i1 = steps;
    if (sy != 0) {
        int32_t ya = p->y[0] << 12;
        int ta = (int)(((int64_t)(tl->y0 << 16) - ya) / sy), tb = (int)(((int64_t)(tl->y1 << 16) - ya) / sy);
        if (ta > tb) { int q = ta; ta = tb; tb = q; }
        i0 = ta - 1 > 0 ? ta - 1 : 0;
        i1 = tb + 1 < steps ? tb + 1 : steps;
    }
    int32_t x = (p->x[0] << 12) + sx * i0, y = (p->y[0] << 12) + sy * i0, w = p->w[0] + sw * i0;
    int32_t r = (p->r[0] << 16) + 0x8000 + sr * i0, g = (p->g[0] << 16) + 0x8000 + sg * i0,
            b = (p->b[0] << 16) + 0x8000 + sb * i0;
    int W = e->w, yt0 = tl->y0, yt1 = tl->y1;
    for (int i = i0; i <= i1; i++, x += sx, y += sy, w += sw, r += sr, g += sg, b += sb) {
        int px = x >> 16, py = y >> 16;
        if ((unsigned)px >= (unsigned)W || py < yt0 || py >= yt1)
            continue;
        int at = (py - yt0) * W + px;
        if (w >= tl->z[at]) {
            tl->z[at] = w;
            tl->c[at] = PACK_RGB(r >> 16, g >> 16, b >> 16);
        }
    }
}

static void raster_glow(const e3d_t *e, const prim_t *p, tile_t *tl)
{
    float cx = p->x[0] * (1.f / SUB), cy = p->y[0] * (1.f / SUB), R = p->x[1] * (1.f / SUB);
    int32_t w = p->w[0];
    int y0 = (int)(cy - R), y1 = (int)(cy + R) + 1, x0 = (int)(cx - R), x1 = (int)(cx + R) + 1;
    if (y0 < tl->y0) y0 = tl->y0;
    if (y1 > tl->y1) y1 = tl->y1;
    if (x0 < 0) x0 = 0;
    if (x1 > e->w) x1 = e->w;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float ddx = (float)x + 0.5f - cx, ddy = (float)y + 0.5f - cy;
            float d = sqrtf(ddx * ddx + ddy * ddy) / R;
            int at = (y - tl->y0) * e->w + x;
            if (d >= 1.f || w < tl->z[at])
                continue;
            float k = (1.f - d) * (1.f - d);
            float hot = d < 0.25f ? (0.25f - d) * 4.f : 0.f;   /* white core */
            uint16_t *px = &tl->c[at];
            int r = (*px >> 11) << 3, g = ((*px >> 5) & 63) << 2, b = (*px & 31) << 3;
            r += (int)(p->r[0] * k + 255.f * hot);
            g += (int)(p->g[0] * k + 255.f * hot);
            b += (int)(p->b[0] * k + 255.f * hot);
            *px = PACK_RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
        }
}

/* Fog by depth without a divide per pixel: z (1/x in 2.30) as a float, its
 * exponent and top 6 mantissa bits index a table of factors, made once per
 * density. A step is 1.6% in depth. */
static inline int fog_zi(int32_t z)
{
    union { float f; uint32_t u; } v = { (float)z };
    return (int)(v.u >> 17) - (127 << 6);            /* z >= 1: exponent 127 and up */
}

static void fog_table(e3d_t *e)
{
    float k = e->par.fog_density * 32.f * WONE;   /* depth * density * 32 = k / z */
    if (k == e->fog_zk)
        return;
    e->fog_zk = k;
    for (int i = 0; i < FOG_ZN; i++) {
        union { uint32_t u; float f; } v = { ((uint32_t)(i + (127 << 6)) << 17) | (1u << 16) };   /* mid-step */
        float idx = k / v.f;
        e->fog_z[i] = idx >= 256.f ? 0 : e->fog_lut[(int)idx];
    }
}

E3D_FAST static void fog_tile(const e3d_t *e, tile_t *tl)
{
    /* RGB565 spread to 0x07E0F81F (green in the top half) so one multiply blends all three */
    uint32_t fc = e->par.fog_rgb565;
    fc = (fc | (fc << 16)) & 0x07E0F81Fu;
    uint32_t fpart[33];                                 /* the fog's share, by factor */
    for (int f = 0; f <= 32; f++)
        fpart[f] = fc * (uint32_t)(32 - f);
    int n = (tl->y1 - tl->y0) * e->w;
    bool keep = e->par.fog_keep_bg;
    const uint8_t *fz = e->fog_z;
    uint16_t *cp = tl->c;
    const int32_t *zp = tl->z;
    for (int i = 0; i < n; i++) {
        int32_t z = zp[i];
        int f;
        if (z > 0)
            f = fz[fog_zi(z)];
        else if (keep)
            continue;                                   /* a painted background fogs itself */
        else
            f = 0;                                      /* background: all fog */
        uint32_t c = cp[i];
        c = (c | (c << 16)) & 0x07E0F81Fu;
        c = ((c * (uint32_t)f + fpart[f]) >> 5) & 0x07E0F81Fu;
        cp[i] = (uint16_t)(c | (c >> 16));
    }
}

/* Block fill and copy, n bytes. With the P4's PIE unit (-march=..._xesppie)
 * they use 128-bit stores when both ends are 16-byte aligned and n is a
 * multiple of 64. */
#if defined(__riscv_xesppie)
static inline bool pie_ok(const void *a, const void *b, size_t n)
{
    return !(((uintptr_t)a | (uintptr_t)b) & 15) && !(n & 63);
}

E3D_FAST static void fill32(void *dst, uint32_t v, size_t n)
{
    if (!pie_ok(dst, dst, n)) {
        uint32_t *d = dst;
        for (size_t i = 0; i < n / 4; i++)
            d[i] = v;
        return;
    }
    uint32_t vv[4] __attribute__((aligned(16))) = { v, v, v, v };
    const uint32_t *pv = vv;
    __asm__ volatile("esp.vld.128.ip q0, %0, 0" : "+r"(pv) : "m"(vv) : "memory");
    for (size_t i = 0; i < n; i += 64)
        __asm__ volatile("esp.vst.128.ip q0, %0, 16\n\t"
                         "esp.vst.128.ip q0, %0, 16\n\t"
                         "esp.vst.128.ip q0, %0, 16\n\t"
                         "esp.vst.128.ip q0, %0, 16"
                         : "+r"(dst) : : "memory");
}

E3D_FAST static void copy_block(void *dst, const void *src, size_t n)
{
    if (!pie_ok(dst, src, n)) {
        memcpy(dst, src, n);
        return;
    }
    for (size_t i = 0; i < n; i += 64)
        __asm__ volatile("esp.vld.128.ip q0, %0, 16\n\t"
                         "esp.vld.128.ip q1, %0, 16\n\t"
                         "esp.vld.128.ip q2, %0, 16\n\t"
                         "esp.vld.128.ip q3, %0, 16\n\t"
                         "esp.vst.128.ip q0, %1, 16\n\t"
                         "esp.vst.128.ip q1, %1, 16\n\t"
                         "esp.vst.128.ip q2, %1, 16\n\t"
                         "esp.vst.128.ip q3, %1, 16"
                         : "+r"(src), "+r"(dst) : : "memory");
}
#else
static void fill32(void *dst, uint32_t v, size_t n)
{
    if (v == 0) {
        memset(dst, 0, n);
        return;
    }
    uint32_t *d = dst;
    for (size_t i = 0; i < n / 4; i++)
        d[i] = v;
}

static void copy_block(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
#endif

E3D_FAST static void raster_band(e3d_t *e, tile_t *tl, int band)
{
    int W = e->w, y0 = band * TILE_H, y1 = y0 + TILE_H > e->h ? e->h : y0 + TILE_H, n = (y1 - y0) * W;
    tl->y0 = y0;
    tl->y1 = y1;
    if (e->bg_fn) {
        for (int y = y0; y < y1; y++)
            e->bg_fn(tl->c + (y - y0) * W, y, W, e->bg_ctx);
    } else if (e->bg) {
        copy_block(tl->c, e->bg + y0 * W, sizeof(uint16_t) * (size_t)n);
    } else {
        fill32(tl->c, e->par.clear_rgb565 * 0x10001u, sizeof(uint16_t) * (size_t)n);
    }
    fill32(tl->z, 0, sizeof(int32_t) * (size_t)n);
    fill32(tl->s, 0, (size_t)n);
    for (int l = 0; l < NLAYERS; l++)
        for (int k = 0; k < e->threads; k++) {
            const bin_t *bin = &e->geo[k].bins[band * NLAYERS + l];
            const plist_t *list = &e->geo[k].list[l];
            for (int i = 0; i < bin->n; i++) {
                const prim_t *p = prim_at(list, bin->i[i]);
                if (p->mode == M_LINE)
                    raster_line(e, p, tl);
                else if (p->mode == M_GLOW)
                    raster_glow(e, p, tl);
                else
                    raster_tri(e, &e->geo[k], p, tl);
            }
        }
    if (e->par.fog_density > 0.f)
        fog_tile(e, tl);
    if (e->stride == W) {
        copy_block(e->fb + y0 * W, tl->c, sizeof(uint16_t) * (size_t)n);
    } else {
        for (int y = y0; y < y1; y++)
            memcpy(e->fb + y * e->stride, tl->c + (y - y0) * W, sizeof(uint16_t) * (size_t)W);
    }
}

static void raster(e3d_t *e, int tid)
{
    tile_t *tl = &e->tile[tid];
    for (;;) {   /* bands as the threads come for them: the sky's are cheap, the floor's are not */
        int b = __atomic_fetch_add(&e->next_band, 1, __ATOMIC_RELAXED);
        if (b >= e->nbands)
            break;
        raster_band(e, tl, b);
    }
}

static void geo_run(e3d_t *e, int tid)
{
    geo_t *g = &e->geo[tid];
    for (;;) {
        int i = __atomic_fetch_add(&e->next_job, 1, __ATOMIC_RELAXED);
        if (i >= e->njobs)
            break;
        draw_object(e, g, &e->jobs[i]);
    }
}

/* task 0: geometry, 1: rasterising; on both cores when there are 2 threads */
static void run(e3d_t *e, int task)
{
#if E3D_PORT_THREADS
    if (e->threads == 2) {
        e->task = task;
        e3d_port_sem_give(e->go);
        if (task)
            raster(e, 0);
        else
            geo_run(e, 0);
        e3d_port_sem_take(e->done, -1);
        return;
    }
#endif
    if (task)
        raster(e, 0);
    else
        geo_run(e, 0);
}

void e3d_end(e3d_t *e)
{
    if (!e->fb)
        return;
    e->next_job = 0;
    xform(&e->view, e->light.pos, e->vlight);
    run(e, 0);
    int64_t t1 = now_us();
    e->next_band = 0;
    if (e->par.fog_density > 0.f)
        fog_table(e);
    run(e, 1);
    int64_t t2 = now_us();
    for (int k = 0; k < 2; k++) {
        e->st_frame.tris_drawn += e->geo[k].tris_drawn;
        e->st_frame.shadow_tris += e->geo[k].shadow_tris;
    }
    e->st_frame.t_geom_us = (int)(t1 - e->t0);
    e->st_frame.t_raster_us = (int)(t2 - t1);
    e->st = e->st_frame;
}

/* ── Text ────────────────────────────────────────────────────────────────── */

static const char font_chars[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:-/%+";
static const uint8_t font5x7[][7] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { 0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C }, { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 },
    { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 }, { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },
};

void e3d_text(uint16_t *pixels, int stride, int w, int h, int x, int y, const char *s,
              uint16_t color, int scale)
{
    if (scale < 1)
        scale = 1;
    for (; *s; s++, x += 6 * scale) {
        char ch = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        const char *f = strchr(font_chars, ch);
        if (!f || !ch)
            continue;
        const uint8_t *g = font5x7[f - font_chars];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c)))
                    continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + c * scale + sx, py = y + r * scale + sy;
                        if (px >= 0 && py >= 0 && px < w && py < h)
                            pixels[py * stride + px] = color;
                    }
            }
    }
}
