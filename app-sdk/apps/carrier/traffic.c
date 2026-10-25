/* ADS-B copies become contacts; received trajectories remain separate from AI.
 */
#include "internal.h"
static bool prefix(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static bool military(const char *s) {
    return prefix(s, "RCH") || prefix(s, "REACH") || prefix(s, "DUKE") || prefix(s, "NAVY") ||
           prefix(s, "AF") || prefix(s, "RRR");
}
static bool airline(const char *s) {
    return s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' && s[2] >= 'A' && s[2] <= 'Z' &&
           s[3] >= '0' && s[3] <= '9';
}
bool game_aircraft_hostile(uint32_t id, int government) {
    return cg_hash(id) % 100 < (uint32_t)(28 - cg_clamp((float)government, 0, 7) * 3);
}
game_class game_aircraft_class(const lap_aircraft_t *a) {
    if (!a)
        return SH_ASP;
    if (military(a->callsign) || !strcmp(a->category, "A6"))
        return (cg_hash(a->icao) & 1) ? SH_VIPER : SH_KRAIT;
    if (a->category[0] == 'A')
        switch (a->category[1]) {
        case '1':
            return SH_SIDEWINDER;
        case '2':
            return SH_ADDER;
        case '3':
        case '4':
            return SH_COBRA;
        case '5':
            return (cg_hash(a->icao) & 1) ? SH_ANACONDA : SH_BOA;
        case '7':
            return a->callsign[0] ? SH_MORAY : SH_DRONE;
        default:
            break;
        }
    if (a->category[0] == 'B')
        switch (a->category[1]) {
        case '1':
        case '2':
            return SH_PROBE;
        case '4':
            return SH_WORM;
        case '6':
            return SH_DRONE;
        default:
            break;
        }
    if (airline(a->callsign))
        return SH_PYTHON;
    return a->callsign[0] ? SH_ADDER : SH_ASP;
}
int game_aircraft_faction(const lap_aircraft_t *a) {
    if (!a || !airline(a->callsign))
        return 0;
    return 1 + (int)(cg_hash((uint32_t)a->callsign[0] * 65536u + (uint32_t)a->callsign[1] * 256u +
                             a->callsign[2]) %
                     12);
}
int cg_register(const game_contact *c) {
    if (!c->active) return -1;
    game_view *v = &cg->v;
    int i;
    for (i = 0; i < v->registry_count; i++)
        if (v->registry[i].id == c->id)
            break;
    if (i == GAME_REGISTRY)
        return -1;
    if (i == v->registry_count) {
        v->registry_count++;
        v->registry[i].visits = 1;
    }
    game_registry_entry *e = &v->registry[i];
    e->id = c->id;
    memcpy(e->callsign, c->callsign, 9);
    e->ship_class = c->ship_class;
    e->faction = c->faction;
    e->altitude_ft = c->altitude_ft;
    e->speed_kt = c->aircraft_speed_kt;
    e->live = c->live;
    snprintf(e->name, sizeof(e->name), "%.15s", c->name);
    return i;
}
void cg_spawn(int i, uint32_t id, game_class type, game_role role, l3_vec p) {
    if (i < 0 || i >= GAME_CONTACTS || (cg->v.traffic_mode == 2 && !cg->importing))
        return;
    game_contact *c = &cg->v.contacts[i];
    /* A reused slot must not inherit AI, ordnance, dialogue or scan state. */
    if (cg->rescue_target == i) cg->rescue_target = -1;
    if (cg->v.talk.open && cg->v.talk.who == i) game_talk_close();
    for (int m=0;m<CG_MISSILES;m++) if (cg->missiles[m].target==i) cg->missiles[m].life=0;
    memset(&cg->encounter[i], 0, sizeof(cg->encounter[i]));
    cg->encounter_enemy[i] = -2;
    for(int j=0;j<GAME_CONTACTS;j++) if(cg->encounter_enemy[j]==i) cg->encounter_enemy[j]=-2;
    cg->cooldown[i] = 0;
    cg->dead_until[i] = 0;
    memset(c, 0, sizeof(*c));
    c->id = id;
    cg_name(c);
    cg->mapped[i] = false;
    cg->npc_control[i] = true;
    cg->police_origin[i] = role == ROLE_POLICE;
    cg->lane_out[i] = role == ROLE_TRADER && !(i & 1);
    cg->talk_used[i] = 0;
    c->active = true;
    c->ship_class = type;
    c->role = role;
    c->hostile = role == ROLE_PIRATE && !cg_friendly(id);
    c->faction = cg_hash(id) % 13;
    c->position = p;
    c->attitude = (l3_quat){0, 0, 0, 1};
    c->hull = game_ship(type)->hull;
    c->shield = game_ship(type)->shield;
    cg->contact_age[i] = 0;
    cg->provoked[i] = false;
    cg->rescued[i] = false;
    cg->anchor[i] = p;
    cg->desired[i] = p;
    cg_combat_contact_init(i);
}
static void import(const lap_aircraft_t *a, int index) {
    game_view *v = &cg->v;
    int slot = -1;
    for (int i = 0; i < GAME_CONTACTS; i++)
        if (cg->mapped[i] && v->contacts[i].id == a->icao) {
            slot = i;
            break;
        }
    if (slot < 0)
        for (int i = 0; i < GAME_CONTACTS; i++)
            if (!v->contacts[i].active && cg->dead_until[i] <= v->clock) {
                slot = i;
                break;
            }
    if (slot < 0) {
        /* Receiver snapshots have priority over simulated residents. */
        float farthest=-1;
        for (int i=0;i<GAME_CONTACTS;i++)
            if (!v->contacts[i].live) {
                float d=cg_len(l3_sub(v->contacts[i].position,v->player.position));
                if (d>farthest) {farthest=d;slot=i;}
            }
    }
    if (slot < 0) return;
    if (v->contacts[slot].id == a->icao && cg->dead_until[slot] > v->clock)
        return;
    float bearing =
        a->range_km >= 0 ? a->bearing_deg : (float)(index * 137 + cg_hash(a->icao) % 45);
    float range = a->range_km >= 0 ? 100 + sqrtf(fmaxf(0, a->range_km)) * 35 : 160 + index * 48;
    range = fminf(range, 850);
    float y = a->has_alt ? cg_clamp((a->alt_ft - 12000) * 0.008f, -110, 180) : 0;
    l3_vec d = l3v(sinf(bearing * CG_RAD) * range, y, -cosf(bearing * CG_RAD) * range);
    l3_vec mapped = l3_add(v->player.position, d);
    bool fresh = v->contacts[slot].id != a->icao || !v->contacts[slot].active;
    game_class type = game_aircraft_class(a);
    game_role role = game_aircraft_hostile(a->icao, v->systems[v->system].government) ? ROLE_PIRATE
                     : type == SH_VIPER                                               ? ROLE_POLICE
                                                                                      : ROLE_TRADER;
    if (a->squawk == 7500 || a->emergency == 5)
        role = ROLE_PIRATE;
    else if (a->squawk == 7600 || a->emergency == 4)
        role = ROLE_DERELICT;
    else if (a->squawk == 7700 || a->emergency == 1 || a->emergency == 2 || a->emergency == 3 ||
             a->emergency == 6)
        role = ROLE_RESCUE;
    if (military(a->callsign) || !strcmp(a->category,"A6"))
        type=role==ROLE_PIRATE?SH_KRAIT:SH_VIPER;
    if (fresh) {
        cg->importing = true;
        cg_spawn(slot, a->icao, type, role, mapped);
        cg->importing = false;
    }
    cg->mapped[slot] = true;
    game_contact *c = &v->contacts[slot];
    if (role == ROLE_RESCUE && (cg->rescued[slot] || cg_friendly(c->id)))
        role = ROLE_TRADER;
    if (fresh || c->squawk != a->squawk || c->emergency != a->emergency) {
        if (role == ROLE_RESCUE)
            cg_message("Distress call: supplies requested.");
        if (role == ROLE_DERELICT) {
            cg_message("Silent derelict: cargo ejected.");
            for (int k = 0; k < CG_CANS; k++)
                if (cg->cans[k].life <= 0) {
                    cg->cans[k] = (cg_can){mapped, 120, 11};
                    break;
                }
        }
        if (a->squawk == 7500 || a->emergency == 5)
            cg_message("Hijacked contact: bounty authorized.");
    }
    cg->desired[slot] = mapped;
    cg->contact_age[slot] = 0;
    c->jump_out = 0;
    c->role = cg->provoked[slot] && !cg->police_origin[slot] ? ROLE_PIRATE : role;
    c->hostile = (role == ROLE_PIRATE || cg->provoked[slot]) && !cg_friendly(c->id);
    c->live = (a->icao & 0xFFFFF8) != 0xF00000;
    c->faction = game_aircraft_faction(a);
    c->ship_class = type;
    memcpy(c->callsign, a->callsign, 9);
    c->callsign[8] = 0;
    c->altitude_ft = a->has_alt ? a->alt_ft : 0;
    c->aircraft_speed_kt = a->has_speed ? a->speed_kt : 0;
    c->heading = a->has_track ? a->track_deg : 0;
    c->speed = a->has_speed ? cg_clamp(a->speed_kt * 0.045f, 0, 45) : 8;
    if (role == ROLE_DERELICT)
        c->speed = 0;
    c->velocity =
        l3v(sinf(c->heading * CG_RAD) * c->speed, 0, -cosf(c->heading * CG_RAD) * c->speed);
    c->attitude = l3_quat_euler(-c->heading, 0, 0);
    c->squawk = a->squawk;
    c->emergency = a->emergency;
    cg_register(c);
}
void cg_traffic(float dt) {
    game_view *v = &cg->v;
    cg->poll += dt;
    if (cg->poll >= 1) {
        cg->poll = fmodf(cg->poll, 1);
        int n = v->traffic_mode == 1 ? 0 : lap_aircraft(cg->aircraft, 16);
        for (int i = 0; i < n; i++)
            if (cg->aircraft[i].age_ms < 30000 &&
                (v->traffic_mode != 2 || (cg->aircraft[i].icao & 0xFFFFF8) != 0xF00000))
                import(&cg->aircraft[i], i);
    }
    cg_encounters(dt);
    for (int i = 0; i < GAME_CONTACTS; i++) {
        game_contact *c = &v->contacts[i];
        if (!c->active || !cg->mapped[i] || c->jump_out > 0)
            continue;
        cg->contact_age[i] += dt;
        if(!cg->fighters[i].initialized || cg->fighters[i].id!=c->id) cg_combat_contact_init(i);
        if(cg->fighters[i].modules[MODULE_DRIVES]<=0) {
            c->position=l3_add(c->position,l3_scale(c->velocity,dt));continue;
        }
        if (cg->contact_age[i] > 30) {
            cg_depart(i);
            continue;
        }
        cg->desired[i] = l3_add(cg->desired[i], l3_scale(c->velocity, dt));
        c->position =
            l3_add(c->position, l3_scale(l3_sub(l3_add(cg->desired[i],
                c->hull < game_ship(c->ship_class)->hull*.25f ? l3_scale(l3_norm(l3_sub(cg->desired[i],v->player.position)),20) :
                (c->role==ROLE_PIRATE ? l3v((i%3-1)*8,0,0) : l3v(0,0,0))), c->position), fminf(1, dt * 1.8f)));
    }
}
void cg_contacts(void) {
    game_view *v = &cg->v;
    v->contact_count = 0;
    l3_vec right = l3_quat_rotate(v->player.attitude, l3v(1, 0, 0)), forward = cg_forward();
    for (int i = 0; i < GAME_CONTACTS; i++)
        if (v->contacts[i].active) {
            game_contact *c = &v->contacts[i];
            l3_vec d = l3_sub(c->position, v->player.position);
            c->range = cg_len(d);
            c->height = d.y;
            c->bearing = atan2f(l3_dot(d, right), l3_dot(d, forward)) / CG_RAD;
            c->detectable=v->player.modules[MODULE_SENSORS]>0 &&
                c->range<cg->rules.combat.defence[CF_SCAN_RANGE]*v->player.modules[MODULE_SENSORS] &&
                game_poi_detectable(v->player.position,c->position);
            if(c->detectable) v->contact_count++;
        }
    if(v->player.modules[MODULE_SENSORS]<=0) {v->selected=-1;v->subtarget=-1;return;}
    if (v->selected < 0 || !v->contacts[v->selected].active)
        game_target_nearest(false);
}
