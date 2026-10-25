/* Height fields, grid walls, particle recording and reusable CPU queries. */
#include "l3_private.h"
void l3_camera_look(l3_scene *s, l3_vec eye, float yaw, float pitch, float fov) {
    yaw *= L3_PI/180; pitch *= L3_PI/180;
    l3_vec d = l3v(sinf(yaw)*cosf(pitch), sinf(pitch), -cosf(yaw)*cosf(pitch));
    l3_camera(s, eye, l3_add(eye, d), fov);
}
void l3_camera_orbit(l3_scene *s, l3_vec target, float distance, float yaw, float pitch, float fov) {
    if (!(distance > 0) || !isfinite(distance)) return;
    yaw *= L3_PI/180; pitch *= L3_PI/180;
    l3_vec offset = l3v(-sinf(yaw)*cosf(pitch), sinf(pitch), cosf(yaw)*cosf(pitch));
    l3_camera(s, l3_add(target, l3_scale(offset, distance)), target, fov);
}
void l3_terrain(l3_scene *s, const uint8_t *h, const uint8_t *m, int w, int d,
                float cell, float scale, l3_vec origin, int mat) {
    if (!h || w < 2 || d < 2 || w > 65535 || d > 65535 || !(cell > 0) ||
        !isfinite(cell) || !isfinite(scale) || scale < 0) return;
    l3_cmd *c = l3_record(s, S_TERRAIN, mat);
    if (!c) return;
    c->map = h; c->map_mats = m; c->map_w = w; c->map_d = d;
    c->r = cell; c->r2 = scale; c->a = origin; c->b.x = 1/cell;
}
void l3_walls(l3_scene *s, const uint8_t *map, int w, int d, float cell, float height, l3_vec origin) {
    if (!s || !s->nmat || !map || w < 1 || d < 1 || w > 65535 || d > 65535 ||
        !(cell > 0) || !(height > 0) || !isfinite(cell) || !isfinite(height)) return;
    l3_cmd *c = l3_record(s, S_WALLS, 0);
    if (!c) return;
    c->map = map; c->map_w = w; c->map_d = d;
    c->r = cell; c->r2 = height; c->a = origin;
}
void l3_particles(l3_scene *s, const l3_vec *p, int n, float size, int mat) {
    if (!p || n < 1 || !(size > 0) || !isfinite(size)) return;
    l3_cmd *c = l3_record(s, S_PARTICLES, mat);
    if (!c) return;
    c->points = p; c->np = n; c->r = size*.5f;
}
void l3_world_bounds(l3_cmd *c) {
    if (c->kind == S_PARTICLES) {
        l3_vec lo = c->points[0], hi = lo;
        for (int i = 1; i < c->np; i++) {
            l3_vec p = c->points[i];
            lo.x = fminf(lo.x,p.x); lo.y = fminf(lo.y,p.y); lo.z = fminf(lo.z,p.z);
            hi.x = fmaxf(hi.x,p.x); hi.y = fmaxf(hi.y,p.y); hi.z = fmaxf(hi.z,p.z);
        }
        c->center = l3_scale(l3_add(lo,hi),.5f);
        c->bound = l3_length(l3_sub(hi,lo))*.5f+c->r;
    } else {
        float x = (c->map_w-(c->kind == S_TERRAIN))*c->r;
        float z = (c->map_d-(c->kind == S_TERRAIN))*c->r;
        float y = c->r2, low = 0;
        if (c->kind == S_TERRAIN) {
            unsigned lo = 255, hi = 0;
            for (size_t i = 0; i < (size_t)c->map_w*c->map_d; i++) {
                if (c->map[i] < lo) lo = c->map[i];
                if (c->map[i] > hi) hi = c->map[i];
            }
            low = lo*c->r2; y = (hi-lo)*c->r2;
        }
        c->half = l3v(x*.5f,y*.5f,z*.5f);
        c->center = l3_add(c->a,c->half);
        c->center.y += low;
        c->bound = l3_length(c->half);
    }
    c->shadow_bound = c->bound;
}
bool l3_ray_triangle(l3_vec a, l3_vec b, l3_vec c, l3_work *w,
                     float min_t, float max_t, bool two) {
    l3_vec e = l3_sub(b,a), f = l3_sub(c,a), p = l3_cross(w->dir,f);
    float det = l3_dot(e,p);
    if (two ? fabsf(det) < 1e-8f : det < 1e-8f) return false;
    float inv = 1/det;
    l3_vec q = l3_sub(w->o,a);
    float u = l3_dot(q,p)*inv;
    if (u < -1e-6f || u > 1.000001f) return false;
    q = l3_cross(q,e);
    float v = l3_dot(w->dir,q)*inv;
    if (v < -1e-6f || u+v > 1.000001f) return false;
    float t = l3_dot(f,q)*inv;
    if (t < min_t || t > max_t) return false;
    w->hit = t; w->p = l3_add(w->o,l3_scale(w->dir,t));
    w->n = l3_unit(l3_cross(e,f));
    w->u = w->p.x; w->v = w->p.z;
    return true;
}
static __attribute__((noinline)) bool terrain_cell(const l3_cmd *c, l3_work *w,
                                                    int x, int z, int span_x, int span_z, float lo, float hi) {
    int at = z*c->map_w+x;
    float px = c->a.x+x*c->r, pz = c->a.z+z*c->r;
    float ix = 1/(span_x*c->r), iz = 1/(span_z*c->r);
    float h00 = c->a.y+c->map[at]*c->r2;
    float h10 = c->a.y+c->map[at+span_x]*c->r2;
    float h01 = c->a.y+c->map[at+span_z*c->map_w]*c->r2;
    float h11 = c->a.y+c->map[at+span_z*c->map_w+span_x]*c->r2;
    for (int side = 0; side < 2; side++) {
        float gx = (side ? h11-h01 : h10-h00)*ix;
        float gz = (side ? h11-h10 : h01-h00)*iz;
        float denominator = w->dir.y-gx*w->dir.x-gz*w->dir.z;
        if (fabsf(denominator) < 1e-8f) continue;
        float height = side ? h11-gx*(span_x*c->r)-gz*(span_z*c->r) : h00;
        float t = (height+gx*(w->o.x-px)+gz*(w->o.z-pz)-w->o.y)/denominator;
        if (t < lo || t > hi) continue;
        float u = (w->o.x+w->dir.x*t-px)*ix, v = (w->o.z+w->dir.z*t-pz)*iz;
        if (u < -1e-6f || v < -1e-6f || u > 1.000001f || v > 1.000001f ||
            (side ? u+v < .999999f : u+v > 1.000001f)) continue;
        w->hit = t; w->p = l3_add(w->o,l3_scale(w->dir,t));
        w->n = l3_unit(l3v(-gx,1,-gz)); w->u = w->p.x; w->v = w->p.z;
        w->material = c->map_mats ? c->map_mats[at] : c->mat;
        return true;
    }
    return false;
}

static bool interval(float o, float d, float extent, float *lo, float *hi) {
    if (fabsf(d) < 1e-20f) return o >= -1e-6f && o <= extent+1e-6f;
    float a = -o/d, b = (extent-o)/d;
    *lo = fmaxf(*lo,fminf(a,b)); *hi = fminf(*hi,fmaxf(a,b));
    return *lo <= *hi;
}
/* Intersect angular coverage of every traversed empty cell. A ray inside
 * this interval visits the same corridor and the cached first solid face. */
static void column_interval(const l3_cmd *c, l3_work *w, int x, int z, l3_vec normal) {
    float lo = L3_INF, hi = -L3_INF;
    for (int k = 0; k < 4; k++) {
        if (normal.x && (k&1) != (normal.x > 0)) continue;
        if (normal.z && ((k>>1)&1) != (normal.z > 0)) continue;
        l3_vec p = l3v(c->a.x+(x+(k&1))*c->r-w->o.x,0,
                       c->a.z+(z+((k>>1)&1))*c->r-w->o.z);
        float d = l3_dot(p,w->local);
        if (d <= 1e-7f) return; /* cell containing the eye imposes no limit */
        float slope = l3_dot(p,w->ld)/d;
        lo = fminf(lo,slope); hi = fmaxf(hi,slope);
    }
    w->beam_lo = fmaxf(w->beam_lo,lo); w->beam_hi = fminf(w->beam_hi,hi);
}
static __attribute__((noinline)) bool grid_hit(const l3_cmd *c, l3_work *w, float lo, float hi) {
    int width = c->map_w-(c->kind == S_TERRAIN), depth = c->map_d-(c->kind == S_TERRAIN);
    float ox = w->o.x-c->a.x, oz = w->o.z-c->a.z;
    /* A reused frame can skip preparation entirely. Query bounds therefore
     * come from recorded inputs, not transient projected-command metadata. */
    float bottom = c->kind == S_WALLS || w->lod == 0 ? c->a.y : c->center.y-c->half.y;
    float extent = c->kind == S_WALLS ? c->r2 : w->lod == 0 ? 255*c->r2 : 2*c->half.y;
    if (!interval(w->o.y-bottom,w->dir.y,extent,&lo,&hi)) return false;
    if (!interval(ox,w->dir.x,width*c->r,&lo,&hi) ||
        !interval(oz,w->dir.z,depth*c->r,&lo,&hi)) return false;
    float t = lo;
    int x = (int)l3_clamp((float)l3_ifloor((ox+w->dir.x*t)/c->r),0,(float)width-1);
    int z = (int)l3_clamp((float)l3_ifloor((oz+w->dir.z*t)/c->r),0,(float)depth-1);
    int sx = w->dir.x < 0 ? -1 : 1, sz = w->dir.z < 0 ? -1 : 1;
    float dx = fabsf(w->dir.x) > 1e-20f ? c->r/fabsf(w->dir.x) : L3_INF;
    float dz = fabsf(w->dir.z) > 1e-20f ? c->r/fabsf(w->dir.z) : L3_INF;
    float tx = dx < L3_INF ? ((x+(sx>0))*c->r-ox)/w->dir.x : L3_INF;
    float tz = dz < L3_INF ? ((z+(sz>0))*c->r-oz)/w->dir.z : L3_INF;
    for (int step = 0; step <= width+depth && x >= 0 && z >= 0 && x < width && z < depth && t <= hi; step++) {
        int span_x = 1, span_z = 1;
        if (c->kind == S_TERRAIN && w->lod != 0) {
            float footprint = w->lod < 0 ? -w->lod : t*w->lod;
            int stride = 1;
            while (stride < 32 && stride*2 <= footprint) stride *= 2;
            if (stride > 1) {
                x = l3_ifloor((ox+w->dir.x*(t+1e-5f))/c->r);
                z = l3_ifloor((oz+w->dir.z*(t+1e-5f))/c->r);
                x = x < 0 ? 0 : x >= width ? width-1 : x;
                z = z < 0 ? 0 : z >= depth ? depth-1 : z;
                x = x/stride*stride; z = z/stride*stride;
                span_x = x+stride <= width ? stride : width-x;
                span_z = z+stride <= depth ? stride : depth-z;
                tx = dx < L3_INF ? ((x+(sx>0 ? span_x : 0))*c->r-ox)/w->dir.x : L3_INF;
                tz = dz < L3_INF ? ((z+(sz>0 ? span_z : 0))*c->r-oz)/w->dir.z : L3_INF;
            }
        }
        if (w->column_query) column_interval(c,w,x,z,l3v(0,0,0));
        float end = fminf(hi,fminf(tx,tz));
        if (c->kind == S_TERRAIN) {
            if (terrain_cell(c,w,x,z,span_x,span_z,t-1e-5f,end+1e-5f)) return true;
        } else if (c->map[z*c->map_w+x]) {
            /* Full 3D slab inside the DDA cell also handles pitch, looking
             * over a near wall, entry through its top and cameras inside. */
            float a = t, b = end;
            if (interval(w->o.y-c->a.y,w->dir.y,c->r2,&a,&b)) {
                w->hit = a; w->p = l3_add(w->o,l3_scale(w->dir,a));
                /* Starting inside a solid cell hits its exit, as l3_box does. */
                for (int exit = 0; exit < 2; exit++) {
                    float py = w->p.y-c->a.y, px = w->p.x-c->a.x-x*c->r;
                    float pz = w->p.z-c->a.z-z*c->r;
                    w->n = fabsf(py) < 1e-4f ? l3v(0,-1,0) : fabsf(py-c->r2)<1e-4f ? l3v(0,1,0)
                         : fabsf(px)<1e-4f ? l3v(-1,0,0) : fabsf(px-c->r)<1e-4f ? l3v(1,0,0)
                         : fabsf(pz)<1e-4f ? l3v(0,0,-1) : fabsf(pz-c->r)<1e-4f ? l3v(0,0,1)
                         : l3v(0,0,0);
                    if (l3_dot(w->n,w->n) > 0) break;
                    w->hit = b; w->p = l3_add(w->o,l3_scale(w->dir,b));
                }
                if (l3_dot(w->n,w->n) == 0) return false;
                float py = w->p.y-c->a.y;
                w->u = fabsf(w->n.x) > .5f ? (w->p.z-c->a.z)/c->r : (w->p.x-c->a.x)/c->r;
                w->v = py/c->r2; w->material = c->map[z*c->map_w+x]-1;
                if (w->column_query) column_interval(c,w,x,z,w->n);
                return true;
            }
        }
        if (end >= hi) break;
        if (tx < tz) { t = tx; tx += dx*span_x; x += sx*span_x; }
        else { t = tz; tz += dz*span_z; z += sz*span_z; }
    }
    return false;
}
/* The coarse sun mesh is built only on a caster/sun cache miss. It uses the
 * shared clipper and rasterizer, never a height-grid walk per shadow texel. */
static void terrain_corner(l3_scene *s, const l3_cmd *c, int i, int x, int z) {
    l3_corner *v = &s->clip_a[i];
    v->p = l3v(c->a.x+x*c->r,c->a.y+c->map[z*c->map_w+x]*c->r2,c->a.z+z*c->r);
    v->n = l3v(0,1,0);
    v->z = l3_dot(l3_sub(v->p,s->view.eye),s->view.forward);
}
void l3_terrain_shadow(l3_scene *s, int command) {
    l3_cmd *c = &s->cmd[command];
    c->first = s->ntri;
    for (int z = 0; z+1 < c->map_d; z += 4) for (int x = 0; x+1 < c->map_w; x += 4) {
        int xx = x+4 < c->map_w ? x+4 : c->map_w-1;
        int zz = z+4 < c->map_d ? z+4 : c->map_d-1;
        terrain_corner(s,c,0,x,z); terrain_corner(s,c,1,xx,z); terrain_corner(s,c,2,x,zz);
        l3_submit_triangle(s,command,c->mat);
        terrain_corner(s,c,0,xx,zz); terrain_corner(s,c,1,x,zz); terrain_corner(s,c,2,xx,z);
        l3_submit_triangle(s,command,c->mat);
    }
    c->count = s->ntri-c->first;
}
/* Bilinear height and gradient. Coordinates outside the finite map are never
 * extended: the near and far silhouettes stay open, as with the query mesh. */
static inline bool height_at(const l3_cmd *c, l3_work *w, float px, float pz) {
    float u = (px-c->a.x)*c->b.x, v = (pz-c->a.z)*c->b.x;
    bool inside = u >= 0 && v >= 0 && u <= c->map_w-1 && v <= c->map_d-1;
    u = l3_clamp(u,0,(float)c->map_w-1); v = l3_clamp(v,0,(float)c->map_d-1);
    int x = l3_ifloor(u), z = l3_ifloor(v);
    if (x == c->map_w-1) x--;
    if (z == c->map_d-1) z--;
    u -= x; v -= z;
    int at = z*c->map_w+x;
    float a = c->map[at]*c->r2, b = c->map[at+1]*c->r2,
          d = c->map[at+c->map_w]*c->r2, e = c->map[at+c->map_w+1]*c->r2;
    w->p = l3v(px,c->a.y+(a+(b-a)*u)*(1-v)+(d+(e-d)*u)*v,pz);
    w->n = l3v(-((b-a)*(1-v)+(e-d)*v)*c->b.x,1,-((d-a)*(1-u)+(e-b)*u)*c->b.x);
    w->material = c->map_mats ? c->map_mats[at] : c->mat;
    return inside;
}
static bool terrain_sample(const l3_cmd *c, const l3_view *v, l3_work *w, float bx, float bz, float sx, float *height) {
    bool valid = false;
    for (int iteration = 0; iteration < 3; iteration++) {
        float shift = sx*v->forward.y*(*height-v->eye.y);
        valid = height_at(c,w,bx+v->right.x*shift,bz+v->right.z*shift);
        float delta = fabsf(*height-w->p.y);
        *height = w->p.y;
        if (delta < .00001f) break;
    }
    return valid;
}
static __attribute__((noinline)) void terrain_column(l3_scene *s, int command, int x, int worker) {
    const l3_cmd *c = &s->cmd[command];
    const l3_view *v = &s->camera;
    l3_work *w = &s->work[worker];
    float cp = sqrtf(1-v->forward.y*v->forward.y);
    if (cp < .01f) return;
    float sx = (x+.5f-v->w*.5f)*v->inv_fx;
    float dx = v->forward.x/cp+v->right.x*sx*cp;
    float dz = v->forward.z/cp+v->right.z*sx*cp;
    float end = v->far_z;
    if (s->fog_end > s->fog_start) end = fminf(end,s->fog_end);
    float begin = 0;
    /* Height-dependent lateral shift keeps each sample in the exact camera
     * column plane, even with pitch. Expand only the traversal interval. */
    float margin = fabsf(sx*v->forward.y)*(fabsf(c->a.y-v->eye.y)+255*c->r2);
    if (!interval(v->eye.x-c->a.x+margin,dx,(c->map_w-1)*c->r+2*margin,&begin,&end) ||
        !interval(v->eye.z-c->a.z+margin,dz,(c->map_d-1)*c->r+2*margin,&begin,&end)) return;
    int ceiling = v->h;
    bool previous = false;
    float py = 0, iz = 0, height = c->a.y, last_distance = begin;
    bool was_inside = false;
    for (float distance = begin; distance <= end && ceiling > 0;) {
        float bx = v->eye.x+dx*distance, bz = v->eye.z+dz*distance;
        bool valid = terrain_sample(c,v,w,bx,bz,sx,&height);
        bool exit = false;
        /* Refine finite-map entry/exit once, rather than letting the distance
         * step cut a sawtooth into the near silhouette. */
        if (valid != was_inside && distance > last_distance) {
            float lo = last_distance, hi = distance;
            for (int k = 0; k < 12; k++) {
                float middle = (lo+hi)*.5f;
                bool inside = terrain_sample(c,v,w,v->eye.x+dx*middle,v->eye.z+dz*middle,sx,&height);
                if (inside == was_inside) lo = middle; else hi = middle;
            }
            float edge = valid ? hi : lo;
            terrain_sample(c,v,w,v->eye.x+dx*edge,v->eye.z+dz*edge,sx,&height);
            distance = edge; exit = !valid; valid = true;
        }
        if (valid) {
            height = w->p.y;
            float z = cp*distance+v->forward.y*(height-v->eye.y);
            if (z >= v->near_z && z <= v->far_z) {
                float inv = 1/z;
                float row = v->h*.5f-.5f-v->fy*(cp*(height-v->eye.y)-v->forward.y*distance)*inv;
                if (!previous) {
                    int first = (int)l3_clamp(ceilf(row),0,(float)v->h);
                    if (first < ceiling) ceiling = first;
                }
                int top = (int)l3_clamp(ceilf(row),0,(float)v->h);
                if (previous && top < ceiling && row < py) {
                    float slope = (inv-iz)/(row-py);
                    w->n = l3_unit(w->n);
                    int8_t nx = (int8_t)(w->n.x*127), nz = (int8_t)(w->n.z*127);
                    for (int y = top; y < ceiling; y++) {
                        float depth = 1/(iz+(y-py)*slope);
                        l3_land *q = &s->land[((c->flags & L3_STATIC) ? 0 : s->w*s->h)+x*s->h+y];
                        if (!(c->flags & L3_NO_DEPTH) && q->command && depth >= q->depth) continue;
                        q->depth = depth; q->command = (uint16_t)(command+1);
                        q->mat = w->material < s->nmat ? w->material : c->mat;
                        q->nx = nx; q->nz = nz;
                    }
                    ceiling = top;
                }
                py = row; iz = inv; previous = true;
            }
        }
        /* Sub-cell near samples, then a sub-dot projected footprint. Bound
         * the largest step so narrow terrain features cannot be skipped. */
        was_inside = valid && !exit;
        if (exit) previous = false;
        last_distance = distance;
        if (distance == end) break;
        float step = fminf(c->r*4,fmaxf(c->r*.5f,distance*v->inv_fy*1.5f));
        if (c->map_w <= 16 && c->map_d <= 16) step = c->r*.125f;
        distance = fminf(end,distance+step);
    }
}
void l3_columns(l3_scene *s, int x, int worker) {
    uint64_t time = l3_clock();
    for (int layer = 0; layer < 2; layer++)
        for (int y = 0; y < s->h; y++) s->land[layer*s->w*s->h+x*s->h+y].command = 0;
    for (int i = 0; i < s->ncmd; i++) if (s->cmd[i].kind == S_TERRAIN && s->cmd[i].map_w > 2 && s->cmd[i].map_d > 2) {
#ifdef L3_PROFILE
        uint64_t start = l3_clock();
#endif
        terrain_column(s,i,x,worker);
#ifdef L3_PROFILE
        s->counts[worker].kind_us[3] += (uint32_t)(l3_clock()-start);
#endif
    }
    s->counts[worker].us_raster += (uint32_t)(l3_clock()-time);
}
/* One horizontal DDA per column, with an angular corridor and exact face
 * plane. Pitched rays reuse a certified corridor; rays crossing its edge,
 * rolled cameras and wall tops retain the exact plane/slab query fallback. */
void l3_walls_prepare(l3_scene *s, int command) {
    l3_cmd *c = &s->cmd[command];
    c->first = -1;
    if (fabsf(s->view.right.y) > 1e-7f || s->ncolumn+s->view.w > s->columncap) return;
    c->first = s->ncolumn; s->ncolumn += s->view.w;
    l3_work *w = &s->query;
    w->lod = 0;
    w->column_query = true;
    w->local = l3_unit(l3v(s->view.forward.x,0,s->view.forward.z));
    w->ld = s->view.right;
    for (int x = 0; x < s->view.w; x++) {
        l3_wall_column *q = &s->column[c->first+x];
        l3_zray(&s->view,(float)x,(s->view.h-1)*.5f,&w->o,&w->dir);
        w->o.y = c->a.y+c->r2*.5f; w->dir.y = 0;
        w->beam_lo = -L3_INF; w->beam_hi = L3_INF;
        q->hit = grid_hit(c,w,s->view.near_z,s->view.far_z);
        if (!q->hit) continue;
        q->t = w->hit; q->u = w->u; q->n = w->n; q->mat = w->material;
        q->plane = l3_dot(w->p,w->n); q->lo = w->beam_lo; q->hi = w->beam_hi;
    }
    w->column_query = false;
}
static __attribute__((noinline)) bool particle_hit(const l3_cmd *c, l3_work *w, float lo, float hi) {
    bool found = false;
    float a = l3_dot(w->dir,w->dir);
    for (int i = 0; i < c->np; i++) {
        l3_vec q = l3_sub(w->o,c->points[i]);
        float b = l3_dot(q,w->dir), d = b*b-a*(l3_dot(q,q)-c->r*c->r);
        if (d < 0) continue;
        float t = (-b-sqrtf(d))/a;
        if (t < lo) t = (-b+sqrtf(d))/a;
        if (t < lo || t > hi) continue;
        hi = t; w->hit = t; found = true;
        w->p = l3_add(w->o,l3_scale(w->dir,t)); w->n = l3_unit(l3_sub(w->p,c->points[i]));
    }
    return found;
}
bool l3_world_hit(const l3_cmd *c, l3_work *w, float min_t, float max_t) {
    w->sun = -1; w->material = c->mat;
    return c->kind == S_PARTICLES ? particle_hit(c,w,min_t,max_t) : grid_hit(c,w,min_t,max_t);
}
static __attribute__((noinline)) bool mesh_hit(const l3_cmd *c, l3_work *w, float hi) {
    bool found = false;
    for (int i = 0; i < c->mesh.ntri; i++) {
        int a = c->mesh.idx[i*3], b = c->mesh.idx[i*3+1], d = c->mesh.idx[i*3+2];
        if (a >= c->mesh.nv || b >= c->mesh.nv || d >= c->mesh.nv) continue;
        w->local = l3_transform(&c->t,c->mesh.v[a]);
        w->ld = l3_transform(&c->t,c->mesh.v[b]);
        w->lp = l3_transform(&c->t,c->mesh.v[d]);
        if (l3_ray_triangle(w->local,w->ld,w->lp,w,0,hi,(c->flags & L3_TWO_SIDED)!=0)) {
            hi = w->hit; found = true;
            /* Subsequent misses may use work vectors; retain the result. */
            w->ln = w->n;
        }
    }
    if (found) { w->hit = hi; w->p = l3_add(w->o,l3_scale(w->dir,hi)); w->n = w->ln; }
    return found;
}
bool l3_raycast(const l3_scene *scene, l3_vec origin, l3_vec dir, float max_t, l3_hit *out) {
    if (!scene || scene->recording || !scene->have_frame || !out || !(max_t >= 0) ||
        !isfinite(max_t) || l3_dot(dir,dir) < 1e-20f) return false;
    /* Query scratch is scene-owned, like preparation scratch. Calls on the
     * same scene are serialized with recording/rendering. No heap or stack pool. */
    l3_work *w = &((l3_scene *)scene)->query;
    memset(w,0,sizeof(*w)); w->o = origin; w->dir = dir;
#ifdef L3_PROFILE
    w->counts = &((l3_scene *)scene)->query_counts;
#endif
    bool found = false;
    for (int i = 0; i < scene->ncmd; i++) {
        const l3_cmd *c = &scene->cmd[i];
        if (c->kind >= S_LINE) continue;
        bool hit = c->kind == S_MESH ? mesh_hit(c,w,max_t) :
                   c->kind >= S_TERRAIN ? l3_world_hit(c,w,0,max_t) : l3_intersect(c,w,0,max_t);
        if (!hit) continue;
        max_t = w->hit; found = true;
        out->tag = c->tag; out->t = w->hit; out->p = w->p; out->n = w->n;
    }
    return found;
}
