/* Integer ordered-dither resolve, dispatched on the shared row-band workers. */
#include "l3_private.h"
static inline int bayer(int x, int y) {
    /* Bit interleave produces the existing lc_ramp 4 x 4 matrix. */
    int a = x ^ y;
    return ((a & 1) << 3) | ((y & 1) << 2) | (a & 2) | ((y & 2) >> 1);
}
static inline uint32_t depth_bits(float z) {
    uint32_t bits;
    memcpy(&bits, &z, sizeof bits);
    return bits;
}
static inline bool boundary(l3_scene *s, const l3_pixel *p, int at,
                            uint32_t lo, uint32_t hi, int worker) {
    L3_LOCAL_BYTES(s, worker, 4);
    /* Little-endian P4 and host: one aligned word instead of three PSRAM
     * field loads. Keep the float read for ambiguous depth threshold ties. */
    uint32_t key; memcpy(&key,&s->keys[at],sizeof key);
    uint32_t depth = key & 65535u;
    if (!depth || (key>>16) != ((uint32_t)p->tag | ((uint32_t)p->mat<<8))) return true;
    uint32_t lower = lo >> 16, upper = hi >> 16;
    if (depth < lower || depth > upper) return true;
    if (depth > lower && depth < upper) return false;
    /* The narrow key cannot decide a threshold tie. Read the original float
     * only here, retaining every original outline and public depth value. */
    L3_BYTES(s, worker, 4);
    uint32_t bits = depth_bits(s->pixels[at].depth);
    return bits < lo || bits > hi;
}
static __attribute__((noinline)) void resolve_rows(l3_scene *s, int y0, int y1, int worker, bool local) {
    l3_work *w = &s->work[worker];
    int x0 = 0, x1 = s->w;
    if (s->reuse) {
        int halo = s->outline != LAP_DOT_CLEAR;
        if (y1 <= s->dirty.y0-halo || y0 >= s->dirty.y1+halo) return;
        x0 = s->dirty.x0 > halo ? s->dirty.x0-halo : 0;
        x1 = s->dirty.x1+halo < s->w ? s->dirty.x1+halo : s->w;
    }
    if (x0 >= x1) return;
    if (!local) {
    w->y0 = y0;
    for (int y = y0; y < y1; y++)
        memcpy(l3_work_pixel(s, w, x0, y), s->pixels+y*s->w+x0, (size_t)(x1-x0)*sizeof(l3_pixel));
    L3_BYTES(s, worker, (size_t)(y1-y0)*(x1-x0)*sizeof(l3_pixel));
    L3_LOCAL_BYTES(s, worker, (size_t)(y1-y0)*(x1-x0)*sizeof(l3_pixel));
    }
    float fog_inv = s->fog_end > s->fog_start ? 1.0f / (s->fog_end - s->fog_start) : 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int at = y * s->w + x;
            if (s->reuse) {
                /* Outline needs a one-dot halo around changed visibility. */
                L3_BYTES(s, worker, s->outline != LAP_DOT_CLEAR ? 5 : 1);
                bool dirty = s->dirty_mask[at];
                if (s->outline != LAP_DOT_CLEAR)
                    dirty |= (x && s->dirty_mask[at - 1]) ||
                             (x + 1 < s->w && s->dirty_mask[at + 1]) ||
                             (y && s->dirty_mask[at - s->w]) ||
                             (y + 1 < s->h && s->dirty_mask[at + s->w]);
                if (!dirty)
                    continue;
            }
            L3_BYTES(s, worker, 1);
            L3_LOCAL_BYTES(s, worker, 8);
            L3_COUNT(s, worker, resolves);
            const l3_pixel *p = l3_work_pixel(s, w, x, y);
            int threshold = 2 * bayer(x, y) + 1;
            uint8_t ink;
            if (!p->object)
                ink = 32 * y > threshold * (s->h > 1 ? s->h - 1 : 1) ? s->sky_bottom : s->sky_top;
            else if (p->ink != LAP_DOT_CLEAR)
                ink = p->ink;
            else {
                const l3_mat *m = &s->material[p->mat];
                int f = p->light * (m->n - 1);
                int lo = f / 255, index = lo + (32 * (f - lo * 255) > threshold * 255);
                if (m->flags & L3_FLAT)
                    index = (f + 127) / 255;
                if (index >= m->n)
                    index = m->n - 1;
                ink = m->ink[index];
                if (s->outline != LAP_DOT_CLEAR) {
                    float z = p->depth;
                    float delta = fmaxf(0.08f, z * 0.045f);
                    uint32_t lo_depth = depth_bits(fmaxf(0, z - delta)), hi_depth = depth_bits(z + delta);
                    if (x == 0 || y == 0 || x == s->w - 1 || y == s->h - 1 ||
                        boundary(s, p, at - 1, lo_depth, hi_depth, worker) ||
                        boundary(s, p, at + 1, lo_depth, hi_depth, worker) ||
                        boundary(s, p, at - s->w, lo_depth, hi_depth, worker) ||
                        boundary(s, p, at + s->w, lo_depth, hi_depth, worker))
                        ink = m->flags & L3_GLOW ? m->ink[m->n - 1] : s->outline;
                }
                if (fog_inv > 0) {
                    float dx = (x + 0.5f - s->w * 0.5f) * s->camera.inv_fx,
                          dy = (s->h * 0.5f - y - 0.5f) * s->camera.inv_fy;
                    float distance = s->camera.ortho ? sqrtf(p->depth * p->depth + dx * dx + dy * dy)
                                                     : p->depth;
                    if ((distance - s->fog_start) * fog_inv * 32 > threshold)
                        ink = s->fog_ink;
                }
            }
            s->dots[at] = ink;
        }
}
void l3_resolve_band(l3_scene *s, int band, int worker) {
    uint64_t start = l3_clock();
    int end = band*4+4 < s->h ? band*4+4 : s->h;
    for (int y = band*4; y < end; y += 2)
        resolve_rows(s, y, y+2 < end ? y+2 : end, worker, false);
    s->counts[worker].us_resolve += (uint32_t)(l3_clock()-start);
}
void l3_resolve(l3_scene *s) { l3_run(s, 5); }

/* A published tile owns finished visibility and material keys. Resolving is
 * claimed only after both one-row neighbours are published; no worker waits
 * for another, and no full-frame resolve job is launched. */
void l3_resolve_try(l3_scene *s, int tile, int worker, bool local) {
    int tiles = (s->h+1)/2;
    if (tile < 0 || tile >= tiles) return;
    if (atomic_load_explicit(&s->tile_ready[tile],memory_order_acquire) != 1) return;
    if (s->outline != LAP_DOT_CLEAR &&
        ((tile && !atomic_load_explicit(&s->tile_ready[tile-1],memory_order_acquire)) ||
         (tile+1 < tiles && !atomic_load_explicit(&s->tile_ready[tile+1],memory_order_acquire)))) return;
    unsigned char ready = 1;
    if (!atomic_compare_exchange_strong_explicit(&s->tile_ready[tile],&ready,2,
                                                  memory_order_acq_rel,memory_order_relaxed)) return;
    uint64_t start = l3_clock();
    resolve_rows(s,tile*2,tile*2+2 < s->h ? tile*2+2 : s->h,worker,local);
    s->counts[worker].us_resolve += (uint32_t)(l3_clock()-start);
}
void l3_resolve_publish(l3_scene *s, int tile, int worker) {
    atomic_store_explicit(&s->tile_ready[tile],1,memory_order_release);
    l3_resolve_try(s,tile,worker,s->work[worker].full_dirty &&
                    (!s->reuse || s->outline == LAP_DOT_CLEAR));
    l3_resolve_try(s,tile-1,worker,false);
    l3_resolve_try(s,tile+1,worker,false);
}
