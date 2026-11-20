/* The cockpit round the glass: every hull its own canopy, drawn as dots over the
 * view in the frame's two greys. One table row per hull says what its frame is
 * made of; the code below builds that for the desk's wide glass or the board's
 * tall one, keeping clear of the tapes, the heading tape, the reticle and the
 * words. Struts taper toward their far end and are shaded across like tubes,
 * lit from the sun's side; a second rim runs inside the glass; the dash lip
 * carries the instruments' amber glow. The frame sways a dot or two against
 * the view in hard turns and boosts, never while the board itself is the stick. */
#include "internal.h"
#include "art.h"
#include "ui.h"

enum { CK_FRAME, CK_BUBBLE, CK_BRIDGE, CK_SLIT, CK_PANO, CK_PORT };     /* the canopy's kind */
enum { DASH_FLAT, DASH_COWL, DASH_TWIN, DASH_NOTCH, DASH_ARC };          /* the dash edge's shape */
enum {
    CF_RIVETS = 1,      /* rivet heads along the pillars and the roof's edge */
    CF_PANELS = 2,      /* seams across the plates */
    CF_CONSOLES = 4,    /* side consoles outside the pillars (wide glass) */
    CF_OVERHEAD = 8,    /* a box hung from the roof over the middle */
    CF_HOOP = 16,       /* a fighter's canopy bow arching over the glass */
    CF_RIM = 32,        /* the glass's inner seal, a second rim inside the struts */
    CF_ASYM = 64,       /* a rebuilt frame: one heavy pillar, one light, a patch brace */
    CF_SEAM = 128,      /* a bubble's centre seam over the top */
    CF_PERISCOPE = 256, /* a sight block on the roof's middle */
};

typedef struct {
    uint8_t kind;
    uint8_t pw, pf;             /* pillar thickness at the near and the far end, dots */
    uint8_t foot, head;         /* wide glass: pillar foot and head, % of the width in from the side */
    uint8_t bx, by;             /* corner braces: head % across the top, foot % down the side (0: none) */
    uint8_t roof_side, roof_mid;/* the roof's depth at the side and over the middle, dots */
    uint8_t posts;              /* mullions across a wide glass: 0, 2 or 4 */
    uint8_t dash, rise, nose;   /* the dash edge, its lip above the sill and the nose's rise, dots */
    uint16_t flags;
} cockpit_def;

/* Hull by hull: a starter's bubble, haulers' boxy frames and bridges, an
   armoured slit, interceptors' bows and noses, the explorer's panorama, a pod's
   porthole. Indexed by game_class; hulls past the table fly the pod's. */
static const cockpit_def cockpits[] = {
    /* Dogfish: a small bubble, thin bow, the nose below the glass */
    {CK_BUBBLE, 3, 2, 4, 24, 0, 0, 0, 0, 0, DASH_ARC, 2, 7, CF_SEAM | CF_RIM},
    /* Houndshark: a light hauler's square frame, chamfered corners, riveted */
    {CK_FRAME, 7, 4, 6, 9, 13, 30, 6, 4, 0, DASH_FLAT, 3, 0, CF_RIVETS | CF_CONSOLES},
    /* Mako Mk III: the all-rounder's raked frame and cowl */
    {CK_FRAME, 6, 3, 3, 12, 21, 30, 3, 2, 0, DASH_COWL, 2, 0, CF_RIM | CF_PANELS},
    /* Thresher: a trader's bridge, three panes, a deep roof with its overhead */
    {CK_BRIDGE, 6, 5, 2, 4, 9, 20, 11, 5, 2, DASH_FLAT, 2, 0, CF_OVERHEAD | CF_CONSOLES | CF_PANELS},
    /* Whale Shark: the long bridge, five panes, a deep riveted roof */
    {CK_BRIDGE, 7, 5, 1, 2, 7, 16, 13, 6, 4, DASH_FLAT, 2, 0, CF_OVERHEAD | CF_CONSOLES | CF_PANELS | CF_RIVETS},
    /* Basker: armour; heavy pillars and braces, a slit between deep plates */
    {CK_SLIT, 13, 7, 4, 10, 22, 34, 14, 7, 0, DASH_ARC, 6, 0, CF_RIVETS | CF_PANELS},
    /* Blacktip: the interceptor's bow over the top, a sharp nose */
    {CK_BUBBLE, 4, 2, 5, 20, 0, 0, 0, 0, 0, DASH_NOTCH, 2, 9, CF_HOOP | CF_RIM},
    /* Tiger: a pirate's rebuilt frame, raked hard, lopsided */
    {CK_FRAME, 5, 2, 2, 30, 0, 0, 2, 0, 0, DASH_NOTCH, 2, 6, CF_ASYM},
    /* Hammerhead: the explorer's panorama, the frame pushed to the edges */
    {CK_PANO, 3, 2, 1, 3, 0, 0, 2, 1, 0, DASH_TWIN, 2, 0, CF_CONSOLES | CF_OVERHEAD | CF_RIM},
    /* Goblin: military, an octagon of thick riveted plate with a sight block */
    {CK_SLIT, 10, 6, 5, 9, 17, 32, 9, 5, 0, DASH_TWIN, 4, 0, CF_RIVETS | CF_PERISCOPE},
    /* Cookiecutter: a pod's porthole */
    {CK_PORT, 0, 0, 0, 0, 0, 0, 0, 0, 0, DASH_FLAT, 0, 0, CF_RIVETS},
};
#define COCKPITS ((int)(sizeof cockpits / sizeof cockpits[0]))

static uint8_t *dots;
static int W, H;
static bool wide;
static float lx, ly, face;              /* the light across the screen, and on the faces we see */
static float sw_x, sw_y;                /* the sway, dots, at the nearest parts */
static bool sway_on = true;
typedef struct { int x0, y0, x1, y1; } ck_box;
static ck_box quiet[4];  /* the words' ground: the frame stays black there */
static int nquiet;

#define GREY HUD_DIM                    /* 8, the frame's dark slate */
#define LIT LAP_WHITE                   /* 7, a lit edge */
#define GLOW LAP_YELLOW                 /* the instruments' amber, dim */

void ui_cockpit_sway(bool on) { sway_on = on; }

static inline void put(int x, int y, uint8_t ink) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    if (ink != LAP_BLACK && (y < 12 || y >= H - 6))      /* the only rows the words' boxes reach */
        for (int i = 0; i < nquiet; i++)
            if (x >= quiet[i].x0 && x < quiet[i].x1 && y >= quiet[i].y0 && y < quiet[i].y1) { ink = LAP_BLACK; break; }
    dots[y * W + x] = ink;
}
static void line(float x0, float y0, float x1, float y1, uint8_t ink, int every) {
    int ax = UI_ROUND(x0), ay = UI_ROUND(y0), bx = UI_ROUND(x1), by = UI_ROUND(y1);
    int dx = abs(bx - ax), dy = -abs(by - ay), sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1, e = dx + dy;
    for (int n = 0; n < 1200; n++) {
        if (n % every == 0) put(ax, ay, ink);
        if (ax == bx && ay == by) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; ax += sx; }
        if (e2 <= dx) { e += dx; ay += sy; }
    }
}

/* A strut from a (near) to b (far), wa and wb dots thick at each end, its ends
   swaying by depth da and db (0 far .. 1 near). Shaded across like a tube: the
   half toward the light solid slate with a lit rim, a short dithered turn,
   the shadow half black inside a slate outline; rivets and seams on request. */
static void strut(float ax, float ay, float bx, float by, float wa, float wb, float da, float db, uint16_t flags) {
    ax += sw_x * da; ay += sw_y * da; bx += sw_x * db; by += sw_y * db;
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    float ux = dx / len, uy = dy / len, nx = -uy, ny = ux;
    float ha = wa * .5f, hb = wb * .5f;
    float qx[4] = {ax + nx * ha, bx + nx * hb, bx - nx * hb, ax - nx * ha};
    float qy[4] = {ay + ny * ha, by + ny * hb, by - ny * hb, ay - ny * ha};
    float ymin = qy[0], ymax = qy[0];
    for (int i = 1; i < 4; i++) { ymin = fminf(ymin, qy[i]); ymax = fmaxf(ymax, qy[i]); }
    int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
    if (y0 < 0) y0 = 0;
    if (y1 > H - 1) y1 = H - 1;
    const float nl = nx * lx + ny * ly;             /* light across the strut: + lights the +n side */
    const float sg = nl >= 0 ? 1 : -1, band = .25f - face * .8f;
    const bool strong = fabsf(nl) > .15f;
    for (int y = y0; y <= y1; y++) {
        float yc = y + .5f, xl = 1e9f, xr = -1e9f;
        for (int e = 0; e < 4; e++) {
            float px = qx[e], py = qy[e], rx = qx[(e + 1) & 3], ry = qy[(e + 1) & 3];
            if ((py <= yc && ry > yc) || (ry <= yc && py > yc)) {
                float x = px + (yc - py) * (rx - px) / (ry - py);
                xl = fminf(xl, x); xr = fmaxf(xr, x);
            }
        }
        if (xl > xr) continue;
        int x0 = (int)ceilf(xl - .5f), x1 = (int)floorf(xr - .5f);
        if (x0 < 0) x0 = 0;
        if (x1 > W - 1) x1 = W - 1;
        if (x0 > x1) continue;
        /* across (d) and along (s) are linear in x: step them */
        float rx = x0 + .5f - ax, ry = yc - ay;
        float d = rx * nx + ry * ny, s = rx * ux + ry * uy;
        float mid = (s + ((x1 - x0) * .5f) * ux) / len;
        float h = ha + (hb - ha) * fminf(1, fmaxf(0, mid)), inv = 1 / fmaxf(h, .5f);
        float edge = 1 - 1.1f * inv;              /* the outermost dot is the rim */
        for (int x = x0; x <= x1; x++, d += nx, s += ux) {
            float tl = d * inv * sg;                /* across, + toward the light */
            uint8_t ink;
            if (tl > edge) ink = strong ? LIT : GREY;           /* the lit rim */
            else if (tl < -edge) ink = GREY;                    /* the far rim, against space */
            else if (tl > band) ink = GREY;                     /* the lit half, solid */
            else ink = tl > band - fminf(.4f, 1.6f * inv) && .5f > lc_bayer(x, y) ? GREY : LAP_BLACK;   /* turning into shadow */
            if (tl <= edge && tl >= -edge) {
                if ((flags & CF_PANELS) && h > 2 && ((int)s % 18) == 9) ink = ink == GREY ? LAP_BLACK : GREY;
                if ((flags & CF_RIVETS) && h > 2.5f && tl < band && tl > band - .6f && ((int)s % 9) == 4 && s > 4) ink = LIT;
            }
            put(x, y, ink);
        }
    }
}

/* The glass's inner seal: a dotted line beside a strut, on the side toward the middle. */
static void rim(float ax, float ay, float bx, float by, float wa, float wb, float da, float db) {
    ax += sw_x * da; ay += sw_y * da; bx += sw_x * db; by += sw_y * db;
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    float nx = -dy / len, ny = dx / len;
    if ((W * .5f - ax) * nx + (H * .5f - ay) * ny < 0) { nx = -nx; ny = -ny; }
    float oa = wa * .5f + 2, ob = wb * .5f + 2;
    line(ax + nx * oa, ay + ny * oa, bx + nx * ob, by + ny * ob, GREY, 2);
}

/* The roof: a plate across the top, deeper at the sides (nearer) than over the
   middle, its lower edge lit, seams and rivets on request. */
static float roof_at(const cockpit_def *c, float x, float k) {
    float side = c->roof_side * k, mid = c->roof_mid * k, f = x / W;
    float u = f < .3f ? f / .3f : f > .7f ? (1 - f) / .3f : 1;
    return side + (mid - side) * u;
}
static void roof(const cockpit_def *c, float k) {
    if (!c->roof_side && !c->roof_mid) return;
    for (int x = 0; x < W; x++) {
        float sx = x - sw_x * .5f;
        int yb = UI_ROUND(roof_at(c, sx, k) + sw_y * .5f);
        for (int y = 0; y < yb && y < H; y++) {
            uint8_t ink = LAP_BLACK;
            if ((c->flags & CF_PANELS) && ((int)(sx + 999) % 24) == 0) ink = GREY;
            put(x, y, ink);
        }
        put(x, yb, ly < -.2f && (x & 1) ? LIT : GREY);           /* the edge, lit from below */
        if ((c->flags & CF_RIVETS) && yb > 3 && ((int)(sx + 999) % 8) == 4) put(x, yb - 2, LIT);
        if ((c->flags & CF_RIM) && (x & 1)) put(x, yb + 2, GREY);   /* the glass's seal */
    }
}

/* The dash's lip: where its top edge runs, x in dots. */
static float lip_at(const cockpit_def *c, float x, int rise) {
    float f = x / W, r = rise;
    switch (c->dash) {
    case DASH_COWL:                     /* a raised middle with sloped ends */
        if (f > .3f && f < .7f) r += 4;
        else if (f > .22f && f < .78f) r += 4 * (1 - fminf(fabsf(f - .3f), fabsf(f - .7f)) / .08f);
        break;
    case DASH_TWIN: {                   /* two binnacles either side of the middle */
        float a = fabsf(f - .34f), b = fabsf(f - .66f), m = fminf(a, b);
        if (m < .08f) r += 4 * cosf(m / .08f * CG_PI * .5f);
        break;
    }
    case DASH_NOTCH:                    /* low in the middle, for the nose */
        if (fabsf(f - .5f) < .12f) r -= fminf(2, (.12f - fabsf(f - .5f)) * 30);
        break;
    case DASH_ARC: {                    /* rising toward the sides: a sill that wraps */
        float u = (f - .5f) * 2;
        r += (wide ? 9 : 3) * u * u;
        break;
    }
    default: break;
    }
    return H - 1 - r;
}
static void dash(const cockpit_def *c, int rise) {
    for (int x = 0; x < W; x++) {
        float sx = x - sw_x;
        int yl = UI_ROUND(lip_at(c, sx, rise) + sw_y);
        float f = sx / W;
        float m = fabsf(f - .5f);                   /* the instruments face the pilot round the middle */
        for (int y = yl + 1; y < H; y++) put(x, y, LAP_BLACK);
        uint8_t ink = m < .1f ? (m < .06f || .5f > lc_bayer(x, yl) ? FHUD_HI : GLOW)
                    : m < .2f ? ((.2f - m) * 12 > lc_bayer(x, yl) ? GLOW : GREY)
                    : (ly < -.2f && (x & 1)) ? LIT : GREY;
        put(x, yl, ink);
        /* their light on the glass just above the lip: a few stray grains */
        uint32_t g = (uint32_t)x * 2654435761u;
        if (m < .16f && yl > 1 && ((g >> 24) & 255) < (unsigned)((.16f - m) * 520)) put(x, yl - 1, GLOW);
    }
}

/* The nose ahead of the glass, over the dash: a cone's back, lit on one side. */
static void nose(const cockpit_def *c, int rise, int tall) {
    if (tall <= 0) return;
    float cx = W * .5f + sw_x * .8f, base = lip_at(c, W * .5f, rise) + sw_y * .8f, half = tall * (wide ? 2.6f : 1.8f);
    const float sg = lx >= 0 ? 1 : -1;
    int pa = 0, pb = 0;
    for (int h = 0; h <= tall; h++) {
        float k = 1 - (float)h / tall, w = half * sqrtf(k);
        int y = UI_ROUND(base - h);
        int xa = UI_ROUND(cx - w), xb = UI_ROUND(cx + w);
        for (int x = xa; x <= xb; x++) {
            float tl = (x - cx) / fmaxf(w, .5f) * sg;
            uint8_t ink = tl > .55f - face * .5f ? GREY : tl > .2f - face * .5f && .5f > lc_bayer(x, y) ? GREY : LAP_BLACK;
            if (x == UI_ROUND(cx)) ink = GREY;                /* the seam along its back */
            put(x, y, ink);
        }
        if (h) {                                            /* the outline, unbroken */
            line(pa, y + 1, xa, y, sg < 0 ? (fabsf(lx) > .15f ? LIT : GREY) : GREY, 1);
            line(pb, y + 1, xb, y, sg > 0 ? (fabsf(lx) > .15f ? LIT : GREY) : GREY, 1);
        }
        pa = xa; pb = xb;
    }
    put(UI_ROUND(cx), UI_ROUND(base - tall - 1), GREY);   /* the probe on its tip */
}

/* A box hung from the roof over the middle: lamps along its face. */
static void overhead(const cockpit_def *c, float k) {
    float half = (c->kind == CK_PANO ? .16f : .09f) * (wide ? 1 : .8f);
    int x0 = UI_ROUND(W * (.5f - half) + sw_x * .5f), x1 = UI_ROUND(W * (.5f + half) + sw_x * .5f);
    int yb = UI_ROUND(fminf(7, c->roof_mid * k + (c->kind == CK_PANO ? 2 : 4)) + sw_y * .5f);
    for (int y = 0; y <= yb; y++)
        for (int x = x0; x <= x1; x++) {
            uint8_t ink = y == yb || x == x0 || x == x1 ? GREY : LAP_BLACK;
            if (y == yb - 2 && x > x0 + 1 && x < x1 - 1 && (x % 4) == 1) ink = GLOW;
            put(x, y, ink);
        }
    if (yb > 1) for (int x = x0 + 1; x < x1; x += 2) put(x, yb, ly < -.2f ? LIT : GREY);
}

/* The porthole: hull all round an oval of glass, its ring riveted. */
static void port(void) {
    float cx = W * .5f + sw_x * .6f, cy = H * (wide ? .46f : .47f) + sw_y * .6f;
    float rx = W * (wide ? .5f : .54f), ry = H * (wide ? .64f : .5f);
    for (int y = 0; y < H; y++) {
        float v = (y + .5f - cy) / ry;
        float xh = v * v < 1 ? rx * sqrtf(1 - v * v) : 0;
        int xa = UI_ROUND(cx - xh), xb = UI_ROUND(cx + xh);
        if (xa > W) xa = W;
        if (xb < -1) xb = -1;
        for (int x = 0; x < xa; x++) put(x, y, (x + y) % 9 == 0 && face * .5f > lc_bayer(x, y) ? GREY : LAP_BLACK);
        for (int x = xb + 1; x < W; x++) put(x, y, (x + y) % 9 == 0 && face * .5f > lc_bayer(x, y) ? GREY : LAP_BLACK);
    }
    int n = (int)(fmaxf(rx, ry) * 3);
    const float dc = cosf(2 * CG_PI / n), ds = sinf(2 * CG_PI / n);
    float ca = 1, sa = 0, px = cx + rx, py = cy;
    for (int i = 0; i < n; i++) {                   /* round the ring by turning a unit vector, no trig per step */
        float t = ca * dc - sa * ds;
        sa = sa * dc + ca * ds; ca = t;
        bool lit = ca * lx + sa * ly < -.2f;        /* the ring's inner face toward the light */
        float qx = cx + ca * rx, qy = cy + sa * ry;
        line(px, py, qx, qy, lit ? LIT : GREY, 1);
        px = qx; py = qy;
        if (i % 2 == 0) put(UI_ROUND(cx + ca * (rx - 3)), UI_ROUND(cy + sa * (ry - 2)), GREY);
        if (i % 8 == 4) put(UI_ROUND(cx + ca * (rx + 3)), UI_ROUND(cy + sa * (ry + 2)), LIT);
    }
}

/* A fighter's canopy bow: an arch from one side over the top to the other. */
static void hoop(float top, float foot, float x_in, float w) {
    const int n = 10;
    float px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
        float a = CG_PI * i / n;
        float x = W * .5f - cosf(a) * (W * (.5f - x_in)), y = top + (foot - top) * (1 - sinf(a));
        if (i) {
            float near0 = fabsf(cosf(CG_PI * (i - 1) / n)), near1 = fabsf(cosf(a));
            strut(px, py, x, y, w * (.6f + .4f * near0), w * (.6f + .4f * near1), near0, near1, 0);
        }
        px = x; py = y;
    }
}

/* The light: the sun across the screen, and how much of it falls on the
   frame's faces we see (most when it is behind us). */
static void light(void) {
    const game_view *v = game_state();
    l3_vec ts = l3_sub(v->contents.star, render_eye);
    float d = cg_len(ts);
    if (d < 1) { lx = -.5f; ly = -.5f; face = .4f; return; }
    l3_vec rt = l3_cross(render_fwd, render_up);
    float x = l3_dot(ts, rt) / d, y = -l3_dot(ts, render_up) / d, z = l3_dot(ts, render_fwd) / d;
    lx = x * .8f - .15f; ly = y * .8f - .25f;      /* a little cabin light from above left */
    face = cg_clamp(.38f - .3f * z, .12f, .7f);
}

static void sway(void) {
    const game_player *p = &game_state()->player;
    float tx = 0, ty = 0;
    if (sway_on && !p->motion_steer && !p->docked && !p->docking) {
        float k = ui.steer == 0 ? .5f : 1;
        tx = -p->yaw * 2.4f * k;                /* turning left throws the head right: the frame slides left */
        ty = (-p->pitch * 1.6f + (p->boost_left > 0 ? -1.5f : 0)) * k;
    }
    float a = fminf(1, ui.dt * 5);
    sw_x += (tx - sw_x) * a;
    sw_y += (ty - sw_y) * a;
}

/* Where words sit on the glass: the frame stays black under them. */
static void quiet_boxes(void) {
    nquiet = 0;
    const int bw = 2 * (ui.view.w < 60 ? 18 : 12);        /* SPD at the bottom left, the way to go at the right */
    quiet[nquiet++] = (ck_box){0, H - 6, bw, H};
    quiet[nquiet++] = (ck_box){W - 2 * 14, H - 6, W, H - 3};
    quiet[nquiet++] = (ck_box){0, 0, 2 * (wide ? 32 : 21), 12};   /* system and target, top left */
}

void ui_cockpit_draw(void) {
    dots = ui.dots;
    W = ui.dw; H = ui.dh;
    wide = ui.wide;
    if (!dots || W < 8 || H < 8) return;
    const game_class cls = game_player_class();
    const cockpit_def *c = &cockpits[cls >= 0 && cls < COCKPITS ? cls : COCKPITS - 1];
    light();
    sway();
    quiet_boxes();
    const float k = wide ? 1 : .85f;                        /* plates are shallower on a tall glass */
    if (c->kind == CK_PORT) {
        port();
        return;
    }
    const float pw = c->pw, pf = c->pf;
    const float asym_l = c->flags & CF_ASYM ? 1.5f : 1, asym_r = c->flags & CF_ASYM ? .7f : 1;
    if (wide) {
        /* pillars: up from the dash outside the speed and height tapes, then
           raked in over them to the roof as far as head says */
        const float yk = H * .2f;
        for (int s = 0; s < 2; s++) {
            float m = s ? asym_r : asym_l, w0 = pw * m, w2 = pf * m, w1 = w0 * .75f + w2 * .25f;
            float xb = W * c->foot / 100.f, xt = W * c->head / 100.f;
            float xk = fmaxf(xb, fminf(xb + (xt - xb) * .2f, W * .085f - w1 * .5f));
            float yt = roof_at(c, xt, k);
            if (s) { xb = W - 1 - xb; xt = W - 1 - xt; xk = W - 1 - xk; }
            strut(xb, H + 2, xk, yk, w0, w1, 1, .7f, c->flags);
            strut(xk, yk + 1.5f, xt, yt - 1, w1, w2, .7f, .45f, c->flags);
            if (c->flags & CF_RIM) {
                rim(xb, H + 2, xk, yk, w0, w1, 1, .7f);
                rim(xk, yk, xt, yt - 1, w1, w2, .7f, .45f);
            }
            if (c->bx && !(c->flags & CF_ASYM && !s)) {    /* corner braces, over the tapes */
                float bx = W * c->bx / 100.f, by = H * c->by / 100.f;
                if (s) bx = W - 1 - bx;
                strut(s ? W + 1 : -2, by, bx, -1, pw * .8f, pf * .8f, .9f, .4f, c->flags);
            }
        }
        if (c->flags & CF_ASYM)                             /* the patch: one brace, on the light side */
            strut(W + 1, H * .3f, W * .8f, -1, pw * .7f, pf * .7f, .9f, .4f, CF_RIVETS);
        static const float at2[2] = {.27f, .73f}, at4[4] = {.18f, .27f, .73f, .82f};
        for (int i = 0; i < c->posts && i < 4; i++) {   /* mullions, clear of the heading tape and the tapes */
            float x = W * (c->posts == 2 ? at2[i] : at4[i]);
            strut(x + (x - W * .5f) * .06f, H + 2, x, roof_at(c, x, k) - 1, 3, 2, .9f, .45f, 0);
        }
        if (c->flags & CF_CONSOLES)                         /* consoles low in the corners, under the tapes */
            for (int s = 0; s < 2; s++) {
                float x0 = W * c->foot / 100.f + pw * .5f, x1 = x0 + W * .15f;
                for (int x = (int)x0; x <= (int)x1; x++) {
                    float f = (x - x0) / (x1 - x0);
                    int top = UI_ROUND(H * (.83f + .08f * f * f) + sw_y);
                    int px = (s ? W - 1 - x : x) + UI_ROUND(sw_x);
                    put(px, top, ly < -.2f && (x & 1) ? LIT : GREY);
                    for (int y = top + 1; y < H; y++)
                        put(px, y, y == top + 2 && (x % 3) == 1 && f > .1f && f < .85f ? GLOW : LAP_BLACK);
                }
            }
    } else {
        /* a tall glass: corner struts only, above the tapes */
        for (int s = 0; s < 2; s++) {
            float m = s ? asym_r : asym_l;
            float hx = W * (c->bx ? c->bx : c->head) / 100.f, fy = H * (c->by ? fminf(c->by, 30) : 28) / 100.f;
            if (c->kind == CK_BRIDGE || c->kind == CK_PANO) hx = W * .14f;
            float yt = roof_at(c, hx, k);
            float hx2 = s ? W - 1 - hx : hx;
            strut(s ? W + 1 : -2, fy, hx2, yt - 1, pw * m, pf * m, 1, .45f, c->flags);
            if (c->flags & CF_RIM) rim(s ? W + 1 : -2, fy, hx2, yt - 1, pw * m, pf * m, 1, .45f);
        }
        if (c->flags & CF_ASYM) strut(W + 1, H * .18f, W * .72f, -1, pw * .6f, pf * .6f, .9f, .4f, CF_RIVETS);
    }
    if (c->flags & CF_HOOP)
        hoop(wide ? 2 : 3, H * (wide ? .2f : .28f), 0, pw);
    if (c->flags & CF_SEAM) line(W * .5f + sw_x * .4f, 0, W * .5f + sw_x * .4f, wide ? 5 : 6, GREY, 1);
    roof(c, k);
    if (c->flags & CF_OVERHEAD) overhead(c, k);
    if (c->flags & CF_PERISCOPE) {                         /* a sight block on the roof's middle */
        int x0 = UI_ROUND(W * .46f + sw_x * .5f), x1 = UI_ROUND(W * .54f + sw_x * .5f), yb = UI_ROUND(fminf(7, c->roof_mid * k + 3));
        for (int y = 0; y <= yb; y++)
            for (int x = x0; x <= x1; x++) put(x, y, y == yb || x == x0 || x == x1 ? GREY : y == yb - 2 && (x & 1) ? GLOW : LAP_BLACK);
    }
    int rise = wide ? c->rise : (c->rise > 2 ? 2 : c->rise);
    dash(c, rise);
    nose(c, rise, wide ? c->nose : c->nose * 2 / 3);
}
