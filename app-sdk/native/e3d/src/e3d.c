/*
 * e3d — matrices, meshes (loader, normals, winding repair), objects.
 * The renderer is in e3d_render.c.
 */
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "e3d_mem.h"

#define DEG2RAD 0.017453292519943295f

/* ── Matrices ────────────────────────────────────────────────────────────── */

void e3d_identity(e3d_mat *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.f;
}

void e3d_move(e3d_mat *m, float dx, float dy, float dz)
{
    e3d_identity(m);
    m->m[0][3] = dx;
    m->m[1][3] = dy;
    m->m[2][3] = dz;
}

void e3d_scale(e3d_mat *m, float sx, float sy, float sz)
{
    e3d_identity(m);
    m->m[0][0] = sx == 0.f ? 1.f : sx;
    m->m[1][1] = sy == 0.f ? 1.f : sy;
    m->m[2][2] = sz == 0.f ? 1.f : sz;
}

void e3d_mul(e3d_mat *a, const e3d_mat *b)
{
    e3d_mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] +
                        a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    *a = r;
}

/* The original's per-axis matrices, signs included, so angles turn the same way. */
void e3d_rotate(e3d_mat *m, float ax, float ay, float az)
{
    e3d_mat r;
    e3d_identity(m);
    if (az != 0.f) {
        float c = cosf(az * DEG2RAD), s = sinf(az * DEG2RAD);
        e3d_identity(&r);
        r.m[0][0] = c; r.m[0][1] = -s;
        r.m[1][0] = s; r.m[1][1] = c;
        e3d_mul(m, &r);
    }
    if (ay != 0.f) {
        float c = cosf(ay * DEG2RAD), s = sinf(ay * DEG2RAD);
        e3d_identity(&r);
        r.m[0][0] = c; r.m[0][2] = -s;
        r.m[2][0] = s; r.m[2][2] = c;
        e3d_mul(m, &r);
    }
    if (ax != 0.f) {
        float c = cosf(ax * DEG2RAD), s = sinf(ax * DEG2RAD);
        e3d_identity(&r);
        r.m[1][1] = c;  r.m[1][2] = s;
        r.m[2][1] = -s; r.m[2][2] = c;
        e3d_mul(m, &r);
    }
}

void e3d_apply(const e3d_mat *m, float v[3])
{
    float x = v[0], y = v[1], z = v[2];
    for (int i = 0; i < 3; i++)
        v[i] = m->m[i][0] * x + m->m[i][1] * y + m->m[i][2] * z + m->m[i][3];
}

/* Rodrigues; the sense matches the original rotateArbitrary for the y and x axes. */
void e3d_rotate_about(e3d_mat *m, const float b[3], float ax, float ay, float az, float deg)
{
    float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len < 1e-9f) {
        e3d_identity(m);
        return;
    }
    ax /= len; ay /= len; az /= len;
    float c = cosf(-deg * DEG2RAD), s = sinf(-deg * DEG2RAD), t = 1.f - c;
    e3d_mat r;
    e3d_identity(&r);
    r.m[0][0] = c + ax * ax * t;      r.m[0][1] = ax * ay * t - az * s; r.m[0][2] = ax * az * t + ay * s;
    r.m[1][0] = ay * ax * t + az * s; r.m[1][1] = c + ay * ay * t;      r.m[1][2] = ay * az * t - ax * s;
    r.m[2][0] = az * ax * t - ay * s; r.m[2][1] = az * ay * t + ax * s; r.m[2][2] = c + az * az * t;
    e3d_mat back;
    e3d_move(m, b[0], b[1], b[2]);
    e3d_mul(m, &r);
    e3d_move(&back, -b[0], -b[1], -b[2]);
    e3d_mul(m, &back);
}

void e3d_scale_about(e3d_mat *m, const float b[3], float sx, float sy, float sz)
{
    e3d_mat s, back;
    e3d_move(m, b[0], b[1], b[2]);
    e3d_scale(&s, sx, sy, sz);
    e3d_mul(m, &s);
    e3d_move(&back, -b[0], -b[1], -b[2]);
    e3d_mul(m, &back);
}

/* ── Meshes ──────────────────────────────────────────────────────────────── */

e3d_mesh *e3d_mesh_create(int nverts, int ntris)
{
    if (nverts <= 0 || nverts > 65535 || ntris <= 0 || ntris > 21845)
        return NULL;
    e3d_mesh *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->nverts = nverts;
    m->ntris = ntris;
    m->pos = calloc((size_t)nverts, sizeof(*m->pos));
    m->tris = calloc((size_t)ntris, sizeof(*m->tris));
    if (!m->pos || !m->tris) {
        e3d_mesh_free(m);
        return NULL;
    }
    return m;
}

static void mesh_release(e3d_mesh *m)
{
    free(m->pos);
    free(m->tris);
    free(m->nrm);
    free(m->nrm_vert);
    free(m->qn);
    m->pos = NULL;
    m->tris = NULL;
    m->nrm = NULL;
    m->nrm_vert = NULL;
    m->qn = NULL;
}

void e3d_mesh_free(e3d_mesh *m)
{
    if (m) {
        mesh_release(m);
        free(m);
    }
}

void e3d_meshes_free(e3d_mesh *m, int count)
{
    if (!m)
        return;
    for (int i = 0; i < count; i++)
        mesh_release(&m[i]);
    free(m);
}

/* The original's normal: (v0 - v1) x (v2 - v1), not normalised (twice the area). */
static void face_normal(const e3d_mesh *m, const e3d_tri *t, float n[3])
{
    const float *p0 = m->pos[t->v[0]], *p1 = m->pos[t->v[1]], *p2 = m->pos[t->v[2]];
    float a[3] = { p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2] };
    float b[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
    n[0] = a[1] * b[2] - a[2] * b[1];
    n[1] = a[2] * b[0] - a[0] * b[2];
    n[2] = a[0] * b[1] - a[1] * b[0];
}

typedef struct { uint32_t key; int tri; } half_edge;
typedef struct { int tri; uint16_t a, b; } neighbour;   /* across the edge {a, b} */

static int cmp_half_edge(const void *pa, const void *pb)
{
    const half_edge *a = pa, *b = pb;
    return a->key < b->key ? -1 : a->key > b->key ? 1 : a->tri - b->tri;
}

/* +1 if t runs a->b, -1 if b->a, 0 if the edge is not in t */
static int edge_dir(const e3d_tri *t, uint16_t a, uint16_t b)
{
    for (int k = 0; k < 3; k++) {
        uint16_t p = t->v[k], q = t->v[(k + 1) % 3];
        if (p == a && q == b) return 1;
        if (p == b && q == a) return -1;
    }
    return 0;
}

static void flip_tri(e3d_tri *t)
{
    uint16_t tmp = t->v[1];
    t->v[1] = t->v[2];
    t->v[2] = tmp;
}

/* Hand-made meshes mix windings. Make neighbours agree across every edge shared by
 * exactly two triangles, then turn each connected piece outward: by its signed
 * volume when it is closed, else the way most of its triangles were wound.
 * Edges with one or more than two triangles mark the mesh open: drawn two-sided. */
static void fix_winding(e3d_mesh *m)
{
    int n = m->ntris, nh = n * 3;
    half_edge *he = malloc(sizeof(*he) * (size_t)nh);
    neighbour *nb = malloc(sizeof(*nb) * (size_t)nh);   /* up to 3 per triangle */
    uint8_t *nn = calloc((size_t)n, 1);
    int *queue = malloc(sizeof(int) * (size_t)n);
    uint8_t *state = calloc((size_t)n, 1);              /* bit0 seen, bit1 flipped */
    if (!he || !nb || !nn || !queue || !state) {
        free(he); free(nb); free(nn); free(queue); free(state);
        return;
    }
    for (int t = 0; t < n; t++)
        for (int k = 0; k < 3; k++) {
            uint16_t a = m->tris[t].v[k], b = m->tris[t].v[(k + 1) % 3];
            uint16_t lo = a < b ? a : b, hi = a < b ? b : a;
            he[t * 3 + k] = (half_edge){ ((uint32_t)lo << 16) | hi, t };
        }
    qsort(he, (size_t)nh, sizeof(*he), cmp_half_edge);
    bool open = false;
    for (int i = 0; i < nh;) {
        int j = i;
        while (j < nh && he[j].key == he[i].key)
            j++;
        int t0 = he[i].tri, t1 = j - i == 2 ? he[i + 1].tri : -1;
        if (t1 >= 0 && t0 != t1 && nn[t0] < 3 && nn[t1] < 3) {
            uint16_t a = (uint16_t)(he[i].key >> 16), b = (uint16_t)(he[i].key & 0xFFFF);
            nb[t0 * 3 + nn[t0]++] = (neighbour){ t1, a, b };
            nb[t1 * 3 + nn[t1]++] = (neighbour){ t0, a, b };
        } else {
            open = true;
        }
        i = j;
    }
    m->open = open;

    for (int s = 0; s < n; s++) {
        if (state[s] & 1)
            continue;
        int head = 0, tail = 0, flipped = 0;
        bool closed = true;
        queue[tail++] = s;
        state[s] = 1;
        while (head < tail) {
            int t = queue[head++];
            if (nn[t] < 3)
                closed = false;
            for (int k = 0; k < nn[t]; k++) {
                const neighbour *e = &nb[t * 3 + k];
                if (state[e->tri] & 1)
                    continue;
                /* neighbours run their shared edge in opposite directions */
                if (edge_dir(&m->tris[t], e->a, e->b) == edge_dir(&m->tris[e->tri], e->a, e->b)) {
                    flip_tri(&m->tris[e->tri]);
                    state[e->tri] |= 2;
                    flipped++;
                }
                state[e->tri] |= 1;
                queue[tail++] = e->tri;
            }
        }
        bool turn;
        if (closed) {
            /* with n = (v0-v1)x(v2-v1) outward, a closed piece has negative volume */
            double vol = 0;
            for (int i = 0; i < tail; i++) {
                const e3d_tri *t = &m->tris[queue[i]];
                const float *a = m->pos[t->v[0]], *b = m->pos[t->v[1]], *c = m->pos[t->v[2]];
                vol += (double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) -
                       (double)a[1] * ((double)b[0] * c[2] - (double)b[2] * c[0]) +
                       (double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0]);
            }
            turn = vol > 0;
        } else {
            turn = flipped * 2 > tail;   /* keep what most of the data said */
        }
        if (turn)
            for (int i = 0; i < tail; i++)
                flip_tri(&m->tris[queue[i]]);
    }
    free(he); free(nb); free(nn); free(queue); free(state);
}

void e3d_mesh_finish(e3d_mesh *m, float crease_deg)
{
    int nv = m->nverts, nt = m->ntris;

    float c[3] = { 0, 0, 0 };
    for (int i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++)
            c[k] += m->pos[i][k];
    for (int k = 0; k < 3; k++)
        m->center[k] = c[k] / (float)nv;
    float r2 = 0;
    for (int i = 0; i < nv; i++) {
        float dx = m->pos[i][0] - m->center[0], dy = m->pos[i][1] - m->center[1],
              dz = m->pos[i][2] - m->center[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d > r2)
            r2 = d;
    }
    m->radius = sqrtf(r2);

    fix_winding(m);

    /* vertex -> triangles (CSR) */
    float (*fn)[3] = malloc(sizeof(*fn) * (size_t)nt);
    int *start = calloc((size_t)nv + 1, sizeof(int));
    int *list = malloc(sizeof(int) * (size_t)nt * 3);
    if (!fn || !start || !list) {
        free(fn); free(start); free(list);
        return;
    }
    for (int t = 0; t < nt; t++) {
        face_normal(m, &m->tris[t], fn[t]);
        for (int k = 0; k < 3; k++)
            start[m->tris[t].v[k] + 1]++;
    }
    for (int i = 0; i < nv; i++)
        start[i + 1] += start[i];
    int *fill = malloc(sizeof(int) * (size_t)nv);
    if (!fill) {
        free(fn); free(start); free(list);
        return;
    }
    memcpy(fill, start, sizeof(int) * (size_t)nv);
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++)
            list[fill[m->tris[t].v[k]]++] = t;
    free(fill);

    /* corner normal: area-weighted sum of the faces around the vertex that are
       within the crease angle of this face (the original used 45 degrees);
       corners of one vertex with the same normal share an entry */
    float cos_crease = cosf(crease_deg * DEG2RAD);
    free(m->nrm);
    free(m->nrm_vert);
    m->nnorm = 0;
    m->nrm = malloc(sizeof(*m->nrm) * (size_t)nt * 3);
    m->nrm_vert = malloc(sizeof(uint16_t) * (size_t)nt * 3);
    int *head = malloc(sizeof(int) * (size_t)nv), *next = malloc(sizeof(int) * (size_t)nt * 3);
    if (!m->nrm || !m->nrm_vert || !head || !next) {
        free(head); free(next); free(fn); free(start); free(list);
        return;
    }
    for (int i = 0; i < nv; i++)
        head[i] = -1;
    for (int t = 0; t < nt; t++) {
        const float *nt0 = fn[t];
        float lt = sqrtf(nt0[0] * nt0[0] + nt0[1] * nt0[1] + nt0[2] * nt0[2]);
        for (int k = 0; k < 3; k++) {
            int v = m->tris[t].v[k];
            float s[3] = { 0, 0, 0 };
            for (int j = start[v]; j < start[v + 1]; j++) {
                const float *nu = fn[list[j]];
                float lu = sqrtf(nu[0] * nu[0] + nu[1] * nu[1] + nu[2] * nu[2]);
                if (lt <= 0.f || lu <= 0.f)
                    continue;
                float d = (nt0[0] * nu[0] + nt0[1] * nu[1] + nt0[2] * nu[2]) / (lt * lu);
                if (list[j] == t || d >= cos_crease) {
                    s[0] += nu[0]; s[1] += nu[1]; s[2] += nu[2];
                }
            }
            float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            if (l > 0.f) {
                s[0] /= l; s[1] /= l; s[2] /= l;
            } else {
                s[0] = 0; s[1] = 1; s[2] = 0;
            }
            int u = head[v];
            while (u >= 0 && m->nrm[u][0] * s[0] + m->nrm[u][1] * s[1] + m->nrm[u][2] * s[2] < 0.99999f)
                u = next[u];
            if (u < 0) {
                u = m->nnorm++;
                memcpy(m->nrm[u], s, sizeof(s));
                m->nrm_vert[u] = (uint16_t)v;
                next[u] = head[v];
                head[v] = u;
            }
            m->tris[t].n[k] = (uint16_t)u;
        }
    }
    free(head); free(next); free(fn); free(start); free(list);

    /* Normals in vertex order (a counting sort), so the renderer's lighting
     * walks them with their vertices and never has to gather */
    int nn = m->nnorm;
    int *cnt = calloc((size_t)nv + 1, sizeof(int)), *to = malloc(sizeof(int) * (size_t)(nn ? nn : 1));
    float (*nrm2)[3] = malloc(sizeof(*nrm2) * (size_t)(nn ? nn : 1));
    uint16_t *nv2 = malloc(sizeof(uint16_t) * (size_t)(nn ? nn : 1));
    if (cnt && to && nrm2 && nv2) {
        for (int u = 0; u < nn; u++)
            cnt[m->nrm_vert[u] + 1]++;
        for (int v = 0; v < nv; v++)
            cnt[v + 1] += cnt[v];
        for (int u = 0; u < nn; u++) {
            int d = cnt[m->nrm_vert[u]]++;
            to[u] = d;
            memcpy(nrm2[d], m->nrm[u], sizeof(nrm2[d]));
            nv2[d] = m->nrm_vert[u];
        }
        for (int t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++)
                m->tris[t].n[k] = (uint16_t)to[m->tris[t].n[k]];
        free(m->nrm);
        free(m->nrm_vert);
        m->nrm = nrm2;
        m->nrm_vert = nv2;
        nrm2 = NULL;
        nv2 = NULL;
    }
    free(cnt); free(to); free(nrm2); free(nv2);

    /* the renderer's batched lighting reads 8 normals at a time */
    int np = (m->nnorm + 7) & ~7;
    free(m->qn);
    m->qn = calloc((size_t)np * 3, sizeof(int16_t));
    if (m->qn)
        for (int u = 0; u < m->nnorm; u++)
            for (int k = 0; k < 3; k++)
                m->qn[k * np + u] = (int16_t)lrintf(m->nrm[u][k] * 16384.f);
}

/* ── Loader ──────────────────────────────────────────────────────────────── */

static bool next_long(const char **p, long *out)
{
    char *end;
    long v = strtol(*p, &end, 10);
    if (end == *p)
        return false;
    *p = end;
    *out = v;
    return true;
}

static bool next_float(const char **p, float *out)
{
    char *end;
    float v = strtof(*p, &end);
    if (end == *p)
        return false;
    *p = end;
    *out = v;
    return true;
}

static int clamp255(long v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }

int e3d_load_text(const char *text, e3d_mesh **out)
{
    const char *p = text;
    long count;
    *out = NULL;
    if (!next_long(&p, &count) || count <= 0 || count > 10000)
        return 0;
    e3d_mesh *arr = calloc((size_t)count, sizeof(*arr));
    if (!arr)
        return 0;
    for (long o = 0; o < count; o++) {
        long nt, nv;
        if (!next_long(&p, &nt) || nt <= 0 || nt > 1000000)
            goto fail;
        e3d_tri *tris = calloc((size_t)nt, sizeof(*tris));
        if (!tris)
            goto fail;
        arr[o].tris = tris;
        arr[o].ntris = (int)nt;
        for (long t = 0; t < nt; t++) {
            long f[6];
            for (int k = 0; k < 6; k++)
                if (!next_long(&p, &f[k]))
                    goto fail;
            for (int k = 0; k < 3; k++)
                tris[t].v[k] = (uint16_t)(f[k] < 0 ? 0 : f[k]);
            tris[t].r = (uint8_t)clamp255(f[3]);
            tris[t].g = (uint8_t)clamp255(f[4]);
            tris[t].b = (uint8_t)clamp255(f[5]);
        }
        if (!next_long(&p, &nv) || nv <= 0 || nv > 65535)
            goto fail;
        arr[o].pos = calloc((size_t)nv, sizeof(*arr[o].pos));
        if (!arr[o].pos)
            goto fail;
        arr[o].nverts = (int)nv;
        for (long v = 0; v < nv; v++)
            for (int k = 0; k < 3; k++)
                if (!next_float(&p, &arr[o].pos[v][k]))
                    goto fail;
        for (long t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++)
                if (tris[t].v[k] >= nv)
                    goto fail;
        e3d_mesh_finish(&arr[o], 45.f);
    }
    *out = arr;
    return (int)count;
fail:
    e3d_meshes_free(arr, (int)count);
    return 0;
}

int e3d_load_file(const char *path, e3d_mesh **out)
{
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = size > 0 ? malloc((size_t)size + 1) : NULL;
    int n = 0;
    if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
        buf[size] = 0;
        n = e3d_load_text(buf, out);
    }
    free(buf);
    fclose(f);
    return n;
}

/* ── Generated meshes (the original built these with 3dball / 3dfloor) ───── */

e3d_mesh *e3d_mesh_sphere(float radius, int slices, int stacks, int r, int g, int b)
{
    if (slices < 3) slices = 3;
    if (stacks < 2) stacks = 2;
    int nv = 2 + slices * (stacks - 1);
    int nt = slices * 2 + slices * (stacks - 2) * 2;
    e3d_mesh *m = e3d_mesh_create(nv, nt);
    if (!m)
        return NULL;
    m->pos[0][1] = radius;
    m->pos[1][1] = -radius;
    for (int s = 1; s < stacks; s++) {
        float phi = 3.14159265f * (float)s / (float)stacks;
        for (int i = 0; i < slices; i++) {
            float th = 6.2831853f * (float)i / (float)slices;
            float *p = m->pos[2 + (s - 1) * slices + i];
            p[0] = radius * sinf(phi) * cosf(th);
            p[1] = radius * cosf(phi);
            p[2] = radius * sinf(phi) * sinf(th);
        }
    }
    int t = 0;
#define RING(s, i) (2 + ((s) - 1) * slices + ((i) % slices))
    for (int i = 0; i < slices; i++) {
        m->tris[t++] = (e3d_tri){ .v = { 0, RING(1, i), RING(1, i + 1) } };
        m->tris[t++] = (e3d_tri){ .v = { 1, RING(stacks - 1, i + 1), RING(stacks - 1, i) } };
    }
    for (int s = 1; s < stacks - 1; s++)
        for (int i = 0; i < slices; i++) {
            m->tris[t++] = (e3d_tri){ .v = { RING(s, i), RING(s + 1, i), RING(s + 1, i + 1) } };
            m->tris[t++] = (e3d_tri){ .v = { RING(s, i), RING(s + 1, i + 1), RING(s, i + 1) } };
        }
#undef RING
    for (int i = 0; i < nt; i++) {
        m->tris[i].r = (uint8_t)r;
        m->tris[i].g = (uint8_t)g;
        m->tris[i].b = (uint8_t)b;
    }
    e3d_mesh_finish(m, 80.f);   /* winding repair turns it outward */
    return m;
}

e3d_mesh *e3d_mesh_floor(float x0, float z0, float x1, float z1, float y, int n,
                         uint32_t rgb_a, uint32_t rgb_b)
{
    if (n < 1)
        n = 1;
    if ((n + 1) * (n + 1) > 65535)
        return NULL;
    e3d_mesh *m = e3d_mesh_create((n + 1) * (n + 1), n * n * 2);
    if (!m)
        return NULL;
    for (int j = 0; j <= n; j++)
        for (int i = 0; i <= n; i++) {
            float *p = m->pos[j * (n + 1) + i];
            p[0] = x0 + (x1 - x0) * (float)i / (float)n;
            p[1] = y;
            p[2] = z0 + (z1 - z0) * (float)j / (float)n;
        }
    int t = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            uint16_t p00 = (uint16_t)(j * (n + 1) + i), p10 = (uint16_t)(p00 + 1);
            uint16_t p01 = (uint16_t)(p00 + n + 1), p11 = (uint16_t)(p01 + 1);
            uint32_t c = ((i + j) & 1) ? rgb_b : rgb_a;
            e3d_tri a = { .v = { p01, p00, p10 }, .r = c >> 16, .g = (c >> 8) & 255, .b = c & 255 };
            e3d_tri b = { .v = { p10, p11, p01 }, .r = c >> 16, .g = (c >> 8) & 255, .b = c & 255 };
            m->tris[t++] = a;
            m->tris[t++] = b;
        }
    e3d_mesh_finish(m, 10.f);  /* n = (v0-v1)x(v2-v1) points up */
    return m;
}

/* ── Objects ─────────────────────────────────────────────────────────────── */

void e3d_object_init(e3d_object *o, const e3d_mesh *mesh)
{
    memset(o, 0, sizeof(*o));
    o->mesh = mesh;
    e3d_identity(&o->m);
    o->color = -1;
}

void e3d_transform(e3d_object *o, const e3d_mat *mat)
{
    e3d_mat r = *mat;
    e3d_mul(&r, &o->m);
    o->m = r;
}

void e3d_object_pos(const e3d_object *o, float out[3])
{
    memcpy(out, o->mesh->center, sizeof(float) * 3);
    e3d_apply(&o->m, out);
}

void e3d_object_color(e3d_object *o, int r, int g, int b)
{
    o->color = (clamp255(r) << 16) | (clamp255(g) << 8) | clamp255(b);
}
