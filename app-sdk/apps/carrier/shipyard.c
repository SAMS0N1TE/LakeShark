/* Dockside hull transactions and insurance; bounded rules in the app PSRAM pool. */
#include "internal.h"

game_class game_player_class(void) {
    return cg && cg->v.player.fuel_max>0 && cg->v.player.ship_class<11 ?
        (game_class)cg->v.player.ship_class : SH_MAKO;
}
const game_ship_stats *game_player_ship(void) { return game_ship(game_player_class()); }
void cg_ship_fit(game_player *p, game_class c) {
    const cg_yard_hull *h=&cg->rules.yard.hulls[c];
    p->ship_class=(uint8_t)c;p->fuel_max=h->fuel_max;p->seats=h->seats;
    p->pylons=h->pylons;p->mounts=h->mounts;
}
bool cg_ship_valid(const game_player *p) {
    if(p->ship_class>=11) return false;
    const game_ship_stats *s=game_ship((game_class)p->ship_class);
    const cg_yard_hull *h=&cg->rules.yard.hulls[p->ship_class];
    int cargo=0;
    for(int i=0;i<GAME_GOODS;i++) cargo+=p->cargo[i];
    return p->hull<=s->hull && p->fuel<=h->fuel_max && p->missiles<=h->pylons &&
        p->lasers<=h->mounts && ((p->lasers!=0)==((p->equipment&(1u<<EQ_LASER))!=0)) &&
        cargo<=s->cargo+((p->equipment&(1u<<EQ_CARGO))?15:0);
}
uint32_t cg_ship_trade_in(void) {
    if(!cg) return 0;
    const cg_yard_hull *h=&cg->rules.yard.hulls[game_player_class()];
    float condition=cg_clamp(cg->v.player.hull/game_player_ship()->hull,0,1);
    return (uint32_t)floorf((float)h->price*cg->rules.yard.trade_percent*.01f*condition);
}
static bool stocked(game_class c) {
    const game_system *s=&cg->v.systems[cg->v.system];
    const cg_yard_hull *h=&cg->rules.yard.hulls[c];
    return s->tech>=h->min_tech && s->government<8 &&
        (h->government_mask&(1u<<s->government)) &&
        cg_hash(s->seed ^ cg_hash(cg->v.market_day) ^ ((uint32_t)c*7919u))%100<h->stock_percent;
}
int game_shipyard(game_yard_offer *out, int max) {
    if(!cg || !out || max<=0 || !cg->v.player.docked || cg->v.player.dead) return 0;
    int n=max<11?max:11;
    uint32_t trade=cg_ship_trade_in();
    for(int i=0;i<n;i++) {
        const cg_yard_hull *h=&cg->rules.yard.hulls[i];
        out[i]=(game_yard_offer){.ship_class=(game_class)i,.price=(int32_t)h->price-(int32_t)trade,
            .trade_in=trade,.available=i!=(int)game_player_class() && stocked((game_class)i),
            .fuel_max=h->fuel_max,.seats=h->seats,.pylons=h->pylons,.mounts=h->mounts,.pitch=h->pitch};
    }
    return n;
}
static bool refuse(const char *reason) { cg_message(reason);return false; }
static void new_hull(game_class c) {
    game_player *p=&cg->v.player;
    cg_combat_player_ensure(p);
    cg_ship_fit(p,c);
    if(p->missiles>p->pylons) p->missiles=p->pylons;
    if(p->lasers>p->mounts) p->lasers=p->mounts;
    if(!p->lasers) p->equipment&=~(1u<<EQ_LASER);
    p->fuel=fminf(p->fuel,p->fuel_max); /* no complimentary fuel */
    p->hull=game_player_ship()->hull;
    p->shield_fore=p->shield_aft=game_player_ship()->shield*((p->equipment&(1u<<EQ_SHIELD))?1.5f:1);
    p->heat=p->cabin_heat=0;p->energy=100;
    /* Transfer mounted fittings that fit the replacement hull. Older callers
       describe their laser count only, so fill vacant legacy pulse mounts. */
    for(int i=0;i<GAME_MOUNTS;i++) {
        if(i>=p->mounts) p->weapons[i]=(game_mount){.kind=WEAPON_NONE};
        else if(i<p->lasers && p->weapons[i].kind==WEAPON_NONE) p->weapons[i].kind=WEAPON_PULSE;
        p->weapons[i].charge=p->weapons[i].heat=0;p->weapons[i].firing=false;
    }
    for(int i=0;i<GAME_MODULES;i++) p->modules[i]=1;
    p->canopy=1;p->oxygen=cg->rules.combat.defence[CF_OXYGEN];p->weapon_capacitor=100;
    p->shield_reboot[0]=p->shield_reboot[1]=0;p->heat_level=0;
    p->boost_left=p->boost_cooldown=p->chaff_left=0;
    if(p->pips[0]+p->pips[1]+p->pips[2]!=6) game_pips_preset(0);
    cg_combat_reset();
}
bool game_buy_ship(game_class c) {
    if(!cg) return false;
    game_player *p=&cg->v.player;
    if(!p->docked || p->dead) return refuse("Shipyard requires a living, docked commander.");
    if(c<SH_DOGFISH || c>SH_COOKIECUTTER || c==game_player_class() || !stocked(c))
        return refuse("That hull is unavailable at this shipyard.");
    int capacity=game_ship(c)->cargo+((p->equipment&(1u<<EQ_CARGO))?15:0);
    if(game_cargo_used()>capacity) return refuse("Cargo will not fit. Sell freight before exchanging hulls.");
    int occupied=p->seats-game_passenger_seats();
    if(occupied>cg->rules.yard.hulls[c].seats) return refuse("Passengers will not fit. Complete their passage first.");
    int32_t price=(int32_t)cg->rules.yard.hulls[c].price-(int32_t)cg_ship_trade_in();
    if(price>0 && p->credits<(uint32_t)price) return refuse("Insufficient credits for this hull.");
    if(price<0 && (uint32_t)-price>1000000000u-p->credits) return refuse("Trade-in refund would exceed the credit limit.");
    if(price>=0) p->credits-=(uint32_t)price;else p->credits+=(uint32_t)-price;
    new_hull(c);
    cg_message("Hull exchanged. Fittings transferred; refuel separately.");
    return true;
}
uint32_t game_insurance_cost(void) {
    if(!cg) return 0;
    return (cg->rules.yard.hulls[game_player_class()].price*cg->rules.yard.insurance_percent+99u)/100u;
}
bool game_rebuy(void) {
    if(!cg || !cg->v.player.dead) return false;
    game_player *p=&cg->v.player;
    uint32_t cost=game_insurance_cost();
    bool insured=p->credits>=cost;
    game_class c=insured?game_player_class():SH_DOGFISH;
    if(insured) p->credits-=cost;
    else {p->equipment=(1u<<EQ_LASER)|(1u<<EQ_DOCKING);p->lasers=1;p->missiles=0;
        cg_combat_player_init(p);}
    memset(p->cargo,0,sizeof(p->cargo));
    for(int i=0;i<6;i++) {game_mission *m=&cg->v.missions[i];
        if(m->accepted && !m->done && !m->failed) {m->failed=true;m->ready=false;}}
    new_hull(c);
    cg_arrive(cg->yard_station);
    p->dead=false;p->docked=true;p->docking=p->firing=p->mass_locked=false;
    p->position=l3_add(cg->v.contents.station,l3v(0,0,-95));
    p->attitude=l3_quat_euler(180,0,cg->v.station_angle);p->velocity=l3v(0,0,0);
    p->speed=p->throttle=p->pitch=p->roll=p->yaw=0;
    cg->fire_cooldown=cg->ecm=cg->dock_time=0;cg->dock_slot=false;
    cg->poi.shield_valid=false;
    cg_actions();
    cg_message(insured?"Insurance rebuy complete. Cargo and contracts lost.":
        "Emergency Dogfish issued. Cargo and contracts lost.");
    return true;
}
