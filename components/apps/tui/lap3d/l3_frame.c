/* Frame preparation, conservative dirty regions and sun-map orchestration. */
#include "l3_private.h"
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
void l3_prepare(l3_scene *s) {
    s->ntri = 0;
    s->nparticle = s->ncolumn = 0;
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
        if (c->kind == S_TERRAIN && s->shadow_pass) l3_terrain_shadow(s, i);
        else if (c->kind == S_MESH) l3_mesh_prepare(s, i);
        else if (c->kind == S_BOX || c->kind == S_RBOX || c->kind == S_QUAD || c->kind == S_DISC)
            l3_flat_prepare(s, i);
        else if (c->kind == S_PARTICLES) l3_particles_prepare(s, i);
        else if (c->kind == S_WALLS) l3_walls_prepare(s, i);
    }
    l3_bins_prepare(s);
    if (s->shadows && !s->shadow_pass) {
        /* Refresh receiver reach even when the caster-keyed sun map is reused.
         * This keeps moving shadows conservative on distant noncasters. */
        s->bounds_min = l3v(L3_INF,L3_INF,L3_INF);
        s->bounds_max = l3v(-L3_INF,-L3_INF,-L3_INF);
        for (int i = 0; i < s->ncmd; i++)
            if (s->cmd[i].kind < S_LINE && s->cmd[i].kind != S_FLOOR) include_bounds(s,&s->cmd[i]);
    }
}
static __attribute__((noinline)) uint64_t hash(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static __attribute__((noinline)) uint64_t scene_hash(l3_scene *s, bool all) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < s->ncmd; i++) {
        l3_cmd *c = &s->cmd[i];
        if (!all && !(c->flags & L3_STATIC))
            continue;
        h = hash(h, c, sizeof(*c));
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
    }
    return h;
}
static uint64_t caster_hash(l3_scene *s) {
    uint64_t h = hash(1469598103934665603ull,&s->sun,sizeof(s->sun));
    for (int i = 0; i < s->ncmd; i++) {
        const l3_cmd *c = &s->cmd[i];
        if (!(c->flags & L3_CAST) || c->kind >= S_LINE || c->kind == S_FLOOR) continue;
        h = hash(h,c,sizeof(*c));
        if (c->kind == S_TERRAIN || c->kind == S_WALLS)
            h = hash(h,c->map,(size_t)c->map_w*c->map_d);
        if (c->kind == S_MESH) {
            h = hash(h,c->mesh.v,(size_t)c->mesh.nv*sizeof(l3_vec));
            h = hash(h,c->mesh.idx,(size_t)c->mesh.ntri*3*sizeof(uint16_t));
        }
        if (c->kind == S_PARTICLES) h = hash(h,c->points,(size_t)c->np*sizeof(l3_vec));
    }
    return h;
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
static __attribute__((noinline)) void shadow(l3_scene *s) {
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
    shadow_camera(s, any);
    s->shadow_scale = 65534.0f / s->sunview.far_z;
    s->shadow_inv = 1.0f / s->shadow_scale;
    s->shadow_pass = true;
    s->view = s->sunview;
    s->dirty = (l3_rect){0, 0, L3_SHADOW, L3_SHADOW};
    l3_prepare(s);
    l3_run(s, 0);
    s->shadow_pass = false;
}
/* Where a moving caster can change the picture: its own box, and its shadow,
   which lies inside its bounding sphere swept along the sun until it leaves
   the scene's bounds or reaches a floor. */
static __attribute__((noinline)) l3_rect reach(l3_scene *s, const l3_cmd *c) {
    l3_rect r = c->rect;
    if (!s->shadows || !(c->flags & L3_CAST) || c->kind >= S_LINE || c->kind == S_FLOOR)
        return r;
    const l3_vec d = s->sun, o = c->center;
    float t = 0;
    const float lo[3] = {s->bounds_min.x - c->bound, s->bounds_min.y - c->bound, s->bounds_min.z - c->bound};
    const float hi[3] = {s->bounds_max.x + c->bound, s->bounds_max.y + c->bound, s->bounds_max.z + c->bound};
    const float od[3][2] = {{o.x, d.x}, {o.y, d.y}, {o.z, d.z}};
    float exit = L3_INF;
    for (int k = 0; k < 3; k++)
        if (fabsf(od[k][1]) > 1e-6f) {
            float te = ((od[k][1] > 0 ? hi[k] : lo[k]) - od[k][0]) / od[k][1];
            if (te < exit) exit = te;
        }
    if (exit < L3_INF) t = exit;
    for (int i = 0; i < s->ncmd; i++)
        if (s->cmd[i].kind == S_FLOOR && d.y < -1e-6f) {
            float tf = (s->cmd[i].a.y - o.y) / d.y;
            if (tf > t) t = tf;
        }
    /* Steps along the sweep: a point off the screen clamps to nothing, so
       one far end alone would lose the stretch that crosses the screen. */
    t = fminf(t, 1000);
    for (int k = 1; t > 0 && k <= 8; k++)
        r = unite(r, screen(&s->view, l3_add(o, l3_scale(d, t * k * 0.125f)), c->bound * (1 + 0.125f * k)));
    return r;
}
void l3_end(l3_scene *s) {
    if (!s || !s->recording)
        return;
    s->recording = false;
    uint64_t start = l3_clock();
    memset(s->counts, 0, sizeof(s->counts));
    s->frame_bytes = 0;
    s->static_hash = scene_hash(s, false);
    uint64_t complete_hash;
    s->reuse =
        s->valid && s->static_hash == s->previous_hash && s->revision == s->previous_revision;
    /* Changes in caster silhouettes can affect distant receivers; invalidate
     * conservatively. */
    bool ordered = false, dynamic_seen = false, dynamic_caster = false;
    for (int i = 0; i < s->ncmd; i++) {
        l3_cmd *c = &s->cmd[i];
        if (c->kind == S_MESH)
            s->stats.triangles += (uint32_t)c->mesh.ntri;
        else if (c->kind < S_LINE)
            s->stats.shapes++;
        if (!(c->flags & L3_STATIC)) {
            dynamic_seen = true;
            if (c->flags & L3_CAST) dynamic_caster = true;
        }
        else if (dynamic_seen)
            ordered = true;
        if (c->flags & L3_NO_DEPTH)
            ordered = true;
    }
    complete_hash = s->shadows && dynamic_caster ? scene_hash(s,true) : 0;
    s->ordered = ordered;
    s->cacheable = !ordered && !(s->shadows && dynamic_caster);
    /* A moving caster can shadow anything, so the lit static layer cannot be
       kept - but last frame's finished picture can, outside where the casters
       and their shadows were and are. Those dots are drawn again in full. */
    s->partial = !s->cacheable && !ordered && s->have_frame &&
                 s->static_hash == s->previous_hash && s->revision == s->previous_revision;
    if (!s->cacheable) s->reuse = s->partial;
    s->shadow_changed = false;
    if (s->shadows) {
        uint64_t h = caster_hash(s);
        if (!s->shadow_valid || h != s->shadow_hash) {
            uint64_t t = l3_clock();
            shadow(s);
            s->stats.us_shadow = (uint32_t)(l3_clock() - t);
            s->shadow_hash = h;
            s->shadow_valid = true;
            s->shadow_changed = true;
            s->reuse = s->partial;
        }
    }
    /* The complete-frame hash also covers noncasters. An identical frame can retain its final output
     * even when dynamic caster flags prohibit storing a static layer. */
    bool same_complete = complete_hash == s->static_hash_all;
    s->static_hash_all = complete_hash;
    bool unchanged_shadow_frame = same_complete && s->shadows && dynamic_caster && !s->shadow_changed &&
                                  s->revision == s->previous_revision;
    if (unchanged_shadow_frame ||
        (s->reuse && !dynamic_seen && s->previous_dynamic.x0 >= s->previous_dynamic.x1)) {
        /* No geometry preparation, copies or resolve on an unchanged frame. */
        s->stats.dots_reused = (uint32_t)s->w * s->h;
        s->stats.us_total = (uint32_t)(l3_clock() - start);
        l3_profile_frame(s);
        return;
    }
    s->view = s->camera;
    s->ortho_half = l3_unit(l3_sub(l3_scale(s->sun,-1),s->view.forward));
    l3_prepare(s);
    s->current_dynamic = empty();
    for (int i = 0; i < s->ncmd; i++)
        if (!(s->cmd[i].flags & L3_STATIC))
            s->current_dynamic = unite(s->current_dynamic, reach(s, &s->cmd[i]));
    size_t pixels = (size_t)s->w * s->h;
    L3_BYTES(s, 0, pixels * 3);
    memcpy(s->dirty_mask, s->previous_mask, pixels);
    memset(s->previous_mask, 0, pixels);
    for (int i = 0; i < s->ncmd; i++)
        if (!(s->cmd[i].flags & L3_STATIC)) {
            const l3_rect rr = reach(s, &s->cmd[i]), *r = &rr;
            if (r->x0 >= r->x1 || r->y0 >= r->y1)
                continue;
            for (int y = r->y0; y < r->y1; y++) {
                L3_BYTES(s, 0, (size_t)(r->x1-r->x0)*2);
                memset(s->dirty_mask + y * s->w + r->x0, 1, (size_t)(r->x1 - r->x0));
                memset(s->previous_mask + y * s->w + r->x0, 1, (size_t)(r->x1 - r->x0));
            }
        }
    if (!s->reuse) L3_BYTES(s, 0, pixels);
    if (!s->reuse)
        memset(s->dirty_mask, 1, pixels);
    s->dirty =
        s->reuse ? unite(s->current_dynamic, s->previous_dynamic) : (l3_rect){0, 0, s->w, s->h};
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
    s->stats.us_raster = s->counts[0].us_raster + s->counts[1].us_raster;
    s->stats.us_resolve = s->counts[0].us_resolve + s->counts[1].us_resolve;
    s->stats.us_shade = s->counts[0].us_shade + s->counts[1].us_shade;
    s->stats.dots_shaded = s->reuse ? s->band_dots[0] + s->band_dots[1] : (uint32_t)s->w * s->h;
    s->stats.dots_reused = (uint32_t)s->w * s->h - s->stats.dots_shaded;
    s->previous_dynamic = s->current_dynamic;
    s->previous_hash = s->static_hash;
    s->previous_revision = s->revision;
    s->stats.us_total = (uint32_t)(l3_clock() - start);
    l3_profile_frame(s);
}
