/* Fused row-band raster/shade: disjoint tiles in each worker's own scratch. */
#include "l3_private.h"
static inline bool shadowed(const l3_scene *s, l3_vec p) {
    /* Sun view is orthographic: no perspective projection or divide. */
    l3_vec q = l3_sub(p, s->sunview.eye);
    float x = (L3_SHADOW - 1) * 0.5f + l3_dot(q, s->sunview.right) * s->sunview.fx;
    float y = (L3_SHADOW - 1) * 0.5f - l3_dot(q, s->sunview.up) * s->sunview.fy;
    if (x < -0.5f || y < -0.5f || x >= L3_SHADOW - 0.5f || y >= L3_SHADOW - 0.5f)
        return false;
    int ix = (int)(x + 0.5f), iy = (int)(y + 0.5f);
    float z = l3_dot(q, s->sunview.forward), depth = s->shadow[iy * L3_SHADOW + ix];
    float bias = 0.012f + 2.0f * s->sunview.inv_fy;
    return depth > 0 && (z - bias) * s->shadow_scale + 1 > depth;
}
static inline __attribute__((always_inline)) void write_pixel(l3_scene *s, const l3_cmd *c, int command,
                                int mat, int x, int y, l3_work *w, uint8_t ink) {
    int at = y * s->view.w + x;
    if (s->shadow_pass) L3_BYTES(s, (int)(w - s->work), 8);
    else L3_LOCAL_BYTES(s, (int)(w - s->work), 8);
    if (s->shadow_pass) {
        uint16_t depth = (uint16_t)l3_clamp(w->hit * s->shadow_scale + 1, 1, 65535);
        if (!s->shadow[at] || depth < s->shadow[at] || (c->flags & L3_NO_DEPTH))
            s->shadow[at] = depth;
        return;
    }
    l3_pixel *p = l3_work_pixel(s, w, x, y);
    if (!(c->flags & L3_NO_DEPTH) && p->object && w->hit >= p->depth) {
        /* Front-loaded walls may tie an earlier floor/ceiling command. Keep
         * the original first-recorded winner, including its material/tag. */
        uint16_t previous = w->surface[(y-w->y0)*s->w+x].command;
        if (!s->walls_first || w->hit > p->depth || !previous || command+1 >= previous) return;
    }
    L3_LOCAL_BYTES(s, (int)(w - s->work), 8 + (ink == LAP_DOT_CLEAR ? 8 : 2));
    p->depth = w->hit;
    p->object = 1;
    p->tag = c->tag;
    p->ink = ink;
    p->mat = (uint8_t)mat;
    p->light = 255;
    l3_surface *g = &w->surface[(y - w->y0) * s->w + x];
    g->command = ink == LAP_DOT_CLEAR ? (uint16_t)(command + 1) : 0;
    if (!g->command)
        return;
    if (s->material[mat].texture >= 0 || (s->material[mat].flags & L3_CHECKER)) {
        L3_LOCAL_BYTES(s, (int)(w - s->work), 8);
        g->u = w->u; g->v = w->v;
    }
    w->lighting[(y - w->y0) * s->w + x].p = w->p;
    w->lighting[(y - w->y0) * s->w + x].sun = w->sun;
    g->nx = (int16_t)(l3_clamp(w->n.x, -1, 1) * 32767);
    g->ny = (int16_t)(l3_clamp(w->n.y, -1, 1) * 32767);
    g->nz = (int16_t)(l3_clamp(w->n.z, -1, 1) * 32767);
}
static void shade_pixel(l3_scene *s, int x, int y, l3_work *w, int worker) {
    L3_LOCAL_BYTES(s, worker, 10);
    l3_pixel *p = l3_work_pixel(s, w, x, y);
    l3_surface *g = &w->surface[(y - w->y0) * s->w + x];
    if (!p->object || !g->command)
        return;
    L3_LOCAL_BYTES(s, worker, 9);
    if (s->shadows) L3_BYTES(s, worker, 4);
    L3_COUNT(s, worker, lights);
    const l3_cmd *c = &s->cmd[g->command - 1];
    g->command = 0;
    w->n = l3v(g->nx * (1.0f / 32767), g->ny * (1.0f / 32767), g->nz * (1.0f / 32767));
    if (s->material[p->mat].texture >= 0 || (s->material[p->mat].flags & L3_CHECKER)) {
        L3_LOCAL_BYTES(s, worker, 8);
        w->u = g->u; w->v = g->v;
    }
    const l3_lighting *lit = &w->lighting[(y - w->y0) * s->w + x];
    w->p = lit->p;
    l3_mat *m = &s->material[p->mat];
    if (m->texture >= 0) {
        const l3_tex *t = &s->texture[m->texture];
        float u = w->u, v = w->v;
        if (c->kind == S_BOX || c->kind == S_RBOX) {
            /* World normal back to local normal via transpose(object transform). */
            float nx = c->t.m[0]*w->n.x + c->t.m[3]*w->n.y + c->t.m[6]*w->n.z,
                  ny = c->t.m[1]*w->n.x + c->t.m[4]*w->n.y + c->t.m[7]*w->n.z,
                  nz = c->t.m[2]*w->n.x + c->t.m[5]*w->n.y + c->t.m[8]*w->n.z;
            float hu = fabsf(ny) >= fabsf(nx) && fabsf(ny) >= fabsf(nz) ? c->half.x
                       : fabsf(nx) >= fabsf(nz) ? c->half.z : c->half.x;
            float hv = fabsf(ny) >= fabsf(nx) && fabsf(ny) >= fabsf(nz) ? c->half.z : c->half.y;
            u = hu > 0 ? (u / hu + 1) * 0.5f : 0;
            v = hv > 0 ? (v / hv + 1) * 0.5f : 0;
            u = l3_clamp(u, 0, 0.999999f);
            v = l3_clamp(v, 0, 0.999999f);
            if (t->w == 6 * t->h) {
                /* Six tiles side by side: one a face, +x -x +y -y +z -z (dice). */
                const float ax = fabsf(nx), ay = fabsf(ny), az = fabsf(nz);
                const int face = ay >= ax && ay >= az ? (ny < 0 ? 3 : 2)
                                 : ax >= az ? (nx < 0 ? 1 : 0) : (nz < 0 ? 5 : 4);
                u = (face + u) * (1.0f / 6);
            }
        } else {
            u -= (float)l3_ifloor(u); v -= (float)l3_ifloor(v);
        }
        float tu = u * t->w, tv = v * t->h;
        float du = tu - (float)l3_ifloor(tu) - .5f, dv = tv - (float)l3_ifloor(tv) - .5f;
        uint8_t id = (m->flags & L3_ROUND) && du*du + dv*dv > t->dot*t->dot ? 255
                     : t->data[(int)tv * t->w + (int)tu];
        if ((m->flags & L3_ROUND) && t->dot > .5f) {
            /* Dots bigger than a texel: the nearest set texel within reach,
               never across the edge of a six-tile box texture's face. */
            const int ix = (int)tu, iy = (int)tv, tile = t->w == 6 * t->h ? t->h : t->w;
            const int x0 = ix / tile * tile, x1 = x0 + tile;
            float best = t->dot * t->dot;
            id = 255;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++) {
                    const int nx = ix + ox, ny = iy + oy;
                    if (nx < x0 || nx >= x1 || ny < 0 || ny >= t->h) continue;
                    const uint8_t n = t->data[ny * t->w + nx];
                    const float ex = tu - (nx + .5f), ey = tv - (ny + .5f), d2 = ex*ex + ey*ey;
                    if (n != 255 && d2 < best) { best = d2; id = n; }
                }
        }
        if (id < s->nmat) {
            p->mat = id;
            m = &s->material[id];
        }
    }
    if (m->flags & L3_UNLIT) {
        p->light = 255;
        return;
    }
    float diffuse = lit->sun >= 0 ? lit->sun : fmaxf(0, -l3_dot(w->n, s->sun));
    float light = s->ambient;
    if (s->shadows)
        L3_COUNT(s, worker, shadows);
    if (!s->shadows || !shadowed(s, w->p)) {
        light += s->sun_strength * diffuse;
        if (m->spec > 0 && diffuse > 0) {
            /* View direction is screen-affine. Position stays in lighting scratch;
             * specular uses the original camera ray to preserve dither thresholds. */
            w->dir = s->view.ortho ? s->view.forward : l3_add(s->view.forward,
                l3_add(l3_scale(s->view.right,(x+.5f-s->w*.5f)*s->view.inv_fx),
                       l3_scale(s->view.up,(s->h*.5f-y-.5f)*s->view.inv_fy)));
            l3_vec half = s->view.ortho ? s->ortho_half
                : l3_unit(l3_sub(l3_scale(s->sun, -1), l3_unit(w->dir)));
            float h = fmaxf(0, l3_dot(w->n, half));
            h *= h; h *= h; h *= h; h *= h; h *= h;
            light += m->spec * h;
        }
    }
    for (int i = 0; i < 4; i++)
        if (s->lamp_mask[worker] & (1u<<i)) {
            l3_vec d = l3_sub(s->lamp[i].pos, w->p);
            float d2 = l3_dot(d,d), fall = 1-d2*s->lamp_inv[i];
            float nl = l3_dot(w->n,d);
            if (fall > 0 && nl > 0 && d2 > 1e-10f)
                light += s->lamp[i].strength*fall*fall*nl*l3_rsqrt(d2);
        }
    if (m->flags & L3_CHECKER) {
        if ((l3_ifloor(w->u) ^ l3_ifloor(w->v)) & 1)
            light -= 1.0f / fmaxf(1, (float)m->n - 1);
    }
    if (m->flags & L3_GRAIN) {
        uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        light += ((h >> 24) * (1.0f / 255.0f) - 0.5f) * 0.12f;
    }
    p->light = (uint8_t)(l3_clamp(light, 0, 1) * 255 + 0.5f);
}
static void shade_rows(l3_scene *s, l3_rect r, int worker) {
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++)
            if (s->work[worker].full_dirty ||
                (L3_BYTES(s, worker, 1), s->dirty_mask[y * s->w + x]))
                shade_pixel(s, x, y, &s->work[worker], worker);
}
static inline int32_t edge(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                           int32_t x, int32_t y) {
    return (bx - ax) * (y - ay) - (by - ay) * (x - ax);
}
static void triangle_hit(l3_scene *s, const l3_tri *t, l3_work *w,
                         float a, float b, float d, float z, int x, int y) {
    w->hit = z;
    if (!s->shadow_pass) {
        w->p.x = t->v[0].p.x * a + t->v[1].p.x * b + t->v[2].p.x * d;
        w->p.y = t->v[0].p.y * a + t->v[1].p.y * b + t->v[2].p.y * d;
        w->p.z = t->v[0].p.z * a + t->v[1].p.z * b + t->v[2].p.z * d;
        w->sun = t->flat ? t->sun : -1;
        if (t->flat) w->n = t->v[0].n;
        else {
            w->n.x = t->v[0].n.x * a + t->v[1].n.x * b + t->v[2].n.x * d;
            w->n.y = t->v[0].n.y * a + t->v[1].n.y * b + t->v[2].n.y * d;
            w->n.z = t->v[0].n.z * a + t->v[1].n.z * b + t->v[2].n.z * d;
            w->n = l3_unit(w->n);
        }
        w->u = w->p.x; w->v = w->p.z;
        const l3_cmd *c = &s->cmd[t->cmd];
        if (c->kind == S_BOX || c->kind == S_RBOX) {
            w->lp = l3_transform(&c->inv, w->p);
            l3_vec n = l3v(c->t.m[0]*w->n.x+c->t.m[3]*w->n.y+c->t.m[6]*w->n.z,
                           c->t.m[1]*w->n.x+c->t.m[4]*w->n.y+c->t.m[7]*w->n.z,
                           c->t.m[2]*w->n.x+c->t.m[5]*w->n.y+c->t.m[8]*w->n.z);
            w->u = fabsf(n.y)>=fabsf(n.x) && fabsf(n.y)>=fabsf(n.z) ? w->lp.x
                     : fabsf(n.x)>=fabsf(n.z) ? w->lp.z : w->lp.x;
            w->v = fabsf(n.y)>=fabsf(n.x) && fabsf(n.y)>=fabsf(n.z) ? w->lp.z : w->lp.y;
        }
    }
    write_pixel(s, &s->cmd[t->cmd], t->cmd, t->mat, x, y, w, LAP_DOT_CLEAR);
}
static __attribute__((noinline)) void wire(l3_scene *s, const l3_tri *t, l3_rect r, l3_work *w) {
    const l3_cmd *c = &s->cmd[t->cmd];
    const l3_mat *mat = &s->material[t->mat];
    for (int edge = 0; edge < 3; edge++) if (t->edges & (1<<edge)) {
        const l3_corner *a = &t->v[edge], *b = &t->v[(edge+1)%3];
        float dx = b->x-a->x, dy = b->y-a->y;
        int steps = (int)ceilf(fmaxf(fabsf(dx),fabsf(dy)));
        float inv = steps ? 1.0f/steps : 0;
        for (int i = 0; i <= steps; i++) {
            float f = i*inv;
            int x = (int)floorf(a->x+f*dx+.5f), y = (int)floorf(a->y+f*dy+.5f);
            if (x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1) continue;
            if (!s->shadow_pass && !w->full_dirty && !s->dirty_mask[y*s->w+x]) continue;
            w->hit = s->view.ortho ? a->z+(b->z-a->z)*f : 1/((1-f)/a->z+f/b->z);
            write_pixel(s,c,t->cmd,t->mat,x,y,w,mat->ink[mat->n-1]);
        }
    }
}
static __attribute__((noinline)) void raster_tri(l3_scene *s, const l3_tri *t, l3_rect band, l3_work *w, int worker) {
    if (s->cmd[t->cmd].kind == S_MESH && (s->cmd[t->cmd].flags & L3_WIRE)) {
        wire(s,t,band,w); return;
    }
    int x0 = t->rect.x0 > band.x0 ? t->rect.x0 : band.x0,
        x1 = t->rect.x1 < band.x1 ? t->rect.x1 : band.x1,
        y0 = t->rect.y0 > band.y0 ? t->rect.y0 : band.y0,
        y1 = t->rect.y1 < band.y1 ? t->rect.y1 : band.y1;
    if (x0 >= x1 || y0 >= y1)
        return;
    const l3_cmd *command = &s->cmd[t->cmd];
    bool plane = !s->shadow_pass && !s->view.ortho && command->kind == S_QUAD && t->flat;
    float plane_distance = l3_dot(l3_sub(command->a,s->view.eye),t->v[0].n);
    float plane_step = l3_dot(s->view.right,t->v[0].n)*s->view.inv_fx;
    float za = t->weight[0], zb = t->weight[1], zd = t->weight[2];
    int32_t e0 = edge(t->x[1], t->y[1], t->x[2], t->y[2], x0 * 16, y0 * 16),
            e1 = edge(t->x[2], t->y[2], t->x[0], t->y[0], x0 * 16, y0 * 16),
            e2 = edge(t->x[0], t->y[0], t->x[1], t->y[1], x0 * 16, y0 * 16);
    int32_t dx0 = (t->y[1] - t->y[2]) * 16, dx1 = (t->y[2] - t->y[0]) * 16,
            dx2 = (t->y[0] - t->y[1]) * 16;
    for (int y = y0; y < y1; y++) {
        int32_t a0 = e0, a1 = e1, a2 = e2;
        float row_z = e0*za*t->v[0].z + e1*zb*t->v[1].z + e2*zd*t->v[2].z;
        float step_z = dx0*za*t->v[0].z + dx1*zb*t->v[1].z + dx2*zd*t->v[2].z;
        if (plane) l3_zray(&s->view,(float)x0,(float)y,&w->o,&w->dir);
        float plane_start = plane ? l3_dot(w->dir,t->v[0].n) : 0;
        float constant_z = plane && plane_step == 0 && fabsf(plane_start) > 1e-8f ?
            l3_dot(l3_sub(command->a,w->o),t->v[0].n)/plane_start : 0;
        if (plane && plane_step == 0) {
            w->lp = l3_add(w->o,l3_scale(w->dir,constant_z));
            w->ld = l3_scale(s->view.right,s->view.inv_fx*(s->view.ortho ? 1 : constant_z));
        }
        for (int x = x0; x < x1; x++, a0 += dx0, a1 += dx1, a2 += dx2) {
            L3_COUNT(s, worker, edges);
            if (!s->shadow_pass && !w->full_dirty && (L3_BYTES(s, worker, 1), !s->dirty_mask[y * s->view.w + x]))
                continue;
            if (t->area > 0 ? (a0 < 0 || a1 < 0 || a2 < 0) : (a0 > 0 || a1 > 0 || a2 > 0))
                continue;
            if (plane) {
                float denominator = plane_start+(x-x0)*plane_step;
                if (fabsf(denominator) < 1e-8f) continue;
                float z = plane_step == 0 ? constant_z : plane_distance/denominator;
                float old = l3_work_pixel(s,w,x,y)->depth;
                if (old > 0 && z > old && !(command->flags & L3_NO_DEPTH)) continue;
                if (plane_step == 0) w->p = l3_add(w->lp,l3_scale(w->ld,(float)(x-x0)));
                else {
                    l3_zray(&s->view,(float)x,(float)y,&w->o,&w->dir);
                    if (s->view.ortho) z = l3_dot(l3_sub(command->a,w->o),t->v[0].n)/denominator;
                    w->p = l3_add(w->o,l3_scale(w->dir,z));
                }
                if (z < s->view.near_z || z > s->view.far_z) continue;
                w->hit = z;
                w->n = t->v[0].n; w->sun = t->sun; w->u = w->p.x; w->v = w->p.z;
                write_pixel(s,command,t->cmd,t->mat,x,y,w,LAP_DOT_CLEAR);
                continue;
            }
            float a = (float)a0 * za, b = (float)a1 * zb, d = (float)a2 * zd;
            float z;
            if (!s->view.ortho) {
                L3_COUNT(s, worker, triangle_reciprocals);
                z = 1.0f / (a + b + d);
                a *= z; b *= z; d *= z;
            } else
                z = row_z + (x-x0)*step_z;
            int at = y * s->view.w + x;
            if (s->shadow_pass) L3_BYTES(s, worker, 4);
            else L3_LOCAL_BYTES(s, worker, 4);
            float old = s->shadow_pass ? (s->shadow[at] ? (s->shadow[at]-1)*s->shadow_inv : 0) : l3_work_pixel(s, w, x, y)->depth;
            if (!(s->cmd[t->cmd].flags & L3_NO_DEPTH) && old > 0 && z >= old)
                continue;
            triangle_hit(s, t, w, a, b, d, z, x, y);
        }
        e0 += (t->x[2] - t->x[1]) * 16;
        e1 += (t->x[0] - t->x[2]) * 16;
        e2 += (t->x[1] - t->x[0]) * 16;
    }
}
static inline bool glyph_at(const l3_cmd *c, int x, int y) {
    if (x < 0 || y < 0 || y >= 5 * c->scale)
        return false;
    int col = x / (4 * c->scale), k = x / c->scale % 4;
    if (col >= (int)strlen(c->text) || k >= 3)
        return false;
    return (l3_glyph(c->text[col]) >> (14 - y / c->scale * 3 - k)) & 1;
}
static __attribute__((noinline)) void overlay(l3_scene *s, const l3_cmd *c, int command, int x,
                                              int y, l3_work *w) {
    uint8_t ink = LAP_DOT_CLEAR;
    float z = c->c.z;
    if (c->kind == S_POINT) {
        float dx = x - c->c.x, dy = y - c->c.y;
        if (dx * dx + dy * dy <= c->r * c->r * 0.25f)
            ink = c->ink;
    } else if (c->kind == S_LINE) {
        float dx = c->d.x - c->c.x, dy = c->d.y - c->c.y, len = dx * dx + dy * dy,
              t = len > 0 ? l3_clamp(((x - c->c.x) * dx + (y - c->c.y) * dy) / len, 0, 1) : 0;
        float a = x - c->c.x - t * dx, b = y - c->c.y - t * dy;
        if (a * a + b * b <= 0.5f) {
            ink = c->ink;
            z = s->view.ortho ? c->c.z + (c->d.z - c->c.z) * t
                              : 1 / ((1 - t) / c->c.z + t / c->d.z);
        }
    } else if (c->kind == S_LABEL) {
        int px = (int)floorf(x - c->c.x), py = (int)floorf(y - c->c.y);
        if (glyph_at(c, px, py))
            ink = c->ink;
        else if (c->edge != LAP_DOT_CLEAR) {
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (glyph_at(c, px + dx, py + dy))
                        ink = c->edge;
        }
    } else if (c->kind == S_SPRITE && c->r2 > 0 && c->half.x > 0) {
        int px = (int)floorf((x - c->c.x) / c->half.x), py = (int)floorf((y - c->c.y) / c->r2);
        if (py >= 0 && py < c->nrows && px >= 0 && px < (int)strlen(c->rows[py])) {
            const char *key = strchr(c->keys, c->rows[py][px]);
            if (key)
                ink = c->inks[key - c->keys];
        }
    }
    if (ink == LAP_DOT_CLEAR)
        return;
    l3_zray(&s->view, (float)x, (float)y, &w->o, &w->dir);
    w->hit = z;
    w->p = l3_add(w->o, l3_scale(w->dir, w->hit));
    write_pixel(s, c, command, 0, x, y, w, ink);
}
static __attribute__((noinline)) void raster_particle(l3_scene *s, const l3_particle *q,
                                                          l3_rect band, l3_work *w) {
    const l3_cmd *c = &s->cmd[q->cmd];
    int x0 = q->rect.x0 > band.x0 ? q->rect.x0 : band.x0;
    int x1 = q->rect.x1 < band.x1 ? q->rect.x1 : band.x1;
    int y0 = q->rect.y0 > band.y0 ? q->rect.y0 : band.y0;
    int y1 = q->rect.y1 < band.y1 ? q->rect.y1 : band.y1;
    float ix = 1/q->rx, iy = 1/q->ry;
    w->sun = -1;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        if (!s->shadow_pass && !w->full_dirty && !s->dirty_mask[y*s->w+x]) continue;
        float a = (x-q->x)*ix, b = (q->y-y)*iy, radius = a*a+b*b;
        if (radius > 1) continue;
        float z = sqrtf(1-radius);
        w->hit = q->z-c->r*z;
        if (w->hit < s->view.near_z || w->hit > s->view.far_z) continue;
        w->n = l3_sub(l3_add(l3_scale(s->view.right,a),l3_scale(s->view.up,b)),l3_scale(s->view.forward,z));
        w->p = l3_add(q->p,l3_scale(w->n,c->r));
        w->u = a; w->v = b;
        write_pixel(s,c,q->cmd,c->mat,x,y,w,LAP_DOT_CLEAR);
    }
}
/* Each column is marched front to back through the field. The band-local
 * y buffer marks a ray after its nearest terrain/wall hit, including a hit
 * occluded by other geometry: a farther field sample cannot win that dot.
 * DDA is exact on each LOD patch, including pitched and orthographic rays. */
static __attribute__((noinline)) void raster_world(l3_scene *s, const l3_cmd *c,
                                                    int command, l3_rect r, l3_work *w) {
    int x0 = c->rect.x0 > r.x0 ? c->rect.x0 : r.x0;
    int x1 = c->rect.x1 < r.x1 ? c->rect.x1 : r.x1;
    int y0 = c->rect.y0 > r.y0 ? c->rect.y0 : r.y0;
    int y1 = c->rect.y1 < r.y1 ? c->rect.y1 : r.y1;
    if (!s->shadow_pass && c->kind == S_TERRAIN && c->map_w > 2 && c->map_d > 2 && s->land_active) {
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            if (!w->full_dirty && !s->dirty_mask[y*s->w+x]) continue;
            const l3_land *q = &s->land[((c->flags & L3_STATIC) ? 0 : s->w*s->h)+x*s->h+y];
            if (q->command != command+1) continue;
            w->hit = q->depth;
            l3_zray(&s->view,(float)x,(float)y,&w->o,&w->dir);
            w->p = l3_add(w->o,l3_scale(w->dir,w->hit));
            w->u = w->p.x; w->v = w->p.z; w->sun = -1;
            float nx = q->nx*(1.0f/127), nz = q->nz*(1.0f/127);
            w->n = l3v(nx,sqrtf(fmaxf(0,1-nx*nx-nz*nz)),nz);
            write_pixel(s,c,command,q->mat,x,y,w,LAP_DOT_CLEAR);
        }
        return;
    }
    l3_vec step = l3_scale(s->view.up,-s->view.inv_fy);
    w->lod = (s->view.ortho ? -1 : 1)*s->view.inv_fy/c->r;
    float pitch_cos = sqrtf(1-s->view.forward.y*s->view.forward.y);
    float inv_height = c->r2 > 0 ? 1/c->r2 : 0;
    for (int x = x0; x < x1; x++) {
        unsigned y_buffer = 0;
        l3_zray(&s->view,(float)x,(float)y0,&w->o,&w->dir);
        l3_vec o = w->o, d = w->dir;
        for (int y = y0; y < y1; y++) {
            unsigned bit = 1u<<(y-y0);
            if (y_buffer & bit) continue;
            if (!s->shadow_pass && !w->full_dirty && !s->dirty_mask[y*s->w+x]) continue;
            w->o = s->view.ortho ? l3_add(o,l3_scale(step,(float)(y-y0))) : o;
            w->dir = s->view.ortho ? d : l3_add(d,l3_scale(step,(float)(y-y0)));
            float old = s->shadow_pass ? (s->shadow[y*s->view.w+x] ?
                          (s->shadow[y*s->view.w+x]-1)*s->shadow_inv : 0)
                        : l3_work_pixel(s,w,x,y)->depth;
            float far_t = old > 0 && !(c->flags & L3_NO_DEPTH) ? fminf(old,s->view.far_z) : s->view.far_z;
            bool cached = false;
            if (c->kind == S_WALLS && c->first >= 0) {
                const l3_wall_column *q = &s->column[c->first+x];
                bool level = fabsf(s->view.forward.y) < 1e-7f;
                bool beam = level;
                if (!level && !s->view.ortho) {
                    float cp = pitch_cos;
                    float forward = (w->dir.x*s->view.forward.x+w->dir.z*s->view.forward.z)/cp;
                    float slope = l3_dot(w->dir,s->view.right)/forward;
                    int column = (int)floorf(s->w*.5f+slope*cp*s->view.fx);
                    if (forward > 0 && column >= 0 && column < s->w) {
                        q = &s->column[c->first+column];
                        beam = q->hit && slope > q->lo+1e-6f && slope < q->hi-1e-6f;
                    }
                }
                if (beam) {
                    if (!q->hit && level) continue;
                    if (q->hit) {
                        float denominator = l3_dot(q->n,w->dir);
                        float depth = level ? q->t : fabsf(denominator) > 1e-8f ?
                            (q->plane-l3_dot(q->n,w->o))/denominator : -1;
                        float height = w->o.y+w->dir.y*depth-c->a.y;
                        if (depth >= s->view.near_z && depth <= far_t && height >= -1e-6f && height <= c->r2+1e-6f) {
                            w->hit = depth; w->p = l3_add(w->o,l3_scale(w->dir,depth));
                            w->n = q->n;
                            w->u = level ? q->u : fabsf(q->n.x) > .5f ?
                                (w->p.z-c->a.z)/c->r : (w->p.x-c->a.x)/c->r;
                            w->v = height*inv_height;
                            w->material = q->mat; w->sun = -1; cached = true;
                        } else if (depth >= s->view.near_z && w->o.y >= c->a.y && w->o.y <= c->a.y+c->r2) continue;
                    }
                }
            }
            if (!cached && !l3_world_hit(c,w,s->view.near_z,far_t)) continue;
            y_buffer |= bit;
            int mat = w->material >= 0 && w->material < s->nmat ? w->material : c->mat;
            write_pixel(s,c,command,mat,x,y,w,LAP_DOT_CLEAR);
        }
    }
    w->lod = 0;
}
static __attribute__((noinline)) void raster_rows(l3_scene *s, l3_rect r, int worker, int pass) {
    l3_work *w = &s->work[worker];
    w->depth_only = s->shadow_pass;
    w->sun = -1;
#ifdef L3_PROFILE
    w->counts = &s->counts[worker];
#endif
    int band = r.y0/4;
    /* Opaque wall columns first: reject hidden floor/ceiling dots before
     * position/normal writes. Ordered/no-depth scenes retain recording order. */
    for (int stage = 0; stage < (s->walls_first ? 2 : 1); stage++)
    for (int item = 0; item < s->bin_count; item++) {
        if (!(item&15)) {
            const l3_bin *g = &s->bin_group[item/16];
            if (band < g->first || band > g->last) { item += 15; continue; }
        }
        const l3_bin *b = &s->bin[item];
        if (band < b->first || band > b->last) continue;
        uint32_t entry = b->entry, index = entry & 0x3fffffffu;
        int command = entry & 0x80000000u ? s->tri[index].cmd
                    : entry & 0x40000000u ? s->particle[index].cmd : (int)entry;
        const l3_cmd *c = &s->cmd[command];
        if (s->walls_first && ((c->kind == S_WALLS) != (stage == 0))) continue;
        if (pass == 0 && (!(c->flags & L3_CAST) || c->kind >= S_LINE))
            continue;
        if (pass == 1 && !(c->flags & L3_STATIC))
            continue;
        if (pass == 2 && (c->flags & L3_STATIC))
            continue;
        if (c->rect.y1 <= r.y0 || c->rect.y0 >= r.y1 || c->rect.x1 <= r.x0 || c->rect.x0 >= r.x1)
            continue;
#ifdef L3_PROFILE
        uint64_t kind_start = l3_clock();
        int kind = c->kind == S_SPHERE ? 0 : c->kind == S_MESH ? 2 :
                   c->kind == S_TERRAIN ? 3 : c->kind == S_WALLS ? 4 :
                   c->kind == S_PARTICLES ? 5 : c->kind == S_FLOOR ? 6 : 1;
#endif
        if (entry & 0x80000000u) {
            raster_tri(s, &s->tri[index], r, w, worker); goto kind_done;
        }
        if (entry & 0x40000000u) {
            raster_particle(s, &s->particle[index], r, w); goto kind_done;
        }
        if (c->kind == S_TERRAIN || c->kind == S_WALLS) {
            raster_world(s,c,command,r,w); goto kind_done;
        }
        w->sun = -1;
        int x0 = c->rect.x0 > r.x0 ? c->rect.x0 : r.x0, x1 = c->rect.x1 < r.x1 ? c->rect.x1 : r.x1,
            ya = c->rect.y0 > r.y0 ? c->rect.y0 : r.y0, yb = c->rect.y1 < r.y1 ? c->rect.y1 : r.y1;
        for (int y = ya; y < yb; y++) {
            /* Rebase each row; no accumulated drift across rows. */
            l3_zray(&s->view, (float)x0, (float)y, &w->o, &w->dir);
            float ox = w->o.x, oy = w->o.y, oz = w->o.z;
            float dx = w->dir.x, dy = w->dir.y, dz = w->dir.z;
            l3_vec step = l3_scale(s->view.right, s->view.inv_fx);
            /* With an unrolled camera the floor denominator is row-constant.
             * Orthographic rays also share their denominator, even with roll. */
            bool floor_affine = c->kind == S_FLOOR && (s->view.ortho || step.y == 0);
            float floor_inv = floor_affine && fabsf(dy) >= 1.0e-8f ? 1.0f / dy : 0;
            if (floor_affine && floor_inv != 0)
                L3_COUNT(s, worker, plane_reciprocals);
            for (int x = x0; x < x1; x++) {
                float k = (float)(x - x0);
                w->o = s->view.ortho ? l3v(ox + k * step.x, oy + k * step.y, oz + k * step.z)
                                     : l3v(ox, oy, oz);
                w->dir = s->view.ortho ? l3v(dx, dy, dz)
                                       : l3v(dx + k * step.x, dy + k * step.y, dz + k * step.z);
                if (!s->shadow_pass && !w->full_dirty && (L3_BYTES(s, worker, 1), !s->dirty_mask[y * s->view.w + x]))
                    continue;
                L3_COUNT(s, worker, candidates);
                if (c->kind >= S_LINE) {
                    overlay(s, c, command, x, y, w);
                    continue;
                }
                int at = y * s->view.w + x;
                if (s->shadow_pass) L3_BYTES(s, worker, 4);
                else L3_LOCAL_BYTES(s, worker, 4);
                float old = s->shadow_pass ? (s->shadow[at] ? (s->shadow[at]-1)*s->shadow_inv : 0) : l3_work_pixel(s, w, x, y)->depth;
                float far_t = old > 0 && !(c->flags & L3_NO_DEPTH) ? fminf(s->view.far_z, old)
                                                                 : s->view.far_z;
                if (c->kind == S_FLOOR) {
                    /* 1/z and UV/z advance affinely, without a ray normalization. */
                    if (fabsf(w->dir.y) < 1.0e-8f)
                        continue;
                    if (floor_affine)
                        w->hit = (c->a.y - w->o.y) * floor_inv;
                    else {
                        L3_COUNT(s, worker, plane_reciprocals);
                        w->hit = (c->a.y - w->o.y) / w->dir.y;
                    }
                    if (w->hit < s->view.near_z || w->hit > far_t)
                        continue;
                    w->p = l3_add(w->o, l3_scale(w->dir, w->hit));
                    w->n = l3v(0, w->dir.y > 0 ? -1 : 1, 0);
                    w->sun = fmaxf(0,-w->n.y*s->sun.y);
                    w->u = w->p.x; w->v = w->p.z;
                    write_pixel(s, c, command, c->mat, x, y, w, LAP_DOT_CLEAR);
                    continue;
                }
                if (c->kind != S_SPHERE && c->kind != S_QUAD && c->kind != S_DISC) {
                    l3_vec q = l3_sub(w->o, c->center);
                    float a = l3_dot(w->dir, w->dir), b = l3_dot(q, w->dir);
                    float d = l3_dot(q, q) - c->bound * c->bound;
                    if (b * b < a * d || (b > 0 && d > 0))
                        continue;
                }
                L3_COUNT(s, worker, intersections);
                if (l3_intersect(c, w, s->view.near_z, far_t)) {
                    L3_COUNT(s, worker, writes);
                    write_pixel(s, c, command, c->mat, x, y, w, LAP_DOT_CLEAR);
                }
            }
        }
    kind_done:
#ifdef L3_PROFILE
        s->counts[worker].kind_us[kind] += (uint32_t)(l3_clock()-kind_start);
#endif
        ;
    }
}
/* Copy contiguous spans once. Clean bands never read or restore the cache. */
static __attribute__((noinline)) void transfer_rows(l3_scene *s, l3_rect r, int worker, int mode) {
    l3_work *w = &s->work[worker];
    for (int y = r.y0; y < r.y1; y++) {
        int x = r.x0;
        while (x < r.x1) {
            if (!w->full_dirty)
                while (x < r.x1 && (L3_BYTES(s, worker, 1), !s->dirty_mask[y * s->w + x])) x++;
            int first = x;
            if (w->full_dirty) x = r.x1;
            else while (x < r.x1 && (L3_BYTES(s, worker, 1), s->dirty_mask[y * s->w + x])) x++;
            size_t n = (size_t)(x - first);
            if (!n) continue;
            l3_pixel *local = l3_work_pixel(s, w, first, y);
            int at = y * s->w + first;
            L3_BYTES(s, worker, n * 8);
            L3_LOCAL_BYTES(s, worker, n * 8);
            if (mode == 0) memcpy(local, s->cache + at, n * 8);
            else if (mode == 1) memcpy(s->cache + at, local, n * 8);
            else {
                memcpy(s->pixels + at, local, n * 8);
                L3_BYTES(s, worker, n * sizeof(l3_key));
                for (size_t i = 0; i < n; i++) {
                    l3_key *key = &s->keys[at + i];
                    key->depth = l3_key_depth(local[i].depth);
                    key->tag = local[i].tag;
                    key->mat = local[i].mat;
                }
            }
        }
    }
}
/* A lamp sphere outside either horizontal tile plane cannot reach any dot
 * shaded in that tile. Plane normals need not be normalized. */
static uint8_t band_lamps(const l3_scene *s, l3_rect r) {
    uint8_t mask = 0;
    float top = (s->h*.5f-r.y0)*s->view.inv_fy;
    float bottom = (s->h*.5f-r.y1)*s->view.inv_fy;
    for (int i = 0; i < 4; i++) if (s->lamp[i].strength > 0) {
        l3_vec p = l3_sub(s->lamp[i].pos,s->view.eye);
        float y = l3_dot(p,s->view.up), z = l3_dot(p,s->view.forward);
        float range = s->lamp[i].range;
        if (s->view.ortho ? y-range > top || y+range < bottom :
            y-top*z > range*sqrtf(1+top*top) || bottom*z-y > range*sqrtf(1+bottom*bottom)) continue;
        mask |= (uint8_t)(1u<<i);
    }
    return mask;
}
static __attribute__((noinline)) void tile(l3_scene *s, l3_rect r, int worker) {
    l3_work *w = &s->work[worker];
    bool cacheable = s->cacheable && s->pass != 3;
    w->y0 = r.y0;
    s->lamp_mask[worker] = band_lamps(s,r);
    unsigned dirty = 0, n = (unsigned)(r.x1-r.x0) * (unsigned)(r.y1-r.y0);
    if (s->reuse) {
        for (int y = r.y0; y < r.y1; y++)
            for (int x = r.x0; x < r.x1; x++) {
                L3_BYTES(s, worker, 1);
                dirty += s->dirty_mask[y * s->w + x] != 0;
            }
    } else dirty = n;
    if (!dirty) {
        if (s->pass == 6) {
            atomic_store_explicit(&s->tile_ready[r.y0/2],1,memory_order_release);
            l3_resolve_try(s,r.y0/2,worker,false);
            l3_resolve_try(s,r.y0/2-1,worker,false);
        }
        return;
    }
    w->full_dirty = dirty == n;
    s->band_dots[worker] += dirty;
    uint64_t t = l3_clock();
    for (int y = r.y0; y < r.y1; y++) {
        memset(l3_work_pixel(s, w, r.x0, y), 0, (size_t)(r.x1-r.x0) * sizeof(l3_pixel));
        for (int x = r.x0; x < r.x1; x++)
            w->surface[(y-r.y0)*s->w+x].command = 0;
    }
    L3_LOCAL_BYTES(s, worker, (size_t)n * 10);
    if (!s->reuse || s->partial) raster_rows(s, r, worker, cacheable && !s->partial ? 1 : 3);
    s->counts[worker].us_raster += (uint32_t)(l3_clock()-t);
    if (s->reuse && !s->partial) {
        t = l3_clock();
        transfer_rows(s, r, worker, 0);
#ifdef L3_PROFILE
        s->counts[worker].us_cache += (uint32_t)(l3_clock()-t);
#endif
    }
    if (cacheable && !s->reuse) {
        t = l3_clock();
        shade_rows(s, r, worker);
        s->counts[worker].us_shade += (uint32_t)(l3_clock()-t);
        t = l3_clock();
        transfer_rows(s, r, worker, 1);
#ifdef L3_PROFILE
        s->counts[worker].us_cache += (uint32_t)(l3_clock()-t);
#endif
    }
    if (cacheable) {
        t = l3_clock();
        raster_rows(s, r, worker, 2);
        s->counts[worker].us_raster += (uint32_t)(l3_clock()-t);
    }
    t = l3_clock();
    shade_rows(s, r, worker);
    s->counts[worker].us_shade += (uint32_t)(l3_clock()-t);
    transfer_rows(s, r, worker, 2);
    if (s->pass == 6) l3_resolve_publish(s,r.y0/2,worker);
}
void l3_raster_band(l3_scene *s, int band, int worker) {
    if (s->pass == 7) { l3_columns(s,band,worker); return; }
    if (s->pass == 5) { l3_resolve_band(s, band, worker); return; }
    l3_rect r = s->dirty;
    if (!s->shadow_pass) {
        r.y0 &= ~1; r.y1 = (r.y1+1)&~1;
        if (r.y1 > s->h) r.y1 = s->h;
    }
    int y0 = band * 4, y1 = y0 + 4;
    if (r.y0 < y0) r.y0 = y0;
    if (r.y1 > y1) r.y1 = y1;
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    if (s->shadow_pass) {
        for (int y = r.y0; y < r.y1; y++)
            memset(s->shadow+y*s->view.w+r.x0, 0, (size_t)(r.x1-r.x0)*sizeof(uint16_t));
        L3_BYTES(s, worker, (size_t)(r.y1-r.y0)*(r.x1-r.x0)*4);
        raster_rows(s, r, worker, 0);
    } else {
        y1 = r.y1;
        for (int y = r.y0; y < y1; y += 2) {
            r.y0 = y; r.y1 = y+2 < y1 ? y+2 : y1;
            tile(s, r, worker);
        }
    }
}
void l3_run(l3_scene *s, int pass) {
    if (pass != 0 && s->dirty.x0 >= s->dirty.x1) {
        if (pass != 5)
            s->band_dots[0] = s->band_dots[1] = 0;
        return;
    }
    s->pass = pass;
    if (pass == 6) for (int tile = 0; tile < (s->h+1)/2; tile++) {
        int halo = s->outline != LAP_DOT_CLEAR;
        bool pending = tile*2 < s->dirty.y1 && tile*2+2 > s->dirty.y0;
        bool touched = tile*2 < s->dirty.y1+halo && tile*2+2 > s->dirty.y0-halo;
        atomic_store(&s->tile_ready[tile],pending ? 0 : touched ? 1 : 2);
    }
    s->bands = pass == 7 ? s->w : (s->view.h + 3) / 4;
    if (pass != 5)
        s->band_dots[0] = s->band_dots[1] = 0;
    atomic_store(&s->next_band, 0);
    atomic_store(&s->completed, 0);
    l3_worker_start(s);
    for (;;) {
        int band = atomic_fetch_add(&s->next_band, 1);
        if (band >= s->bands)
            break;
        l3_raster_band(s, band, 0);
        atomic_fetch_add_explicit(&s->completed, 1, memory_order_release);
    }
    l3_worker_finish(s);
    if (pass == 6) for (int tile = 0; tile < (s->h+1)/2; tile++) {
        /* Entirely clean tiles are published without touching their cache. */
        if (!atomic_load(&s->tile_ready[tile])) atomic_store(&s->tile_ready[tile],1);
        l3_resolve_try(s,tile,0,false);
    }
}
