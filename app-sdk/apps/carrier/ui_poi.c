/* Places worth flying to, drawn: each kind has its own look in the scene and,
 * where light matters (nebulae, a second sun, a dark star's bent light), a
 * pass over the empty dots behind it. The rules live in poi.c. */
#include "internal.h"
#include "art.h"
#include "ui.h"

enum { M_STEEL, M_POLICE, M_GUN, M_FIRE, M_FLASH, M_ROCK, M_MOON, M_STAR0 };

static inline float hash01(uint32_t n) { return (cg_hash(n) & 1023) * (1 / 1023.f); }

static void label(l3_scene *sc, const game_poi *p, float lift) {
    (void)sc; (void)p; (void)lift;      /* names are drawn in cells: ui_poi_labels */
}

/* Names over places and commanders, in the normal font, after the dots. */
void ui_poi_labels(void) {
    const game_view *v = game_state();
    const game_player *pl = &v->player;
    char t[32];
    for (int i = 0; i < v->poi_count; i++) {
        const game_poi *p = &v->pois[i];
        float d = cg_len(l3_sub(p->position, pl->position));
        if (!p->discovered && d > p->scan_range * 3) continue;
        if (d > 8000) continue;
        float x, y, z;
        float lift = p->kind == POI_NEBULA ? 0 : fmaxf(p->size, 60) * .6f + 8;
        if (!ui_project(l3_add(p->position, l3v(0, lift, 0)), &x, &y, &z)) continue;
        if (d >= 1000) snprintf(t, sizeof t, "%s %.1fk", p->discovered ? p->name : "?", d / 1000);
        else snprintf(t, sizeof t, "%s %.0fm", p->discovered ? p->name : "?", d);
        int col = ui.view.x + (int)x / 2 - (int)strlen(t) / 2, row = ui.view.y + (int)y / 3 - 1;
        if (row < 4 || row >= ui.view.y + ui.view.h - 4) continue;
        if (col < 0) col = 0;
        if (col + (int)strlen(t) > ui.view.x + ui.view.w) col = ui.view.x + ui.view.w - (int)strlen(t);
        ui_put(col, row, t, LAP_ATTR(p->scanned ? HUD_COOL : HUD, 0));
    }
    for (int i = 0; i < ui_net_count(); i++) {
        const ui_peer *pe = ui_net_peer(i);
        float x, y, z;
        if (!ui_project(l3_add(pe->position, l3v(0, 12, 0)), &x, &y, &z)) continue;
        snprintf(t, sizeof t, "CMDR %s", pe->name);
        int col = ui.view.x + (int)x / 2 - (int)strlen(t) / 2, row = ui.view.y + (int)y / 3 - 1;
        if (row < 4 || row >= ui.view.y + ui.view.h - 4 || col < 0) continue;
        ui_put(col, row, t, LAP_ATTR(HUD_COOL, 0));
    }
}

static void shark_rock(l3_scene *sc, const game_poi *p, float t) {
    /* Old Toothy: a long body, a fin, a tail - from far it is just a rock */
    float s = p->size * .5f;
    l3_quat q = l3_quat_euler(t * 3, 0, 8);
    l3_xform body = l3_pose(p->position, q, 1);
    l3_ellipsoid(sc, &body, l3v(s * .22f, s * .2f, s), render_material(M_ROCK));
    l3_vec up = l3_quat_rotate(q, l3v(0, 1, 0)), back = l3_quat_rotate(q, l3v(0, 0, 1));
    l3_cone(sc, l3_add(p->position, l3_scale(up, s * .15f)), l3_add(p->position, l3_add(l3_scale(up, s * .55f),
            l3_scale(back, s * .2f))), s * .14f, s * .01f, render_material(M_ROCK));
    l3_vec tail = l3_add(p->position, l3_scale(back, s * .95f));
    l3_cone(sc, tail, l3_add(tail, l3_add(l3_scale(back, s * .3f), l3_scale(up, s * .35f))), s * .08f, s * .01f,
            render_material(M_ROCK));
    l3_cone(sc, tail, l3_add(tail, l3_add(l3_scale(back, s * .25f), l3_scale(up, -s * .25f))), s * .07f, s * .01f,
            render_material(M_ROCK));
}

void render_pois(l3_scene *sc) {
    const game_view *v = game_state();
    const game_player *pl = &v->player;
    float t = v->clock;
    for (int i = 0; i < v->poi_count && i < GAME_POIS; i++) {
        const game_poi *p = &v->pois[i];
        float d = cg_len(l3_sub(p->position, pl->position));
        if (d > 9000) { l3_point(sc, p->position, 1, p->colour ? p->colour : 7); continue; }
        l3_tag(sc, 230 + i);
        float s = fmaxf(p->size, p->kind == POI_TOOTHY ? 70 : p->kind == POI_GENERATION ? 220 : 30);
        switch (p->kind) {
        case POI_NEBULA:                     /* painted behind, in ui_poi_paint */
            break;
        case POI_DARK_STAR: {
            l3_sphere(sc, p->position, s * .35f, AM_SLOT);
            l3_xform disc = l3_pose(p->position, l3_quat_euler(t * 4, 0, 18), 1);
            l3_torus(sc, &disc, s * .75f, s * .05f, render_material(M_FIRE));
            l3_torus(sc, &disc, s * 1.0f, s * .025f, render_material(M_STAR0 + 0));
            break;
        }
        case POI_COMET: {
            l3_xform b = l3_pose(p->position, l3_quat_euler(t * 20, t * 7, 0), s * .25f);
            l3_draw_mesh(sc, game_mesh(SH_ROCK2), &b, render_material(M_MOON));
            l3_sphere(sc, p->position, s * .18f, render_material(M_STAR0 + 3));
            l3_vec tail = l3_norm(p->tail);
            for (int k = 0; k < 28; k++) {
                float a = hash01(p->seed + (uint32_t)k) * 2 * CG_PI, r = hash01(p->seed * 7 + (uint32_t)k);
                l3_vec side = l3v(cosf(a) * r * s * .5f, sinf(a) * r * s * .5f, 0);
                float len = s * (3 + 5 * hash01(p->seed * 3 + (uint32_t)k));
                l3_vec a0 = l3_add(p->position, side);
                l3_line(sc, a0, l3_add(a0, l3_add(l3_scale(tail, len), l3_scale(side, 1.5f))), k % 3 ? 14 : 15);
            }
            break;
        }
        case POI_WRECK: {                    /* a capital ship broken in two, still lit in places */
            float k = s / 5.5f;
            l3_quat q = l3_quat_euler((float)(p->seed % 360), 6, 24 + t * .5f);
            l3_xform front = l3_pose(p->position, q, k);
            l3_draw_mesh(sc, game_mesh(SH_ANACONDA), &front, render_material(M_GUN));
            l3_vec off = l3_quat_rotate(q, l3v(s * .15f, -s * .1f, s * .7f));
            l3_xform rear = l3_pose(l3_add(p->position, off), l3_quat_mul(q, l3_quat_euler(30, 20, 40)), k * .8f);
            l3_draw_mesh(sc, game_mesh(SH_BOA), &rear, render_material(M_GUN));
            for (int j = 0; j < 6; j++) {
                l3_vec at = l3_add(p->position, l3v((hash01(p->seed + (uint32_t)j) - .5f) * s, (hash01(p->seed + 9u + (uint32_t)j) - .5f) * s * .5f,
                                                    (hash01(p->seed + 17u + (uint32_t)j) - .5f) * s));
                l3_xform r = l3_pose(at, l3_quat_euler(t * 9 + j * 40, j * 30, 0), s * .03f);
                l3_draw_mesh(sc, game_mesh((game_class)(SH_ROCK1 + j % 3)), &r, render_material(M_GUN));
                if (((int)(t * 2) + j) % 4 == 0) l3_point(sc, l3_add(at, l3v(0, s * .03f, 0)), 2, 11);
            }
            break;
        }
        case POI_RELIC: {                    /* a black slab; it lights when it answers */
            l3_quat q = l3_quat_euler((float)(p->seed % 360), 0, 0);
            l3_xform slab = l3_pose(p->position, q, 1);
            l3_box(sc, &slab, l3v(s * .12f, s * .5f, s * .03f), AM_SLOT);
            float glow = p->answered ? 1 : p->hold_progress;
            /* its edges hold a faint light, brighter as it answers */
            l3_vec hx = l3_quat_rotate(q, l3v(s * .12f, 0, 0)), hy = l3_quat_rotate(q, l3v(0, s * .5f, 0)),
                   hz = l3_quat_rotate(q, l3v(0, 0, s * .031f));
            uint8_t edge = glow > .5f ? 14 : glow > 0 ? 12 : LAP_BLUE;
            for (int side = -1; side <= 1; side += 2) {
                l3_vec c0 = l3_add(p->position, l3_scale(hz, (float)side));
                l3_vec a0 = l3_sub(l3_sub(c0, hx), hy), a1 = l3_sub(l3_add(c0, hx), hy);
                l3_vec a2 = l3_add(l3_add(c0, hx), hy), a3 = l3_add(l3_sub(c0, hx), hy);
                l3_line(sc, a0, a1, edge); l3_line(sc, a1, a2, edge); l3_line(sc, a2, a3, edge); l3_line(sc, a3, a0, edge);
            }
            uint8_t ink = glow > .66f ? 15 : glow > .33f ? 14 : glow > 0 ? 12 : 8;
            for (int k = 0; k < 4; k++) {
                l3_vec corner = l3_quat_rotate(q, l3v((k & 1 ? 1 : -1) * s * .12f, (k & 2 ? 1 : -1) * s * .5f, 0));
                l3_point(sc, l3_add(p->position, corner), glow > 0 ? 2 : 1, ink);
            }
            if (glow > 0) l3_line(sc, l3_add(p->position, l3_quat_rotate(q, l3v(0, -s * .5f, -s * .04f))),
                                  l3_add(p->position, l3_quat_rotate(q, l3v(0, s * (glow - .5f), -s * .04f))), 14);
            break;
        }
        case POI_SIGNAL: {
            l3_xform b = l3_pose(p->position, l3_quat_euler(t * 30, 0, 0), 4);
            l3_draw_mesh(sc, game_mesh(SH_PROBE), &b, render_material(M_STEEL));
            if (((int)(t * 3)) & 1) l3_point(sc, p->position, 3, p->squawk == 7700 ? 9 : 11);
            break;
        }
        case POI_PULSAR: {
            l3_sphere(sc, p->position, s * .12f, render_material(M_FLASH));
            l3_vec axis = l3_quat_rotate(l3_quat_euler(t * 140, 20, 0), l3v(1, 0, 0));
            l3_line(sc, l3_sub(p->position, l3_scale(axis, s * 4)), l3_add(p->position, l3_scale(axis, s * 4)), 14);
            l3_line(sc, l3_sub(p->position, l3_scale(axis, s * 2.5f)), l3_add(p->position, l3_scale(axis, s * 2.5f)), 15);
            break;
        }
        case POI_BINARY:
            l3_sphere(sc, p->position, s * .6f, render_material(M_STAR0 + (p->colour & 3)));
            break;
        case POI_ION_STORM: {               /* lightning that never strikes the same place twice */
            uint32_t frame = (uint32_t)(t * 8);
            for (int k = 0; k < 5; k++) {
                uint32_t h = p->seed ^ (frame + (uint32_t)k * 977u);
                l3_vec a0 = l3_add(p->position, l3v((hash01(h) - .5f) * s * 2, (hash01(h + 1) - .5f) * s, (hash01(h + 2) - .5f) * s * 2));
                for (int seg = 0; seg < 4; seg++) {
                    l3_vec a1 = l3_add(a0, l3v((hash01(h + 3u + (uint32_t)seg) - .5f) * s * .4f,
                                               (hash01(h + 9u + (uint32_t)seg) - .5f) * s * .4f,
                                               (hash01(h + 15u + (uint32_t)seg) - .5f) * s * .4f));
                    l3_line(sc, a0, a1, seg & 1 ? 15 : 12);
                    a0 = a1;
                }
            }
            break;
        }
        case POI_RUINS: {
            l3_xform b = l3_pose(p->position, l3_quat_euler(t * 1.5f, 25, 40), s / 2.2f);
            l3_draw_mesh(sc, art_mesh(ART_DODECA), &b, render_material(M_GUN));
            for (int j = 0; j < 8; j++) {
                l3_vec at = l3_add(p->position, l3v((hash01(p->seed + (uint32_t)j) - .5f) * s * 2, (hash01(p->seed + 5u + (uint32_t)j) - .5f) * s,
                                                    (hash01(p->seed + 11u + (uint32_t)j) - .5f) * s * 2));
                l3_xform r = l3_pose(at, l3_quat_euler(t * 5 + j * 50, j * 20, 0), s * .04f);
                l3_draw_mesh(sc, game_mesh((game_class)(SH_ROCK1 + j % 3)), &r, render_material(M_STEEL));
            }
            break;
        }
        case POI_GENERATION: {              /* a long ark: a spine, three wheels, a few lit windows */
            l3_quat q = l3_quat_euler((float)(p->seed % 360), 10, 0);
            l3_vec ax = l3_quat_rotate(q, l3v(0, 0, 1));
            l3_vec a0 = l3_sub(p->position, l3_scale(ax, s * .6f)), a1 = l3_add(p->position, l3_scale(ax, s * .6f));
            l3_cylinder(sc, a0, a1, s * .05f, render_material(M_STEEL));
            for (int k = -1; k <= 1; k++) {
                l3_vec c = l3_add(p->position, l3_scale(ax, k * s * .35f));
                l3_xform w = l3_pose(c, l3_quat_mul(q, l3_quat_euler(0, 90, t * 6 + k * 30)), 1);
                l3_torus(sc, &w, s * .2f, s * .025f, render_material(M_STEEL));
                if (((int)(t * 1.5f) + k) & 1) l3_point(sc, l3_add(c, l3v(0, s * .2f, 0)), 2, 11);
            }
            l3_sphere(sc, a1, s * .06f, render_material(M_STAR0 + 3));
            break;
        }
        case POI_TOOTHY:
            shark_rock(sc, p, t);
            break;
        case POI_BEACON: {
            l3_xform b = l3_pose(p->position, l3_quat_euler(t * 20, 0, 0), 6);
            l3_draw_mesh(sc, game_mesh(SH_PROBE), &b, render_material(M_STEEL));
            if (((int)(t * 2)) & 1) l3_point(sc, l3_add(p->position, l3v(0, 9, 0)), 3, 11);
            break;
        }
        default:
            l3_point(sc, p->position, 2, p->colour);
            break;
        }
        float lift = p->kind == POI_NEBULA ? 0 : p->size * .7f + 10;
        if (p->kind != POI_NEBULA) label(sc, p, lift);
    }
    l3_tag(sc, 0);
}

/* The scene's extras: mesh commanders, then places. */
void render_world_extras(l3_scene *sc) {
    render_peers(sc);
    render_pois(sc);
    /* inside a cloud, the view closes in */
    const game_view *v = game_state();
    if (v->in_nebula && v->nebula_density > .05f) {
        uint8_t ink = 8;
        for (int i = 0; i < v->poi_count; i++) if (v->pois[i].kind == POI_NEBULA) ink = v->pois[i].colour & 7;
        l3_fog(sc, ink, 40, 900 - v->nebula_density * 650);
    } else
        l3_fog(sc, 0, 0, 0);
}

/* Light behind things: clouds, a companion sun's glow, the ring a dark star bends. */
void ui_poi_paint(void) {
    const game_view *v = game_state();
    const game_player *pl = &v->player;
    for (int i = 0; i < v->poi_count; i++) {
        const game_poi *p = &v->pois[i];
        float x, y, z;
        bool on = ui_project(p->position, &x, &y, &z);
        float fy = ui.fy;
        float d = cg_len(l3_sub(p->position, pl->position));
        if (p->kind == POI_NEBULA) {
            uint8_t hi = (uint8_t)((p->colour & 7) | 8), lo = (uint8_t)(p->colour & 7);
            if (d < p->radius) {             /* inside: the cloud is everywhere */
                float k = .25f + v->nebula_density * .5f;
                for (int j = 0; j < ui.dw * ui.dh; j++)
                    if (ui.dots[j] == LAP_DOT_CLEAR) {
                        float n = hash01(p->seed ^ (uint32_t)(j * 2654435761u));
                        if (n < k * .5f) ui.dots[j] = n < k * .08f ? hi : lo;
                    }
                continue;
            }
            if (!on || z < 1) continue;
            float r = p->radius / z * fy;
            if (r < 2 || r > ui.dw * 3) continue;
            int x0 = (int)(x - r), x1 = (int)(x + r), y0 = (int)(y - r), y1 = (int)(y + r);
            if (x1 < 0 || y1 < 0 || x0 >= ui.dw || y0 >= ui.dh) continue;
            for (int yy = y0 < 0 ? 0 : y0; yy <= y1 && yy < ui.dh; yy++)
                for (int xx = x0 < 0 ? 0 : x0; xx <= x1 && xx < ui.dw; xx++) {
                    uint8_t *dot = &ui.dots[yy * ui.dw + xx];
                    if (*dot != LAP_DOT_CLEAR) continue;
                    float dx = (xx - x) / r, dy = (yy - y) / r, rr = dx * dx + dy * dy;
                    if (rr > 1) continue;
                    /* a lumpy cloud: density falls to the edge, broken by fixed noise */
                    float lump = hash01(p->seed + (uint32_t)((int)(dx * 6 + 7) * 31 + (int)(dy * 6 + 7)));
                    float k = (1 - rr) * (.45f + lump * .7f);
                    float n = hash01((uint32_t)xx * 73856093u ^ (uint32_t)yy * 19349663u);
                    if (n < k * .55f) *dot = n < k * .12f ? hi : lo;
                }
        } else if (p->kind == POI_BINARY && on) {   /* the companion's bloom */
            float r = p->size * .6f / fmaxf(z, 1) * fy * 3 + 4;
            static const uint8_t star_hi[4] = {9, 11, 15, 12}, star_lo[4] = {LAP_RED, LAP_YELLOW, LAP_WHITE, LAP_BLUE};
            uint8_t s_hi = star_hi[p->colour & 3], s_lo = star_lo[p->colour & 3];
            if (r > ui.dw * 2) r = ui.dw * 2;
            int ya = (int)(y - r) < 0 ? 0 : (int)(y - r), yb = (int)(y + r) >= ui.dh ? ui.dh - 1 : (int)(y + r);
            int xa = (int)(x - r) < 0 ? 0 : (int)(x - r), xb = (int)(x + r) >= ui.dw ? ui.dw - 1 : (int)(x + r);
            for (int yy = ya; yy <= yb; yy++)
                for (int xx = xa; xx <= xb; xx++) {
                    uint8_t *dot = &ui.dots[yy * ui.dw + xx];
                    if (*dot != LAP_DOT_CLEAR) continue;
                    float dd = sqrtf((xx - x) * (xx - x) + (yy - y) * (yy - y)) / r;
                    if (dd > 1) continue;
                    float k = (1 - dd) * (1 - dd) * (1 - dd);
                    if (k > lc_bayer(xx, yy)) *dot = k > .35f ? s_hi : s_lo;
                }
        } else if (p->kind == POI_DARK_STAR && on) {  /* bent starlight hugging the core */
            float r = p->size * .35f / fmaxf(z, 1) * fy;
            for (int k = 0; k < 64; k++) {
                float a = k * 2 * CG_PI / 64;
                for (int j = 0; j < 2; j++) {
                    int xx = (int)(x + cosf(a) * (r * 1.25f + j)), yy = (int)(y + sinf(a) * (r * 1.25f + j));
                    if (xx < 0 || yy < 0 || xx >= ui.dw || yy >= ui.dh) continue;
                    if (ui.dots[yy * ui.dw + xx] == LAP_DOT_CLEAR) ui.dots[yy * ui.dw + xx] = j ? LAP_BLUE : 15;
                }
            }
        }
    }
}

/* What the HUD says about places: a scan underway, cloud, storm. */
const char *ui_poi_status(char *buf, int n) {
    const game_view *v = game_state();
    if (v->scanning_poi >= 0 && v->scanning_poi < v->poi_count && v->poi_scan_progress > 0 && v->poi_scan_progress < 1) {
        snprintf(buf, (size_t)n, "SCANNING %d%%", (int)(v->poi_scan_progress * 100));
        return buf;
    }
    for (int i = 0; i < v->poi_count; i++)
        if (v->pois[i].kind == POI_RELIC && v->pois[i].hold_progress > 0 && !v->pois[i].answered) {
            snprintf(buf, (size_t)n, "THE STONE LISTENS %d%%", (int)(v->pois[i].hold_progress * 100));
            return buf;
        }
    if (v->scanner_scrambled) return "SCANNER SCRAMBLED";
    if (v->in_nebula) return "IN THE CLOUD";
    return NULL;
}
