/* Small generated surfaces feed the shared clipped triangle rasterizer. */
#include "l3_private.h"
static void face(l3_scene *s, int *n, int a, int b, int c) {
    s->shape_idx[(*n)++] = (uint16_t)a;
    s->shape_idx[(*n)++] = (uint16_t)b;
    s->shape_idx[(*n)++] = (uint16_t)c;
}
static void patch(l3_scene *s, int *n, int a, int b, int c, int d) {
    face(s, n, a, b, c); face(s, n, a, c, d);
}
static int boxes(l3_scene *s, const l3_cmd *c) {
    float r = c->kind == S_RBOX ? c->r : 0;
    int n = 0;
    /* Three points on the round at each corner: face, edge and corner
     * patches share these normals, without smoothing across a box face. */
    for (int k = 0; k < 8; k++) for (int axis = 0; axis < 3; axis++) {
        float x = k & 1 ? 1 : -1, y = k & 2 ? 1 : -1, z = k & 4 ? 1 : -1;
        s->shape_v[k*3+axis] = l3v(x*(c->half.x-(axis != 0 ? r : 0)),
                                   y*(c->half.y-(axis != 1 ? r : 0)),
                                   z*(c->half.z-(axis != 2 ? r : 0)));
        s->shape_n[k*3+axis] = l3v(axis == 0 ? x : 0, axis == 1 ? y : 0, axis == 2 ? z : 0);
    }
    for (int axis = 0; axis < 3; axis++) {
        int a = (axis+1)%3, b = (axis+2)%3;
        for (int side = 0; side < 2; side++) {
            int k = side << axis;
            patch(s, &n, k*3+axis, (k|(1<<a))*3+axis,
                  (k|(1<<a)|(1<<b))*3+axis, (k|(1<<b))*3+axis);
        }
        if (r > 0) for (int k = 0; k < 8; k++) if (!(k & (1<<axis))) {
            int j = k | (1<<axis);
            patch(s, &n, k*3+a, j*3+a, j*3+b, k*3+b);
        }
    }
    if (r > 0) for (int k = 0; k < 8; k++) face(s, &n, k*3, k*3+1, k*3+2);
    return n;
}
static int disc(l3_scene *s, const l3_cmd *c) {
    l3_vec u = l3_unit(l3_cross(c->b, fabsf(c->b.y) < .9f ? l3v(0,1,0) : l3v(1,0,0)));
    l3_vec v = l3_cross(c->b, u);
    float z = l3_dot(l3_sub(c->a, s->view.eye), s->view.forward);
    float radius = c->r * s->view.fy / (s->view.ortho ? 1 : fmaxf(z, s->view.near_z));
    /* Sagitta <= 1/4 dot, capped by the reserved generator scratch. */
    int segments = (int)ceilf(L3_PI * sqrtf(fmaxf(1, radius)*2));
    if (segments < 8) segments = 8;
    if (segments > 128) segments = 128;
    s->shape_v[0] = c->a; s->shape_n[0] = c->b;
    for (int k = 0; k < segments; k++) {
        float a = k * (2*L3_PI/segments);
        s->shape_v[k+1] = l3_add(c->a, l3_add(l3_scale(u, c->r*cosf(a)), l3_scale(v, c->r*sinf(a))));
        s->shape_n[k+1] = c->b;
        s->shape_idx[k*3] = 0; s->shape_idx[k*3+1] = (uint16_t)(k+1);
        s->shape_idx[k*3+2] = (uint16_t)((k+1)%segments+1);
    }
    return segments*3;
}
static __attribute__((noinline)) void emit(l3_scene *s, int command, int i) {
    const l3_cmd *c = &s->cmd[command];
    bool box = c->kind == S_BOX || c->kind == S_RBOX;
    for (int j = 0; j < 3; j++) {
        int id = s->shape_idx[i+j];
        l3_corner *v = &s->clip_a[j];
        v->p = box ? l3_transform(&c->t, s->shape_v[id]) : s->shape_v[id];
        v->n = box ? l3_normal(&c->inv, s->shape_n[id]) : s->shape_n[id];
        v->z = l3_dot(l3_sub(v->p, s->view.eye), s->view.forward);
    }
    l3_vec n = l3_cross(l3_sub(s->clip_a[1].p, s->clip_a[0].p),
                        l3_sub(s->clip_a[2].p, s->clip_a[0].p));
    /* Generator topology is computed, so orient each patch with its normals. */
    if (l3_dot(n, s->clip_a[0].n) < 0) {
        l3_corner tmp = s->clip_a[1]; s->clip_a[1] = s->clip_a[2]; s->clip_a[2] = tmp;
        n = l3_scale(n, -1);
    }
    float facing = l3_dot(n, s->view.ortho ? s->view.forward : l3_sub(s->clip_a[0].p, s->view.eye));
    if (facing >= 0) {
        if (box) {
            /* Closed shapes also expose their exit surface when the eye or
             * near plane cuts the object, matching analytic slab tracing. */
            float front = l3_dot(l3_sub(c->center,s->view.eye),s->view.forward)-c->bound;
            if (front >= s->view.near_z) return;
        } else {
            if (c->kind == S_QUAD && !(c->flags & L3_TWO_SIDED)) return;
            for (int j = 0; j < 3; j++) s->clip_a[j].n = l3_scale(s->clip_a[j].n, -1);
        }
    }
    l3_submit_triangle(s, command, c->mat);
}
void l3_flat_prepare(l3_scene *s, int command) {
    l3_cmd *c = &s->cmd[command];
    int n;
    c->first = s->ntri;
    if (c->kind == S_BOX || c->kind == S_RBOX) n = boxes(s, c);
    else if (c->kind == S_DISC) n = disc(s, c);
    else {
        s->shape_v[0] = c->a; s->shape_v[1] = c->b;
        s->shape_v[2] = c->c; s->shape_v[3] = c->d;
        for (int j = 0; j < 4; j++) s->shape_n[j] = c->half;
        n = 0; patch(s, &n, 0, 1, 2, 3);
    }
    for (int i = 0; i < n; i += 3) emit(s, command, i);
    c->count = s->ntri - c->first;
}
