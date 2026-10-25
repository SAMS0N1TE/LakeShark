/* The fight, on the glass: a halo on every ship and object in view, the target's
 * hologram in the corner, and what damage does to the cockpit - shake, cracks in
 * the canopy, a red alarm, sparks, a HUD that starts to glitch. */
#include "internal.h"
#include "art.h"
#include "ui.h"

static inline void dot(int x, int y, uint8_t ink) {
    if (x >= 0 && y >= 0 && x < ui.dw && y < ui.dh) ui.dots[y * ui.dw + x] = ink;
}

static uint8_t status_ink(const game_contact *c) {
    if (c->hostile) return HUD_WARN;
    if (c->role == ROLE_POLICE) return HUD_LAW;
    if (c->role == ROLE_BEACON) return HUD_OK;
    if (c->live) return HUD_COOL;
    return HUD_HI;
}

/* ---- halos ----------------------------------------------------------------- */

void ui_halos(void) {
    const game_view *v = game_state();
    int blink = ((int)(ui.t * 6)) & 1;
    for (int i = 0; i < GAME_CONTACTS; i++) {
        const game_contact *c = &v->contacts[i];
        if (!c->active || i == v->selected) continue;
        float x, y, z;
        if (!ui_project(c->position, &x, &y, &z) || x < 2 || y < 2 || x >= ui.dw - 2 || y >= ui.dh - 2) continue;
        const game_ship_stats *st = game_ship(c->ship_class);
        /* Only the target gets brackets (bracket()); everyone else a small
           triangle over them, the same size at any range, so a ship too far
           to see is still a mark you can find: filled for hostile, hollow
           for the rest, white while it is shooting at you. */
        int r = (int)fminf(10, (st ? st->radius : 10) * .6f / fmaxf(z, 1) * ui.fy);
        uint8_t ink = status_ink(c);
        if (c->scanned && c->wanted) ink = HUD_WARN;
        if (c->attacking_player && blink) ink = 15;
        int ix = (int)x, ty = (int)y - r - 4;
        for (int k = -2; k <= 2; k++) dot(ix + k, ty, ink);
        if (c->hostile) { dot(ix - 1, ty + 1, ink); dot(ix, ty + 1, ink); dot(ix + 1, ty + 1, ink); }
        else { dot(ix - 1, ty + 1, ink); dot(ix + 1, ty + 1, ink); }
        dot(ix, ty + 2, ink);
        if (c->live) dot(ix, ty - 2, HUD_COOL);          /* a real aircraft: a dot over the mark */
    }
    /* canisters: a small square each */
    for (int i = 0; i < CG_CANS; i++) {
        if (cg->cans[i].life <= 0) continue;
        float x, y, z;
        if (!ui_project(cg->cans[i].p, &x, &y, &z) || z > 900) continue;
        int ix = (int)x, iy = (int)y;
        for (int k = -1; k <= 1; k++) { dot(ix + k, iy - 2, HUD); dot(ix + k, iy + 2, HUD); dot(ix - 2, iy + k, HUD); dot(ix + 2, iy + k, HUD); }
    }
}

/* ---- smart zoom ------------------------------------------------------------------- */

/* The field of view narrows on a far target held near the nose, so it is
   big enough to see and hit, and opens again when it leaves the nose or
   comes close. Off in STATUS. */
void ui_zoom_update(float dt) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    float want = 1;
    if (ui.autozoom && v->selected >= 0 && v->contacts[v->selected].active && !p->docked && !p->cruising &&
        v->panel == PANEL_NONE && v->jump != JUMP_TUNNEL) {
        const game_contact *c = &v->contacts[v->selected];
        const game_ship_stats *st = game_ship(c->ship_class);
        l3_vec to = l3_sub(c->position, p->position);
        float range = cg_len(to);
        l3_vec nose = l3_quat_rotate(p->attitude, l3v(0, 0, -1));
        float off = range > 1 ? acosf(cg_clamp(l3_dot(nose, l3_scale(to, 1 / range)), -1, 1)) : CG_PI;
        if (off < 7 * CG_RAD && range > 250) {
            float fy0 = .5f * ui.dh / tanf(UI_FOV * CG_RAD * .5f);
            float px = (st ? st->radius : 10) / range * fy0;     /* its size on screen at no zoom */
            want = cg_clamp(5 / fmaxf(px, .1f), 1, 3);
        }
    }
    float rate = want > ui.zoom ? 1.5f : 4;
    ui.zoom += (want - ui.zoom) * fminf(1, dt * rate);
    if (ui.zoom < 1.01f) ui.zoom = 1;
}

/* ---- the target hologram ------------------------------------------------------ */

static l3_scene *holo;
static int holo_mat;

void ui_holo_free(void) {
    l3_destroy(holo);
    holo = NULL;
}

/* Where the hologram goes, and whether there is one to draw. */
static bool holo_place(const game_contact **cp, const game_ship_stats **sp, const l3_mesh **mp, int *cx, int *cy) {
    const game_view *v = game_state();
    if (v->selected < 0 || !v->contacts[v->selected].active || v->panel != PANEL_NONE || v->player.docked) return false;
    if (ui.view.h < 30) return false;
    *cp = &v->contacts[v->selected];
    *sp = game_ship((*cp)->ship_class);
    *mp = game_mesh((*cp)->ship_class);
    *cx = ui.view.x + 1;
    *cy = ui.view.y + ui.view.h - 8 - 7;
    return *sp && *mp;
}

/* The hologram's dots go into the view before it is drawn, so the space
   behind shows through everywhere the ship and its ring are not. */
void ui_holo_paint(void) {
    const game_view *v = game_state();
    const game_contact *c;
    const game_ship_stats *st;
    const l3_mesh *m;
    int cx, cy, w = 16, h = 8;
    if (!holo_place(&c, &st, &m, &cx, &cy)) return;
    if (!holo) {
        holo = l3_create(w, h);
        if (!holo) return;
        l3_reserve(holo, 8, 128, 128);
        static const uint8_t cyan[] = {LAP_BLACK, LAP_CYAN, 14, 15};
        holo_mat = l3_material(holo, cyan, 4, .5f, 0);
        l3_sky(holo, LAP_DOT_CLEAR, LAP_DOT_CLEAR);
        l3_outline(holo, LAP_BLUE);
        l3_sun(holo, l3v(-.4f, -.7f, -.6f), 1, .25f);
    }
    float r = 1;
    for (int k = 0; k < m->nv; k++) r = fmaxf(r, cg_len(m->v[k]));
    /* as you see it: its attitude relative to your own */
    l3_quat pa = v->player.attitude;
    l3_quat inv = {-pa.x, -pa.y, -pa.z, pa.w};
    l3_quat rel = l3_quat_mul(inv, c->attitude);
    l3_camera(holo, l3v(0, r * .5f, r * 3.2f), l3v(0, 0, 0), 40);
    l3_camera_up(holo, l3v(0, 1, 0));
    l3_begin(holo);
    l3_xform t = l3_pose(l3v(0, 0, 0), rel, 1);
    l3_draw_mesh(holo, m, &t, holo_mat);
    l3_end(holo);
    /* the shield ring around it: as much of the ring as the shield has left */
    uint8_t *d = l3_dots(holo);
    int dw = w * 2, dh = h * 3;
    float sh = st->shield > 0 ? cg_clamp(c->shield / st->shield, 0, 1) : 0;
    int n = 72;
    for (int k = 0; k < n; k++) {
        float a = k * 2 * CG_PI / n - CG_PI / 2;
        int x = dw / 2 + UI_ROUND(cosf(a) * (dw / 2 - 1)), y = dh / 2 + UI_ROUND(sinf(a) * (dh / 2 - 1));
        if (x < 0 || y < 0 || x >= dw || y >= dh) continue;
        bool lit = k < (int)(sh * n);
        if (lit) d[y * dw + x] = HUD_COOL;
        else if (k % 3 == 0) d[y * dw + x] = HUD_DIM;
    }
    int ox = (cx - ui.view.x) * 2, oy = (cy - ui.view.y) * 3;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            if (d[y * dw + x] != LAP_DOT_CLEAR && ox + x < ui.dw && oy + y < ui.dh)
                ui.dots[(oy + y) * ui.dw + ox + x] = d[y * dw + x];
}

void ui_holo_draw(void) {
    const game_contact *c;
    const game_ship_stats *st;
    const l3_mesh *m;
    int cx, cy, w = 16, h = 8;
    if (!holo_place(&c, &st, &m, &cx, &cy) || !holo) return;
    float sh = st->shield > 0 ? cg_clamp(c->shield / st->shield, 0, 1) : 0;
    ui_target_parts(cx + w + 1, cy);
    /* words under it: name, hull */
    char line[40];
    float hu = st->hull > 0 ? c->hull / st->hull : 0;
    snprintf(line, sizeof line, "%-10.10s", st->name);
    ui_put(cx, cy + h, line, LAP_ATTR(status_ink(c), 0));
    ui_put(cx, cy + h + 1, "HULL", LAP_ATTR(HUD_SUB, 0));
    for (int i = 0; i < 10; i++) {
        bool on = i < (int)(hu * 10 + .5f);
        ui_cell(cx + 5 + i, cy + h + 1, on ? LAP_BRAILLE(0x1B) : LAP_BRAILLE(0x12),
                     LAP_ATTR(on ? (hu < .3f ? HUD_WARN : HUD) : HUD_DIM, 0));
    }
    if (sh <= 0 && st->shield > 0) ui_put(cx, cy - 1, "SHIELDS DOWN", LAP_ATTR(HUD_WARN, 0));
}

/* ---- the cockpit taking damage --------------------------------------------- */

static float hull_frac(void) {
    const game_player *p = &game_state()->player;
    const game_ship_stats *me = game_player_ship();
    return me && me->hull > 0 ? cg_clamp(p->hull / me->hull, 0, 1) : 1;
}

/* Cracks that grow as the hull goes: seeded lines from the canopy's edge. */
static void cracks(void) {
    float h = fminf(hull_frac(), cg_clamp(game_state()->player.canopy, 0, 1));
    int n = h < .2f ? 4 : h < .4f ? 3 : h < .6f ? 2 : h < .8f ? 1 : 0;
    for (int c = 0; c < n; c++) {
        uint32_t s = cg_hash(0xC7AC + (uint32_t)c * 977u);
        float x = (s & 1) ? ui.dw - 2 - (s >> 4) % (ui.dw / 3) : 1 + (s >> 4) % (ui.dw / 3);
        float y = (float)((s >> 12) % (ui.dh / 2));
        float a = (float)((s >> 20) % 628) * .01f;
        for (int seg = 0; seg < 14; seg++) {
            uint32_t q = cg_hash(s + (uint32_t)seg);
            a += ((q & 255) / 255.f - .5f) * 1.1f;
            float len = 2 + (q >> 8) % 4;
            for (int k = 0; k < (int)len; k++) {
                x += cosf(a);
                y += sinf(a);
                dot((int)x, (int)y, k == 0 ? 15 : LAP_WHITE);
            }
            if ((q >> 16) % 4 == 0) {      /* a branch */
                float bx = x, by = y, ba = a + 1.2f;
                for (int k = 0; k < 5; k++) { bx += cosf(ba); by += sinf(ba); dot((int)bx, (int)by, 8); }
            }
        }
    }
}

static struct { float x, y, vx, vy, life; } spark[16];

void ui_damage_dots(float dt) {
    const game_player *p = &game_state()->player;
    if (p->docked) return;
    float h = hull_frac();
    cracks();
    /* low hull: a red alarm breathing around the canopy */
    if (h < .35f && !p->dead) {
        float pulse = .5f + .5f * sinf(ui.t * 6);
        if (pulse > .55f)
            for (int x = 0; x < ui.dw; x += 2) { dot(x, 0, HUD_WARN); dot(x, ui.dh - 3, HUD_WARN); }
        if (pulse > .55f)
            for (int y = 0; y < ui.dh; y += 2) { dot(0, y, HUD_WARN); dot(ui.dw - 1, y, HUD_WARN); }
    }
    /* sparks spitting from the bottom corners */
    if (h < .4f && !p->dead && (lap_random() % 100) < (int)((.4f - h) * 140)) {
        for (int i = 0; i < 16; i++)
            if (spark[i].life <= 0) {
                bool left = lap_random() & 1;
                spark[i].x = left ? 3 : ui.dw - 4;
                spark[i].y = ui.dh - 4 - (float)(lap_random() % 10);
                spark[i].vx = (left ? 1 : -1) * (20 + (float)(lap_random() % 40));
                spark[i].vy = -(30 + (float)(lap_random() % 50));
                spark[i].life = .5f;
                break;
            }
    }
    for (int i = 0; i < 16; i++) {
        if (spark[i].life <= 0) continue;
        spark[i].life -= dt;
        spark[i].vy += 140 * dt;
        spark[i].x += spark[i].vx * dt;
        spark[i].y += spark[i].vy * dt;
        dot((int)spark[i].x, (int)spark[i].y, spark[i].life > .3f ? 15 : 11);
    }
}

/* Low hull: a few cells of the dash scramble each frame, as if the wiring were going. */
void ui_glitch_cells(void) {
    const game_player *p = &game_state()->player;
    float h = hull_frac();
    if (h > .25f || p->docked || p->dead) return;
    int n = (int)((.25f - h) * 60) + 1;
    for (int k = 0; k < n; k++) {
        uint32_t r = lap_random();
        int x = ui.dash.x + (int)(r % (uint32_t)ui.dash.w), y = ui.dash.y + (int)((r >> 8) % (uint32_t)ui.dash.h);
        ui_cell(x, y, LAP_SEXT((int)(r >> 16) & 63), LAP_ATTR((r >> 24) & 1 ? HUD_WARN : HUD, 0));
    }
}

/* A hit rocks the camera: render.c reads the offset. */
void ui_shake_update(float dt) {
    const game_player *p = &game_state()->player;
    static float hull_was = -1;
    if (hull_was >= 0 && p->hull < hull_was - .5f) ui.shake = fminf(1.5f, ui.shake + (hull_was - p->hull) * .08f + .4f);
    hull_was = p->hull;
    ui.shake = fmaxf(0, ui.shake - dt * 2.5f);
    float a = ui.shake;
    render_shake = a > 0 ? l3v(((lap_random() & 1023) / 511.5f - 1) * a, ((lap_random() & 1023) / 511.5f - 1) * a, 0)
                         : l3v(0, 0, 0);
}
