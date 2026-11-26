/* WARDRIVE's 3D page (wd_view3d.h): the drive over the real map.

   The ground is MAP's own picture of the streets (ls_carto_map_pixels, the
   same worker and style source MAP uses), laid flat and seen from a camera
   that leans over it, rendered row by row: each screen row below the horizon
   meets the ground at one distance, so a row is one straight walk through
   the picture. On it: the route, coloured by how many new networks were
   found along each stretch; a pillar at the place each network was heard
   strongest, as tall as it was strong, in the colour of what it is; the
   board as an arrow. Tap a pillar: its card, and (in a replay) every place
   the log heard it, coloured by signal. Drag pans, the keys under the view
   turn, tilt and zoom, FOLLOW rides behind the board.

   Nothing moves unless the drive, the camera or the picture did: a still
   view is not redrawn. */
#include "wd_view3d.h"
#include "wd_core.h"
#include "wd_feed.h"
#include "wd_replay.h"
#include "../ls_tui.h"
#include "../ls_tui_ui.h"
#include "../ls_tui_touch.h"
#include "../ls_cartocore_map.h"
#include "../ls_trace.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#define V3_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define V3_FREE(p) heap_caps_free(p)
#else
#define EXT_RAM_BSS_ATTR
#define V3_ALLOC(n) malloc(n)
#define V3_FREE(p) free(p)
#endif

#define PX_X 5                      /* render pixels a cell, across and down: about square on this font */
#define PX_Y 8
#define TEX 512                     /* the map picture, pixels a side: about three view-lengths of ground */
#define MAX_PILLARS 4096
#define DRAWN_PILLARS 700
#define MAX_DIST 250000.0f           /* metres: a long drive's whole route (a 3 h drive spans 120 km) */
#define DEG (3.14159265f / 180.0f)

typedef struct { float x, y, z; } v3;
static v3 vadd(v3 a, v3 b) { return (v3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static v3 vsub(v3 a, v3 b) { return (v3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static v3 vmul(v3 a, float k) { return (v3){ a.x * k, a.y * k, a.z * k }; }
static float vdot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 vnorm(v3 a) { const float l = sqrtf(vdot(a, a)); return l > 1e-6f ? vmul(a, 1 / l) : a; }

/* ------------------------------------------------------------ state */

static uint16_t *s_img, *s_tex;
static int s_pw, s_ph;
static uint32_t s_img_serial, s_tex_serial;
static bool s_tex_have;
static double s_tex_lat, s_tex_lon;          /* the picture's centre */
static unsigned s_tex_z;
static float s_tex_mpp;                       /* metres a picture pixel */
static bool s_style_set;

/* Camera: looks at (tx, tz), session metres (x east, z south), from `dist`
   metres away, `pitch` degrees above the ground, facing `yaw` (0 north). */
static float c_tx, c_tz, c_yaw, c_pitch = 48, c_dist = 420;
static bool c_init, c_follow = true;
/* Where the buttons send the camera; it glides there (about a third of a
   second), so a turn, a zoom or a change of view is a move, not a jump. */
static float g_yaw, g_pitch = 48, g_dist = 420, g_tx, g_tz;
static bool g_pan;                           /* the target is a place (FIT), not the board */
static uint32_t c_frame_ms;
/* The board as drawn: its last fix carried on along its speed and heading
   until the next one, so it moves every frame instead of fix to fix. */
static float b_x, b_z;
/* Street and place names in the map picture, at its pixels. */
#define LABELS 40
typedef struct { int16_t px, py; uint8_t cls; char text[28]; } lab_t;
static EXT_RAM_BSS_ATTR lab_t s_lab[LABELS];
static EXT_RAM_BSS_ATTR ls_carto_label s_lab_raw[LABELS];
static int s_nlab;
static uint32_t s_lab_serial;
#define LAB_COLS (TEX / 4)                   /* the label grid the worker places names on */
#define LAB_ROWS (TEX / 8)
static const char *c_mode = "FOLLOW";
static unsigned st_us, st_dots;

/* The frame: basis and where it was last drawn from, to skip still frames. */
static v3 c_pos, c_f, c_r, c_u;
static float c_F, c_cx, c_cy;
static uint32_t s_sig;

/* What a tap can land on: last frame's pillar tops and bases, render pixels. */
typedef struct { int16_t bx, by, tx, ty; uint16_t dev; } hit_t;
static EXT_RAM_BSS_ATTR hit_t s_hits[DRAWN_PILLARS];
static int s_nhits;
static int s_sel = -1;                       /* device index, -1 none */
static uint8_t s_sel_addr[6], s_sel_kind;

/* SHOW: what the pillars are. */
enum { SH_ALL, SH_WIFI, SH_OPEN, SH_BT, SH_OTHER, SH_RADIO, SH_TARGET, SH_N };
static int s_show;
static const char *const SHOW_N[SH_N] = { "ALL", "WI-FI", "OPEN", "BT", "CELL+UAS", "RF+AIR+LORA", "TARGETS" };

static tui_rect s_view;                      /* the image cells */

/* Drag. */
static bool d_on;
static int d_lx, d_ly;

/* ------------------------------------------------------------ colour */

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
static uint16_t mix(uint16_t a, uint16_t b, int k256)
{
    const int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31, br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar * (256 - k256) + br * k256) >> 8) << 11) | (((ag * (256 - k256) + bg * k256) >> 8) << 5) |
                      ((ab * (256 - k256) + bb * k256) >> 8));
}

/* The map in the dark, a little lifted so the drive reads over it. */
static const uint32_t MAP_STYLE[7] = { 0x0A0F16, 0x0C2340, 0x0F2418, 0x262B33, 0x34404C, 0x5C6C7C, 0xB48C3C };
/* LIGHT MAP: MAP's PAPER chart, for the sun. */
static const uint32_t MAP_LIGHT[7] = { 0xECE9E0, 0x96BED6, 0xC4DBB2, 0xCDC6BC, 0x96928C, 0x3E3A34, 0xD6782E };
static bool s_light;
/* How far the turn keys have swung the view from the heading while following. */
static float s_yaw_off;

static uint16_t kind_rgb(int kind, int sec)
{
    if (kind == WD_WIFI) {
        static const uint32_t W[WD_SECS] = { 0x30E060, 0xF0D030, 0xF0D030, 0x40D8F0, 0x5878FF };
        const uint32_t c = W[sec >= 0 && sec < WD_SECS ? sec : 3];
        return rgb(c >> 16, (c >> 8) & 255, c & 255);
    }
    switch (kind) {
    case WD_BLE: return rgb(230, 90, 240);
    case WD_CELL: return rgb(235, 235, 235);
    case WD_RF: return rgb(255, 170, 40);      /* sub-GHz: TPMS, sensors */
    case WD_AIR: return rgb(90, 200, 255);     /* aircraft */
    case WD_LORA: return rgb(90, 255, 150);    /* LoRa nodes */
    default: return rgb(255, 70, 60);          /* drones */
    }
}
/* a target (wd_sig / wd_track said what it is): red, whatever its kind.
   PEER only says who reported it, as in HUNT and wd_track. */
#define ALERT_TAGS ((uint16_t)~(WD_T_REGULAR | WD_T_NEWHERE | WD_T_PEER))

/* finds a minute along the route: dim blue none, then cyan, yellow, red */
static uint16_t rate_rgb(float per_min)
{
    if (per_min < 0.5f) return rgb(40, 70, 130);
    if (per_min < 4) return rgb(40, 200, 230);
    if (per_min < 12) return rgb(240, 210, 50);
    return rgb(255, 80, 60);
}

/* signal along a selected network's sightings: weak blue to strong white */
static uint16_t rssi_rgb(int rssi)
{
    const float f = fmaxf(0, fminf(1, (rssi + 95) / 55.0f));
    return f < 0.33f ? rgb(60, 90, 220) : f < 0.66f ? rgb(60, 220, 230) : f < 0.85f ? rgb(250, 230, 80) : rgb(255, 255, 255);
}

/* ------------------------------------------------------------ pixels */

static void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)s_pw && (unsigned)y < (unsigned)s_ph) s_img[y * s_pw + x] = c;
}

static void line(int x0, int y0, int x1, int y1, uint16_t c, int w)
{
    /* Clipped to the picture first (Liang-Barsky). A segment that ends near
       the horizon projects thousands of pixels out, and stepping every one of
       them off screen was 25 ms of a following frame (the replay's route). */
    {
        const float fx = (float)x0, fy = (float)y0, ex = (float)(x1 - x0), ey = (float)(y1 - y0);
        const float p[4] = { -ex, ex, -ey, ey };
        const float q[4] = { fx + w, (float)(s_pw + w) - fx, fy + w, (float)(s_ph + w) - fy };
        float t0 = 0, t1 = 1;
        for (int i = 0; i < 4; i++) {
            if (p[i] == 0) { if (q[i] < 0) return; continue; }
            const float r = q[i] / p[i];
            if (p[i] < 0) { if (r > t1) return; if (r > t0) t0 = r; }
            else { if (r < t0) return; if (r < t1) t1 = r; }
        }
        if (t1 < 1) { x1 = (int)lroundf(fx + t1 * ex); y1 = (int)lroundf(fy + t1 * ey); }
        if (t0 > 0) { x0 = (int)lroundf(fx + t0 * ex); y0 = (int)lroundf(fy + t0 * ey); }
    }
    const int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 4000; guard++) {
        for (int k = 0; k < w; k++) { px(x0 + (dx > -dy ? 0 : k), y0 + (dx > -dy ? k : 0), c); }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void box(int x, int y, int r, uint16_t c)
{
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) px(x + i, y + j, c);
}

/* ------------------------------------------------------------ camera */

static void basis(void)
{
    const float cp = cosf(c_pitch * DEG), sp = sinf(c_pitch * DEG), sy = sinf(c_yaw * DEG), cy = cosf(c_yaw * DEG);
    /* forward on the ground: north is -z */
    const v3 fwd = { sy, 0, -cy };
    const v3 tgt = { c_tx, 0, c_tz };
    c_pos = vadd(vsub(tgt, vmul(fwd, c_dist * cp)), (v3){ 0, c_dist * sp, 0 });
    c_f = vnorm(vsub(tgt, c_pos));
    /* right from the heading, not from f: looking straight down f has no
       sideways part and the old cross product was zero (a frame of noise) */
    c_r = (v3){ cy, 0, sy };
    c_u = (v3){ c_r.y * c_f.z - c_r.z * c_f.y, c_r.z * c_f.x - c_r.x * c_f.z, c_r.x * c_f.y - c_r.y * c_f.x };
    c_F = s_pw * 0.95f;
    c_cx = s_pw / 2.0f;
    c_cy = s_ph * (c_pitch > 80 ? 0.5f : 0.56f);
}

static bool project(float x, float y, float z, float *sx, float *sy, float *depth)
{
    const v3 d = { x - c_pos.x, y - c_pos.y, z - c_pos.z };
    const float zc = vdot(d, c_f);
    if (zc < 1) return false;
    *sx = c_cx + c_F * vdot(d, c_r) / zc;
    *sy = c_cy - c_F * vdot(d, c_u) / zc;
    if (depth) *depth = zc;
    return true;
}

/* ------------------------------------------------------------ the ground */

static void local_of(double lat, double lon, float *x, float *z)
{
    wd_local(lat, lon, x, z);
}

/* Asks the map worker for the streets around the camera's target: the
   centre snapped to a quarter of the picture, so following the board does
   not ask for a new picture every frame. */
static void ground_request(const wd_stats_t *st)
{
    if (!st->origin) return;
    const float want_mpp = c_dist * 3.0f / TEX;   /* fine near the camera; the far rows go to fog past it */
    const double lat_t = st->lat0 - c_tz / st->m_lat;
    const double cl = cos(lat_t * 3.14159265 / 180);
    int z = (int)floor(log2(156543.03 * cl / want_mpp));
    /* never past the deepest zoom the installed maps have (the US cells stop
       at 16): CHASE asked for 18 and never got a picture */
    unsigned lo = 0, hi = 16;
    if (!ls_carto_map_limits(&lo, &hi) || hi < 9) { lo = 5; hi = 16; }
    if (lo < 3) lo = 3;
    z = z < (int)lo ? (int)lo : z > (int)hi ? (int)hi : z;
    /* Far out, the downloaded cells are the costly way to draw it: one
       picture over a whole drive opened and decoded a dozen of them at once,
       held the map worker for seconds and starved the board (the screen
       flashed blue). Their national overview draws that view in one go. */
    const unsigned cells_lo = ls_carto_map_cells_lo();
    if (cells_lo > 3 && z < (int)cells_lo + 2) z = (int)cells_lo - 1;
    /* The grid the centre snaps to is fixed per zoom: sized at the drive's
       origin, not at the camera's latitude. That latitude moves every frame
       while the board drives north or south, so a snap sized by it moved too,
       the snapped centre changed in its last bits every frame, the 0.4 s hold
       below never passed, and the picture stayed where the drive had been
       (blank ground while following; only a pause brought the map back). */
    const float mpp = (float)(156543.03 * cos(st->lat0 * 3.14159265 / 180) / (1 << z));
    /* a new picture only once the view has moved a third of this one: the
       worker renders each in a fraction of a second, but not every frame */
    const float snap = TEX * mpp / 3;
    /* centred ahead of the target, where the camera looks: behind it the
       view sees little ground, ahead it sees the most */
    const float ahead = c_pitch >= 85 ? 0 : TEX * mpp * 0.22f;
    const float ax = c_tx + sinf(c_yaw * DEG) * ahead, az = c_tz - cosf(c_yaw * DEG) * ahead;
    const float sx = roundf(ax / snap) * snap, sz = roundf(az / snap) * snap;
    double lat = st->lat0 - sz / st->m_lat, lon = st->lon0 + sx / st->m_lon;
    unsigned uz = (unsigned)z;
    /* While the camera glides (a zoom, FIT ALL, a turn) the wanted picture
       changes every frame: asking for each one kept the worker rendering
       pictures nobody would see. Ask only once the want has held still for
       0.4 s; until then keep the picture already asked for. */
    {
        static double c_lat, c_lon, r_lat, r_lon;
        static unsigned c_z, r_z;
        static uint32_t c_since;
        static bool r_have;
        const uint32_t real = (uint32_t)(esp_timer_get_time() / 1000);
        if (lat != c_lat || lon != c_lon || uz != c_z) { c_lat = lat; c_lon = lon; c_z = uz; c_since = real; }
        if (!r_have || !s_tex_have || real - c_since >= 400) { r_lat = c_lat; r_lon = c_lon; r_z = c_z; r_have = true; }
        lat = r_lat; lon = r_lon; uz = r_z;
    }
    if (!s_style_set) { ls_carto_map_pixel_style(s_light ? MAP_LIGHT : MAP_STYLE, 35); s_style_set = true; }
    double plat = lat, plon = lon;
    unsigned pz = uz;
    (void)ls_carto_map_prepare(&plat, &plon, &pz);
    double flat, flon;
    unsigned fz;
    const uint32_t was = s_tex_serial;
    if (ls_carto_map_pixels(lat, lon, uz, TEX, TEX, LAB_COLS, LAB_ROWS, true, s_tex, &s_tex_serial, &flat, &flon, &fz)) {
        if (!s_tex_have || s_tex_serial != was) {
            s_tex_have = true;
            s_tex_lat = flat; s_tex_lon = flon; s_tex_z = fz;
            s_tex_mpp = (float)(156543.03 * cos(flat * 3.14159265 / 180) / (1 << fz));
        }
    }
    /* the names that go with this picture */
    if (s_tex_have && s_lab_serial != s_tex_serial) {
        const size_t n = ls_carto_map_labels_held(s_lab_raw, LABELS, LAB_COLS, LAB_ROWS);
        s_nlab = 0;
        for (size_t i = 0; i < n; i++) {
            const ls_carto_label *l = &s_lab_raw[i];
            const int len = (int)strlen(l->text);
            if (len < 2) continue;
            lab_t *o = &s_lab[s_nlab++];
            o->px = (int16_t)(l->x * 4 + len * 2);
            o->py = (int16_t)(l->y * 8 + 4);
            o->cls = l->label_class;
            snprintf(o->text, sizeof(o->text), "%.27s", l->text);
        }
        s_lab_serial = s_tex_serial;
    }
}

/* Row by row: where each row meets the ground, walked across in a line. */
static void ground_draw(void)
{
    const uint16_t sky_top = s_light ? rgb(120, 160, 205) : rgb(2, 4, 10), sky_low = s_light ? rgb(214, 226, 238) : rgb(12, 26, 48),
                   fog = s_light ? rgb(226, 228, 226) : rgb(8, 14, 24);
    const uint16_t grid_line = s_light ? rgb(170, 175, 182) : rgb(24, 44, 76), grid_fill = s_light ? rgb(230, 228, 222) : rgb(6, 10, 16);
    float tex_x0 = 0, tex_z0 = 0;
    if (s_tex_have) local_of(s_tex_lat, s_tex_lon, &tex_x0, &tex_z0);
    const float inv = s_tex_have && s_tex_mpp > 0 ? 1.0f / s_tex_mpp : 0;
    const float far = c_dist * 4.5f;
    for (int y = 0; y < s_ph; y++) {
        uint16_t *row = s_img + (size_t)y * s_pw;
        const float vs = (c_cy - y) / c_F;
        const v3 base = vadd(c_f, vmul(c_u, vs));     /* ray at u_s = 0 */
        if (base.y >= -0.004f) {
            const uint16_t c = mix(sky_top, sky_low, (int)(255 * fminf(1, (float)y / (c_cy > 1 ? c_cy : 1))));
            for (int x = 0; x < s_pw; x++) row[x] = c;
            continue;
        }
        const float t = -c_pos.y / base.y;          /* distance along the ray to the ground */
        const v3 g0 = vadd(c_pos, vmul(base, t));
        /* across the row: G = g0 + us * t * r, us = (x - cx) / F */
        const float kx = t * c_r.x / c_F, kz = t * c_r.z / c_F;
        float gx = g0.x - c_cx * kx, gz = g0.z - c_cx * kz;
        const int fade = (int)(255 * fminf(1, fmaxf(0, (t - far * 0.45f) / (far * 0.55f))));
        if (fade >= 255) { for (int x = 0; x < s_pw; x++) row[x] = fog; continue; }   /* all fog: nothing to look up */
        if (inv > 0) {
            /* The texel steps linearly across a row: 16.16 fixed point, one add
               a pixel, where two float multiplies and two floors were half of a
               following frame. (An arithmetic shift floors, as floorf did.) */
            int64_t u = (int64_t)floorf(((gx - tex_x0) * inv + TEX / 2) * 65536.0f);
            int64_t v = (int64_t)floorf(((gz - tex_z0) * inv + TEX / 2) * 65536.0f);
            const int64_t du = (int64_t)(kx * inv * 65536.0f), dv = (int64_t)(kz * inv * 65536.0f);
            for (int x = 0; x < s_pw; x++, u += du, v += dv) {
                const int tx = (int)(u >> 16), tz = (int)(v >> 16);
                const uint16_t c = (unsigned)tx < TEX && (unsigned)tz < TEX ? s_tex[tz * TEX + tx] : fog;
                row[x] = fade ? mix(c, fog, fade) : c;
            }
            continue;
        }
        /* no map here: a 100 m grid on the ground */
        const float px_m = t / c_F * 1.5f;
        for (int x = 0; x < s_pw; x++, gx += kx, gz += kz) {
            const float mx = fabsf(fmodf(gx, 100)), mz = fabsf(fmodf(gz, 100));
            const uint16_t c = (mx < px_m || mx > 100 - px_m || mz < px_m || mz > 100 - px_m) ? grid_line : grid_fill;
            row[x] = fade ? mix(c, fog, fade) : c;
        }
    }
}

/* ------------------------------------------------------------ the drive */

static bool shown(const wd_dev_t *e)
{
    switch (s_show) {
    case SH_WIFI: return e->kind == WD_WIFI;
    case SH_OPEN: return e->kind == WD_WIFI && wd_sec_class(e->auth) == WD_SEC_OPEN;
    case SH_BT: return e->kind == WD_BLE;
    case SH_OTHER: return e->kind == WD_CELL || e->kind == WD_DRONE;
    case SH_RADIO: return e->kind == WD_RF || e->kind == WD_AIR || e->kind == WD_LORA;
    case SH_TARGET: return (e->tags & ALERT_TAGS) != 0;
    default: return true;
    }
}

/* Finds a minute around each moment of the drive, from when each device was
   first heard, in 15 s bins. */
#define BINS 1024
static EXT_RAM_BSS_ATTR uint16_t s_bins[BINS];
static uint32_t s_bin_ms;

static void rate_bins(const wd_stats_t *st)
{
    memset(s_bins, 0, sizeof(s_bins));
    const uint32_t span = (st->running ? st->scan_ms : st->stop_ms) - st->start_ms;
    s_bin_ms = 15000;
    while (span / s_bin_ms >= BINS) s_bin_ms *= 2;
    for (int i = 0; i < wd_dev_count(); i++) {
        const wd_dev_t *e = wd_dev_at(i);
        const uint32_t b = (e->first_ms - st->start_ms) / s_bin_ms;
        if (b < BINS && s_bins[b] < 65535) s_bins[b]++;
    }
}

static float rate_at(const wd_stats_t *st, uint32_t ms)
{
    const uint32_t b = (ms - st->start_ms) / s_bin_ms;
    if (b >= BINS) return 0;
    const unsigned n = s_bins[b] + (b ? s_bins[b - 1] : 0) / 2 + (b + 1 < BINS ? s_bins[b + 1] : 0) / 2;
    return n * 60000.0f / (s_bin_ms * 2);
}

/* A replay's whole route, ahead of the playback too: faint, under the
   coloured part already driven. */
static float r_px, r_py;
static bool r_have;
static void ahead_point(double lat, double lon, uint32_t secs, void *ctx)
{
    (void)secs; (void)ctx;
    float x, z, sx, sy;
    local_of(lat, lon, &x, &z);
    if (!project(x, 0.5f, z, &sx, &sy, NULL)) { r_have = false; return; }
    if (r_have) line((int)r_px, (int)r_py, (int)sx, (int)sy, rgb(110, 118, 130), 1);
    r_px = sx; r_py = sy; r_have = true;
}

/* The replay's route in local metres, made once a replay: converting every
   fix of a long drive (double maths, in software on this core) each frame was
   23 ms of a following frame. Made again when the replay or the session's
   origin changes. */
static float *s_rt;                          /* x, z a point */
static int s_rt_n, s_rt_cap;
static char s_rt_name[WD_REPLAY_NAME];
static unsigned s_rt_pts, s_rt_rows;
static double s_rt_lat0, s_rt_lon0;
static void rt_count(double lat, double lon, uint32_t secs, void *ctx) { (void)lat; (void)lon; (void)secs; (*(int *)ctx)++; }
static void rt_point(double lat, double lon, uint32_t secs, void *ctx)
{
    (void)secs; (void)ctx;
    if (s_rt_n < s_rt_cap) { local_of(lat, lon, &s_rt[2 * s_rt_n], &s_rt[2 * s_rt_n + 1]); s_rt_n++; }
}
static void rt_free(void) { V3_FREE(s_rt); s_rt = NULL; s_rt_n = s_rt_cap = 0; s_rt_name[0] = 0; }
static bool rt_ready(const wd_stats_t *st)
{
    wd_replay_info_t ri;
    wd_replay_info(&ri);
    if (s_rt && ri.points == s_rt_pts && ri.rows == s_rt_rows && !strcmp(ri.name, s_rt_name) &&
        st->lat0 == s_rt_lat0 && st->lon0 == s_rt_lon0)
        return true;
    rt_free();
    int n = 0;
    wd_replay_track(rt_count, &n);
    if (n <= 0) return false;
    s_rt = V3_ALLOC((size_t)n * 2 * sizeof(float));
    if (!s_rt) return false;
    s_rt_cap = n;
    wd_replay_track(rt_point, NULL);
    snprintf(s_rt_name, sizeof(s_rt_name), "%s", ri.name);
    s_rt_pts = ri.points; s_rt_rows = ri.rows;
    s_rt_lat0 = st->lat0; s_rt_lon0 = st->lon0;
    return true;
}

static void route_draw(const wd_stats_t *st)
{
    if (wd_replay_on()) {
        r_have = false;
        if (rt_ready(st)) {
            const uint16_t ink = rgb(110, 118, 130);
            for (int i = 0; i < s_rt_n; i++) {
                float sx, sy;
                if (!project(s_rt[2 * i], 0.5f, s_rt[2 * i + 1], &sx, &sy, NULL)) { r_have = false; continue; }
                if (r_have) line((int)r_px, (int)r_py, (int)sx, (int)sy, ink, 1);
                r_px = sx; r_py = sy; r_have = true;
            }
        } else wd_replay_track(ahead_point, NULL);
    } else if (s_rt) rt_free();
    const int nt = wd_trail_count();
    /* wider far out, with a dark edge, so the drive reads over any map */
    const int w = c_dist > 1400 ? 3 : 2;
    const uint16_t edge = rgb(0, 0, 0);
    float px0 = 0, py0 = 0;
    bool have = false;
    for (int pass = 0; pass < 2; pass++) {
        have = false;
        for (int i = 0; i < nt; i++) {
            const wd_pt_t *p = wd_trail_at(i);
            float sx, sy, d;
            if (!project(p->x, 0.5f, p->z, &sx, &sy, &d)) { have = false; continue; }
            if (have) {
                if (pass == 0) line((int)px0 - 1, (int)py0 - 1, (int)sx - 1, (int)sy - 1, edge, w + 2);
                else line((int)px0, (int)py0, (int)sx, (int)sy, rate_rgb(rate_at(st, p->ms)), d < c_dist * 1.2f ? w : w - 1);
            }
            px0 = sx; py0 = sy; have = true;
        }
    }
}

typedef struct { float d; uint16_t i; } order_t;
static EXT_RAM_BSS_ATTR order_t s_order[MAX_PILLARS];
static int by_far(const void *a, const void *b)
{
    const float x = ((const order_t *)a)->d, y = ((const order_t *)b)->d;
    return x > y ? -1 : x < y;
}

/* Where a pillar stands: where it was strongest, nudged a few metres by its
   address so a dozen heard at one stop do not stand inside each other. */
static void stand(const wd_dev_t *e, float *x, float *z)
{
    const uint32_t h = (uint32_t)(e->addr[5] | e->addr[4] << 8 | e->addr[3] << 16) * 2654435761u;
    const float a = (float)(h >> 20) * (6.2831853f / 4096.0f), r = 1.5f + (h >> 8 & 7) * 0.6f;
    *x = e->x + cosf(a) * r;
    *z = e->z + sinf(a) * r;
}

/* Far out, a pillar per network is a hedge of lines: the networks are
   counted in squares of ground instead, one bar per square as tall as its
   count, in the colour of what most of it is, with the count over the
   biggest. */
#define AGG_N 24
typedef struct { float x, z; uint16_t n, k[WD_KINDS]; uint16_t open; } agg_t;
static EXT_RAM_BSS_ATTR agg_t s_agg[AGG_N * AGG_N];
typedef struct { int16_t sx, sy; uint16_t n, open; uint8_t kind; } agg_lab_t;
#define AGG_LABELS 10
static agg_lab_t s_agg_lab[AGG_LABELS];
static int s_nagg_lab;
static bool s_agg_on;

static void columns_draw(void)
{
    memset(s_agg, 0, sizeof(s_agg));
    /* squares pinned to the ground, their size a power of two metres, so a
       pan or a small zoom does not regroup the networks (bars kept changing
       colour and height while panning) */
    const float cell = exp2f(roundf(log2f(c_dist / 9.0f)));
    const float x0 = (floorf(c_tx / cell) - AGG_N / 2) * cell, z0 = (floorf(c_tz / cell) - AGG_N / 2) * cell;
    const int n = wd_dev_count();
    for (int i = 0; i < n; i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (!(e->flags & WD_F_PLACED) || !shown(e)) continue;
        const int gx = (int)floorf((e->x - x0) / cell), gz = (int)floorf((e->z - z0) / cell);
        if (gx < 0 || gz < 0 || gx >= AGG_N || gz >= AGG_N) continue;
        agg_t *a = &s_agg[gz * AGG_N + gx];
        a->x += e->x; a->z += e->z;
        a->n++;
        a->k[e->kind & 3]++;
        if (e->kind == WD_WIFI && wd_sec_class(e->auth) == WD_SEC_OPEN) a->open++;
    }
    /* far to near: rows away from the camera first */
    order_t *o = s_order;
    int m = 0, top = 1;
    for (int i = 0; i < AGG_N * AGG_N; i++) {
        agg_t *a = &s_agg[i];
        if (!a->n) continue;
        a->x /= a->n; a->z /= a->n;
        if (a->n > top) top = a->n;
        const v3 d = { a->x - c_pos.x, -c_pos.y, a->z - c_pos.z };
        const float zc = vdot(d, c_f);
        if (zc < 1) continue;
        o[m++] = (order_t){ zc, (uint16_t)i };
    }
    qsort(o, (size_t)m, sizeof(o[0]), by_far);
    s_nagg_lab = 0;
    const float unit = c_dist * 0.2f;           /* short enough to stand upright at this distance */
    for (int k = 0; k < m; k++) {
        const agg_t *a = &s_agg[o[k].i];
        /* a fixed scale, not the biggest bar in view: a bar keeps its height
           whatever else comes into the picture (full at 60 networks) */
        const float h = unit * (0.08f + 0.92f * fminf(1, sqrtf(a->n / 60.0f)));
        float bx, by, tx, ty;
        if (!project(a->x, 0, a->z, &bx, &by, NULL) || !project(a->x, h, a->z, &tx, &ty, NULL)) continue;
        int best = 0;
        for (int j = 1; j < WD_KINDS; j++) if (a->k[j] > a->k[best]) best = j;
        const uint16_t c = best == WD_WIFI && a->open * 3 > a->n ? kind_rgb(WD_WIFI, WD_SEC_OPEN) : kind_rgb(best, WD_SEC_WPA2);
        const int w = o[k].d < c_dist * 1.3f ? 5 : 3;
        line((int)bx, (int)by, (int)tx, (int)ty, mix(c, rgb(0, 0, 0), 60), w);
        box((int)tx, (int)ty, 2, c);
        /* the biggest few get their count */
        if (a->n >= 8) {                                /* a fixed bar, a fixed rule: no flicker on a pan */
            int slot = s_nagg_lab < AGG_LABELS ? s_nagg_lab++ : -1;
            if (slot < 0) {
                int lo = 0;
                for (int j = 1; j < AGG_LABELS; j++) if (s_agg_lab[j].n < s_agg_lab[lo].n) lo = j;
                if (s_agg_lab[lo].n < a->n) slot = lo;
            }
            if (slot >= 0) s_agg_lab[slot] = (agg_lab_t){ (int16_t)tx, (int16_t)ty, a->n, a->open, (uint8_t)best };
        }
    }
    st_dots = (unsigned)m;
}

/* A marker's head, by what it is: Wi-Fi a square, Bluetooth a diamond, a
   cell a triangle, a drone a cross; r pixels from the centre. */
static void head(int x, int y, int kind, int r, uint16_t c, uint16_t rim)
{
    switch (kind) {
    case WD_BLE:
        for (int j = -r - 1; j <= r + 1; j++) for (int i = -r - 1; i <= r + 1; i++) {
            const int d = abs(i) + abs(j);
            if (d <= r + 1) px(x + i, y + j, d == r + 1 ? rim : c);
        }
        break;
    case WD_CELL:
        for (int j = 0; j <= r * 2; j++) for (int i = -j / 2; i <= j / 2; i++)
            px(x + i, y - r + j, j == r * 2 || i == -j / 2 || i == j / 2 ? rim : c);
        break;
    case WD_DRONE:
        for (int k = -r - 1; k <= r + 1; k++) { box(x + k, y + k, 0, c); box(x + k, y - k, 0, c); px(x + k, y + k + 1, rim); }
        break;
    case WD_RF:                     /* a ring: a tyre's sensor, a remote */
        for (int j = -r - 1; j <= r + 1; j++) for (int i = -r - 1; i <= r + 1; i++) {
            const int d2 = i * i + j * j, o = (r + 1) * (r + 1), n = (r - 1) * (r - 1);
            if (d2 <= o && d2 >= n) px(x + i, y + j, d2 > r * r ? rim : c);
        }
        break;
    case WD_AIR:                    /* wings and a body */
        for (int k = -r - 1; k <= r + 1; k++) { px(x + k, y, c); px(x + k, y + 1, rim); }
        for (int k = -r; k <= r; k++) { px(x, y + k, c); px(x + 1, y + k, rim); }
        for (int k = -r / 2; k <= r / 2; k++) px(x + k, y + r, c);
        break;
    case WD_LORA:                   /* a mast with two bars */
        for (int k = -r - 1; k <= r + 1; k++) { px(x, y + k, c); px(x + 1, y + k, rim); }
        for (int k = -r; k <= r; k++) { px(x + k, y - r, c); px(x + k / 2, y, c); }
        break;
    default:
        for (int j = -r - 1; j <= r + 1; j++) for (int i = -r - 1; i <= r + 1; i++)
            px(x + i, y + j, abs(i) == r + 1 || abs(j) == r + 1 ? rim : c);
    }
}

/* Names over the nearest markers: what it is, its name, its signal. */
#define TAGS 10
typedef struct { int16_t sx, sy; uint8_t kind, sec; int8_t rssi; char text[22]; } tag_t;
static EXT_RAM_BSS_ATTR tag_t s_tag[TAGS];
static int s_ntag;

/* Where each marker is drawn. A network stands where it was heard strongest,
   so while the car closes on it every stronger reading moves it to the car:
   drawn there at once, the markers near the car hopped forward with it and
   new ones popped up whole. The drawn place glides to the heard one (about a
   third of a second) and a new marker grows out of the ground (half a
   second). Only the drawing moves; the drive's data is unchanged. */
typedef struct { float x, z; uint32_t born, key; } disp_t;
static EXT_RAM_BSS_ATTR disp_t s_disp[MAX_PILLARS];
static uint32_t s_disp_session, s_disp_real;
static float s_disp_k;

static void disp_frame(void)
{
    const uint32_t real = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t session = wd_stats()->start_ms;
    if (session != s_disp_session) { memset(s_disp, 0, sizeof(s_disp)); s_disp_session = session; }
    const float dt = s_disp_real ? (float)(real - s_disp_real) / 1000.0f : 1.0f;
    s_disp_real = real;
    s_disp_k = dt >= 1.0f ? 1.0f : 1.0f - expf(-dt / 0.12f);
}
static void disp_at(int i, const wd_dev_t *e, float *x, float *z, float *grow)
{
    float tx, tz;
    stand(e, &tx, &tz);
    *grow = 1.0f;
    if (i < 0 || i >= MAX_PILLARS) { *x = tx; *z = tz; return; }
    disp_t *d = &s_disp[i];
    const uint32_t key = ((uint32_t)e->addr[2] << 24 | (uint32_t)e->addr[3] << 16 | (uint32_t)e->addr[4] << 8 | e->addr[5]) | 1u;
    if (d->key != key) { d->key = key; d->x = tx; d->z = tz; d->born = s_disp_real; }
    else {
        const float dx = tx - d->x, dz = tz - d->z;
        if (dx * dx + dz * dz > 4.0e6f) { d->x = tx; d->z = tz; }       /* over 2 km: a seek, not a glide */
        else { d->x += dx * s_disp_k; d->z += dz * s_disp_k; }
    }
    const uint32_t age = s_disp_real - d->born;
    if (age < 500) *grow = 0.15f + 0.85f * (float)age / 500.0f;
    *x = d->x; *z = d->z;
}
static float disp_grow(int i)
{
    if (i < 0 || i >= MAX_PILLARS) return 1.0f;
    const uint32_t age = s_disp_real - s_disp[i].born;
    return age < 500 ? 0.15f + 0.85f * (float)age / 500.0f : 1.0f;
}

static void pillars_draw(void)
{
    s_ntag = 0;
    s_agg_on = c_dist > 1400;
    if (s_agg_on) { s_nhits = 0; columns_draw(); return; }
    disp_frame();
    const int n = wd_dev_count();
    int m = 0;
    for (int i = 0; i < n && m < MAX_PILLARS; i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (!(e->flags & WD_F_PLACED) || (!shown(e) && i != s_sel)) continue;
        float x, z, grow;
        disp_at(i, e, &x, &z, &grow);
        (void)grow;
        const v3 d = { x - c_pos.x, -c_pos.y, z - c_pos.z };
        const float zc = vdot(d, c_f);
        if (zc < 1) continue;
        s_order[m++] = (order_t){ zc, (uint16_t)i };
    }
    /* the nearest few hundred, drawn far to near */
    if (m > DRAWN_PILLARS) {
        qsort(s_order, (size_t)m, sizeof(s_order[0]), by_far);
        memmove(s_order, s_order + (m - DRAWN_PILLARS), DRAWN_PILLARS * sizeof(s_order[0]));
        m = DRAWN_PILLARS;
    } else qsort(s_order, (size_t)m, sizeof(s_order[0]), by_far);
    s_nhits = 0;
    const float unit = c_dist * 0.36f;
    const uint16_t black = rgb(0, 0, 0);
    for (int k = 0; k < m; k++) {
        const wd_dev_t *e = wd_dev_at(s_order[k].i);
        float x, z, bx, by, tx, ty;
        const int di = s_order[k].i;
        if (di < MAX_PILLARS && s_disp[di].key) { x = s_disp[di].x; z = s_disp[di].z; }
        else stand(e, &x, &z);
        const float s = fmaxf(0, fminf(1, (e->rssi + 100) / 60.0f));
        const float h = unit * (0.12f + 0.88f * s) * disp_grow(di);
        if (!project(x, 0, z, &bx, &by, NULL) || !project(x, h, z, &tx, &ty, NULL)) continue;
        const bool sel = s_order[k].i == s_sel, near = s_order[k].d < c_dist * 1.2f;
        const int sec = e->kind == WD_WIFI ? wd_sec_class(e->auth) : 0;
        const bool target = (e->tags & ALERT_TAGS) != 0;
        const uint16_t c = sel ? rgb(255, 255, 255) : target ? rgb(255, 40, 40) : kind_rgb(e->kind, sec);
        /* where it stands: a flat ring on the ground, wider the stronger */
        {
            float rx, ry;
            const float rr = c_dist * (0.006f + 0.012f * s);
            if (project(x + rr, 0, z, &rx, &ry, NULL)) {
                const int R = (int)fmaxf(2, fabsf(rx - bx)), Ry = (int)fmaxf(1, R * sinf(c_pitch * DEG));
                for (int t = 0; t < 24; t++) {
                    const float a = t * 0.2618f;
                    px((int)(bx + cosf(a) * R), (int)(by + sinf(a) * Ry), mix(c, black, 110));
                }
                /* a target wears a second, wider ring in full red */
                if (target)
                    for (int t = 0; t < 32; t++) {
                        const float a = t * 0.19635f;
                        px((int)(bx + cosf(a) * R * 2), (int)(by + sinf(a) * Ry * 2), rgb(255, 40, 40));
                    }
            }
        }
        /* the stem, outlined so it reads against any ground */
        const int w = sel ? 4 : near ? 3 : 2;
        line((int)bx - 1, (int)by, (int)tx - 1, (int)ty, black, 1);
        line((int)bx + w, (int)by, (int)tx + w, (int)ty, black, 1);
        line((int)bx, (int)by, (int)tx, (int)ty, mix(c, black, sel ? 0 : 60), w);
        head((int)tx + w / 2, (int)ty, e->kind, sel ? 4 : near ? 3 : 2, c, sel ? rgb(255, 230, 80) : black);
        if (s_nhits < DRAWN_PILLARS)
            s_hits[s_nhits++] = (hit_t){ (int16_t)bx, (int16_t)by, (int16_t)tx, (int16_t)ty, s_order[k].i };
        /* the nearest few (drawn last) get a tag, newest list wins */
        if (near || sel) {
            tag_t *t = s_ntag < TAGS ? &s_tag[s_ntag++] : &s_tag[k % TAGS];
            t->sx = (int16_t)tx; t->sy = (int16_t)ty; t->kind = e->kind; t->sec = (uint8_t)sec; t->rssi = e->rssi;
            /* no name: the end of its address, so two of them tell apart */
            char nm[16];
            if (e->name[0]) snprintf(nm, sizeof(nm), "%.13s", e->name);
            else snprintf(nm, sizeof(nm), "%s%02x:%02x", e->kind == WD_WIFI ? "hidden " : "", e->addr[4], e->addr[5]);
            if (target) snprintf(t->text, sizeof(t->text), "! %s %s", wd_tag_word(e->tags & ALERT_TAGS), nm);
            else snprintf(t->text, sizeof(t->text), "%c %s %d", e->kind < WD_KINDS ? "WBCDRAL"[e->kind] : '?', nm, e->rssi);
        }
    }
    st_dots = (unsigned)m;
}

static void sighting(double lat, double lon, int rssi, void *ctx)
{
    (void)ctx;
    float x, z, sx, sy;
    local_of(lat, lon, &x, &z);
    if (project(x, 0.5f, z, &sx, &sy, NULL)) box((int)sx, (int)sy, 1, rssi_rgb(rssi));
}

/* The car: the same size on the screen at any zoom, a filled arrow along
   its heading inside a white ring, outlined in black so it shows over any
   map, and a short line down to where it is on the ground. */
static void board_draw(const wd_stats_t *st)
{
    if (!st->origin) return;
    const float h = (st->heading >= 0 ? st->heading : c_yaw) * DEG;
    float gx, gy, hx, hy, ux, uy;
    if (!project(b_x, 0, b_z, &gx, &gy, NULL)) return;
    /* the heading as seen on screen: project a point a little ahead */
    const float step = fmaxf(2, c_dist * 0.02f);
    if (!project(b_x + sinf(h) * step, 0, b_z - cosf(h) * step, &hx, &hy, NULL)) { hx = gx; hy = gy - 1; }
    float dx = hx - gx, dy = hy - gy;
    const float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-3f) { dx = 0; dy = -1; } else { dx /= l; dy /= l; }
    /* lift the marker a little off the ground so the arrow is not flattened */
    const float lift = c_dist * 0.04f;
    if (!project(b_x, lift, b_z, &ux, &uy, NULL)) { ux = gx; uy = gy; }
    const uint16_t white = rgb(255, 255, 255), red = rgb(255, 64, 48), black = rgb(0, 0, 0);
    line((int)gx, (int)gy, (int)ux, (int)uy, white, 1);
    const int R = 9;
    for (int t = 0; t < 48; t++) {
        const float a = t * 0.1309f;
        px((int)(ux + cosf(a) * (R + 1)), (int)(uy + sinf(a) * (R + 1)), black);
        px((int)(ux + cosf(a) * R), (int)(uy + sinf(a) * R), white);
    }
    /* the arrow: tip ahead, two corners behind, filled */
    const float tx = ux + dx * 7, ty = uy + dy * 7;
    const float lx = ux - dx * 5 - dy * 5, ly = uy - dy * 5 + dx * 5;
    const float rx = ux - dx * 5 + dy * 5, ry = uy - dy * 5 - dx * 5;
    for (float f = 0; f <= 1.0f; f += 0.08f) {
        line((int)tx, (int)ty, (int)(lx + (rx - lx) * f), (int)(ly + (ry - ly) * f), red, 1);
    }
    line((int)tx, (int)ty, (int)lx, (int)ly, white, 1);
    line((int)tx, (int)ty, (int)rx, (int)ry, white, 1);
}

/* ------------------------------------------------------------ camera follow */

static float wrap180(float d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d; }

/* A camera value for the "has the view changed" signature, rounded so a
   glide's last tiny steps do not redraw. Through a signed int: a negative
   float cast straight to uint32_t is undefined, and this core makes it 0, so
   the view did not redraw while the yaw was below zero (half the turns: the
   image froze while the names moved, then jumped) or while the board was
   west or north of where the drive began. */
static uint32_t sq(float f)
{
    if (!(f > -2e9f)) f = -2e9f;
    if (f > 2e9f) f = 2e9f;
    return (uint32_t)(int32_t)f;
}

static float glide(float cur, float want, float k) { return cur + (want - cur) * k; }

static void follow(const wd_stats_t *st, uint32_t now)
{
    /* the camera moves on the real clock: a paused replay (its clock stopped)
       froze every glide, FIT ALL included */
    const uint32_t real = (uint32_t)(esp_timer_get_time() / 1000);
    const float dt = c_frame_ms && real > c_frame_ms ? fminf(0.3f, (real - c_frame_ms) / 1000.0f) : 0;
    c_frame_ms = real;
    if (!st->origin) return;
    /* the board between fixes: on along its speed and heading, at most a
       second and a half past the last one (no further guessing than that) */
    float bx = st->x, bz = st->z;
    if (st->fix_ms && now > st->fix_ms && st->speed > 1.0f && st->heading >= 0) {
        const float ahead = fminf(1.5f, (now - st->fix_ms) / 1000.0f) * st->speed;
        bx += sinf(st->heading * DEG) * ahead;
        bz -= cosf(st->heading * DEG) * ahead;
    }
    if (!c_init) {
        c_tx = g_tx = b_x = bx; c_tz = g_tz = b_z = bz;
        c_yaw = g_yaw = st->heading >= 0 ? st->heading : 0;
        g_pitch = c_pitch; g_dist = c_dist;
        c_init = true;
    }
    /* the drawn board eases onto that, so a fix that lands a little off
       does not make it hop */
    const float kb = fminf(1, dt * 8.0f);
    b_x = glide(b_x, bx, kb); b_z = glide(b_z, bz, kb);
    if (c_follow) {
        g_pan = false;
        g_tx = b_x; g_tz = b_z;
        /* close in the view turns with the car; far out it stays north up, an overview that does not spin */
        if (st->speed > 2 && st->heading >= 0 && g_dist <= 1400) g_yaw = c_yaw + wrap180(st->heading + s_yaw_off - c_yaw);
    }
    c_mode = c_follow ? (c_pitch < 20 ? "CHASE" : "FOLLOW") : c_pitch >= 89 ? "TOP" : g_pan ? "FIT" : "FREE";
    /* everything glides toward where it was sent */
    const float k = fminf(1, dt * 6.0f), ky = fminf(1, dt * (c_follow ? 2.0f : 6.0f));
    const float kt = c_follow ? fminf(1, dt * 5.0f) : k;
    c_tx = glide(c_tx, g_tx, kt);
    c_tz = glide(c_tz, g_tz, kt);
    c_yaw = wrap180(c_yaw + wrap180(g_yaw - c_yaw) * ky);
    c_pitch = glide(c_pitch, g_pitch, k);
    c_dist = c_dist * powf(g_dist / c_dist, k);
    if (fabsf(c_dist - g_dist) < 0.5f) c_dist = g_dist;
    if (fabsf(c_pitch - g_pitch) < 0.05f) c_pitch = g_pitch;
}

/* A finger dragged on the view moves the ground under it. */
static void drag(uint32_t now)
{
    (void)now;
    int x, y, x0, y0;
    int64_t since;
    if (!ls_tui_touch_px(&x, &y, &x0, &y0, &since)) { d_on = false; return; }
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    const int col = x0 / (cw > 0 ? cw : 10), row = y0 / (ch > 0 ? ch : 17);
    if (!d_on) {
        if (col < s_view.x || col >= s_view.x + s_view.w || row < s_view.y || row >= s_view.y + s_view.h) return;
        d_on = true; d_lx = x; d_ly = y;
        return;
    }
    const int dx = x - d_lx, dy = y - d_ly;
    if (!dx && !dy) return;
    d_lx = x; d_ly = y;
    /* grid pixels to render pixels, then to metres at the target */
    const float rx = dx * (float)PX_X / cw, ry = dy * (float)PX_Y / ch;
    const float m = c_dist / c_F;
    const float sy = sinf(c_yaw * DEG), cy = cosf(c_yaw * DEG);
    const float right_x = cy, right_z = sy, fwd_x = sy, fwd_z = -cy;
    const float sp = fmaxf(0.35f, sinf(c_pitch * DEG));
    c_tx -= (rx * right_x - ry / sp * fwd_x) * m;
    c_tz -= (rx * right_z - ry / sp * fwd_z) * m;
    g_tx = c_tx; g_tz = c_tz;                  /* a finger moves the view at once */
    c_follow = false; g_pan = false;
}

/* ------------------------------------------------------------ the page */

static bool buffers(int pw, int ph)
{
    if (s_img && pw == s_pw && ph == s_ph && s_tex) return true;
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    V3_FREE(s_img); s_img = NULL;
    s_img = V3_ALLOC((size_t)pw * ph * 2);
    if (!s_tex) { s_tex = V3_ALLOC((size_t)TEX * TEX * 2); s_tex_have = false; s_tex_serial = 0; }
    s_pw = pw; s_ph = ph; s_sig = 0;
    return s_img && s_tex;
}

void wd3d_leave(void)
{
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    V3_FREE(s_img); s_img = NULL;
    V3_FREE(s_tex); s_tex = NULL;
    rt_free();
    s_tex_have = false; s_tex_serial = 0; s_style_set = false;
    ls_carto_map_leave();
    c_init = false;
    d_on = false;
}

static void put(tui_surface *sf, tui_rect a, int x, int y, const char *s, uint8_t at)
{
    if (y >= a.y && y < a.y + a.h) tui_put_str(sf, a, x, y, s, at);
}

/* The selected network's card, top left over the view. */
static void card(tui_surface *sf, tui_rect a, int sightings)
{
    if (s_sel < 0 || s_sel >= wd_dev_count()) return;
    const wd_dev_t *e = wd_dev_at(s_sel);
    if (memcmp(e->addr, s_sel_addr, 6) || e->kind != s_sel_kind) { s_sel = -1; return; }
    char l[4][56];
    snprintf(l[0], sizeof(l[0]), " %.30s ", e->name[0] ? e->name : e->kind == WD_WIFI ? "<hidden>" : "(no name)");
    snprintf(l[1], sizeof(l[1]), " %02x:%02x:%02x:%02x:%02x:%02x  %s ", e->addr[0], e->addr[1], e->addr[2], e->addr[3],
             e->addr[4], e->addr[5], e->kind == WD_WIFI ? wd_auth_short(e->auth) : e->kind == WD_BLE ? "BT" : e->kind == WD_CELL ? "LTE" : "UAS");
    snprintf(l[2], sizeof(l[2]), " best %d dBm  heard %lux%s ", e->rssi, (unsigned long)e->seen,
             e->kind == WD_WIFI && e->channel ? "" : "");
    if (sightings > 0) snprintf(l[3], sizeof(l[3]), " %d places in the log  (dots) ", sightings);
    else snprintf(l[3], sizeof(l[3]), " %.5f, %.5f ", e->lat, e->lon);
    const uint8_t hue = e->kind == WD_WIFI ? TUI_GREEN : e->kind == WD_BLE ? TUI_MAGENTA : TUI_WHITE;
    put(sf, a, a.x, a.y + 1, l[0], TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT));
    for (int i = 1; i < 4; i++) put(sf, a, a.x, a.y + 1 + i, l[i], TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

/* The map's names where they lie on the ground, near ones first, none on
   top of another; drawn as glass so the map shows round the letters. */
static void names(tui_surface *sf, tui_rect a)
{
    static EXT_RAM_BSS_ATTR uint8_t used[96][48];
    memset(used, 0, sizeof(used));
    /* the markers' tags first: they are what the view is for */
    static const uint8_t TAG_HUE[WD_KINDS] = { TUI_GREEN, TUI_MAGENTA, TUI_WHITE, TUI_RED, TUI_YELLOW, TUI_CYAN, TUI_GREEN };
    for (int i = s_ntag - 1; i >= 0; i--) {
        const tag_t *t = &s_tag[i];
        const int len = (int)strlen(t->text);
        const int row = a.y + t->sy / PX_Y - 1, col = a.x + t->sx / PX_X + 1;
        if (row < a.y + 1 || row >= a.y + a.h - 1 || col < a.x || col + len > a.x + a.w) continue;
        const int rr = row - a.y, cc = col - a.x;
        if (rr >= 48 || cc + len > 96) continue;
        bool clash = false;
        for (int k = -1; k <= len && !clash; k++) if (cc + k >= 0 && cc + k < 96 && used[cc + k][rr]) clash = true;
        if (clash) continue;
        for (int k = 0; k < len; k++) used[cc + k][rr] = 1;
        uint8_t hue = TAG_HUE[t->kind < WD_KINDS ? t->kind : 0];
        if (t->text[0] == '!') hue = TUI_RED;          /* a target */
        else if (t->kind == WD_WIFI) hue = t->sec == WD_SEC_OPEN ? TUI_GREEN : t->sec <= WD_SEC_WPA ? TUI_YELLOW : t->sec == WD_SEC_WPA3 ? TUI_BLUE : TUI_CYAN;
        ls_tui_put_glass(sf, a, col, row, t->text[0], TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT));
        for (int k = 1; k < len; k++) ls_tui_put_glass(sf, a, col + k, row, t->text[k], TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
    }
    if (!s_tex_have) return;
    float tx0, tz0;
    local_of(s_tex_lat, s_tex_lon, &tx0, &tz0);
    int drawn = 0;
    for (int pass = 0; pass < 2 && drawn < 14; pass++)
        for (int i = 0; i < s_nlab && drawn < 14; i++) {
            const lab_t *l = &s_lab[i];
            if ((pass == 0) != (l->cls <= 2)) continue;          /* places first, then the rest */
            const float gx = tx0 + (l->px - TEX / 2) * s_tex_mpp, gz = tz0 + (l->py - TEX / 2) * s_tex_mpp;
            float sx, sy, d;
            if (!project(gx, 0, gz, &sx, &sy, &d) || d > c_dist * 3.2f) continue;
            const int len = (int)strlen(l->text);
            const int row = a.y + (int)(sy / PX_Y), col = a.x + (int)(sx / PX_X) - len / 2;
            if (row < a.y + 1 || row >= a.y + a.h - 1 || col < a.x || col + len > a.x + a.w) continue;
            const int rr = row - a.y, cc = col - a.x;
            if (rr >= 48 || cc + len > 96) continue;
            bool clash = false;
            for (int k = -1; k <= len && !clash; k++)
                if (cc + k >= 0 && cc + k < 96 && (used[cc + k][rr] || (rr > 0 && used[cc + k][rr - 1]) || used[cc + k][rr + 1 < 48 ? rr + 1 : rr]))
                    clash = true;
            if (clash) continue;
            for (int k = 0; k < len; k++) used[cc + k][rr] = 1;
            /* names in ink on the light map, in white on the dark one */
            const uint8_t at = s_light ? TUI_ATTR(TUI_BLACK, TUI_BLACK)
                             : l->cls <= 2 ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : TUI_ATTR(TUI_WHITE, TUI_BLACK);
            for (int k = 0; k < len; k++) ls_tui_put_glass(sf, a, col + k, row, (int16_t)(unsigned char)l->text[k], at);
            drawn++;
        }
    /* the column counts when the view is far out */
    if (s_agg_on)
        for (int i = 0; i < s_nagg_lab; i++) {
            static const char *const KN[WD_KINDS] = { "WiFi", "BT", "LTE", "UAS" };
            char t[24];
            if (s_agg_lab[i].kind == WD_WIFI && s_agg_lab[i].open)
                snprintf(t, sizeof(t), "%u WiFi %u open", s_agg_lab[i].n, s_agg_lab[i].open);
            else snprintf(t, sizeof(t), "%u %s", s_agg_lab[i].n, KN[s_agg_lab[i].kind & 3]);
            const int len = (int)strlen(t);
            const int row = a.y + s_agg_lab[i].sy / PX_Y - 1, col = a.x + s_agg_lab[i].sx / PX_X - len / 2;
            if (row < a.y + 1 || row >= a.y + a.h - 1 || col < a.x || col + len > a.x + a.w) continue;
            const uint8_t hue = s_agg_lab[i].kind == WD_BLE ? TUI_MAGENTA : s_agg_lab[i].kind == WD_WIFI ? TUI_GREEN : TUI_WHITE;
            for (int k = 0; k < len; k++) ls_tui_put_glass(sf, a, col + k, row, t[k], TUI_ATTR(s_light ? (hue == TUI_WHITE ? TUI_BLACK : hue) : hue | TUI_BRIGHT, TUI_BLACK));
        }
}

static void legend(tui_surface *sf, tui_rect a)
{
    /* the route's colours, bottom left over the view */
    const int y = a.y + a.h - 1;
    put(sf, a, a.x, y, " new/min ", TUI_ATTR(TUI_WHITE, TUI_BLACK));
    put(sf, a, a.x + 9, y, "0", TUI_ATTR(TUI_BLUE, TUI_BLACK));
    put(sf, a, a.x + 11, y, "1-3", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    put(sf, a, a.x + 15, y, "4-11", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    put(sf, a, a.x + 20, y, "12+ ", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
}

bool wd3d_draw(tui_surface *sf, tui_rect a, uint32_t now)
{
    if (a.w < 16 || a.h < 6) return false;
    s_view = a;
    if (!buffers(s_view.w * PX_X, s_view.h * PX_Y)) return false;
    wd_lock();
    const wd_stats_t st = *wd_stats();
    wd_unlock();
    follow(&st, now);
    drag(now);
    if (c_pitch > 89.5f) c_pitch = 89.5f;      /* straight down, as near as the maths allows */
    basis();
    ground_request(&st);
    /* a still view is not redrawn */
    uint32_t sig = sq(c_tx * 7) ^ sq(c_tz * 13) << 3 ^ sq(c_yaw * 101) << 7 ^ sq(c_pitch * 3) << 11 ^
                   sq(c_dist) << 13 ^ s_tex_serial * 2654435761u ^ (uint32_t)wd_dev_count() << 17 ^
                   (uint32_t)wd_trail_count() * 40503u ^ (uint32_t)(s_sel + 2) << 21 ^ (uint32_t)s_show << 27 ^ sq(b_x * 4) ^ sq(b_z * 4) << 5;
    static int sightings;
    /* where a slow frame's time went, in 'carto trace' (only slow ones) */
    const int64_t t0 = esp_timer_get_time();
    int64_t t_ground = 0, t_bins = 0, t_route = 0, t_marks = 0;
    if (sig != s_sig) {
        s_sig = sig;
        ground_draw();
        t_ground = esp_timer_get_time();
        wd_lock();
        rate_bins(&st);
        t_bins = esp_timer_get_time();
        route_draw(&st);
        t_route = esp_timer_get_time();
        pillars_draw();
        sightings = s_sel >= 0 && wd_replay_on() ? wd_replay_sightings(s_sel_addr, s_sel_kind, sighting, NULL) : 0;
        board_draw(&st);
        wd_unlock();
        t_marks = esp_timer_get_time();
        s_img_serial++;
    }
    for (int y = 0; y < s_view.h; y++)
        for (int x = 0; x < s_view.w; x++) tui_put_char(sf, s_view, s_view.x + x, s_view.y + y, LS_TUI_IMAGE_CELL, 0);
    ls_tui_image(s_view, s_img, s_pw, s_ph, s_img_serial);
    names(sf, s_view);
    card(sf, s_view, sightings);
    legend(sf, s_view);
    const int64_t t_end = esp_timer_get_time();
    st_us = (unsigned)(t_end - t0);
    if (t_end - t0 > 30000 && t_ground) {
        ls_map_trace("3d ground|bins", (int32_t)((t_ground - t0) / 1000), (int32_t)((t_bins - t_ground) / 1000));
        ls_map_trace("3d route|marks", (int32_t)((t_route - t_bins) / 1000), (int32_t)((t_marks - t_route) / 1000));
        ls_map_trace("3d text|trail", (int32_t)((t_end - t_marks) / 1000), wd_trail_count());
    }
    return true;
}

static void sel_none(void) { s_sel = -1; s_sig = 0; }

/* The whole drive in view: the route and every placed network, from above
   at a slant, north up. */
static float f_lx, f_hx, f_lz, f_hz;
static void fit_point(double lat, double lon, uint32_t secs, void *ctx)
{
    (void)secs; (void)ctx;
    float x, z;
    local_of(lat, lon, &x, &z);
    f_lx = fminf(f_lx, x); f_hx = fmaxf(f_hx, x); f_lz = fminf(f_lz, z); f_hz = fmaxf(f_hz, z);
}

static void fit_all(void)
{
    /* a replay: the whole drive, the part still to come included */
    f_lx = f_lz = 1e30f; f_hx = f_hz = -1e30f;
    if (wd_replay_on()) wd_replay_track(fit_point, NULL);
    float lx = f_lx, hx = f_hx, lz = f_lz, hz = f_hz;
    wd_lock();
    for (int i = 0; i < wd_trail_count(); i++) {
        const wd_pt_t *p = wd_trail_at(i);
        lx = fminf(lx, p->x); hx = fmaxf(hx, p->x); lz = fminf(lz, p->z); hz = fmaxf(hz, p->z);
    }
    for (int i = 0; i < wd_dev_count(); i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (!(e->flags & WD_F_PLACED)) continue;
        lx = fminf(lx, e->x); hx = fmaxf(hx, e->x); lz = fminf(lz, e->z); hz = fmaxf(hz, e->z);
    }
    wd_unlock();
    if (lx > hx) return;
    /* the view's width at the target is about the distance (F ~ 0.95 width);
       its depth is longer, foreshortened by the slant */
    /* at a 62 degree slant the ground in view from the target's row to the
       edges is about 0.55 of the distance either way north-south (the far
       half foreshortened), the width about 0.5 each side */
    const float need_w = (hx - lx) * 1.15f, need_h = (hz - lz) * 1.25f;
    const float span = fmaxf(fmaxf(need_w, need_h), 250.0f);
    c_follow = false; g_pan = true;
    g_tx = (lx + hx) / 2; g_tz = (lz + hz) / 2;
    g_yaw = c_yaw + wrap180(0 - c_yaw);
    g_pitch = 62;
    g_dist = fminf(MAX_DIST, span);
}

void wd3d_act(int k)
{
    switch (k) {
    /* the view turns the way the arrow points: < swings the ground to the left */
    /* A turn keeps following: the camera stays on the board, swung that far
       from its heading (a turn used to drop FOLLOW, and the heading undid it). */
    case WD3D_LEFT: g_yaw += 30; s_yaw_off = wrap180(s_yaw_off + 30); break;
    case WD3D_RIGHT: g_yaw -= 30; s_yaw_off = wrap180(s_yaw_off - 30); break;
    case WD3D_TILT: g_pitch = g_pitch < 30 ? 48 : g_pitch < 60 ? 70 : g_pitch < 89 ? 89.5f : 25; break;
    case WD3D_UP: g_pitch = fminf(89.5f, g_pitch + 12); break;          /* toward straight down */
    case WD3D_DOWN: g_pitch = fmaxf(12, g_pitch - 12); break;           /* toward the horizon */
    case WD3D_LIGHT: s_light = !s_light; s_style_set = false; break;    /* the map comes back in the other palette */
    case WD3D_CHASE: g_pitch = 16; g_dist = 110; c_follow = true; s_yaw_off = 0; break;   /* low and close, behind the board */
    case WD3D_IN: g_dist = fmaxf(60, g_dist * 0.7f); break;
    case WD3D_OUT: g_dist = fminf(MAX_DIST, g_dist * 1.43f); break;
    case WD3D_FOLLOW: c_follow = !c_follow; if (!c_follow) { g_tx = c_tx; g_tz = c_tz; } break;
    case WD3D_FIT: fit_all(); break;
    case WD3D_SHOW: s_show = (s_show + 1) % SH_N; break;
    }
    s_sig = 0;
}

bool wd3d_key(ls_tk_t k, char ch, uint32_t now)
{
    if (k == LS_TK_LEFT) { wd3d_act(WD3D_LEFT); return true; }
    if (k == LS_TK_RIGHT) { wd3d_act(WD3D_RIGHT); return true; }
    if (k == LS_TK_UP) { g_pitch = fminf(89.5f, g_pitch + 8); s_sig = 0; return true; }
    if (k == LS_TK_DOWN) { g_pitch = fmaxf(12, g_pitch - 8); s_sig = 0; return true; }
    if (ch == '+' || ch == '=') { wd3d_act(WD3D_IN); return true; }
    if (ch == '-' || ch == '_') { wd3d_act(WD3D_OUT); return true; }
    if (ch == 'f' || ch == 'F') { wd3d_act(WD3D_FOLLOW); return true; }
    if (ch == 't' || ch == 'T') { wd3d_act(WD3D_TILT); return true; }
    if (ch == 'v' || ch == 'V') { wd3d_act(WD3D_SHOW); return true; }
    if (k == LS_TK_ESC && s_sel >= 0) { sel_none(); return true; }
    return false;
}

bool wd3d_tap(int col, int row, tui_rect a, uint32_t now)
{
    (void)a;
    if (col < s_view.x || col >= s_view.x + s_view.w || row < s_view.y || row >= s_view.y + s_view.h) return false;
    /* the pillar nearest the tap: its top or anywhere down its line */
    const int x = (col - s_view.x) * PX_X + PX_X / 2, y = (row - s_view.y) * PX_Y + PX_Y / 2;
    int best = -1, bd = 14 * 14;
    for (int i = s_nhits - 1; i >= 0; i--) {             /* nearest drawn last */
        const hit_t *h = &s_hits[i];
        const int ty = h->ty < h->by ? h->ty : h->by, by = h->ty < h->by ? h->by : h->ty;
        const int cy = y < ty ? ty : y > by ? by : y;
        const int dx = x - h->tx, dy = y - cy, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = h->dev; }
    }
    if (best < 0) { if (s_sel >= 0) { sel_none(); return true; } return false; }
    s_sel = best;
    const wd_dev_t *e = wd_dev_at(best);
    memcpy(s_sel_addr, e->addr, 6);
    s_sel_kind = e->kind;
    s_sig = 0;
    return true;
}

const char *wd3d_mode(void) { return c_mode; }

const char *wd3d_label(int k, char *b, size_t n)
{
    switch (k) {
    case WD3D_LEFT: return "< TURN";
    case WD3D_RIGHT: return "TURN >";
    case WD3D_TILT:
        if (c_pitch >= 89) snprintf(b, n, "TOP");
        else snprintf(b, n, "TILT %.0f", (double)c_pitch);
        return b;
    case WD3D_IN: return "ZOOM +";
    case WD3D_OUT: return "ZOOM -";
    case WD3D_FOLLOW: return c_follow ? "FOLLOW" : "FREE";
    case WD3D_CHASE: return "CHASE";
    case WD3D_FIT: return "FIT ALL";
    case WD3D_SHOW: snprintf(b, n, "%s", SHOW_N[s_show]); return b;
    case WD3D_UP: return "TILT ^";
    case WD3D_DOWN: return "TILT v";
    case WD3D_LIGHT: return "LIGHT MAP";
    default: return "";
    }
}

bool wd3d_lit(int k)
{
    return (k == WD3D_FOLLOW && c_follow) || (k == WD3D_TILT && g_pitch >= 89) ||
           (k == WD3D_CHASE && g_pitch < 20 && c_follow) || (k == WD3D_SHOW && s_show != SH_ALL) ||
           (k == WD3D_FIT && g_pan && !c_follow) || (k == WD3D_LIGHT && s_light);
}
void wd3d_stats(unsigned *us, unsigned *dots) { *us = st_us; *dots = st_dots; }
