/* Frame preparation, conservative dirty regions and sun-map orchestration. */
#include "l3_private.h"
#include <stddef.h>
static __attribute__((noinline)) l3_rect empty(void) {
    return (l3_rect){INT32_MAX, INT32_MAX, 0, 0};
}
static __attribute__((noinline)) l3_rect unite(l3_rect a, l3_rect b) {
    if (b.x0 >= b.x1 || b.y0 >= b.y1)
        return a;
    if (a.x0 >= a.x1 || a.y0 >= a.y1)
        return b;
    return (l3_rect){a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                     a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
}
static __attribute__((noinline)) l3_rect screen(const l3_view *v, l3_vec center, float r) {
    l3_vec p = l3_sub(center, v->eye);
    float z = l3_dot(p, v->forward), x = l3_dot(p, v->right), y = l3_dot(p, v->up);
    if (z + r < v->near_z || z - r > v->far_z)
        return empty();
    if (!v->ortho && z - r < v->near_z)
        return (l3_rect){0, 0, v->w, v->h};
    float xl = x - r, xh = x + r, yl = y - r, yh = y + r;
    if (!v->ortho) {
        float a = 1 / (z - r), b = 1 / (z + r);
        xl = fminf((x - r) * a, (x - r) * b);
        xh = fmaxf((x + r) * a, (x + r) * b);
        yl = fminf((y - r) * a, (y - r) * b);
        yh = fmaxf((y + r) * a, (y + r) * b);
    }
    float x0 = l3_clamp(v->w * 0.5f - 1.5f + xl * v->fx, 0, (float)v->w),
          x1 = l3_clamp(v->w * 0.5f + 1.5f + xh * v->fx, 0, (float)v->w),
          y0 = l3_clamp(v->h * 0.5f - 1.5f - yh * v->fy, 0, (float)v->h),
          y1 = l3_clamp(v->h * 0.5f + 1.5f - yl * v->fy, 0, (float)v->h);
    return (l3_rect){(int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1)};
}
static __attribute__((noinline)) void bounds(l3_cmd *c) {
    if (c->kind >= S_TERRAIN && c->kind <= S_PARTICLES) { l3_world_bounds(c); return; }
    c->center = l3_apply(&c->t, l3v(0, 0, 0));
    float r = l3_length(c->half);
    if (c->kind == S_SPHERE || c->kind == S_DISC) {
        c->center = c->a;
        c->shadow_bound = c->bound = c->r;
        return;
    }
    if (c->kind == S_QUAD) {
        c->center = l3v((c->a.x + c->b.x + c->c.x + c->d.x) * 0.25f,
                        (c->a.y + c->b.y + c->c.y + c->d.y) * 0.25f,
                        (c->a.z + c->b.z + c->c.z + c->d.z) * 0.25f);
        c->bound = l3_length(l3_sub(c->a, c->center));
        c->bound = fmaxf(c->bound, l3_length(l3_sub(c->b, c->center)));
        c->bound = fmaxf(c->bound, l3_length(l3_sub(c->c, c->center)));
        c->bound = fmaxf(c->bound, l3_length(l3_sub(c->d, c->center)));
        c->shadow_bound = c->bound;
        return;
    }
    if (c->kind == S_MESH) {
        r = 0;
        for (int i = 0; i < c->mesh.nv; i++)
            r = fmaxf(r, l3_length(c->mesh.v[i]));
    } else if (c->kind == S_TORUS)
        r = c->r + c->r2;
    else if (c->kind == S_CAPSULE)
        r = c->half.y + c->r;
    else if (c->kind == S_CYLINDER || c->kind == S_CONE) {
        float a = fmaxf(c->r, c->r2);
        r = sqrtf(c->half.y * c->half.y + a * a);
    }
    float scale = 0;
    for (int i = 0; i < 9; i++)
        scale += c->t.m[i] * c->t.m[i];
    /* Rigid/uniform poses have orthogonal columns; Frobenius overbounds them
     * by sqrt(3), needlessly tripling projected candidate area. */
    float a = c->t.m[0]*c->t.m[0] + c->t.m[3]*c->t.m[3] + c->t.m[6]*c->t.m[6];
    float b = c->t.m[1]*c->t.m[1] + c->t.m[4]*c->t.m[4] + c->t.m[7]*c->t.m[7];
    float d = c->t.m[2]*c->t.m[2] + c->t.m[5]*c->t.m[5] + c->t.m[8]*c->t.m[8];
    float ab = c->t.m[0]*c->t.m[1] + c->t.m[3]*c->t.m[4] + c->t.m[6]*c->t.m[7];
    float ad = c->t.m[0]*c->t.m[2] + c->t.m[3]*c->t.m[5] + c->t.m[6]*c->t.m[8];
    float bd = c->t.m[1]*c->t.m[2] + c->t.m[4]*c->t.m[5] + c->t.m[7]*c->t.m[8];
    /* Gershgorin bound on A-transpose A, valid even for shear. */
    float eigen = fmaxf(a + fabsf(ab) + fabsf(ad),
                       fmaxf(b + fabsf(ab) + fabsf(bd), d + fabsf(ad) + fabsf(bd)));
    c->bound = r * sqrtf(fminf(scale, eigen));
    c->shadow_bound = r * sqrtf(scale);
}
static __attribute__((noinline)) void sprite_bounds(l3_scene *s, l3_cmd *c, float z) {
    int width = 0;
    for (int i = 0; i < c->nrows; i++) {
        int n = (int)strlen(c->rows[i]);
        if (n > width)
            width = n;
    }
    c->r2 = c->r * s->view.fy / (c->nrows * (s->view.ortho ? 1 : z));
    c->half.x = c->r2 * s->view.fx / s->view.fy;
    c->half.y = (float)width;
    c->c.x -= width * c->half.x * 0.5f;
    c->c.y -= c->nrows * c->r2;
    c->rect =
        (l3_rect){(int)floorf(c->c.x), (int)floorf(c->c.y), (int)ceilf(c->c.x + width * c->half.x),
                  (int)ceilf(c->c.y + c->nrows * c->r2)};
}
static __attribute__((noinline)) void line_project(const l3_view *v, l3_vec *p) {
    float x = p->x - v->eye.x, y = p->y - v->eye.y, z = p->z - v->eye.z;
    float depth = x * v->forward.x + y * v->forward.y + z * v->forward.z,
          k = v->ortho ? 1 : 1 / depth;
    p->x = v->w * 0.5f - 0.5f + (x * v->right.x + y * v->right.y + z * v->right.z) * v->fx * k;
    p->y = v->h * 0.5f - 0.5f - (x * v->up.x + y * v->up.y + z * v->up.z) * v->fy * k;
    p->z = depth;
}
static __attribute__((noinline)) void line_bounds(l3_scene *s, l3_cmd *c) {
    c->rect = empty();
    float a = l3_dot(l3_sub(c->a, s->view.eye), s->view.forward),
          b = l3_dot(l3_sub(c->b, s->view.eye), s->view.forward), t0 = 0, t1 = 1;
    if (fabsf(b - a) < 1.0e-12f) {
        if (a < s->view.near_z || a > s->view.far_z)
            return;
    } else {
        float near_t = (s->view.near_z - a) / (b - a), far_t = (s->view.far_z - a) / (b - a);
        t0 = fmaxf(0, fminf(near_t, far_t));
        t1 = fminf(1, fmaxf(near_t, far_t));
        if (t0 > t1)
            return;
    }
    c->c = l3_add(c->a, l3_scale(l3_sub(c->b, c->a), t0));
    c->d = l3_add(c->a, l3_scale(l3_sub(c->b, c->a), t1));
    line_project(&s->view, &c->c);
    line_project(&s->view, &c->d);
    c->rect = (l3_rect){(int)floorf(l3_clamp(fminf(c->c.x, c->d.x) - 1, 0, (float)s->view.w)),
                        (int)floorf(l3_clamp(fminf(c->c.y, c->d.y) - 1, 0, (float)s->view.h)),
                        (int)ceilf(l3_clamp(fmaxf(c->c.x, c->d.x) + 2, 0, (float)s->view.w)),
                        (int)ceilf(l3_clamp(fmaxf(c->c.y, c->d.y) + 2, 0, (float)s->view.h))};
}
static __attribute__((noinline)) void overlay_prepare(l3_scene *s, l3_cmd *c) {
    if (c->kind == S_LINE) {
        line_bounds(s, c);
        return;
    }
    c->rect = empty();
    float x, y;
    if (!l3_view_project(&s->view, c->a, &x, &y))
        return;
    float z = l3_dot(l3_sub(c->a, s->view.eye), s->view.forward);
    c->c = l3v(x, y, z);
    c->center = c->a;
    c->bound = 0;
    if (c->kind == S_POINT) {
        float r = c->r * 0.5f;
        c->rect = (l3_rect){(int)floorf(x - r), (int)floorf(y - r), (int)ceilf(x + r + 1),
                            (int)ceilf(y + r + 1)};
    } else if (c->kind == S_LABEL) {
        int w = (int)strlen(c->text) * 4 * c->scale;
        c->c.x -= w * 0.5f;
        c->c.y -= 2.5f * c->scale;
        c->rect = (l3_rect){(int)floorf(c->c.x - 1), (int)floorf(c->c.y - 1),
                            (int)ceilf(c->c.x + w + 1), (int)ceilf(c->c.y + 5 * c->scale + 1)};
    } else if (c->kind == S_SPRITE) {
        sprite_bounds(s, c, z);
    }
    if (c->rect.x0 < 0)
        c->rect.x0 = 0;
    if (c->rect.y0 < 0)
        c->rect.y0 = 0;
    if (c->rect.x1 > s->view.w)
        c->rect.x1 = s->view.w;
    if (c->rect.y1 > s->view.h)
        c->rect.y1 = s->view.h;
}
static void include_bounds(l3_scene *s, const l3_cmd *c) {
    float r = c->shadow_bound;
    s->bounds_min.x = fminf(c->center.x-r,s->bounds_min.x);
    s->bounds_min.y = fminf(c->center.y-r,s->bounds_min.y);
    s->bounds_min.z = fminf(c->center.z-r,s->bounds_min.z);
    s->bounds_max.x = fmaxf(c->center.x+r,s->bounds_max.x);
    s->bounds_max.y = fmaxf(c->center.y+r,s->bounds_max.y);
    s->bounds_max.z = fmaxf(c->center.z+r,s->bounds_max.z);
}
/* Receiver bounds for reach(): an axis box per shape instead of its bounding
   sphere. A long cabinet's sphere reached far below the floor, and every
   moving caster's shadow sweep ran down to it. The sun camera keeps the
   sphere bounds, so shadows look exactly as they did. */
static void include_tight(l3_scene *s, const l3_cmd *c) {
    l3_vec lo, hi;
    if (c->kind == S_QUAD) {
        lo = hi = c->a;
        const l3_vec q[3] = {c->b, c->c, c->d};
        for (int i = 0; i < 3; i++) {
            lo = l3v(fminf(lo.x, q[i].x), fminf(lo.y, q[i].y), fminf(lo.z, q[i].z));
            hi = l3v(fmaxf(hi.x, q[i].x), fmaxf(hi.y, q[i].y), fmaxf(hi.z, q[i].z));
        }
    } else if (c->kind == S_CYLINDER || c->kind == S_CONE || c->kind == S_CAPSULE) {
        float r = fmaxf(c->r, c->r2);
        lo = l3v(fminf(c->a.x, c->b.x) - r, fminf(c->a.y, c->b.y) - r, fminf(c->a.z, c->b.z) - r);
        hi = l3v(fmaxf(c->a.x, c->b.x) + r, fmaxf(c->a.y, c->b.y) + r, fmaxf(c->a.z, c->b.z) + r);
    } else if (c->kind == S_BOX || c->kind == S_RBOX || c->kind == S_ELLIPSOID) {
        const float *m = c->t.m;
        l3_vec e = l3v(fabsf(m[0]) * c->half.x + fabsf(m[1]) * c->half.y + fabsf(m[2]) * c->half.z,
                       fabsf(m[3]) * c->half.x + fabsf(m[4]) * c->half.y + fabsf(m[5]) * c->half.z,
                       fabsf(m[6]) * c->half.x + fabsf(m[7]) * c->half.y + fabsf(m[8]) * c->half.z);
        l3_vec o = l3v(m[9], m[10], m[11]);
        lo = l3_sub(o, e);
        hi = l3_add(o, e);
    } else {
        include_bounds(s, c);
        return;
    }
    s->bounds_min = l3v(fminf(lo.x, s->bounds_min.x), fminf(lo.y, s->bounds_min.y), fminf(lo.z, s->bounds_min.z));
    s->bounds_max = l3v(fmaxf(hi.x, s->bounds_max.x), fmaxf(hi.y, s->bounds_max.y), fmaxf(hi.z, s->bounds_max.z));
}
/* Bounds and screen boxes for every command, and the receiver bounds. */
static void prepare_bounds(l3_scene *s) {
    s->walls_first = false;
    for (int i = 0; i < s->ncmd; i++) {
        l3_cmd *c = &s->cmd[i];
        if (s->shadow_pass && !(c->flags & L3_CAST)) { c->rect = empty(); continue; }
        if (c->kind >= S_LINE) {
            overlay_prepare(s, c);
            continue;
        }
        if (c->kind == S_WALLS && !s->ordered && !s->shadow_pass) s->walls_first = true;
        bounds(c);
        c->rect = c->kind == S_FLOOR ? (l3_rect){0, 0, s->view.w, s->view.h}
                                     : screen(&s->view, c->center, c->bound);
    }
    if (s->shadows && !s->shadow_pass) {
        /* Refresh receiver reach even when the caster-keyed sun map is reused.
         * This keeps moving shadows conservative on distant noncasters. */
        s->bounds_min = l3v(L3_INF,L3_INF,L3_INF);
        s->bounds_max = l3v(-L3_INF,-L3_INF,-L3_INF);
        for (int i = 0; i < s->ncmd; i++)
            if (s->cmd[i].kind < S_LINE && s->cmd[i].kind != S_FLOOR) include_tight(s,&s->cmd[i]);
    }
}
/* Generated triangles, particles and wall columns, then the band bins. Only
   dots inside `area` are drawn this pass, so a box, quad, disc or mesh whose
   screen box misses it builds no triangles (a partial frame used to build
   all of them, every frame, for the few dots around a ball). */
static void prepare_raster(l3_scene *s, l3_rect area) {
    s->ntri = 0;
    s->nparticle = s->ncolumn = 0;
    for (int i = 0; i < s->ncmd; i++) {
        l3_cmd *c = &s->cmd[i];
        if (c->kind >= S_LINE || (s->shadow_pass && !(c->flags & L3_CAST))) continue;
        const bool flat = c->kind == S_BOX || c->kind == S_RBOX || c->kind == S_QUAD || c->kind == S_DISC;
        if ((flat || c->kind == S_MESH) &&
            (c->rect.x1 <= area.x0 || c->rect.x0 >= area.x1 || c->rect.y1 <= area.y0 || c->rect.y0 >= area.y1)) {
            c->first = s->ntri;
            c->count = 0;
            continue;
        }
        if (c->kind == S_TERRAIN && s->shadow_pass) l3_terrain_shadow(s, i);
        else if (c->kind == S_MESH) l3_mesh_prepare(s, i);
        else if (flat) l3_flat_prepare(s, i);
        else if (c->kind == S_PARTICLES) l3_particles_prepare(s, i);
        else if (c->kind == S_WALLS) l3_walls_prepare(s, i);
    }
    l3_bins_prepare(s);
}
void l3_prepare(l3_scene *s) {
    prepare_bounds(s);
    prepare_raster(s, (l3_rect){0, 0, s->view.w, s->view.h});
}
/* Two 32-bit lanes over whole words: each step is a bijection, so a single
   changed word always changes the result, and on the P4 (RV32) it costs two
   32-bit multiplies a word where byte-wise FNV-64 cost three a byte. */
static __attribute__((noinline)) uint64_t hash(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t a = (uint32_t)h, b = (uint32_t)(h >> 32);
    for (; n >= 4; n -= 4, p += 4) {
        uint32_t w;
        memcpy(&w, p, 4);
        a = (a ^ w) * 0x01000193u;
        b = (b + w) * 0x85ebca77u;
        b ^= b >> 15;
    }
    for (; n; n--, p++) {
        a = (a ^ *p) * 0x01000193u;
        b = (b + *p) * 0x85ebca77u;
        b ^= b >> 15;
    }
    return (uint64_t)b << 32 | a;
}
#define L3_HASH_BASIS 1469598103934665603ull
/* One command and everything it points at. `inv` follows from `t`; the
   prepared fields (rect, center, bound...) are still zero when this runs. */
static __attribute__((noinline)) uint64_t command_hash(const l3_cmd *c) {
    uint64_t h = hash(L3_HASH_BASIS, c, offsetof(l3_cmd, inv));
    h = hash(h, (const uint8_t *)c + offsetof(l3_cmd, a), sizeof(*c) - offsetof(l3_cmd, a));
    if (c->kind == S_MESH) {
        h = hash(h, c->mesh.v, (size_t)c->mesh.nv * sizeof(l3_vec));
        h = hash(h, c->mesh.idx, (size_t)c->mesh.ntri * 3 * sizeof(uint16_t));
        if (c->mesh.mats)
            h = hash(h, c->mesh.mats, (size_t)c->mesh.ntri);
    }
    if (c->kind == S_TERRAIN || c->kind == S_WALLS) {
        h = hash(h,c->map,(size_t)c->map_w*c->map_d);
        if (c->map_mats) h = hash(h,c->map_mats,(size_t)c->map_w*c->map_d);
    }
    if (c->kind == S_PARTICLES) h = hash(h,c->points,(size_t)c->np*sizeof(l3_vec));
    if (c->kind == S_LABEL)
        h = hash(h, c->text, strlen(c->text));
    if (c->kind == S_SPRITE) {
        h = hash(h, c->keys, strlen(c->keys));
        h = hash(h, c->inks, strlen(c->keys));
        for (int j = 0; j < c->nrows; j++)
            h = hash(h, c->rows[j], strlen(c->rows[j]));
    }
    return h;
}
static inline bool casts(const l3_scene *s, const l3_cmd *c) {
    return s->shadows && (c->flags & L3_CAST) && c->kind < S_LINE && c->kind != S_FLOOR;
}
static __attribute__((noinline)) void shadow_camera(l3_scene *s, bool any) {
    l3_work *w = &s->work[0];
    w->p = any ? l3_scale(l3_add(s->bounds_min, s->bounds_max), 0.5f) : s->target;
    float radius = any ? fmaxf(1, l3_length(l3_sub(s->bounds_max, s->bounds_min)) * 0.5f) : 5;
    w->dir = l3_dot(s->sun, s->sun) > 0 ? s->sun : l3v(0, -1, 0);
    w->o = l3_sub(w->p, l3_scale(w->dir, radius * 2 + 1));
    l3_view_setup(&s->sunview, w->o, w->p, l3v(0, 1, 0), 0, radius * 2.1f, L3_SHADOW, L3_SHADOW, 1,
                  0.01f, radius * 4 + 2);
}
static bool same_view(const l3_view *a, const l3_view *b) {
    return !memcmp(&a->eye, &b->eye, sizeof(l3_vec) * 4) && a->fx == b->fx && a->fy == b->fy &&
           a->near_z == b->near_z && a->far_z == b->far_z && a->w == b->w && a->h == b->h &&
           a->ortho == b->ortho;
}
/* The sun map. In full when it has to be (first frame, the static scene, the
   sun or the sun camera changed), otherwise only inside the sun-view boxes
   of the dynamic casters that changed, where they were and where they are:
   every other texel would come out the same. */
static __attribute__((noinline)) void shadow(l3_scene *s, bool full, const l3_dyn *prev, int nprev,
                                             l3_dyn *cur, int ndyn) {
    s->bounds_min = l3v(L3_INF, L3_INF, L3_INF);
    s->bounds_max = l3v(-L3_INF, -L3_INF, -L3_INF);
    bool any = false;
    for (int i = 0; i < s->ncmd; i++) {
        l3_cmd *c = &s->cmd[i];
        if (c->kind >= S_LINE || c->kind == S_FLOOR)
            continue;
        bounds(c);
        include_bounds(s,c);
        any = true;
    }
    l3_view old = s->sunview;
    shadow_camera(s, any);
    full = full || !same_view(&old, &s->sunview);
    s->shadow_scale = 65534.0f / s->sunview.far_z;
    s->shadow_inv = 1.0f / s->shadow_scale;
    for (int k = 0; k < ndyn; k++) {
        const l3_cmd *c = &s->cmd[cur[k].cmd];
        cur[k].sun = casts(s, c) ? screen(&s->sunview, c->center, c->shadow_bound) : empty();
    }
    if (full)
        s->dirty = (l3_rect){0, 0, L3_SHADOW, L3_SHADOW};
    else {
        s->dirty = empty();
        for (int k = 0; k < ndyn; k++)
            if (cur[k].changed) {
                s->dirty = unite(s->dirty, cur[k].sun);
                if (k < nprev) s->dirty = unite(s->dirty, prev[k].sun);
            }
        for (int k = ndyn; k < nprev; k++)
            s->dirty = unite(s->dirty, prev[k].sun);
    }
    if (s->dirty.x0 < s->dirty.x1 && s->dirty.y0 < s->dirty.y1) {
        s->shadow_pass = true;
        s->view = s->sunview;
        prepare_bounds(s);
        prepare_raster(s, s->dirty);
        l3_run(s, 0);
        s->shadow_pass = false;
    }
}
/* Where a moving caster can change the picture: its own box, and its shadow,
   which lies inside its bounding sphere swept along the sun (a capsule) until
   it leaves the receivers' bounds or reaches a floor. Receiver bounds are
   axis boxes (include_tight), so a sweep stops near the playfield, not far
   below a long cabinet's bounding sphere. */
static __attribute__((noinline)) void sweep(l3_scene *s, const l3_cmd *c, l3_dyn *d, float floor_lo) {
    d->rect = c->rect;
    d->n = 0;
    if (!s->shadows || !(c->flags & L3_CAST) || c->kind >= S_LINE || c->kind == S_FLOOR)
        return;
    const l3_vec dir = s->sun, o = c->center;
    float t = 0;
    const float lo[3] = {s->bounds_min.x - c->bound, s->bounds_min.y - c->bound, s->bounds_min.z - c->bound};
    const float hi[3] = {s->bounds_max.x + c->bound, s->bounds_max.y + c->bound, s->bounds_max.z + c->bound};
    const float od[3][2] = {{o.x, dir.x}, {o.y, dir.y}, {o.z, dir.z}};
    float exit = L3_INF;
    for (int k = 0; k < 3; k++)
        if (fabsf(od[k][1]) > 1e-6f) {
            float te = ((od[k][1] > 0 ? hi[k] : lo[k]) - od[k][0]) / od[k][1];
            if (te < exit) exit = te;
        }
    if (exit < L3_INF) t = exit;
    if (floor_lo < L3_INF && dir.y < -1e-6f) {     /* the lowest floor reaches furthest */
        float tf = (floor_lo - o.y) / dir.y;
        if (tf > t) t = tf;
    }
    t = fminf(t, 1000);
    /* One sun-map texel more: a receiver dot reads the texel nearest its
       sun-map spot (at most 0.71 texel away), and a texel the caster changed
       has its centre inside the caster's silhouette. */
    d->o = o;
    d->t = t;
    d->radius = c->bound + s->sunview.inv_fy;
    int n = t > 0 ? (int)ceilf(t / fmaxf(c->bound, s->sunview.inv_fy)) : 0;
    d->n = n > 32 ? 32 : n;
}
/* Marks a rectangle of dots for this frame's redraw. */
static void mark(l3_scene *s, l3_rect r) {
    if (r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    for (int y = r.y0; y < r.y1; y++) {
        L3_BYTES(s, 0, (size_t)(r.x1-r.x0));
        memset(s->dirty_mask + y * s->w + r.x0, 1, (size_t)(r.x1 - r.x0));
    }
    s->dirty = unite(s->dirty, r);
}
/* Where a changed caster's shadow can have changed: where it was (last
   frame's sweep) and where it is. A shadow only changes a dot whose visible
   point lies inside a swept capsule, and every dot whose visible point
   changes this frame is marked by whatever changed it, so last frame's depth
   decides: the capsule's run under an opaque playfield, or across the screen
   beside a thin shadow, costs nothing. The capsule's axis runs along the
   sun, so the test is a distance on the sun map's plane, taken once a dot for
   both sweeps. Candidates are the rows' spans of spheres along the sweeps,
   spaced no wider than the caster and grown by half a step. */
static void mark_shadow(l3_scene *s, const l3_dyn *a, const l3_dyn *b) {
    const l3_dyn *g[2] = {a, b};
    const l3_vec R = s->sunview.right, U = s->sunview.up, S = s->sunview.forward;
    float du[2], dv[2], ds[2], r2[2], lo[2], hi[2];
    int ng = 0, nb = 0;
    l3_rect area = empty(), *ball = s->sweep_ball;
    for (int i = 0; i < 2; i++) {
        const l3_dyn *d = g[i];
        if (!d || !d->n)
            continue;
        const float step = d->t / d->n;
        for (int k = 0; k <= d->n; k++) {
            ball[nb] = screen(&s->view, l3_add(d->o, l3_scale(S, step * k)), d->radius + 0.5f * step);
            area = unite(area, ball[nb++]);
        }
        du[ng] = l3_dot(d->o, R);
        dv[ng] = l3_dot(d->o, U);
        ds[ng] = l3_dot(d->o, S);
        r2[ng] = d->radius * d->radius;
        lo[ng] = ds[ng] - d->radius;
        hi[ng] = ds[ng] + d->t + d->radius;
        ng++;
    }
    if (!ng)
        return;
    const l3_view *v = &s->view;
    const l3_vec dx = l3_scale(v->right, v->inv_fx);
    l3_rect hit = empty();
    for (int y = area.y0; y < area.y1; y++) {
        int sx0 = INT32_MAX, sx1 = 0;
        for (int k = 0; k < nb; k++)
            if (y >= ball[k].y0 && y < ball[k].y1) {
                if (ball[k].x0 < sx0) sx0 = ball[k].x0;
                if (ball[k].x1 > sx1) sx1 = ball[k].x1;
            }
        if (sx0 >= sx1)
            continue;
        /* The casters' own boxes are marked already: jump over them (a big
           caster's sweep is mostly its own box). */
        int skip0[2], skip1[2], nskip = 0;
        for (int i = 0; i < 2; i++)
            if (g[i] && y >= g[i]->rect.y0 && y < g[i]->rect.y1 && g[i]->rect.x0 < g[i]->rect.x1) {
                skip0[nskip] = g[i]->rect.x0;
                skip1[nskip++] = g[i]->rect.x1;
            }
        l3_vec o, ray;
        l3_zray(v, (float)sx0, (float)y, &o, &ray);
        int x0 = INT32_MAX, x1 = -1;
        for (int x = sx0; x < sx1; x++, v->ortho ? (o = l3_add(o, dx)) : (ray = l3_add(ray, dx))) {
            for (int i = 0; i < nskip; i++)
                if (x < sx1 && x >= skip0[i] && x < skip1[i]) {
                    const int jump = (skip1[i] < sx1 ? skip1[i] : sx1) - x;
                    if (v->ortho) o = l3_add(o, l3_scale(dx, (float)jump));
                    else ray = l3_add(ray, l3_scale(dx, (float)jump));
                    x += jump;
                    i = -1;                 /* the other box may start here */
                }
            if (x >= sx1)
                break;
            const int at = y * s->w + x;
            if (s->dirty_mask[at])
                continue;
            const l3_pixel *px = &s->pixels[at];
            L3_BYTES(s, 0, 9);
            if (!px->object)
                continue;
            const l3_vec p = l3_add(o, l3_scale(ray, px->depth));
            const float pu = l3_dot(p, R), pv = l3_dot(p, U), ps = l3_dot(p, S);
            bool in = false;
            for (int i = 0; i < ng && !in; i++) {
                const float eu = pu - du[i], ev = pv - dv[i];
                in = eu * eu + ev * ev <= r2[i] && ps >= lo[i] && ps <= hi[i];
            }
            if (!in)
                continue;
            s->dirty_mask[at] = 1;
            if (x < x0) x0 = x;
            x1 = x + 1;
        }
        if (x1 > x0) hit = unite(hit, (l3_rect){x0, y, x1, y + 1});
    }
    s->dirty = unite(s->dirty, hit);
}
void l3_end(l3_scene *s) {
    if (!s || !s->recording)
        return;
    s->recording = false;
    uint64_t start = l3_clock();
    memset(s->counts, 0, sizeof(s->counts));
    s->frame_bytes = 0;
    /* One pass: the static fingerprint, and a signature per dynamic command
       so a dynamic part that did not change since last frame is not drawn
       again (a lit lamp, a parked flipper), only the ones that did. */
    l3_dyn *cur = s->dyn[s->dyn_flip], *prev = s->dyn[!s->dyn_flip];
    const int nprev = s->have_frame ? s->ndyn : 0;
    int ndyn = 0;
    bool ordered = false, dynamic_seen = false, dynamic_caster = false;
    bool dynamic_changed = false, caster_changed = false;
    uint64_t static_hash = L3_HASH_BASIS;
    /* The sun map depends on the sun and the casters only, not the camera. */
    uint64_t caster_hash = hash(L3_HASH_BASIS, &s->sun, sizeof s->sun);
    if (!s->shadows) s->shadow_valid = false;   /* casters are not tracked while off */
    for (int i = 0; i < s->ncmd; i++) {
        l3_cmd *c = &s->cmd[i];
        if (c->kind == S_MESH)
            s->stats.triangles += (uint32_t)c->mesh.ntri;
        else if (c->kind < S_LINE)
            s->stats.shapes++;
        if (c->flags & L3_NO_DEPTH)
            ordered = true;
        uint64_t h = command_hash(c);
        if (c->flags & L3_STATIC) {
            static_hash = hash(static_hash, &h, sizeof h);
            if (casts(s, c)) caster_hash = hash(caster_hash, &h, sizeof h);
            if (dynamic_seen) ordered = true;
            continue;
        }
        dynamic_seen = true;
        l3_dyn *d = &cur[ndyn];
        d->sig = h;
        d->cmd = i;
        d->caster = casts(s, c);
        dynamic_caster |= (c->flags & L3_CAST) != 0;
        d->changed = ndyn >= nprev || prev[ndyn].sig != h;
        if (d->changed) {
            dynamic_changed = true;
            caster_changed |= d->caster || (ndyn < nprev && prev[ndyn].caster);
        }
        d->sun = d->changed ? empty() : prev[ndyn].sun;
        ndyn++;
    }
    for (int k = ndyn; k < nprev; k++) {
        dynamic_changed = true;
        caster_changed |= prev[k].caster;
    }
    s->static_hash = static_hash;
    const bool same_static = s->have_frame && static_hash == s->previous_hash &&
                             s->revision == s->previous_revision;
    s->ordered = ordered;
    s->cacheable = !ordered && !(s->shadows && dynamic_caster);
    /* Partial redraw draws its dots with the whole command list in recording
       order, the same as a full frame, so an ordered scene keeps it too. Only
       the lit static cache needs the static parts drawn first. */
    s->partial = !s->cacheable && same_static;
    s->reuse = s->cacheable ? s->valid && same_static : s->partial;
    s->shadow_changed = false;
    if (s->shadows) {
        const bool full = !s->shadow_valid || caster_hash != s->shadow_hash;
        s->shadow_hash = caster_hash;
        if (full || caster_changed) {
            uint64_t t = l3_clock();
            /* A sun camera that moved (a dynamic caster at the edge of the scene
               refits it) re-renders the whole map but keeps the partial frame,
               as before: only dots near what changed are drawn again. */
            shadow(s, full, prev, nprev, cur, ndyn);
            s->stats.us_shadow = (uint32_t)(l3_clock() - t);
            s->shadow_valid = true;
            s->shadow_changed = true;
        }
    }
    if (same_static && !dynamic_changed && !s->shadow_changed && (s->reuse || !s->cacheable)) {
        /* No geometry preparation, copies or resolve on an unchanged frame. */
        s->stats.dots_reused = (uint32_t)s->w * s->h;
        s->stats.us_total = (uint32_t)(l3_clock() - start);
        l3_profile_frame(s);
        return;
    }
    s->view = s->camera;
    s->ortho_half = l3_unit(l3_sub(l3_scale(s->sun,-1),s->view.forward));
    prepare_bounds(s);
    /* Clear what last frame marked, then mark this frame's changes. */
    for (int y = s->mask_rect.y0; y < s->mask_rect.y1; y++)
        memset(s->dirty_mask + y * s->w + s->mask_rect.x0, 0, (size_t)(s->mask_rect.x1 - s->mask_rect.x0));
    s->dirty = empty();
    float floor_lo = L3_INF;
    for (int i = 0; i < s->ncmd; i++)
        if (s->cmd[i].kind == S_FLOOR) floor_lo = fminf(floor_lo, s->cmd[i].a.y);
    /* An unchanged command under the same camera and receivers sweeps where
       it swept last frame. */
    const bool same_reach = s->reuse && floor_lo == s->floor_lo &&
                            !memcmp(&s->bounds_min, &s->reach_min, sizeof(l3_vec)) &&
                            !memcmp(&s->bounds_max, &s->reach_max, sizeof(l3_vec));
    s->floor_lo = floor_lo;
    s->reach_min = s->bounds_min;
    s->reach_max = s->bounds_max;
    for (int k = 0; k < ndyn; k++) {
        if (same_reach && !cur[k].changed && k < nprev) {
            cur[k].rect = prev[k].rect;
            cur[k].o = prev[k].o;
            cur[k].t = prev[k].t;
            cur[k].radius = prev[k].radius;
            cur[k].n = prev[k].n;
        } else
            sweep(s, &s->cmd[cur[k].cmd], &cur[k], floor_lo);
    }
    if (s->reuse) {
        /* Own boxes first, so the capsule tests skip dots already marked. */
        for (int k = 0; k < ndyn; k++)
            if (cur[k].changed) {
                mark(s, cur[k].rect);
                if (k < nprev) mark(s, prev[k].rect);
            }
        for (int k = ndyn; k < nprev; k++)
            mark(s, prev[k].rect);
        for (int k = 0; k < ndyn; k++)
            if (cur[k].changed) mark_shadow(s, &cur[k], k < nprev ? &prev[k] : NULL);
        for (int k = ndyn; k < nprev; k++)
            mark_shadow(s, &prev[k], NULL);
    } else {
        L3_BYTES(s, 0, (size_t)s->w * s->h);
        memset(s->dirty_mask, 1, (size_t)s->w * s->h);
        s->dirty = (l3_rect){0, 0, s->w, s->h};
    }
    prepare_raster(s, s->dirty);
    s->mask_rect = s->dirty;
    s->land_active = false;
    for (int i = 0; i < s->ncmd; i++)
        if (s->cmd[i].kind == S_TERRAIN && s->cmd[i].map_w > 2 && s->cmd[i].map_d > 2 && !s->ordered && !s->camera.ortho && fabsf(s->camera.forward.y) < .9999f && s->camera.up.y > 0 && fabsf(s->camera.right.y) < 1e-6f)
            s->land_active = true;
    for (int i = 0; i < 4; i++) s->lamp_inv[i] = 1/(s->lamp[i].range*s->lamp[i].range);
    if (s->land_active) l3_run(s, 7);
    /* Outlines need neighbouring visibility. Resolving after the raster join
     * avoids repeatedly retiring/reloading two-row scratch in arbitrary order. */
    if (s->outline == LAP_DOT_CLEAR) l3_run(s, 6);
    else { l3_run(s, 4); l3_resolve(s); }
    s->valid = s->cacheable;
    s->have_frame = true;
    s->ndyn = ndyn;
    s->dyn_flip ^= 1;
    s->stats.us_raster = s->counts[0].us_raster + s->counts[1].us_raster;
    s->stats.us_resolve = s->counts[0].us_resolve + s->counts[1].us_resolve;
    s->stats.us_shade = s->counts[0].us_shade + s->counts[1].us_shade;
    s->stats.dots_shaded = s->reuse ? s->band_dots[0] + s->band_dots[1] : (uint32_t)s->w * s->h;
    s->stats.dots_reused = (uint32_t)s->w * s->h - s->stats.dots_shaded;
    s->previous_hash = s->static_hash;
    s->previous_revision = s->revision;
    s->stats.us_total = (uint32_t)(l3_clock() - start);
    l3_profile_frame(s);
}
