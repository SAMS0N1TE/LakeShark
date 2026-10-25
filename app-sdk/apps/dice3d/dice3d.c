/* DICE 3D for LakeShark: DICE drawn by lap3d, the firmware's 3D engine,
   instead of its own ray caster. The physics, sounds, readout and buttons
   are DICE's own, so the two can be run side by side. Tilt the board and they slide and tumble; shake it, tap
   ROLL or fling a finger across the tray to throw them. The face that ends
   up on top is the face that counts: physics, not a random number painted on.
   Uses API 1.2 (lap3d) and 1.1: lap_motion, lap_touch, lap_swipe, lap_tone,
   lap_haptic. */
#include "lakeshark_app.h"
#include "lap3d.h"
#include "lap_canvas.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAXD 5
#define PI 3.14159265f

typedef struct { float x, y, z; } v3;
static inline v3 V(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static inline v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 mul(v3 a, float k) { return V(a.x * k, a.y * k, a.z * k); }
static inline float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float len(v3 a) { return sqrtf(dot(a, a)); }

typedef struct {
    v3 p, v, w;                 /* centre, velocity, spin (world, rad/s) */
    v3 ax[3];                   /* the die's own axes in the world: its rotation */
    bool still;                 /* lying flat and not moving */
    int top;                    /* the face up, once still */
} die_t;

static lap_canvas_t cv;
static die_t dice[MAXD];
static int ndice = 2;
static uint32_t last_ms, result_ms, throw_ms, shake_ms;
static uint32_t kick_ms, thud_ms;   /* the last throw and the last hard landing: the screen jolts */
static bool rolling, have_result, tilt_live;
static int total, history[8], nhist;
static float tilt_x, tilt_y;    /* in-plane pull from the sensor, world units/s^2 */

/* The tray, in world units: the floor is |x|<WX, |y|<WY at z=0, the rim is
   RIM wide and HW high. The die's half size is H. */
static float WX = 1.0f, WY = 1.3f;
static const float RIM = 0.10f, HW = 0.12f, H = 0.20f;
/* Felt: what it takes to start a resting die, and the drag on a moving one. */
static const float MU_STATIC = 0.36f, MU_SLIDE = 0.28f, MU_TUMBLE = 0.6f;
static const float ELEV = 58.0f * PI / 180.0f;
static float sin_e, cos_e;

/* Layout in dots. */
static int tray_x0, tray_y0, tray_x1, tray_y1, hud_w, hud_h, bar_x, bar_y;
static float S, tcx, tcy;       /* dots per world unit, tray centre in dots */

static const v3 LIGHT = { -0.45f, 0.35f, 0.82f };
static v3 light;

/* ---- the dice ----------------------------------------------------------- */

/* Faces: axis i positive, then negative. Opposite faces add to seven. */
static const int FACE[3][2] = { { 1, 6 }, { 2, 5 }, { 3, 4 } };

static void orthonormal(die_t *d)
{
    d->ax[0] = mul(d->ax[0], 1.0f / len(d->ax[0]));
    d->ax[1] = add(d->ax[1], mul(d->ax[0], -dot(d->ax[0], d->ax[1])));
    d->ax[1] = mul(d->ax[1], 1.0f / len(d->ax[1]));
    d->ax[2] = cross(d->ax[0], d->ax[1]);
}

static void rotate(die_t *d, v3 w, float dt)
{
    const float a = len(w) * dt;
    if (a < 1e-6f) return;
    const v3 k = mul(w, 1.0f / len(w));
    const float c = cosf(a), s = sinf(a);
    for (int i = 0; i < 3; i++) {
        const v3 v = d->ax[i];
        d->ax[i] = add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1 - c)));
    }
    orthonormal(d);
}

/* How high the centre sits when the die rests on its lowest corner. */
static float rest_height(const die_t *d)
{
    return H * (fabsf(d->ax[0].z) + fabsf(d->ax[1].z) + fabsf(d->ax[2].z));
}

static int up_face(const die_t *d, int *axis, int *sign)
{
    int best = 0;
    for (int i = 1; i < 3; i++) if (fabsf(d->ax[i].z) > fabsf(d->ax[best].z)) best = i;
    *axis = best;
    *sign = d->ax[best].z > 0 ? 1 : -1;
    return FACE[best][*sign > 0 ? 0 : 1];
}

static void place(int i)
{
    die_t *d = &dice[i];
    memset(d, 0, sizeof(*d));
    d->ax[0] = V(1, 0, 0); d->ax[1] = V(0, 1, 0); d->ax[2] = V(0, 0, 1);
    rotate(d, V(0, 0, 1), lc_frand() * 2 * PI);
    d->p = V((i - (ndice - 1) / 2.0f) * H * 2.6f, 0, H);
    d->still = true;
    int a, s;
    d->top = up_face(d, &a, &s);
}

static void click(float speed)
{
    if (speed < 0.5f) return;
    int v = (int)(speed * 18);
    lap_tone(0, 1, 0, LAP_WAVE_SINE);
    lap_tone(900 + (int)(lap_random() % 500), 12, v > 60 ? 60 : v, LAP_WAVE_NOISE);
    if (speed > 3.0f) {
        /* A die coming down hard: a thud you feel, and the glass jumps. */
        const uint32_t now = lap_millis();
        if (now - thud_ms > 120) {
            thud_ms = now ? now : 1;
            lap_tone(70 + (int)(lap_random() % 30), 30, 70, LAP_WAVE_SQUARE);
            lap_vibrate(30, 80);
        }
    } else if (speed > 1.5f)
        lap_haptic(LAP_HAPTIC_TICK);
}

/* Throw: up, along `dir` (or anywhere), spinning hard. */
static void throw_dice(float dx, float dy)
{
    const uint32_t now = lap_millis();
    if (throw_ms && now - throw_ms < 350) return;
    throw_ms = now ? now : 1;
    for (int i = 0; i < ndice; i++) {
        die_t *d = &dice[i];
        const float a = lc_frand() * 2 * PI, sp = 2.4f + lc_frand() * 2.0f;
        d->v = V(cosf(a) * sp + dx * 4.0f, sinf(a) * sp + dy * 4.0f, 3.6f + lc_frand() * 1.8f);
        d->w = V((lc_frand() - .5f) * 60, (lc_frand() - .5f) * 60, (lc_frand() - .5f) * 24);
        d->still = false;
    }
    rolling = true;
    have_result = false;
    kick_ms = throw_ms;
    /* The throw: a low thump, then the dice rattling out of the hand. */
    lap_tone(0, 1, 0, LAP_WAVE_SINE);
    lap_tone(110, 35, 85, LAP_WAVE_SQUARE);
    lap_tone(70, 45, 70, LAP_WAVE_SQUARE);
    for (int k = 0; k < 6; k++) {
        lap_tone(600 + (int)(lap_random() % 900), 10, 60 - k * 6, LAP_WAVE_NOISE);
        lap_tone(0, 12 + k * 5, 0, LAP_WAVE_SINE);
    }
    lap_vibrate(80, 100);
}

/* The jolt: where the whole picture sits this frame, in cells. */
static void jolt(int *ox, int *oy)
{
    static const signed char KICK[][2] = { { 0, 1 }, { -1, 0 }, { 1, -1 }, { -1, 1 }, { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 0 } };
    static const signed char THUD[][2] = { { 0, 1 }, { 0, -1 }, { 0, 0 } };
    const uint32_t now = lap_millis();
    *ox = *oy = 0;
    if (kick_ms && now - kick_ms < 8 * 35) {
        const int k = (int)((now - kick_ms) / 35);
        *ox = KICK[k][0]; *oy = KICK[k][1];
    } else if (thud_ms && now - thud_ms < 3 * 30) {
        const int k = (int)((now - thud_ms) / 30);
        *ox = THUD[k][0]; *oy = THUD[k][1];
    }
}

static void step(die_t *d, float dt)
{
    const float g = 9.0f;
    const float zr = rest_height(d);
    const bool floor = d->p.z <= zr + 0.004f;
    /* Gravity, and the pull along the glass when the board is tilted. */
    d->v.z -= g * dt;
    if (floor) {
        /* Felt friction, the way a real die feels it: at rest it holds until
           the board is tipped about 20 degrees; once moving it is slowed by a
           steady drag that a gentle tilt cannot beat, so it comes to a stop. */
        const float pull = sqrtf(tilt_x * tilt_x + tilt_y * tilt_y);
        float sp = sqrtf(d->v.x * d->v.x + d->v.y * d->v.y);
        if (sp < 0.05f && pull < MU_STATIC * g) {
            d->v.x = d->v.y = 0;
        } else {
            d->v.x += tilt_x * dt;
            d->v.y += tilt_y * dt;
            sp = sqrtf(d->v.x * d->v.x + d->v.y * d->v.y);
            /* A die on its edge or corner digs in harder than one sliding flat. */
            float drag = (d->still ? MU_SLIDE : MU_TUMBLE) * g * dt;
            if (!d->still && lap_millis() - throw_ms > 3000) drag *= 3;   /* every throw ends */
            if (sp <= drag) d->v.x = d->v.y = 0;
            else { d->v.x *= (sp - drag) / sp; d->v.y *= (sp - drag) / sp; }
        }
        /* Fast, it rolls edge over edge; nudged, it just slides on its face. */
        if (!d->still && sp > 1.0f) {
            const v3 roll = V(d->v.y / H, -d->v.x / H, d->w.z * 0.9f);
            const float k = fminf(1.0f, 9.0f * dt);
            d->w = add(mul(d->w, 1 - k), mul(roll, k * 0.85f));
        } else if (d->still)
            d->w = V(0, 0, 0);
    }
    /* Airborne or spinning: no longer lying on a face. */
    if (d->p.z > H + 0.03f || len(d->w) > 2.5f) d->still = false;
    rotate(d, d->w, dt);
    d->p = add(d->p, mul(d->v, dt));

    /* The floor: bounce, lose height, keep a little spin. */
    const float zr2 = rest_height(d);
    if (d->p.z < zr2) {
        d->p.z = zr2;
        if (d->v.z < -0.6f) {
            click(-d->v.z);
            d->w = add(mul(d->w, 0.75f), V((lc_frand() - .5f) * 6, (lc_frand() - .5f) * 6, 0));
            d->v.z = -d->v.z * 0.38f;
        } else
            d->v.z = 0;
    }
    /* The walls of the tray. */
    const float r = H * 1.25f;
    if (d->p.x < -WX + r) { d->p.x = -WX + r; if (d->v.x < 0) { click(-d->v.x); d->v.x = -d->v.x * 0.5f; d->w.y += d->v.x * 4; } }
    if (d->p.x > WX - r)  { d->p.x = WX - r;  if (d->v.x > 0) { click(d->v.x);  d->v.x = -d->v.x * 0.5f; d->w.y += d->v.x * 4; } }
    if (d->p.y < -WY + r) { d->p.y = -WY + r; if (d->v.y < 0) { click(-d->v.y); d->v.y = -d->v.y * 0.5f; d->w.x -= d->v.y * 4; } }
    if (d->p.y > WY - r)  { d->p.y = WY - r;  if (d->v.y > 0) { click(d->v.y);  d->v.y = -d->v.y * 0.5f; d->w.x -= d->v.y * 4; } }

    /* Settling: slow and on the felt, fall onto the nearest face. */
    const float sp = sqrtf(d->v.x * d->v.x + d->v.y * d->v.y);
    if (!d->still && d->p.z <= zr2 + 0.01f && sp < 0.6f && fabsf(d->v.z) < 0.3f) {
        int a, s;
        up_face(d, &a, &s);
        const v3 want = V(0, 0, (float)s);
        const v3 axis = cross(d->ax[a], want);
        const float sn = len(axis);
        if (sn < 0.004f && len(d->w) < 1.2f) {
            /* Square it up exactly and call it. */
            const float yaw = atan2f(d->ax[(a + 1) % 3].y, d->ax[(a + 1) % 3].x);
            v3 u = V(cosf(yaw), sinf(yaw), 0);
            d->ax[a] = want;
            d->ax[(a + 1) % 3] = u;
            d->ax[(a + 2) % 3] = cross(d->ax[a], u);
            d->w = V(0, 0, 0);
            d->p.z = H;
            d->still = true;
            d->top = FACE[a][s > 0 ? 0 : 1];
        } else {
            /* Topple toward flat: a spin that closes the angle, damped. */
            const v3 k = sn > 1e-6f ? mul(axis, 1.0f / sn) : V(0, 0, 0);
            const float ang = asinf(fminf(1.0f, sn));
            d->w = add(mul(d->w, 0.6f), mul(k, ang * 14.0f));
        }
    }
}

static void collide(die_t *a, die_t *b)
{
    const float dx = b->p.x - a->p.x, dy = b->p.y - a->p.y, dist = sqrtf(dx * dx + dy * dy), min = H * 2.3f;
    if (dist >= min || dist < 1e-4f) return;
    const float nx = dx / dist, ny = dy / dist, push = (min - dist) / 2;
    a->p.x -= nx * push; a->p.y -= ny * push;
    b->p.x += nx * push; b->p.y += ny * push;
    const float rel = (b->v.x - a->v.x) * nx + (b->v.y - a->v.y) * ny;
    if (rel < 0) {
        const float j = -rel * 0.8f;
        a->v.x -= nx * j; a->v.y -= ny * j;
        b->v.x += nx * j; b->v.y += ny * j;
        a->w.z += (lc_frand() - .5f) * 8; b->w.z += (lc_frand() - .5f) * 8;
        a->still = b->still = false;
        click(-rel);
    }
}

/* ---- the picture --------------------------------------------------------- */

/* A dot's height over its width (cells are 2 x 3 dots). */
static float dot_aspect(void)
{
    int cw = 10, ch = 17;
    lap_cell_px(&cw, &ch);
    return (ch / 3.0f) / (cw / 2.0f);
}

/* DICE's world has z up and y away from you; lap3d has y up and z toward
   you. One swap carries every point and axis across. */
static inline l3_vec E(v3 a) { return l3v(a.x, a.z, -a.y); }

static l3_scene *sc;
static int scene_rows;                  /* cells: the tray, aligned to the grid */
static int m_felt, m_wood, m_ivory, m_pip, m_ace;
static int seen_n = -1;
static float seen[MAXD][12];

static const uint8_t IVORY[4] = { LC_GREY, LC_WHITE, LC_BR_WHITE, LC_BR_WHITE };
static const uint8_t FELT[4] = { LC_BLACK, LC_GREEN, LC_GREEN, LC_BR_GREEN };
static const uint8_t PIPS[2] = { LC_BLACK, LC_GREY };
static const uint8_t ACE[2] = { LC_RED, LC_BR_RED };

/* The six faces side by side, 7 x 7 texels each; the material is L3_ROUND,
   so each pip texel draws as a round dot about a quarter of the face across, in the order lap3d gives a
   box: +x, -x, +y, -y, +z, -z. A die's own axes are DICE's ax[0..2], whose
   faces read 1/6, 2/5 and 3/4. 'o' is a pip, 'r' the red ace. */
#define TW 7
static void pip_texture(void)
{
    static const signed char at[7][6][2] = {
        { { 0 } }, { { 0, 0 } }, { { -1, -1 }, { 1, 1 } }, { { -1, -1 }, { 0, 0 }, { 1, 1 } },
        { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } },
        { { -1, -1 }, { 1, -1 }, { 0, 0 }, { -1, 1 }, { 1, 1 } },
        { { -1, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 1, 1 } },
    };
    static const int order[6] = { 1, 6, 2, 5, 3, 4 };
    static uint8_t tex[6 * TW * TW];
    memset(tex, 0xFF, sizeof(tex));
    for (int f = 0; f < 6; f++) {
        const int n = order[f];
        for (int i = 0; i < n; i++) {
            const int cx = 3 + at[n][i][0] * 2, cy = 3 + at[n][i][1] * 2;
            tex[cy * 6 * TW + f * TW + cx] = (uint8_t)(n == 1 ? m_ace : m_pip);
        }
    }
    const int pips = l3_texture(sc, 6 * TW, TW, tex);
    l3_texture_dot(sc, pips, 0.85f);              /* a pip a quarter of the face, as DICE draws it */
    l3_material_texture(sc, m_ivory, pips);
}

static void scene_setup(void)
{
    m_felt  = l3_material(sc, FELT, 4, 0, L3_GRAIN);
    m_wood  = l3_material(sc, LC_RAMP_WOOD, 4, 0.1f, L3_GRAIN);
    m_ivory = l3_material(sc, IVORY, 4, 0.35f, L3_ROUND);
    m_pip   = l3_material(sc, PIPS, 2, 0, 0);
    m_ace   = l3_material(sc, ACE, 2, 0, 0);
    pip_texture();
    /* DICE's light, from the upper left and toward you; the sun shines the other way. */
    l3_sun(sc, l3_scale(E(LIGHT), -1), 0.55f, 0.18f);
    l3_shadows(sc, true);
    l3_sky(sc, LC_BLACK, LC_BLACK);
}

/* DICE's view: orthographic, looking down at ELEV, S dots to a world unit
   across. lap3d keeps dots their true shape, so the height in world units
   is the tray's height in dots over S, stretched by the dot's aspect. */
static void scene_camera(void)
{
    const float aspect = dot_aspect();
    const float k = (HW - 0.12f) * cos_e / 2;
    const v3 up = V(0, sin_e, cos_e), dir = V(0, cos_e, -sin_e);
    const v3 target = mul(up, k);
    l3_camera_ortho(sc, E(add(target, mul(dir, -8))), E(target), scene_rows * 3 * aspect / S);
}

/* True when no die has moved since the last picture of the tray. */
static bool unchanged(void)
{
    bool same = seen_n == ndice;
    for (int i = 0; i < ndice; i++) {
        const die_t *d = &dice[i];
        const float now[12] = { d->p.x, d->p.y, d->p.z, d->ax[0].x, d->ax[0].y, d->ax[0].z,
                                d->ax[1].x, d->ax[1].y, d->ax[1].z, d->ax[2].x, d->ax[2].y, d->ax[2].z };
        if (same && memcmp(now, seen[i], sizeof(now))) same = false;
        memcpy(seen[i], now, sizeof(now));
    }
    seen_n = ndice;
    return same;
}

static void wall(float cx, float cy, float z0, float z1, float hx, float hy)
{
    const l3_xform t = l3_pose(E(V(cx, cy, (z0 + z1) / 2)), (l3_quat){ 0, 0, 0, 1 }, 1);
    l3_box(sc, &t, l3v(hx, (z1 - z0) / 2, hy), m_wood);
}

static void render_tray(void)
{
    if (!sc) {
        sc = l3_create(cv.cw, scene_rows);
        if (!sc) return;
        scene_setup();
    }
    /* At rest nothing moved: the dice join the still part of the scene and
       the engine keeps last frame's picture. */
    const bool rest = unchanged();
    l3_begin(sc);
    scene_camera();
    l3_flags(sc, L3_STATIC);
    l3_quad(sc, E(V(-WX, -WY, 0)), E(V(WX, -WY, 0)), E(V(WX, WY, 0)), E(V(-WX, WY, 0)), m_felt);
    wall(0, WY + RIM / 2, 0, HW, WX + RIM, RIM / 2);                 /* back */
    wall(0, -(WY + RIM / 2), -0.12f, HW, WX + RIM, RIM / 2);         /* front, down past the felt */
    wall(-(WX + RIM / 2), 0, 0, HW, RIM / 2, WY);                    /* left */
    wall(WX + RIM / 2, 0, 0, HW, RIM / 2, WY);                       /* right */
    l3_flags(sc, L3_CAST | (rest ? L3_STATIC : 0));
    for (int i = 0; i < ndice; i++) {
        const die_t *d = &dice[i];
        const l3_vec a = E(d->ax[0]), b = E(d->ax[1]), c = E(d->ax[2]), p = E(d->p);
        const l3_xform t = { { a.x, b.x, c.x, a.y, b.y, c.y, a.z, b.z, c.z, p.x, p.y, p.z } };
        l3_rbox(sc, &t, l3v(H, H, H), H * 0.10f, m_ivory);
    }
    l3_end(sc);
    /* The tray's dots into the canvas, under the readout and the buttons. */
    const uint8_t *dots = l3_dots(sc);
    for (int y = 0; y < scene_rows * 3; y++)
        memcpy(cv.d + (tray_y0 + y) * cv.w, dots + y * cv.w, (size_t)cv.w);
}

/* ---- heads-up and buttons ------------------------------------------------- */

typedef struct { int x, y, w, h; } box_t;
static box_t btn_minus, btn_roll, btn_plus;

static void pill(box_t b, const char *label, uint8_t fill, uint8_t ink, bool pressed)
{
    /* A rounded rectangle; a stadium when it is wider than tall. */
    const int r = (b.h < b.w ? b.h : b.w) / 2;
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++) {
            const int dx = x < r ? r - x : x >= b.w - r ? x - (b.w - r - 1) : 0;
            const int dy = y < r ? r - y : y >= b.h - r ? y - (b.h - r - 1) : 0;
            if (dx * dx + dy * dy > r * r) continue;
            const bool lip = !pressed && y >= b.h - 3;
            const uint8_t dark = fill == LC_BR_RED ? LC_RED : fill == LC_BR_YELLOW ? LC_YELLOW : fill == LC_GREY ? LC_BLACK : LC_GREY;
            lc_pset(&cv, b.x + x, b.y + y, lip ? dark : fill);
        }
    int sc = b.h >= 26 ? 3 : 2;
    while (sc > 1 && lc_text_w(label, sc) > b.w - 6) sc--;
    lc_text(&cv, b.x + (b.w - lc_text_w(label, sc)) / 2, b.y + (b.h - 3 - 5 * sc) / 2 + (pressed ? 2 : 0), label, sc, ink);
}

static int pressed_btn = -1;
static uint32_t pressed_ms;

static void draw_hud(void)
{
    lc_fill_rect(&cv, 0, 0, hud_w, hud_h, LC_BLACK);
    const uint32_t now = lap_millis();
    char t[16];
    const int top = 9;                  /* the label line above the message */
    if (have_result) {
        const uint32_t age = now - result_ms;
        const int extra = ndice > 1 ? 9 : 0;
        int k = (hud_h - top - extra - 2) / 5;
        k = k < 2 ? 2 : k > 7 ? 7 : k;
        snprintf(t, sizeof(t), "%d", total);
        const int y = top + (hud_h - top - extra - 5 * k) / 2;
        /* A pop: the number lands big and settles to size. */
        const int kk = k + (age < 100 ? 2 : age < 200 ? 1 : 0);
        lc_text_outlined(&cv, (hud_w - lc_text_w(t, kk)) / 2, y - (kk - k) * 5 / 2, t, kk, age < 450 ? LC_BR_YELLOW : LC_BR_WHITE, LC_RED);
        /* The faces that made it, small, under the total. */
        char faces[32] = "";
        for (int i = 0; i < ndice; i++) {
            char f[4];
            snprintf(f, sizeof(f), i ? "+%d" : "%d", dice[i].top);
            strcat(faces, f);
        }
        if (ndice > 1) lc_text(&cv, (hud_w - lc_text_w(faces, 1)) / 2, y + 5 * k + 3, faces, 1, LC_WHITE);
    } else if (rolling && now - kick_ms < 380) {
        /* The throw lands in the readout first: big, then settling. */
        const uint32_t age = now - kick_ms;
        int k = (hud_h - top - 4) / 5;
        k = k < 2 ? 2 : k > 6 ? 6 : k;
        int kk = k + (age < 90 ? 2 : age < 180 ? 1 : 0);
        while (kk > 1 && lc_text_w("ROLL!", kk) > hud_w - 4) kk--;
        lc_text_outlined(&cv, (hud_w - lc_text_w("ROLL!", kk)) / 2, top + (hud_h - top - 5 * kk) / 2, "ROLL!", kk,
                         age < 140 ? LC_BR_WHITE : LC_BR_YELLOW, LC_RED);
    } else if (rolling) {
        const int n = (int)(now / 120) % 4;
        const char *msg = n == 0 ? "ROLLING" : n == 1 ? "ROLLING." : n == 2 ? "ROLLING.." : "ROLLING...";
        lc_text(&cv, (hud_w - lc_text_w("ROLLING...", 2)) / 2, top + (hud_h - top) / 2 - 5, msg, 2, LC_BR_CYAN);
    } else {
        const int y = top + (hud_h - top) / 2 - 9;
        lc_text(&cv, (hud_w - lc_text_w("SHAKE TO ROLL", 2)) / 2, y, "SHAKE TO ROLL", 2, LC_BR_WHITE);
        lc_text(&cv, (hud_w - lc_text_w("TILT TO SLIDE", 1)) / 2, y + 14, "TILT TO SLIDE", 1, LC_GREY);
    }
    /* What the engine did for this frame, for timing against DICE. */
    if (sc) {
        l3_stats_t st;
        l3_stats(sc, &st);
        snprintf(t, sizeof(t), "%u.%uMS %u", (unsigned)(st.us_total / 1000),
                 (unsigned)(st.us_total / 100 % 10), (unsigned)st.dots_shaded);
        lc_text(&cv, 2, hud_h - 7, t, 1, LC_GREY);
    }
    /* The last few totals down the right. */
    /* Top line: how many dice, the sensor, and the last totals. */
    snprintf(t, sizeof(t), "%dD6", ndice);
    lc_text(&cv, 2, 1, t, 1, LC_BR_CYAN);
    if (tilt_live) lc_text(&cv, 2 + lc_text_w(t, 1) + 6, 1, "TILT", 1, LC_GREEN);
    int x = hud_w - (hud_w == cv.w ? 14 : 2);    /* the board's close button sits top right */
    for (int i = 0; i < nhist && i < 6; i++) {
        snprintf(t, sizeof(t), "%d", history[i]);
        x -= lc_text_w(t, 1);
        if (x < 34) break;              /* never over the label */
        lc_text(&cv, x, 1, t, 1, i ? LC_GREY : LC_WHITE);
        x -= 5;
    }
}

static void draw_bar(void)
{
    lc_fill_rect(&cv, bar_x, bar_y, cv.w - bar_x, cv.h - bar_y, LC_BLACK);
    const bool hot = pressed_btn >= 0 && lap_millis() - pressed_ms < 140;
    pill(btn_minus, "-", ndice > 1 ? LC_GREY : LC_BLACK, LC_BR_WHITE, hot && pressed_btn == 0);
    const bool kicked = kick_ms && lap_millis() - kick_ms < 160;
    pill(btn_roll, "ROLL", kicked ? LC_BR_YELLOW : LC_BR_RED, kicked ? LC_RED : LC_BR_WHITE, kicked || (hot && pressed_btn == 1));
    pill(btn_plus, "+", ndice < MAXD ? LC_GREY : LC_BLACK, LC_BR_WHITE, hot && pressed_btn == 2);
}

/* ---- the app -------------------------------------------------------------- */

static void layout(void)
{
    const int cols = lap_cols(), rows = lap_rows();
    if (!lap_canvas_init(&cv, cols, rows)) return;
    l3_destroy(sc);
    sc = NULL;
    seen_n = -1;
    const bool wide = cv.w > cv.h * 13 / 10;
    int bar_h = 0, bar_w = 0;
    if (wide) {
        /* Landscape: the readout on the left, the buttons on the right. */
        hud_w = cv.w / 4; hud_h = cv.h;
        bar_w = cv.w / 6 < 34 ? 34 : cv.w / 6;
        bar_x = cv.w - bar_w; bar_y = 0;
        tray_x0 = hud_w; tray_x1 = bar_x; tray_y0 = 0; tray_y1 = cv.h;
    } else {
        hud_w = cv.w; hud_h = cv.h / 5 < 32 ? 32 : cv.h / 5;
        bar_h = cv.h / 7 < 32 ? 32 : cv.h / 7;
        bar_x = 0; bar_y = cv.h - bar_h;
        tray_x0 = 0; tray_x1 = cv.w; tray_y0 = hud_h; tray_y1 = bar_y;
    }
    /* On whole cells: the engine draws whole cells. */
    tray_y0 = (tray_y0 + 2) / 3 * 3;
    tray_y1 = tray_y1 / 3 * 3;
    if (!wide) { hud_h = tray_y0; bar_y = tray_y1; }
    tray_x0 = 0; tray_x1 = cv.w;
    scene_rows = (tray_y1 - tray_y0) / 3;
    /* Fit the tray, rim and all, into the space between. */
    sin_e = sinf(ELEV); cos_e = cosf(ELEV);
    const float tw = (float)(wide ? bar_x - hud_w : tray_x1 - tray_x0), th = (float)(tray_y1 - tray_y0);
    WX = 1.0f;
    S = (tw - 6) / (2 * (WX + RIM));
    const float Sy = S / dot_aspect();   /* dots per world unit down the screen */
    WY = ((th - 6) / Sy - HW * cos_e - 0.12f * cos_e) / (2 * sin_e) - RIM;
    if (WY < 0.6f) {                    /* a wide, short space: fit the height instead */
        WY = 0.6f;
        S = (th - 6) / (2 * (WY + RIM) * sin_e + (HW + 0.12f) * cos_e);
        WX = (tw - 6) / S / 2 - RIM;
    }
    tcx = wide ? (hud_w + bar_x) / 2.0f : (tray_x0 + tray_x1) / 2.0f;
    tcy = (tray_y0 + tray_y1) / 2.0f + (HW - 0.12f) * cos_e * S / 2;
    const int pad = 4, gap = 4;
    if (wide) {
        const int side = (cv.h - 8 - pad - 2 * gap) / 4, w = bar_w - 2 * pad;
        const int top = 8;                 /* below the board's close button */
        btn_plus = (box_t){ bar_x + pad, top, w, side };
        btn_roll = (box_t){ bar_x + pad, top + side + gap, w, cv.h - top - pad - 2 * side - 2 * gap };
        btn_minus = (box_t){ bar_x + pad, cv.h - pad - side, w, side };
    } else {
        /* In from the sides and up from the bottom: the glass's corners are round. */
        const int side_pad = 9, bottom = 7, side = (cv.w - 2 * side_pad - 2 * gap) / 4, h = bar_h - 3 - bottom;
        btn_minus = (box_t){ side_pad, bar_y + 3, side, h };
        btn_roll = (box_t){ side_pad + side + gap, bar_y + 3, cv.w - 2 * side_pad - 2 * side - 2 * gap, h };
        btn_plus = (box_t){ cv.w - side_pad - side, bar_y + 3, side, h };
    }
}

static void enter(void)
{
    lap_fullscreen(true);
    lap_hold_rotation(true);
    lap_keep_awake(true);
    light = mul(LIGHT, 1.0f / len(LIGHT));
    for (int i = 0; i < MAXD; i++) place(i);
    last_ms = lap_millis();
}

static void leave(void)
{
    l3_destroy(sc);
    sc = NULL;
    lap_canvas_free(&cv);
}

static void settle_check(void)
{
    for (int i = 0; i < ndice; i++) if (!dice[i].still) return;
    if (!rolling) {
        /* A tilt tipped a die onto another face: the total follows, quietly. */
        int t = 0;
        for (int i = 0; i < ndice; i++) t += dice[i].top;
        if (have_result && t != total) { total = t; history[0] = t; result_ms = lap_millis(); }
        return;
    }
    rolling = false;
    total = 0;
    for (int i = 0; i < ndice; i++) total += dice[i].top;
    have_result = true;
    result_ms = lap_millis();
    memmove(history + 1, history, sizeof(history) - sizeof(history[0]));
    history[0] = total;
    if (nhist < 8) nhist++;
    lap_tone(0, 30, 0, LAP_WAVE_SINE);
    lap_tone(660, 70, 45, LAP_WAVE_TRIANGLE);
    lap_tone(880, 70, 45, LAP_WAVE_TRIANGLE);
    lap_tone(1320, 140, 40, LAP_WAVE_TRIANGLE);
    lap_haptic(LAP_HAPTIC_CONFIRM);
}

static void draw(void)
{
    if (cv.cw != lap_cols() || cv.ch != lap_rows() || !cv.d) layout();
    if (!cv.d) { lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK)); lap_put_str(1, 1, "not enough memory", LAP_ATTR(LAP_RED, LAP_BLACK)); return; }

    const uint32_t now = lap_millis();
    float dt = (now - last_ms) / 1000.0f;
    last_ms = now;
    if (dt > 0.06f) dt = 0.06f;
    if (dt <= 0) dt = 0.001f;

    /* The sensor: tilt pulls, a sharp jolt throws. */
    lap_motion_t m;
    tilt_x = tilt_y = 0;
    tilt_live = lap_motion(&m);
    if (tilt_live) {
        tilt_x = -m.ax * 9.0f;
        tilt_y = -m.ay * 9.0f;
        const float a = sqrtf(m.ax * m.ax + m.ay * m.ay + m.az * m.az);
        const float spin = sqrtf(m.gx * m.gx + m.gy * m.gy + m.gz * m.gz);
        if ((fabsf(a - 1.0f) > 0.65f || spin > 280) && now - shake_ms > 500) {
            shake_ms = now;
            throw_dice(-m.ax * 0.5f, -m.ay * 0.5f);
        }
    }
    /* A fling across the tray throws that way. */
    const int sw = lap_swipe();
    if (sw) throw_dice(sw == LAP_SWIPE_RIGHT ? 1.f : sw == LAP_SWIPE_LEFT ? -1.f : 0, sw == LAP_SWIPE_UP ? 1.f : sw == LAP_SWIPE_DOWN ? -1.f : 0);

    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < ndice; i++) step(&dice[i], dt / 3);
        for (int i = 0; i < ndice; i++)
            for (int j = i + 1; j < ndice; j++) collide(&dice[i], &dice[j]);
    }
    settle_check();

    render_tray();
    draw_hud();
    draw_bar();
    int ox, oy;
    jolt(&ox, &oy);
    if (ox || oy) lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_canvas_show(&cv, ox, oy);
}

static bool in(box_t b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void press(int which)
{
    pressed_btn = which;
    pressed_ms = lap_millis();
    if (which == 1) throw_dice(0, 0);
    else {
        const int n = ndice + (which == 2 ? 1 : -1);
        if (n < 1 || n > MAXD) { lap_haptic(LAP_HAPTIC_WARN); return; }
        ndice = n;
        for (int i = 0; i < ndice; i++) place(i);
        have_result = false;
        lap_tone(which == 2 ? 740 : 520, 40, 35, LAP_WAVE_SQUARE);
        lap_haptic(LAP_HAPTIC_TICK);
    }
}

static bool touch(int col, int row)
{
    const int x = col * 2 + 1, y = row * 3 + 1;
    if (in(btn_minus, x, y)) press(0);
    else if (in(btn_roll, x, y)) press(1);
    else if (in(btn_plus, x, y)) press(2);
    else if (y >= tray_y0 && y < tray_y1) throw_dice(0, 0);
    return true;
}

static bool key(lap_key_t k, char ch)
{
    if (k == LAP_KEY_ENTER || (k == LAP_KEY_CHAR && ch == ' ')) { press(1); return true; }
    if (k == LAP_KEY_UP || k == LAP_KEY_RIGHT || (k == LAP_KEY_CHAR && ch == '+')) { press(2); return true; }
    if (k == LAP_KEY_DOWN || k == LAP_KEY_LEFT || (k == LAP_KEY_CHAR && ch == '-')) { press(0); return true; }
    return false;
}

/* The tile icon, in colour dots. */
LAP_ICON_PX(
    "...........777777...",
    "..........7007700788",
    "..........7007700788",
    "..........7777777788",
    "..........7777777788",
    ".ffffffffff.77700788",
    "ffffffffffff77700788",
    "ff00ffff00ff77777788",
    "ff00ffff00ff77888.88",
    "ffffffffffff77......",
    "fffff99fffff77......",
    "fffff99fffff77......",
    "ffffffffffff77......",
    "ff00ffff00ff77......",
    "ff00ffff00ff77......",
    "ffffffffffff77......",
    "ffffffffffff77......",
    ".7777777777.77......");

LAP_APP(.id = "dice3d", .name = "DICE 3D", .sub = "on the 3D engine", .version = "1.0",
        .author = "LakeShark", .color = LAP_RED,
        .icon_art = { " .----.   ", " |o  o|.-.", " | o  ||o|", " |o  o|'-'", " '----'   ", " SHAKE ME " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
