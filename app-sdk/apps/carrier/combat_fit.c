/* Mount transactions and consumable servicing on the existing dockside market. */
#include "internal.h"
static uint32_t mode_price(int kind,int mode) {
    return (uint32_t)floorf(cg->rules.combat.weapons[kind].price*cg->rules.mount_price[mode]+.5f);
}
static void laser_count(void) {
    game_player *p=&cg->v.player;p->lasers=0;
    for(int i=0;i<p->mounts;i++) if(p->weapons[i].kind<=WEAPON_BEAM) p->lasers++;
    if(p->lasers) p->equipment|=1u<<EQ_LASER;else p->equipment&=~(1u<<EQ_LASER);
}
bool game_weapon_info(game_weapon_kind kind,game_equipment_offer *out) {
    static const char *const names[]={"PULSE","BEAM","RATTLE GUN","SLUG GUN","SEEKER","DUMBFIRE"};
    if(!cg || !out || kind<0 || kind>=WEAPON_COUNT) return false;
    const cg_weapon_rule *w=&cg->rules.combat.weapons[kind];const game_player *p=&cg->v.player;
    uint32_t price=mode_price(kind,MOUNT_FIXED);
    *out=(game_equipment_offer){.name=names[kind],.price=price,.mode=MOUNT_FIXED,.mode_price=price,.min_tech=w->tech,
        .available=p->docked && !p->dead && p->credits>=price &&
            cg->v.systems[cg->v.system].tech>=w->tech && (kind<WEAPON_SEEKER || p->pylons>0)};
    return true;
}
bool game_fit_weapon(int mount,game_weapon_kind kind) {
    if(!cg) return false;
    cg_combat_player_ensure(&cg->v.player);
    if(mount<0 || mount>=cg->v.player.mounts) return false;
    game_equipment_offer out;if(!game_weapon_info(kind,&out) || !out.available) return false;
    game_player *p=&cg->v.player;if(p->weapons[mount].kind==kind) return false;
    const cg_weapon_rule *w=&cg->rules.combat.weapons[kind];
    uint8_t mode=p->weapons[mount].mode;
    uint32_t price=mode_price(kind,mode);
    if(p->credits<price) return false;
    p->credits-=price;
    p->weapons[mount]=(game_mount){.kind=(uint8_t)kind,.group=w->group,.ammo=w->ammo,.mode=mode};
    cg->mount_cooldown[mount]=0;laser_count();return true;
}
bool game_weapon_group(int mount,int group) {
    if(!cg || mount<0 || mount>=cg->v.player.mounts || group<0 || group>1) return false;
    cg->v.player.weapons[mount].group=(uint8_t)group;
    cg->v.player.weapons[mount].charge=0;cg->v.player.weapons[mount].firing=false;return true;
}
uint32_t game_rearm_cost(void) {
    if(!cg) return 0;
    const game_player *p=&cg->v.player;const cg_combat_rules *r=&cg->rules.combat;
    uint32_t cost=(uint32_t)(p->pylons-p->missiles)*cg->rules.equipment[EQ_MISSILE];
    const uint8_t counts[]={p->shield_cells,p->chaff,p->heat_sinks};
    for(int i=0;i<3;i++) if(counts[i]<r->supplies[i]) cost+=(r->supplies[i]-counts[i])*r->supply_price[i];
    for(int i=0;i<p->mounts;i++) if(p->weapons[i].kind==WEAPON_RATTLE) {
        const cg_weapon_rule *w=&r->weapons[WEAPON_RATTLE];
        uint32_t unit=(w->price+w->ammo*20-1)/(w->ammo*20);
        if(p->weapons[i].ammo<w->ammo) cost+=(w->ammo-p->weapons[i].ammo)*unit;
    }
    return cost;
}
bool game_rearm(void) {
    if(!cg || !cg->v.player.docked || cg->v.player.dead) return false;
    game_player *p=&cg->v.player;uint32_t price=game_rearm_cost();if(p->credits<price) return false;
    p->credits-=price;p->missiles=p->pylons;
    const cg_combat_rules *r=&cg->rules.combat;
    p->shield_cells=(uint8_t)r->supplies[0];p->chaff=(uint8_t)r->supplies[1];p->heat_sinks=(uint8_t)r->supplies[2];
    for(int i=0;i<p->mounts;i++) if(p->weapons[i].kind==WEAPON_RATTLE) p->weapons[i].ammo=r->weapons[WEAPON_RATTLE].ammo;
    return true;
}

/* Quotes retain the kind-based API; this mount-based quote includes its mode. */
bool game_mount_info(int mount,game_equipment_offer *out) {
    if(!cg || mount<0 || mount>=cg->v.player.mounts || !out) return false;
    game_mount *m=&cg->v.player.weapons[mount];
    if(!game_weapon_info((game_weapon_kind)m->kind,out)) return false;
    out->mode=m->mode;out->price=out->mode_price=mode_price(m->kind,m->mode);
    out->available=out->available && cg->v.player.credits>=out->price;
    return true;
}
bool game_mount_mode(int mount,int mode) {
    if(!cg || !cg->v.player.docked || cg->v.player.dead || mode<MOUNT_FIXED || mode>MOUNT_TURRET) return false;
    cg_combat_player_ensure(&cg->v.player);
    game_player *p=&cg->v.player;
    if(mount<0 || mount>=p->mounts || p->weapons[mount].kind>=WEAPON_COUNT) return false;
    game_mount *m=&p->weapons[mount];
    if(m->mode==mode) return false;
    int32_t difference=(int32_t)mode_price(m->kind,mode)-(int32_t)mode_price(m->kind,m->mode);
    if(difference>0 && p->credits<(uint32_t)difference) return false;
    if(difference>=0) p->credits-=(uint32_t)difference;
    else {
        if((uint32_t)-difference>1000000000u-p->credits) return false;
        p->credits+=(uint32_t)-difference;
    }
    m->mode=(uint8_t)mode;m->charge=0;m->firing=false;
    cg->v.turret_aim[mount]=cg_forward();cg->v.turret_on[mount]=false;
    return true;
}
