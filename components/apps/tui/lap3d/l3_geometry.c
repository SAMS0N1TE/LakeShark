/* Exact primitive intersections; bounded SDF tracing for torus/rounded box. */
#include "l3_private.h"
static inline bool roots(float a, float b, float c, float *lo, float *hi) {
    if (fabsf(a) < 1.0e-20f) {
        if (fabsf(b) < 1.0e-20f)
            return false;
        *lo = *hi = -c / (2 * b);
        return true;
    }
    float d = b * b - a * c;
    if (d < 0)
        return false;
    float r = sqrtf(d), q = -b - copysignf(r, b);
    float x = q / a, y = fabsf(q) > 1.0e-20f ? c / q : x;
    *lo = fminf(x, y);
    *hi = fmaxf(x, y);
    return true;
}
static inline float root(float a, float b, float c, float min_t, float max_t) {
    float lo, hi;
    if (!roots(a, b, c, &lo, &hi))
        return L3_INF;
    if (lo >= min_t && lo <= max_t)
        return lo;
    if (hi >= min_t && hi <= max_t)
        return hi;
    return L3_INF;
}
static inline bool slab(const l3_vec *o, const l3_vec *d, const l3_vec *h,
                                           float *lo, float *hi) {
    *lo = -L3_INF;
    *hi = L3_INF;
    for (int i = 0; i < 3; i++) {
        float a = i == 0   ? o->x
                  : i == 1 ? o->y
                           : o->z,
              b = i == 0   ? d->x
                  : i == 1 ? d->y
                           : d->z,
              r = i == 0   ? h->x
                  : i == 1 ? h->y
                           : h->z;
        if (fabsf(b) < 1.0e-20f) {
            if (fabsf(a) > r)
                return false;
            continue;
        }
        float inv = 1.0f / b, x = (-r - a) * inv, y = (r - a) * inv;
        *lo = fmaxf(*lo, fminf(x, y));
        *hi = fminf(*hi, fmaxf(x, y));
        if (*lo > *hi)
            return false;
    }
    return true;
}
static inline float sdf(const l3_cmd *c, const l3_vec *p) {
    if (c->kind == S_TORUS) {
        float x = sqrtf(p->x * p->x + p->z * p->z) - c->r;
        return sqrtf(x * x + p->y * p->y) - c->r2;
    }
    l3_vec q = l3v(fabsf(p->x) - c->half.x + c->r, fabsf(p->y) - c->half.y + c->r,
                   fabsf(p->z) - c->half.z + c->r);
    return l3_length(l3v(fmaxf(q.x, 0), fmaxf(q.y, 0), fmaxf(q.z, 0))) +
           fminf(fmaxf(q.x, fmaxf(q.y, q.z)), 0) - c->r;
}
static inline void sdf_normal(const l3_cmd *c, l3_work *w) {
    /* Analytic SDF gradients replace six extra distance evaluations. */
    if (c->kind == S_TORUS) {
        float r = sqrtf(w->lp.x * w->lp.x + w->lp.z * w->lp.z);
        float k = r > 1.0e-12f ? 1 - c->r / r : 0;
        w->ln = l3_unit(l3v(w->lp.x * k, w->lp.y, w->lp.z * k));
    } else {
        float x = fabsf(w->lp.x) - c->half.x + c->r,
              y = fabsf(w->lp.y) - c->half.y + c->r,
              z = fabsf(w->lp.z) - c->half.z + c->r;
        if (x > 0 || y > 0 || z > 0)
            w->ln = l3_unit(l3v(copysignf(fmaxf(x, 0), w->lp.x),
                               copysignf(fmaxf(y, 0), w->lp.y),
                               copysignf(fmaxf(z, 0), w->lp.z)));
        else
            w->ln = x >= y && x >= z ? l3v(copysignf(1, w->lp.x), 0, 0)
                    : y >= z ? l3v(0, copysignf(1, w->lp.y), 0)
                             : l3v(0, 0, copysignf(1, w->lp.z));
    }
}
static inline bool traced(const l3_cmd *c, l3_work *w, float min_t,
                                             float max_t) {
    float lo, hi;
    w->n = c->kind == S_TORUS ? l3v(c->r + c->r2, c->r2, c->r + c->r2) : c->half;
    if (!slab(&w->local, &w->ld, &w->n, &lo, &hi))
        return false;
    lo = fmaxf(lo, min_t);
    hi = fminf(hi, max_t);
    if (lo > hi)
        return false;
    float speed = l3_length(w->ld);
    if (speed < 1.0e-12f)
        return false;
    float inv_speed = 1.0f / speed;
    for (int i = 0; i < 160 && lo <= hi; i++) {
#ifdef L3_PROFILE
        w->counts->steps++;
#endif
        w->lp.x = w->local.x + w->ld.x * lo;
        w->lp.y = w->local.y + w->ld.y * lo;
        w->lp.z = w->local.z + w->ld.z * lo;
        float d = sdf(c, &w->lp);
        if (fabsf(d) < 0.00015f) {
            w->hit = lo;
            if (!w->depth_only)
                sdf_normal(c, w);
            return true;
        }
        lo += fmaxf(fabsf(d), 0.00005f) * inv_speed;
    }
    return false;
}
static inline void tube_caps(const l3_cmd *c, l3_work *w, float min_t,
                                                float max_t) {
    const l3_vec *o = &w->local, *d = &w->ld;
    float h = c->half.y;
    if (c->kind == S_CAPSULE) {
        for (int i = -1; i <= 1; i += 2) {
            float py = o->y - i * h;
            float t = root(l3_dot(*d, *d), o->x * d->x + py * d->y + o->z * d->z,
                           o->x * o->x + py * py + o->z * o->z - c->r * c->r, min_t,
                           fminf(w->hit, max_t));
            if (t < w->hit) {
                w->hit = t;
                w->u = (float)i;
            }
        }
    } else if (fabsf(d->y) > 1.0e-20f) {
        for (int i = -1; i <= 1; i += 2) {
            float t = (i * h - o->y) / d->y, x = o->x + t * d->x, z = o->z + t * d->z,
                  r = i < 0 ? c->r : c->r2;
            if (t >= min_t && t <= max_t && t < w->hit && x * x + z * z <= r * r) {
                w->hit = t;
                w->u = (float)i;
            }
        }
    }
}
static inline void tube_normal(const l3_cmd *c, l3_work *w, int surface) {
    const l3_vec *o = &w->local, *d = &w->ld;
    float best = w->hit, h = c->half.y, k = c->kind == S_CONE ? (c->r2 - c->r) / (2 * h) : 0;
    w->lp.x = o->x + d->x * best;
    w->lp.y = o->y + d->y * best;
    w->lp.z = o->z + d->z * best;
    w->ln = surface ? (c->kind == S_CAPSULE ? l3_sub(w->lp, l3v(0, surface * h, 0))
                                            : l3v(0, (float)surface, 0))
                    : l3v(w->lp.x, -k * (c->r + k * (w->lp.y + h)), w->lp.z);
}
static inline bool tube(const l3_cmd *c, l3_work *w, float min_t, float max_t) {
    const l3_vec *o = &w->local, *d = &w->ld;
    float h = c->half.y, k = c->kind == S_CONE ? (c->r2 - c->r) / (2 * h) : 0;
    float radius = c->r + k * (o->y + h), dy = k * d->y;
    float lo, hi, best = L3_INF;
    int surface = 0;
    if (roots(d->x * d->x + d->z * d->z - dy * dy, o->x * d->x + o->z * d->z - radius * dy,
              o->x * o->x + o->z * o->z - radius * radius, &lo, &hi)) {
        for (int i = 0; i < 2; i++) {
            float t = i ? hi : lo, y = o->y + d->y * t;
            if (t >= min_t && t <= max_t && fabsf(y) <= h && t < best)
                best = t;
        }
    }
    w->hit = best;
    w->u = 0;
    tube_caps(c, w, min_t, max_t);
    best = w->hit;
    surface = (int)w->u;
    if (best == L3_INF)
        return false;
    w->hit = best;
    if (!w->depth_only)
        tube_normal(c, w, surface);
    return true;
}
static inline float edge_side(const l3_vec *a, const l3_vec *b,
                                                 const l3_work *w) {
    float x = b->x - a->x, y = b->y - a->y, z = b->z - a->z, px = w->p.x - a->x, py = w->p.y - a->y,
          pz = w->p.z - a->z;
    return w->n.x * (y * pz - z * py) + w->n.y * (z * px - x * pz) + w->n.z * (x * py - y * px);
}
static inline bool plane_hit(const l3_cmd *c, l3_work *w, float min_t,
                                                float max_t) {
    w->n = c->kind == S_FLOOR ? l3v(0, 1, 0) : c->kind == S_DISC ? c->b : c->half;
    float d = l3_dot(w->n, w->dir);
    if (fabsf(d) < 1.0e-8f || (c->kind == S_QUAD && !(c->flags & L3_TWO_SIDED) && d >= 0))
        return false;
    w->hit =
        ((c->a.x - w->o.x) * w->n.x + (c->a.y - w->o.y) * w->n.y + (c->a.z - w->o.z) * w->n.z) / d;
    if (w->hit < min_t || w->hit > max_t)
        return false;
    w->p = l3_add(w->o, l3_scale(w->dir, w->hit));
    w->lp = w->p;
    if (c->kind == S_DISC) {
        float x = w->p.x - c->a.x, y = w->p.y - c->a.y, z = w->p.z - c->a.z;
        if (x * x + y * y + z * z > c->r * c->r)
            return false;
    }
    if (c->kind == S_QUAD &&
        (edge_side(&c->a, &c->b, w) < -1.0e-5f || edge_side(&c->b, &c->c, w) < -1.0e-5f ||
         edge_side(&c->c, &c->d, w) < -1.0e-5f || edge_side(&c->d, &c->a, w) < -1.0e-5f))
        return false;
    if (d > 0)
        w->n = l3_scale(w->n, -1);
    return true;
}
static inline bool ellipsoid_hit(const l3_cmd *c, l3_work *w, float min_t,
                                                    float max_t) {
    if (c->half.x <= 0 || c->half.y <= 0 || c->half.z <= 0)
        return false;
    w->p = l3v(w->local.x / c->half.x, w->local.y / c->half.y, w->local.z / c->half.z);
    w->n = l3v(w->ld.x / c->half.x, w->ld.y / c->half.y, w->ld.z / c->half.z);
    w->hit = root(l3_dot(w->n, w->n), l3_dot(w->p, w->n), l3_dot(w->p, w->p) - 1, min_t, max_t);
    if (w->hit == L3_INF)
        return false;
    if (w->depth_only)
        return true;
    w->lp = l3_add(w->local, l3_scale(w->ld, w->hit));
    w->ln = l3v(w->lp.x / (c->half.x * c->half.x), w->lp.y / (c->half.y * c->half.y),
                w->lp.z / (c->half.z * c->half.z));
    return true;
}
static inline bool box_hit(const l3_cmd *c, l3_work *w, float min_t,
                                              float max_t) {
    float lo, hi;
    if (!slab(&w->local, &w->ld, &c->half, &lo, &hi))
        return false;
    w->hit = lo >= min_t ? lo : hi;
    if (w->hit < min_t || w->hit > max_t)
        return false;
    if (w->depth_only)
        return true;
    w->lp = l3_add(w->local, l3_scale(w->ld, w->hit));
    float x = fabsf(w->lp.x / c->half.x), y = fabsf(w->lp.y / c->half.y),
          z = fabsf(w->lp.z / c->half.z);
    w->ln = x >= y && x >= z ? l3v(copysignf(1, w->lp.x), 0, 0)
            : y >= z         ? l3v(0, copysignf(1, w->lp.y), 0)
                             : l3v(0, 0, copysignf(1, w->lp.z));
    return true;
}
static inline bool sphere_hit(const l3_cmd *c, l3_work *w, float min_t,
                                                 float max_t) {
    l3_vec q = l3_sub(w->o, c->a);
    w->hit = root(l3_dot(w->dir, w->dir), l3_dot(q, w->dir), l3_dot(q, q) - c->r * c->r, min_t, max_t);
    if (w->hit == L3_INF)
        return false;
    if (w->depth_only)
        return true;
    w->p = l3_add(w->o, l3_scale(w->dir, w->hit));
    w->n = l3_unit(l3_sub(w->p, c->a));
    w->lp = l3_sub(w->p, c->a);
    return true;
}
bool l3_intersect(const l3_cmd *c, l3_work *w, float min_t, float max_t) {
    w->hit = L3_INF;
    w->sun = -1;
    w->u = w->v = 0;
    if (c->kind == S_SPHERE) {
        if (!sphere_hit(c, w, min_t, max_t))
            return false;
    } else if (c->kind == S_FLOOR || c->kind == S_DISC || c->kind == S_QUAD) {
        if (!plane_hit(c, w, min_t, max_t))
            return false;
    } else {
        w->local = l3_transform(&c->inv, w->o);
        w->ld = l3_dir(&c->inv, w->dir);
        if (c->kind == S_ELLIPSOID) {
            if (!ellipsoid_hit(c, w, min_t, max_t))
                return false;
        } else if (c->kind == S_BOX) {
            if (!box_hit(c, w, min_t, max_t))
                return false;
        } else if (c->kind == S_TORUS || c->kind == S_RBOX) {
            if (!traced(c, w, min_t, max_t))
                return false;
        } else if (c->kind == S_CYLINDER || c->kind == S_CAPSULE || c->kind == S_CONE) {
            if (!tube(c, w, min_t, max_t))
                return false;
        } else
            return false;
        if (w->depth_only)
            return true;
        w->p = l3_add(w->o, l3_scale(w->dir, w->hit));
        w->n = l3_normal(&c->inv, w->ln);
    }
    if (w->depth_only)
        return true;
    /* Planar mapping uses the major normal axis, matching all six box faces. */
    l3_vec n = c->kind >= S_ELLIPSOID && c->kind <= S_TORUS ? w->ln : w->n;
    if (fabsf(n.y) >= fabsf(n.x) && fabsf(n.y) >= fabsf(n.z)) {
        w->u = w->lp.x;
        w->v = w->lp.z;
    } else if (fabsf(n.x) >= fabsf(n.z)) {
        w->u = w->lp.z;
        w->v = w->lp.y;
    } else {
        w->u = w->lp.x;
        w->v = w->lp.y;
    }
    return true;
}
