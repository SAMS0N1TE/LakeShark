/* CARRIER's sounds: short tones on the board's one tone queue (it plays notes in turn,
 * it does not mix), so every effect is brief and has a priority - an explosion or an
 * alarm cuts in, a laser blip waits its turn or is skipped. Quiet by default: the
 * board's own volume (kept at 30) does the rest. Effects come from edges in the game's
 * state, the same way the voice does. */
#include "internal.h"
#include "art.h"
#include "ui.h"

typedef struct { int16_t hz, ms; uint8_t vol, wave; } note;
enum { P_TICK, P_SHOT, P_HIT, P_EVENT, P_ALARM, P_BOOM };

static int playing = -1;            /* the priority of what is queued now */
static uint32_t until_ms;           /* when it ends */

static void play(int prio, const note *n, int count) {
    if (!ui.sound) return;
    uint32_t now = lap_millis();
    bool busy = lap_tone_busy() && now < until_ms;
    if (busy && prio < playing) return;          /* lesser sounds wait their turn: skip */
    if (busy && prio > playing) lap_tone_stop();  /* greater ones cut in */
    else if (busy) return;                         /* equal: let it finish */
    uint32_t len = 0;
    for (int i = 0; i < count; i++) {
        lap_tone(n[i].hz, n[i].ms, n[i].vol, n[i].wave);
        len += (uint32_t)n[i].ms;
    }
    playing = prio;
    until_ms = now + len;
}

/* the library: one table per effect */
#define W_SIN LAP_WAVE_SINE
#define W_SQR LAP_WAVE_SQUARE
#define W_TRI LAP_WAVE_TRIANGLE
#define W_SAW LAP_WAVE_SAW
#define W_NSE LAP_WAVE_NOISE
static const note s_pulse[] = {{1500, 18, 11, W_SQR}, {1050, 22, 9, W_SQR}, {700, 18, 6, W_SQR}};
static const note s_beam[] = {{190, 45, 8, W_SAW}, {205, 45, 8, W_SAW}};
static const note s_rattle[] = {{0, 6, 0, W_NSE}, {140, 22, 14, W_NSE}};
static const note s_slug_charge[] = {{300, 120, 7, W_TRI}, {450, 120, 8, W_TRI}, {650, 120, 9, W_TRI},
                                     {900, 120, 10, W_TRI}, {1250, 120, 11, W_TRI}};
static const note s_slug_fire[] = {{90, 30, 22, W_NSE}, {1800, 20, 12, W_SQR}, {60, 120, 16, W_SIN}};
static const note s_missile[] = {{0, 10, 0, W_NSE}, {400, 60, 12, W_NSE}, {300, 80, 10, W_SAW}, {220, 120, 8, W_SAW}};
static const note s_boom[] = {{60, 70, 26, W_SIN}, {90, 220, 24, W_NSE}, {50, 160, 18, W_SIN}, {70, 180, 12, W_NSE}};
static const note s_boom_far[] = {{70, 160, 12, W_NSE}, {50, 120, 9, W_SIN}};
static const note s_shield_hit[] = {{700, 30, 12, W_TRI}, {420, 40, 10, W_TRI}, {260, 40, 7, W_TRI}};
static const note s_hull_hit[] = {{110, 40, 18, W_NSE}, {80, 60, 16, W_SQR}};
static const note s_shields_down[] = {{880, 80, 12, W_SQR}, {660, 80, 12, W_SQR}, {440, 160, 12, W_SQR}};
static const note s_alarm[] = {{720, 140, 11, W_SQR}, {540, 140, 11, W_SQR}};
static const note s_missile_warn[] = {{1250, 50, 11, W_SQR}, {0, 40, 0, W_SIN}, {1250, 50, 11, W_SQR}};
static const note s_lock[] = {{1600, 40, 8, W_SIN}};
static const note s_locked[] = {{1600, 220, 9, W_SIN}};
static const note s_boost[] = {{180, 60, 12, W_SAW}, {300, 60, 12, W_SAW}, {460, 80, 10, W_SAW}, {600, 120, 7, W_SAW}};
static const note s_cruise_on[] = {{200, 120, 9, W_SIN}, {300, 120, 10, W_SIN}, {450, 140, 10, W_SIN}, {680, 220, 9, W_SIN}};
static const note s_cruise_off[] = {{600, 120, 10, W_SIN}, {400, 120, 9, W_SIN}, {250, 200, 8, W_SIN}};
static const note s_launch[] = {{70, 300, 16, W_NSE}, {90, 300, 14, W_SAW}, {140, 200, 10, W_SAW}};
static const note s_docked[] = {{523, 120, 10, W_TRI}, {659, 120, 10, W_TRI}, {784, 260, 10, W_TRI}};
static const note s_jump_charge[] = {{110, 400, 10, W_SAW}, {160, 400, 11, W_SAW}, {230, 400, 12, W_SAW},
                                     {330, 400, 12, W_SAW}, {470, 400, 13, W_SAW}};
static const note s_jump_go[] = {{60, 120, 22, W_NSE}, {1200, 60, 12, W_SIN}, {900, 60, 12, W_SIN}, {600, 300, 10, W_SIN}};
static const note s_discovery[] = {{659, 90, 10, W_TRI}, {784, 90, 10, W_TRI}, {988, 90, 10, W_TRI}, {1319, 260, 9, W_TRI}};
static const note s_coins[] = {{1319, 50, 9, W_SQR}, {1760, 120, 9, W_SQR}};
static const note s_comms[] = {{988, 50, 8, W_SIN}, {0, 30, 0, W_SIN}, {1319, 70, 8, W_SIN}};
static const note s_scoop[] = {{800, 40, 8, W_TRI}, {1100, 60, 8, W_TRI}};
static const note s_cell[] = {{300, 60, 10, W_TRI}, {500, 60, 10, W_TRI}, {800, 60, 10, W_TRI}, {1200, 140, 9, W_TRI}};
static const note s_chaff[] = {{2000, 30, 10, W_NSE}, {1500, 30, 9, W_NSE}, {1000, 40, 8, W_NSE}, {600, 60, 6, W_NSE}};
static const note s_sink[] = {{400, 60, 10, W_NSE}, {300, 120, 9, W_NSE}, {200, 200, 7, W_NSE}};
static const note s_ecm[] = {{1800, 40, 9, W_SQR}, {900, 40, 9, W_SQR}, {1800, 40, 9, W_SQR}, {900, 40, 9, W_SQR}};
static const note s_oxygen[] = {{1000, 70, 10, W_SIN}, {0, 60, 0, W_SIN}, {1000, 70, 10, W_SIN}};
static const note s_heat[] = {{500, 90, 9, W_SQR}, {400, 90, 9, W_SQR}};
static const note s_shields_up[] = {{440, 80, 10, W_TRI}, {660, 80, 10, W_TRI}, {880, 160, 10, W_TRI}};
static const note s_interdict[] = {{300, 200, 14, W_SAW}, {250, 200, 14, W_SAW}, {300, 200, 14, W_SAW}};
#define PLAY(prio, s) play(prio, s, (int)(sizeof s / sizeof s[0]))

static struct {
    int beams, missiles_mine, missiles_enemy, kills, cans, selected, docked, cruising, jump, discoveries, msg, rattles;
    float charge[GAME_MOUNTS], lock;
    uint32_t beam_ms, oxy_ms, heat_ms;
    bool locked, shields_back;
    uint32_t credits, alarm_ms, warn_ms;
    float fore, aft, hull;
    bool shields_down, init;
} was;

void ui_sound_update(void) {
    const game_view *v = game_state();
    const game_player *p = &v->player;
    if (!was.init) {
        memset(&was, 0, sizeof was);
        was.init = true;
        was.docked = p->docked; was.cruising = p->cruising; was.jump = v->jump; was.kills = (int)p->kills;
        was.credits = p->credits; was.discoveries = (int)v->discoveries.count; was.selected = v->selected;
        was.fore = p->shield_fore; was.aft = p->shield_aft; was.hull = p->hull; was.msg = v->message_count;
        return;
    }
    /* your guns */
    int beams = 0;
    for (int i = 0; i < CG_BEAMS; i++) beams += cg->beams[i].life > 0 && cg->beams[i].player;
    /* each weapon its own voice */
    bool pulse = false, beam = false;
    for (int m = 0; m < GAME_MOUNTS; m++) {
        const game_mount *w = &p->weapons[m];
        if (w->kind == WEAPON_BEAM && w->firing) beam = true;
        if (w->kind == WEAPON_PULSE) pulse = true;
        if (w->kind == WEAPON_SLUG) {
            if (w->charge > .05f && was.charge[m] <= .05f) PLAY(P_SHOT, s_slug_charge);
            if (w->charge < .05f && was.charge[m] > .8f) PLAY(P_EVENT, s_slug_fire);
        }
        was.charge[m] = w->charge;
    }
    if (beams > was.beams && (pulse || !beam)) PLAY(P_SHOT, s_pulse);
    was.beams = beams;
    if (beam && lap_millis() - was.beam_ms > 85) { PLAY(P_SHOT, s_beam); was.beam_ms = lap_millis(); }
    int rattles = 0;
    for (int i = 0; i < GAME_PROJECTILES; i++)
        rattles += v->projectiles[i].life > 0 && v->projectiles[i].player && v->projectiles[i].kind == WEAPON_RATTLE;
    if (rattles > was.rattles) PLAY(P_SHOT, s_rattle);
    was.rattles = rattles;
    /* a seeker finding its mark: ticks, then the steady tone */
    if (p->seeker_lock > 0 && p->seeker_lock < 1 && (int)(p->seeker_lock * 6) != (int)(was.lock * 6)) PLAY(P_TICK, s_lock);
    if (p->seeker_lock >= 1 && !was.locked) PLAY(P_EVENT, s_locked);
    was.locked = p->seeker_lock >= 1;
    was.lock = p->seeker_lock;
    /* air and heat */
    if (p->canopy <= 0 && p->oxygen > 0 && p->oxygen < 60 && lap_millis() - was.oxy_ms > 1500) {
        PLAY(P_ALARM, s_oxygen);
        was.oxy_ms = lap_millis();
    }
    if (p->heat_level > 1 && lap_millis() - was.heat_ms > 1200) { PLAY(P_ALARM, s_heat); was.heat_ms = lap_millis(); }
    bool back = !(p->shield_reboot[0] > 0 || p->shield_reboot[1] > 0);
    if (back && !was.shields_back && was.init) PLAY(P_EVENT, s_shields_up);
    was.shields_back = back;
    int mine = 0, enemy = 0;
    for (int i = 0; i < CG_MISSILES; i++) {
        if (cg->missiles[i].life <= 0) continue;
        if (cg->missiles[i].player) mine++; else enemy++;
    }
    if (mine > was.missiles_mine) PLAY(P_EVENT, s_missile);
    was.missiles_mine = mine;
    /* incoming: a missile on you beeps until it is gone */
    if (enemy > was.missiles_enemy || (enemy && lap_millis() - was.warn_ms > 700)) {
        PLAY(P_ALARM, s_missile_warn);
        was.warn_ms = lap_millis();
    }
    was.missiles_enemy = enemy;
    /* kills: the bang, and the bounty's coins after */
    if ((int)p->kills > was.kills) { PLAY(P_BOOM, s_boom); }
    was.kills = (int)p->kills;
    /* being hit */
    bool shield_hit = p->shield_fore < was.fore - .3f || p->shield_aft < was.aft - .3f;
    bool hull_hit = p->hull < was.hull - .3f;
    if (hull_hit) PLAY(P_HIT, s_hull_hit);
    else if (shield_hit) PLAY(P_HIT, s_shield_hit);
    bool down = p->shield_fore < 1 && p->shield_aft < 1 && !p->docked;
    if (down && !was.shields_down) PLAY(P_ALARM, s_shields_down);
    was.shields_down = down;
    was.fore = p->shield_fore; was.aft = p->shield_aft; was.hull = p->hull;
    /* low hull: the two-tone, every two seconds */
    const game_ship_stats *me = game_player_ship();
    if (me && p->hull < me->hull * .25f && !p->dead && !p->docked && lap_millis() - was.alarm_ms > 2000) {
        PLAY(P_ALARM, s_alarm);
        was.alarm_ms = lap_millis();
    }
    if (p->dead && !was.hull) {}
    /* travel */
    if (was.docked && !p->docked) PLAY(P_EVENT, s_launch);
    if (!was.docked && p->docked) PLAY(P_EVENT, s_docked);
    was.docked = p->docked;
    if (p->cruising && !was.cruising) PLAY(P_EVENT, s_cruise_on);
    if (!p->cruising && was.cruising) PLAY(P_EVENT, s_cruise_off);
    was.cruising = p->cruising;
    if (v->jump == JUMP_CHARGE && was.jump != JUMP_CHARGE) PLAY(P_EVENT, s_jump_charge);
    if (v->jump == JUMP_TUNNEL && was.jump != JUMP_TUNNEL) PLAY(P_BOOM, s_jump_go);
    was.jump = v->jump;
    if (v->interdiction.active && lap_millis() - was.warn_ms > 900) { PLAY(P_ALARM, s_interdict); was.warn_ms = lap_millis(); }
    /* rewards and small things */
    if ((int)v->discoveries.count > was.discoveries) PLAY(P_EVENT, s_discovery);
    was.discoveries = (int)v->discoveries.count;
    if (p->credits > was.credits + 50 && !p->docked) PLAY(P_TICK, s_coins);
    was.credits = p->credits;
    int cans = 0;
    for (int i = 0; i < GAME_GOODS; i++) cans += p->cargo[i];
    if (cans > was.cans && !p->docked) PLAY(P_TICK, s_scoop);
    was.cans = cans;
    if (v->selected != was.selected && v->selected >= 0) PLAY(P_TICK, s_lock);
    was.selected = v->selected;
    if (v->message_count != was.msg || (v->message_count == GAME_MESSAGES && ui.msg_ms > lap_millis() - 60)) {
        if (v->talk.open) PLAY(P_TICK, s_comms);
    }
    was.msg = v->message_count;
    (void)s_beam; (void)s_rattle; (void)s_slug_charge; (void)s_slug_fire; (void)s_boom_far; (void)s_locked;
    (void)s_boost;
}

void ui_sound_reset(void) { was.init = false; playing = -1; until_ms = 0; }

/* For the screen's own moments: a far explosion (someone else's), a boost. */
void ui_sound_far_boom(void) { PLAY(P_HIT, s_boom_far); }
void ui_sound_boost(void) { PLAY(P_EVENT, s_boost); }
void ui_sound_kit(int which) {
    if (which == 0) PLAY(P_EVENT, s_cell);
    else if (which == 1) PLAY(P_EVENT, s_chaff);
    else if (which == 2) PLAY(P_EVENT, s_sink);
    else PLAY(P_EVENT, s_ecm);
}
