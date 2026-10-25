/* The view through the canopy: the scene's dots copied out, space painted in
 * behind them (faint nebula, the sun's bloom, planets' air), then the HUD on top
 * in amber: canopy struts, reticle, the target's bracket, lasers, jump effects. */
#include "internal.h"
#include "art.h"
#include "ui.h"

#define CUBE 24
static uint8_t cube[6][CUBE * CUBE];    /* nebula density per direction, 0..255 */
static uint8_t neb_ink, neb_ink2;
static char line[64];

/* A direction to (face, texel). */
static inline int cube_at(float x, float y, float z) {
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    int face;
    float u, v, m;
    if (ax >= ay && ax >= az) { face = x > 0 ? 0 : 1; m = ax; u = z; v = y; }
    else if (ay >= az) { face = y > 0 ? 2 : 3; m = ay; u = x; v = z; }
    else { face = z > 0 ? 4 : 5; m = az; u = x; v = y; }
    int iu = (int)((u / m + 1) * (CUBE / 2)), iv = (int)((v / m + 1) * (CUBE / 2));
    iu = iu < 0 ? 0 : iu >= CUBE ? CUBE - 1 : iu;
    iv = iv < 0 ? 0 : iv >= CUBE ? CUBE - 1 : iv;
    return face * CUBE * CUBE + iv * CUBE + iu;
}

void ui_space_reset(uint32_t seed) {
    /* A few soft clouds on the sky, broken up by hash noise: faint, never bright. */
    l3_vec centre[4];
    float width[4], power[4];
    uint32_t r = seed ^ 0xBADA55;
    int clouds = 1 + (int)(cg_rand(&r) % 3);
    for (int i = 0; i < clouds; i++) {
        float y = cg_unit(&r) * 1.4f - .7f, a = cg_unit(&r) * 2 * CG_PI, h = sqrtf(1 - y * y);
        centre[i] = l3v(cosf(a) * h, y, sinf(a) * h);
        width[i] = .25f + cg_unit(&r) * .45f;
        power[i] = .5f + cg_unit(&r) * .5f;
    }
    static const uint8_t hues[][2] = {{LAP_BLUE, 12}, {LAP_MAGENTA, 13}, {LAP_RED, 9}, {LAP_CYAN, 14}};
    int hue = (int)(cg_rand(&r) % 4);
    ui_planets_reset();
    neb_ink = hues[hue][0];
    neb_ink2 = hues[hue][1];
    for (int f = 0; f < 6; f++)
        for (int v = 0; v < CUBE; v++)
            for (int u = 0; u < CUBE; u++) {
                float a = (u + .5f) / (CUBE / 2) - 1, b = (v + .5f) / (CUBE / 2) - 1;
                l3_vec d = f == 0 ? l3v(1, b, a) : f == 1 ? l3v(-1, b, a) : f == 2 ? l3v(a, 1, b)
                         : f == 3 ? l3v(a, -1, b) : f == 4 ? l3v(a, b, 1) : l3v(a, b, -1);
                d = l3_norm(d);
                float s = 0;
                for (int i = 0; i < clouds; i++) {
                    float c = 1 - l3_dot(d, centre[i]);
                    s += power[i] * expf(-c / (width[i] * width[i]) * 2.2f);
                }
                uint32_t n = cg_hash(seed + (uint32_t)(f * 977 + v * 31 + u));
                s *= .55f + (n & 255) / 255.f * .6f;
                cube[f][v * CUBE + u] = (uint8_t)(fminf(1, s) * 255);
            }
}

/* A box the dots keep to while one is set (the HUD glass for the ladder); else the whole view. */
static int clip_x0, clip_y0, clip_x1 = 1 << 20, clip_y1 = 1 << 20;
static inline void dot(int x, int y, uint8_t ink) {
    if (x >= 0 && y >= 0 && x < ui.dw && y < ui.dh && x >= clip_x0 && y >= clip_y0 && x < clip_x1 && y < clip_y1)
        ui.dots[y * ui.dw + x] = ink;
}
static void dline(int x0, int y0, int x1, int y1, uint8_t ink) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int n = 0; n < 2000; n++) {
        dot(x0, y0, ink);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static void dring(int cx, int cy, float rx, float ry, uint8_t ink, int step) {
    int n = (int)(fmaxf(rx, ry) * 6) + 8;
    for (int i = 0; i < n; i += step) {
        float a = i * 2 * CG_PI / n;
        dot(cx + UI_ROUND(cosf(a) * rx), cy + UI_ROUND(sinf(a) * ry), ink);
    }
}

/* Space behind the scene, only where the scene left the dots clear. */
static void paint_space(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    l3_vec f = render_fwd, up = render_up;             /* the camera the view was drawn from */
    l3_vec rt = l3_cross(f, up);
    float fy = ui.fy, fx = ui.fx, inv = 1 / fy, invx = 1 / fx;
    float cx = ui.dw * .5f, cy = ui.dh * .5f;
    /* the sun on screen, for its bloom */
    float sx = -1e9f, sy = 0, sr = 0;
    l3_vec ts = l3_sub(v->contents.star, render_eye);
    float tz = l3_dot(ts, f);
    if (tz > 0) {
        sx = cx + l3_dot(ts, rt) / tz * fx;
        sy = cy - l3_dot(ts, up) / tz * fy;
        sr = v->contents.star_radius / tz * fy * 3.2f + 6;
    }
    static const uint8_t star_ink[4] = {9, 11, 15, 12}, star_dim[4] = {LAP_RED, LAP_YELLOW, LAP_WHITE, LAP_BLUE};
    uint8_t s_hi = star_ink[v->contents.star_type & 3], s_lo = star_dim[v->contents.star_type & 3];
    /* The nebula is a 24-texel-a-face cube, a texel some 13 dots across at this
       field of view, so it is looked up every fourth dot of a row and held;
       the grain hash and the sun's bloom stay per dot. Most of the view is
       empty space, and this pass was the frame's largest after the engine. */
    static uint8_t neb[(256 * 2) / 4 + 2];   /* 256 columns: wider than any grid a board gives */
    int nstep = ui.dw / 4 + 1;
    if (nstep > (int)sizeof neb) nstep = (int)sizeof neb;
    for (int y = 0; y < ui.dh; y++) {
        float ry = (cy - y) * inv;
        l3_vec row = l3_add(f, l3_scale(up, ry));
        for (int i = 0; i < nstep; i++) {
            l3_vec dir = l3_add(row, l3_scale(rt, (i * 4 + 2 - cx) * invx));
            neb[i] = (&cube[0][0])[cube_at(dir.x, dir.y, dir.z)];   /* the six faces as one array */
        }
        float ddy = (y - sy) * 1.1f;
        bool sun_row = sr > 0 && fabsf(ddy) < sr;
        uint8_t *line = &ui.dots[y * ui.dw];
        for (int x = 0; x < ui.dw; x++) {
            uint8_t *d = &line[x];
            if (*d != LAP_DOT_CLEAR) continue;
            if (sun_row) {
                float ddx = x - sx;
                if (fabsf(ddx) < sr) {
                    float dd = sqrtf(ddx * ddx + ddy * ddy);
                    if (dd < sr) {
                        float k = 1 - dd / sr;
                        k = k * k * k;
                        if (k * .9f > lc_bayer(x, y)) { *d = k > .35f ? s_hi : s_lo; continue; }
                    }
                }
            }
            int nb = neb[(x >> 2) < nstep ? x >> 2 : nstep - 1];
            /* sparse: space stays black, the cloud only a hint */
            if (nb < 115) continue;                 /* .45 of 255: the clouds' cores only */
            /* grain, not a lattice: a fixed hash per dot reads as dust and glow */
            uint32_t g = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
            g ^= g >> 13; g *= 0x5bd1e995u; g ^= g >> 15;
            float b = (g & 1023) * (1 / 1023.f);
            float n = nb * (1 / 255.f);
            n = (n - .45f) * 1.8f;
            /* a hint, not a splatter: a few coloured grains in the densest part,
               a sparse grey dust around them (user, 2026-10-24: "a bit much") */
            if (n > .35f && n * .02f > b) *d = n > .8f && b < .006f ? neb_ink2 : neb_ink;
            else if (n * .045f > b) *d = 8;
        }
    }
    /* a thin band of air around the living worlds */
    for (int i = 0; i < v->contents.planet_count; i++) {
        int kind = render_planet_kind(i);
        if (kind != 0 && kind != 2) continue;
        const game_planet *g = &v->contents.planets[i];
        l3_vec t = l3_sub(g->position, p->position);
        float z = l3_dot(t, f);
        if (z <= g->radius) continue;
        float px = cx + l3_dot(t, rt) / z * fx, py = cy - l3_dot(t, up) / z * fy, pr = g->radius / z * fy;
        if (pr < 3 || pr > ui.dw * 3) continue;
        int n = (int)(pr * 7) + 12;
        for (int k = 0; k < n; k++) {
            float a = k * 2 * CG_PI / n;
            for (int j = 1; j <= 2; j++) {
                int x = UI_ROUND(px + cosf(a) * (pr + j)), y = UI_ROUND(py + sinf(a) * (pr + j));
                if (x < 0 || y < 0 || x >= ui.dw || y >= ui.dh) continue;
                uint8_t *d = &ui.dots[y * ui.dw + x];
                if (*d == LAP_DOT_CLEAR || *d == 8 || *d == neb_ink) *d = j == 1 ? 12 : LAP_BLUE;
            }
        }
    }
}

/* Where a world point lands on the view; false behind. */
bool ui_project(l3_vec w, float *x, float *y, float *z) {
    /* through the camera the view was drawn from: the cockpit's, or the chase camera's */
    l3_vec f = render_fwd, up = render_up;
    l3_vec rt = l3_cross(f, up), t = l3_sub(w, render_eye);
    float fy = ui.fy, fx = ui.fx;
    *z = l3_dot(t, f);
    float lx = l3_dot(t, rt), ly = l3_dot(t, up);
    if (*z <= .1f) { *x = lx; *y = ly; return false; }
    *x = ui.dw * .5f + lx / *z * fx;
    *y = ui.dh * .5f - ly / *z * fy;
    return true;
}

static void canopy(void) {
    int w = ui.dw, h = ui.dh;
    /* two struts down from the top corners, dark metal with a lit edge */
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 4; k++) {
            int x0 = s ? w - 1 - (w * 22 / 100) - k : (w * 22 / 100) + k, y0 = 0;
            int x1 = s ? w - 1 : 0, y1 = h * 30 / 100 + k;
            dline(x0, y0, x1, y1, k == 0 ? HUD_DIM : k == 3 ? LAP_BLACK : HUD_DIM);
        }
    /* the sill: a rail along the bottom, ticks every eighth */
    for (int x = 0; x < w; x++) {
        dot(x, h - 1, HUD_DIM);
        if (x % (w / 8) == 0) dot(x, h - 2, HUD);
    }
    if (!ui.wide) return;
    /* a desktop's wide glass gets the whole frame: pillars from the dash up
       to the roof, the roof bar, and a cowl over the middle of the dash */
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 5; k++) {
            int xb = w * 3 / 100 + k, xt = w * 11 / 100 + k;
            if (s) { xb = w - 1 - xb; xt = w - 1 - xt; }
            dline(xb, h - 1, xt, 0, k == 0 || k == 4 ? HUD_DIM : LAP_BLACK);
        }
    for (int x = w * 11 / 100; x < w - w * 11 / 100; x++) { dot(x, 0, HUD_DIM); dot(x, 1, LAP_BLACK); }
    const int c0 = w * 30 / 100, c1 = w - c0, ch = 5;   /* the cowl: raised, its ends sloped */
    for (int x = c0 - ch * 3; x <= c1 + ch * 3; x++) {
        int rise = x < c0 ? (x - (c0 - ch * 3)) / 3 : x > c1 ? ((c1 + ch * 3) - x) / 3 : ch;
        for (int y = h - 1 - rise; y < h - 1; y++) dot(x, y, LAP_BLACK);
        dot(x, h - 1 - rise, rise == ch ? HUD : HUD_DIM);
    }
}

static void reticle(void) {
    const game_view *v = game_state();
    int cx = ui.dw / 2, cy = ui.dh / 2;
    if (render_view == 1) {             /* chase: where the nose points, far ahead */
        float x, y, z;
        const game_player *p = &v->player;
        if (ui_project(l3_add(p->position, l3_scale(l3_quat_rotate(p->attitude, l3v(0, 0, -1)), 800)), &x, &y, &z)) {
            cx = (int)x;
            cy = (int)y;
        }
    }
    /* green while the guns are bending onto the target (aim assist) */
    uint8_t ink = v->aim_assisted ? HUD_OK : v->player.firing ? HUD_HI : HUD;
    for (int k = 3; k <= 6; k++) {
        dot(cx - k, cy, ink); dot(cx + k, cy, ink);
        dot(cx, cy - k, ink); dot(cx, cy + k, ink);
    }
    dot(cx, cy, HUD_DIM);
    /* turrets: a small ring where each one points */
    for (int m = 0; m < GAME_MOUNTS; m++) {
        float tx, ty, tz;
        if (!v->turret_on[m]) continue;
        if (ui_project(l3_add(v->player.position, l3_scale(v->turret_aim[m], 500)), &tx, &ty, &tz))
            dring((int)tx, (int)ty, 2, 2, HUD_COOL, 1);
    }
    /* the lead pip: where the shots meet the target; put it in the cross */
    float x, y, z;
    if (v->aim_valid && ui_project(v->aim_point, &x, &y, &z) && x > 2 && y > 2 && x < ui.dw - 2 && y < ui.dh - 2) {
        uint8_t pip = v->aim_assisted ? HUD_OK : HUD_HI;
        int ix = (int)x, iy = (int)y;
        dot(ix - 1, iy - 1, pip); dot(ix + 1, iy - 1, pip); dot(ix - 1, iy + 1, pip); dot(ix + 1, iy + 1, pip);
        dot(ix, iy, pip);
    }
}

static uint8_t contact_ink(const game_contact *c) {
    if (c->hostile) return HUD_WARN;
    if (c->role == ROLE_POLICE) return HUD_LAW;
    if (c->role == ROLE_BEACON) return HUD_OK;
    if (c->live) return HUD_COOL;
    return HUD_HI;
}

static void bracket(void) {
    const game_view *v = game_state();
    if (v->selected < 0 || !v->contacts[v->selected].active || (ui.hud_off & UI_HUD_TARGET)) return;
    const game_contact *c = &v->contacts[v->selected];
    float x, y, z;
    uint8_t ink = contact_ink(c);
    bool on = ui_project(c->position, &x, &y, &z);
    if (on && x >= 0 && y >= 0 && x < ui.dw && y < ui.dh) {
        const game_ship_stats *st = game_ship(c->ship_class);
        float fy = ui.fy;
        int r = (int)fmaxf(4, fminf(30, (st ? st->radius : 10) * .7f / z * fy)) + 2;
        int ix = (int)x, iy = (int)y, l = r / 2 + 1;
        for (int k = 0; k < l; k++) {
            dot(ix - r + k, iy - r, ink); dot(ix - r, iy - r + k, ink);
            dot(ix + r - k, iy - r, ink); dot(ix + r, iy - r + k, ink);
            dot(ix - r + k, iy + r, ink); dot(ix - r, iy + r - k, ink);
            dot(ix + r - k, iy + r, ink); dot(ix + r, iy + r - k, ink);
        }
        /* name and range under it, in cells */
        int col = ix / 2 - 4, row = (iy + r) / 3 + 1;
        if (row < ui.view.h - 1 && col >= 0) {
            snprintf(line, sizeof line, "%.0fm", c->range);
            col = col < ui.view.w - 8 ? col : ui.view.w - 8;
            ui_put(ui.view.x + col, ui.view.y + row, line, LAP_ATTR(ink, 0));
        }
        return;
    }
    /* off the view: an arrow on the edge pointing toward it */
    float ax = on ? x - ui.dw * .5f : x, ay = on ? y - ui.dh * .5f : -y;
    float len = sqrtf(ax * ax + ay * ay);
    if (len < 1e-3f) { ax = 0; ay = 1; len = 1; }
    ax /= len; ay /= len;
    float hx = ui.dw * .5f - 6, hy = ui.dh * .5f - 6;
    float k = fminf(hx / fmaxf(fabsf(ax), 1e-3f), hy / fmaxf(fabsf(ay), 1e-3f));
    int px = (int)(ui.dw * .5f + ax * k), py = (int)(ui.dh * .5f + ay * k);
    for (int i = 0; i < 6; i++) {
        float bx = px - ax * i, by = py - ay * i, wv = i * .6f;
        dline((int)(bx - ay * wv), (int)(by + ax * wv), (int)(bx + ay * wv), (int)(by - ax * wv), ink);
    }
}

static bool ladder_words;
static void ladder(void);
static void nav_marker(void);
static void jump_marker(void);
static char jump_words[2][40];      /* the jump mark's two lines, drawn after the dots */
static int jump_col, jump_row;
static uint8_t jump_ink;
static void toast(void);
static void interdiction(void);

static void lasers(void) {
    const game_player *p = &game_state()->player;
    bool on = false;
    for (int i = 0; i < CG_BEAMS; i++)
        if (cg->beams[i].life > 0 && cg->beams[i].player) on = true;
    if (!on || !p->firing) return;
    int cx = ui.dw / 2, cy = ui.dh / 2, h = ui.dh - 2;
    int j = (int)(ui.t * 30) & 1;
    for (int s = -1; s <= 1; s += 2) {
        int x0 = cx + s * (ui.dw / 2 - 4);
        for (int k = -1; k <= 1; k++)
            dline(x0 + k, h, cx + s * 1, cy + 2, k == 0 ? 15 : (k + j) & 1 ? 9 : LAP_RED);
    }
}

/* Hyperspace: rings rushing out of the middle, star streaks around them. */
static void tunnel(float t, bool launch) {
    memset(ui.dots, LAP_DOT_CLEAR, (size_t)ui.dw * ui.dh);
    int cx = ui.dw / 2, cy = ui.dh / 2;
    static const uint8_t jump_inks[] = {LAP_BLUE, 12, 14, 15, 13, LAP_MAGENTA};
    static const uint8_t launch_inks[] = {8, LAP_YELLOW, 11, 15};
    for (int i = 0; i < 9; i++) {
        float z = fmodf(t * 1.6f + i / 9.f, 1);
        float r = 2 + z * z * ui.dw * 1.1f;
        uint8_t ink = launch ? launch_inks[(i + (int)(t * 8)) % 4] : jump_inks[(i + (int)(t * 6)) % 6];
        if (launch) {   /* the slot's tunnel is square */
            int rx = (int)r, ry = (int)(r * .6f);
            for (int x = -rx; x <= rx; x++) { dot(cx + x, cy - ry, ink); dot(cx + x, cy + ry, ink); }
            for (int y = -ry; y <= ry; y++) { dot(cx - rx, cy + y, ink); dot(cx + rx, cy + y, ink); }
        } else
            dring(cx, cy, r, r * .82f, ink, 1);
    }
    if (!launch)
        for (int i = 0; i < 40; i++) {
            uint32_t r = cg_hash((uint32_t)i * 7919u);
            float a = (r & 1023) / 1023.f * 2 * CG_PI, s = fmodf(t * 2 + (r >> 10 & 255) / 255.f, 1);
            float r0 = 6 + s * s * ui.dw, r1 = r0 + 3 + s * 14;
            dline(cx + (int)(cosf(a) * r0), cy + (int)(sinf(a) * r0), cx + (int)(cosf(a) * r1),
                  cy + (int)(sinf(a) * r1), s > .5f ? 15 : LAP_WHITE);
        }
}

static void hit_flash(void) {
    if (ui.hit_flash <= 0) return;
    uint8_t ink = ui.hit_flash > .5f ? HUD_COOL : LAP_BLUE;
    const game_player *p = &game_state()->player;
    if (p->shield_fore < 1 && p->shield_aft < 1) ink = HUD_WARN;
    for (int x = 0; x < ui.dw; x += 1) { dot(x, 0, ink); if (x & 1) dot(x, 1, ink); }
    for (int y = 0; y < ui.dh; y++) { dot(0, y, ink); dot(ui.dw - 1, y, ink); }
}

static void stick_marker(void) {
    if (ui.grab != UI_G_VIEW || !ui.dragging) return;
    int cw, ch;
    lap_cell_px(&cw, &ch);
    /* finger pixels to the view's own dots */
    int x0 = ui.x0 * 2 / cw - ui.view.x * 2, y0 = ui.y0 * 3 / ch - ui.view.y * 3;
    int x = ui.x * 2 / cw - ui.view.x * 2, y = ui.y * 3 / ch - ui.view.y * 3;
    dring(x0, y0, 7, 7, HUD_DIM, 2);
    dline(x0, y0, x, y, HUD_DIM);
    dring(x, y, 1.5f, 1.5f, HUD, 1);
}

static void text_hud(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    static const char *legal[] = {"CLEAN", "OFFENDER", "FUGITIVE"};
    snprintf(line, sizeof line, "%s", v->systems[v->system].name);
    ui_put(ui.view.x + 1, ui.view.y + 0, line, LAP_ATTR(HUD_HI, 0));
    int x = ui.view.x + 2 + (int)strlen(line);
    ui_put(x, ui.view.y, legal[p->legal < 3 ? p->legal : 0], LAP_ATTR(p->legal ? HUD_WARN : HUD_SUB, 0));
    {
        int live = 0;
        for (int i = 0; i < GAME_CONTACTS; i++) live += v->contacts[i].active && v->contacts[i].live;
        int lx = x + (int)strlen(legal[p->legal < 3 ? p->legal : 0]) + 1;
        if (live) { snprintf(line, sizeof line, "LIVE %d", live); ui_put(lx, ui.view.y, line, LAP_ATTR(HUD_OK, 0)); lx += 7; }
        if (ui_net_count()) { snprintf(line, sizeof line, "MESH %d", ui_net_count()); ui_put(lx, ui.view.y, line, LAP_ATTR(HUD_COOL, 0)); }
    }
    if (v->selected >= 0 && v->contacts[v->selected].active) {
        const game_contact *c = &v->contacts[v->selected];
        const game_ship_stats *st = game_ship(c->ship_class);
        uint8_t ink = contact_ink(c);
        snprintf(line, sizeof line, "%s %s", c->name[0] ? c->name : c->callsign[0] ? c->callsign : "------",
                 st ? st->name : "");
        ui_put(ui.view.x + 1, ui.view.y + 1, line, LAP_ATTR(ink, 0));
        if (c->live)
            snprintf(line, sizeof line, "%s FL%03d %dkt", c->callsign, (int)(c->altitude_ft / 100),
                     c->aircraft_speed_kt);
        else
            snprintf(line, sizeof line, "%s", game_faction(c->faction));
        ui_put(ui.view.x + 1, ui.view.y + 2, line, LAP_ATTR(HUD_SUB, 0));
        /* its shields and hull, as two short rails */
        if (st && st->hull > 0) {
            float sh = st->shield > 0 ? c->shield / st->shield : 0, hu = c->hull / st->hull;
            ui_put(ui.view.x + 1, ui.view.y + 3, "S", LAP_ATTR(HUD_SUB, 0));
            ui_put(ui.view.x + 9, ui.view.y + 3, "H", LAP_ATTR(HUD_SUB, 0));
            for (int i = 0; i < 6; i++) {
                ui_cell(ui.view.x + 2 + i, ui.view.y + 3, i < (int)(sh * 6 + .5f) ? LAP_BRAILLE(0x1B) : LAP_BRAILLE(0x12),
                             LAP_ATTR(i < (int)(sh * 6 + .5f) ? HUD_COOL : HUD_DIM, 0));
                ui_cell(ui.view.x + 10 + i, ui.view.y + 3, i < (int)(hu * 6 + .5f) ? LAP_BRAILLE(0x1B) : LAP_BRAILLE(0x12),
                             LAP_ATTR(i < (int)(hu * 6 + .5f) ? (hu < .3f ? HUD_WARN : HUD) : HUD_DIM, 0));
            }
        }
    }
    /* speed and the way to go, low on each side of the sill */
    {
        const game_ship_stats *me = game_player_ship();
        if (p->speed >= 1000) snprintf(line, sizeof line, "SPD %.1fk", p->speed / 1000);
        else snprintf(line, sizeof line, p->reversing ? "REV %3.0f" : "SPD %3.0f", p->speed);
        int y = ui.view.y + ui.view.h - 2;
        ui_put(ui.view.x + 1, y, line, LAP_ATTR(me && p->speed > me->speed * .9f ? HUD_HI : HUD, 0));
        l3_vec goal = v->contents.station;
        const char *what = v->contents.station_kind ? "STN" : "STN";
        ui_dest nd;
        if (ui_nav_target(&nd)) { goal = nd.position; what = "NAV"; }
        else if (v->selected >= 0 && v->contacts[v->selected].active) { goal = v->contacts[v->selected].position; what = "TGT"; }
        float d = cg_len(l3_sub(goal, p->position));
        if (d >= 1000) snprintf(line, sizeof line, "%s %.1fkm", what, d / 1000);
        else snprintf(line, sizeof line, "%s %.0fm", what, d);
        ui_put(ui.view.x + ui.view.w - 1 - (int)strlen(line), y, line, LAP_ATTR(HUD, 0));
        /* closing on something solid: a warning that cannot be missed */
        float near = 1e9f;
        for (int i = 0; i < v->contents.planet_count; i++)
            near = fminf(near, cg_len(l3_sub(p->position, v->contents.planets[i].position)) - v->contents.planets[i].radius);
        if (near < p->speed * 6 && near < 600 && ((int)(ui.t * 4) & 1))
            lap_put_str(ui.view.x + (ui.view.w - 13) / 2, ui.view.y + ui.view.h / 2 + 5, "PULL UP  ALT", LAP_ATTR(LAP_BLACK, HUD_WARN));
    }
    /* what is happening, low in the middle */
    const char *mid = NULL;
    if (v->jump == JUMP_CHARGE) {
        snprintf(line, sizeof line, "HYPERSPACE %.0f", fmaxf(1, ceilf(3 - v->jump_progress)));
        mid = line;
    } else if (p->docking) mid = "DOCKING COMPUTER ON";
    else if (game_state()->interdiction.active) mid = "INTERDICTION: HOLD THE MARKER";
    else if (p->cruising) {
        snprintf(line, sizeof line, "%sCRUISE x%.0f", ui.autopilot ? "AUTO " : "", p->cruise_factor);
        mid = line;
    } else if (p->cruise_charge > 0 && p->cruise_charge < 1) {
        snprintf(line, sizeof line, "CRUISE SPOOLING %d%%", (int)(p->cruise_charge * 100));
        mid = line;
    }
    else if (ui.autopilot) mid = "AUTOPILOT";
    else if (p->mass_locked && v->plotted >= 0) mid = "MASS LOCKED";
    else if (v->rescue_progress > 0 && v->rescue_progress < 1) {
        snprintf(line, sizeof line, "SUPPLIES %d%%", (int)(v->rescue_progress * 100));
        mid = line;
    } else if (ui.scoop_t < .5f) {
        snprintf(line, sizeof line, "SCOOPING FUEL %.1f ly  HEAT %d%%", p->fuel, (int)(p->cabin_heat * 100));
        mid = line;
    } else {
        static char pbuf[32];
        mid = ui_poi_status(pbuf, sizeof pbuf);
    }
    if (mid) {
        int n = (int)strlen(mid);
        ui_put(ui.view.x + (ui.view.w - n) / 2, ui.view.y + ui.view.h - 3, mid, LAP_ATTR(HUD_HI, 0));
    }
}

/* HUD words over the view sit on what is drawn there: each letter takes the
   commonest of its cell's six dots as its ground, so a label over a planet or
   a hull reads on that colour instead of punching a black box in the scene. */
uint8_t ui_ground(int col, int row, uint8_t fg) {
    int x = col - ui.view.x, y = row - ui.view.y;
    if (!ui.dots || x < 0 || y < 0 || x >= ui.view.w || y >= ui.view.h) return 0;
    int count[16] = {0}, best = 0;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 2; i++) {
            uint8_t d = ui.dots[(y * 3 + j) * ui.dw + x * 2 + i];
            count[d == LAP_DOT_CLEAR ? 0 : d & 15]++;
        }
    for (int k = 1; k < 16; k++)
        if (count[k] > count[best]) best = k;
    return best == (fg & 15) ? 0 : (uint8_t)best;
}
void ui_put(int x, int y, const char *s, uint8_t attr) {
    if (attr >> 4) { lap_put_str(x, y, s, attr); return; }
    for (; *s; s++, x++)            /* a space leaves the scene as it is */
        if (*s != ' ') lap_put_cell(x, y, (uint8_t)*s, LAP_ATTR(attr, ui_ground(x, y, attr)));
}
void ui_cell(int x, int y, int16_t ch, uint8_t attr) {
    lap_put_cell(x, y, ch, attr >> 4 ? attr : LAP_ATTR(attr, ui_ground(x, y, attr)));
}

void ui_view_draw(void) {
    const game_view *v = game_state();
    l3_scene *sc = ui.scene;
    if (!sc) return;
    if (v->player.docked && ui.dock_t > 0) {   /* arriving: in through the slot, then the bay */
        render_tunnel(sc, 1 - ui.dock_t / UI_DOCK_S, false);
        memcpy(ui.dots, l3_dots(sc), (size_t)ui.dw * ui.dh);
        canopy();
        lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, ui.dots);
        ui_put(ui.view.x + (ui.view.w - 7) / 2, ui.view.y + ui.view.h / 2 + 6, "DOCKING", LAP_ATTR(HUD_HI, 0));
        return;
    }
    if (v->player.docked) {
        render_hangar(sc, ui.t);
        memcpy(ui.dots, l3_dots(sc), (size_t)ui.dw * ui.dh);
        lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, ui.dots);
        snprintf(line, sizeof line, "DOCKED  %s, %s", v->contents.station_name[0] ? v->contents.station_name : "STATION",
                 v->systems[v->system].name);
        ui_put(ui.view.x + 1, ui.view.y + 0, line, LAP_ATTR(HUD_HI, 0));
        ui_put(ui.view.x + 1, ui.view.y + 1, v->systems[v->system].description, LAP_ATTR(HUD_SUB, 0));
        const game_ship_stats *me = game_player_ship();
        if (me) {
            int n = (int)strlen(me->name);
            ui_put(ui.view.x + (ui.view.w - n) / 2, ui.view.y + ui.view.h * 3 / 4, me->name, LAP_ATTR(HUD_HI, 0));
        }
        return;
    }
    if (ui.launch_t > 0) {                      /* out along the slot */
        render_tunnel(sc, 1 - ui.launch_t / UI_LAUNCH_S, true);
        memcpy(ui.dots, l3_dots(sc), (size_t)ui.dw * ui.dh);
        canopy();
        lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, ui.dots);
        ui_put(ui.view.x + (ui.view.w - 9) / 2, ui.view.y + ui.view.h / 2 + 6, "LAUNCHING", LAP_ATTR(HUD_HI, 0));
        return;
    }
    if (v->jump == JUMP_TUNNEL) {
        tunnel(v->jump_progress * 4, false);
        lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, ui.dots);
        snprintf(line, sizeof line, "HYPERSPACE  %s", v->plotted >= 0 ? v->systems[v->plotted].name : "");
        ui_put(ui.view.x + (ui.view.w - (int)strlen(line)) / 2, ui.view.y + ui.view.h - 3, line, LAP_ATTR(HUD_HI, 0));
        return;
    }
    render_view = ui.view_mode == UI_VIEW_CHASE;
    if (ui.view_mode == UI_VIEW_TACTICAL) {     /* the plot instead of the scene: no engine frame at all */
        ui_tactical_paint();
        lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, ui.dots);
        ui_tactical_words();
        text_hud();
        toast();
        return;
    }
    game_render(sc);
    memcpy(ui.dots, l3_dots(sc), (size_t)ui.dw * ui.dh);
    ui_planets_paint();
    ui_poi_paint();
    paint_space();
    if (render_view == 0) canopy();     /* struts and sill belong to the cockpit */
    ui_weapons_dots();
    ui_incoming_dots();
    ladder_words = false;
    if (render_view == 0) ladder();
    ui_halos();
    ui_holo_paint();
    ui_damage_dots(ui.dt);
    nav_marker();
    jump_marker();
    reticle();
    hit_flash();
    interdiction();
    stick_marker();
    lap_dots(ui.view.x, ui.view.y, ui.view.w, ui.view.h, ui.dots);
    bracket();      /* draws a word in cells, so after the dots */
    ui_poi_labels();
    ui_holo_draw();
    ui_combat_words();
    if (ui.nav_label[0] && ui.nav_label_row < ui.view.h - 1)
        ui_put(ui.view.x + (ui.nav_label_col < 0 ? 0 : ui.nav_label_col), ui.view.y + ui.nav_label_row, ui.nav_label,
                    LAP_ATTR(HUD_COOL, 0));
    for (int i = 0; i < 2 && jump_words[0][0]; i++) {
        int n = (int)strlen(jump_words[i]), c = jump_col - n / 2;
        if (jump_row + i < ui.view.h - 1)
            ui_put(ui.view.x + (c < 0 ? 0 : c), ui.view.y + jump_row + i, jump_words[i], LAP_ATTR(i ? jump_ink : HUD_HI, 0));
    }
    ladder_words = true;
    if (render_view == 0) ladder();
    text_hud();
    toast();
}

/* Explosion flashes follow the game's debris bursts. */
void ui_effects_update(float dt) {
    render_tick(dt);
    ui_shake_update(dt);
    ui_zoom_update(dt);
    int fresh = 0;
    l3_vec at = l3v(0, 0, 0);
    for (int i = 0; i < CG_DEBRIS; i++)
        if (cg->debris[i].life > .97f) { at = l3_add(at, cg->debris[i].p); fresh++; }
    if (fresh >= 4 && ui.debris_seen == 0) {
        render_blast(l3_scale(at, 1.f / fresh), 14);
        ui_sound_far_boom();
        lap_vibrate(120, 60);
    }
    ui.debris_seen = fresh;
    if (ui.hit_flash > 0) ui.hit_flash -= dt * 3;
    if (ui.launch_t > 0) ui.launch_t -= dt;
    if (ui.dock_t > 0) ui.dock_t -= dt;
    const game_player *p = &game_state()->player;
    if (p->shield_fore < ui.last_fore - .5f || p->shield_aft < ui.last_aft - .5f) {
        ui.hit_flash = 1;
        lap_vibrate(40, 40);
    }
    ui.last_fore = p->shield_fore;
    if (p->fuel > ui.fuel_seen + 1e-4f && !p->docked) ui.scoop_t = 0;
    else ui.scoop_t += dt;
    ui.fuel_seen = p->fuel;
    ui.last_aft = p->shield_aft;
    if (ui.was_docked && !p->docked) ui.launch_t = UI_LAUNCH_S;
    if (!ui.was_docked && p->docked && ui.launch_t <= 0) ui.dock_t = UI_DOCK_S;
    ui.was_docked = p->docked;
}

/* The way out, part of the cockpit: a hazard placard on the right strut, and
   when tapped, a plain question with two big answers. */
void ui_exit_draw(void) {
    const ui_rect *r = &ui.close;
    if (game_state()->panel != PANEL_NONE && !ui.quit_ask) return;   /* panels have CLOSE there */
    /* VIEW under it: cockpit, chase camera, tactical plot */
    if (!ui.quit_ask && ui.viewkey.w > 0) {     /* docked, the same placard takes you out of the seat */
        static const char *const name[3] = {"COCKPIT", " CHASE ", "TACTICAL"};
        const bool docked = game_state()->player.docked;
        const ui_rect *k = &ui.viewkey;
        lap_fill(k->x, k->y, k->w, k->h, ' ', LAP_ATTR(HUD, LAP_BLACK));
        lap_hline(k->x, k->y, k->w, LAP_BRAILLE(0x09), LAP_ATTR(HUD_DIM, LAP_BLACK));
        const char *top = docked ? "WALK" : "VIEW", *s = docked ? "ON FOOT" : name[ui.view_mode % 3];
        lap_put_str(k->x + (k->w - (int)strlen(top)) / 2, k->y + 1, top, LAP_ATTR(HUD_SUB, LAP_BLACK));
        lap_put_str(k->x + (k->w - (int)strlen(s)) / 2, k->y + 2, s,
                    LAP_ATTR(docked ? HUD_OK : ui.view_mode ? HUD_COOL : HUD, LAP_BLACK));
    }
    for (int i = 0; i < r->w; i++) {
        lap_put_cell(r->x + i, r->y, (i & 1) ? '/' : ' ', LAP_ATTR(LAP_BLACK, HUD));
        lap_put_cell(r->x + i, r->y + 2, (i & 1) ? ' ' : '/', LAP_ATTR(LAP_BLACK, HUD));
    }
    lap_fill(r->x, r->y + 1, r->w, 1, ' ', LAP_ATTR(HUD_HI, LAP_BLACK));
    lap_put_str(r->x + (r->w - 4) / 2, r->y + 1, "EXIT", LAP_ATTR(HUD_HI, LAP_BLACK));
    if (!ui.quit_ask) return;
    int x = ui.quit_stay.x - 1, w = ui.quit_leave.x + ui.quit_leave.w + 1 - x, y = ui.quit_stay.y - 4;
    lap_fill(x, y, w, 9, ' ', LAP_ATTR(HUD, LAP_BLACK));
    lap_hline(x, y, w, LAP_BRAILLE(0x09), LAP_ATTR(HUD, LAP_BLACK));
    lap_hline(x, y + 8, w, LAP_BRAILLE(0xC0), LAP_ATTR(HUD, LAP_BLACK));
    lap_put_str(x + (w - 18) / 2, y + 1, "LEAVE THE COCKPIT?", LAP_ATTR(HUD_HI, LAP_BLACK));
    lap_put_str(x + (w - 24) / 2, y + 2, "Your commander is saved.", LAP_ATTR(HUD_SUB, LAP_BLACK));
    ui_key_box(&ui.quit_stay, "STAY", "", HUD, LAP_BLACK, false);
    ui_key_box(&ui.quit_leave, "LEAVE", "", HUD_WARN, LAP_BLACK, true);
}

/* The keyboard card, on ?: two columns, the key in amber and what it does. */
void ui_keys_draw(void) {
    if (!ui.keys_help) {
        if (ui.wide && game_state()->panel == PANEL_NONE)
            lap_put_str(ui.view.x + 1, ui.view.y + ui.view.h - 1, "? keys", LAP_ATTR(HUD_SUB, LAP_BLACK));
        return;
    }
    static const char *const keys[][2] = {
        {"mouse", "drag: pitch and roll"}, {"A / D", "yaw left / right"}, {"W / S, wheel", "throttle"},
        {"X", "zero throttle"},          {"Q / E", "thrust left / right"}, {"R / F", "thrust up / down"},
        {"TAB", "boost"},                {"J", "frame shift / cruise"}, {"L", "dock, land, launch"},
        {"T", "target ahead"},           {"G", "next target"},       {"H", "highest threat"},
        {"Y", "subsystem"},              {"V", "heat sink"},         {"arrows", "power pips, reset"},
        {"1 - 4", "panels"},             {"C", "next dash page"},    {"SPACE", "fire"},
        {"B", "secondary fire"},         {"M", "missile"},           {"5", "chaff"},
        {"6", "shield cell"},            {"7", "ECM"},               {"P", "autopilot"},
        {"O", "view"},                   {"\\", "free camera"},      {"[ ] - =", "swing the camera"},
        {"BACKSPACE", "close panel"},
    };
    const int n = (int)(sizeof keys / sizeof keys[0]), half = (n + 1) / 2;
    const int w = 72 < ui.view.w - 2 ? 72 : ui.view.w - 2, h = half + 4;
    const int x = ui.view.x + (ui.view.w - w) / 2, y = ui.view.y + (ui.view.h - h) / 2;
    lap_fill(x, y, w, h, ' ', LAP_ATTR(HUD, LAP_BLACK));
    lap_hline(x, y, w, LAP_BRAILLE(0x09), LAP_ATTR(HUD, LAP_BLACK));
    lap_hline(x, y + h - 1, w, LAP_BRAILLE(0xC0), LAP_ATTR(HUD, LAP_BLACK));
    lap_put_str(x + 2, y + 1, "KEYS", LAP_ATTR(HUD_HI, LAP_BLACK));
    lap_put_str(x + w - 22, y + 1, "any key or tap closes", LAP_ATTR(HUD_SUB, LAP_BLACK));
    for (int i = 0; i < n; i++) {
        int cx = x + 2 + (i >= half) * (w / 2), cy = y + 3 + i % half;
        lap_put_str(cx, cy, keys[i][0], LAP_ATTR(HUD_HI, LAP_BLACK));
        lap_put_str(cx + 13, cy, keys[i][1], LAP_ATTR(HUD, LAP_BLACK));
    }
}

/* Comms: a hailed ship talks back in the lower part of the view, with two or
   three answers as big keys. */
void ui_talk_draw(void) {
    const game_view *v = game_state();
    ui.talk_n = 0;
    if (!v->talk.open || v->panel != PANEL_NONE || v->player.docked) return;
    const game_talk *t = &v->talk;
    const game_contact *c = t->who >= 0 && t->who < GAME_CONTACTS ? &v->contacts[t->who] : NULL;
    int h = 10, y = ui.view.y + ui.view.h - h - 1, x = ui.view.x + 1, w = ui.view.w - 2;
    lap_fill(x, y, w, h, ' ', LAP_ATTR(HUD, LAP_BLACK));
    lap_hline(x, y, w, LAP_BRAILLE(0x09), LAP_ATTR(HUD, LAP_BLACK));
    uint8_t ink = c ? contact_ink(c) : HUD_HI;
    const game_ship_stats *st = c ? game_ship(c->ship_class) : NULL;
    snprintf(line, sizeof line, "COMMS  %s %s", c && c->name[0] ? c->name : c ? c->callsign : "",
             st ? st->name : "");
    lap_put_str(x + 1, y + 1, line, LAP_ATTR(ink, LAP_BLACK));
    for (int i = 0; i < t->line_count && i < 3; i++)
        lap_put_str(x + 2, y + 2 + i, t->lines[i], LAP_ATTR(HUD_HI, LAP_BLACK));
    int n = t->choice_count < 1 ? 1 : t->choice_count > 3 ? 3 : t->choice_count;
    int kx = x;
    for (int i = 0; i < n; i++) {
        int kw = x + w * (i + 1) / n - kx;
        ui.talk_keys[i] = (ui_rect){kx, y + h - 4, kw, 4};
        ui_key_box(&ui.talk_keys[i], t->choice_count ? t->choices[i] : "CLOSE", "", i ? HUD : HUD_HI,
                   LAP_BLACK, false);
        kx += kw;
    }
    ui.talk_n = n;
}

/* Destroyed: the canopy goes red and one key brings back the last save. */
void ui_dead_draw(void) {
    const game_view *v = game_state();
    if (!v->player.dead) return;
    int w = ui.view.w - 10 < 34 ? ui.view.w - 10 : 34, x = ui.view.x + (ui.view.w - w) / 2;
    int y = ui.view.y + ui.view.h / 2 - 5;
    lap_fill(x, y, w, 10, ' ', LAP_ATTR(HUD_WARN, LAP_BLACK));
    lap_hline(x, y, w, LAP_BRAILLE(0x09), LAP_ATTR(HUD_WARN, LAP_BLACK));
    lap_put_str(x + (w - 14) / 2, y + 1, "SHIP DESTROYED", LAP_ATTR(HUD_WARN, LAP_BLACK));
    snprintf(line, sizeof line, "%.*s", w - 2, v->messages[0]);
    lap_put_str(x + (w - (int)strlen(line)) / 2, y + 3, line, LAP_ATTR(HUD_SUB, LAP_BLACK));
    ui.dead_key = (ui_rect){x + 2, y + 5, (w - 4) / 2, 4};
    ui.rebuy_key = (ui_rect){x + 2 + (w - 4) / 2, y + 5, w - 4 - (w - 4) / 2, 4};
    char cost[24];
    snprintf(cost, sizeof cost, "%u cr", (unsigned)game_insurance_cost());
    ui_key_box(&ui.dead_key, "LAST SAVE", "", HUD, LAP_BLACK, false);
    ui_key_box(&ui.rebuy_key, "REBUY", cost, HUD_HI, LAP_BLACK, true);
}

/* ---- the approach ladder ------------------------------------------------
 * Near a world (or the station), amber rungs show your pitch against its
 * surface: the horizon long and bright, then every ten degrees up and down,
 * with altitude and climb or sink rate on the right, so coming in to land is
 * a matter of watching the rungs and the numbers. */

static void hud_tapes(const game_player *p, l3_vec local_up, l3_vec f, l3_vec rt, float alt);

/* Seven-segment numerals: a line per lit segment, so a number turned with its
   rung stays clean at any angle. Bits a b c d e f g, a the top bar. */
static const uint8_t LADDER_SEGS[11] = {0x7E, 0x30, 0x6D, 0x79, 0x33, 0x5B, 0x5F, 0x70, 0x7F, 0x7B, 0x01};

/* A number centred on (x, y), running along (tx, ty); it never reads upside down. */
static void ladder_number(float x, float y, float tx, float ty, int value, uint8_t ink) {
    static const int8_t seg[7][4] = {{0, 0, 4, 0}, {4, 0, 4, 4}, {4, 4, 4, 8}, {0, 8, 4, 8},
                                     {0, 4, 0, 8}, {0, 0, 0, 4}, {0, 4, 4, 4}};
    char s[8];
    snprintf(s, sizeof s, "%d", value);
    if (tx < 0) { tx = -tx; ty = -ty; }
    const float nx = -ty, ny = tx;                 /* down the glyph, on screen */
    const int n = (int)strlen(s);
    const float w = n * 6 - 2;
    const float ox = x - tx * w * .5f - nx * 4, oy = y - ty * w * .5f - ny * 4;
    for (int i = 0; i < n; i++) {
        const uint8_t bits = LADDER_SEGS[s[i] == '-' ? 10 : s[i] - '0'];
        for (int k = 0; k < 7; k++) {
            if (!(bits & (0x40 >> k))) continue;
            float gx0 = i * 6 + seg[k][0], gy0 = seg[k][1], gx1 = i * 6 + seg[k][2], gy1 = seg[k][3];
            dline(UI_ROUND(ox + tx * gx0 + nx * gy0), UI_ROUND(oy + ty * gx0 + ny * gy0),
                  UI_ROUND(ox + tx * gx1 + nx * gy1), UI_ROUND(oy + ty * gx1 + ny * gy1), ink);
        }
    }
}

/* A point on the sky: `deg` above the local horizon, `az` degrees round from the
   nose's bearing, projected through the cockpit; false behind. */
static bool sky_at(l3_vec north, l3_vec east, l3_vec up, float deg, float az, float *x, float *y) {
    const float e = deg * CG_RAD, a = az * CG_RAD;
    l3_vec d = l3_add(l3_scale(up, sinf(e)),
                      l3_scale(l3_add(l3_scale(north, cosf(a)), l3_scale(east, sinf(a))), cosf(e)));
    float z;
    return ui_project(l3_add(render_eye, l3_scale(d, 1000)), x, y, &z);
}

static void ladder(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    /* the body you are closest to, if close */
    l3_vec centre = v->contents.station;
    float radius = 45, alt = cg_len(l3_sub(p->position, centre)) - radius;
    bool world = false;
    for (int i = 0; i < v->contents.planet_count; i++) {
        const game_planet *g = &v->contents.planets[i];
        float a = cg_len(l3_sub(p->position, g->position)) - g->radius;
        if (a < fmaxf(g->radius * 1.5f, 1200) && a < alt) { centre = g->position; radius = g->radius; alt = a; world = true; }
    }
    if (!world && alt > 900) return;
    if (world && alt > fmaxf(radius * 1.5f, 1200)) return;
    l3_vec f = l3_quat_rotate(p->attitude, l3v(0, 0, -1));
    l3_vec rt = l3_cross(f, l3_quat_rotate(p->attitude, l3v(0, 1, 0))), up = l3_norm(l3_sub(p->position, centre));
    if (ladder_words) { if (!(ui.hud_off & UI_HUD_TAPES)) hud_tapes(p, up, f, rt, alt); return; }
    if (ui.hud_off & UI_HUD_LADDER) return;
    /* The rungs are real circles on the sky, ten degrees apart, each drawn as an
       arc round the nose's bearing and projected like everything else, so they
       scroll through straight up and straight down without a jump. Near a pole
       the bearing turns fast, so the circles round it are drawn whole. */
    static l3_vec last_north = {0, 0, -1};
    const float sp = cg_clamp(l3_dot(render_fwd, up), -1, 1);
    l3_vec north = l3_sub(render_fwd, l3_scale(up, sp));
    if (cg_len(north) > 1e-3f) last_north = north = l3_norm(north);
    else north = l3_norm(l3_sub(last_north, l3_scale(up, l3_dot(last_north, up))));
    const l3_vec east = l3_cross(north, up);
    const float pole_deg = acosf(fabsf(sp)) / CG_RAD;   /* the nose's distance from straight up or down */
    /* the HUD glass: the middle of the view, clear of the heading tape and the dash cowl */
    clip_x0 = ui.dw * 26 / 100; clip_x1 = ui.dw - clip_x0;
    clip_y0 = 12; clip_y1 = ui.dh - (ui.wide ? 9 : 3);
    for (int deg = -90; deg <= 90; deg += 10) {
        uint8_t ink = deg == 0 ? FHUD_HI : FHUD;
        float x0, y0;
        if (deg == 90 || deg == -90) {          /* the pole: a small ring, open below for nadir */
            if (!sky_at(north, east, up, deg, 0, &x0, &y0)) continue;
            for (int k = 0; k < 24; k++) {
                if (deg < 0 && (k % 3) == 2) continue;
                float a = k * CG_PI / 12;
                dot(UI_ROUND(x0 + cosf(a) * 4), UI_ROUND(y0 + sinf(a) * 4), ink);
            }
            dot(UI_ROUND(x0), UI_ROUND(y0), ink);
            continue;
        }
        const float c = cosf(deg * CG_RAD);
        const bool ring = pole_deg < 20 && deg * sp > 0 && abs(deg) >= 60;
        const float half = ring ? 180 : fminf(180, (deg == 0 ? 14.f : 8.f) / c);
        const float gap = ring ? 0 : (deg == 0 ? 1.5f : 3.f) / c;
        const int steps = ring ? 96 : 40;
        for (int side = -1; side <= 1; side += 2) {
            float px = 0, py = 0;
            bool had = false;
            for (int i = 0; i <= steps; i++) {
                float az = ring ? -180 + 360.f * i / steps : side * (gap + (half - gap) * i / steps), x, y;
                bool on = sky_at(north, east, up, (float)deg, az, &x, &y);
                /* above the horizon long dashes, below it short ones: up and down never confuse */
                bool draw = deg >= 0 ? (i % 6) != 5 : (i % 3) != 2;
                if (on && had && draw && fabsf(x - px) + fabsf(y - py) < ui.dw * .5f) dline(UI_ROUND(px), UI_ROUND(py), UI_ROUND(x), UI_ROUND(y), ink);
                px = x; py = y; had = on;
            }
            if (ring) break;                    /* a whole circle has one side */
            if (!had || deg == 0) continue;
            /* the end tips toward the horizon, then the number beyond it, turned with the rung */
            float tx, ty, ax, ay;
            if (sky_at(north, east, up, deg - (deg > 0 ? 1.2f : -1.2f), side * half, &tx, &ty))
                dline(UI_ROUND(px), UI_ROUND(py), UI_ROUND(tx), UI_ROUND(ty), ink);
            if (sky_at(north, east, up, (float)deg, side * (half + 7.f / c), &ax, &ay)) {
                float dx = ax - px, dy = ay - py, len = sqrtf(dx * dx + dy * dy);
                if (len > .5f) ladder_number(ax, ay, side * dx / len, side * dy / len, deg, FHUD_HI);
            }
        }
    }
    clip_x0 = clip_y0 = 0;
    clip_x1 = clip_y1 = 1 << 20;
}

/* The flying tapes: heading across the top, speed down the left, height and
   climb down the right. Each scrolls under a fixed box with the value in it. */
static void tape_v(int col, int mid, int rows, float value, float step, bool right, const char *title, uint8_t ink) {
    int top = mid - rows / 2;
    char t[12];
    for (int r = 0; r < rows; r++) {
        float at = value + (mid - (top + r)) * step;          /* the value this row stands for */
        float snap = floorf(at / step + .5f) * step;
        bool major = fmodf(fabsf(snap), step * 2) < step * .5f;
        if (top + r == mid) continue;
        if (major && snap >= 0) {
            if (step >= 1000) snprintf(t, sizeof t, "%.0fk", snap / 1000);
            else snprintf(t, sizeof t, "%.0f", snap);
            ui_put(right ? col + 2 : col + 5 - (int)strlen(t), top + r, t, LAP_ATTR(FHUD, 0));
        }
        ui_put(right ? col : col + 6, top + r, major ? "-" : ".", LAP_ATTR(FHUD, 0));
    }
    if (value >= 10000) snprintf(t, sizeof t, "%.1fk", value / 1000);
    else snprintf(t, sizeof t, "%.0f", value);
    char box[14];
    snprintf(box, sizeof box, right ? "<%5s" : "%5s>", t);
    ui_put(right ? col : col + 1, mid, box, LAP_ATTR(ink, 0));
    ui_put(right ? col + 1 : col + 2, top - 1, title, LAP_ATTR(FHUD, 0));
}

static float tape_step(float span) {        /* a round number per row, about span/12 */
    static const float steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    for (int i = 0; i < 12; i++) if (steps[i] * 12 >= span) return steps[i];
    return 10000;
}

static void hud_tapes(const game_player *p, l3_vec local_up, l3_vec f, l3_vec rt, float alt) {
    const int vx = ui.view.x, vw = ui.view.w, mid = ui.view.y + ui.view.h / 2;
    const int rows = ui.view.h - 8 < 13 ? (ui.view.h - 8) | 1 : 13;
    /* height and climb on the right */
    float vs = l3_dot(p->velocity, local_up);
    bool hard = vs < 0 && -vs * 4 > alt;          /* would hit within four seconds */
    int rc = vx + vw - (ui.wide ? 22 : 9);
    tape_v(rc, mid, rows, fmaxf(0, alt), tape_step(fmaxf(alt, 120)), true, "ALT", hard ? HUD_WARN : FHUD_HI);
    char t[16];
    snprintf(t, sizeof t, "%s %.0f", vs < 0 ? "SINK" : "CLMB", fabsf(vs));
    ui_put(rc + 1, mid + rows / 2 + 1, t, LAP_ATTR(hard ? HUD_WARN : FHUD, 0));
    /* speed on the left */
    const game_ship_stats *me = game_player_ship();
    float spd = cg_len(p->velocity);
    tape_v(vx + (ui.wide ? 14 : 1), mid, rows, spd, tape_step(fmaxf(me ? me->speed : 100, spd)), false, "SPD", FHUD_HI);
    /* heading across the top: the nose's bearing on the world below, 0 toward
       its north, growing to the right */
    l3_vec ref = fabsf(local_up.y) < .9f ? l3v(0, 1, 0) : l3v(0, 0, -1);
    l3_vec north = l3_norm(l3_sub(ref, l3_scale(local_up, l3_dot(ref, local_up))));
    l3_vec east = l3_cross(north, local_up);
    l3_vec ft = l3_sub(f, l3_scale(local_up, l3_dot(f, local_up)));
    if (cg_len(ft) < 1e-3f) ft = l3_cross(local_up, rt);         /* nose straight down: the right wing's left */
    float hdg = atan2f(l3_dot(ft, east), l3_dot(ft, north)) / CG_RAD;
    if (hdg < 0) hdg += 360;
    const int half_w = vw / 2 - 16 > 30 ? 30 : vw / 2 - 16, hc = vx + vw / 2, hr = ui.view.y + 3;
    const float per = 2;                             /* degrees a cell */
    for (int c = -half_w; c <= half_w; c++) {
        float a = hdg + c * per;
        int ia = (int)floorf(a / per + .5f) * (int)per;
        int norm = ((ia % 360) + 360) % 360;
        if (norm % 10 == 0) ui_put(hc + c, hr + 1, norm % 30 == 0 ? "|" : "'", LAP_ATTR(FHUD, 0));
        if (norm % 30 == 0 && c > -half_w + 2 && c < half_w - 2) {
            snprintf(t, sizeof t, "%03d", norm);
            ui_put(hc + c - 1, hr, t, LAP_ATTR(FHUD, 0));
        }
    }
    snprintf(t, sizeof t, "[%03.0f]", fmodf(hdg + .5f, 360));
    ui_put(hc - 2, hr + 2, t, LAP_ATTR(FHUD_HI, 0));
}

/* The NAV choice in the view: a diamond on it with its name, or an arrow to it. */
static void nav_marker(void) {
    ui_dest d;
    ui.nav_label[0] = 0;
    if (ui.hud_off & UI_HUD_NAV) return;
    if (!ui_nav_target(&d) || d.kind == UI_NAV_CONTACT) return;   /* contacts have the bracket */
    float x, y, z;
    const game_player *p = &game_state()->player;
    bool on = ui_project(d.position, &x, &y, &z);
    if (on && x >= 2 && y >= 2 && x < ui.dw - 2 && y < ui.dh - 2) {
        int ix = (int)x, iy = (int)y, r = 4;
        for (int k = 0; k <= r; k++) {
            dot(ix - r + k, iy - k, HUD_COOL); dot(ix + r - k, iy - k, HUD_COOL);
            dot(ix - r + k, iy + k, HUD_COOL); dot(ix + r - k, iy + k, HUD_COOL);
        }
        float dist = cg_len(l3_sub(d.position, p->position)) - (d.kind == UI_NAV_PLANET ? d.radius : 0);
        char t[40];
        if (dist >= 1000) snprintf(t, sizeof t, "%.12s %.1fkm", d.name, dist / 1000);
        else snprintf(t, sizeof t, "%.12s %.0fm", d.name, dist);
        ui.nav_label_col = ix / 2 - (int)strlen(t) / 2;
        ui.nav_label_row = (iy + r) / 3 + 1;
        snprintf(ui.nav_label, sizeof ui.nav_label, "%s", t);
    } else {
        ui.nav_label[0] = 0;
        float ax = on ? x - ui.dw * .5f : x, ay = on ? y - ui.dh * .5f : -y, len = sqrtf(ax * ax + ay * ay);
        if (len < 1e-3f) return;
        ax /= len; ay /= len;
        float k = fminf((ui.dw * .5f - 9) / fmaxf(fabsf(ax), 1e-3f), (ui.dh * .5f - 9) / fmaxf(fabsf(ay), 1e-3f));
        int px = (int)(ui.dw * .5f + ax * k), py = (int)(ui.dh * .5f + ay * k);
        for (int i = 0; i < 4; i++) dring(px, py, 1 + i * .5f, 1 + i * .5f, i == 3 ? HUD_COOL : LAP_BLUE, 1);
    }
}

/* The plotted jump, a mark of its own: a ring with four ticks over the next
   system, cyan while the nose comes round, green once it is aligned and J will
   take it, the inner ring beating while the drive charges. Its name, distance
   and the angle left sit under it; off the view, a hollow chevron at the edge. */
static void jump_marker(void) {
    const game_view *v = game_state();
    l3_vec dir;
    jump_words[0][0] = 0;
    if (ui.hud_off & UI_HUD_JUMP) return;
    if (v->player.docked || v->jump == JUMP_TUNNEL || !game_jump_direction(&dir)) return;
    const float off = game_jump_alignment();
    jump_ink = off <= GAME_JUMP_ALIGN_DEG ? HUD_OK : HUD_COOL;
    float x, y, z;
    bool on = ui_project(l3_add(render_eye, l3_scale(dir, 5000)), &x, &y, &z);
    if (on && x >= 12 && y >= 12 && x < ui.dw - 12 && y < ui.dh - 12) {
        int ix = (int)x, iy = (int)y;
        dring(ix, iy, 7, 7, jump_ink, 1);
        if (v->jump == JUMP_CHARGE) dring(ix, iy, 3 + ((int)(ui.t * 6) & 1), 3 + ((int)(ui.t * 6) & 1), 15, 1);
        else dot(ix, iy, jump_ink);
        for (int k = 9; k <= 11; k++) {
            dot(ix - k, iy, jump_ink); dot(ix + k, iy, jump_ink);
            dot(ix, iy - k, jump_ink); dot(ix, iy + k, jump_ink);
        }
        snprintf(jump_words[0], sizeof jump_words[0], "%.14s %.1f LY", v->systems[v->plotted].name, v->plotted_ly);
        if (v->jump == JUMP_CHARGE) snprintf(jump_words[1], sizeof jump_words[1], "CHARGING");
        else if (v->player.mass_locked) snprintf(jump_words[1], sizeof jump_words[1], "MASS LOCKED");
        else if (off <= GAME_JUMP_ALIGN_DEG) snprintf(jump_words[1], sizeof jump_words[1], "ALIGNED  J TO JUMP");
        else snprintf(jump_words[1], sizeof jump_words[1], "ALIGN %.0f", off);
        jump_col = ix / 2;
        jump_row = (iy + 12) / 3 + 1;
        return;
    }
    /* off the view or behind: point the way round */
    float ax = x - ui.dw * .5f, ay = on ? y - ui.dh * .5f : -y, len;
    if (!on) ax = x;
    len = sqrtf(ax * ax + ay * ay);
    if (len < 1e-3f) { ax = 0; ay = 1; len = 1; }
    ax /= len; ay /= len;
    float k = fminf((ui.dw * .5f - 10) / fmaxf(fabsf(ax), 1e-3f), (ui.dh * .5f - 10) / fmaxf(fabsf(ay), 1e-3f));
    int px = (int)(ui.dw * .5f + ax * k), py = (int)(ui.dh * .5f + ay * k);
    for (int i = 0; i < 6; i++) {           /* a V pointing outward */
        dot(px - (int)(ax * i - ay * i), py - (int)(ay * i + ax * i), jump_ink);
        dot(px - (int)(ax * i + ay * i), py - (int)(ay * i - ax * i), jump_ink);
    }
    dring(px - (int)(ax * 4), py - (int)(ay * 4), 1.5f, 1.5f, jump_ink, 1);
}

/* The newest message, in the view for a few seconds, the way comms float in. */
static void toast(void) {
    const game_view *v = game_state();
    uint32_t h = 2166136261u;           /* the text itself: the revision moves for any change */
    for (const char *c = v->messages[0]; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    h ^= (uint32_t)v->message_count;
    if (h != (uint32_t)ui.msg_rev) { ui.msg_rev = (int)h; ui.msg_ms = lap_millis(); }
    uint32_t age = lap_millis() - ui.msg_ms;
    if (age > 6000 || !v->messages[0][0]) return;
    char t[64];
    snprintf(t, sizeof t, "%.*s", ui.view.w - 4, v->messages[0]);
    int n = (int)strlen(t);
    ui_put(ui.view.x + (ui.view.w - n) / 2, ui.view.y + ui.view.h - 4, t,
                LAP_ATTR(age < 4500 ? HUD_HI : HUD, 0));
}

/* An interdiction: a ring on the escape vector to hold the nose on, and a tug
   meter at the top: left is losing, right is winning. */
static void interdiction(void) {
    const game_player *p = &game_state()->player;
    if (!game_state()->interdiction.active) return;
    float x, y, z;
    l3_vec at = l3_add(p->position, l3_scale(l3_norm(game_state()->interdiction.escape), 2000));
    if (ui_project(at, &x, &y, &z) && x > 2 && y > 2 && x < ui.dw - 2 && y < ui.dh - 2) {
        dring((int)x, (int)y, 6, 6, HUD_COOL, 1);
        dring((int)x, (int)y, 3, 3, ((int)(ui.t * 6)) & 1 ? 15 : HUD_COOL, 1);
    } else {        /* off the view: an arrow toward it */
        float ax = x - ui.dw * .5f, ay = y - ui.dh * .5f, len = sqrtf(ax * ax + ay * ay);
        if (z <= 0) { ax = -ax; ay = -ay; }
        if (len > 1e-3f) {
            ax /= len; ay /= len;
            int px = (int)(ui.dw * .5f + ax * (ui.dw * .5f - 8)), py = (int)(ui.dh * .5f + ay * (ui.dh * .5f - 8));
            for (int i = 0; i < 7; i++) dline(px - (int)(ax * i - ay * i * .5f), py - (int)(ay * i + ax * i * .5f),
                                              px - (int)(ax * i + ay * i * .5f), py - (int)(ay * i - ax * i * .5f), HUD_COOL);
        }
    }
    int w = ui.dw * 6 / 10, x0 = (ui.dw - w) / 2, y0 = 14, mid = x0 + w / 2;
    float tug = cg_clamp(game_state()->interdiction.tug, -1, 1);
    for (int i = 0; i <= w; i++) dot(x0 + i, y0 + 2, HUD_DIM);
    int to = mid + (int)(tug * w / 2);
    for (int i = mid < to ? mid : to; i <= (mid < to ? to : mid); i++) {
        dot(i, y0, tug < 0 ? HUD_WARN : HUD_COOL);
        dot(i, y0 + 1, tug < 0 ? HUD_WARN : HUD_COOL);
    }
    dot(mid, y0 - 1, 15); dot(mid, y0 + 3, 15);
}

/* First flight: three short tips, one at a time, each gone on a tap or after a while. */
void ui_tips_draw(float dt) {
    const game_view *v = game_state();
    if (ui.tips >= 3 || v->player.docked || ui.launch_t > 0 || v->panel != PANEL_NONE) return;
    static const char *const tip[3][3] = {
        {"DRAG THE VIEW TO STEER", "up and down pitch, sideways rolls", "let go and the ship holds"},
        {"THROTTLE: RIGHT OF THE SCANNER", "slide it up to go, down to stop", "CRUISE crosses a system fast"},
        {"NAV: PICK A PLACE, AUTOPILOT", "it flies there and docks for you", "tap the view for the next tip"},
    };
    ui.tip_t += dt;
    if (ui.tip_t > 12) { ui.tips++; ui.tip_t = 0; ui_save_settings(); return; }
    int w = ui.view.w - 6 < 38 ? ui.view.w - 6 : 38, x = ui.view.x + (ui.view.w - w) / 2;
    int y = ui.view.y + ui.view.h / 2 + 3;
    lap_fill(x, y, w, 5, ' ', LAP_ATTR(HUD, LAP_BLACK));
    lap_hline(x, y, w, LAP_BRAILLE(0x09), LAP_ATTR(HUD_COOL, LAP_BLACK));
    for (int i = 0; i < 3; i++) {
        const char *t = tip[ui.tips][i];
        lap_put_str(x + (w - (int)strlen(t)) / 2, y + 1 + i, t, LAP_ATTR(i ? HUD_SUB : HUD_COOL, LAP_BLACK));
    }
    char n[8];
    snprintf(n, sizeof n, "%d/3", ui.tips + 1);
    lap_put_str(x + w - 4, y + 4, n, LAP_ATTR(HUD_DIM == 8 ? HUD_SUB : HUD_SUB, LAP_BLACK));
}
