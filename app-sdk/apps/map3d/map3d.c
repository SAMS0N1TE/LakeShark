/* MAP 3D for LakeShark: the town around you in 3D, from CartoCore's map
   files. Nine tiles from /sdcard/maps3d (made by `cartocore export3d`) become
   one landscape: real elevation, CartoCore's map draped over it - water,
   woods, roads, land use - and every building standing up, with sun shadows
   and haze. Drag to fly round, + and - to come closer or pull back.
   Uses API 1.2 (lap3d, l3_mesh_prisms) and the SD card jobs. */
#include "lakeshark_app.h"
#include "lap3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TN 3
#define S 128
#define G (TN * S)
#define DIR "/sdcard/maps3d"

typedef struct {
    float side, elev[33 * 33];
    uint8_t *file;
    int size;
} tile_t;

static tile_t tiles[TN][TN];
static int zoom_z = 14, cx = 4931, cy = 5992;   /* the centre tile */
static uint8_t *height8, *mats;
static float hscale, cell;
static l3_mesh meshes[TN * TN];
static int nmeshes, ntris;
static l3_scene *sc;
static int stage, job, loading;                 /* loading: which tile, 0..8 */
static int failed_at, failed_code;             /* the step and the card's answer when reading failed */
static char status[48];
static float yaw = 30, pitch = 35, dist = 1500;
static uint32_t opened_ms;

enum { M_LAND, M_WATER, M_WOOD, M_GRASS, M_RESID, M_COMM, M_ROAD, M_MINOR, M_RAIL, M_LOT, M_ROCK, M_WET,
       M_WALL, M_ROOF };

static void materials(void) {
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

static float tile_elev(const tile_t *t, float u, float v) {
    float fx = u * 32, fy = v * 32;
    int x0 = (int)fx, y0 = (int)fy;
    if (x0 > 31) x0 = 31;
    if (y0 > 31) y0 = 31;
    fx -= x0; fy -= y0;
    const float *e = t->elev;
    const float a = e[y0 * 33 + x0], b = e[y0 * 33 + x0 + 1], c = e[(y0 + 1) * 33 + x0], d = e[(y0 + 1) * 33 + x0 + 1];
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

static float ground(float x, float z) {
    int ix = (int)(x / cell), iz = (int)(z / cell);
    ix = ix < 0 ? 0 : ix >= G ? G - 1 : ix;
    iz = iz < 0 ? 0 : iz >= G ? G - 1 : iz;
    return height8[iz * G + ix] * hscale;
}

/* All nine read: the landscape, then a roof-and-wall mesh a tile. */
static bool build(void) {
    height8 = malloc(G * G);
    mats = malloc(G * G);
    float *elev = malloc(G * G * sizeof(float));
    if (!height8 || !mats || !elev) { free(elev); return false; }
    float lo = 1e9f, hi = -1e9f;
    for (int gy = 0; gy < G; gy++)
        for (int gx = 0; gx < G; gx++) {
            const tile_t *t = &tiles[gy / S][gx / S];
            const float e = tile_elev(t, (gx % S + 0.5f) / S, (gy % S + 0.5f) / S);
            elev[gy * G + gx] = e;
            lo = e < lo ? e : lo; hi = e > hi ? e : hi;
            mats[gy * G + gx] = t->file[4392 + (gy % S) * S + gx % S];
        }
    hscale = (hi - lo) / 255.0f;
    if (hscale <= 0) hscale = 1;
    for (int i = 0; i < G * G; i++) height8[i] = (uint8_t)((elev[i] - lo) / hscale + 0.5f);
    free(elev);
    for (int ty = 0; ty < TN; ty++)
        for (int tx = 0; tx < TN; tx++) {
            const tile_t *t = &tiles[ty][tx];
            uint32_t nb;
            memcpy(&nb, t->file + 20776, 4);
            float *xz = malloc((size_t)nb * 48 * sizeof(float));
            uint16_t *counts = malloc(nb * sizeof(uint16_t) + 2);
            float *base = malloc(nb * sizeof(float) + 4), *top = malloc(nb * sizeof(float) + 4);
            if (!xz || !counts || !base || !top) { free(xz); free(counts); free(base); free(top); return false; }
            int n = 0, at = 0;
            const uint8_t *p = t->file + 20780, *end = t->file + t->size;
            for (uint32_t i = 0; i < nb && p + 6 <= end; i++) {
                uint16_t h, mh, c;
                memcpy(&h, p, 2); memcpy(&mh, p + 2, 2); memcpy(&c, p + 4, 2);
                p += 6;
                if (p + c * 4 > end) break;
                float mx = 0, mz = 0, low = 1e9f;
                for (int k = 0; k < c; k++) {
                    int16_t px, pz;
                    memcpy(&px, p + k * 4, 2); memcpy(&pz, p + k * 4 + 2, 2);
                    mx += px / 16.0f; mz += pz / 16.0f;
                }
                mx /= c; mz /= c;
                if (mx >= 0 && mz >= 0 && mx < t->side && mz < t->side) {   /* its own tile only */
                    for (int k = 0; k < c; k++) {
                        int16_t px, pz;
                        memcpy(&px, p + k * 4, 2); memcpy(&pz, p + k * 4 + 2, 2);
                        const float wx = tx * t->side + px / 16.0f, wz = ty * t->side + pz / 16.0f;
                        xz[at * 2] = wx; xz[at * 2 + 1] = wz; at++;
                        const float g = ground(wx, wz);
                        low = g < low ? g : low;
                    }
                    counts[n] = c;
                    base[n] = low + mh / 10.0f;
                    top[n] = low + h / 10.0f;
                    n++;
                }
                p += c * 4;
            }
            if (n && l3_mesh_prisms(&meshes[nmeshes], xz, counts, n, base, top, M_WALL, M_ROOF)) {
                ntris += meshes[nmeshes].ntri;
                nmeshes++;
            }
            free(xz); free(counts); free(base); free(top);
        }
    for (int j = 0; j < TN; j++)
        for (int i = 0; i < TN; i++) { free(tiles[j][i].file); tiles[j][i].file = NULL; }
    return true;
}

/* One SD job at a time: each tile's size, then its bytes. */
static void load_step(void) {
    tile_t *t = &tiles[loading / TN][loading % TN];
    char path[64];
    snprintf(path, sizeof path, DIR "/%d_%d_%d.bin", zoom_z, cx - 1 + loading % TN, cy - 1 + loading / TN);
    if (stage == 0) { job = lap_sd_size(path); if (job < 0) { failed_at = 1; failed_code = job; stage = 9; } else stage = 1; return; }
    const int r = lap_sd_poll(job);
    if (r == LAP_SD_PENDING) return;
    if (stage == 1) {
        if (r < 20780 || r > 65536 || !(t->file = malloc((size_t)r))) { failed_at = 2; failed_code = r; stage = 9; return; }
        t->size = r;
        job = lap_sd_read(path, 0, t->file, (uint32_t)r);
        if (job < 0) { failed_at = 3; failed_code = job; stage = 9; } else stage = 2;
        return;
    }
    if (r != t->size || memcmp(t->file, "L3M1", 4)) { failed_at = 4; failed_code = r; stage = 9; return; }
    memcpy(&t->side, t->file + 16, 4);
    memcpy(t->elev, t->file + 36, sizeof t->elev);
    stage = 0;
    if (++loading == TN * TN) {
        stage = build() ? 10 : 9;
        cell = tiles[1][1].side / S;
    }
}

static void enter(void) {
    lap_fullscreen(true);
    lap_keep_awake(true);
    opened_ms = lap_millis();
}

static void leave(void) {
    for (int i = 0; i < nmeshes; i++) l3_mesh_free(&meshes[i]);
    nmeshes = ntris = 0;
    for (int j = 0; j < TN; j++)
        for (int i = 0; i < TN; i++) { free(tiles[j][i].file); tiles[j][i].file = NULL; }
    free(height8); free(mats); height8 = mats = NULL;
    l3_destroy(sc); sc = NULL;
    stage = loading = 0;
}

/* The zoom keys: two filled keys, 3 rows by 10 columns, low on the right. */
#define KEY_W 10
#define KEY_H 3
static int key_x(void) { return lap_cols() - KEY_W - 2; }
static int key_y(int which) { return lap_rows() - (KEY_H + 1) * (2 - which) - 1; }   /* 0: +, 1: - */
static int key_at(int col, int row) {
    for (int k = 0; k < 2; k++)
        if (col >= key_x() && col < key_x() + KEY_W && row >= key_y(k) && row < key_y(k) + KEY_H)
            return k;
    return -1;
}
static void draw_key(int k, bool held) {
    const uint8_t at = held ? LAP_ATTR(LAP_BLACK, LAP_WHITE | LAP_BRIGHT) : LAP_ATTR(LAP_BLACK, LAP_CYAN | LAP_BRIGHT);
    for (int r = 0; r < KEY_H; r++)
        lap_put_str(key_x(), key_y(k) + r, r == 1 ? (k ? "    -     " : "    +     ") : "          ", at);
}

/* A finger that lands on a key zooms for as long as it stays there; any
   other finger flies the camera round. */
static int held_key = -1;
static void handle_touch(float dt) {
    lap_touch_t f;
    int cw, ch;
    lap_cell_px(&cw, &ch);
    if (!lap_touch(&f) || !f.down) { held_key = -1; l3_drag_turn(&yaw, &pitch, 0.35f); return; }
    held_key = key_at(f.x0 / cw, f.y0 / ch);       /* where the finger landed decides */
    if (held_key >= 0) {
        dist *= held_key == 0 ? 1 - 0.9f * dt : 1 + 0.9f * dt;
        dist = dist < 150 ? 150 : dist > 3500 ? 3500 : dist;
        return;
    }
    l3_drag_turn(&yaw, &pitch, 0.35f);
}

static void draw(void) {
    const uint8_t hud = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK);
    if (stage < 9) {
        load_step();
        lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
        snprintf(status, sizeof status, " MAP 3D  reading tile %d of 9 ", loading + 1);
        lap_put_str(1, lap_rows() / 2, status, hud);
        return;
    }
    if (stage == 9) {
        lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
        lap_put_str(1, lap_rows() / 2 - 1, " No 3D map on the card. ", LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK));
        lap_put_str(1, lap_rows() / 2 + 1, " Put CartoCore export3d tiles in", hud);
        lap_put_str(1, lap_rows() / 2 + 2, " " DIR, hud);
        snprintf(status, sizeof status, " tile %d, step %d, answer %d ", loading + 1, failed_at, failed_code);
        lap_put_str(1, lap_rows() / 2 + 4, status, LAP_ATTR(LAP_WHITE, LAP_BLACK));
        return;
    }
    if (!sc) {
        sc = l3_create(lap_cols(), lap_rows());
        if (!sc) { lap_put_str(1, 1, "not enough memory", LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK)); return; }
        l3_reserve(sc, 64, 20000, 16384);     /* what is on screen at once, not every building */
        materials();
        l3_sun(sc, l3v(-0.45f, -0.75f, -0.5f), 0.65f, 0.22f);
        l3_shadows(sc, true);
        l3_sky(sc, LAP_BLUE, 14);
        l3_fog(sc, 14, 2200, 4200);
        l3_clip(sc, 2, 6000);
    }
    l3_resize(sc, lap_cols(), lap_rows());
    static uint32_t last;
    const uint32_t now = lap_millis();
    const float dt = last ? fminf(0.1f, (now - last) / 1000.0f) : 0;
    last = now;
    handle_touch(dt);
    if (pitch < 8) pitch = 8;
    const float mx = cell * G / 2, mz = cell * G / 2;
    l3_begin(sc);
    l3_camera_orbit(sc, l3v(mx, ground(mx, mz) + 10, mz), dist, yaw, pitch, 45);
    l3_flags(sc, L3_STATIC | L3_CAST);
    l3_terrain(sc, height8, mats, G, G, cell, hscale, l3v(0, 0, 0), M_LAND);
    l3_xform id = l3_identity();
    for (int i = 0; i < nmeshes; i++) l3_draw_mesh(sc, &meshes[i], &id, M_WALL);
    l3_end(sc);
    l3_show(sc, 0, 0);
    l3_stats_t st;
    l3_stats(sc, &st);
    snprintf(status, sizeof status, " FRANKLIN NH  %2u ms ", (unsigned)(st.us_total / 1000));
    lap_put_str(0, 0, status, hud);
    draw_key(0, held_key == 0);
    draw_key(1, held_key == 1);
    if (lap_millis() - opened_ms < 6000)
        lap_put_str(0, lap_rows() - 1, " drag to fly round ", LAP_ATTR(LAP_WHITE, LAP_BLACK));
}

static void zoom_by(float k) {
    dist *= k;
    dist = dist < 150 ? 150 : dist > 3500 ? 3500 : dist;
}

static bool touch(int col, int row) {
    const int k = key_at(col, row);
    if (k == 0) { zoom_by(0.8f); return true; }      /* a quick tap steps */
    if (k == 1) { zoom_by(1.25f); return true; }
    return false;
}

static bool key(lap_key_t k, char ch) {
    if (k == LAP_KEY_UP || ch == '+') { zoom_by(0.75f); return true; }
    if (k == LAP_KEY_DOWN || ch == '-') { zoom_by(1.33f); return true; }
    if (k == LAP_KEY_LEFT) { yaw += 15; return true; }
    if (k == LAP_KEY_RIGHT) { yaw -= 15; return true; }
    return false;
}

LAP_ICON_PX(
    "cccccccccccccccccccc",
    "cccccccccccccccccccc",
    "ccccccccccccc77ccccc",
    "cccccccccccc7777cccc",
    "cccccc11ccc777777ccc",
    "ccccc1111c77777777cc",
    "cccc999999ff777777cc",
    "cccc9ff9f9ff777777cc",
    "ccc99ff9f99f7117777c",
    "ccaafffffffff9999faa",
    "aaaaf00ff00f999999aa",
    "aaaaffffffff9ff9ffaa",
    "aaaaaaaaaaaabb2b22aa",
    "bbbbbbbbbbbbbbbbbbbb",
    "aaaaaaaaaaaaaa2222aa",
    "44444444444444444444",
    "44e4444444e444444e44",
    "44444444444444444444");

LAP_APP(.id = "map3d", .name = "MAP 3D", .sub = "your town in 3D", .version = "0.1",
        .author = "LakeShark", .color = LAP_GREEN,
        .icon_art = { "   _/\\_   ", "  |[]|[]  ", " _|[]|[]_ ", "/_______\\ ", "~~~~~~~~~~", "  MAP 3D  " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
