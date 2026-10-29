/* lap3d scene ownership and allocation-free frame recording. */
#include "l3_private.h"
#include "ls_cold.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#else
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#endif
void *l3_alloc(size_t n) {
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}
uint64_t l3_clock(void) {
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#elif defined(_WIN32)
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (uint64_t)c.QuadPart * 1000000u / (uint64_t)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}
static __attribute__((noinline)) void camera(l3_scene *s) {
    int cw = 10, ch = 17;
    lap_cell_px(&cw, &ch);
    float aspect = cw > 0 && ch > 0 ? (cw * 1.5f) / ch : 1;
    l3_view_setup(&s->camera, s->eye, s->target, s->up, s->fov, s->ortho_h, s->w, s->h, aspect,
                  s->near_z, s->far_z);
    s->revision++;
}
bool l3_reserve(l3_scene *s, int commands, int vertices, int triangles) {
    if (!s || s->recording || commands < 1 || commands > 65534 || vertices < 1 ||
        vertices > 65535 || triangles < 1 || triangles > 262144)
        return false;
    if (commands <= s->cmdcap && vertices <= s->vertcap && triangles <= s->tricap)
        return true;
    commands = commands > s->cmdcap ? commands : s->cmdcap;
    vertices = vertices > s->vertcap ? vertices : s->vertcap;
    triangles = triangles > s->tricap ? triangles : s->tricap;
    l3_cmd *c = l3_alloc((size_t)commands * sizeof(*c));
    l3_vertex *v = l3_alloc((size_t)vertices * sizeof(*v));
    l3_tri *t = l3_alloc((size_t)triangles * sizeof(*t));
    l3_particle *particles = l3_alloc((size_t)triangles * sizeof(*particles));
    l3_wall_column *column = l3_alloc((size_t)(s->w > L3_SHADOW ? s->w : L3_SHADOW)*2*sizeof(*column));
    int bands = ((s->h > L3_SHADOW ? s->h : L3_SHADOW) + 3) / 4;
    int stride = commands + triangles * 2;
    l3_bin *bin = l3_alloc(((size_t)stride+(stride+15)/16)*sizeof(*bin));
    if (!c || !v || !t || !particles || !column || !bin) {
        free(c); free(v); free(t); free(particles); free(column); free(bin);
        return false;
    }
    free(s->particle); free(s->cmd); free(s->vertices); free(s->tri);
    free(s->bin);
    free(s->column); s->column = column;
    s->columncap = (s->w > L3_SHADOW ? s->w : L3_SHADOW)*2;
    s->particle = particles;
    s->cmd = c; s->vertices = v; s->tri = t;
    s->cmdcap = commands; s->vertcap = vertices; s->tricap = triangles;
    s->bin = bin; s->bin_group = bin+stride;
    s->bin_stride = stride; s->bin_bands = bands;
    s->valid = false; s->have_frame = false;
    return true;
}
LS_COLD bool l3_resize(l3_scene *s, int w, int h) {
    if (!s || s->recording || w < 1 || h < 1 || w > 1024 || h > 1024)
        return false;
    if (s->w == w * 2 && s->h == h * 3) {
        int cw = 10, ch = 17;
        lap_cell_px(&cw, &ch);
        if (cw > 0 && ch > 0 && fabsf(s->camera.fx - s->camera.fy * ch / (cw * 1.5f)) > 0.00001f)
            camera(s);
        return true;
    }
    size_t n = (size_t)w * 2 * h * 3;
    l3_pixel *p = l3_alloc(n * sizeof(*p)), *c = l3_alloc(n * sizeof(*c));
    l3_key *keys = l3_alloc(n * sizeof(*keys));
    l3_land *land = l3_alloc(n * 2 * sizeof(*land));
    int columncap = (w*2 > L3_SHADOW ? w*2 : L3_SHADOW)*2;
    l3_wall_column *columns = l3_alloc((size_t)columncap*sizeof(*columns));
    /* Two two-row tiles fit both workers below 24 KiB at 232 dots wide.
     * Scheduling still claims four rows, keeping worker handoffs unchanged. */
    size_t tile_bytes = (size_t)w * 2 * 2 * (sizeof(l3_pixel) + sizeof(l3_surface));
    void *scratch[2] = {NULL, NULL};
    bool internal[2] = {false, false};
    size_t staged_internal = s->internal_bytes;
    for (int i = 0; i < 2; i++) {
#ifdef ESP_PLATFORM
        if (tile_bytes * 2 <= 24 * 1024 && staged_internal + tile_bytes <= 24 * 1024) {
            scratch[i] = heap_caps_malloc(tile_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            internal[i] = scratch[i] != NULL;
        }
#else
        if (tile_bytes * 2 <= 24 * 1024 && staged_internal + tile_bytes <= 24 * 1024) scratch[i] = malloc(tile_bytes);
        internal[i] = scratch[i] != NULL; /* host models successful internal allocation */
#endif
        if (internal[i]) staged_internal += tile_bytes;
        if (!scratch[i]) scratch[i] = l3_alloc(tile_bytes);
    }
    uint8_t *d = l3_alloc(n), *dirty = l3_alloc(n), *previous = l3_alloc(n);
    l3_lighting *lighting[2];
    for (int i = 0; i < 2; i++) lighting[i] = l3_alloc((size_t)w*4*sizeof(l3_lighting));
    atomic_uchar *tile_ready = l3_alloc((size_t)(h*3+1)/2*sizeof(*tile_ready));
    if (!lighting[0] || !lighting[1] || !tile_ready ||
        !columns || !land || !keys || !scratch[0] || !scratch[1] || !p || !c || !d || !dirty || !previous) {
        free(lighting[0]); free(lighting[1]); free(tile_ready); free(land); free(columns);
        free(keys);
        free(scratch[0]);
        free(scratch[1]);
        free(dirty);
        free(previous);
        free(p);
        free(c);
        free(d);
        return false;
    }
    memset(p, 0, n * sizeof(*p));
    memset(c, 0, n * sizeof(*c));
    memset(keys, 0, n * sizeof(*keys));
    memset(d, LAP_DOT_CLEAR, n);
    memset(dirty, 0, n);
    memset(previous, 0, n);
    free(s->pixels);
    free(s->cache);
    free(s->dots);
    free(s->dirty_mask);
    free(s->previous_mask);
    free(s->keys);
    s->keys = keys;
    for (int i = 0; i < 2; i++) free(s->scratch[i]);
    /* Once the old scene buffers are gone, upgrade staged fallbacks without
     * temporarily consuming two scenes' worth of internal RAM. */
    for (int i = 0; i < 2; i++) if (!internal[i] && tile_bytes*2 <= 24*1024) {
#ifdef ESP_PLATFORM
        void *local = heap_caps_malloc(tile_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
        void *local = malloc(tile_bytes);
#endif
        if (local) { free(scratch[i]); scratch[i] = local; internal[i] = true; }
    }
    for (int i = 0; i < 2; i++) {
        s->scratch[i] = scratch[i];
        s->scratch_internal[i] = internal[i];
        s->work[i].pixels = scratch[i];
        s->work[i].surface = (l3_surface *)((l3_pixel *)scratch[i] + w * 2 * 2);
    }
    s->internal_bytes = (internal[0] + internal[1]) * tile_bytes;
    s->lighting_internal = 0;
    for (int i = 0; i < 2; i++) {
        free(s->lighting[i]);
        size_t bytes = (size_t)w * 4 * sizeof(l3_lighting);
        void *local = NULL;
        if (s->internal_bytes + s->lighting_internal + bytes <= 24*1024) {
#ifdef ESP_PLATFORM
            local = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
            local = malloc(bytes);
#endif
        }
        if (local) s->lighting_internal += bytes;
        if (local) free(lighting[i]);
        s->lighting[i] = local ? local : lighting[i];
        s->work[i].lighting = s->lighting[i];

    }
    free(s->land); s->land = land;
    free(s->column); s->column = columns; s->columncap = columncap;
    free(s->tile_ready);
    s->tile_ready = tile_ready;
    s->camera.w = 0;
    s->pixels = p;
    s->cache = c;
    s->dots = d;
    s->dirty_mask = dirty;
    s->previous_mask = previous;
    s->bin_bands = (h*3 > L3_SHADOW ? h*3 : L3_SHADOW)+3;
    s->bin_bands /= 4;
    s->w = w * 2;
    s->h = h * 3;
    s->valid = false;
    s->have_frame = false;
    camera(s);
    return true;
}
l3_scene *l3_create(int w, int h) {
    l3_scene *s = l3_alloc(sizeof(*s));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->eye = l3v(0, 2, 6);
    s->target = l3v(0, 0, 0);
    s->up = l3v(0, 1, 0);
    s->fov = 40;
    s->ortho_h = 6;
    s->near_z = 0.05f;
    s->far_z = 1000;
    s->sun = l3_norm(l3v(-0.5f, -1, -0.7f));
    s->sun_strength = 0.8f;
    s->ambient = 0.2f;
    s->outline = LAP_DOT_CLEAR;
    s->sky_top = s->sky_bottom = LAP_DOT_CLEAR;
    if (!l3_reserve(s, 256, 1024, 2048) || !l3_resize(s, w, h)) {
        l3_destroy(s);
        return NULL;
    }
    l3_worker_acquire();
    /* Test the DMA margin after allocating the band's internal scratch and
     * setting up the shared worker, so a successful shadow allocation leaves
     * the requested 48 KiB contiguous internal block. */
    size_t shadow_bytes = L3_SHADOW * L3_SHADOW * sizeof(*s->shadow);
#ifdef ESP_PLATFORM
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= shadow_bytes + 48*1024) {
        s->shadow = heap_caps_malloc(shadow_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s->shadow_internal = s->shadow != NULL;
    }
#endif
    if (!s->shadow) s->shadow = l3_alloc(shadow_bytes);
    if (!s->shadow) {
        l3_destroy(s);
        return NULL;
    }
    return s;
}
void l3_destroy(l3_scene *s) {
    if (!s)
        return;
    if (s->w)
        l3_worker_release();
    for (int i = 0; i < s->ntex; i++)
        free(s->texture[i].data);
    free(s->land);
    free(s->pixels);
    free(s->cache);
    free(s->dots);
    free(s->dirty_mask);
    free(s->previous_mask);
    free(s->shadow);
    free(s->tile_ready);
    free(s->lighting[0]); free(s->lighting[1]);
    free(s->bin); free(s->particle); free(s->column);
    free(s->keys);
    free(s->scratch[0]);
    free(s->scratch[1]);
    free(s->cmd);
    free(s->vertices);
    free(s->tri);
    free(s);
}
void l3_size(const l3_scene *s, int *w, int *h) {
    if (w)
        *w = s ? s->w : 0;
    if (h)
        *h = s ? s->h : 0;
}
void l3_camera(l3_scene *s, l3_vec eye, l3_vec target, float fov) {
    if (!s)
        return;
    fov = fov == 0 ? 0 : l3_clamp(fov, 1, 175);
    if (memcmp(&s->eye, &eye, sizeof eye) || memcmp(&s->target, &target, sizeof target) ||
        s->fov != fov) {
        s->eye = eye;
        s->target = target;
        s->fov = fov;
        camera(s);
    }
}
void l3_camera_ortho(l3_scene *s, l3_vec eye, l3_vec target, float height) {
    if (!s || height <= 0)
        return;
    if (s->ortho_h != height) {
        s->ortho_h = height;
        camera(s);
    }
    l3_camera(s, eye, target, 0);
}
void l3_camera_up(l3_scene *s, l3_vec up) {
    if (s && memcmp(&s->up, &up, sizeof up)) {
        s->up = up;
        camera(s);
    }
}
void l3_clip(l3_scene *s, float near_z, float far_z) {
    if (s && near_z > 0 && far_z > near_z && (s->near_z != near_z || s->far_z != far_z)) {
        s->near_z = near_z;
        s->far_z = far_z;
        camera(s);
    }
}
void l3_sun(l3_scene *s, l3_vec dir, float strength, float ambient) {
    if (!s)
        return;
    dir = l3_norm(dir);
    strength = fmaxf(0, strength);
    ambient = l3_clamp(ambient, 0, 1);
    if (memcmp(&s->sun, &dir, sizeof dir) || s->sun_strength != strength || s->ambient != ambient) {
        s->sun = dir;
        s->sun_strength = strength;
        s->ambient = ambient;
        s->revision++;
    }
}
bool l3_lamp(l3_scene *s, int slot, l3_vec pos, float strength, float range) {
    if (!s || slot < 0 || slot >= 4 || (strength > 0 && range <= 0))
        return false;
    if (memcmp(&s->lamp[slot].pos, &pos, sizeof pos) || s->lamp[slot].strength != strength ||
        s->lamp[slot].range != range) {
        s->lamp[slot].pos = pos;
        s->lamp[slot].strength = fmaxf(0, strength);
        s->lamp[slot].range = range;
        s->revision++;
    }
    return true;
}
void l3_shadows(l3_scene *s, bool on) {
    if (s && s->shadows != on) {
        s->shadows = on;
        s->revision++;
    }
}
void l3_sky(l3_scene *s, uint8_t top, uint8_t bottom) {
    if (s && (s->sky_top != top || s->sky_bottom != bottom)) {
        s->sky_top = top;
        s->sky_bottom = bottom;
        s->revision++;
    }
}
void l3_fog(l3_scene *s, uint8_t ink, float start, float end) {
    if (s && (s->fog_ink != ink || s->fog_start != start || s->fog_end != end)) {
        s->fog_ink = ink;
        s->fog_start = start;
        s->fog_end = end;
        s->revision++;
    }
}
void l3_outline(l3_scene *s, uint8_t ink) {
    if (s && s->outline != ink) {
        s->outline = ink;
        s->revision++;
    }
}
int l3_material(l3_scene *s, const uint8_t *inks, int n, float spec, uint32_t flags) {
    if (!s || !inks || n < 1 || n > 8 || s->nmat == 32 || s->recording)
        return -1;
    l3_mat *m = &s->material[s->nmat];
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < n; i++)
        m->ink[i] = inks[i] & 15;
    m->n = (uint8_t)n;
    m->spec = l3_clamp(spec, 0, 1);
    m->flags = flags;
    m->texture = -1;
    s->revision++;
    return s->nmat++;
}
int l3_texture(l3_scene *s, int w, int h, const uint8_t *texels) {
    if (!s || !texels || s->recording || w < 1 || h < 1 || w > 4096 || h > 4096 ||
        s->ntex == L3_TEXTURES)
        return -1;
    uint8_t *data = l3_alloc((size_t)w * h);
    if (!data)
        return -1;
    memcpy(data, texels, (size_t)w * h);
    s->texture[s->ntex] = (l3_tex){w, h, data, 0.42f};
    s->revision++;
    return s->ntex++;
}
void l3_texture_dot(l3_scene *s, int texture, float radius) {
    if (s && texture >= 0 && texture < s->ntex && !s->recording) {
        s->texture[texture].dot = l3_clamp(radius, 0.1f, 1.4f);
        s->revision++;
    }
}
void l3_material_texture(l3_scene *s, int mat, int texture) {
    if (s && mat >= 0 && mat < s->nmat && texture >= -1 && texture < s->ntex &&
        s->material[mat].texture != texture) {
        s->material[mat].texture = texture;
        s->revision++;
    }
}
void l3_begin(l3_scene *s) {
    if (!s)
        return;
    s->recording = true;
    s->ncmd = s->ntri = s->nvert = 0;
    s->flags = 0;
    s->tag = 0;
    uint32_t cores = s->stats.cores;
    memset(&s->stats, 0, sizeof s->stats);
    s->stats.cores = cores;
}
void l3_flags(l3_scene *s, uint32_t flags) {
    if (s)
        s->flags = flags;
}
void l3_tag(l3_scene *s, int tag) {
    if (s)
        s->tag = (uint8_t)l3_clamp((float)tag, 0, 255);
}
l3_cmd *l3_record(l3_scene *s, int kind, int mat) {
    if (!s || !s->recording)
        return NULL;
    if (s->ncmd == s->cmdcap || (kind < S_LINE && (mat < 0 || mat >= s->nmat))) {
        s->stats.dropped++;
        return NULL;
    }
    l3_cmd *c = &s->cmd[s->ncmd++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->mat = mat;
    c->tag = s->tag;
    c->flags = s->flags;
    c->t = c->inv = l3_identity();
    return c;
}
static l3_cmd *shape(l3_scene *s, int kind, const l3_xform *t, l3_vec half, float r, float r2,
                     int mat) {
    l3_cmd *c = l3_record(s, kind, mat);
    if (!c)
        return NULL;
    if (t) {
        c->t = *t;
        if (!l3_inverse(&c->inv, t)) {
            s->ncmd--;
            return NULL;
        }
    }
    c->half = l3v(fabsf(half.x), fabsf(half.y), fabsf(half.z));
    c->r = fabsf(r);
    c->r2 = fabsf(r2);
    return c;
}
void l3_sphere(l3_scene *s, l3_vec p, float r, int mat) {
    l3_cmd *c = shape(s, S_SPHERE, NULL, l3v(0, 0, 0), r, 0, mat);
    if (c)
        c->a = p;
}
void l3_ellipsoid(l3_scene *s, const l3_xform *t, l3_vec radii, int mat) {
    shape(s, S_ELLIPSOID, t, radii, 0, 0, mat);
}
void l3_box(l3_scene *s, const l3_xform *t, l3_vec half, int mat) {
    shape(s, S_BOX, t, half, 0, 0, mat);
}
void l3_rbox(l3_scene *s, const l3_xform *t, l3_vec half, float round, int mat) {
    l3_cmd *c = shape(s, S_RBOX, t, half, round, 0, mat);
    if (c)
        c->r = fminf(c->r, fminf(c->half.x, fminf(c->half.y, c->half.z)));
}
static __attribute__((noinline)) void segment_frame(l3_cmd *c) {
    c->half.y = l3_length(l3_sub(c->b, c->a)) * 0.5f;
    if (c->half.y < 1.0e-6f) {
        c->kind = S_SPHERE;
        return;
    }
    c->c = l3_norm(l3_sub(c->b, c->a));
    c->d = fabsf(c->c.y) < 0.9f ? l3v(0, 1, 0) : l3v(1, 0, 0);
    c->d = l3_cross(c->d, c->c);
    c->d = l3_norm(c->d);
    c->center = l3_cross(c->d, c->c);
    c->t.m[0] = c->d.x;
    c->t.m[1] = c->c.x;
    c->t.m[2] = c->center.x;
    c->t.m[3] = c->d.y;
    c->t.m[4] = c->c.y;
    c->t.m[5] = c->center.y;
    c->t.m[6] = c->d.z;
    c->t.m[7] = c->c.z;
    c->t.m[8] = c->center.z;
    c->t.m[9] = (c->a.x + c->b.x) * 0.5f;
    c->t.m[10] = (c->a.y + c->b.y) * 0.5f;
    c->t.m[11] = (c->a.z + c->b.z) * 0.5f;
    l3_inverse(&c->inv, &c->t);
}
static __attribute__((noinline)) void segment(l3_scene *s, int kind, l3_vec a, l3_vec b, float ra,
                                              float rb, int mat) {
    l3_cmd *c = shape(s, kind, NULL, l3v(0, 0, 0), ra, rb, mat);
    if (!c)
        return;
    c->a = a;
    c->b = b;
    segment_frame(c);
}
void l3_cylinder(l3_scene *s, l3_vec a, l3_vec b, float r, int mat) {
    segment(s, S_CYLINDER, a, b, r, r, mat);
}
void l3_capsule(l3_scene *s, l3_vec a, l3_vec b, float r, int mat) {
    segment(s, S_CAPSULE, a, b, r, r, mat);
}
void l3_cone(l3_scene *s, l3_vec a, l3_vec b, float ra, float rb, int mat) {
    segment(s, S_CONE, a, b, ra, rb, mat);
}
void l3_torus(l3_scene *s, const l3_xform *t, float ring, float tube, int mat) {
    shape(s, S_TORUS, t, l3v(0, 0, 0), ring, tube, mat);
}
void l3_disc(l3_scene *s, l3_vec p, l3_vec normal, float r, int mat) {
    l3_cmd *c = shape(s, S_DISC, NULL, l3v(0, 0, 0), r, 0, mat);
    if (c) {
        c->a = p;
        c->b = l3_norm(normal);
    }
}
void l3_floor(l3_scene *s, float y, int mat) {
    l3_cmd *c = shape(s, S_FLOOR, NULL, l3v(0, 0, 0), 0, 0, mat);
    if (c)
        c->a.y = y;
}
void l3_quad(l3_scene *s, l3_vec a, l3_vec b, l3_vec c, l3_vec d, int mat) {
    l3_cmd *o = l3_record(s, S_QUAD, mat);
    if (o) {
        o->a = a;
        o->b = b;
        o->c = c;
        o->d = d;
        o->half = l3_norm(l3_cross(l3_sub(b, a), l3_sub(c, a)));
    }
}
void l3_draw_mesh(l3_scene *s, const l3_mesh *m, const l3_xform *t, int mat) {
    if (!m || !m->v || !m->idx || m->nv < 1 || m->ntri < 1)
        return;
    l3_cmd *c = shape(s, S_MESH, t, l3v(0, 0, 0), 0, 0, mat);
    if (c)
        c->mesh = *m;
}
void l3_line(l3_scene *s, l3_vec a, l3_vec b, uint8_t ink) {
    l3_cmd *c = l3_record(s, S_LINE, 0);
    if (c) {
        c->a = a;
        c->b = b;
        c->ink = ink;
    }
}
void l3_point(l3_scene *s, l3_vec p, float size, uint8_t ink) {
    l3_cmd *c = l3_record(s, S_POINT, 0);
    if (c) {
        c->a = p;
        c->r = fmaxf(1, size);
        c->ink = ink;
    }
}
void l3_sprite(l3_scene *s, l3_vec p, float height, const char *const *rows, int nrows,
               const char *keys, const uint8_t *inks) {
    if (!rows || !keys || !inks || nrows < 1 || height <= 0)
        return;
    l3_cmd *c = l3_record(s, S_SPRITE, 0);
    if (c) {
        c->a = p;
        c->r = height;
        c->rows = rows;
        c->nrows = nrows;
        c->keys = keys;
        c->inks = inks;
    }
}
void l3_label(l3_scene *s, l3_vec p, const char *text, int scale, uint8_t ink, uint8_t edge) {
    if (!text || scale < 1 || scale > 64)
        return;
    l3_cmd *c = l3_record(s, S_LABEL, 0);
    if (c) {
        c->a = p;
        c->text = text;
        c->scale = scale;
        c->ink = ink;
        c->edge = edge;
    }
}
uint8_t *l3_dots(l3_scene *s) { return s ? s->dots : NULL; }
void l3_show(const l3_scene *s, int x, int y) {
    if (s)
        lap_dots(x, y, s->w / 2, s->h / 3, s->dots);
}
int l3_pick(const l3_scene *s, int x, int y) {
    return s && (unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h
               ? s->pixels[y * s->w + x].tag
               : 0;
}
float l3_depth(const l3_scene *s, int x, int y) {
    if (!s || (unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h)
        return -1;
    const l3_pixel *p = &s->pixels[y * s->w + x];
    if (!p->object)
        return -1;
    if (!s->camera.ortho) {
        float dx = (x + 0.5f - s->w * 0.5f) * s->camera.inv_fx,
              dy = (s->h * 0.5f - y - 0.5f) * s->camera.inv_fy;
        return p->depth * sqrtf(1 + dx * dx + dy * dy);
    }
    float dx = (x + 0.5f - s->w * 0.5f) / s->camera.fx;
    float dy = (y + 0.5f - s->h * 0.5f) / s->camera.fy;
    return sqrtf(p->depth * p->depth + dx * dx + dy * dy);
}
void l3_stats(const l3_scene *s, l3_stats_t *out) {
    if (s && out)
        *out = s->stats;
}

/* Profile output is opt-in; normal frames do no timing or traffic accounting. */
#ifdef L3_PROFILE
#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif
#endif
void l3_profile_frame(l3_scene *s) {
#ifdef L3_PROFILE
    s->frame_bytes = s->counts[0].bytes + s->counts[1].bytes;
    s->psram_bytes = sizeof(*s) + (size_t)s->w*s->h*23
        + (s->shadow_internal ? 0 : L3_SHADOW*L3_SHADOW*sizeof(uint16_t))
        + ((size_t)s->bin_stride+(s->bin_stride+15)/16)*sizeof(l3_bin)
        + (size_t)s->w*s->h*2*sizeof(l3_land) + (size_t)(s->h+1)/2*sizeof(*s->tile_ready)
        + (size_t)s->tricap*sizeof(l3_particle) + (size_t)s->columncap*sizeof(l3_wall_column)
        + (size_t)s->w*4*2*sizeof(l3_lighting)-s->lighting_internal
        + (size_t)s->cmdcap*sizeof(l3_cmd) + (size_t)s->vertcap*sizeof(l3_vertex)
        + (size_t)s->tricap*sizeof(l3_tri);
    for (int i = 0; i < s->ntex; i++) s->psram_bytes += (size_t)s->texture[i].w*s->texture[i].h;
    for (int i = 0; i < 2; i++)
        if (!s->scratch_internal[i]) s->psram_bytes += (size_t)s->w*2*24;
    s->profile_us[0] += s->stats.us_shadow;
    s->profile_us[1] += s->counts[0].us_raster + s->counts[1].us_raster;
    s->profile_us[2] += s->counts[0].us_shade + s->counts[1].us_shade;
    s->profile_us[3] += s->stats.us_resolve;
    s->profile_us[4] += s->counts[0].us_cache + s->counts[1].us_cache;
    for (int i = 0; i < 7; i++) s->profile_kind[i] += s->counts[0].kind_us[i]+s->counts[1].kind_us[i];
    if (++s->profile_frames != 50) return;
#ifdef ESP_PLATFORM
    ESP_LOGW("lap3d", "50 frames: shadow %llu raster_cpu %llu shade_cpu %llu resolve %llu cache_cpu %llu us; internal %u PSRAM %u B",
#else
    printf("lap3d 50 frames: shadow %llu raster_cpu %llu shade_cpu %llu resolve %llu cache_cpu %llu us; internal %u PSRAM %u B\n",
#endif
        (unsigned long long)(s->profile_us[0]/50), (unsigned long long)(s->profile_us[1]/50),
        (unsigned long long)(s->profile_us[2]/50), (unsigned long long)(s->profile_us[3]/50),
        (unsigned long long)(s->profile_us[4]/50), (unsigned)(s->internal_bytes+s->lighting_internal+
            (s->shadow_internal ? L3_SHADOW*L3_SHADOW*sizeof(uint16_t) : 0)), (unsigned)s->psram_bytes);
#ifdef ESP_PLATFORM
    ESP_LOGW("lap3d", "raster us: sphere %llu box/flat %llu mesh %llu terrain %llu walls %llu particles %llu floor %llu",
#else
    printf("lap3d raster us: sphere %llu box/flat %llu mesh %llu terrain %llu walls %llu particles %llu floor %llu\n",
#endif
        (unsigned long long)(s->profile_kind[0]/50), (unsigned long long)(s->profile_kind[1]/50),
        (unsigned long long)(s->profile_kind[2]/50), (unsigned long long)(s->profile_kind[3]/50),
        (unsigned long long)(s->profile_kind[4]/50), (unsigned long long)(s->profile_kind[5]/50),
        (unsigned long long)(s->profile_kind[6]/50));
    memset(s->profile_kind,0,sizeof(s->profile_kind));
    s->profile_frames = 0;
    memset(s->profile_us, 0, sizeof(s->profile_us));
#else
    (void)s;
#endif
}
