/* A 3D map from CartoCore's L3M1 tiles: the 3 x 3 tiles around a point as one
   landscape, CartoCore's 2D map draped on it as surface materials, buildings
   standing on it, rendered by lap3d at the board's size.

       l3map DIR Z X Y OUT_PREFIX      (X, Y: the centre tile; DIR holds Z_X_Y.bin)
*/
#include "l3png.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TN 3                        /* tiles a side */
#define S 128                       /* surface samples a tile side */
#define G (TN * S)                  /* landscape samples a side */

typedef struct {
    float side, elev[33 * 33];
    uint8_t surf[S * S];
    uint32_t nb;
    uint8_t *recs;                  /* footprint records as read */
    size_t recs_size;
} tile_t;

static tile_t tiles[TN][TN];
static uint8_t height8[G * G], mats[G * G];
static float hmin, hscale, cell;

static bool load(const char *dir, int z, int x, int y, tile_t *t) {
    char path[512];
    snprintf(path, sizeof path, "%s/%d_%d_%d.bin", dir, z, x, y);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n || memcmp(b, "L3M1", 4)) { fclose(f); return false; }
    fclose(f);
    memcpy(&t->side, b + 16, 4);
    memcpy(t->elev, b + 36, sizeof t->elev);
    memcpy(t->surf, b + 4392, sizeof t->surf);
    memcpy(&t->nb, b + 20776, 4);
    t->recs_size = (size_t)n - 20780;
    t->recs = malloc(t->recs_size);
    memcpy(t->recs, b + 20780, t->recs_size);
    free(b);
    return true;
}

/* Elevation in metres at a point of tile t, u and v 0..1 across it. */
static float tile_elev(const tile_t *t, float u, float v) {
    float fx = u * 32, fy = v * 32;
    int x0 = (int)fx, y0 = (int)fy;
    if (x0 > 31) x0 = 31;
    if (y0 > 31) y0 = 31;
    fx -= x0; fy -= y0;
    const float *e = t->elev;
    float a = e[y0 * 33 + x0], b = e[y0 * 33 + x0 + 1], c = e[(y0 + 1) * 33 + x0], d = e[(y0 + 1) * 33 + x0 + 1];
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

/* Ground height in world units (metres above hmin) at world x, z. */
static float ground(float x, float z) {
    int ix = (int)(x / cell), iz = (int)(z / cell);
    if (ix < 0) ix = 0;
    if (iz < 0) iz = 0;
    if (ix >= G) ix = G - 1;
    if (iz >= G) iz = G - 1;
    return height8[iz * G + ix] * hscale;
}

/* Surface classes (CartoCore/docs/3d.md) to materials made in setup(). */
enum { M_LAND, M_WATER, M_WOOD, M_GRASS, M_RESID, M_COMM, M_ROAD, M_MINOR, M_RAIL, M_LOT, M_ROCK, M_WET,
       M_WALL, M_ROOF, M_COUNT };
static void materials(l3_scene *sc) {
    static const uint8_t land[4] = {LAP_BLACK, LAP_GREEN, LAP_GREEN, 10};
    static const uint8_t water[4] = {LAP_BLACK, LAP_BLUE, LAP_BLUE, 12};
    static const uint8_t wood[4] = {LAP_BLACK, LAP_BLACK, LAP_GREEN, LAP_GREEN};
    static const uint8_t grass[4] = {LAP_GREEN, LAP_GREEN, 10, 10};
    static const uint8_t resid[4] = {LAP_BLACK, 8, 8, LAP_WHITE};
    static const uint8_t comm[4] = {LAP_BLACK, 8, LAP_WHITE, LAP_WHITE};
    static const uint8_t road[4] = {8, LAP_YELLOW, 11, 11};
    static const uint8_t minor[4] = {8, LAP_WHITE, LAP_WHITE, 15};
    static const uint8_t rail[3] = {LAP_BLACK, LAP_RED, 8};
    static const uint8_t lot[4] = {LAP_BLACK, 8, 8, LAP_WHITE};
    static const uint8_t rock[4] = {LAP_BLACK, 8, LAP_YELLOW, 11};
    static const uint8_t wet[4] = {LAP_BLACK, LAP_CYAN, LAP_GREEN, 10};
    static const uint8_t wall[4] = {8, LAP_WHITE, 15, 15};
    static const uint8_t roof[4] = {LAP_BLACK, LAP_RED, 9, 9};
    l3_material(sc, land, 4, 0, L3_GRAIN);
    l3_material(sc, water, 4, 0.7f, 0);
    l3_material(sc, wood, 4, 0, L3_GRAIN);
    l3_material(sc, grass, 4, 0, L3_GRAIN);
    l3_material(sc, resid, 4, 0, 0);
    l3_material(sc, comm, 4, 0, 0);
    l3_material(sc, road, 4, 0, 0);
    l3_material(sc, minor, 4, 0, 0);
    l3_material(sc, rail, 3, 0, 0);
    l3_material(sc, lot, 4, 0, 0);
    l3_material(sc, rock, 4, 0, L3_GRAIN);
    l3_material(sc, wet, 4, 0, L3_GRAIN);
    l3_material(sc, wall, 4, 0.1f, 0);
    l3_material(sc, roof, 4, 0.2f, 0);
}

static l3_mesh meshes[TN * TN];
static int nmeshes;

/* One roof-and-wall mesh a tile; a footprint belongs to the tile its
   centre is in, so border buildings are not drawn twice. */
static void buildings(void) {
    for (int ty = 0; ty < TN; ty++)
        for (int tx = 0; tx < TN; tx++) {
            const tile_t *t = &tiles[ty][tx];
            float *xz = malloc((size_t)t->nb * 24 * 2 * sizeof(float));
            uint16_t *counts = malloc(t->nb * sizeof(uint16_t));
            float *base = malloc(t->nb * sizeof(float)), *top = malloc(t->nb * sizeof(float));
            int n = 0, at = 0;
            const uint8_t *p = t->recs;
            for (uint32_t i = 0; i < t->nb; i++) {
                uint16_t h, mh, c;
                memcpy(&h, p, 2); memcpy(&mh, p + 2, 2); memcpy(&c, p + 4, 2);
                p += 6;
                float cx = 0, cz = 0, lo = 1e9f;
                for (int k = 0; k < c; k++) {
                    int16_t px, pz;
                    memcpy(&px, p + k * 4, 2); memcpy(&pz, p + k * 4 + 2, 2);
                    cx += px / 16.0f; cz += pz / 16.0f;
                }
                cx /= c; cz /= c;
                const bool mine = cx >= 0 && cz >= 0 && cx < t->side && cz < t->side;
                if (mine) {
                    for (int k = 0; k < c; k++) {
                        int16_t px, pz;
                        memcpy(&px, p + k * 4, 2); memcpy(&pz, p + k * 4 + 2, 2);
                        const float wx = tx * t->side + px / 16.0f, wz = ty * t->side + pz / 16.0f;
                        xz[at * 2] = wx; xz[at * 2 + 1] = wz; at++;
                        const float g = ground(wx, wz);
                        if (g < lo) lo = g;
                    }
                    counts[n] = c;
                    base[n] = lo + mh / 10.0f;
                    top[n] = lo + h / 10.0f;
                    n++;
                }
                p += c * 4;
            }
            if (n && l3_mesh_prisms(&meshes[nmeshes], xz, counts, n, base, top, M_WALL, M_ROOF))
                nmeshes++;
            free(xz); free(counts); free(base); free(top);
        }
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: l3map DIR Z X Y OUT_PREFIX\n"); return 2; }
    const int z = atoi(argv[2]), cx = atoi(argv[3]), cy = atoi(argv[4]);
    for (int j = 0; j < TN; j++)
        for (int i = 0; i < TN; i++)
            if (!load(argv[1], z, cx - 1 + i, cy - 1 + j, &tiles[j][i])) return 1;
    const float side = tiles[1][1].side;
    cell = side / S;
    /* Elevation at every surface sample, then into eight bits over the range. */
    static float elev[G * G];
    float lo = 1e9f, hi = -1e9f;
    for (int gy = 0; gy < G; gy++)
        for (int gx = 0; gx < G; gx++) {
            const tile_t *t = &tiles[gy / S][gx / S];
            const float e = tile_elev(t, (gx % S + 0.5f) / S, (gy % S + 0.5f) / S);
            elev[gy * G + gx] = e;
            lo = e < lo ? e : lo; hi = e > hi ? e : hi;
            mats[gy * G + gx] = t->surf[(gy % S) * S + gx % S];
        }
    hmin = lo;
    hscale = (hi - lo) / 255.0f;
    for (int i = 0; i < G * G; i++) height8[i] = (uint8_t)lrintf((elev[i] - lo) / hscale);
    printf("landscape %d x %d samples, %.1f m apart, %.0f..%.0f m\n", G, G, cell, lo, hi);
    buildings();
    int tris = 0;
    for (int i = 0; i < nmeshes; i++) tris += meshes[i].ntri;
    printf("buildings: %d meshes, %d triangles\n", nmeshes, tris);

    static const struct { const char *name; float dist, yaw, pitch, fov; int cw, ch; } views[] = {
        {"overview", 1500, 30, 38, 45, 54, 70},
        {"town", 650, 210, 28, 50, 54, 70},
        {"street", 260, 120, 14, 60, 54, 70},
        {"wide", 1300, 300, 30, 45, 116, 30},
    };
    const float mx = side * TN / 2, mz = side * TN / 2;
    for (size_t v = 0; v < sizeof views / sizeof views[0]; v++) {
        l3_scene *sc = l3_create(views[v].cw, views[v].ch);
        if (!sc) return 1;
        l3_reserve(sc, 64, 20000, tris * 3 + 4096);   /* clipped triangles, in the sun view and the camera view */
        materials(sc);
        l3_sun(sc, l3v(-0.45f, -0.75f, -0.5f), 0.65f, 0.22f);
        l3_shadows(sc, true);
        l3_sky(sc, LAP_BLUE, 14);
        l3_fog(sc, 14, 2200, 4200);
        l3_clip(sc, 2, 6000);
        l3_begin(sc);
        const float gy = ground(mx, mz) + 10;
        l3_camera_orbit(sc, l3v(mx, gy, mz), views[v].dist, views[v].yaw, views[v].pitch, views[v].fov);
        l3_flags(sc, L3_STATIC | L3_CAST);
        l3_terrain(sc, height8, mats, G, G, cell, hscale, l3v(0, 0, 0), M_LAND);
        l3_xform id = l3_identity();
        for (int i = 0; i < nmeshes; i++) l3_draw_mesh(sc, &meshes[i], &id, M_WALL);
        l3_end(sc);
        l3_stats_t st;
        l3_stats(sc, &st);
        char path[600];
        snprintf(path, sizeof path, "%s-%s.png", argv[5], views[v].name);
        printf("%-9s %6u us  dropped %u  -> %s\n", views[v].name, (unsigned)st.us_total, (unsigned)st.dropped, path);
        l3png(path, sc);
        l3_destroy(sc);
    }
    return 0;
}
