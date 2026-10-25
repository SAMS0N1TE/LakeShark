/* Systems coordinator; all mutable game memory is one allocation in PSRAM. */
#include "internal.h"
cg_state *cg;
const game_view *game_state(void) { return cg ? &cg->v : NULL; }
void cg_message(const char *text) {
    for (int i = GAME_MESSAGES - 1; i; i--)
        memcpy(cg->v.messages[i], cg->v.messages[i - 1], 80);
    snprintf(cg->v.messages[0], 80, "%s", text);
    if (cg->v.message_count < GAME_MESSAGES)
        cg->v.message_count++;
    cg->v.revision++;
}
bool game_init(void) {
    if (cg)
        return true;
    cg = calloc(1, sizeof(*cg));
    if (!cg)
        return false;
    cg->v.config_valid = cg_config();
    cg->v.config_valid = cg_system_config() && cg->v.config_valid;
    cg->v.config_valid = cg_encounter_config() && cg->v.config_valid;
    cg->v.encounter_level = 1;
    cg->encounter_system = -1;
    cg->last_chatter = -1;
    cg->last_tick_ms = lap_millis();
    lap_ble_scan(true);
    lap_gps_t gps;
    bool fix = lap_gps(&gps) && gps.fix;
    cg->v.galaxy_seed = game_gps_seed(fix, fix ? (float)gps.lat : 0, fix ? (float)gps.lon : 0);
    cg->rng = cg->v.galaxy_seed;
    cg->next_id = 0x1000000;
    game_universe(cg->v.galaxy_seed, cg->v.systems);
    game_player *p = &cg->v.player;
    p->attitude = (l3_quat){0, 0, 0, 1};
    p->credits = 10000;
    p->fuel = 7;
    p->hull = game_ship(SH_COBRA)->hull;
    p->shield_fore = p->shield_aft = p->energy = 100;
    p->equipment = (1u << EQ_LASER) | (1u << EQ_DOCKING);
    p->missiles = 3;
    p->docked = true;
    cg_combat_player_init(p);
    cg->v.selected = -1;
    cg->v.plotted = -1;
    cg_arrive(0);
    p->docked = true;
    game_load();
    cg_combat_reset();
    cg_cruise_reset();
    cg->poll = 2;
    cg_traffic(0);
    cg_contacts();
    cg_actions();
    cg_message(cg->v.config_valid ? "CARRIER ready. Launch when ready."
                                  : "Rules file updated for this version.");
    /* Prime both bounded file caches at entry; docking never grows a buffer. */
    game_save();
    game_save();
    return true;
}
void game_shutdown(void) {
    if (cg) {
        game_save();
        lap_ble_scan(false);
        free(cg);
        cg = NULL;
    }
}
void game_stick(float p, float r) {
    if (cg) {
        cg->v.player.pitch = isfinite(p) ? cg_clamp(p, -1, 1) : 0;
        cg->v.player.roll = isfinite(r) ? cg_clamp(r, -1, 1) : 0;
    }
}
void game_yaw(float y) {
    if (cg)
        cg->v.player.yaw = isfinite(y) ? cg_clamp(y, -1, 1) : 0;
}
void game_thrust(float x, float y) {
    if (cg) {
        cg->thrust_x = isfinite(x) ? cg_clamp(x, -1, 1) : 0;
        cg->thrust_y = isfinite(y) ? cg_clamp(y, -1, 1) : 0;
    }
}
void game_throttle(float t) {
    if (cg && isfinite(t))
        cg->v.player.throttle = cg_clamp(t, -cg->rules.reverse, 1);
}
float game_reverse_max(void) { return cg ? cg->rules.reverse : .35f; }
void game_aim_assist(int level) { if(cg) cg->aim_assist=level<0?0:level>2?2:level; }
void game_fire(bool on) {
    if (cg) {
        cg->v.player.firing = on;
        if(on) cg_weapons_hot();
    }
}
void game_panel_open(game_panel p) {
    if (cg && p >= PANEL_NONE && p <= PANEL_LOG)
        cg->v.panel = p;
}
void game_motion_steer(bool on) {
    if (cg)
        cg->v.player.motion_steer = on;
}
void game_target_next(void) {
    if (!cg)
        return;
    cg_combat_player_ensure(&cg->v.player);
    if(cg->v.player.modules[MODULE_SENSORS]<=0) {cg->v.selected=-1;return;}
    for (int i = 1; i <= GAME_CONTACTS; i++) {
        int j = (cg->v.selected + i + GAME_CONTACTS) % GAME_CONTACTS;
        if (cg->v.contacts[j].active) {
            cg->v.selected = j;
            return;
        }
    }
    cg->v.selected = -1;
}
void game_target_nearest(bool hostile) {
    if (!cg)
        return;
    cg_combat_player_ensure(&cg->v.player);
    if(cg->v.player.modules[MODULE_SENSORS]<=0) {cg->v.selected=-1;return;}
    float best = 1e20f;
    cg->v.selected = -1;
    for (int i = 0; i < GAME_CONTACTS; i++) {
        const game_contact *c = &cg->v.contacts[i];
        if (c->active && (!hostile || c->hostile) && c->range < best) {
            best = c->range;
            cg->v.selected = i;
        }
    }
}
/* The contact closest to the nose, within a 30 degree cone; keeps the old choice if none. */
void game_target_ahead(void) {
    if (!cg)
        return;
    cg_combat_player_ensure(&cg->v.player);
    if(cg->v.player.modules[MODULE_SENSORS]<=0) {cg->v.selected=-1;return;}
    const l3_vec f = l3_quat_rotate(cg->v.player.attitude, l3v(0, 0, -1));
    float best = cosf(30 * CG_RAD);
    for (int i = 0; i < GAME_CONTACTS; i++) {
        const game_contact *c = &cg->v.contacts[i];
        if (!c->active) continue;
        l3_vec d = l3_sub(c->position, cg->v.player.position);
        float len = cg_len(d), a = len > 1e-3f ? l3_dot(d, f) / len : -1;
        if (a > best) { best = a; cg->v.selected = i; }
    }
}
void cg_actions(void) {
    game_view *v = &cg->v;
    game_player *p = &v->player;
    v->action = ACT_NONE;
    v->action_label = "---";
    if (p->dead)
        return;
    if (p->docked) {
        v->action = ACT_LAUNCH;
        v->action_label = "LAUNCH";
        return;
    }
    for (int i = 0; i < CG_CANS; i++)
        if (cg->cans[i].life > 0 && cg_len(l3_sub(cg->cans[i].p, p->position)) < 28) {
            v->action = ACT_SCOOP;
            v->action_label = "SCOOP";
            return;
        }
    if (v->selected >= 0 && v->contacts[v->selected].active &&
        v->contacts[v->selected].role == ROLE_BEACON && v->contacts[v->selected].range < 80) {
        v->action = ACT_HAIL; v->action_label = "REFUEL"; return;
    }
    if (v->plotted >= 0 && !p->mass_locked) {
        v->action = ACT_JUMP;
        v->action_label = "JUMP";
        return;
    }
    if (cg_len(l3_sub(p->position, v->contents.station)) < 850) {
        v->action = ACT_DOCK;
        v->action_label = p->docking ? "CANCEL DOCK" : "DOCK";
        return;
    }
    if (v->selected >= 0 && v->contacts[v->selected].active &&
        v->contacts[v->selected].range < 700) {
        v->action = v->contacts[v->selected].role == ROLE_RESCUE && v->contacts[v->selected].range < 80 ? ACT_RESCUE : ACT_HAIL;
        v->action_label = v->action == ACT_RESCUE ? "SUPPLIES" : "HAIL";
    }
}
void cg_launch(void) {
    game_player *p = &cg->v.player;
    p->docked = p->docking = false;
    p->position = l3_add(cg->v.contents.station, l3v(0, 0, -95));
    p->attitude = (l3_quat){0, 0, 0, 1};
    p->velocity = l3v(0, 0, -8);
    p->throttle = 0.25f;
    cg_message("Launch clearance granted.");
}
void game_context(void) {
    if (!cg)
        return;
    cg_actions();
    game_view *v = &cg->v;
    game_player *p = &v->player;
    switch (v->action) {
    case ACT_LAUNCH:
        cg_launch();
        break;
    case ACT_DOCK:
        if (!(p->equipment & (1u << EQ_DOCKING))) {
            cg_message("Docking computer required.");
            break;
        }
        p->docking = !p->docking;
        cg->dock_time = 0;
        cg->dock_slot = false;
        break;
    case ACT_JUMP:
        if(game_drive_refusal()) {cg_message(game_drive_refusal());break;}
        if (v->jump == JUMP_IDLE && game_plot_jump(v->plotted)) {
            if (game_jump_alignment() > GAME_JUMP_ALIGN_DEG) {
                char message[80];
                snprintf(message, sizeof(message), "Align with %s to jump.", v->systems[v->plotted].name);
                cg_message(message);
                break;
            }
            v->jump = JUMP_CHARGE;
            v->jump_progress = 0;
            p->firing = false;
            cg_message("Hyperspace charging.");
        }
        break;
    case ACT_SCOOP:
        for (int i = 0; i < CG_CANS; i++)
            if (cg->cans[i].life > 0 && cg_len(l3_sub(cg->cans[i].p, p->position)) < 28 &&
                game_cargo_used() < game_cargo_capacity()) {
                p->cargo[cg->cans[i].good]++;
                cg->cans[i].life = 0;
                cg_message("Cargo secured.");
                break;
            }
        break;
    case ACT_RESCUE:
        if (v->contacts[v->selected].range < 80 && p->cargo[0]) {
            cg->rescue_target = v->selected;
            cg->rescue = 0;
            cg_message("Transferring supplies. Hold within 80 for five seconds.");
        } else
            cg_message("Need food and range below 80 for supplies.");
        break;
    case ACT_HAIL:
        cg_talk_open(v->selected);
        break;
    default:
        break;
    }
    cg_actions();
}
static void rescue_update(float dt) {
    game_view *v = &cg->v;
    game_player *p = &v->player;
    int i = cg->rescue_target;
    if (i < 0)
        return;
    game_contact *c = &v->contacts[i];
    bool distant=cg_len(l3_sub(c->position,p->position))>=80;
    if (!c->active || c->role != ROLE_RESCUE || p->docked || p->dead || !p->cargo[0] ||
        (distant && cg->rescue>=0)) {
        cg->rescue_target = -1;
        cg->rescue = 0;
        v->rescue_progress = 0;
        return;
    }
    if(distant) return; /* accepted hail: wait for the commander to approach */
    cg->rescue=fmaxf(0,cg->rescue)+dt;
    v->rescue_progress = fminf(1, cg->rescue / 5);
    cg_beam_add(l3_add(p->position, l3_scale(cg_forward(), 5)), c->position, true);
    if (cg->rescue >= 5) {
        p->cargo[0]--;
        cg_award(500);
        c->role = ROLE_TRADER;
        c->hostile = false;
        cg->rescued[i] = true;
        cg_friend(c->id);
        cg_mission_rescue(c->id);
        cg->rescue_target = -1;
        cg_message("Supplies transferred. Rescue award 500 cr.");
    }
}
void game_update(float dt) {
    if (!cg || !isfinite(dt) || dt <= 0)
        return;
    dt = fminf(dt, 0.2f);
    /* tilt steering, when chosen, is read by the screen side against the level it took */
    while (dt > 0) {
        float h = fminf(dt, 0.04f);
        dt -= h;
        cg->v.clock += h;
        cg->v.station_angle = fmodf(cg->v.station_angle + h * 12, 360);
        cg_traffic(h);
        cg_flight(h);
        rescue_update(h);
        cg_combat(h);
        cg_poi_update(h);
        cg_jump(h);
        cg_system_update(h);
        if(cg->v.player.modules[MODULE_SENSORS]<.5f) cg->v.scanner_scrambled=true;
        cg_combat_status();
    }
    cg_contacts();
    cg_aim_observe();
    cg->v.player.reversing=l3_dot(cg->v.player.velocity,cg_forward())<-.001f;
    cg_actions();
    cg->v.revision++;
}
