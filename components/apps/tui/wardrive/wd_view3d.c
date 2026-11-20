/* WARDRIVE's 3D page (wd_view3d.h), on lap3d.
   World: one unit is 10 m; +x east, +z south, +y up, origin where the
   session's first fix was. Yaw 0 looks north, so the camera's yaw is the
   board's heading when it rides behind. */
#include "wd_view3d.h"
#include "wd_core.h"
#include "wd_feed.h"
#include "../../../../app-sdk/include/lap3d.h"
#include "../ls_tui_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define UNIT 10.0f                 /* metres in a world unit */
#define REACH 60.0f                /* units around the camera's target that are drawn */
#define PILLARS 140
#define TRAIL_SEGS 320
#define LABELS 2
#define ORBIT_MS 30000u            /* after stopping the camera swings round this long, then rests */
#define LOOK_MS 15000u             /* a turn by hand holds this long */

typedef struct { float x, z, h; uint8_t kind, sec; int8_t rssi; uint32_t age; float d2; } pillar_t;

static l3_scene *sc;
static int sc_w, sc_h;
static int m_kind[WD_KINDS + WD_SECS], m_trail, m_trail_old, m_board, m_glow, m_ring;
static EXT_RAM_BSS_ATTR pillar_t P[512];
static int np;
static EXT_RAM_BSS_ATTR l3_vec trail_v[TRAIL_SEGS + 1];
static uint8_t trail_new[TRAIL_SEGS + 1];
static int ntv;
static EXT_RAM_BSS_ATTR char label_text[LABELS][20];
static EXT_RAM_BSS_ATTR l3_vec label_at[LABELS];
static uint8_t label_ink[LABELS];
static int nlabels;

/* Camera. */
static float c_tx, c_tz, c_yaw, c_pitch = 46, c_dist = 26;
static bool c_init;
static uint32_t c_last_ms, c_still_since, c_look_ms, c_render_ms;
static float r_tx = 1e9f, r_tz, r_yaw, r_pitch, r_dist;
static bool c_follow = true, c_wide;
static const char *c_mode = "FOLLOW";
static unsigned st_us, st_dots;

static uint8_t sec_hue(int kind, int sec)
{
    if (kind == WD_WIFI) {
        static const uint8_t H[WD_SECS] = { TUI_GREEN, TUI_YELLOW, TUI_YELLOW, TUI_CYAN, TUI_BLUE };
        return H[sec < WD_SECS ? sec : 3];
    }
    return kind == WD_BLE ? TUI_MAGENTA : kind == WD_CELL ? TUI_WHITE : TUI_RED;
}

static int material_for(int kind, int sec)
{
    return kind == WD_WIFI ? m_kind[WD_KINDS + (sec < WD_SECS ? sec : 3)] : m_kind[kind];
}

static void materials(void)
{
    for (int k = 0; k < WD_KINDS + WD_SECS; k++) {
        const int kind = k < WD_KINDS ? k : WD_WIFI, sec = k < WD_KINDS ? 3 : k - WD_KINDS;
        const uint8_t h = sec_hue(kind, sec);
        const uint8_t inks[4] = { TUI_BLACK, h, (uint8_t)(h | TUI_BRIGHT), TUI_WHITE | TUI_BRIGHT };
        m_kind[k] = l3_material(sc, inks, h == TUI_WHITE ? 3 : 4, 0.35f, 0);
    }
    const uint8_t trail[2] = { TUI_YELLOW, TUI_YELLOW | TUI_BRIGHT };
    m_trail = l3_material(sc, trail, 2, 0, L3_UNLIT);
    const uint8_t old[1] = { TUI_YELLOW };
    m_trail_old = l3_material(sc, old, 1, 0, L3_UNLIT);
    const uint8_t board[4] = { TUI_RED, TUI_RED | TUI_BRIGHT, TUI_WHITE, TUI_WHITE | TUI_BRIGHT };
    m_board = l3_material(sc, board, 4, 0.6f, 0);
    const uint8_t glow[1] = { TUI_WHITE | TUI_BRIGHT };
    m_glow = l3_material(sc, glow, 1, 0, L3_UNLIT);
    const uint8_t ring[1] = { TUI_CYAN | TUI_BRIGHT };
    m_ring = l3_material(sc, ring, 1, 0, L3_UNLIT | L3_GLOW);
}

static bool scene(int w, int h)
{
    if (sc && (w != sc_w || h != sc_h)) {
        if (!l3_resize(sc, w, h)) { l3_destroy(sc); sc = NULL; }
    }
    if (!sc) {
        sc = l3_create(w, h);
        if (!sc) return false;
        l3_reserve(sc, 640, 1024, 2048);
        materials();
        r_tx = 1e9f;
    }
    sc_w = w; sc_h = h;
    return true;
}

void wd3d_leave(void)
{
    if (sc) l3_destroy(sc);
    sc = NULL;
    c_init = false;
}

static float wrap180(float d)
{
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

static int by_dist(const void *a, const void *b)
{
    const float x = ((const pillar_t *)a)->d2, y = ((const pillar_t *)b)->d2;
    return x < y ? -1 : x > y;
}

/* Where a pillar stands. A network is known by where the board was when it
   was strongest, so a scan's finds would all stand on one spot of the road;
   each is set out from it by how weak it was (a weak one is far away), in a
   direction its address picks, so the street fills out the way it is. */
static void spot(const wd_dev_t *e, float *x, float *z)
{
    const uint32_t h = (uint32_t)(e->addr[5] | e->addr[4] << 8 | e->addr[3] << 16) * 2654435761u;
    const float ang = (float)(h >> 20) * (6.2831853f / 4096.0f);
    const float r = fminf(9.0f, fmaxf(0.8f, powf(10.0f, (-35.0f - e->rssi) / 27.0f) / UNIT));
    *x = e->x / UNIT + cosf(ang) * r;
    *z = e->z / UNIT + sinf(ang) * r;
}

/* What the scene needs, copied under the lock so the worker is held up for
   as little as a copy. */
static void gather(uint32_t now, float *bx, float *bz, float *heading, float *speed, bool *fix,
                   unsigned *scans, uint32_t *scan_ms)
{
    wd_lock();
    const wd_stats_t *s = wd_stats();
    *bx = s->x / UNIT; *bz = s->z / UNIT;
    *heading = s->heading; *speed = s->speed;
    *fix = s->origin && s->fix.fix;
    *scans = s->scans; *scan_ms = s->scan_ms;
    const float cx = c_init ? c_tx : *bx, cz = c_init ? c_tz : *bz;
    np = 0;
    const int n = wd_dev_count();
    for (int i = 0; i < n && np < (int)(sizeof(P) / sizeof(P[0])); i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (!(e->flags & WD_F_PLACED)) continue;
        /* Far off by where it was heard: skip before the trigonometry (a full table is 4096). */
        const float rx = e->x / UNIT - cx, rz = e->z / UNIT - cz;
        if (rx * rx + rz * rz > (REACH + 9) * (REACH + 9)) continue;
        float x, z;
        spot(e, &x, &z);
        const float d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
        if (d2 > REACH * REACH) continue;
        pillar_t *p = &P[np++];
        p->x = x; p->z = z; p->d2 = d2;
        p->kind = e->kind;
        p->sec = (uint8_t)(e->kind == WD_WIFI ? wd_sec_class(e->auth) : 0);
        p->rssi = e->rssi;
        p->age = now - e->first_ms;
    }
    /* The newest trail points within reach, thinned to what the ribbon can carry. */
    ntv = 0;
    const int nt = wd_trail_count();
    int stride = 1;
    while (nt / stride > TRAIL_SEGS * 4) stride *= 2;
    for (int i = nt - 1; i >= 0 && ntv <= TRAIL_SEGS; i -= stride) {
        const wd_pt_t *t = wd_trail_at(i);
        const float x = t->x / UNIT, z = t->z / UNIT;
        if ((x - cx) * (x - cx) + (z - cz) * (z - cz) > REACH * REACH * 1.6f) { if (ntv) break; continue; }
        trail_new[ntv] = now - t->ms < 120000;
        trail_v[ntv++] = l3v(x, 0.04f, z);
    }
    /* The strongest Wi-Fi names close by float over their pillars. */
    nlabels = 0;
    int best[LABELS];
    for (int k = 0; k < LABELS; k++) best[k] = -1;
    for (int i = 0; i < n; i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (e->kind != WD_WIFI || !e->name[0] || !(e->flags & WD_F_PLACED) || e->rssi < -72) continue;
        const float rx = e->x / UNIT - cx, rz = e->z / UNIT - cz;
        if (rx * rx + rz * rz > 27.0f * 27.0f) continue;
        float x, z;
        spot(e, &x, &z);
        x -= cx; z -= cz;
        if (x * x + z * z > 18.0f * 18.0f || e->rssi < -72) continue;
        for (int k = 0; k < LABELS; k++) {
            if (best[k] < 0 || e->rssi > wd_dev_at(best[k])->rssi) {
                for (int m = LABELS - 1; m > k; m--) best[m] = best[m - 1];
                best[k] = i;
                break;
            }
        }
    }
    for (int k = 0; k < LABELS && best[k] >= 0; k++) {
        const wd_dev_t *e = wd_dev_at(best[k]);
        int j = 0;
        for (const char *c = e->name; *c && j < 16; c++) {
            const char u = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
            label_text[k][j++] = (u >= 32 && u < 127) ? u : '?';
        }
        label_text[k][j] = 0;
        const float hgt = 0.6f + fmaxf(0, fminf(1, (e->rssi + 95) / 60.0f)) * 4.2f;
        float lx, lz;
        spot(e, &lx, &lz);
        label_at[k] = l3v(lx, hgt + 0.9f, lz);
        label_ink[k] = (uint8_t)(sec_hue(WD_WIFI, wd_sec_class(e->auth)) | TUI_BRIGHT);
        nlabels++;
    }
    wd_unlock();
    if (np > PILLARS) { qsort(P, (size_t)np, sizeof(P[0]), by_dist); np = PILLARS; }
}

static void camera(uint32_t now, float bx, float bz, float heading, float speed, bool fix)
{
    const float dt = c_last_ms ? fminf(0.25f, (now - c_last_ms) / 1000.0f) : 0;
    c_last_ms = now;
    const bool moving = fix && speed > 1.5f;
    /* Moving, the view looks a little ahead of the board, where the next finds are. */
    if (moving && heading >= 0) {
        const float hr = heading * 3.14159265f / 180;
        const float ahead = c_wide ? 2.5f : 6.0f;
        bx += sinf(hr) * ahead; bz -= cosf(hr) * ahead;
    }
    if (!c_init) { c_tx = bx; c_tz = bz; c_yaw = heading >= 0 ? heading : 0; c_init = true; c_still_since = now; }
    if (moving) c_still_since = now;
    const bool looking = c_look_ms && now - c_look_ms < LOOK_MS;
    if (!looking) c_look_ms = 0;
    /* Follow the board; stood still, only a real move shifts the view, not GPS wander. */
    const float dx = bx - c_tx, dz = bz - c_tz;
    if (moving || dx * dx + dz * dz > 0.8f * 0.8f) {
        const float k = fminf(1, dt * 3.5f);
        c_tx += dx * k; c_tz += dz * k;
        if (dx * dx + dz * dz < 0.0004f) { c_tx = bx; c_tz = bz; }
    }
    if (looking) c_mode = "LOOK";
    else if (moving && c_follow) {
        c_mode = "FOLLOW";
        if (heading >= 0) c_yaw += wrap180(heading - c_yaw) * fminf(1, dt * 1.6f);
    } else if (now - c_still_since < ORBIT_MS && c_follow) {
        c_mode = "ORBIT";
        c_yaw += 9.0f * dt;
    } else c_mode = c_follow ? "REST" : "LOOK";
    c_yaw = wrap180(c_yaw);
}

static void blit(tui_surface *sf, tui_rect a)
{
    const uint8_t *dots = l3_dots(sc);
    const int dw = sc_w * 2;
    for (int r = 0; r < a.h && r < sc_h; r++) {
        const uint8_t *row0 = dots + (size_t)(3 * r) * dw;
        for (int c = 0; c < a.w && c < sc_w; c++) {
            uint8_t col[6], count[16] = { 0 };
            for (int k = 0; k < 6; k++) {
                const uint8_t d = row0[(size_t)(k / 2) * dw + 2 * c + (k & 1)];
                col[k] = d == 0xFF ? 0 : d & 15;
                count[col[k]]++;
            }
            int fg = col[0], bg = 0, bits = 0x3F;
            if (count[fg] != 6) {
                for (int k = 1; k < 6; k++)
                    if (count[col[k]] > count[fg] || (count[col[k]] == count[fg] && col[k] > fg)) fg = col[k];
                int paper = -1;
                for (int k = 0; k < 6; k++)
                    if (col[k] != fg && (paper < 0 || count[col[k]] > count[paper])) paper = col[k];
                bg = paper >= 0 ? paper : 0;
                bits = 0;
                for (int k = 0; k < 6; k++) if (col[k] == fg) bits |= 1 << k;
            }
            if (fg == 0 && bits == 0x3F) tui_put_char(sf, a, a.x + c, a.y + r, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
            else tui_put_char(sf, a, a.x + c, a.y + r, LS_TUI_SEXT(bits), TUI_ATTR(fg, bg));
        }
    }
}

bool wd3d_draw(tui_surface *sf, tui_rect a, uint32_t now)
{
    if (a.w < 8 || a.h < 4 || !scene(a.w, a.h)) return false;
    ls_ink_keep(a);
    float bx, bz, heading, speed;
    bool fix;
    unsigned scans;
    uint32_t scan_ms;
    gather(now, &bx, &bz, &heading, &speed, &fix, &scans, &scan_ms);
    c_wide = a.w > 2 * a.h;
    camera(now, bx, bz, heading, speed, fix);
    const bool moved = fabsf(c_tx - r_tx) > 1e-3f || fabsf(c_tz - r_tz) > 1e-3f || fabsf(c_yaw - r_yaw) > 0.05f ||
                       c_pitch != r_pitch || c_dist != r_dist;
    /* A moving camera redraws every dot: hold it to 12 frames a second. A
       still one redraws only what changed, so every frame is cheap. */
    if (!moved || now - c_render_ms >= 80 || now < c_render_ms) {
        c_render_ms = now;
        r_tx = c_tx; r_tz = c_tz; r_yaw = c_yaw; r_pitch = c_pitch; r_dist = c_dist;
        l3_begin(sc);
        l3_sky(sc, TUI_BLACK, TUI_BLACK);
        l3_fog(sc, TUI_BLACK, 28, 62);
        l3_clip(sc, 0.3f, 400);
        l3_sun(sc, l3v(-0.4f, 1, 0.3f), 0.9f, 0.35f);
        /* A wide screen shows more around the board: come in closer there. */
        const bool wide = a.w > 2 * a.h;
        const float dist = c_dist * (wide ? 0.62f : 1.0f), pitch = fminf(80, c_pitch + (wide ? 14 : 0));
        l3_camera_orbit(sc, l3v(c_tx, 0.6f, c_tz), dist, c_yaw, pitch, 55);
        /* The ground: an 80 m grid that stays put under the moving board, a
           line every fourth a little brighter, and N where north is. */
        l3_flags(sc, L3_STATIC);
        const float step = 8, gx = floorf(c_tx / step) * step, gz = floorf(c_tz / step) * step;
        for (int i = -5; i <= 5; i++) {
            const float x = gx + i * step, z = gz + i * step;
            const bool major_x = ((int)lroundf(x / step) & 3) == 0, major_z = ((int)lroundf(z / step) & 3) == 0;  /* every 320 m */
            l3_line(sc, l3v(x, 0, gz - 40), l3v(x, 0, gz + 40), major_x ? TUI_CYAN : TUI_BLUE);
            l3_line(sc, l3v(gx - 40, 0, z), l3v(gx + 40, 0, z), major_z ? TUI_CYAN : TUI_BLUE);
        }
        l3_label(sc, l3v(c_tx, 0.2f, c_tz - 30), "N", 2, TUI_RED | TUI_BRIGHT, TUI_BLACK);
        /* The drive, newest first. */
        for (int i = 1; i < ntv; i++) {
            const l3_vec p = trail_v[i - 1], q = trail_v[i];
            const float ex = q.x - p.x, ez = q.z - p.z, len = sqrtf(ex * ex + ez * ez);
            if (len < 1e-3f || len > 12) continue;
            const float w = 0.3f / len, nx = -ez * w, nz = ex * w;
            l3_flags(sc, L3_STATIC | L3_TWO_SIDED);
            l3_quad(sc, l3v(p.x + nx, p.y, p.z + nz), l3v(q.x + nx, q.y, q.z + nz),
                    l3v(q.x - nx, q.y, q.z - nz), l3v(p.x - nx, p.y, p.z - nz), trail_new[i] ? m_trail : m_trail_old);
        }
        /* A pillar for each network, as tall as it was strong; a new one grows in.
           Pillars stay dynamic and in table order, so a new one is appended and
           a still camera redraws only around it (lap3d compares by position). */
        l3_flags(sc, 0);
        for (int i = 0; i < np; i++) {
            const pillar_t *p = &P[i];
            float h = 0.6f + fmaxf(0, fminf(1, (p->rssi + 95) / 60.0f)) * 4.2f;
            if (p->age < 700) { const float t = p->age / 700.0f; h *= 1 - (1 - t) * (1 - t); }
            const int mat = material_for(p->kind, p->sec);
            if (p->kind == WD_CELL) l3_cone(sc, l3v(p->x, 0, p->z), l3v(p->x, h + 2, p->z), 0.5f, 0.08f, mat);
            else if (p->kind == WD_DRONE) {
                l3_cylinder(sc, l3v(p->x, 0, p->z), l3v(p->x, h + 1, p->z), 0.08f, mat);
                l3_sphere(sc, l3v(p->x, h + 1.3f, p->z), 0.45f, mat);
            } else l3_cylinder(sc, l3v(p->x, 0, p->z), l3v(p->x, h, p->z), p->kind == WD_BLE ? 0.15f : 0.22f, mat);
        }
        for (int k = 0; k < nlabels; k++) l3_label(sc, label_at[k], label_text[k], 1, label_ink[k], TUI_BLACK);
        /* The board: an arrow along its heading and an aerial. */
        const float hr = (heading >= 0 ? heading : c_yaw) * 3.14159265f / 180;
        const float fx = sinf(hr), fz = -cosf(hr);
        l3_flags(sc, 0);
        l3_cone(sc, l3v(bx - fx * 0.9f, 0.5f, bz - fz * 0.9f), l3v(bx + fx * 1.1f, 0.5f, bz + fz * 1.1f), 0.6f, 0.02f, m_board);
        l3_cylinder(sc, l3v(bx, 0.5f, bz), l3v(bx, 1.9f, bz), 0.05f, m_glow);
        l3_sphere(sc, l3v(bx, 2.0f, bz), 0.16f, m_glow);
        /* ...and a ring that goes out once for each scan that finished. */
        const uint32_t since = now - scan_ms;
        if (scans && scan_ms && since < 900) {
            const float t = since / 900.0f;
            const l3_xform x = l3_pose(l3v(bx, 0.1f, bz), l3_quat_axis(l3v(0, 1, 0), 0), 1);
            l3_torus(sc, &x, 0.6f + t * 5.5f, 0.07f, m_ring);
        }
        l3_end(sc);
        l3_stats_t st;
        l3_stats(sc, &st);
        st_us = st.us_total;
        st_dots = st.dots_shaded;
    }
    blit(sf, a);
    return true;
}

bool wd3d_key(ls_tk_t k, char ch, uint32_t now)
{
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT) { c_yaw += k == LS_TK_LEFT ? -20 : 20; c_look_ms = now; return true; }
    if (k == LS_TK_UP) { c_pitch = fminf(80, c_pitch + 6); return true; }
    if (k == LS_TK_DOWN) { c_pitch = fmaxf(8, c_pitch - 6); return true; }
    if (ch == '+' || ch == '=') { c_dist = fmaxf(6, c_dist * 0.8f); return true; }
    if (ch == '-' || ch == '_') { c_dist = fminf(45, c_dist * 1.25f); return true; }
    if (ch == 'f' || ch == 'F') { c_follow = true; c_look_ms = 0; c_still_since = now; return true; }
    return false;
}

bool wd3d_tap(int col, int row, tui_rect a, uint32_t now)
{
    if (col < a.x || col >= a.x + a.w || row < a.y || row >= a.y + a.h) return false;
    const int third = (col - a.x) * 3 / (a.w > 0 ? a.w : 1);
    if (third == 1) {
        /* Top and bottom of the middle zoom; its centre follows again (and orbits once more when still). */
        if (row < a.y + a.h / 4) c_dist = fmaxf(6, c_dist * 0.8f);
        else if (row >= a.y + a.h * 3 / 4) c_dist = fminf(45, c_dist * 1.25f);
        else { c_follow = true; c_look_ms = 0; c_still_since = now; }
        return true;
    }
    c_yaw += third == 0 ? -30 : 30;
    c_look_ms = now;
    return true;
}

const char *wd3d_mode(void) { return c_mode; }
void wd3d_stats(unsigned *us, unsigned *dots) { *us = st_us; *dots = st_dots; }
