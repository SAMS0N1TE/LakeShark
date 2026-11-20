/* Deliberately exposed only through the hidden STATUS admin panel. */
#include "internal.h"
static void repair(void) {
    game_player *p=&cg->v.player;
    p->hull=game_player_ship()->hull;p->dead=false;
    float shield=game_player_ship()->shield*((p->equipment&(1u<<EQ_SHIELD))?1.5f:1);
    p->shield_fore=p->shield_aft=shield;p->shield_reboot[0]=p->shield_reboot[1]=0;
    for(int i=0;i<GAME_MODULES;i++) p->modules[i]=1;
    p->canopy=1;p->oxygen=cg->rules.combat.defence[CF_OXYGEN];
    p->energy=p->weapon_capacitor=100;p->heat=p->heat_level=p->cabin_heat=0;
    cg->cargo_spilled=false;
}
void cg_admin_protect(void) { if(cg && cg->v.player.god_mode) repair(); }
bool game_admin(game_admin_action action,int argument) {
    if(!cg || action<ADMIN_CREDITS || action>ADMIN_REVEAL) return false;
    game_player *p=&cg->v.player;
    if(action==ADMIN_JUMP && (argument<0 || argument>=GAME_SYSTEMS)) return false;
    if(action==ADMIN_FOE) {
        int slot=-1;
        for(int i=16;i<GAME_CONTACTS;i++) if(!cg->v.contacts[i].active) {slot=i;break;}
        if(slot<0 || p->docked) return false;
        cg_spawn(slot,++cg->next_id,SH_KRAIT,ROLE_PIRATE,l3_add(p->position,l3_scale(cg_forward(),350)));
        cg->v.contacts[slot].hostile=true;
    } else if(action==ADMIN_MISSION) {
        int slot=-1;
        for(int i=0;i<6;i++) if(cg->v.missions[i].accepted && !cg->v.missions[i].done && !cg->v.missions[i].failed) {slot=i;break;}
        if(slot<0) return false;
        game_mission *m=&cg->v.missions[slot];
        if(m->kind==MISSION_DELIVERY || m->kind==MISSION_SMUGGLING || m->kind==MISSION_SALVAGE)
            p->cargo[m->good]-=(uint16_t)fminf(p->cargo[m->good],m->amount);
        m->ready=m->done=true;cg_award(m->reward);
    } else if(action==ADMIN_CREDITS) cg_award(100000);
    else if(action==ADMIN_FUEL) p->fuel=p->fuel_max;
    else if(action==ADMIN_REPAIR) repair();
    else if(action==ADMIN_RESTOCK) {
        p->missiles=p->pylons;p->shield_cells=(uint8_t)cg->rules.combat.supplies[0];
        p->chaff=(uint8_t)cg->rules.combat.supplies[1];p->heat_sinks=(uint8_t)cg->rules.combat.supplies[2];
        for(int i=0;i<p->mounts;i++) if(p->weapons[i].kind<WEAPON_COUNT)
            p->weapons[i].ammo=cg->rules.combat.weapons[p->weapons[i].kind].ammo;
    } else if(action==ADMIN_CARGO) p->cheat_cargo=true;
    else if(action==ADMIN_GOD) {p->god_mode=!p->god_mode;cg_admin_protect();}
    else if(action==ADMIN_REVEAL) {
        p->galaxy_revealed=true;cg->v.hidden_system_found=true;
        for(int i=0;i<GAME_SYSTEMS;i++) cg->poi.marks[i].discovered=15;
        for(int i=0;i<cg->v.poi_count;i++) cg->v.pois[i].discovered=true;
        cg->poi.dirty=true;
    } else if(action==ADMIN_JUMP) {
        p->docking=p->cruising=false;p->velocity=l3v(0,0,0);p->throttle=0;
        cg->v.jump=JUMP_IDLE;cg->v.jump_progress=0;cg->v.plotted=-1;
        cg_arrive(argument);p->position=l3_add(cg->v.contents.station,l3v(0,0,-300));
    }
    p->cheats_used=true;cg_message("ADMIN: cheats used on this commander.");
    game_save();return true;
}
