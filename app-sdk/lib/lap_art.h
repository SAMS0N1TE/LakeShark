/* Header art in braille: a small canvas of dots (two across and four down a
   cell), lines and rings on it, and a radar with a turning beam and blips.
   Header-only and cheap: one canvas a frame, drawn inside draw(). */
#ifndef LAP_ART_H
#define LAP_ART_H
#include "lakeshark_app.h"
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define ART_RND(f) ((int)((f) < 0 ? (f) - 0.5f : (f) + 0.5f))
#define ART_COLS 32
#define ART_ROWS 12
typedef struct {
    int x, y, w, h;
    uint8_t dot[ART_ROWS][ART_COLS];
    uint8_t attr[ART_ROWS][ART_COLS];
} art_t;

static inline void art_begin(art_t *a, int x, int y, int w, int h)
{
    memset(a, 0, sizeof(*a));
    a->x = x; a->y = y;
    a->w = w < 0 ? 0 : w > ART_COLS ? ART_COLS : w;
    a->h = h < 0 ? 0 : h > ART_ROWS ? ART_ROWS : h;
}
/* One dot; the last colour given to a cell is the one it shows. */
static inline void art_dot(art_t *a, int px, int py, uint8_t attr)
{
    static const uint8_t bit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
    if (px < 0 || py < 0 || px >= a->w * 2 || py >= a->h * 4) return;
    a->dot[py / 4][px / 2] |= bit[px & 1][py & 3];
    if (attr) a->attr[py / 4][px / 2] = attr;
}
static inline void art_line(art_t *a, int x0, int y0, int x1, int y1, uint8_t attr)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (int i = 0; i < 256; i++) {
        art_dot(a, x0, y0, attr);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
/* A ring of radius r dots; every `gap`th point left out (0: solid). */
static inline void art_ring(art_t *a, float cx, float cy, float r, int gap, uint8_t attr)
{
    const int n = (int)(r * 6.3f) + 8;
    for (int i = 0; i < n; i++) {
        if (gap && i % gap == 0) continue;
        const float t = 6.2831853f * (float)i / (float)n;
        art_dot(a, ART_RND(cx + r * cosf(t)), ART_RND(cy + r * sinf(t)), attr);
    }
}
static inline void art_flush(const art_t *a)
{
    for (int r = 0; r < a->h; r++)
        for (int c = 0; c < a->w; c++)
            lap_put_cell(a->x + c, a->y + r, a->dot[r][c] ? LAP_BRAILLE(a->dot[r][c]) : (int16_t)' ',
                         a->attr[r][c] ? a->attr[r][c] : LAP_ATTR(LAP_CYAN, LAP_BLACK));
}

/* ---- the radar ---------------------------------------------------------- */

typedef struct {
    float ang;      /* radians, 0 = east, clockwise on screen */
    float r;        /* 0 centre .. 1 the rim */
    uint8_t hue;    /* LAP_GREEN, LAP_YELLOW, ... (BRIGHT is added while lit) */
    bool hot;       /* flashes in the alarm colour */
} art_blip_t;

/* Rings, a beam turning once every `period_ms` (0: stopped, no beam) with a
   fading trail, and blips that light as the beam passes and fade after. */
static inline void art_radar(int x, int y, int w, int h, uint32_t now, uint32_t period_ms, const art_blip_t *b, int n)
{
    art_t a;
    art_begin(&a, x, y, w, h);
    const float cx = (float)a.w - 0.5f, cy = (float)a.h * 2.0f - 0.5f;
    const float R = (cx < cy ? cx : cy) - 0.5f;
    const uint8_t ring = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    art_ring(&a, cx, cy, R, 0, ring);
    art_ring(&a, cx, cy, R * 0.66f, 3, ring);
    art_ring(&a, cx, cy, R * 0.33f, 2, ring);
    float beam = 0;
    if (period_ms) {
        beam = 6.2831853f * (float)(now % period_ms) / (float)period_ms;
        for (int k = 3; k >= 0; k--) {           /* the trail, dimmer the further behind */
            const float t = beam - 0.16f * (float)k;
            const uint8_t at = k == 0 ? LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK) : k == 1 ? LAP_ATTR(LAP_GREEN, LAP_BLACK)
                                                                                           : LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK);
            art_line(&a, ART_RND(cx), ART_RND(cy), ART_RND(cx + R * cosf(t)), ART_RND(cy + R * sinf(t)), at);
        }
    }
    for (int i = 0; i < n; i++) {
        float behind = beam - b[i].ang;
        while (behind < 0) behind += 6.2831853f;
        const bool lit = !period_ms || behind < 2.4f;
        uint8_t hue = b[i].hue | (lit ? LAP_BRIGHT : 0);
        if (b[i].hot && (now / 250u) % 2u) hue = LAP_RED | LAP_BRIGHT;
        if (!lit && !b[i].hot) hue = LAP_BLUE | LAP_BRIGHT;
        const float rr = (b[i].r < 0.08f ? 0.08f : b[i].r > 1 ? 1 : b[i].r) * (R - 1);
        const int px = ART_RND(cx + rr * cosf(b[i].ang)), py = ART_RND(cy + rr * sinf(b[i].ang));
        art_dot(&a, px, py, LAP_ATTR(hue, LAP_BLACK));
        art_dot(&a, px + 1, py, LAP_ATTR(hue, LAP_BLACK));
        art_dot(&a, px, py + 1, LAP_ATTR(hue, LAP_BLACK));
        art_dot(&a, px + 1, py + 1, LAP_ATTR(hue, LAP_BLACK));
    }
    art_dot(&a, ART_RND(cx), ART_RND(cy), LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK));
    art_flush(&a);
}
/* An address to a bearing on the radar, so a device keeps its place. */
static inline float art_bearing(const uint8_t *a6)
{
    const unsigned h = (unsigned)a6[0] * 131u + a6[1] * 31u + a6[2] * 17u + a6[3] * 7u + a6[4] * 3u + a6[5];
    return 6.2831853f * (float)(h % 360u) / 360.0f;
}
/* RSSI to the radar's radius: -30 dBm at the centre, -100 at the rim. */
static inline float art_range(int rssi)
{
    const float r = (-30.0f - (float)rssi) / 70.0f;
    return r < 0.08f ? 0.08f : r > 1 ? 1 : r;
}
/* ---- the tab strip ------------------------------------------------------ */

/* Three finger-sized keys, < name >, drawn by the app in the look of the
   board's key bar. Not lap_ui_keys: the board keeps one record of the keys
   a screen drew, and a second bar in the same frame makes the bottom bar's
   keys look freshly changed (lit cyan) on every frame. The app hit-tests the
   strip itself (col * 3 / w). */
static inline void art_strip(int y, int w, int h, const char *label, const char *value, bool badge)
{
    const uint8_t fr = LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK), fill = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    const uint8_t face = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK), val = LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK);
    if (h < 3 || w < 12) return;
    for (int i = 0; i < 3; i++) {
        const int x0 = i * w / 3, x1 = (i + 1) * w / 3, bx = x0 + 1, bw = x1 - x0 - 2;
        lap_box(bx, y, bw, h, NULL, fr);
        lap_fill(bx + 1, y + 1, bw - 2, h - 2, LAP_SHADE_25, fill);
        char t[40];
        if (i != 1) {
            snprintf(t, sizeof(t), " %s ", i ? ">" : "<");
            lap_put_str(bx + (bw - 3) / 2, y + h / 2, t, face);
            continue;
        }
        const int ly = y + (h - (value ? 2 : 1)) / 2;
        int n = snprintf(t, sizeof(t), " %.*s ", bw - 4 > 0 ? bw - 4 : 0, label ? label : "");
        lap_put_str(bx + (bw - n) / 2, ly, t, face);
        if (value) {
            n = snprintf(t, sizeof(t), " %.*s ", bw - 4 > 0 ? bw - 4 : 0, value);
            lap_put_str(bx + (bw - n) / 2, ly + 1, t, val);
        }
        if (badge) lap_put_cell(bx + bw - 2, y + 1, '*', LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK));
    }
}
#endif
