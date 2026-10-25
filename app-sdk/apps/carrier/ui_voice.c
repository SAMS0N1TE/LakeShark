/* The ship's computer speaks: what changed, said once, never chattering.
 * Everything here watches the game's state for edges; nothing drives it. */
#include "internal.h"
#include "art.h"
#include "ui.h"

static struct {
    bool docked, docking, dead, talk, low_hull, shields_down, launched, discoveries_seen, nebula, cruising,
        interdiction, hot;
    uint32_t discoveries;
    int count, jump, system, missiles;
    uint32_t last_ms, pull_ms;
} was;

static void say(const char *s, bool now) {
    if (!ui.voice) return;
    uint32_t t = lap_millis();
    if (!now && t - was.last_ms < 1400) return;   /* one thing at a time */
    if (now) lap_say_stop();
    lap_say(s);
    was.last_ms = t;
}

void ui_voice_reset(void) {
    const game_view *v = game_state();
    memset(&was, 0, sizeof was);
    if (!v) return;
    was.docked = v->player.docked;
    was.system = v->system;
    was.count = -1;
}

void ui_voice_update(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    char line[64];
    /* hyperspace: the count down is the drive's (ui_fsd_voice); here, where you came out */
    if (v->system != was.system && v->jump == JUMP_IDLE) {
        snprintf(line, sizeof line, "Arriving at %s", v->systems[v->system].name);
        say(line, true);
        was.system = v->system;
    }
    was.jump = v->jump;
    if (was.docked && !p->docked) say("Launch clearance granted", true);
    if (!was.docked && p->docked) say("Docked. Commander saved", true);
    if (p->docking && !was.docking) say("Docking computer engaged", false);
    was.docked = p->docked;
    was.docking = p->docking;
    /* danger */
    int enemy = 0;
    for (int i = 0; i < CG_MISSILES; i++) enemy += cg->missiles[i].life > 0 && !cg->missiles[i].player;
    if (enemy > was.missiles) say("Missile incoming", true);
    was.missiles = enemy;
    bool down = p->shield_fore < 1 && p->shield_aft < 1;
    if (down && !was.shields_down && !p->docked) say("Shields down", false);
    was.shields_down = down;
    const game_ship_stats *me = game_player_ship();
    bool low = me && p->hull < me->hull * .25f && !p->dead;
    if (low && !was.low_hull) say("Hull critical", true);
    was.low_hull = low;
    if (p->dead && !was.dead) say("Ship destroyed", true);
    was.dead = p->dead;
    /* the ground coming up */
    float near = 1e9f;
    for (int i = 0; i < v->contents.planet_count; i++)
        near = fminf(near, cg_len(l3_sub(p->position, v->contents.planets[i].position)) - v->contents.planets[i].radius);
    if (!p->docked && near < p->speed * 6 && near < 600 && lap_millis() - was.pull_ms > 4000) {
        say("Pull up", true);
        was.pull_ms = lap_millis();
    }
    if (v->discoveries.count > was.discoveries && was.discoveries_seen) say("New discovery logged", false);
    was.discoveries = v->discoveries.count;
    was.discoveries_seen = true;
    if (v->in_nebula && !was.nebula) say("Entering the cloud. Sensors reduced", false);
    was.nebula = v->in_nebula;
    bool hot = p->cabin_heat > .8f;
    if (hot && !was.hot) say("Heat critical. Pull away from the star", true);
    was.hot = hot;
    if (p->cruising && !was.cruising) say("Cruise engaged", true);
    if (!p->cruising && was.cruising) say(p->cruise_reason && p->cruise_reason[0] ? p->cruise_reason : "Cruise off", true);
    was.cruising = p->cruising;
    if (game_state()->interdiction.active && !was.interdiction) say("Interdiction. Hold the escape marker", true);
    was.interdiction = game_state()->interdiction.active;
    uint32_t ev = v->combat_events;
    if (ev) {
        if (ev & COMBAT_CANOPY) say("Canopy breached. Oxygen reserves engaged", true);
        else if (ev & COMBAT_HULL_10) say("Hull at ten percent", true);
        else if (ev & COMBAT_HULL_25) say("Hull at twenty five percent", false);
        else if (ev & COMBAT_HULL_50) say("Hull at fifty percent", false);
        if (ev & COMBAT_SHIELD_UP) say("Shields online", false);
        game_combat_ack(ev);
    }
    /* a hailed ship answers out loud */
    if (v->talk.open && !was.talk && v->talk.line_count > 0) say(v->talk.lines[0], true);
    was.talk = v->talk.open;
}
