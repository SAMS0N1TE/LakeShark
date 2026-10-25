/* CRUISE drive and pursuit rules. No IO, allocation, drawing or receiver writes. */
#include "internal.h"

static float top(void) { return game_player_ship()->speed; }
static float smooth(float t) { return t * t * (3 - 2 * t); }
static void reason(const char *s) {
    snprintf(cg->cruise.reason, sizeof(cg->cruise.reason), "%.24s", s);
    cg->v.player.cruise_reason = cg->cruise.reason;
}
void cg_cruise_reset(void) {
    cg_combat_player_ensure(&cg->v.player);
    memset(&cg->cruise, 0, sizeof(cg->cruise));
    memset(&cg->v.interdiction, 0, sizeof(cg->v.interdiction));
    cg->v.interdiction.who = -1;
    cg->v.player.cruising = false;
    cg->v.player.cruise_factor = 1;
    cg->v.player.cruise_charge = 0;
    reason("");
}
bool cg_cruise_mass_locked(void) {
    game_view *v = &cg->v;
    bool locked = cg_len(l3_sub(v->player.position, v->contents.station)) < 650;
    for (int i = 0; i < v->contents.planet_count; i++) {
        game_planet *b = &v->contents.planets[i];
        if (cg_len(l3_sub(v->player.position, b->position)) < b->radius + 500)
            locked = true;
    }
    v->player.mass_locked = locked;
    return locked;
}
static void drop(const char *s) {
    game_player *p = &cg->v.player;
    p->cruising = false;
    p->cruise_charge = 0;
    cg->cruise.bleed = 1.5f;
    /* Include actual velocity, including any external impulse. */
    cg->cruise.bleed_factor = fmaxf(p->cruise_factor, cg_len(p->velocity) / top());
    cg->v.interdiction.active = false;
    reason(s);
}
void cg_cruise_hot(void) { if(cg->v.player.cruising) drop("Weapons hot"); }
static bool pursuer(int who) {
    if (who < 0 || who >= GAME_CONTACTS || cg->v.traffic_mode == 2) return false;
    game_contact *c = &cg->v.contacts[who];
    return c->active && !c->live && !cg->mapped[who] && cg->npc_control[who] &&
        c->jump_out <= 0 && !cg->encounter[who].fleeing && !cg_friendly(c->id) &&
        (c->role == ROLE_PIRATE || (c->role == ROLE_POLICE && cg->v.player.legal >= 128));
}
bool cg_interdict(int who) {
    game_view *v = &cg->v;
    if (!v->player.cruising || v->interdiction.active || !pursuer(who) ||
        cg_cruise_mass_locked() ||
        cg_len(l3_sub(v->player.position, v->contents.station)) <= fmaxf(1800, cg->encounters.safe_radius))
        return false;
    v->interdiction.active = true;
    v->interdiction.who = who;
    v->interdiction.tug = v->interdiction.progress = 0;
    cg->cruise.axis = v->interdiction.escape = cg_forward();
    cg->cruise.elapsed = 0;
    cg->cruise.who_id = v->contacts[who].id;
    cg_message("Interdiction! Follow escape vector or submit.");
    return true;
}
static void caught(bool lost) {
    game_player *p = &cg->v.player;
    int who = cg->v.interdiction.who;
    drop("Interdicted");
    /* The pursuer exits alongside the player; never teleport into a body. */
    game_contact *c = &cg->v.contacts[who];
    c->position = l3_sub(p->position, l3_scale(cg_forward(), 180));
    c->hostile = true;
    cg->anchor[who] = p->position;
    cg->encounter_enemy[who] = -1;
    p->cruise_factor = 1;
    cg->cruise.bleed = 0;
    p->velocity = l3_scale(cg_forward(), top() * p->throttle);
    p->speed = cg_len(p->velocity);
    cg->cruise.stall = lost ? 2 : 0;
}
bool game_cruise(bool on) {
    if (!cg) return false;
    game_player *p = &cg->v.player;
    if (!on) {
        int who = cg->v.interdiction.who;
        if (cg->v.interdiction.active && pursuer(who) &&
            cg->v.contacts[who].id == cg->cruise.who_id) caught(false);
        else if (p->cruising) drop("CRUISE disengaged");
        return true;
    }
    if(game_drive_refusal()) {reason(game_drive_refusal());cg_message(game_drive_refusal());return false;}
    if(p->throttle<0) { p->throttle=0; reason("Reverse thrust"); return false; }
    const char *why = p->dead ? "Commander lost" : p->docked ? "Docked" :
        cg->v.jump != JUMP_IDLE ? "Jumping" : p->docking ? "Docking" :
        cg_cruise_mass_locked() ? "Mass locked" : cg->cruise.hit > 0 ? "Shields charging" :
        cg->cruise.stall > 0 ? "Engine stalled" : NULL;
    if (why) { reason(why); return false; }
    if (!p->cruising) {
        p->cruising = true;
        p->cruise_factor = fmaxf(1, p->cruise_factor);
        /* Re-engagement during bleed resumes at the current factor. */
        float lo = 0, hi = 1;
        for (int i = 0; i < 16; i++) {
            float m = (lo + hi) * .5f;
            if (1 + 24 * smooth(m) < p->cruise_factor) lo = m; else hi = m;
        }
        p->cruise_charge = p->cruise_factor <= 1 ? 0 : (lo + hi) * .5f;
        cg->cruise.bleed = 0;
        p->firing = false;
        reason("");
    }
    return true;
}
float game_cruise_eta(l3_vec target) {
    if (!cg || !isfinite(target.x) || !isfinite(target.y) || !isfinite(target.z)) return INFINITY;
    game_player *p = &cg->v.player;
    float distance = cg_len(l3_sub(target, p->position));
    float speed = top() * p->throttle * (p->cruising ? fmaxf(1, p->cruise_factor) : 1);
    return distance == 0 ? 0 : speed > 0 ? distance / speed : INFINITY;
}
/* Ray intersects the safe sphere within three seconds, including this step.
 * Test both inertia and commanded heading so a turn cannot bypass braking. */
static bool approaching(l3_vec centre, float radius, float dt) {
    game_player *p = &cg->v.player;
    l3_vec off = l3_sub(p->position, centre);
    float q = l3_dot(off, off) - radius * radius;
    if (q <= 0) return true;
    float speed = fmaxf(cg_len(p->velocity), top() * p->throttle * p->cruise_factor);
    for (int i = 0; i < 2; i++) {
        l3_vec d = i ? cg_forward() : l3_norm(p->velocity);
        float b = l3_dot(off, d), det = b * b - q;
        if (b < 0 && det >= 0 && -b - sqrtf(det) <= speed * (3 + dt)) return true;
    }
    return false;
}
void cg_cruise_update(float dt) {
    game_view *v = &cg->v;
    game_player *p = &v->player;
    cg->cruise.hit = fmaxf(0, cg->cruise.hit - dt);
    cg->cruise.stall = fmaxf(0, cg->cruise.stall - dt);
    if (p->cruise_factor < 1) p->cruise_factor = 1;
    if (p->cruising) {
        if (p->dead || p->docked || p->docking || v->jump != JUMP_IDLE)
            drop(p->dead ? "Commander lost" : p->docked ? "Docked" : p->docking ? "Docking" : "Jumping");
        else if (p->firing || p->firing2) drop("Weapons hot");
        else if (p->throttle<0) drop("Reverse thrust");
        else if (p->mass_locked) drop("Mass locked");
        else if (approaching(v->contents.station, 650, dt)) drop("Station approach");
        else if (approaching(v->contents.star, v->contents.star_radius + 500, dt)) drop("Star approach");
        else for (int i = 0; i < v->contents.planet_count; i++) {
            game_planet *b = &v->contents.planets[i];
            if (approaching(b->position, b->radius + 500, dt)) {
                char s[25];
                snprintf(s, sizeof(s), "Too close to %.11s", v->systems[v->system].name);
                drop(s); break;
            }
        }
    }
    if (p->cruising) {
        p->firing = false;
        p->cruise_charge = fminf(1, p->cruise_charge + dt / 4);
        p->cruise_factor = 1 + 24 * smooth(p->cruise_charge);
    } else if (cg->cruise.bleed > 0) {
        cg->cruise.bleed = fmaxf(0, cg->cruise.bleed - dt);
        p->cruise_factor = 1 + (cg->cruise.bleed_factor - 1) * smooth(cg->cruise.bleed / 1.5f);
    }
    if (!v->interdiction.active) return;
    int who = v->interdiction.who;
    if (!p->cruising || !pursuer(who) || v->contacts[who].id != cg->cruise.who_id) {
        v->interdiction.active = false; return;
    }
    cg->cruise.elapsed += dt;
    float t = cg->cruise.elapsed;
    l3_vec axis = cg->cruise.axis;
    l3_vec right = l3_norm(l3_cross(axis, fabsf(axis.y) < .9f ? l3v(0,1,0) : l3v(1,0,0)));
    l3_vec up = l3_cross(right, axis);
    v->interdiction.escape = l3_norm(l3_add(axis, l3_add(l3_scale(right, .9f * sinf(t * .65f)),
        l3_scale(up, .7f * sinf(t * .43f)))));
    float alignment = l3_dot(cg_forward(), v->interdiction.escape);
    v->interdiction.tug = cg_clamp(v->interdiction.tug + dt * (alignment >= .97f ? .28f : -.22f), -1, 1);
    v->interdiction.progress = fminf(1, t / 12);
    if (v->interdiction.tug >= 1 || (t >= 12 && v->interdiction.tug >= 0)) {
        v->interdiction.active = false; v->interdiction.progress = 1;
        cg_depart(who);
        cg_message("Interdiction escaped. CRUISE maintained.");
    } else if (v->interdiction.tug <= -1 || t >= 12) {
        v->interdiction.progress = 1; caught(true);
    }
}
