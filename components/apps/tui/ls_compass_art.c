#include "ls_compass_art.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_tui.h"
#include "ls_tui_ui.h"
#define LS_ATTR_DIM_COMPAT LS_ATTR_DIM
#define LS_ATTR_FAINT_COMPAT LS_ATTR_FAINT

#define RAD 0.017453292519943295f

/* The model, in card radii: the card, the bezel's inner and outer edge,
   heights above the card, and how far the bezel's side drops. */
#define CARD     1.00f
#define BEZEL_IN 1.00f
#define BEZEL_OUT 1.17f
#define H_BEZEL  0.05f
#define H_NEEDLE 0.06f
#define H_CAP    0.08f
#define WALL_LOW (-0.24f)

typedef ls_dot_t dot_t;

/* A blip placed on the card: centre, radius squared, ink. */
struct blip_px { float x, y, r2; uint8_t ink; };
EXT_RAM_BSS_ATTR static struct blip_px s_blip_px[LS_COMPASS_BLIPS];

#define MAX_DOTS (160 * 120)
EXT_RAM_BSS_ATTR static dot_t s_dots[MAX_DOTS];

static float wrap180(float a) { a = fmodf(a + 540.0f, 360.0f) - 180.0f; return a; }

float ls_compass_spring_step(ls_compass_spring_t *s, float target, float dt)
{
    if (!isfinite(target)) return s->started ? s->angle : NAN;
    if (!s->started || !isfinite(s->angle)) { s->angle = target; s->velocity = 0; s->started = true; return target; }
    if (dt > 0.1f) dt = 0.1f;
    /* A stiff spring, a little under critical damping: it swings past by a
       degree or two and settles in about half a second. */
    const float omega = 9.0f, zeta = 0.55f;
    const int steps = 4;
    for (int i = 0; i < steps; i++) {
        const float h = dt / steps;
        const float err = wrap180(target - s->angle);
        const float acc = omega * omega * err - 2 * zeta * omega * s->velocity;
        s->velocity += acc * h;
        s->angle += s->velocity * h;
    }
    s->angle = fmodf(s->angle + 720.0f, 360.0f);
    return s->angle;
}

typedef struct {
    float e, se, ce;        /* view elevation and its sine / cosine */
    float roll_s, roll_c;
    float cx, cy, rpx;      /* centre and radius in pixels */
    float sub_w, sub_h;     /* one dot in pixels */
    float hc, hs;           /* cosine and sine of the card's heading */
    int blips;
    const struct blip_px *b;   /* s_blip_px: kept off the TUI task's stack */
    const ls_compass_scene_t *sc;
} view_t;

/* Screen position (pixels, y down) of a point on the card's plane. */
static void project(const view_t *v, float azimuth, float r, float h, float *px, float *py)
{
    const float rel = (azimuth - v->sc->heading) * RAD;
    const float x = r * sinf(rel), y = r * cosf(rel);
    const float X = x, Y = y * v->se + h * v->ce;
    const float Xr = X * v->roll_c - Y * v->roll_s, Yr = X * v->roll_s + Y * v->roll_c;
    *px = v->cx + Xr * v->rpx;
    *py = v->cy - Yr * v->rpx;
}

/* The signal lobe in blues, dim to bright, with cyan only at the very top;
   the estimate is red, the one warm colour on the card besides the needle,
   so the answer never reads as part of the lobe. White blended in. */
static uint8_t heat(float level)
{
    if (level < 0.50f) return TUI_BLUE;
    if (level < 0.85f) return TUI_BLUE | TUI_BRIGHT;
    return TUI_CYAN | TUI_BRIGHT;
}
#define DF_INK (TUI_RED | TUI_BRIGHT)

static bool near_angle(float a, float b, float within) { return isfinite(b) && fabsf(wrap180(a - b)) < within; }

/* A 4x4 ordered-dither threshold, so a smooth brightness becomes a
   pattern of two colours that the eye averages. */
static float bayer(int x, int y)
{
    static const uint8_t M[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    return (M[y & 3][x & 3] + 0.5f) / 16.0f;
}

/* Metal from dark to bright: one of four inks, dithered. */
static uint8_t metal(float b, int x, int y)
{
    static const uint8_t INK[4] = { TUI_BLUE, TUI_BLACK | TUI_BRIGHT, TUI_WHITE, TUI_WHITE | TUI_BRIGHT };
    if (b <= 0) return INK[0];
    if (b >= 1) return INK[3];
    const float f = b * 3.0f;
    const int lo = (int)f;
    return INK[lo + (f - lo > bayer(x, y))];
}

/* What one dot sees, front to back. */
static void shade(const view_t *v, float X, float Y, int dx, int dy, dot_t *d)
{
    const ls_compass_scene_t *sc = v->sc;
    d->set = false;
    const uint8_t white = TUI_WHITE | TUI_BRIGHT, grey = TUI_WHITE, dark = TUI_BLACK | TUI_BRIGHT;
    /* Light from above, over the left shoulder: board frame, unit length. */
    const float Lx = -0.45f, Ly = 0.35f, Lz = 0.82f;

    /* Trigonometry only where its answer is used: this runs for every dot
       of the dial every frame. The cap needs a radius, the needle a rotation
       by the heading (r cos and r sin of phi + heading, from X and y and the
       heading's own cosine and sine), and only the bezel the angle itself. */
    for (int layer = 0; layer < 3; layer++) {
        const float h = layer == 0 ? H_CAP : layer == 1 ? H_NEEDLE : H_BEZEL;
        const float y = (Y - h * v->ce) / v->se;
        const float r2 = X * X + y * y;
        if (layer == 0) {
            if (r2 < 0.075f * 0.075f) {
                const float r = sqrtf(r2);
                d->set = true;
                d->colour = r < 0.03f ? (TUI_YELLOW | TUI_BRIGHT) : metal(0.75f - r * 6.0f - X * 4.0f, dx, dy);
                return;
            }
            continue;
        }
        if (layer == 1) {
            if (!sc->valid) continue;
            const float p = y * v->hc - X * v->hs, q = X * v->hc + y * v->hs;   /* along north, across */
            const float len = 0.80f, half = 0.11f;
            if (fabsf(p) < len && fabsf(q) < half * (1.0f - fabsf(p) / len)) {
                d->set = true;
                /* Two facets per half catch the light differently: a ridge. */
                const bool lit = q < 0;
                if (p > 0) d->colour = lit ? (TUI_RED | TUI_BRIGHT) : TUI_RED;
                else d->colour = lit ? white : grey;
                return;
            }
            continue;
        }
        const float r = sqrtf(r2);
        if (r >= BEZEL_IN && r < BEZEL_OUT) {
            const float phi = atan2f(X, y) / RAD;           /* clockwise from the board's top */
            const float az = phi + sc->heading;              /* the card's own bearing */
            d->set = true;
            /* Markers are wedges cut into the bezel. */
            if (near_angle(az, sc->target, 6.0f)) { d->colour = TUI_GREEN | TUI_BRIGHT; return; }
            if (near_angle(az, sc->sun, 5.0f)) { d->colour = TUI_YELLOW | TUI_BRIGHT; return; }
            if (near_angle(az, sc->lock, 4.5f)) { d->colour = TUI_MAGENTA | TUI_BRIGHT; return; }
            if (near_angle(az, sc->df, 4.5f)) { d->colour = DF_INK; return; }
            for (int m = 0; m < sc->mark_count; m++)
                if (near_angle(az, sc->marks[m].bearing, 3.0f)) { d->colour = TUI_RED; return; }
            /* The lubber line: fixed to the board, at its top. */
            if (fabsf(X) < 0.024f && y > 0) { d->colour = TUI_YELLOW | TUI_BRIGHT; return; }
            /* A rounded ring: across its width the surface turns from facing
               in, through up, to facing out. Lambert against the light, and
               a narrow travelling glint for the polish. */
            const float t01 = (r - BEZEL_IN) / (BEZEL_OUT - BEZEL_IN);
            const float tilt = (t01 - 0.5f) * 2.4f;          /* radians of turn */
            const float nr = sinf(tilt), nz = cosf(tilt);
            const float ux = X / r, uy = y / r;
            const float lambert = fmaxf(0.0f, nr * (ux * Lx + uy * Ly) + nz * Lz);
            float b = 0.12f + 0.95f * lambert * lambert;
            /* Notches every 10 degrees of the card's own scale. */
            float m10 = fmodf(az + 360.0f, 10.0f);
            if (m10 > 5.0f) m10 -= 10.0f;
            if (t01 < 0.3f && fabsf(m10) < 0.9f) b *= 0.35f;
            d->colour = metal(b, dx, dy);
            return;
        }
    }

    /* The card. */
    {
        const float y = Y / v->se;
        const float r = sqrtf(X * X + y * y);
        if (r < CARD) {
            /* Ticks start at 0.80; inside that only the lobe and the
               estimate need to know which way a dot lies. */
            const bool need_az = r > 0.80f || sc->lobe || isfinite(sc->df) || sc->recent || sc->facing || sc->peak_count;
            const float phi = need_az ? atan2f(X, y) / RAD : 0.0f;
            const float az = phi + sc->heading;
            if (r > 0.80f) {
                float m = fmodf(az + 360.0f, 5.0f); if (m > 2.5f) m -= 5.0f;
                const float arc = fabsf(m) * RAD * r;        /* distance to the tick, in radii */
                const int deg = (int)lroundf((az + 360.0f) / 5.0f) * 5 % 360;
                const bool major = deg % 30 == 0, ten = deg % 10 == 0;
                /* Five-degree ticks only where there are dots enough to draw them. */
                const bool small = v->rpx < 180.0f;
                if (!(small && !ten)) {
                    const float inner = major ? 0.80f : ten ? 0.87f : 0.92f;
                    if (r > inner && r < 0.985f && arc < (major ? 0.017f : 0.011f)) {
                        d->set = true; d->colour = major ? white : ten ? grey : dark; return;
                    }
                }
            }
            /* Direction finding, in the order it should read: the estimate's
               line from the centre, the lobe's edge, the lobe filled, then
               the estimate's spread lightly. The line and edge are a dot or
               more wide at any size, so they survive the two-colour cells. */
            const float df_off = isfinite(sc->df) ? fabsf(wrap180(az - sc->df)) : 999.0f;
            const bool in_df = isfinite(sc->df) && df_off <= fmaxf(3.0f, sc->df_spread);
            const float dot_r = 1.2f / v->rpx * fmaxf(v->sub_w, v->sub_h);   /* one dot, in radii */
            if (in_df && r > 0.95f) { d->set = true; d->colour = DF_INK; return; }
            if (isfinite(sc->df) && r > 0.12f && df_off * RAD * r < fmaxf(0.02f, dot_r * 0.6f)) {
                d->set = true; d->colour = DF_INK; return;
            }
            /* Lesser lobes: a short red tick at the rim. */
            if (r > 0.95f)
                for (int p = 0; p < sc->peak_count; p++)
                    if (near_angle(az, sc->peaks[p], 2.5f)) { d->set = true; d->colour = TUI_RED; return; }
            /* Hits, as blips. */
            for (int b = 0; b < v->blips; b++) {
                const float ex = X - v->b[b].x, ey = y - v->b[b].y;
                if (ex * ex + ey * ey < v->b[b].r2) { d->set = true; d->colour = v->b[b].ink; return; }
            }
            /* Where the board's top points, while a receiver is being heard. */
            if (sc->facing && r > 0.10f && fabsf(phi) * RAD * r < fmaxf(0.018f, dot_r * 0.55f)) {
                d->set = true; d->colour = TUI_GREEN | TUI_BRIGHT; return;
            }
            if (sc->lobe && sc->fill != LS_COMPASS_FILL_OFF) {
                const int bin = ((int)lroundf((az + 360.0f) / 5.0f)) % 72;
                const float lv = fmaxf(0.0f, fminf(1.0f, sc->lobe[bin]));
                const float rl = 0.16f + 0.56f * lv;
                if (fabsf(r - rl) < fmaxf(0.03f, dot_r * 0.7f)) { d->set = true; d->colour = heat(lv); return; }
                if (r < rl && sc->fill == LS_COMPASS_FILL_FULL && bayer(dx, dy) < 0.55f) { d->set = true; d->colour = heat(lv); return; }
                if (r < rl && sc->fill == LS_COMPASS_FILL_LIGHT && bayer(dx, dy) < 0.15f) { d->set = true; d->colour = TUI_BLUE; return; }
            }
            /* The directions heard in the last few seconds, fading. */
            if (sc->recent && r > 0.10f) {
                const float rv = sc->recent[((int)lroundf((az + 720.0f) / 5.0f)) % 72];
                if (rv > 0 && bayer(dx, dy) < 0.55f * rv) { d->set = true; d->colour = TUI_GREEN; return; }
            }
            if (in_df && r > 0.12f && bayer(dx, dy) < 0.25f) { d->set = true; d->colour = TUI_RED; return; }
            if (fabsf(r - 0.60f) < 0.009f) { d->set = true; d->colour = TUI_BLUE; return; }
            /* The needle's shadow, cast down and to the right. */
            if (sc->valid) {
                const float sx = X - 0.035f, sy = y + 0.04f;
                const float p = sy * v->hc - sx * v->hs, q = sx * v->hc + sy * v->hs;
                if (fabsf(p) < 0.80f && fabsf(q) < 0.11f * (1.0f - fabsf(p) / 0.80f) && bayer(dx, dy) < 0.5f) {
                    d->set = true; d->colour = TUI_BLUE; return;
                }
            }
            return;                                          /* the card's own black */
        }
    }

    /* The bezel's side, where the near edge curves away. */
    if (fabsf(X) < BEZEL_OUT) {
        const float yw = -sqrtf(BEZEL_OUT * BEZEL_OUT - X * X);
        const float z = (Y - yw * v->se) / v->ce;
        if (z <= H_BEZEL && z >= WALL_LOW) {
            d->set = true;
            /* A cylinder facing the viewer, lit from the left, darker toward
               its foot where the light does not reach. */
            const float nx = X / BEZEL_OUT;
            float b = 0.2f + 0.75f * fmaxf(0.0f, 0.55f - nx * 0.9f);
            b *= 0.55f + 0.45f * (z - WALL_LOW) / (H_BEZEL - WALL_LOW);
            if (fabsf(z + 0.085f) < 0.014f) b *= 0.4f;           /* a machined groove */
            d->colour = metal(b, dx, dy);
        }
    }
}

uint8_t ls_compass_letter_ink(bool north, bool white)
{
    if (north) return TUI_RED | TUI_BRIGHT;
    return white ? (TUI_WHITE | TUI_BRIGHT) : (TUI_YELLOW | TUI_BRIGHT);
}

/* Degree numbers in bright colours, one per quarter, so the quarter the
   board faces reads at a glance: orange, blue, red, magenta. */
static uint8_t quarter_colour(int deg)
{
    static const uint8_t Q[4] = { TUI_YELLOW | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT,
                                  TUI_RED | TUI_BRIGHT, TUI_MAGENTA | TUI_BRIGHT };
    return Q[((deg % 360) + 360) % 360 / 90];
}

/* The flat face, for SIMPLE style: LORA LABS's ring, drawn the way it
   draws it, with this app's markers and lobe on top. */
static void simple_face(tui_surface *sf, tui_rect area, const ls_compass_scene_t *sc, int cw, int ch)
{
    if (area.w < 8 || area.h < 5) return;
    const int cx = area.x + area.w / 2, cy = area.y + area.h / 2;
    float ry = (area.h - 2) * .45f, rx = ry * ch / cw;
    if (rx > (area.w - 4) * .5f) { rx = (area.w - 4) * .5f; ry = rx * cw / ch; }
    const bool valid = sc->valid;
    const float heading = valid ? sc->heading : 0;
    for (int i = 0; i < 72; i++) {
        const float deg = i * 5.0f, angle = (deg - heading) * RAD;
        const int x = cx + (int)(sinf(angle) * rx), y = cy - (int)(cosf(angle) * ry);
        char c = i % 6 ? '.' : '+';
        uint8_t at = valid ? LS_ATTR_DIM_COMPAT : LS_ATTR_FAINT_COMPAT;
        if (near_angle(deg, sc->target, 3.0f)) { c = '@'; at = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK); }
        else if (near_angle(deg, sc->sun, 3.0f)) { c = '*'; at = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK); }
        else if (near_angle(deg, sc->lock, 3.0f)) { c = '#'; at = TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK); }
        else if (near_angle(deg, sc->df, 3.0f)) { c = 'X'; at = TUI_ATTR(DF_INK, TUI_BLACK); }
        tui_put_char(sf, area, x, y, c, at);
        if (valid && sc->lobe && sc->lobe[i] > 0 && sc->fill != LS_COMPASS_FILL_OFF) {
            const float v = 0.1f + 0.9f * fminf(1.0f, sc->lobe[i]);
            tui_put_char(sf, area, cx + (int)(sinf(angle) * rx * v), cy - (int)(cosf(angle) * ry * v), '*',
                         TUI_ATTR(heat(fminf(1.0f, sc->lobe[i])), TUI_BLACK));
        }
    }
    if (valid) {
        static const char cardinals[] = "NESW";
        for (int i = 0; i < 4; i++) {
            const float angle = (i * 90 - heading) * RAD;
            tui_put_char(sf, area, cx + (int)lroundf(sinf(angle) * rx * .72f), cy - (int)lroundf(cosf(angle) * ry * .72f),
                         cardinals[i], TUI_ATTR(ls_compass_letter_ink(i == 0, sc->white_letters), TUI_BLACK));
        }
        if (area.h >= 22 && area.w >= 34)
            for (int degrees = 0; degrees < 360; degrees += 30) {
                const float angle = (degrees - heading) * RAD;
                char t[4]; snprintf(t, sizeof(t), "%d", degrees);
                tui_put_str(sf, area, cx + (int)lroundf(sinf(angle) * rx * .9f) - (int)strlen(t) / 2,
                            cy - (int)lroundf(cosf(angle) * ry * .9f), t, LS_ATTR_DIM_COMPAT);
            }
    }
    if (valid) {
        for (int b = 0; b < sc->blip_count && sc->blips; b++) {
            const ls_compass_blip_t *h = &sc->blips[b];
            if (!isfinite(h->bearing)) continue;
            const float angle = (h->bearing - heading) * RAD, v = 0.2f + 0.7f * fmaxf(0.0f, fminf(1.0f, h->strength));
            tui_put_char(sf, area, cx + (int)lroundf(sinf(angle) * rx * v), cy - (int)lroundf(cosf(angle) * ry * v), 'o',
                         TUI_ATTR(h->age < 0.4f ? (TUI_GREEN | TUI_BRIGHT) : TUI_GREEN, TUI_BLACK));
        }
        for (int m = 0; m < sc->mark_count && sc->marks; m++) {
            if (!isfinite(sc->marks[m].bearing)) continue;
            const float angle = (sc->marks[m].bearing - heading) * RAD;
            tui_put_char(sf, area, cx + (int)lroundf(sinf(angle) * rx * 1.08f), cy - (int)lroundf(cosf(angle) * ry * 1.08f),
                         sc->marks[m].tag, TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
        }
    }
    const int marker_y = cy - (int)lroundf(ry) - 2;
    tui_put_char(sf, area, cx, marker_y, '^', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    tui_put_char(sf, area, cx, marker_y + 1, '|', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    tui_put_char(sf, area, cx, cy, '+', LS_ATTR_DIM_COMPAT);
    if (!valid) tui_put_str(sf, area, cx - 4, cy + 1, "NO MAG", LS_ATTR_DIM_COMPAT);
}

static void label(tui_surface *sf, tui_rect area, const view_t *v, float az, float r, float h,
                  const char *text, uint8_t attr)
{
    float px, py;
    project(v, az, r, h, &px, &py);
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    const int col = area.x + (int)floorf(px / cw) - (int)strlen(text) / 2;
    const int row = area.y + (int)floorf(py / ch);
    tui_put_str(sf, area, col, row, text, attr);
}

void ls_dots_blit(tui_surface *sf, tui_rect area, const ls_dot_t *dots, int dots_w)
{
    /* Six dots to a cell. A sextant carries two colours, its ink and its
       paper, so the two most common colours in the cell are kept: the
       dither survives, and edges stay crisp. */
    for (int r = 0; r < area.h; r++)
        for (int c = 0; c < area.w; c++) {
            int col[6], count[6] = { 0 };
            for (int k = 0; k < 6; k++) {
                const ls_dot_t *q = &dots[(3 * r + k / 2) * dots_w + 2 * c + (k & 1)];
                col[k] = q->set ? q->colour : -1;
            }
            for (int k = 0; k < 6; k++) for (int m = 0; m < 6; m++) count[k] += col[m] == col[k];
            int ink = -1;
            for (int k = 0; k < 6; k++)
                if (col[k] >= 0 && (ink < 0 || count[k] > count[ink] || (count[k] == count[ink] && col[k] > col[ink]))) ink = k;
            if (ink < 0) continue;
            int paper = -1;
            for (int k = 0; k < 6; k++)
                if (col[k] != col[ink] && (paper < 0 || count[k] > count[paper])) paper = k;
            const int fg = col[ink], bg = paper >= 0 && col[paper] >= 0 ? col[paper] : TUI_BLACK;
            int bits = 0;
            for (int k = 0; k < 6; k++) if (col[k] == fg) bits |= 1 << k;
            tui_put_char(sf, area, area.x + c, area.y + r, LS_TUI_SEXT(bits), TUI_ATTR(fg, bg));
        }
}

void ls_compass_art_draw(tui_surface *sf, tui_rect area, const ls_compass_scene_t *sc)
{
    if (!sf || !sc || area.w < 12 || area.h < 6) return;
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw = 10;
    if (ch < 1) ch = 17;
    /* Two dots across and three down to a cell (sextants): on this font
       the dots come out nearly square, and there are half as many again as
       quadrants gave. */
    const int dots_w = area.w * 2, dots_h = area.h * 3;
    if (sc->simple) { simple_face(sf, area, sc, cw, ch); return; }
    if (dots_w * dots_h > MAX_DOTS) return;

    view_t v = { .sc = sc, .sub_w = cw / 2.0f, .sub_h = ch / 3.0f, .b = s_blip_px };
    const float wpx = area.w * (float)cw, hpx = area.h * (float)ch;
    /* Across, the dial is as wide as its degree chips allow: a chip is
       five cells centred 0.13 radii outside the bezel. */
    const float rpx_w = (wpx / 2 - 2.6f * cw) / (BEZEL_OUT + 0.13f);
    /* Fifty degrees above the card by default: enough to read the whole
       scale, low enough to see a dish and not a drawing. When the area is
       tall (portrait) and width sets the size, the spare height goes on a
       steeper view, up to 66 degrees, which shows more of the card. */
    float base = 50.0f;
    for (float t = 66.0f; t > 50.0f; t -= 2.0f) {
        const float st = sinf(t * RAD), ct = cosf(t * RAD);
        const float tall = (BEZEL_OUT + 0.2f) * st + H_BEZEL * ct + 0.08f +
                           fmaxf((BEZEL_OUT + 0.2f) * st, BEZEL_OUT * st - WALL_LOW * ct) + 0.04f;
        if (tall * rpx_w <= hpx) { base = t; break; }
    }
    float e = base - (isfinite(sc->pitch) ? sc->pitch : 0) * 0.5f;
    if (e < 28) e = 28;
    if (e > 84) e = 84;
    v.e = e; v.se = sinf(e * RAD); v.ce = cosf(e * RAD);
    v.hc = cosf(sc->heading * RAD); v.hs = sinf(sc->heading * RAD);

    const float roll = (isfinite(sc->roll) ? sc->roll : 0) * 0.4f * RAD;
    v.roll_s = sinf(roll); v.roll_c = cosf(roll);
    /* Fit the whole instrument, side wall and lubber mark included. */
    const float top = (BEZEL_OUT + 0.2f) * v.se + H_BEZEL * v.ce + 0.08f;
    const float bottom = fmaxf((BEZEL_OUT + 0.2f) * v.se, BEZEL_OUT * v.se - WALL_LOW * v.ce) + 0.04f;
    v.rpx = fminf(rpx_w, hpx / (top + bottom));
    v.cx = wpx / 2;
    v.cy = top * v.rpx + (hpx - (top + bottom) * v.rpx) / 2;
    {
        /* Blips sit further out the louder they were, on the card's own
           bearing, bright while new. */
        const float dot_r = 1.2f / v.rpx * fmaxf(v.sub_w, v.sub_h);
        const int n = sc->blips ? (sc->blip_count < LS_COMPASS_BLIPS ? sc->blip_count : LS_COMPASS_BLIPS) : 0;
        for (int b = 0; b < n; b++) {
            const ls_compass_blip_t *h = &sc->blips[b];
            if (!isfinite(h->bearing)) continue;
            const float st = fmaxf(0.0f, fminf(1.0f, h->strength)), age = fmaxf(0.0f, fminf(1.0f, h->age));
            const float rb = 0.24f + 0.58f * st, rel = (h->bearing - sc->heading) * RAD;
            const float size = fmaxf(0.05f, dot_r * 1.8f) * (1.0f - 0.4f * age);
            s_blip_px[v.blips] = (struct blip_px){ rb * sinf(rel), rb * cosf(rel), size * size,
                                                   age < 0.4f ? (TUI_GREEN | TUI_BRIGHT) : TUI_GREEN };
            v.blips++;
        }
    }

    for (int j = 0; j < dots_h; j++)
        for (int i = 0; i < dots_w; i++) {
            const float px = (i + 0.5f) * v.sub_w, py = (j + 0.5f) * v.sub_h;
            float X = (px - v.cx) / v.rpx, Y = (v.cy - py) / v.rpx;
            const float Xu = X * v.roll_c + Y * v.roll_s, Yu = -X * v.roll_s + Y * v.roll_c;
            shade(&v, Xu, Yu, i, j, &s_dots[j * dots_w + i]);
        }

    ls_dots_blit(sf, area, s_dots, dots_w);

    /* Letters on the card. */
    static const char *const CARD_N[] = { "N", "E", "S", "W" };
    static const char *const CARD_I[] = { "NE", "SE", "SW", "NW" };
    for (int i = 0; i < 4; i++) {
        const float az = i * 90.0f;
        const uint8_t dim = TUI_BLACK | TUI_BRIGHT;
        const uint8_t fg = sc->valid ? ls_compass_letter_ink(i == 0, sc->white_letters) : dim;
        label(sf, area, &v, az, 0.70f, 0, CARD_N[i], TUI_ATTR(fg, TUI_BLACK));
        if (v.rpx > 150)
            label(sf, area, &v, az + 45.0f, 0.70f, 0, CARD_I[i],
                  TUI_ATTR(sc->valid ? ls_compass_letter_ink(false, sc->white_letters) : dim, TUI_BLACK));
    }
    /* Degrees round the outside of the bezel as solid chips of colour, one
       colour per quarter: a filled block reads far stronger than coloured
       strokes on this palette. */
    if (v.rpx > 90)
        for (int deg = 30; deg < 360; deg += 30) {
            if (deg % 90 == 0) continue;
            char t[8]; snprintf(t, sizeof(t), " %d ", deg);
            label(sf, area, &v, (float)deg, BEZEL_OUT + 0.13f, H_BEZEL, t,
                  sc->valid ? TUI_ATTR(TUI_BLACK, quarter_colour(deg)) : TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK));
        }
    /* Each other channel's tag, just inside the bezel at its bearing. */
    for (int m = 0; m < sc->mark_count; m++) {
        if (!isfinite(sc->marks[m].bearing)) continue;
        const char t[2] = { sc->marks[m].tag, 0 };
        label(sf, area, &v, sc->marks[m].bearing, 0.87f, 0, t, TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
    }
    /* The lubber mark above the bezel. */
    {
        float px, py;
        project(&v, sc->heading, BEZEL_OUT + 0.08f, H_BEZEL, &px, &py);
        tui_put_char(sf, area, area.x + (int)(px / cw), area.y + (int)(py / ch), 'v',
                     TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
}
