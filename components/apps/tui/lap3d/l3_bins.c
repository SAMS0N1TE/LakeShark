/* Frame-local band lists in reserved PSRAM; preserve recording order. */
#include "l3_private.h"
void l3_particles_prepare(l3_scene *s, int command) {
    l3_cmd *c = &s->cmd[command];
    c->first = s->nparticle;
    for (int i = 0; i < c->np; i++) {
        l3_vec p = c->points[i];
        float x, y, z = l3_dot(l3_sub(p,s->view.eye),s->view.forward);
        if (z+c->r < s->view.near_z || z-c->r > s->view.far_z) continue;
        if (!l3_view_project(&s->view,p,&x,&y)) continue;
        float inv = s->view.ortho ? 1 : 1/z;
        float rx = c->r*s->view.fx*inv, ry = c->r*s->view.fy*inv;
        if (!isfinite(x) || !isfinite(y) || !isfinite(rx) || !isfinite(ry)) continue;
        if (x+rx < 0 || y+ry < 0 || x-rx >= s->view.w || y-ry >= s->view.h) continue;
        if (s->nparticle == s->tricap) { s->stats.dropped++; break; }
        l3_particle *q = &s->particle[s->nparticle++];
        q->p = p; q->x = x; q->y = y; q->z = z; q->rx = rx; q->ry = ry; q->cmd = command;
        q->rect = (l3_rect){(int)fmaxf(0,floorf(x-rx)),(int)fmaxf(0,floorf(y-ry)),
                             (int)fminf(s->view.w,ceilf(x+rx+1)),(int)fminf(s->view.h,ceilf(y+ry+1))};
    }
    c->count = s->nparticle-c->first;
}
static void add(l3_scene *s, l3_rect r, uint32_t entry) {
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    int first = r.y0/4, last = (r.y1-1)/4;
    if (first < 0) first = 0;
    if (last >= s->bin_bands) last = s->bin_bands-1;
    if (first > last) return;
    if (s->bin_count == s->bin_stride) { s->stats.dropped++; return; }
    s->bin[s->bin_count++] = (l3_bin){entry,(uint16_t)first,(uint16_t)last};
}
static bool same(l3_vec a, l3_vec b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
static void wire_edges(l3_scene *s, const l3_cmd *c, int index) {
    l3_tri *t = &s->tri[index];
    for (int edge = 0; edge < 3; edge++) {
        l3_vec a = t->v[edge].p, b = t->v[(edge+1)%3].p;
        for (int i = c->first; i < index; i++) {
            const l3_tri *q = &s->tri[i];
            for (int e = 0; e < 3; e++) {
                l3_vec x = q->v[e].p, y = q->v[(e+1)%3].p;
                if ((same(a,x) && same(b,y)) || (same(a,y) && same(b,x))) {
                    t->edges &= (uint8_t)~(1<<edge); goto next;
                }
            }
        }
        next:;
    }
}
void l3_bins_prepare(l3_scene *s) {
    s->bin_count = 0;
    for (int i = 0; i < s->ncmd; i++) {
        const l3_cmd *c = &s->cmd[i];
        if ((c->kind == S_TERRAIN && s->shadow_pass) || c->kind == S_MESH || c->kind == S_BOX || c->kind == S_RBOX || c->kind == S_QUAD || c->kind == S_DISC) {
            for (int j = c->first; j < c->first+c->count; j++) {
                if (c->kind == S_MESH && (c->flags & L3_WIRE)) wire_edges(s,c,j);
                add(s,s->tri[j].rect,0x80000000u|(uint32_t)j);
            }
        } else if (c->kind == S_PARTICLES) {
            for (int j = c->first; j < c->first+c->count; j++) add(s,s->particle[j].rect,0x40000000u|(uint32_t)j);
        } else add(s,c->rect,(uint32_t)i);
    }
    /* Small range unions let a band skip sixteen entries at once without an
     * overlap list, allocation, or changing recording order. */
    for (int i = 0; i < s->bin_count; i++) {
        l3_bin *g = &s->bin_group[i/16];
        if (!(i&15)) { g->first = s->bin[i].first; g->last = s->bin[i].last; }
        else {
            if (s->bin[i].first < g->first) g->first = s->bin[i].first;
            if (s->bin[i].last > g->last) g->last = s->bin[i].last;
        }
    }
}
