/* Integer credits and bounded quantities: a failed transaction changes nothing.
 */
#include "internal.h"
void cg_award(uint32_t credits) {
    uint32_t room=1000000000u-cg->v.player.credits;
    cg->v.player.credits += credits<room ? credits : room;
}
int game_price(const game_system *s, int g, uint32_t day) {
    if (!cg || !s || g < 0 || g >= GAME_GOODS)
        return 0;
    int variation = (int)(cg_hash(s->seed ^ cg_hash(day) ^ ((uint32_t)g * 7919)) % 21) - 10;
    int price = cg->rules.base[g] + cg->rules.slope[g] * ((int)s->economy - 3);
    price += ((int)s->government - 3) * 2 + (int)s->tech * (g == 6 ? 3 : 1);
    price += price * variation / 100;
    return (int)fmaxf(1, price * game_event_price_percent(game_system_event(s->seed, day), g) / 100);
}
void cg_market(void) {
    static const char *const names[] = {"Food",     "Textiles",  "Radioactives", "Liquor",
                                        "Luxuries", "Narcotics", "Computers",    "Machinery",
                                        "Alloys",   "Firearms",  "Furs",         "Minerals",
                                        "Gold",     "Platinum",  "Gem-stones",   "Relic fragments"};
    lap_time_t time = {0};
    lap_time(&time);
    cg->v.local_hour = (uint8_t)cg_clamp((float)time.hour, 0, 23);
    for (int i = 0; i < GAME_GOODS; i++) {
        game_market_item *m = &cg->v.market[i];
        snprintf(m->name, sizeof(m->name), "%s", names[i]);
        m->price = game_price(&cg->v.systems[cg->v.system], i, cg->v.market_day);
        m->stock =
            5 + cg_hash(cg->v.systems[cg->v.system].seed ^ cg->v.market_day ^ (uint32_t)i) % 40;
        m->illegal = i == 5 || i == 9;
    }
}
int game_cargo_capacity(void) {
    return cg ? game_player_ship()->cargo + ((cg->v.player.equipment & (1u << EQ_CARGO)) ? 15 : 0)
              : 0;
}
int game_cargo_used(void) {
    if (!cg)
        return 0;
    int n = 0;
    for (int i = 0; i < GAME_GOODS; i++)
        n += cg->v.player.cargo[i];
    return n;
}
bool game_market_buy(int good, int amount) {
    if (!cg || good < 0 || good >= GAME_GOODS || amount <= 0 || !cg->v.player.docked)
        return false;
    game_market_item *m = &cg->v.market[good];
    game_player *p = &cg->v.player;
    if (amount > m->stock || amount > game_cargo_capacity() - game_cargo_used() ||
        (uint32_t)amount > p->credits / (unsigned)m->price)
        return false;
    p->credits -= amount * m->price;
    p->cargo[good] += amount;
    m->stock -= amount;
    return true;
}
bool game_market_sell(int good, int amount) {
    if (!cg || good < 0 || good >= GAME_GOODS || amount <= 0 || !cg->v.player.docked)
        return false;
    game_market_item *m = &cg->v.market[good];
    game_player *p = &cg->v.player;
    if (amount > p->cargo[good] || p->credits > 1000000000u - (unsigned)(amount * m->price))
        return false;
    p->credits += amount * m->price;
    p->cargo[good] -= amount;
    m->stock += amount;
    if (m->illegal)
        p->legal = (uint8_t)fminf(255, p->legal + amount);
    return true;
}
bool game_equipment_info(int e, game_equipment_offer *out) {
    static const char *const names[] = {"Pulse laser",      "Homing missile", "Shield booster",
                                        "Docking computer", "Fuel scoop",     "ECM",
                                        "Energy unit",      "Cargo extension"};
    if (!cg || !out || e < 0 || e >= EQ_COUNT)
        return false;
    const game_player *p = &cg->v.player;
    out->name = names[e];
    out->price = cg->rules.equipment[e];
    out->min_tech = (e == EQ_ECM || e == EQ_ENERGY) ? 5 : 1;
    out->owned = e == EQ_LASER ? p->lasers >= p->mounts :
                 e != EQ_MISSILE && (p->equipment & (1u << e));
    out->available = p->docked && !out->owned && p->credits >= out->price &&
                     cg->v.systems[cg->v.system].tech >= out->min_tech &&
                     (e != EQ_MISSILE || p->missiles < p->pylons) && !p->dead;
    return true;
}
uint32_t game_refuel_cost(void) {
    return cg ? (uint32_t)ceilf(fmaxf(0, cg->v.player.fuel_max - cg->v.player.fuel) * cg->rules.fuel_price) : 0;
}
uint32_t game_repair_cost(void) {
    if(!cg) return 0;
    const game_player *p=&cg->v.player;float damage=fmaxf(0,game_player_ship()->hull-p->hull);
    /* A zeroed legacy test/commander has no combat extension yet. */
    if(p->pips[0]+p->pips[1]+p->pips[2]==6) {
        damage+=(1-p->canopy)*game_player_ship()->hull*.1f;
        for(int i=0;i<GAME_MODULES;i++) damage+=(1-p->modules[i])*game_player_ship()->hull*.05f;
    }
    return (uint32_t)ceilf(damage*cg->rules.repair_price);
}
bool game_equip(int e) {
    game_equipment_offer offer;
    if (!game_equipment_info(e, &offer) || !offer.available)
        return false;
    game_player *p = &cg->v.player;
    cg_combat_player_ensure(p);
    if(e==EQ_LASER) {
        for(int i=0;i<p->mounts;i++) if(p->weapons[i].kind==WEAPON_NONE) {
            p->weapons[i]=(game_mount){.kind=WEAPON_PULSE};p->credits-=offer.price;
            p->lasers++;p->equipment|=1u<<EQ_LASER;return true;
        }
        return false;
    }
    p->credits -= offer.price;
    if (e == EQ_MISSILE)
        p->missiles++;
    else {
        p->equipment |= 1u << e;
        if(e==EQ_LASER) p->lasers++;
    }
    return true;
}
bool game_refuel(void) {
    if (!cg || !cg->v.player.docked || cg->v.player.dead)
        return false;
    game_player *p = &cg->v.player;
    unsigned cost = game_refuel_cost();
    if (p->credits < cost)
        return false;
    p->credits -= cost;
    p->fuel = p->fuel_max;
    return true;
}
bool game_repair(void) {
    if (!cg || !cg->v.player.docked || cg->v.player.dead)
        return false;
    game_player *p = &cg->v.player;
    unsigned cost = game_repair_cost();
    if (p->credits < cost)
        return false;
    p->credits -= cost;
    p->hull = game_player_ship()->hull;
    for(int i=0;i<GAME_MODULES;i++) p->modules[i]=1;
    p->canopy=1;p->oxygen=cg->rules.combat.defence[CF_OXYGEN];
    cg->cargo_spilled=false;cg->alarm_hull=p->hull;
    p->dead = false;
    return true;
}
