/* The 3D scene: black space, grey metal hulls, colour only where it means
 * something - engines, lasers, stars, planets, fire. No HUD here (ui_view.c). */
#include "internal.h"
#include "art.h"
#include "ui.h"

/* Materials past the shared six, made once per scene in this order. */
static int m_steel, m_gun, m_police, m_alien, m_rock, m_star[4], m_fire, m_flash, m_ring, m_moon;
static int m_wall, m_deck, m_hazard, m_window, m_red;     /* the station inside */
static int m_plate, m_panel, m_ceil, m_trim, m_strip, m_neon, m_neon2, m_pane, m_glow;   /* on foot */
static int m_band[4];           /* the walls' lower band, a colour a room: teal, blue, amber, red */
static void unwalk(l3_scene *sc);
enum { P_EARTH, P_DESERT, P_ICE, P_LAVA, P_BARREN, P_GAS, P_TYPES };
static l3_vec star_world[3][64];
static uint32_t prepared_seed;
static ui_blast blasts[UI_BLASTS];
void (*render_new_system)(uint32_t seed);
l3_vec render_shake;   /* degrees of yaw/pitch the screen side jolts the camera by */   /* the screen side's hook: bake sky and worlds */

void render_blast(l3_vec p, float size) {
    for (int i = 0; i < UI_BLASTS; i++)
        if (blasts[i].life <= 0) { blasts[i] = (ui_blast){p, 1, size}; return; }
}
void render_tick(float dt) {
    for (int i = 0; i < UI_BLASTS; i++)
        if (blasts[i].life > 0) blasts[i].life -= dt * .9f;
}

static int planet_type(const game_planet *p) {
    if (p->radius > 520) return P_GAS;
    return (int)(p->seed % 5);
}

static l3_scene *space_done, *walk_done;    /* the scenes whose materials are in */
static void prepare(l3_scene *sc) {
    cg->scene = sc;
    cg->render_ready = true;
    l3_reserve(sc, 400, 3072, 3072);
    l3_clip(sc, .5f, 26000);
    l3_shadows(sc, false);
    l3_fog(sc, 0, 0, 0);
    l3_outline(sc, LAP_BLACK);
    l3_sky(sc, LAP_DOT_CLEAR, LAP_DOT_CLEAR);   /* ui_view paints space behind */
    if (sc != space_done) {     /* a scene keeps its materials: add them once (32 at most) */
        static const uint8_t engine[] = {12}, glass[] = {LAP_BLACK, LAP_BLUE, 12}, dark[] = {LAP_BLACK, 8},
                             slot[] = {LAP_BLACK}, lamp[] = {11};
        l3_material(sc, engine, 1, 0, L3_UNLIT | L3_GLOW);
        l3_material(sc, glass, 3, .9f, 0);
        l3_material(sc, dark, 2, 0, 0);
        l3_material(sc, L3_RAMP_GREY, 4, .5f, 0);
        l3_material(sc, slot, 1, 0, L3_UNLIT);
        l3_material(sc, lamp, 1, 0, L3_UNLIT | L3_GLOW);
        static const uint8_t steel[] = {LAP_BLACK, 8, LAP_WHITE, 15}, gun[] = {LAP_BLACK, 8, LAP_WHITE, 9},
                             police[] = {LAP_BLACK, 8, LAP_BLUE, 12}, alien[] = {LAP_BLACK, LAP_GREEN, 10},
                             rock[] = {LAP_BLACK, 8, LAP_YELLOW, LAP_WHITE};
        m_steel = l3_material(sc, steel, 4, .3f, 0);
        m_gun = l3_material(sc, gun, 4, .45f, 0);   /* pirates: steel with red glints */
        m_police = l3_material(sc, police, 4, .4f, 0);
        m_alien = l3_material(sc, alien, 3, .2f, 0);
        m_rock = l3_material(sc, rock, 4, 0, L3_GRAIN);
        static const uint8_t star_ink[4][1] = {{9}, {11}, {15}, {12}};
        for (int i = 0; i < 4; i++)
            m_star[i] = l3_material(sc, star_ink[i], 1, 0, L3_UNLIT | L3_GLOW);
        static const uint8_t fire[] = {11}, flash[] = {15}, ring[] = {LAP_BLACK, 8, LAP_YELLOW, LAP_WHITE};
        m_fire = l3_material(sc, fire, 1, 0, L3_UNLIT | L3_GLOW);
        m_flash = l3_material(sc, flash, 1, 0, L3_UNLIT);
        /* the station inside: panelled walls, a darker deck, hazard yellow, lit windows */
        static const uint8_t wall[] = {8}, deck[] = {LAP_BLACK, 8}, hazard[] = {LAP_BLACK, LAP_YELLOW, 11},
                             window[] = {12}, red[] = {9};
        m_wall = l3_material(sc, wall, 1, 0, 0);     /* one flat grey: the ribs and lights draw the shape */
        m_deck = l3_material(sc, deck, 2, .35f, 0);
        m_hazard = l3_material(sc, hazard, 3, .2f, 0);
        m_window = l3_material(sc, window, 1, 0, L3_UNLIT | L3_GLOW);
        m_red = l3_material(sc, red, 1, 0, L3_UNLIT | L3_GLOW);
        m_ring = l3_material(sc, ring, 4, .1f, 0);
        static const uint8_t moon[] = {LAP_BLACK, 8, LAP_WHITE};
        m_moon = l3_material(sc, moon, 3, 0, 0);
        space_done = sc;
    }
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 64; i++) {
            uint32_t r = cg_hash(0x51A7 + (uint32_t)(k * 64 + i));
            float y = cg_unit(&r) * 2 - 1, a = cg_unit(&r) * 2 * CG_PI, radius = 12000 + k * 2800;
            float h = sqrtf(fmaxf(0, 1 - y * y));
            cg->stars[k][i] = l3v(cosf(a) * h * radius, y * radius, sinf(a) * h * radius);
        }
    prepared_seed = 0;
}

/* The walk view's own scene and its own inks: a scene holds 32 materials,
   and space's set plus these went past it (the last ones fell back to grey).
   The six shared ones keep their places for art.h. */
static int w_steel, w_hazard;
void render_walk_forget(void) { walk_done = NULL; }   /* its scene is freed: a new one may land at the same address */
static void prepare_walk(l3_scene *sc) {
    cg->scene = sc;
    cg->render_ready = true;
    prepared_seed = 0;
    l3_reserve(sc, 400, 3072, 3072);
    l3_sky(sc, LAP_DOT_CLEAR, LAP_DOT_CLEAR);
    if (sc == walk_done) return;
    walk_done = sc;
    static const uint8_t engine[] = {12}, glass[] = {LAP_BLACK, LAP_BLUE, 12}, dark[] = {LAP_BLACK, 8},
                         slot[] = {LAP_BLACK}, lamp[] = {11};
    l3_material(sc, engine, 1, 0, L3_UNLIT | L3_GLOW);
    l3_material(sc, glass, 3, .9f, 0);
    l3_material(sc, dark, 2, 0, 0);
    l3_material(sc, L3_RAMP_GREY, 4, .5f, 0);
    l3_material(sc, slot, 1, 0, L3_UNLIT);
    l3_material(sc, lamp, 1, 0, L3_UNLIT | L3_GLOW);
    /* its own steel and hazard: space's names keep space's numbers */
    static const uint8_t steel[] = {LAP_BLACK, 8, LAP_WHITE, 15}, hazard[] = {LAP_BLACK, LAP_YELLOW, 11};
    w_steel = l3_material(sc, steel, 4, .3f, 0);
    w_hazard = l3_material(sc, hazard, 3, .2f, 0);
    /* clean panels, a navy deck, a coloured band a room, warm light strips,
       neon, windows on space. Flat inks, no dither: each face one clean tone
       (dithered and doubled to half resolution, a grey ramp was all speckle) */
    static const uint8_t band[4][2] = {{LAP_CYAN, 14}, {LAP_BLUE, 12}, {LAP_YELLOW, 11}, {LAP_RED, 9}};
    for (int k = 0; k < 4; k++) m_band[k] = l3_material(sc, band[k], 2, 0, L3_FLAT);
    static const uint8_t panel[] = {LAP_WHITE, 15}, ceil_in[] = {8, LAP_WHITE}, deck_in[] = {8, LAP_BLUE},
                         trim[] = {8, LAP_WHITE}, strip[] = {11}, neon_c[] = {14}, neon_m[] = {13},
                         pane[] = {LAP_BLACK, LAP_BLACK, LAP_BLACK, LAP_BLUE}, glow[] = {15};
    m_panel = l3_material(sc, panel, 2, 0, L3_FLAT);
    m_ceil = l3_material(sc, ceil_in, 2, 0, L3_FLAT);
    m_plate = l3_material(sc, deck_in, 2, 0, L3_FLAT);
    m_trim = l3_material(sc, trim, 2, 0, L3_FLAT);
    m_glow = l3_material(sc, glow, 1, 0, L3_UNLIT | L3_GLOW);
    m_strip = l3_material(sc, strip, 1, 0, L3_UNLIT | L3_GLOW);
    m_neon = l3_material(sc, neon_c, 1, 0, L3_UNLIT | L3_GLOW);
    m_neon2 = l3_material(sc, neon_m, 1, 0, L3_UNLIT | L3_GLOW);
    m_pane = l3_material(sc, pane, 4, .15f, 0);  /* black glass, blue only in its glint: lit or glossy it went sky blue or white */
}

void cg_render_reset(void) {
    if (cg)
        cg->render_seed = 0;
}

/* A new system: the screen side bakes its sky and surfaces. */
static void system_materials(l3_scene *sc) {
    uint32_t seed = cg->v.systems[cg->v.system].seed;
    (void)sc;
    if (cg->render_seed == seed && prepared_seed == seed)
        return;
    cg->render_seed = prepared_seed = seed;
    if (render_new_system) render_new_system(seed);
}

int render_planet_kind(int i) {
    return i >= 0 && i < cg->v.contents.planet_count ? planet_type(&cg->v.contents.planets[i]) : -1;
}

static void station(l3_scene *sc) {
    const game_view *v = &cg->v;
    l3_quat q = l3_quat_euler(0, 0, v->station_angle);
    l3_xform t = l3_pose(v->contents.station, q, 40);
    l3_tag(sc, 240);
    l3_draw_mesh(sc, v->contents.station_kind ? art_mesh(ART_DODECA) : game_mesh(SH_STATION), &t, m_steel);
    /* the slot's guide lights: green while you may enter, chasing toward the opening */
    float k = fmodf(v->clock * 2, 4);
    for (int j = 0; j < 4; j++) {
        if ((int)k != j) continue;
        float z = -40.5f - (3 - j) * 6;
        for (int sd = -1; sd <= 1; sd += 2)
            l3_point(sc, l3_add(v->contents.station, l3_quat_rotate(q, l3v(sd * 22.f, 0, z))), 2, 10);
    }
    l3_tag(sc, 0);
}

static void stars(l3_scene *sc) {
    const game_player *p = &cg->v.player;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 64; i++)
            star_world[k][i] = l3_add(p->position, cg->stars[k][i]);
        for (int i = 0; i < 64; i += 1) {
            uint8_t ink = k == 0 ? 15 : k == 1 ? LAP_WHITE : 8;
            if (i % 11 == 0) ink = k ? 12 : 11;      /* a few coloured stars */
            l3_point(sc, star_world[k][i], k == 0 && i % 5 == 0 ? 2 : 1, ink);
        }
    }
}

static void dust(l3_scene *sc) {
    const game_player *p = &cg->v.player;
    if (p->speed < .3f && cg->v.jump == JUMP_IDLE)
        return;
    l3_vec f = cg_forward();
    /* Motes fixed in space, in a box that travels with the ship (each wraps to
       the far side as it is passed), so they stream by along the real line of
       flight and swing across the view in a turn: the eye reads both. Each is a
       short tick behind its point, longer with speed but never more than a few
       dots, and fainter with distance. */
    const float half = 70;
    float len = fminf(3.2f, p->speed * .025f) + (cg->v.jump == JUMP_CHARGE ? cg->v.jump_progress * 6 : 0);
    l3_vec drift = p->speed > 1 ? l3_scale(p->velocity, 1 / p->speed) : f;
    for (int i = 0; i < 56; i++) {
        uint32_t r = cg_hash((uint32_t)i * 977u + 13u);
        l3_vec seed = l3v(cg_unit(&r) * 2 * half, cg_unit(&r) * 2 * half, cg_unit(&r) * 2 * half);
        l3_vec rel = l3_sub(seed, p->position);
        rel.x = fmodf(rel.x, 2 * half); if (rel.x < -half) rel.x += 2 * half; else if (rel.x > half) rel.x -= 2 * half;
        rel.y = fmodf(rel.y, 2 * half); if (rel.y < -half) rel.y += 2 * half; else if (rel.y > half) rel.y -= 2 * half;
        rel.z = fmodf(rel.z, 2 * half); if (rel.z < -half) rel.z += 2 * half; else if (rel.z > half) rel.z -= 2 * half;
        float ahead = l3_dot(rel, f), far = cg_len(rel);
        if (ahead < 2 || far < 4) continue;            /* behind, or in the cockpit's face */
        l3_vec a = l3_add(p->position, rel);
        uint8_t ink = far < 25 ? LAP_WHITE : far < 50 ? 7 : 8;
        if (len < .4f) l3_point(sc, a, 1, ink);
        else l3_line(sc, a, l3_sub(a, l3_scale(drift, len)), ink);
    }
}

static void worlds(l3_scene *sc) {
    game_view *v = &cg->v;
    game_contents *o = &v->contents;
    l3_tag(sc, 241);
    l3_sphere(sc, o->star, o->star_radius, m_star[o->star_type & 3]);
    for (int i = 0; i < o->planet_count; i++) {
        game_planet *p = &o->planets[i];
        l3_tag(sc, 242 + i);
        float tilt = (float)(p->seed % 40) - 20;
        l3_sphere(sc, p->position, p->radius, AM_SLOT);   /* ui_planets.c paints the surface */
        l3_tag(sc, 0);                                     /* rings and moons are not repainted */
        if (p->rings) {
            l3_xform r = l3_pose(p->position, l3_quat_euler(0, 0, tilt), 1);
            l3_torus(sc, &r, p->radius * 1.5f, p->radius * .05f, m_ring);
            l3_torus(sc, &r, p->radius * 1.7f, p->radius * .03f, m_rock);
            l3_torus(sc, &r, p->radius * 1.85f, p->radius * .015f, m_ring);
        }
        if (p->moon) {
            float a = v->clock * .05f + (float)(p->seed % 100);
            l3_sphere(sc, l3_add(p->position, l3v(cosf(a) * p->radius * 2.4f, .25f * p->radius,
                                                  sinf(a) * p->radius * 2.4f)),
                      p->radius * .18f, m_moon);
        }
    }
    l3_tag(sc, 0);
    {   /* the belt: the game's own rocks, so what you shoot is what you see */
        l3_vec pp = v->player.position;
        for (int i = 0; i < 18; i++) {
            const game_asteroid *a = &v->asteroids[i];
            if (!a->active) continue;
            float d = cg_len(l3_sub(a->position, pp));
            if (d > 2600) continue;
            if (d > 1000) { l3_point(sc, a->position, 1, 8); continue; }
            l3_tag(sc, 220 + i);
            l3_xform t = l3_pose(a->position, l3_quat_euler(i * 23 + v->clock * 6, i * 17, i * 9 + v->clock * 3), a->radius);
            l3_draw_mesh(sc, game_mesh((game_class)(SH_ROCK1 + i % 3)), &t, m_rock);
        }
        l3_tag(sc, 0);
    }
    station(sc);
}

/* Half the widest span of a hull, in its own units: where the wingtip lights sit. */
static float wingspan(int cls) {
    static float span[SH_COUNT];
    if (cls < 0 || cls >= SH_COUNT) return 1;
    if (span[cls] == 0) {
        const l3_mesh *m = game_mesh((game_class)cls);
        span[cls] = .5f;
        for (int k = 0; m && k < m->nv; k++) span[cls] = fmaxf(span[cls], fabsf(m->v[k].x));
    }
    return span[cls];
}

static int hull_mat(const game_contact *c) {
    if (c->ship_class == SH_DRONE) return m_alien;
    if (c->role == ROLE_POLICE) return m_police;
    if (c->role == ROLE_PIRATE) return m_gun;
    return m_steel;
}

static void ships(l3_scene *sc) {
    game_view *v = &cg->v;
    game_player *p = &v->player;
    for (int i = 0; i < GAME_CONTACTS; i++) {
        game_contact *c = &v->contacts[i];
        if (!c->active)
            continue;
        float distance = cg_len(l3_sub(c->position, p->position));
        l3_tag(sc, i + 1);
        const game_ship_stats *st = game_ship(c->ship_class);
        float r = st ? st->radius : 10;
        if (distance > 900 * fmaxf(1, ui.zoom)) {   /* far: a point, amber when targeted; the smart zoom draws it whole further out */
            l3_point(sc, c->position, i == v->selected ? 2 : 1, i == v->selected ? HUD_HI : LAP_WHITE);
            continue;
        }
        if (c->jump_out > 0) {          /* leaving: a white-blue flash swallows it */
            float k = c->jump_out;
            l3_sphere(sc, c->position, r * (.6f + (1.2f - k) * 1.5f), k > .5f ? m_flash : m_star[3]);
            l3_vec fwd = l3_quat_rotate(c->attitude, l3v(0, 0, -1));
            l3_line(sc, c->position, l3_add(c->position, l3_scale(fwd, r * 8 * (1.2f - k))), 12);
            if (k < .4f) continue;
        }
        l3_xform t = l3_pose(c->position, c->attitude, r * .5f);
        l3_draw_mesh(sc, game_mesh(c->ship_class), &t, hull_mat(c));
        /* hurt ships trail smoke and spit sparks */
        if (st && st->hull > 0 && c->hull < st->hull * .45f && distance < 600) {
            l3_vec back = l3_quat_rotate(c->attitude, l3v(0, 0, 1));
            for (int k = 0; k < 7; k++) {
                uint32_t h = cg_hash((uint32_t)i * 131u + (uint32_t)k + (uint32_t)(v->clock * 10));
                l3_vec at = l3_add(c->position, l3_scale(back, r * (.4f + k * .45f)));
                at = l3_add(at, l3v(((h & 255) / 255.f - .5f) * r * .4f, ((h >> 8 & 255) / 255.f - .5f) * r * .4f, 0));
                l3_point(sc, at, k < 2 ? 2 : 1, k == 0 && (h >> 16) % 3 == 0 ? 11 : k < 3 ? LAP_WHITE : 8);
            }
        }
        /* nav lights on the wingtips, red to port, green to starboard, blinking out of step */
        if (distance < 420) {
            float span = wingspan(c->ship_class) * r * .5f;
            bool on = fmodf(v->clock + i * .37f, 1.2f) < .25f;
            if (on) {
                l3_point(sc, l3_add(c->position, l3_quat_rotate(c->attitude, l3v(-span, 0, 0))), 2, 9);
                l3_point(sc, l3_add(c->position, l3_quat_rotate(c->attitude, l3v(span, 0, 0))), 2, 10);
            }
        }
        /* exhaust: a short streak behind, longer the faster it flies */
        if (c->speed > 1 && distance < 600) {
            l3_vec back = l3_quat_rotate(c->attitude, l3v(0, 0, 1));
            l3_vec e = l3_add(c->position, l3_scale(back, r * .55f));
            l3_line(sc, e, l3_add(e, l3_scale(back, r * .3f + c->speed * .15f)), 12);
        }
    }
    l3_tag(sc, 0);
}

static void effects(l3_scene *sc) {
    game_player *p = &cg->v.player;
    for (int i = 0; i < CG_CANS; i++)
        if (cg->cans[i].life > 0) {
            l3_xform t = l3_pose(cg->cans[i].p, l3_quat_euler(cg->v.clock * 40 + i * 50, 35, cg->v.clock * 25), 2.2f);
            l3_draw_mesh(sc, art_mesh(ART_CANISTER), &t, m_steel);
        }
    for (int i = 0; i < CG_BEAMS; i++)
        if (cg->beams[i].life > 0 && !cg->beams[i].player) {
            cg_beam *b = &cg->beams[i];
            l3_line(sc, b->a, b->b, 9);
            l3_line(sc, l3_add(b->a, l3v(.2f, .1f, 0)), l3_add(b->b, l3v(.2f, .1f, 0)), 15);
        }
    for (int i = 0; i < CG_MISSILES; i++)
        if (cg->missiles[i].life > 0) {
            cg_missile *m = &cg->missiles[i];
            l3_vec d = l3_norm(m->v);
            l3_quat q = l3_quat_axis(l3_norm(l3_cross(l3v(0, 0, -1), d)),
                                     acosf(cg_clamp(-d.z, -1, 1)) / CG_RAD);
            l3_xform t = l3_pose(m->p, q, 2.5f);
            l3_draw_mesh(sc, art_mesh(ART_MISSILE), &t, m_steel);
            l3_line(sc, m->p, l3_sub(m->p, l3_scale(d, 18)), 11);
            l3_line(sc, l3_sub(m->p, l3_scale(d, 18)), l3_sub(m->p, l3_scale(d, 34)), 8);
        }
    for (int i = 0; i < GAME_PROJECTILES; i++) {     /* rattle tracers and the like */
        const game_projectile *q = &cg->v.projectiles[i];
        if (q->life <= 0) continue;
        if (q->kind == WEAPON_RATTLE) {
            l3_vec tail = l3_sub(q->position, l3_scale(q->velocity, .025f));
            l3_line(sc, q->position, tail, q->player ? 11 : 9);
        } else
            l3_point(sc, q->position, 2, q->player ? 15 : 9);
    }
    for (int i = 0; i < CG_DEBRIS; i++)
        if (cg->debris[i].life > 0) {
            cg_debris *d = &cg->debris[i];
            uint8_t ink = d->life > .8f ? 15 : d->life > .5f ? 11 : d->life > .25f ? 9 : LAP_RED;
            l3_line(sc, d->p, l3_sub(d->p, l3_scale(d->v, .04f)), ink);
        }
    for (int i = 0; i < UI_BLASTS; i++) {
        const ui_blast *b = &blasts[i];
        if (b->life <= 0) continue;
        float k = 1 - b->life;             /* 0 at the bang, 1 when gone */
        float s = b->size;
        uint32_t seed = (uint32_t)(b->p.x * 13.f) ^ (uint32_t)(b->p.z * 7.f);
        /* a white flash, then a ball of fire grains: white at the heart, yellow, red at the edge */
        if (k < .1f) l3_sphere(sc, b->p, s * (.3f + k * 5), m_flash);
        if (k < .7f) {
            float ball = s * (.35f + 1.6f * sqrtf(k));
            for (int e = 0; e < 70; e++) {
                uint32_t h = cg_hash(seed * 31u + (uint32_t)e * 2246822519u);
                l3_vec dir = l3_norm(l3v((h & 255) / 127.5f - 1, (h >> 8 & 255) / 127.5f - 1, (h >> 16 & 255) / 127.5f - 1));
                float r = (h >> 24) / 255.f;
                r = r * r;                                   /* most grains near the middle */
                float heat = (1 - r) * (1 - k * 1.3f);
                uint8_t ink = heat > .6f ? 15 : heat > .38f ? 11 : heat > .18f ? 9 : LAP_RED;
                if (heat < .05f && (e & 1)) continue;
                l3_point(sc, l3_add(b->p, l3_scale(dir, ball * r)), heat > .4f ? 3 : 2, ink);
            }
        }
        /* the shockwave: a thin blue ring racing out */
        if (k < .6f) {
            l3_xform ring = l3_pose(b->p, l3_quat_euler((float)(seed % 180), 70, (float)(seed % 90)), 1);
            l3_torus(sc, &ring, s * (.6f + k * 7), s * .04f, m_star[3]);
        }
        /* embers drifting out, cooling from white to red */
        for (int e = 0; e < 26; e++) {
            uint32_t h = cg_hash(seed + (uint32_t)e * 2654435761u);
            l3_vec dir = l3_norm(l3v((h & 255) / 127.5f - 1, (h >> 8 & 255) / 127.5f - 1, (h >> 16 & 255) / 127.5f - 1));
            float reach = s * (.5f + 4.5f * k) * (.5f + (h >> 24) / 255.f);
            uint8_t ink = k < .3f ? 15 : k < .55f ? 11 : k < .8f ? 9 : LAP_RED;
            l3_point(sc, l3_add(b->p, l3_scale(dir, reach)), k < .5f ? 2 : 1, ink);
        }
    }
    (void)p;
}

/* The view's field of view: the smart zoom (ui_zoom_update) narrows it. */
float ui_fov(void) {
    return 2 * atanf(tanf(UI_FOV * CG_RAD * .5f) / fmaxf(1, ui.zoom)) / CG_RAD;
}

/* Your drive seen from the chase camera, which sits behind and above: engines read as
   white-hot nozzles in blue halos with short flickering flames, and the exhaust they
   leave hangs in space as puffs that cool from white through blue to grey. One long
   line pointing back at the camera (3 hull lengths behind) used to project as a stick
   across the bottom of the view; the flames stay short, and a puff is only ever a dot. */
#define PUFFS 120
static struct { l3_vec at, vel; float age, life; } puffs[PUFFS];
static int puffs_n, puffs_head;
static uint32_t puffs_ms;
static float puffs_owed;

static float hull_rear(int cls) {       /* how far the hull reaches behind its centre, mesh units */
    static float rear[SH_COUNT];
    if (cls < 0 || cls >= SH_COUNT) return 1;
    if (rear[cls] == 0) {
        const l3_mesh *m = game_mesh((game_class)cls);
        rear[cls] = .5f;
        for (int k = 0; m && k < m->nv; k++) rear[cls] = fmaxf(rear[cls], m->v[k].z);
    }
    return rear[cls];
}

static void plume(l3_scene *sc, const game_player *p, float mr) {
    const uint32_t now = lap_millis();
    const float dt = puffs_n ? fminf(.1f, (now - puffs_ms) / 1000.f) : 0;
    puffs_ms = now;
    const int cls = game_player_class();
    const l3_vec nose = l3_quat_rotate(p->attitude, l3v(0, 0, -1));   /* the hull's, not the trailing camera's */
    const l3_vec side = l3_quat_rotate(p->attitude, l3v(1, 0, 0)), lift = l3_quat_rotate(p->attitude, l3v(0, 1, 0));
    const l3_vec back = l3_scale(nose, -1);
    const float th = fmaxf(0, p->throttle), boost = p->boost_left > 0 || p->cruising ? 1 : 0;
    const float heat = fminf(1.4f, th + boost * .5f);
    /* two engines a fifth of the way out along the wings, at the back of the hull (pose scale mr / 2) */
    const float out = fminf(wingspan(cls) * mr * .5f * .2f, mr * .35f);
    /* just behind the hull: any further in and the deck hides them from a camera above */
    const l3_vec rear = l3_add(p->position, l3_scale(back, hull_rear(cls) * mr * .5f * 1.1f + mr * .05f));
    const l3_vec noz[2] = {l3_add(rear, l3_scale(side, -out)), l3_add(rear, l3_scale(side, out))};

    /* puffs: born at a nozzle, blown backwards, slowing and spreading as they cool */
    if (heat > .02f) {
        puffs_owed += dt * (30 + heat * 80);
        for (; puffs_owed >= 1; puffs_owed -= 1) {
            uint32_t h = cg_hash(now * 2654435761u + (uint32_t)puffs_head * 40503u);
            float jx = ((h & 255) / 255.f - .5f) * mr * .16f, jy = ((h >> 8 & 255) / 255.f - .5f) * mr * .16f;
            int k = puffs_head;
            puffs_head = (puffs_head + 1) % PUFFS;
            if (puffs_n < PUFFS) puffs_n++;
            puffs[k].at = l3_add(noz[h >> 24 & 1], l3_add(l3_scale(side, jx), l3_scale(lift, jy)));
            puffs[k].vel = l3_add(l3_scale(back, mr * (.9f + heat * 1.6f)), l3_add(l3_scale(side, jx * 3), l3_scale(lift, jy * 3)));
            puffs[k].age = 0;
            puffs[k].life = .6f + (h >> 16 & 255) / 255.f * .7f;
        }
    } else puffs_owed = 0;
    for (int i = 0; i < puffs_n; i++) {
        if (puffs[i].age >= puffs[i].life) continue;
        puffs[i].age += dt;
        puffs[i].at = l3_add(puffs[i].at, l3_scale(puffs[i].vel, dt));
        puffs[i].vel = l3_scale(puffs[i].vel, 1 - fminf(1, dt * 1.8f));
        float a = puffs[i].age / puffs[i].life;
        if (a >= 1 || l3_dot(l3_sub(puffs[i].at, render_eye), render_fwd) < mr * .6f) continue;
        uint8_t ink = a < .1f ? 15 : a < .25f ? 14 : a < .5f ? 12 : a < .78f ? LAP_BLUE : 8;
        l3_point(sc, puffs[i].at, a < .15f ? 3 : a < .45f ? 2 : 1, ink);
    }

    if (heat <= .02f) return;
    const float zmin = mr * .9f;
    for (int e = 0; e < 2; e++) {
        const l3_vec n = noz[e];
        const float flick = .82f + .12f * sinf(now * .043f + e * 2.1f) + .1f * sinf(now * .127f + e * 0.7f);
        const float len = mr * (.18f + heat * .5f) * flick;
        const float zt = l3_dot(l3_sub(n, render_eye), render_fwd);
        /* the nozzle's halo: light blue round deep blue. Points keep their dots against
           later points; lines drawn later win over both. */
        l3_point(sc, n, 3 + heat * 2 * flick, 12);
        l3_point(sc, n, 4 + heat * 3 * flick, LAP_BLUE);
        /* the flame: six blue tongues fanned round a white core, the core drawn last
           so the white-hot root shows through */
        for (int s = 6; s >= 0; s--) {
            float ang = s * (2 * CG_PI / 6) + e, r = s ? .3f : 0;
            float ox = cosf(ang) * r, oy = sinf(ang) * r;
            l3_vec root = l3_add(n, l3_add(l3_scale(side, ox * mr * .25f), l3_scale(lift, oy * mr * .25f)));
            float l = len * (s ? .55f + .2f * sinf(now * .09f + s * 1.7f + e) : 1);
            l3_vec dir = l3_norm(l3_add(back, l3_add(l3_scale(side, ox * .6f), l3_scale(lift, oy * .6f))));
            l3_vec tip = l3_add(root, l3_scale(dir, l));
            float ze = l3_dot(l3_sub(tip, render_eye), render_fwd);     /* never reach back to the camera */
            if (ze < zmin && zt > zmin) tip = l3_add(root, l3_scale(l3_sub(tip, root), (zt - zmin) / (zt - ze)));
            l3_vec a = l3_add(root, l3_scale(l3_sub(tip, root), .4f));
            l3_line(sc, root, a, s ? 14 : 15);
            l3_line(sc, a, tip, s ? LAP_BLUE : 12);
        }
        l3_point(sc, n, 1 + heat * flick, 15);       /* overdrawn by nothing after it */
    }
}

/* Where the view looks from: the cockpit, or (render_view 1) a chase camera
   behind and above the ship that shows its own hull. The screen side projects
   every mark through the same eye and basis (ui_project). */
int render_view;
float render_cam_yaw, render_cam_pitch;
static l3_quat trail;               /* the chase camera's lagging attitude */
static uint32_t trail_ms;
static bool trailing;
l3_vec render_eye, render_fwd = {0, 0, -1}, render_up = {0, 1, 0};

void game_render(l3_scene *sc) {
    if (!cg || !sc)
        return;
    if (cg->scene != sc || !cg->render_ready)
        prepare(sc);
    unwalk(sc);
    system_materials(sc);
    game_view *v = &cg->v;
    game_player *p = &v->player;
    /* The sun lights from the star. */
    l3_vec sun = l3_norm(l3_sub(p->position, v->contents.star));
    l3_sun(sc, sun, .95f, .27f);
    l3_fog(sc, 0, 0, 0);                         /* the hangar and the slot fog their insides; space does not */
    l3_lamp(sc, 1, l3v(0, 0, 0), 0, 1);
    l3_lamp(sc, 0, p->position, .55f, 700);    /* the ship's floodlight: shows the near side */
    l3_quat look = l3_quat_mul(p->attitude, l3_quat_euler(render_shake.x, render_shake.y, 0));
    const game_ship_stats *me = game_player_ship();
    const float mr = me ? me->radius : 10;
    render_fwd = l3_quat_rotate(look, l3v(0, 0, -1));
    render_up = l3_quat_rotate(look, l3v(0, 1, 0));
    render_eye = p->position;
    if (render_view == 1) {             /* chase: behind and above, tipped down so the hull sits low in the view */
        /* The camera trails the ship's turns by a third of a second, so a roll or a
           turn shows as the hull banking and swinging; a big jump (launch, arrival)
           snaps it back behind. */
        const uint32_t now = lap_millis();
        const float tdt = trailing ? fminf(.25f, (now - trail_ms) / 1000.f) : 0;
        trail_ms = now;
        const l3_vec tf = l3_quat_rotate(trail, l3v(0, 0, -1));
        if (!trailing || l3_dot(tf, render_fwd) < .3f) trail = look;
        else trail = l3_quat_slerp(trail, look, 1 - expf(-tdt / .33f));
        trailing = true;
        l3_vec f = l3_quat_rotate(trail, l3v(0, 0, -1)), u = l3_quat_rotate(trail, l3v(0, 1, 0));
        render_eye = l3_add(p->position, l3_add(l3_scale(f, -mr * 3.0f), l3_scale(u, mr * 1.15f)));
        render_fwd = l3_norm(l3_sub(l3_scale(f, 1), l3_scale(u, .18f)));
        render_up = l3_norm(l3_sub(u, l3_scale(render_fwd, l3_dot(u, render_fwd))));
        if (render_cam_yaw != 0 || render_cam_pitch != 0) {     /* swung round: the same distance, looking at the ship */
            l3_quat o = l3_quat_mul(trail, l3_quat_euler(render_cam_yaw, render_cam_pitch, 0));
            render_eye = l3_add(p->position, l3_quat_rotate(o, l3v(0, mr * 1.15f, mr * 3.2f)));
            render_fwd = l3_norm(l3_sub(l3_add(p->position, l3_scale(u, mr * .25f)), render_eye));
            render_up = l3_norm(l3_sub(u, l3_scale(render_fwd, l3_dot(u, render_fwd))));
        }
    } else trailing = false;
    if (render_view == 1) l3_lamp(sc, 0, render_eye, .7f, 700);   /* the floodlight from the camera: lights your hull */
    l3_camera(sc, render_eye, l3_add(render_eye, render_fwd), ui_fov());
    l3_camera_up(sc, render_up);
    l3_begin(sc);
    stars(sc);
    if (v->jump != JUMP_TUNNEL) {
        dust(sc);
        worlds(sc);
        ships(sc);
        if (render_view == 1) {         /* your own hull, with its drive lit by the throttle */
            l3_xform t = l3_pose(p->position, p->attitude, mr * .5f);
            l3_draw_mesh(sc, game_mesh(game_player_class()), &t, m_steel);
            plume(sc, p, mr);
        } else puffs_n = 0;
        if (render_extra) render_extra(sc);
        effects(sc);
    }
    l3_end(sc);
}

/* The hangar's boxes never move: the first frame collects them into one
   mesh, later frames draw it. As traced boxes (shapes looked at over every
   dot they might cover) the bay took the Nano 400 ms a frame. */
#define HANGAR_BOXES 96
static l3_vec hg_v[HANGAR_BOXES * 8];
static uint16_t hg_idx[HANGAR_BOXES * 36];
static uint8_t hg_mats[HANGAR_BOXES * 12];
static l3_mesh hg_mesh = {hg_v, hg_idx, hg_mats, 0, 0};
static int hg_collect;          /* 1: box_at adds to the mesh; 2: built, box_at does nothing */

static void box_at(l3_scene *sc, float x, float y, float z, float hx, float hy, float hz, int mat) {
    if (hg_collect == 2) return;
    if (hg_collect == 1) {
        int nv = hg_mesh.nv, nt = hg_mesh.ntri;
        if (nv + 8 > HANGAR_BOXES * 8) return;
        /* corners by bit: 1 = +x, 2 = +y, 4 = +z; faces counter-clockwise from outside */
        static const uint8_t face[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
        for (int k = 0; k < 8; k++)
            hg_v[nv + k] = l3v(x + (k & 1 ? hx : -hx), y + (k & 2 ? hy : -hy), z + (k & 4 ? hz : -hz));
        for (int f = 0; f < 6; f++) {
            uint16_t *t = &hg_idx[nt * 3];
            const uint8_t *q = face[f];
            t[0] = (uint16_t)(nv + q[0]); t[1] = (uint16_t)(nv + q[1]); t[2] = (uint16_t)(nv + q[2]);
            t[3] = (uint16_t)(nv + q[0]); t[4] = (uint16_t)(nv + q[2]); t[5] = (uint16_t)(nv + q[3]);
            hg_mats[nt] = hg_mats[nt + 1] = (uint8_t)mat;
            nt += 2;
        }
        hg_mesh.nv = nv + 8;
        hg_mesh.ntri = nt;
        return;
    }
    l3_xform t = l3_pose(l3v(x, y, z), l3_quat_euler(0, 0, 0), 1);
    l3_box(sc, &t, l3v(hx, hy, hz), mat);
}

/* Walking leaves its near clip and its lamps 2 and 3 on the scene; every
   other view puts them back. */
static bool walked;
int walk_drawn;                 /* triangles drawn in the last walk frame */
static void unwalk(l3_scene *sc) {
    if (!walked) return;
    walked = false;
    l3_clip(sc, .5f, 26000);
    l3_outline(sc, LAP_BLACK);
    l3_shadows(sc, false);
    l3_flags(sc, 0);
    l3_lamp(sc, 2, l3v(0, 0, 0), 0, 1);
    l3_lamp(sc, 3, l3v(0, 0, 0), 0, 1);
}

/* The layout as one mesh, rebuilt when it changes. Boxes are shapes traced
   per dot over all they might cover, and a room's floor, ceiling and walls
   each cover most of the glass: as boxes they took the Nano 190 ms a frame.
   As triangles they are rasterised once. */
#define WALK_PARTS (WALK_BOXES * 4)     /* each box, and the strips and windows dressed on walls */
static l3_vec walk_v[WALK_PARTS * 8];
static uint16_t walk_idx[WALK_PARTS * 36];
static uint8_t walk_mats[WALK_PARTS * 12];
static walk_box walk_built[WALK_BOXES];
static int walk_built_n = -1;
static l3_mesh walk_mesh = {walk_v, walk_idx, walk_mats, 0, 0};
/* the furniture, in its own mesh so it alone casts the sun's shadows (the
   ceiling would shade the whole room); it shares the corners */
#define WALK_PROPS (WALK_BOXES)
static uint16_t walk_pidx[WALK_PROPS * 36];
static uint8_t walk_pmats[WALK_PROPS * 12];
static l3_mesh walk_props = {walk_v, walk_pidx, walk_pmats, 0, 0};
static bool walk_casts(int kind) {
    return kind == WB_SEAT || kind == WB_CONSOLE || kind == WB_CRATE || kind == WB_TABLE || kind == WB_BUNK ||
           kind == WB_ENGINE || kind == WB_PIPE || kind == WB_CLAMP || kind == WB_RIB;
}
/* windows on space aboard: where their stars go (x on the room side) */
#define WALK_PANES 12
static struct { l3_vec c; float face_x, half_y, half_z; } walk_pane[WALK_PANES];
static int walk_panes;

static int walk_mat(const walk_box *x, int i) {
    switch (x->kind) {
    case WB_FLOOR: case WB_RAMP: case WB_PAD: return m_plate;
    case WB_CEILING: return m_ceil;
    case WB_RIB: case WB_DOOR: case WB_TABLE: return m_trim;
    case WB_SEAT: case WB_CONSOLE: return AM_DARK;
    case WB_CLAMP: case WB_HAZARD: case WB_PIPE: return w_hazard;
    case WB_WINDOW: return m_pane;      /* dark glass, stars drawn on it */
    case WB_LIGHT: return m_strip;
    case WB_CRATE: return (i * 7) % 3 ? m_trim : w_hazard;
    case WB_ENGINE: return w_steel;
    default: return m_panel;            /* walls and ceilings */
    }
}

/* Rebuilt when the layout or the eye changes (standing still keeps the
   engine's static cache): only boxes in front of the eye and inside a cone
   wider than the view, nearest first so the depth test turns the walls
   behind away before they are shaded, and only the faces turned to the eye. */
static l3_vec walk_eye_built;
static float walk_yaw_built, walk_pitch_built;

/* One box into a mesh: its corners, and the faces turned to the eye. A
   caster keeps all six: the sun's pass needs the faces it sees. */
static void walk_part_in(l3_vec c, l3_vec half, int mat, l3_vec eye, bool cast) {
    int nv = walk_mesh.nv;
    l3_mesh *m = cast ? &walk_props : &walk_mesh;
    uint16_t *idx = cast ? walk_pidx : walk_idx;
    uint8_t *mats = cast ? walk_pmats : walk_mats;
    int nt = m->ntri;
    if (nv + 8 > WALK_PARTS * 8 || nt + 12 > (cast ? WALK_PROPS : WALK_PARTS) * 12) return;
    /* corners by bit: 1 = +x, 2 = +y, 4 = +z; faces counter-clockwise from outside */
    static const uint8_t face[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    for (int k = 0; k < 8; k++)
        walk_v[nv + k] = l3v(c.x + (k & 1 ? half.x : -half.x), c.y + (k & 2 ? half.y : -half.y), c.z + (k & 4 ? half.z : -half.z));
    const bool shows[6] = {eye.x < c.x - half.x, eye.x > c.x + half.x, eye.y < c.y - half.y,
                           eye.y > c.y + half.y, eye.z < c.z - half.z, eye.z > c.z + half.z};
    for (int f = 0; f < 6; f++) {
        if (!cast && !shows[f]) continue;
        const uint8_t *q = face[f];
        uint16_t *t = &idx[nt * 3];
        t[0] = (uint16_t)(nv + q[0]); t[1] = (uint16_t)(nv + q[1]); t[2] = (uint16_t)(nv + q[2]);
        t[3] = (uint16_t)(nv + q[0]); t[4] = (uint16_t)(nv + q[2]); t[5] = (uint16_t)(nv + q[3]);
        mats[nt] = mats[nt + 1] = (uint8_t)mat;
        nt += 2;
    }
    walk_mesh.nv = walk_props.nv = nv + 8;
    m->ntri = nt;
}
static void walk_part(l3_vec c, l3_vec half, int mat, l3_vec eye) { walk_part_in(c, half, mat, eye, false); }

static void walk_mesh_build(const walk_box *b, int n, l3_vec eye, float yaw, float pitch, bool aboard) {
    if (n == walk_built_n && !memcmp(walk_built, b, (size_t)n * sizeof *b) && !memcmp(&eye, &walk_eye_built, sizeof eye) &&
        yaw == walk_yaw_built && pitch == walk_pitch_built)
        return;
    if (n > WALK_BOXES) n = WALK_BOXES;
    const float yr = yaw * CG_RAD, pr = pitch * CG_RAD;
    const l3_vec fwd = l3v(sinf(yr) * cosf(pr), sinf(pr), -cosf(yr) * cosf(pr));
    const float cone = 66 * CG_RAD;
    int order[WALK_BOXES], m = 0;
    float key[WALK_BOXES];
    for (int i = 0; i < n; i++) {
        const walk_box *x = &b[i];
        /* the eye's distance to the box (0 inside it) */
        float dx = fmaxf(0, fabsf(eye.x - x->c.x) - x->half.x), dy = fmaxf(0, fabsf(eye.y - x->c.y) - x->half.y),
              dz = fmaxf(0, fabsf(eye.z - x->c.z) - x->half.z);
        float gap = sqrtf(dx * dx + dy * dy + dz * dz);
        const l3_vec d = l3_sub(x->c, eye);
        const float r = sqrtf(l3_dot(x->half, x->half)), dist = sqrtf(l3_dot(d, d));
        if (dist > r) {
            const float ahead = l3_dot(d, fwd);
            if (ahead < -r) continue;
            const float spread = cone + asinf(r / dist);   /* the cone, widened by the box's own size */
            if (spread < CG_PI && ahead / dist < cosf(spread)) continue;
        }
        int j = m++;
        while (j > 0 && key[j - 1] > gap) { key[j] = key[j - 1]; order[j] = order[j - 1]; j--; }
        key[j] = gap;
        order[j] = i;
    }
    walk_mesh.nv = walk_mesh.ntri = walk_props.nv = walk_props.ntri = 0;
    walk_panes = 0;
    float side = 0, zmin = 1e9f, zmax = -1e9f;      /* the ship's outer walls, for windows; its length, for room colours */
    for (int i = 0; i < n; i++) {
        if (aboard && b[i].kind == WB_WALL && b[i].half.z > b[i].half.x) side = fmaxf(side, fabsf(b[i].c.x));
        if (b[i].kind == WB_FLOOR) { zmin = fminf(zmin, b[i].c.z - b[i].half.z); zmax = fmaxf(zmax, b[i].c.z + b[i].half.z); }
    }
    for (int o = 0; o < m; o++) {
        const int i = order[o];
        const walk_box *x = &b[i];
        walk_part_in(x->c, x->half, walk_mat(x, i), eye, walk_casts(x->kind));
        if (x->kind == WB_CEILING) {        /* light panels down the length, three rows on a wide ceiling */
            const bool along_z = x->half.z >= x->half.x;
            const float run = along_z ? x->half.z : x->half.x, wide = along_z ? x->half.x : x->half.z;
            const int rows = wide > 6 ? 3 : 1;
            const float y = x->c.y - x->half.y - .02f;
            for (int r = 0; r < rows; r++) {
                const float across = rows == 1 ? 0 : (r - 1) * wide * .55f;
                for (float u = -run + 1.4f; u <= run - 1.2f; u += 2.6f) {
                    const l3_vec c = along_z ? l3v(x->c.x + across, y, x->c.z + u) : l3v(x->c.x + u, y, x->c.z + across);
                    walk_part(c, along_z ? l3v(.32f, .02f, .8f) : l3v(.8f, .02f, .32f), m_glow, eye);
                }
            }
            continue;
        }
        if (x->kind == WB_DOOR) {           /* a neon edge under the frame */
            walk_part(l3v(x->c.x, x->c.y - x->half.y - .02f, x->c.z), l3v(x->half.x + .01f, .02f, x->half.z + .01f), m_neon, eye);
            continue;
        }
        if (x->kind != WB_WALL) continue;
        /* dress the walls: neon where they meet the deck, a band of light up
           high, and windows in the hull's own sides */
        const bool along_z = x->half.z >= x->half.x;
        const float run = along_z ? x->half.z : x->half.x;
        if (run < .6f) continue;
        const float base = x->c.y - x->half.y, top = x->c.y + x->half.y;
        const l3_vec thick = along_z ? l3v(x->half.x + .025f, 0, 0) : l3v(0, 0, x->half.z + .025f);
        const l3_vec len = along_z ? l3v(0, 0, run) : l3v(run, 0, 0);
        {   /* a coloured band to waist height: cockpit teal, corridor blue, hold amber, engines red; the hangar teal */
            const float f = zmax > zmin ? (x->c.z - zmin) / (zmax - zmin) : 0;
            const int room = !aboard ? 0 : f < .28f ? 0 : f < .42f ? 1 : f < .78f ? 2 : 3;
            const l3_vec bt = along_z ? l3v(x->half.x + .012f, 0, 0) : l3v(0, 0, x->half.z + .012f);
            const float h = fminf(.55f, x->half.y);
            walk_part(l3v(x->c.x, base + h, x->c.z), l3_add(l3_add(bt, len), l3v(0, h, 0)), m_band[room], eye);
        }
        walk_part(l3v(x->c.x, base + .07f, x->c.z), l3_add(l3_add(thick, len), l3v(0, .035f, 0)), m_neon, eye);
        walk_part(l3v(x->c.x, top - .18f, x->c.z), l3_add(l3_add(thick, len), l3v(0, .03f, 0)), i % 3 ? m_strip : m_neon2, eye);
        if (aboard && along_z && side > 0 && fabsf(x->c.x) > side - .3f && run > 1.4f && walk_panes < WALK_PANES) {
            const float hz = fminf(run - .5f, 2.4f);
            walk_part(l3v(x->c.x, 1.45f, x->c.z), l3v(x->half.x + .02f, .42f, hz), m_pane, eye);
            walk_part(l3v(x->c.x, 1.9f, x->c.z), l3v(x->half.x + .03f, .04f, hz + .06f), m_trim, eye);
            walk_part(l3v(x->c.x, 1.0f, x->c.z), l3v(x->half.x + .03f, .04f, hz + .06f), m_trim, eye);
            walk_pane[walk_panes].c = l3v(x->c.x, 1.45f, x->c.z);
            walk_pane[walk_panes].face_x = x->c.x + (x->c.x > 0 ? -1 : 1) * (x->half.x + .03f);
            walk_pane[walk_panes].half_y = .38f;
            walk_pane[walk_panes].half_z = hz - .05f;
            walk_panes++;
        }
    }
    memcpy(walk_built, b, (size_t)n * sizeof *b);
    walk_built_n = n;
    walk_eye_built = eye;
    walk_yaw_built = yaw;
    walk_pitch_built = pitch;
}

/* On foot: the layout walk.c built, as boxes in the station's materials, lit
   by the two ceiling lights nearest you and a little lamp at your eye; your
   ship on the pad when in the hangar; a soft mark on every place to use, the
   one in reach brighter. */
void render_walk(l3_scene *sc, const walk_state *w, float t) {
    if (!cg || !sc || !w)
        return;
    if (cg->scene != sc || !cg->render_ready)
        prepare_walk(sc);
    walked = true;
    const bool aboard = w->place == WALK_SHIP;
    l3_clip(sc, .06f, 160);
    /* bright and warm: a high slanted key light and plenty of fill. No
       outlines on foot - a floor seen low across is all depth steps, and
       outlined it went black. */
    l3_sun(sc, l3v(-.45f, -1, -.3f), .42f, aboard ? .36f : .3f);
    l3_fog(sc, LAP_BLACK, aboard ? 14 : 34, aboard ? 30 : 80);
    l3_outline(sc, LAP_DOT_CLEAR);
    l3_vec eye = walk_eye(w);
    l3_camera_up(sc, l3v(0, 1, 0));     /* the slot and the flight camera leave it rolled */
    l3_camera_look(sc, eye, w->yaw, w->pitch, 70);
    const walk_box *b;
    int n = walk_boxes(&b);
    /* the two ceiling lights nearest the eye light the room */
    int near[2] = {-1, -1};
    float nd[2] = {1e9f, 1e9f};
    for (int i = 0; i < n; i++) {
        if (b[i].kind != WB_LIGHT) continue;
        l3_vec d = l3_sub(b[i].c, eye);
        float dd = l3_dot(d, d);
        if (dd < nd[0]) { nd[1] = nd[0]; near[1] = near[0]; nd[0] = dd; near[0] = i; }
        else if (dd < nd[1]) { nd[1] = dd; near[1] = i; }
    }
    /* one ceiling light and the eye's lamp: every lamp costs each of the
       ~25,000 dots a full room covers */
    if (near[0] >= 0) l3_lamp(sc, 0, l3_add(b[near[0]].c, l3v(0, -.4f, 0)), aboard ? .6f : .45f, aboard ? 10 : 24);
    else l3_lamp(sc, 0, l3v(0, 0, 0), 0, 1);
    l3_lamp(sc, 1, eye, .38f, aboard ? 8 : 11);     /* where you look is always lit: the ship has no ceiling lights */
    l3_lamp(sc, 2, l3v(0, 0, 0), 0, 1);
    walk_mesh_build(b, n, eye, w->yaw, w->pitch, aboard);
    walk_drawn = walk_mesh.ntri + walk_props.ntri;
    l3_shadows(sc, true);           /* the furniture and the hull cast the key light's shadows */
    l3_begin(sc);
    l3_xform at = l3_pose(l3v(0, 0, 0), l3_quat_euler(0, 0, 0), 1);
    l3_flags(sc, 0);
    l3_draw_mesh(sc, &walk_mesh, &at, AM_METAL);
    l3_flags(sc, L3_CAST);
    if (walk_props.ntri) l3_draw_mesh(sc, &walk_props, &at, AM_METAL);
    for (int i = 0; i < n; i++) {
        const walk_box *x = &b[i];
        if (x->kind == WB_CONSOLE) {       /* a lit screen on its top */
            float s = fminf(x->half.x, x->half.z) * .8f;
            l3_point(sc, l3v(x->c.x, x->c.y + x->half.y + .02f, x->c.z), s * 6, (int)(t * 3 + i) % 4 ? 12 : 10);
        }
        if (x->kind == WB_ENGINE)          /* the drive's glow at its aft face */
            l3_disc(sc, l3v(x->c.x, x->c.y, x->c.z + x->half.z + .01f), l3v(0, 0, 1),
                    fminf(x->half.x, x->half.y) * .7f, AM_ENGINE);
    }
    const walk_hull *h = walk_hull_on_pad();
    if (h) {
        l3_xform s = l3_pose(h->centre, l3_quat_euler(0, 0, 0), h->scale);
        l3_draw_mesh(sc, game_mesh(game_player_class()), &s, w_steel);
    }
    l3_flags(sc, 0);
    for (int i = 0; i < n; i++) {               /* the layout's own windows: stars on the side you stand */
        const walk_box *x = &b[i];
        if (x->kind != WB_WINDOW) continue;
        const int thin = x->half.x <= x->half.y && x->half.x <= x->half.z ? 0 : x->half.z <= x->half.y ? 2 : 1;
        const float c[3] = {x->c.x, x->c.y, x->c.z}, h[3] = {x->half.x, x->half.y, x->half.z}, e[3] = {eye.x, eye.y, eye.z};
        uint32_t r = cg_hash(0x51DE + (uint32_t)i * 131);
        for (int s = 0; s < 40; s++) {
            float p[3];
            for (int a = 0; a < 3; a++) p[a] = c[a] + (cg_unit(&r) * 2 - 1) * h[a] * .92f;
            p[thin] = c[thin] + (e[thin] > c[thin] ? 1 : -1) * (h[thin] + .02f);
            uint8_t ink = s % 6 == 0 ? ((int)(t * 2 + s) % 3 ? 15 : 7) : s % 3 ? 7 : 12;
            l3_point(sc, l3v(p[0], p[1], p[2]), s % 9 ? 1 : 1.5f, ink);
        }
    }
    for (int k = 0; k < walk_panes; k++) {     /* stars in the windows, a few twinkling */
        uint32_t r = cg_hash(0xA11E + (uint32_t)k * 97);
        for (int s = 0; s < 16; s++) {
            float y = walk_pane[k].c.y + (cg_unit(&r) * 2 - 1) * walk_pane[k].half_y;
            float z = walk_pane[k].c.z + (cg_unit(&r) * 2 - 1) * walk_pane[k].half_z;
            uint8_t ink = s % 5 == 0 ? ((int)(t * 2 + s) % 3 ? 15 : 7) : s % 3 ? 7 : 12;
            l3_point(sc, l3v(walk_pane[k].face_x, y, z), s % 7 ? 1 : 1.5f, ink);
        }
    }
    const walk_spot *sp;
    int ns = walk_spots(&sp);
    for (int i = 0; i < ns; i++) {
        bool on = i == w->spot;
        float pulse = .5f + .5f * sinf(t * 4 + i);
        l3_point(sc, sp[i].p, on ? 3 + pulse * 2 : 1.5f, on ? 15 : pulse > .5f ? 11 : 3);
    }
    l3_end(sc);
}

/* The hangar: a closed bay round your ship on its pad - panelled walls with
   ribs, rows of ceiling lights, the pad's markings and landing lights, clamps
   on the hull, crates and a fuel line, the control room's lit window and a
   lift door with its warning lamp - seen by a camera walking slowly round. */
void render_hangar(l3_scene *sc, float t) {
    if (!cg || !sc)
        return;
    if (cg->scene != sc || !cg->render_ready)
        prepare(sc);
    unwalk(sc);
    const float B = 15, top = 9, deck = -1.2f;  /* half the bay's width and depth; ceiling and deck heights */
    l3_sun(sc, l3v(-.2f, -1, -.3f), .38f, .14f);
    l3_lamp(sc, 0, l3v(0, top - 1.5f, 0), .7f, 22);            /* the pad's flood from the ceiling */
    l3_lamp(sc, 1, l3v(-B + 2, 4, -B + 3), .3f, 12);           /* the control room's spill */
    l3_fog(sc, LAP_BLACK, 24, 48);
    l3_camera_up(sc, l3v(0, 1, 0));     /* level, whatever the slot's roll left */
    l3_camera_orbit(sc, l3v(0, .6f, 0), 10.5f, t * 9 + 140, 13, 52);
    l3_begin(sc);
    hg_collect = hg_mesh.ntri ? 2 : 1;  /* the boxes below go into the mesh once */
    /* the bay: deck, ceiling, four walls */
    box_at(sc, 0, deck - .5f, 0, B, .5f, B, m_deck);
    box_at(sc, 0, top + .5f, 0, B, .5f, B, m_wall);
    box_at(sc, 0, (top + deck) / 2, -B - .5f, B, (top - deck) / 2, .5f, m_wall);
    box_at(sc, 0, (top + deck) / 2, B + .5f, B, (top - deck) / 2, .5f, m_wall);
    box_at(sc, -B - .5f, (top + deck) / 2, 0, .5f, (top - deck) / 2, B, m_wall);
    box_at(sc, B + .5f, (top + deck) / 2, 0, .5f, (top - deck) / 2, B, m_wall);
    /* ribs up every wall, beams across the ceiling, a light row along each beam */
    for (int i = -3; i <= 3; i++) {
        float u = i * B / 3.5f;
        box_at(sc, u, (top + deck) / 2, -B + .35f, .25f, (top - deck) / 2, .35f, AM_DARK);
        box_at(sc, u, (top + deck) / 2, B - .35f, .25f, (top - deck) / 2, .35f, AM_DARK);
        box_at(sc, -B + .35f, (top + deck) / 2, u, .35f, (top - deck) / 2, .25f, AM_DARK);
        box_at(sc, B - .35f, (top + deck) / 2, u, .35f, (top - deck) / 2, .25f, AM_DARK);
        box_at(sc, u, top - .3f, 0, .3f, .3f, B, AM_DARK);
        for (int k = -4; k <= 4; k++) l3_point(sc, l3v(u, top - .65f, k * B / 4.5f), 1, 15);
    }
    /* the pad: a dark disc, hazard blocks round its rim, landing lights chasing round */
    l3_disc(sc, l3v(0, deck + .02f, 0), l3v(0, 1, 0), 5.2f, AM_DARK);
    for (int i = 0; i < 16; i++) {
        float a = i * CG_PI / 8;
        if (i & 1) {
            l3_xform h = l3_pose(l3v(cosf(a) * 5.6f, deck + .05f, sinf(a) * 5.6f), l3_quat_euler(-a / CG_RAD, 0, 0), 1);
            l3_box(sc, &h, l3v(.22f, .04f, .5f), m_hazard);
        }
    }
    for (int i = 0; i < 20; i++) {
        float a = i * CG_PI / 10;
        l3_point(sc, l3v(cosf(a) * 4.6f, deck + .1f, sinf(a) * 4.6f), 1, (i + (int)(t * 6)) % 5 ? 3 : 11);
    }
    for (int k = -2; k <= 2; k++) {                     /* the pad's centre line and cross */
        l3_point(sc, l3v(k * .9f, deck + .06f, 0), 1, 11);
        l3_point(sc, l3v(0, deck + .06f, k * .9f), 1, 11);
    }
    /* your ship on it, held by four clamps */
    l3_xform s = l3_pose(l3v(0, .3f, 0), l3_quat_euler(0, 0, 0), 2.1f);
    l3_draw_mesh(sc, game_mesh(game_player_class()), &s, m_steel);
    for (int i = 0; i < 4; i++) {
        float a = CG_PI / 4 + i * CG_PI / 2;
        l3_vec foot = l3v(cosf(a) * 2.4f, deck, sinf(a) * 2.4f);
        l3_cylinder(sc, foot, l3_add(foot, l3v(0, 1.0f, 0)), .22f, m_hazard);
    }
    /* crates by the walls, a fuel line from the wall to the pad's edge */
    box_at(sc, B - 2.2f, deck + .8f, B - 2.4f, .8f, .8f, .8f, m_wall);
    box_at(sc, B - 3.9f, deck + .6f, B - 2.1f, .6f, .6f, .6f, m_hazard);
    box_at(sc, B - 2.3f, deck + 2.15f, B - 2.4f, .55f, .55f, .55f, m_wall);
    box_at(sc, -B + 2.5f, deck + .7f, B - 3.0f, 1.2f, .7f, .7f, m_wall);
    l3_cylinder(sc, l3v(B - .6f, deck + .4f, -3), l3v(6.2f, deck + .4f, -3), .2f, m_hazard);
    l3_cylinder(sc, l3v(6.2f, deck + .4f, -3), l3v(6.2f, deck + 1.6f, -3), .2f, m_hazard);
    /* the control room's window high on the back wall, figures of light behind it */
    l3_quad(sc, l3v(-9, 6.2f, -B + .02f), l3v(-3, 6.2f, -B + .02f), l3v(-3, 4.4f, -B + .02f), l3v(-9, 4.4f, -B + .02f), m_window);
    box_at(sc, -6, 4.2f, -B + .4f, 3.4f, .15f, .4f, AM_DARK);
    /* the lift door: hazard frame, its lamp blinking while the bay is pressurised */
    box_at(sc, B - .05f, deck + 2.8f, 4, .1f, .25f, 2.6f, m_hazard);
    box_at(sc, B - .05f, deck + .15f, 4, .1f, .15f, 2.6f, m_hazard);
    box_at(sc, B - .05f, deck + 1.5f, 1.5f, .1f, 1.4f, .2f, m_hazard);
    box_at(sc, B - .05f, deck + 1.5f, 6.5f, .1f, 1.4f, .2f, m_hazard);
    if (((int)(t * 2)) & 1) l3_sphere(sc, l3v(B - .3f, deck + 3.5f, 4), .22f, m_red);
    hg_collect = 0;
    l3_xform at = l3_pose(l3v(0, 0, 0), l3_quat_euler(0, 0, 0), 1);
    if (hg_mesh.ntri) l3_draw_mesh(sc, &hg_mesh, &at, AM_DARK);
    l3_end(sc);
}

/* The station's slot from inside: a long square corridor of ribs and light
   rows, guide lights running out along the deck, turning with the station.
   `k` goes 0 to 1 through it; leaving, the mouth opens on stars; arriving,
   the far end is the bright hangar. */
void render_tunnel(l3_scene *sc, float k, bool leaving) {
    if (!cg || !sc)
        return;
    if (cg->scene != sc || !cg->render_ready)
        prepare(sc);
    unwalk(sc);
    const float W = 7, H = 4, L = 150;          /* half width and height; length */
    k = cg_clamp(k, 0, 1);
    float z = 10 - k * (L + 6), roll = (leaving ? 1 : -1) * (20 + k * 50) * CG_RAD;
    l3_sun(sc, l3v(.2f, -1, -.2f), .22f, .1f);
    l3_lamp(sc, 0, l3v(0, 0, z - 8), .5f, 38);
    l3_lamp(sc, 1, l3v(0, 0, 0), 0, 1);
    l3_fog(sc, LAP_BLACK, 60, L + 40);         /* dark down the corridor, the far mouth still shows */
    l3_camera(sc, l3v(0, 0, z), l3v(0, 0, z - 10), 72);
    l3_camera_up(sc, l3v(sinf(roll), cosf(roll), 0));
    l3_begin(sc);
    box_at(sc, 0, -H - .4f, -L / 2, W, .4f, L / 2, m_deck);
    box_at(sc, 0, H + .4f, -L / 2, W, .4f, L / 2, m_wall);
    box_at(sc, -W - .4f, 0, -L / 2, .4f, H, L / 2, m_wall);
    box_at(sc, W + .4f, 0, -L / 2, .4f, H, L / 2, m_wall);
    for (int i = 0; i <= (int)(L / 10); i++) {      /* ribs: a frame every ten metres, a lamp in each corner */
        float zz = -i * 10.f;
        box_at(sc, 0, H - .25f, zz, W, .3f, .5f, AM_DARK);
        box_at(sc, 0, -H + .25f, zz, W, .3f, .5f, AM_DARK);
        box_at(sc, -W + .25f, 0, zz, .3f, H, .5f, AM_DARK);
        box_at(sc, W - .25f, 0, zz, .3f, H, .5f, AM_DARK);
        l3_point(sc, l3v(-W + .7f, H - .7f, zz), 2, 15);
        l3_point(sc, l3v(W - .7f, H - .7f, zz), 2, 15);
    }
    for (int i = 0; i < 64; i++) {                  /* guide lights: a pulse running out along the deck */
        float zz = -i * 2.4f;
        bool lit = ((i - (int)(k * 160)) % 8 + 8) % 8 == 0;
        uint8_t ink = lit ? (leaving ? 11 : 15) : 8;
        l3_point(sc, l3v(-W + 1.3f, -H + .05f, zz), lit ? 2 : 1, ink);
        l3_point(sc, l3v(W - 1.3f, -H + .05f, zz), lit ? 2 : 1, ink);
    }
    for (int i = -3; i <= 3; i++)                   /* hazard bands at both mouths */
        if (i & 1) {
            box_at(sc, i * 2.f, -H + .1f, -2, .9f, .1f, .6f, m_hazard);
            box_at(sc, i * 2.f, -H + .1f, -L + 2, .9f, .1f, .6f, m_hazard);
        }
    if (leaving)                                    /* space through the far mouth */
        for (int i = 0; i < 40; i++) {
            uint32_t r = cg_hash((uint32_t)i * 2654435761u);
            l3_point(sc, l3v((cg_unit(&r) * 2 - 1) * 9, (cg_unit(&r) * 2 - 1) * 6, -L - 8 - cg_unit(&r) * 20), 1, i % 3 ? LAP_WHITE : 15);
        }
    else                                            /* the hangar's light at the end */
        l3_quad(sc, l3v(-W, -H, -L), l3v(W, -H, -L), l3v(W, H, -L), l3v(-W, H, -L), m_flash);
    l3_end(sc);
}

/* One hull alone on black, turning, pushed sideways by `shift` so words fit beside it. */
void render_showcase(l3_scene *sc, int ship, float t, float shift, float back) {
    if (!cg || !sc)
        return;
    if (cg->scene != sc || !cg->render_ready)
        prepare(sc);
    unwalk(sc);
    l3_sun(sc, l3v(-.5f, -.6f, -.6f), .95f, .16f);
    l3_lamp(sc, 0, l3v(0, 0, 0), 0, 1);
    l3_camera(sc, l3v(shift, 3 * back, 11 * back), l3v(shift, 0, 0), 40);
    l3_camera_up(sc, l3v(0, 1, 0));
    l3_begin(sc);
    l3_xform s = l3_pose(l3v(0, 0, 0), l3_quat_euler(t * 20, 8, 0), 2.2f);
    l3_draw_mesh(sc, ship == SH_STATION ? art_mesh(ART_DODECA) : game_mesh((game_class)ship), &s, m_steel);
    l3_end(sc);
}

/* For the screen side's own scene extras (mesh commanders): a material by name. */
int render_material(int which) {
    switch (which) {
    case 1: return m_police;
    case 2: return m_gun;
    case 3: return m_fire;
    case 4: return m_flash;
    case 5: return m_rock;
    case 6: return m_moon;
    case 7: case 8: case 9: case 10: return m_star[which - 7];
    default: return m_steel;
    }
}
void (*render_extra)(l3_scene *sc);

/* One hull turning, centred `lift` units above the view's middle (a card on top of a list). */
void render_hull_card(l3_scene *sc, int ship, float t, float lift) {
    if (!cg || !sc)
        return;
    if (cg->scene != sc || !cg->render_ready)
        prepare(sc);
    unwalk(sc);
    const l3_mesh *m = game_mesh((game_class)ship);
    float r = 1;
    for (int k = 0; m && k < m->nv; k++) r = fmaxf(r, cg_len(m->v[k]));
    l3_sun(sc, l3v(-.5f, -.6f, -.6f), .95f, .2f);
    l3_lamp(sc, 0, l3v(0, 0, 0), 0, 1);
    l3_camera(sc, l3v(0, 2.2f * r - lift, 6.5f * r), l3v(0, -lift, 0), 40);
    l3_camera_up(sc, l3v(0, 1, 0));
    l3_begin(sc);
    l3_xform s = l3_pose(l3v(0, 0, 0), l3_quat_euler(t * 25, 10, 0), 1);
    l3_draw_mesh(sc, m, &s, m_steel);
    l3_end(sc);
}
