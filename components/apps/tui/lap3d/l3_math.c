/* lap3d maths: float-only affine and quaternion operations. */
#include "l3_private.h"
l3_vec l3_norm(l3_vec a) {
    return l3_unit(a);
}
l3_xform l3_identity(void) {
    l3_xform t = {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}};
    return t;
}
l3_vec l3_apply(const l3_xform *t, l3_vec p) {
    return l3_transform(t, p);
}
l3_quat l3_quat_mul(l3_quat a, l3_quat b) {
    l3_quat q = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                 a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return q;
}
static __attribute__((noinline)) l3_quat unit(l3_quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1.0e-20f)
        return (l3_quat){0, 0, 0, 1};
    float k = 1.0f / n;
    return (l3_quat){q.x * k, q.y * k, q.z * k, q.w * k};
}
l3_quat l3_quat_axis(l3_vec a, float deg) {
    a = l3_norm(a);
    if (l3_dot(a, a) == 0)
        return (l3_quat){0, 0, 0, 1};
    float t = deg * (L3_PI / 360.0f), s = sinf(t);
    return (l3_quat){a.x * s, a.y * s, a.z * s, cosf(t)};
}
l3_quat l3_quat_euler(float yaw, float pitch, float roll) {
    return l3_quat_mul(
        l3_quat_axis(l3v(0, 1, 0), yaw),
        l3_quat_mul(l3_quat_axis(l3v(1, 0, 0), pitch), l3_quat_axis(l3v(0, 0, 1), roll)));
}
l3_quat l3_quat_spin(l3_quat q, l3_vec omega, float dt) {
    float speed = l3_length(omega);
    return unit(l3_quat_mul(l3_quat_axis(omega, speed * dt * (180.0f / L3_PI)), q));
}
l3_quat l3_quat_slerp(l3_quat a, l3_quat b, float t) {
    a = unit(a);
    b = unit(b);
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = (l3_quat){-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float x = 1 - t, y = t;
    if (d < 0.9995f) {
        float angle = acosf(l3_clamp(d, -1, 1)), r = 1.0f / sinf(angle);
        x = sinf((1 - t) * angle) * r;
        y = sinf(t * angle) * r;
    }
    return unit(
        (l3_quat){a.x * x + b.x * y, a.y * x + b.y * y, a.z * x + b.z * y, a.w * x + b.w * y});
}
l3_vec l3_quat_rotate(l3_quat q, l3_vec v) {
    q = unit(q);
    l3_vec a = l3v(q.x, q.y, q.z), t = l3_scale(l3_cross(a, v), 2);
    return l3_add(v, l3_add(l3_scale(t, q.w), l3_cross(a, t)));
}
l3_xform l3_pose(l3_vec p, l3_quat q, float s) {
    q = unit(q);
    float x = q.x, y = q.y, z = q.z, w = q.w;
    l3_xform t = {{s * (1 - 2 * (y * y + z * z)), s * 2 * (x * y - z * w), s * 2 * (x * z + y * w),
                   s * 2 * (x * y + z * w), s * (1 - 2 * (x * x + z * z)), s * 2 * (y * z - x * w),
                   s * 2 * (x * z - y * w), s * 2 * (y * z + x * w), s * (1 - 2 * (x * x + y * y)),
                   p.x, p.y, p.z}};
    return t;
}
l3_xform l3_mul(const l3_xform *a, const l3_xform *b) {
    l3_xform t;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            t.m[3 * r + c] = a->m[3 * r] * b->m[c] + a->m[3 * r + 1] * b->m[3 + c] +
                             a->m[3 * r + 2] * b->m[6 + c];
    l3_vec p = l3_apply(a, l3v(b->m[9], b->m[10], b->m[11]));
    t.m[9] = p.x;
    t.m[10] = p.y;
    t.m[11] = p.z;
    return t;
}
bool l3_inverse(l3_xform *o, const l3_xform *a) {
    const float *m = a->m;
    float d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
              m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabsf(d) < 1.0e-20f)
        return false;
    d = 1.0f / d;
    o->m[0] = (m[4] * m[8] - m[5] * m[7]) * d;
    o->m[1] = (m[2] * m[7] - m[1] * m[8]) * d;
    o->m[2] = (m[1] * m[5] - m[2] * m[4]) * d;
    o->m[3] = (m[5] * m[6] - m[3] * m[8]) * d;
    o->m[4] = (m[0] * m[8] - m[2] * m[6]) * d;
    o->m[5] = (m[2] * m[3] - m[0] * m[5]) * d;
    o->m[6] = (m[3] * m[7] - m[4] * m[6]) * d;
    o->m[7] = (m[1] * m[6] - m[0] * m[7]) * d;
    o->m[8] = (m[0] * m[4] - m[1] * m[3]) * d;
    l3_vec p = l3_scale(l3_dir(o, l3v(m[9], m[10], m[11])), -1);
    o->m[9] = p.x;
    o->m[10] = p.y;
    o->m[11] = p.z;
    return true;
}
void l3_view_setup(l3_view *v, l3_vec eye, l3_vec target, l3_vec up, float fov, float height, int w,
                   int h, float aspect, float near_z, float far_z) {
    v->eye = eye;
    v->forward = l3_norm(l3_sub(target, eye));
    if (l3_dot(v->forward, v->forward) == 0)
        v->forward = l3v(0, 0, -1);
    v->right = l3_norm(l3_cross(v->forward, up));
    if (l3_dot(v->right, v->right) < 0.5f)
        v->right =
            l3_norm(l3_cross(v->forward, fabsf(v->forward.y) < 0.9f ? l3v(0, 1, 0) : l3v(1, 0, 0)));
    v->up = l3_cross(v->right, v->forward);
    v->ortho = fov == 0;
    v->w = w;
    v->h = h;
    v->fy = 0.5f * h / (v->ortho ? height * 0.5f : tanf(fov * (L3_PI / 360.0f)));
    v->fx = v->fy / aspect;
    v->inv_fx = 1.0f / v->fx;
    v->inv_fy = 1.0f / v->fy;
    v->near_z = near_z;
    v->far_z = far_z;
}
void l3_view_ray(const l3_view *v, float x, float y, l3_vec *o, l3_vec *d) {
    l3_vec p = l3_add(l3_scale(v->right, (x + 0.5f - v->w * 0.5f) / v->fx),
                      l3_scale(v->up, (v->h * 0.5f - y - 0.5f) / v->fy));
    *o = v->ortho ? l3_add(v->eye, p) : v->eye;
    *d = v->ortho ? v->forward : l3_norm(l3_add(v->forward, p));
}
bool l3_view_project(const l3_view *v, l3_vec p, float *x, float *y) {
    p = l3_sub(p, v->eye);
    float z = l3_dot(p, v->forward);
    if (z < v->near_z || z > v->far_z)
        return false;
    float k = v->ortho ? 1 : 1 / z;
    if (x)
        *x = v->w * 0.5f - 0.5f + l3_dot(p, v->right) * v->fx * k;
    if (y)
        *y = v->h * 0.5f - 0.5f - l3_dot(p, v->up) * v->fy * k;
    return true;
}
bool l3_project(const l3_scene *s, l3_vec p, float *x, float *y) {
    return s && l3_view_project(&s->camera, p, x, y);
}
void l3_ray(const l3_scene *s, float x, float y, l3_vec *o, l3_vec *d) {
    if (s && o && d)
        l3_view_ray(&s->camera, x, y, o, d);
}
