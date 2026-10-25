/* Application-owned PSRAM mesh generators; the font matches lc_glyph. */
#include "l3_private.h"
static __attribute__((noinline)) bool mesh_alloc(l3_mesh *m, int nv, int nt) {
    if (!m || nv < 1 || nv > 65535 || nt < 1)
        return false;
    memset(m, 0, sizeof(*m));
    l3_vec *v = l3_alloc((size_t)nv * sizeof(*v));
    uint16_t *i = l3_alloc((size_t)nt * 3 * sizeof(*i));
    if (!v || !i) {
        free(v);
        free(i);
        return false;
    }
    m->v = v;
    m->idx = i;
    m->nv = nv;
    m->ntri = nt;
    return true;
}
void l3_mesh_free(l3_mesh *m) {
    if (m) {
        free((void *)m->v);
        free((void *)m->idx);
        free((void *)m->mats);
        memset(m, 0, sizeof(*m));
    }
}
static __attribute__((noinline)) void face(l3_mesh *m, int *at, int a, int b, int c) {
    uint16_t *i = (uint16_t *)m->idx;
    i[(*at)++] = (uint16_t)a;
    i[(*at)++] = (uint16_t)b;
    i[(*at)++] = (uint16_t)c;
}
bool l3_mesh_lathe(l3_mesh *out, const float *rz, int n, int seg) {
    if (!rz || n < 2 || seg < 3 || n > 65535 / seg || !mesh_alloc(out, n * seg, (n - 1) * seg * 2))
        return false;
    l3_vec *v = (l3_vec *)out->v;
    for (int j = 0; j < n; j++)
        for (int k = 0; k < seg; k++) {
            float a = 2 * L3_PI * k / seg;
            v[j * seg + k] = l3v(rz[j * 2] * cosf(a), rz[j * 2 + 1], rz[j * 2] * sinf(a));
        }
    int at = 0;
    for (int j = 0; j < n - 1; j++)
        for (int k = 0; k < seg; k++) {
            int a = j * seg + k, b = j * seg + (k + 1) % seg;
            face(out, &at, a, a + seg, b);
            face(out, &at, b, a + seg, b + seg);
        }
    return true;
}
bool l3_mesh_heightmap(l3_mesh *out, const float *h, int w, int d, float cell) {
    if (!h || w < 2 || d < 2 || w > 65535 / d || cell <= 0 ||
        !mesh_alloc(out, w * d, (w - 1) * (d - 1) * 2))
        return false;
    l3_vec *v = (l3_vec *)out->v;
    for (int z = 0; z < d; z++)
        for (int x = 0; x < w; x++)
            v[z * w + x] =
                l3v((x - (w - 1) * 0.5f) * cell, h[z * w + x], (z - (d - 1) * 0.5f) * cell);
    int at = 0;
    for (int z = 0; z < d - 1; z++)
        for (int x = 0; x < w - 1; x++) {
            int a = z * w + x;
            face(out, &at, a, a + w, a + 1);
            face(out, &at, a + 1, a + w, a + w + 1);
        }
    return true;
}
static __attribute__((noinline)) float cross2(const float *xy, int a, int b, int c) {
    return (xy[b * 2] - xy[a * 2]) * (xy[c * 2 + 1] - xy[a * 2 + 1]) -
           (xy[b * 2 + 1] - xy[a * 2 + 1]) * (xy[c * 2] - xy[a * 2]);
}
bool l3_mesh_extrude(l3_mesh *out, const float *xy, int n, float depth) {
    if (!xy || n < 3 || n > 32767 || depth <= 0)
        return false;
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
    }
    if (fabsf(area) < 1.0e-10f)
        return false;
    if (!mesh_alloc(out, n * 2, (n - 2) * 2 + n * 2))
        return false;
    uint16_t *ring = l3_alloc((size_t)n * sizeof(*ring));
    if (!ring) {
        l3_mesh_free(out);
        return false;
    }
    l3_vec *v = (l3_vec *)out->v;
    for (int j = 0; j < n; j++) {
        ring[j] = (uint16_t)(area > 0 ? j : n - 1 - j);
        v[j] = l3v(xy[j * 2], xy[j * 2 + 1], depth * 0.5f);
        v[j + n] = l3v(xy[j * 2], xy[j * 2 + 1], -depth * 0.5f);
    }
    int at = 0;
    for (int j = 0; j < n; j++) {
        int a = ring[j], b = ring[(j + 1) % n];
        face(out, &at, a, b + n, b);
        face(out, &at, a, a + n, b + n);
    }
    int left = n;
    while (left > 2) {
        bool found = false;
        for (int j = 0; j < left; j++) {
            int a = ring[(j + left - 1) % left], b = ring[j], c = ring[(j + 1) % left];
            if (cross2(xy, a, b, c) <= 0)
                continue;
            bool inside = false;
            for (int k = 0; k < left; k++) {
                int p = ring[k];
                if (p == a || p == b || p == c)
                    continue;
                if (cross2(xy, a, b, p) >= 0 && cross2(xy, b, c, p) >= 0 &&
                    cross2(xy, c, a, p) >= 0) {
                    inside = true;
                    break;
                }
            }
            if (inside)
                continue;
            face(out, &at, a, b, c);
            face(out, &at, a + n, c + n, b + n);
            memmove(ring + j, ring + j + 1, (size_t)(left - j - 1) * sizeof(*ring));
            left--;
            found = true;
            break;
        }
        if (!found) {
            free(ring);
            l3_mesh_free(out);
            return false;
        }
    }
    free(ring);
    return true;
}
static const uint16_t ico_faces[60] = {0, 11, 5, 0, 5,  1,  0,  1,  7,  0,  7, 10, 0, 10, 11,
                                       1, 5,  9, 5, 11, 4,  11, 10, 2,  10, 7, 6,  7, 1,  8,
                                       3, 9,  4, 3, 4,  2,  3,  2,  6,  3,  6, 8,  3, 8,  9,
                                       4, 9,  5, 2, 4,  11, 6,  2,  10, 8,  6, 7,  9, 8,  1};
static __attribute__((noinline)) int midpoint(l3_mesh *m, int *nv, uint32_t *keys, uint16_t *values,
                                              unsigned mask, int a, int b) {
    unsigned lo = (unsigned)(a < b ? a : b), hi = (unsigned)(a < b ? b : a);
    uint32_t key = (lo << 16) | hi;
    unsigned slot = (key * 2654435761u) & mask;
    while (keys[slot] != UINT32_MAX && keys[slot] != key)
        slot = (slot + 1) & mask;
    if (keys[slot] == key)
        return values[slot];
    int id = (*nv)++;
    ((l3_vec *)m->v)[id] = l3_norm(l3_add(m->v[a], m->v[b]));
    keys[slot] = key;
    values[slot] = (uint16_t)id;
    return id;
}
static __attribute__((noinline)) void ico_vertices(l3_mesh *out) {
    l3_vec *v = (l3_vec *)out->v;
    const float t = 1.61803398875f;
    for (int i = 0; i < 12; i++) {
        if (i < 4)
            v[i] = l3v(i & 1 ? 1 : -1, i & 2 ? -t : t, 0);
        else if (i < 8)
            v[i] = l3v(0, i & 1 ? 1 : -1, i & 2 ? -t : t);
        else
            v[i] = l3v(i & 2 ? -t : t, 0, i & 1 ? 1 : -1);
        v[i] = l3_norm(v[i]);
    }
}
bool l3_mesh_icosphere(l3_mesh *out, int sub) {
    if (sub < 0 || sub > 6)
        return false;
    int factor = 1 << (sub * 2), nt = 20, nv = 12;
    if (!mesh_alloc(out, 10 * factor + 2, 20 * factor))
        return false;
    ico_vertices(out);
    memcpy((void *)out->idx, ico_faces, sizeof ico_faces);
    uint16_t *temp = NULL, *values = NULL;
    uint32_t *keys = NULL;
    unsigned cap = 1;
    while (cap < (unsigned)(30 * factor * 2))
        cap *= 2;
    if (sub) {
        temp = l3_alloc((size_t)20 * factor * 3 * sizeof(*temp));
        keys = l3_alloc((size_t)cap * sizeof(*keys));
        values = l3_alloc((size_t)cap * sizeof(*values));
        if (!temp || !keys || !values) {
            free(temp);
            free(keys);
            free(values);
            l3_mesh_free(out);
            return false;
        }
    }
    for (int level = 0; level < sub; level++) {
        memset(keys, 255, (size_t)cap * sizeof(*keys));
        int at = 0;
        for (int j = 0; j < nt; j++) {
            int a = out->idx[j * 3], b = out->idx[j * 3 + 1], c = out->idx[j * 3 + 2],
                ab = midpoint(out, &nv, keys, values, cap - 1, a, b),
                bc = midpoint(out, &nv, keys, values, cap - 1, b, c),
                ca = midpoint(out, &nv, keys, values, cap - 1, c, a);
            temp[at++] = (uint16_t)a;
            temp[at++] = (uint16_t)ab;
            temp[at++] = (uint16_t)ca;
            temp[at++] = (uint16_t)b;
            temp[at++] = (uint16_t)bc;
            temp[at++] = (uint16_t)ab;
            temp[at++] = (uint16_t)c;
            temp[at++] = (uint16_t)ca;
            temp[at++] = (uint16_t)bc;
            temp[at++] = (uint16_t)ab;
            temp[at++] = (uint16_t)bc;
            temp[at++] = (uint16_t)ca;
        }
        nt *= 4;
        memcpy((void *)out->idx, temp, (size_t)nt * 3 * sizeof(*temp));
    }
    out->nv = nv;
    free(temp);
    free(keys);
    free(values);
    return true;
}
uint16_t l3_glyph(char ch) {
    static const uint16_t D[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9,
                                   0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};
    static const uint16_t A[26] = {0x2BED, 0x6BAE, 0x7927, 0x6B6E, 0x79E7, 0x79E4, 0x796F,
                                   0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED, 0x6B6D,
                                   0x7B6F, 0x6BA4, 0x2B7B, 0x6BAD, 0x388E, 0x7492, 0x5B6F,
                                   0x5B6A, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7};
    if (ch >= '0' && ch <= '9')
        return D[ch - '0'];
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 32);
    if (ch >= 'A' && ch <= 'Z')
        return A[ch - 'A'];
    switch (ch) {
    case '+':
        return 0x05D0;
    case '-':
        return 0x01C0;
    case '!':
        return 0x2482;
    case '?':
        return 0x6282;
    case '.':
        return 2;
    case ':':
        return 0x0410;
    case '/':
        return 0x12A4;
    case '%':
        return 0x52A5;
    case '<':
        return 0x1511;
    case '>':
        return 0x4454;
    case '\'':
        return 0x2400;
    case '#':
        return 0x5F7D;
    case '*':
        return 0x0AA8;
    case '=':
        return 0x0E38;
    case '_':
        return 7;
    case ',':
        return 0x0014;
    default:
        return 0;
    }
}
static const uint16_t cube_idx[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                      3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
bool l3_mesh_text(l3_mesh *out, const char *text, float depth) {
    if (!text || depth <= 0 || strlen(text) > 512)
        return false;
    int count = 0, len = (int)strlen(text);
    for (int i = 0; i < len; i++) {
        uint16_t g = l3_glyph(text[i]);
        for (int b = 0; b < 15; b++)
            count += (g >> b) & 1;
    }
    if (!count || count > 8191 || !mesh_alloc(out, count * 8, count * 12))
        return false;
    int at = 0, base = 0;
    l3_vec *v = (l3_vec *)out->v;
    for (int i = 0; i < len; i++)
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 3; x++)
                if ((l3_glyph(text[i]) >> (14 - y * 3 - x)) & 1) {
                    float px = i * 4 + x - (len * 4 - 1) * 0.5f, py = 2.5f - y;
                    for (int k = 0; k < 8; k++) {
                        int corner = k & 3;
                        v[base + k] =
                            l3v(px + (corner == 1 || corner == 2 ? 1 : 0),
                                py + (corner >= 2 ? -1 : 0), k < 4 ? -depth * 0.5f : depth * 0.5f);
                    }
                    for (int k = 0; k < 36; k++)
                        ((uint16_t *)out->idx)[at++] =
                            (uint16_t)(base + cube_idx[k % 3 == 1   ? k + 1
                                                       : k % 3 == 2 ? k - 1
                                                                    : k]);
                    base += 8;
                }
    return true;
}
/* Buildings: footprints extruded from base[i] to top[i], walls and a flat
   roof (ear clipped), the ring turned whichever way it was given so every
   face looks outward. xz holds x east, z south pairs; counts[i] points each. */
static __attribute__((noinline)) float ring_cross(const float *xz, const uint16_t *ring, int a, int b, int c) {
    const float *p = xz + ring[a] * 2, *q = xz + ring[b] * 2, *r = xz + ring[c] * 2;
    return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]);
}
bool l3_mesh_prisms(l3_mesh *out, const float *xz, const uint16_t *counts, int n, const float *base,
                    const float *top, int wall_mat, int roof_mat) {
    if (!out || !xz || !counts || !base || !top || n < 1)
        return false;
    int nv = 0, nt = 0, most = 0;
    for (int i = 0; i < n; i++) {
        if (counts[i] < 3)
            continue;
        nv += counts[i] * 2;
        nt += counts[i] * 2 + counts[i] - 2;
        if (counts[i] > most)
            most = counts[i];
    }
    if (!nv || nv > 65535 || !mesh_alloc(out, nv, nt))
        return false;
    uint8_t *mats = l3_alloc((size_t)nt);
    uint16_t *ring = l3_alloc((size_t)most * sizeof(*ring));
    if (!mats || !ring) {
        free(mats);
        free(ring);
        l3_mesh_free(out);
        return false;
    }
    out->mats = mats;
    l3_vec *v = (l3_vec *)out->v;
    int at = 0, vbase = 0, tri = 0;
    const float *p = xz;
    for (int i = 0; i < n; p += counts[i] * 2, i++) {
        const int c = counts[i];
        if (c < 3)
            continue;
        float area = 0;
        for (int j = 0; j < c; j++) {
            const int k = (j + 1) % c;
            area += p[j * 2] * p[k * 2 + 1] - p[k * 2] * p[j * 2 + 1];
        }
        /* With x east and z south, a negative area is the order whose roof
           faces up and whose walls face out (see the cross products below). */
        for (int j = 0; j < c; j++) {
            const int s = area < 0 ? j : c - 1 - j;
            v[vbase + j] = l3v(p[s * 2], base[i], p[s * 2 + 1]);
            v[vbase + c + j] = l3v(p[s * 2], top[i], p[s * 2 + 1]);
        }
        for (int j = 0; j < c; j++) {
            const int a = vbase + j, b = vbase + (j + 1) % c;
            face(out, &at, a, b, b + c);
            face(out, &at, a, b + c, a + c);
            mats[tri++] = (uint8_t)wall_mat;
            mats[tri++] = (uint8_t)wall_mat;
        }
        /* The roof: clip ears off the top ring. */
        for (int j = 0; j < c; j++)
            ring[j] = (uint16_t)(area < 0 ? j : c - 1 - j);
        int left = c, guard = c * c;
        while (left > 2 && guard-- > 0) {
            bool cut = false;
            for (int j = 0; j < left && !cut; j++) {
                const int a = (j + left - 1) % left, b = j, d = (j + 1) % left;
                if (ring_cross(p, ring, a, b, d) >= 0)      /* not convex for this order */
                    continue;
                bool inside = false;
                for (int k = 0; k < left && !inside; k++) {
                    if (k == a || k == b || k == d)
                        continue;
                    inside = ring_cross(p, ring, a, b, k) < 0 && ring_cross(p, ring, b, d, k) < 0 &&
                             ring_cross(p, ring, d, a, k) < 0;
                }
                if (inside)
                    continue;
                /* ring holds source indices; the top vertex of source s is at
                   the position its order put it. */
                const int sa = ring[a], sb = ring[b], sd = ring[d];
                const int va = vbase + c + (area < 0 ? sa : c - 1 - sa);
                const int vb = vbase + c + (area < 0 ? sb : c - 1 - sb);
                const int vd = vbase + c + (area < 0 ? sd : c - 1 - sd);
                face(out, &at, va, vb, vd);
                mats[tri++] = (uint8_t)roof_mat;
                memmove(ring + b, ring + b + 1, (size_t)(left - b - 1) * sizeof(*ring));
                left--;
                cut = true;
            }
            if (!cut)
                break;
        }
        vbase += c * 2;
    }
    free(ring);
    out->ntri = tri;
    return tri > 0;
}
