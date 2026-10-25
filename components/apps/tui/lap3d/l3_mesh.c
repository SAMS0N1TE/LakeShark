/* Mesh transform, smooth normals, near/far clipping and integer edge setup. */
#include "l3_private.h"
static __attribute__((noinline)) l3_corner mix(l3_corner a, l3_corner b, float t) {
    l3_corner c;
    c.p = l3_add(a.p, l3_scale(l3_sub(b.p, a.p), t));
    c.n = l3_add(a.n, l3_scale(l3_sub(b.n, a.n), t));
    c.x = c.y = 0;
    c.z = a.z + (b.z - a.z) * t;
    return c;
}
static __attribute__((noinline)) int clip(l3_corner *out, const l3_corner *in, int n, float plane,
                                          bool near_plane) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        const l3_corner *a = &in[i], *b = &in[(i + 1) % n];
        bool ai = near_plane ? a->z >= plane : a->z <= plane,
             bi = near_plane ? b->z >= plane : b->z <= plane;
        if (ai)
            out[count++] = *a;
        if (ai != bi)
            out[count++] = mix(*a, *b, (plane - a->z) / (b->z - a->z));
    }
    return count;
}
/* Clip homogeneous screen coordinates before 28.4 edge setup. The largest
 * supported view (2048 x 3072 dots) stays below INT32_MAX even at the margins. */
static inline float side_distance(const l3_scene *s, const l3_corner *c, int side) {
    float axis = l3_dot(l3_sub(c->p, s->view.eye), side < 2 ? s->view.right : s->view.up);
    float z = s->view.ortho ? 1 : c->z;
    float extent = side < 2 ? (s->view.w + 1) * 0.5f * s->view.inv_fx
                            : (s->view.h + 1) * 0.5f * s->view.inv_fy;
    return extent * z + (side & 1 ? -axis : axis);
}
static int clip_side(l3_scene *s, l3_corner *out, const l3_corner *in, int n, int side) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        const l3_corner *a = &in[i], *b = &in[(i + 1) % n];
        float da = side_distance(s, a, side), db = side_distance(s, b, side);
        if (da >= 0)
            out[count++] = *a;
        if ((da >= 0) != (db >= 0))
            out[count++] = mix(*a, *b, da / (da - db));
    }
    return count;
}
static __attribute__((noinline)) bool project_corner(const l3_scene *s, l3_tri *t, int i) {
    l3_corner *v = &t->v[i];
    l3_vec p = l3_sub(v->p, s->view.eye);
    float k = s->view.ortho ? 1 : 1 / v->z;
    v->x = s->view.w * 0.5f - 0.5f + l3_dot(p, s->view.right) * s->view.fx * k;
    v->y = s->view.h * 0.5f - 0.5f - l3_dot(p, s->view.up) * s->view.fy * k;
    if (!isfinite(v->x) || !isfinite(v->y) || fabsf(v->x) > 1000000 || fabsf(v->y) > 1000000)
        return false;
    t->x[i] = (int32_t)lroundf(v->x * 16);
    t->y[i] = (int32_t)lroundf(v->y * 16);
    t->rect.x0 = (int)fminf((float)t->rect.x0, floorf(v->x));
    t->rect.x1 = (int)fmaxf((float)t->rect.x1, ceilf(v->x) + 1);
    t->rect.y0 = (int)fminf((float)t->rect.y0, floorf(v->y));
    t->rect.y1 = (int)fmaxf((float)t->rect.y1, ceilf(v->y) + 1);
    return true;
}
static __attribute__((noinline)) void triangle(l3_scene *s, int command, int mat, int a, int b,
                                               int c) {
    if (s->ntri == s->tricap) {
        s->stats.dropped++;
        return;
    }
    l3_tri *t = &s->tri[s->ntri];
    t->v[0] = s->clip_a[a];
    t->v[1] = s->clip_a[b];
    t->v[2] = s->clip_a[c];
    t->cmd = command;
    t->mat = mat;
    t->flat = !memcmp(&t->v[0].n, &t->v[1].n, sizeof(l3_vec)) &&
              !memcmp(&t->v[0].n, &t->v[2].n, sizeof(l3_vec));
    t->sun = fmaxf(0, -l3_dot(t->v[0].n, s->sun));
    t->edges = 7;
    t->rect = (l3_rect){s->view.w, s->view.h, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (!project_corner(s, t, i))
            return;
    }
    t->area = (int32_t)(t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) -
              (int32_t)(t->y[1] - t->y[0]) * (t->x[2] - t->x[0]);
    if (!t->area)
        return;
    float inv = 1.0f / (float)t->area;
    for (int i = 0; i < 3; i++)
        t->weight[i] = s->view.ortho ? inv : inv / t->v[i].z;
    if (t->rect.x0 < 0)
        t->rect.x0 = 0;
    if (t->rect.y0 < 0)
        t->rect.y0 = 0;
    if (t->rect.x1 > s->view.w)
        t->rect.x1 = s->view.w;
    if (t->rect.y1 > s->view.h)
        t->rect.y1 = s->view.h;
    s->ntri++;
}
void l3_submit_triangle(l3_scene *s, int command, int mat) {
    int count = clip(s->clip_b, s->clip_a, 3, s->view.near_z, true);
    count = clip(s->clip_a, s->clip_b, count, s->view.far_z, false);
    for (int side = 0; side < 4; side++)
        count = side & 1 ? clip_side(s, s->clip_a, s->clip_b, count, side)
                         : clip_side(s, s->clip_b, s->clip_a, count, side);
    for (int j = 1; j + 1 < count; j++) triangle(s, command, mat, 0, j, j + 1);
}
void l3_mesh_prepare(l3_scene *s, int command) {
    l3_cmd *c = &s->cmd[command];
    const l3_mesh *m = &c->mesh;
    c->first = s->ntri;
    c->count = 0;
    if (m->nv > s->vertcap) {
        s->stats.dropped++;
        return;
    }
    for (int i = 0; i < m->nv; i++) {
        s->vertices[i].p = l3_apply(&c->t, m->v[i]);
        if (!s->shadow_pass)
            s->vertices[i].n = l3v(0, 0, 0);
    }
    if (!s->shadow_pass && (c->flags & L3_SMOOTH))
        for (int i = 0; i < m->ntri; i++) {
            int a = m->idx[i * 3], b = m->idx[i * 3 + 1], d = m->idx[i * 3 + 2];
            if (a >= m->nv || b >= m->nv || d >= m->nv)
                continue;
            l3_vec n = l3_cross(l3_sub(s->vertices[b].p, s->vertices[a].p),
                                l3_sub(s->vertices[d].p, s->vertices[a].p));
            s->vertices[a].n = l3_add(s->vertices[a].n, n);
            s->vertices[b].n = l3_add(s->vertices[b].n, n);
            s->vertices[d].n = l3_add(s->vertices[d].n, n);
        }
    if (!s->shadow_pass && (c->flags & L3_SMOOTH))
        for (int i = 0; i < m->nv; i++)
            s->vertices[i].n = l3_norm(s->vertices[i].n);
    for (int i = 0; i < m->ntri; i++) {
        int a = m->idx[i * 3], b = m->idx[i * 3 + 1], d = m->idx[i * 3 + 2];
        if (a >= m->nv || b >= m->nv || d >= m->nv) {
            s->stats.dropped++;
            continue;
        }
        l3_vec n = l3_cross(l3_sub(s->vertices[b].p, s->vertices[a].p),
                             l3_sub(s->vertices[d].p, s->vertices[a].p));
        if (!s->shadow_pass)
            n = l3_unit(n);
        float facing =
            l3_dot(n, s->view.ortho ? s->view.forward : l3_sub(s->vertices[a].p, s->view.eye));
        if (!(c->flags & L3_TWO_SIDED) && facing >= 0)
            continue;
        if (facing > 0)
            n = l3_scale(n, -1);
        for (int j = 0; j < 3; j++) {
            int id = m->idx[i * 3 + j];
            l3_corner *v = &s->clip_a[j];
            v->p = s->vertices[id].p;
            v->n = s->shadow_pass ? l3v(0, 0, 0) : c->flags & L3_SMOOTH ? l3_scale(s->vertices[id].n, facing > 0 ? -1 : 1) : n;
            v->z = l3_dot(l3_sub(v->p, s->view.eye), s->view.forward);
        }
        int mat = m->mats && m->mats[i] < s->nmat ? m->mats[i] : c->mat;
        l3_submit_triangle(s, command, mat);
    }
    c->count = s->ntri - c->first;
}
