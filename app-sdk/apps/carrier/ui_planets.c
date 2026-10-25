/* Worlds, shaded per dot: the scene draws each planet as a plain sphere (for
 * depth and what hides what), then every dot it covers is repainted here from
 * a surface map baked once per system - oceans with a sun glint, land, ice
 * caps, cloud, city lights on the night side, lava that glows, gas giants in
 * bands - lit from the star, darker at the limb, a rim of air on living worlds. */
#include "internal.h"
#include "art.h"
#include "ui.h"

#define PC 24                               /* surface map: six faces of PC x PC */
enum { C_DEEP, C_SEA, C_LAND, C_HILL, C_ICE, C_ROCK, C_LAVA, C_RUST, C_GA, C_GB, C_GC, C_GD, C_N };
enum { F_CITY = 0x40, F_CLOUD = 0x80 };
static uint8_t surf[5][6 * PC * PC];
static int kinds[5];
#define BLOCKS 160              /* a 2x2 block per two dot columns, up to a 320-dot view */
typedef struct { float view, diff, spec; uint8_t texel; bool cloud; int row; } block;
static block blk[BLOCKS];
static const uint8_t ramp[C_N][4] = {
    {LAP_BLACK, LAP_BLUE, LAP_BLUE, 12},    /* deep */
    {LAP_BLACK, LAP_BLUE, 12, 14},          /* sea */
    {LAP_BLACK, LAP_GREEN, LAP_GREEN, 10},  /* land */
    {LAP_BLACK, LAP_YELLOW, LAP_YELLOW, 11},/* hills, sand */
    {8, LAP_WHITE, 15, 15},                 /* ice */
    {LAP_BLACK, 8, 8, LAP_WHITE},           /* rock */
    {LAP_BLACK, LAP_RED, 9, 11},            /* lava fields */
    {LAP_BLACK, LAP_RED, LAP_RED, 9},       /* rust */
    {LAP_BLACK, LAP_YELLOW, 11, 15},        /* gas bands */
    {LAP_BLACK, LAP_RED, 9, 11},
    {LAP_BLACK, 8, LAP_WHITE, 15},
    {LAP_BLACK, LAP_MAGENTA, 13, 15},
};

static float lattice(uint32_t seed, int x, int y, int z) {
    uint32_t h = seed ^ (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    return (cg_hash(h) & 1023) * (1 / 1023.f);
}
static float noise3(uint32_t seed, float x, float y, float z) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float fx = x - xi, fy = y - yi, fz = z - zi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
    float c[2][2][2];
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            for (int d = 0; d < 2; d++) c[a][b][d] = lattice(seed, xi + a, yi + b, zi + d);
    float x00 = c[0][0][0] + (c[1][0][0] - c[0][0][0]) * fx, x10 = c[0][1][0] + (c[1][1][0] - c[0][1][0]) * fx;
    float x01 = c[0][0][1] + (c[1][0][1] - c[0][0][1]) * fx, x11 = c[0][1][1] + (c[1][1][1] - c[0][1][1]) * fx;
    float y0 = x00 + (x10 - x00) * fy, y1 = x01 + (x11 - x01) * fy;
    return y0 + (y1 - y0) * fz;
}
static float fbm3(uint32_t seed, l3_vec n, float f) {
    float s = 0, a = .5f, norm = 0;
    for (int o = 0; o < 4; o++, a *= .5f, f *= 2.1f) {
        s += a * noise3(seed + (uint32_t)o * 101u, n.x * f, n.y * f, n.z * f);
        norm += a;
    }
    return s / norm;
}

static l3_vec face_dir(int f, int u, int v) {
    float a = (u + .5f) / (PC / 2) - 1, b = (v + .5f) / (PC / 2) - 1;
    l3_vec d = f == 0 ? l3v(1, b, a) : f == 1 ? l3v(-1, b, a) : f == 2 ? l3v(a, 1, b)
             : f == 3 ? l3v(a, -1, b) : f == 4 ? l3v(a, b, 1) : l3v(a, b, -1);
    return l3_norm(d);
}
static inline int cube_index(l3_vec n) {
    float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z), u, v, m;
    int face;
    if (ax >= ay && ax >= az) { face = n.x > 0 ? 0 : 1; m = ax; u = n.z; v = n.y; }
    else if (ay >= az) { face = n.y > 0 ? 2 : 3; m = ay; u = n.x; v = n.z; }
    else { face = n.z > 0 ? 4 : 5; m = az; u = n.x; v = n.y; }
    int iu = (int)((u / m + 1) * (PC / 2)), iv = (int)((v / m + 1) * (PC / 2));
    iu = iu < 0 ? 0 : iu >= PC ? PC - 1 : iu;
    iv = iv < 0 ? 0 : iv >= PC ? PC - 1 : iv;
    return face * PC * PC + iv * PC + iu;
}

static void bake(int slot, int kind, uint32_t seed) {
    uint8_t *s = surf[slot];
    int bands = 5 + (int)(seed % 4);
    for (int f = 0; f < 6; f++)
        for (int v = 0; v < PC; v++)
            for (int u = 0; u < PC; u++) {
                l3_vec n = face_dir(f, u, v);
                float h = fbm3(seed, n, 2.2f);
                int c, shade = (int)(fbm3(seed ^ 0x55, n, 6) * 4) & 3, flags = 0;
                switch (kind) {
                case 0:     /* living world */
                    c = h < .44f ? C_DEEP : h < .5f ? C_SEA : h < .58f ? C_LAND : h < .64f ? C_HILL : C_ROCK;
                    if (fabsf(n.y) > .86f - (h - .5f) * .3f) c = C_ICE;
                    if ((c == C_LAND || c == C_HILL) && (cg_hash(seed + (uint32_t)(f * 1000 + v * 40 + u)) & 7) == 0)
                        flags |= F_CITY;
                    if (fbm3(seed ^ 0xC10D, n, 3.4f) > .58f) flags |= F_CLOUD;
                    break;
                case 1:     /* desert */
                    c = h < .42f ? C_RUST : h < .62f ? C_HILL : C_ROCK;
                    break;
                case 2:     /* ice */
                    c = h < .45f ? C_SEA : C_ICE;
                    if (fbm3(seed ^ 0xC10D, n, 3) > .62f) flags |= F_CLOUD;
                    break;
                case 3:     /* lava: black crust, glowing seams */
                    c = fabsf(h - .5f) < .025f ? C_LAVA : h > .62f ? C_RUST : C_ROCK;
                    break;
                case 4:     /* barren, cratered */
                    c = h < .4f ? C_ROCK : h > .66f ? C_ICE : C_ROCK;
                    shade = (int)(h * 6) & 3;
                    break;
                default: {  /* gas giant: bands that wobble, a storm */
                    float lat = n.y + (h - .5f) * .35f;
                    int b = (int)floorf((lat + 1) * bands * .5f);
                    static const uint8_t order[4] = {C_GA, C_GC, C_GB, C_GD};
                    c = order[(b + (int)(seed >> 4)) & (seed & 1 ? 3 : 1)];
                    l3_vec spot = l3_norm(l3v(.7f, -.35f, .6f));
                    if (l3_dot(n, spot) > .96f) c = C_GB;
                    shade = (int)(fbm3(seed ^ 0x77, l3v(n.x * .4f, n.y * 3, n.z * .4f), 3) * 4) & 3;
                    break;
                }
                }
                s[f * PC * PC + v * PC + u] = (uint8_t)((c & 15) | (flags & 0xC0) | ((shade & 3) << 4));
            }
}

void ui_planets_reset(void) {
    const game_contents *o = &cg->v.contents;
    for (int i = 0; i < o->planet_count && i < 5; i++) {
        kinds[i] = render_planet_kind(i);
        bake(i, kinds[i], o->planets[i].seed);
    }
}

static inline float hash01(int x, int y) {
    uint32_t g = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;   /* unsigned: no overflow */
    g ^= g >> 13; g *= 0x5bd1e995u; g ^= g >> 15;
    return (g & 1023) * (1 / 1023.f);
}

/* Repaint the dots each planet covers. */
void ui_planets_paint(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    const game_contents *o = &v->contents;
    l3_vec f = l3_quat_rotate(p->attitude, l3v(0, 0, -1)), up = l3_quat_rotate(p->attitude, l3v(0, 1, 0));
    l3_vec rt = l3_cross(f, up);
    float fy = ui.fy, fx = ui.fx, inv = 1 / fy, invx = 1 / fx;
    float cx = ui.dw * .5f, cy = ui.dh * .5f;
    for (int i = 0; i < o->planet_count && i < 5; i++) {
        const game_planet *g = &o->planets[i];
        l3_vec oc = l3_sub(p->position, g->position);
        l3_vec t = l3_scale(oc, -1);
        float z = l3_dot(t, f), dist = sqrtf(l3_dot(oc, oc));
        int x0, x1, y0, y1;
        if (dist < g->radius * 4) {         /* close: the world can fill any part of the view */
            x0 = 0; y0 = 0; x1 = ui.dw; y1 = ui.dh;
        } else {
            if (z <= 1) continue;
            float pcx = cx + l3_dot(t, rt) / z * fx, pcy = cy - l3_dot(t, up) / z * fy;
            float pr = g->radius / sqrtf(fmaxf(1, dist * dist - g->radius * g->radius)) * fy * 1.05f + 2;
            /* off the axis perspective stretches the disc outward by about 1/cos^2 of
               the angle (1 + tan^2): sized for the middle, the box cut it square */
            float tx = (pcx - cx) / fx, ty = (pcy - cy) / fy;
            pr = pr * (1 + tx * tx + ty * ty) * 1.08f + 2;
            x0 = (int)(pcx - pr); x1 = (int)(pcx + pr) + 1; y0 = (int)(pcy - pr); y1 = (int)(pcy + pr) + 1;
            if (x1 < 0 || y1 < 0 || x0 >= ui.dw || y0 >= ui.dh) continue;
        }
        x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > ui.dw ? ui.dw : x1; y1 = y1 > ui.dh ? ui.dh : y1;
        int tag = 242 + i, kind = kinds[i];
        l3_vec L = l3_norm(l3_sub(o->star, g->position));
        float spin = v->clock * .012f + (g->seed % 628) * .01f, cs = cosf(spin), sn = sinf(spin);
        float tilt = ((float)(g->seed % 40) - 20) * CG_RAD, ct = cosf(tilt), st = sinf(tilt);
        float cloud_spin = v->clock * .02f, cc = cosf(cloud_spin), sc2 = sinf(cloud_spin);
        float r2 = g->radius * g->radius, cterm = l3_dot(oc, oc) - r2;
        const uint8_t *map = surf[i];
        bool air = kind == 0 || kind == 2;
        /* a big world is shaded per 2 x 2 block (surface, light, glint, cloud), dithered per dot */
        bool big = (x1 - x0) * (y1 - y0) > 3000 && ui.dw <= 2 * BLOCKS;
        for (int k = 0; k < BLOCKS; k++) blk[k].row = -1;
        for (int y = y0; y < y1; y++) {
            l3_vec row = l3_add(f, l3_scale(up, (cy - y) * inv));
            for (int x = x0; x < x1; x++) {
                if (l3_pick(ui.scene, x, y) != tag) continue;
                block *bk = big ? &blk[x >> 1] : &blk[0];
                if (!big || bk->row != (y & ~1)) {
                    l3_vec d = l3_norm(l3_add(row, l3_scale(rt, (x - cx) * invx)));
                    float b = l3_dot(oc, d), disc = b * b - cterm;
                    float th = disc > 0 ? -b - sqrtf(disc) : -b;
                    l3_vec P = l3_add(oc, l3_scale(d, th));          /* relative to the centre */
                    l3_vec N = l3_scale(P, 1 / g->radius);
                    bk->view = -l3_dot(N, d);                        /* 1 facing us, 0 at the limb */
                    bk->diff = l3_dot(N, L);
                    /* into the planet's own turning frame: tilt about z, then spin about y */
                    float nx = N.x * ct + N.y * st, ny = -N.x * st + N.y * ct, nz = N.z;
                    l3_vec local = l3v(nx * cs - nz * sn, ny, nx * sn + nz * cs);
                    bk->texel = map[cube_index(local)];
                    l3_vec h = l3_norm(l3_sub(L, d));
                    bk->spec = l3_dot(N, h);
                    l3_vec cl = l3v(local.x * cc - local.z * sc2, local.y, local.x * sc2 + local.z * cc);
                    bk->cloud = (map[cube_index(cl)] & F_CLOUD) != 0;
                    bk->row = big ? (y & ~1) : -1;
                }
                float view = bk->view, diff = bk->diff;
                uint8_t texel = bk->texel;
                int c = texel & 15, shade = (texel >> 4) & 3;
                float q = hash01(x, y), bay = lc_bayer(x, y);
                float light = fmaxf(0, diff) * (.78f + shade * .08f) * (.55f + .45f * fminf(1, view * 1.6f));
                uint8_t ink;
                if (light < .03f) {     /* night */
                    ink = LAP_BLACK;
                    if ((texel & F_CITY) && q < .5f) ink = q < .2f ? 11 : LAP_YELLOW;
                    if (c == C_LAVA) ink = q < .5f ? 9 : LAP_RED;
                    if (c == C_ICE && q < .08f) ink = 8;
                } else {
                    const uint8_t *r = ramp[c];
                    float k = fminf(.999f, light) * 3;
                    int lo = (int)k;
                    ink = r[lo + (k - lo > bay ? 1 : 0) > 3 ? 3 : lo + (k - lo > bay ? 1 : 0)];
                    if (c == C_LAVA) ink = light > .5f ? 11 : 9;
                    /* the sun's glint on open water */
                    if (c <= C_SEA) {
                        float spec = bk->spec;
                        if (spec > .985f) ink = 15;
                        else if (spec > .96f && q < .5f) ink = 14;
                    }
                    if (air || kind == 2) {
                        if (bk->cloud && light > .12f)
                            ink = light > .55f ? 15 : light > .3f ? LAP_WHITE : 8;
                    }
                }
                /* air: the lit limb turns pale blue */
                if (air && view < .28f && diff > -.15f && q > view * 3.2f) ink = diff > .2f ? 14 : 12;
                ui.dots[y * ui.dw + x] = ink;
            }
        }
    }
}
