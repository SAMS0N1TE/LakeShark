/* Inertial flight, AI steering and a docking autopilot with slot roll matching.
 */
#include "internal.h"
static l3_quat aim(l3_vec d) {
    return l3_quat_euler(-atan2f(d.x, -d.z) / CG_RAD, atan2f(d.y, hypotf(d.x, d.z)) / CG_RAD, 0);
}
void cg_flight(float dt) {
    game_view *v = &cg->v;
    game_player *p = &v->player;
    cg_combat_player_ensure(p);
    float station_distance = cg_len(l3_sub(p->position, v->contents.station));
    cg_cruise_mass_locked();
    float cruise_before = fmaxf(1, p->cruise_factor);
    bool stalled = cg->cruise.stall > 0;
    cg_cruise_update(dt);
    for (int i = 0; i < v->contents.planet_count; i++) {
        game_planet *b = &v->contents.planets[i];
        float d = cg_len(l3_sub(p->position, b->position));
        if (!p->docked && !p->dead && d < b->radius + 15) {
            l3_vec out = l3_norm(l3_sub(p->position, b->position));
            float impact = -l3_dot(p->velocity, out);     /* speed into the ground */
            p->position = l3_add(b->position, l3_scale(out, b->radius + 16));
            if (impact > 22) {
                p->hull = 0;                               /* a world does not forgive */
                cg_message("Impact with the surface.");
            } else {
                p->velocity = l3_scale(out, fmaxf(4, impact * .5f));
                p->hull = fmaxf(0, p->hull - 20 * dt - impact * 1.5f);
            }
        }
    }
    if (!p->docked && !p->dead && v->jump == JUMP_IDLE) {
        if (p->docking) {
            cg->dock_time += dt;
            l3_vec approach = l3_add(v->contents.station, l3v(0, 0, -150));
            if (fabsf(p->position.x) < 7 && fabsf(p->position.y) < 7 && p->position.z < -40)
                cg->dock_slot = true;
            bool aligned = cg->dock_slot;
            l3_vec target = aligned ? v->contents.station : approach;
            l3_vec delta = l3_sub(target, p->position);
            float distance = cg_len(delta);
            l3_quat desired = aligned ? l3_quat_euler(180, 0, v->station_angle) : aim(delta);
            /* An unhurried computer: it turns, gathers speed and eases into the slot
               rather than snapping to its top speed. */
            p->attitude = l3_quat_slerp(p->attitude, desired, fminf(1, dt * 1.2f));
            l3_vec want = l3_scale(l3_norm(delta), fminf(aligned ? 14 : 38, fmaxf(3, distance * 0.35f)));
            p->velocity = l3_add(p->velocity, l3_scale(l3_sub(want, p->velocity), fminf(1, dt * 0.9f)));
            p->position = l3_add(p->position, l3_scale(p->velocity, dt));
            if (station_distance < 58 && aligned) {
                p->docked = true;
                p->docking = false;
                p->throttle = 0;
                p->velocity = l3v(0, 0, 0);
                cg_message("Docked. Commander saved.");
                cg_mission_dock();
                cg_combat_dock();
                game_save();
                cg_market();
            }
        } else {
            float turn = game_player_ship()->turn * CG_RAD;
            turn *= (1+(p->pips[1]-2)*cg->rules.combat.distributor[CD_TURN])*cg_drive_scale(p);
            if(p->boost_left>0) turn *= cg->rules.combat.defence[CF_BOOST_SPEED];
            if (p->cruising) turn *= .4f;
            p->attitude = l3_quat_spin(
                p->attitude,
                l3_quat_rotate(p->attitude, l3v(p->pitch * turn, p->yaw * turn, -p->roll * turn)),
                dt);
            float top = game_player_ship()->speed;
            top *= 1+(p->pips[1]-2)*cg->rules.combat.distributor[CD_SPEED];
            if(p->boost_left>0 && p->throttle>=0) top *= cg->rules.combat.defence[CF_BOOST_SPEED];
            top *= cg_drive_scale(p);
            float throttle = stalled || cg->cruise.stall > 0 ? 0 : p->throttle;
            l3_vec desired = l3_scale(cg_forward(), throttle * top * (throttle<0?1:p->cruise_factor));
            if (!p->cruising && !stalled)     /* side and up thrusters: half the drive's top speed */
                desired = l3_add(desired, l3_add(l3_scale(l3_quat_rotate(p->attitude, l3v(1, 0, 0)), cg->thrust_x * top * .5f),
                                                 l3_scale(l3_quat_rotate(p->attitude, l3v(0, 1, 0)), cg->thrust_y * top * .5f)));
            /* Drive acceleration follows the four-second spool; ordinary inertia
             * still smooths throttle and steering. Exit braking caps every step. */
            if (p->cruising && p->cruise_factor > cruise_before)
                p->velocity = l3_add(p->velocity,
                    l3_scale(cg_forward(), throttle * top * (p->cruise_factor - cruise_before)));
            p->velocity =
                l3_add(p->velocity, l3_scale(l3_sub(desired, p->velocity), fminf(1, dt * 1.5f * cg_drive_scale(p))));
            if (p->cruising || cruise_before > 1) {
                float speed = cg_len(p->velocity), cap = top * p->cruise_factor;
                if (speed > cap) p->velocity = l3_scale(p->velocity, cap / speed);
            }
            p->position = l3_add(p->position, l3_scale(p->velocity, dt));
            if (station_distance < 48) {
                l3_vec out = l3_norm(l3_sub(p->position, v->contents.station));
                float impact = -l3_dot(p->velocity, out);
                p->position = l3_add(v->contents.station, l3_scale(out, 49));
                p->velocity = l3_add(p->velocity, l3_scale(out, fmaxf(0, impact) * 1.3f + 3));
                p->hull = fmaxf(0, p->hull - dt * 15 - fmaxf(0, impact - 8) * 2.5f);
                if (impact > 45) cg_message("Rammed the station.");
            }
            /* into the star itself */
            if (cg_len(l3_sub(p->position, v->contents.star)) < v->contents.star_radius * 1.02f) {
                p->hull = 0;
                cg_message("Consumed by the star.");
            }
            /* rocks: a glancing knock, or worse at speed */
            for (int i = 0; i < 18; i++) {
                game_asteroid *a = &v->asteroids[i];
                if (!a->active) continue;
                l3_vec off = l3_sub(p->position, a->position);
                float dd = cg_len(off), r = a->radius * 1.2f + 4;
                if (dd < r && dd > .01f) {
                    l3_vec out = l3_scale(off, 1 / dd);
                    float impact = -l3_dot(p->velocity, out);
                    p->position = l3_add(a->position, l3_scale(out, r));
                    p->velocity = l3_add(p->velocity, l3_scale(out, fmaxf(0, impact) * 1.5f));
                    p->hull = fmaxf(0, p->hull - fmaxf(0, impact - 5) * 1.2f);
                }
            }
        }
    }
    if (p->docked) {
        p->position = l3_add(v->contents.station, l3v(0, 0, -95));
        p->attitude = l3_quat_euler(180, 0, v->station_angle);
        p->velocity = l3v(0, 0, 0);
    }
    p->speed = cg_len(p->velocity);
    p->reversing = l3_dot(p->velocity,cg_forward()) < -.001f;
    cg->dust_distance += p->speed * dt;
    /* Leaders move first; followers resolve offsets after the leader's step. */
    for (int pass=0;pass<2;pass++)
    for (int i = 0; i < GAME_CONTACTS; i++) {
        if ((cg->encounter[i].leader!=0) != (pass==1)) continue;
        game_contact *c = &v->contacts[i];
        if (!c->active || cg->mapped[i] || (i<16 && !cg->npc_control[i]))
            continue;
        if(!cg->fighters[i].initialized || cg->fighters[i].id!=c->id) cg_combat_contact_init(i);
        if(cg->fighters[i].modules[MODULE_DRIVES]<=0) {
            c->position=l3_add(c->position,l3_scale(c->velocity,dt));
            c->speed=cg_len(c->velocity);continue;
        }
        if (cg_encounter_flight(i,dt)) continue;
        if (c->role == ROLE_DERELICT || c->role == ROLE_BEACON) {
            c->velocity = l3v(0, 0, 0);
            c->speed = 0;
            continue;
        }
        l3_vec target = cg->anchor[i];
        float health = c->hull / game_ship(c->ship_class)->hull;
        if (health < .25f)
            target = l3_add(c->position, l3_scale(l3_sub(c->position, p->position), 3));
        else if (cg_encounter_enemy(i) != -2) {
            int enemy=cg_encounter_enemy(i);
            target=enemy<0 ? p->position : v->contacts[enemy].position;
        } else if (c->role == ROLE_POLICE) {
            target=l3_add(v->contents.station,l3v(cg->lane_out[i]?180:-180,40,-180));
            if(cg_len(l3_sub(c->position,target))<30) cg->lane_out[i]=!cg->lane_out[i];
        } else if (c->role == ROLE_TRADER) {
            target=cg->lane_out[i] ? v->contents.planets[0].position : v->contents.station;
            if(!cg->lane_out[i] && cg_len(l3_sub(c->position,v->contents.station))<180)
                target=l3_add(v->contents.station,l3v(0,0,-70));
            if(!cg->lane_out[i] && cg_len(l3_sub(c->position,target))<15) {
                c->active=false;cg->dead_until[i]=v->clock+30;continue;
            }
            if(cg->lane_out[i] && cg_len(l3_sub(c->position,target))<180) cg->lane_out[i]=false;
        }
        l3_vec delta = l3_sub(target, c->position);
        c->attitude = l3_quat_slerp(c->attitude, aim(delta),
                                    fminf(1, dt * game_ship(c->ship_class)->turn / 45));
        c->speed = game_ship(c->ship_class)->speed * .35f;
        if (c->role == ROLE_RESCUE)
            c->speed *= .1f;
        if (cg_len(delta) < 120 && (c->hostile || c->role == ROLE_POLICE))
            c->speed *= .15f;
        c->velocity = l3_scale(l3_quat_rotate(c->attitude, l3v(0, 0, -1)), c->speed);
        c->position = l3_add(c->position, l3_scale(c->velocity, dt));
    }
    if (p->hull <= 0 && !p->dead) {
        p->dead = true;
        p->firing = false;
        cg_message("Commander lost. Reload saved commander to resume.");
        cg_explode(p->position, -1, false);
    }
}
