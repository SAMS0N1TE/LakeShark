/* Fixed-pool combat simulation and HUD observations; no drawing, IO or allocation. */
#include "internal.h"

static const cg_combat_rules *rules(void) { return &cg->rules.combat; }
static bool flying(void) {
    cg_combat_player_ensure(&cg->v.player);
    const game_player *p=&cg->v.player;
    return !p->docked && !p->dead && !p->cruising && cg->v.jump==JUMP_IDLE;
}
static float shield_max(void) {
    return game_player_ship()->shield*((cg->v.player.equipment&(1u<<EQ_SHIELD))?1.5f:1);
}
static float sys_rate(const game_player *p) {
    return rules()->distributor[CD_SYS_BASE]+p->pips[0]*rules()->distributor[CD_SYS_STEP];
}
float cg_drive_scale(const game_player *p) { return p->modules[MODULE_DRIVES]*p->modules[MODULE_POWER]; }
static void heat_sync(game_player *p) {
    p->heat=cg_clamp(p->heat,0,150);
    p->heat_level=cg_clamp(fmaxf(p->heat*.01f,p->cabin_heat),0,1.5f);
}
void cg_combat_log(const char *text) {
    for(int i=5;i>0;i--) memcpy(cg->v.combat_log[i],cg->v.combat_log[i-1],48);
    snprintf(cg->v.combat_log[0],48,"%s",text);
    if(cg->v.combat_log_count<6) cg->v.combat_log_count++;
}
void game_combat_ack(uint32_t events) { if(cg) cg->v.combat_events&=~events; }
/* All spawn/migration paths share the same full-health, empty-mount baseline. */
static void combat_defaults(float modules[GAME_MODULES],game_mount weapons[GAME_MOUNTS]) {
    for(int i=0;i<GAME_MODULES;i++) modules[i]=1;
    for(int i=0;i<GAME_MOUNTS;i++) weapons[i]=(game_mount){.kind=WEAPON_NONE};
}
void cg_combat_player_ensure(game_player *p) {
    /* A valid distributor always totals six, even when every module is destroyed.
       Zero pips identify an old commander or a caller without the extension. */
    if(p->pips[0]==0 && p->pips[1]==0 && p->pips[2]==0) cg_combat_player_init(p);
}
void cg_combat_player_init(game_player *p) {
    if(p->fuel_max<=0 || p->ship_class>=11) cg_ship_fit(p,SH_MAKO);
    if(!p->lasers && (p->equipment&(1u<<EQ_LASER))) p->lasers=1;
    if(p->lasers>p->mounts) p->lasers=p->mounts;
    combat_defaults(p->modules,p->weapons);
    p->pips[0]=p->pips[1]=p->pips[2]=2;
    p->canopy=1;p->oxygen=rules()->defence[CF_OXYGEN];p->weapon_capacitor=100;
    p->shield_cells=(uint8_t)rules()->supplies[0];p->chaff=(uint8_t)rules()->supplies[1];
    p->heat_sinks=(uint8_t)rules()->supplies[2];
    p->boost_left=p->boost_cooldown=p->chaff_left=p->seeker_lock=0;
    p->shield_reboot[0]=p->shield_reboot[1]=0;p->firing2=false;
    p->hit_head=p->hit_count=0;p->weapons_hot=0;
    for(int i=0;i<GAME_MOUNTS;i++) p->weapons[i]=(game_mount){.kind=i<p->lasers?WEAPON_PULSE:WEAPON_NONE};
    heat_sync(p);
}
void cg_combat_contact_init(int i) {
    game_contact *c=&cg->v.contacts[i];cg_combat_contact *f=&cg->fighters[i];
    memset(f,0,sizeof(*f));f->initialized=true;f->id=c->id;f->last_attack=-100;f->capacitor=100;
    combat_defaults(f->modules,f->weapons);
    f->mounts=c->ship_class<11?cg->rules.yard.hulls[c->ship_class].mounts:1;
    /* A pirate's two mounts mix energy and ballistic fire. Frill drones charge
       a slug gun and carry seekers instead of behaving like laser fighters. */
    if(c->ship_class==SH_DRONE) f->mounts=2;
    for(int j=0;j<GAME_MOUNTS;j++) {
        int row=c->ship_class==SH_DRONE?2:c->role==ROLE_POLICE?1:c->role==ROLE_PIRATE?0:-1;
        int column=f->mounts==1 && row==0?(int)(cg_hash(c->id)%2):j;
        int kind=j>=f->mounts?WEAPON_NONE:row<0?WEAPON_PULSE:rules()->npc_weapons[row][column];
        f->weapons[j]=(game_mount){.kind=(uint8_t)kind};
        if(kind<WEAPON_COUNT) f->weapons[j].ammo=kind>=WEAPON_SEEKER?3:rules()->weapons[kind].ammo;
    }
    c->scanned=c->wanted=c->attacking_player=false;c->scan_progress=0;c->bounty=0;
    c->rank_text[0]=0;c->threat=0;c->shield_reboot=0;
    memset(c->modules,0,sizeof(c->modules));memcpy(c->weapons,f->weapons,sizeof(c->weapons));
    cg->npc_lock[i]=0;cg->npc_lock_id[i]=0;
}
void cg_combat_reset(void) {
    cg_combat_player_ensure(&cg->v.player);
    memset(cg->v.projectiles,0,sizeof(cg->v.projectiles));
    memset(cg->missiles,0,sizeof(cg->missiles));memset(cg->beams,0,sizeof(cg->beams));
    memset(cg->mount_cooldown,0,sizeof(cg->mount_cooldown));
    memset(cg->npc_lock,0,sizeof(cg->npc_lock));memset(cg->npc_lock_id,0,sizeof(cg->npc_lock_id));
    memset(cg->v.turret_on,0,sizeof(cg->v.turret_on));
    memset(cg->v.turret_aim,0,sizeof(cg->v.turret_aim));
    cg->selected_id=0;cg->selected_slot=-1;cg->v.subtarget=-1;
    cg->missile_pending=cg->cargo_spilled=false;cg->v.missile_warning=false;cg->v.missile_range=0;
    cg->v.player.seeker_lock=0;cg->alarm_hull=cg->v.player.hull;
}
void cg_combat_dock(void) {
    game_player *p=&cg->v.player;
    /* Air and cockpit glass are restored by port services; module/hull work is
       separately priced. The repair quote before docking includes the canopy. */
    p->canopy=1;p->oxygen=rules()->defence[CF_OXYGEN];p->firing=p->firing2=false;
    p->boost_left=p->chaff_left=0;p->seeker_lock=0;cg->missile_pending=false;
}
const char *game_drive_refusal(void) {
    return cg && (cg->v.player.firing || cg->v.player.firing2 || cg->v.player.weapons_hot>0)?"Weapons hot":NULL;
}
void cg_weapons_hot(void) {
    game_player *p=&cg->v.player;
    p->weapons_hot=cg->rules.weapons_cool_s;
    if(cg->v.jump==JUMP_CHARGE) {
        cg->v.jump=JUMP_IDLE;cg->v.jump_progress=0;
        cg_message("Weapons hot");cg_combat_log("Weapons hot");
    }
    if(p->cruising) {cg_cruise_hot();cg_message("Weapons hot");cg_combat_log("Weapons hot");}
}
void game_fire2(bool on) { if(cg) {cg_combat_player_ensure(&cg->v.player);cg->v.player.firing2=on;if(on) cg_weapons_hot();} }
void game_pips_preset(int preset) {
    if(!cg || preset<0 || preset>3) return;
    game_player *p=&cg->v.player;
    cg_combat_player_ensure(p);
    for(int i=0;i<3;i++) p->pips[i]=preset==0?2:(i==preset-1?4:1);
    /* Presets are shields, attack, run, while the bars are SYS, ENG, WEP. */
    if(preset==2) {p->pips[1]=1;p->pips[2]=4;}
    if(preset==3) {p->pips[1]=4;p->pips[2]=1;}
}
void game_pip(int which) {
    if(!cg || which<0 || which>2) return;
    cg_combat_player_ensure(&cg->v.player);
    float *p=cg->v.player.pips;
    int halves=(int)fminf(2,(4-p[which])*2);
    for(int n=0;n<halves;n++) {
        int a=(which+1+n)%3;if(a==which) a=(which+1)%3;
        int b=3-which-a;
        int donor=p[a]>=p[b]?a:b;
        if(p[donor]<.5f) break;
        p[donor]-=.5f;p[which]+=.5f;
    }
}
void game_subtarget_next(void) {
    if(!cg) return;
    int i=cg->v.selected;
    if(i<0 || i>=GAME_CONTACTS || !cg->v.contacts[i].active || !cg->v.contacts[i].scanned) {
        cg->v.subtarget=-1;return;
    }
    cg->v.subtarget++;
    if(cg->v.subtarget>=GAME_MODULES) cg->v.subtarget=-1;
}
static void break_locks(void) {
    for(int i=0;i<CG_MISSILES;i++) {
        cg_missile *m=&cg->missiles[i];
        if(m->life>0 && !m->player && m->target==-1) m->lost_lock=true;
    }
    memset(cg->npc_lock,0,sizeof(cg->npc_lock));
}
bool game_shield_cell(void) {
    if(!cg || !flying()) return false;
    game_player *p=&cg->v.player;
    if(!p->shield_cells || p->modules[MODULE_SHIELDS]<=0 || p->modules[MODULE_POWER]<=0) return false;
    bool restored=false;
    for(int i=0;i<2;i++) {
        float *s=i?&p->shield_aft:&p->shield_fore;
        if(p->shield_reboot[i]<=0 && *s>0 && *s<shield_max()) {
            *s=fminf(shield_max(),*s+shield_max()*rules()->defence[CF_CELL]);restored=true;
        }
    }
    if(!restored) return false;
    p->shield_cells--;p->heat+=rules()->defence[CF_CELL_HEAT];heat_sync(p);
    cg_combat_log("SHIELD CELL discharged.");return true;
}
bool game_boost(void) {
    if(!cg || !flying()) return false;
    game_player *p=&cg->v.player;
    if(p->boost_cooldown>0 || cg_drive_scale(p)<=0) return false;
    p->boost_left=rules()->defence[CF_BOOST_TIME];p->boost_cooldown=rules()->defence[CF_BOOST_COOLDOWN];
    p->heat+=rules()->defence[CF_BOOST_HEAT];heat_sync(p);cg_combat_log("BOOST engaged.");return true;
}
bool game_chaff(void) {
    if(!cg || !flying() || !cg->v.player.chaff || cg->v.player.chaff_left>0) return false;
    game_player *p=&cg->v.player;p->chaff--;p->chaff_left=rules()->defence[CF_CHAFF];
    break_locks();cg_combat_log("CHAFF deployed.");return true;
}
bool game_heat_sink(void) {
    if(!cg || !flying() || !cg->v.player.heat_sinks) return false;
    game_player *p=&cg->v.player;p->heat_sinks--;p->heat=fmaxf(0,p->heat-rules()->defence[CF_SINK]);
    p->cabin_heat=fmaxf(0,p->cabin_heat-rules()->defence[CF_SINK]*.01f);
    break_locks();heat_sync(p);cg_combat_log("HEAT SINK ejected.");return true;
}
void cg_beam_add(l3_vec a, l3_vec b, bool player) {
    for(int i=0;i<CG_BEAMS;i++) if(cg->beams[i].life<=0) {
        cg->beams[i]=(cg_beam){a,b,.12f,player};return;
    }
}
static void spill(l3_vec pos, int good) {
    for(int i=0;i<CG_CANS;i++) if(cg->cans[i].life<=0) {
        cg->cans[i]=(cg_can){.p=pos,.life=150,.good=good};return;
    }
}
void cg_explode(l3_vec p, int target, bool player) {
    for (int i = 0; i < CG_DEBRIS; i++)
        if (cg->debris[i].life <= 0) {
            l3_vec v = l3v(cg_unit(&cg->rng) * 2 - 1, cg_unit(&cg->rng) * 2 - 1,
                           cg_unit(&cg->rng) * 2 - 1);
            cg->debris[i] = (cg_debris){p, l3_scale(v, 30 + cg_unit(&cg->rng) * 30),
                                        .6f + cg_unit(&cg->rng) * 1.4f,
                                        (uint8_t)(i % 3 == 0   ? 15
                                                  : i % 3 == 1 ? 11
                                                               : 9)};
        }
    if (target < 0)
        return;
    game_contact *c = &cg->v.contacts[target];
    c->active = false;
    cg->dead_until[target] = cg->v.clock + 120;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < CG_CANS; i++)
            if (cg->cans[i].life <= 0) {
                cg->cans[i] = (cg_can){l3_add(p, l3v(k * 7 - 7, 3, 4)), 150,
                                       (int)(cg_rand(&cg->rng) % GAME_GOODS)};
                break;
            }
    cg_combat_log("Contact destroyed.");
    if (player) {
        game_player *p = &cg->v.player;
        if ((c->hostile && !cg->provoked[target]) ||
            (cg->encounter[target].kind==CE_SMUGGLERS && c->role==ROLE_TRADER)) {
            uint32_t bounty=(uint32_t)(game_ship(c->ship_class)->weapon*8+game_ship(c->ship_class)->hull);
            cg_award(bounty);
            cg_message("Bounty awarded. Cargo released.");
            char line[48];snprintf(line,sizeof(line),"Bounty paid: %lu cr.",(unsigned long)bounty);cg_combat_log(line);
        } else {
            p->legal = cg->police_origin[target] || c->role==ROLE_POLICE ? 255 : (uint8_t)fminf(255, p->legal + 20);
            cg_message("Unlawful destruction recorded.");
        }
        p->kills++;
        uint32_t weight=1+(uint32_t)(game_ship(c->ship_class)->weapon/12);
        p->combat_score += weight > UINT32_MAX-p->combat_score ? UINT32_MAX-p->combat_score : weight;
        p->rank = game_combat_rank(p->combat_score);
        cg_mission_kill(c->id);
    }
}
/* Damage consumes raw weapon damage, then applies the layer's multiplier.
   Overflow is converted back to raw damage before the hull multiplier. */
void cg_damage_typed(int target,float amount,game_damage_type type,l3_vec source,bool player,int subtarget) {
    if(!cg || !isfinite(amount) || amount<=0 || target>=GAME_CONTACTS) return;
    float sm=type<=DAMAGE_EXPLOSIVE && (int)type>=0?rules()->damage[type][0]:1;
    float hm=type<=DAMAGE_EXPLOSIVE && (int)type>=0?rules()->damage[type][1]:1;
    if(target<0) {
        game_player *p=&cg->v.player;
        cg_combat_player_ensure(p);
        if(p->docked || p->dead) return;
        cg->cruise.hit=5;
        l3_vec delta=l3_sub(source,p->position);bool front=l3_dot(delta,cg_forward())>=0;
        int side=front?0:1;float *shield=front?&p->shield_fore:&p->shield_aft;
        float resist=1-rules()->distributor[CD_RESIST]*(p->pips[0]-2);
        float incoming=amount*sm*resist, absorb=fminf(*shield,incoming);
        *shield-=absorb;
        float hull=amount*(1-absorb/incoming)*hm*(p->rank<3?.35f:1);
        if(*shield<=0 && p->shield_reboot[side]<=0) {
            p->shield_reboot[side]=rules()->defence[CF_REBOOT]/fmaxf(.1f,sys_rate(p));
            cg->v.combat_events|=COMBAT_SHIELD_DOWN;cg_combat_log("Shield ring down.");
        }
        p->hull=fmaxf(0,p->hull-hull);
        if(hull>0) {
            int module=(int)(cg_rand(&cg->rng)%GAME_MODULES);
            p->modules[module]=fmaxf(0,p->modules[module]-hull/game_player_ship()->hull*rules()->defence[CF_MODULE_DAMAGE]);
            if(front) p->canopy=fmaxf(0,p->canopy-hull/game_player_ship()->hull*rules()->defence[CF_CANOPY_DAMAGE]);
            if(p->modules[MODULE_POWER]<=0) p->hull=0;
        }
        int head=p->hit_head;
        p->hit_directions[head]=(game_hit_direction){l3_norm(delta),0};
        p->hit_head=(uint8_t)((head+1)%GAME_HIT_DIRECTIONS);
        if(p->hit_count<GAME_HIT_DIRECTIONS) p->hit_count++;
        cg_combat_status();return;
    }
    game_contact *c=&cg->v.contacts[target];
    if(!c->active) return;
    if(!cg->fighters[target].initialized || cg->fighters[target].id!=c->id) cg_combat_contact_init(target);
    cg_combat_contact *f=&cg->fighters[target];
    cg_encounter_hit(target,player);
    float incoming=amount*sm,absorb=fminf(c->shield,incoming);c->shield-=absorb;
    float hull=amount*(1-absorb/incoming)*hm;
    if(c->shield<=0 && f->reboot<=0) f->reboot=rules()->defence[CF_REBOOT];
    c->hull-=hull;
    if(hull>0 && subtarget>=0 && subtarget<GAME_MODULES) {
        f->modules[subtarget]=fmaxf(0,f->modules[subtarget]-hull/game_ship(c->ship_class)->hull*rules()->defence[CF_MODULE_DAMAGE]);
        if(f->modules[MODULE_POWER]<=0) c->hull=0;
        if(f->modules[MODULE_CARGO]<=0 && !f->spilled) {
            spill(c->position,(int)(cg_rand(&cg->rng)%GAME_GOODS));f->spilled=true;
        }
    }
    bool wanted=cg->encounter[target].kind==CE_SMUGGLERS && c->role==ROLE_TRADER;
    if(player && !c->hostile && !wanted) {
        if(c->role==ROLE_POLICE) cg->police_origin[target]=true;
        cg->provoked[target]=true;cg->v.player.legal=(uint8_t)fminf(255,cg->v.player.legal+1);
        c->hostile=true;
        if(!cg->police_origin[target] && !cg->encounter[target].kind) c->role=ROLE_PIRATE;
    }
    if(c->scanned) memcpy(c->modules,f->modules,sizeof(c->modules));
    if(c->hull<=0) cg_explode(c->position,target,player);
}
/* Environmental/legacy callers use untyped impact damage. Weapons always use
   cg_damage_typed, so old fore/aft collision semantics remain unchanged. */
void cg_damage(int target,float amount,l3_vec source,bool player) {
    cg_damage_typed(target,amount,(game_damage_type)-1,source,player,-1);
}
void cg_combat_status(void) {
    game_player *p=&cg->v.player;float max=game_player_ship()->hull;
    uint32_t alarms=0;
    if(cg->alarm_hull>max*.5f && p->hull<=max*.5f) alarms|=COMBAT_HULL_50;
    if(cg->alarm_hull>max*.25f && p->hull<=max*.25f) alarms|=COMBAT_HULL_25;
    if(cg->alarm_hull>max*.1f && p->hull<=max*.1f) alarms|=COMBAT_HULL_10;
    if(alarms) cg_combat_log("Hull alarm threshold crossed.");
    if(p->canopy<=0 && !(cg->v.combat_events&COMBAT_CANOPY) && p->oxygen>=rules()->defence[CF_OXYGEN]) {
        cg_combat_log("Canopy breached. Oxygen reserve.");cg->v.combat_events|=COMBAT_CANOPY;
    }
    cg->v.combat_events|=alarms;cg->alarm_hull=p->hull;heat_sync(p);
    if(p->hull<=0 && !p->dead) {
        p->dead=true;p->firing=p->firing2=false;cg->missile_pending=false;
        cg_message("Commander lost. Reload saved commander to resume.");
        cg_combat_log("Commander lost.");cg_explode(p->position,-1,false);
    }
}
static float ray_sphere(l3_vec o,l3_vec d,l3_vec c,float r) {
    l3_vec oc=l3_sub(o,c);float b=l3_dot(oc,d),q=l3_dot(oc,oc)-r*r,det=b*b-q;
    if(q<=0) return 0;
    if(det<0) return -1;
    float t=-b-sqrtf(det);return t<0?-b+sqrtf(det):t;
}
/* Both rays and swept projectiles use the same nearest-hit bounds. */
static int trace(l3_vec origin,l3_vec dir,float *range,int owner,int *rock) {
    int hit=-2;*rock=-1;
    if(owner>=0 && !cg->v.player.docked && !cg->v.player.dead) {
        float d=ray_sphere(origin,dir,cg->v.player.position,game_player_ship()->radius);
        if(d>=0 && d<=*range) {*range=d;hit=-1;}
    }
    for(int i=0;i<GAME_CONTACTS;i++) {
        game_contact *c=&cg->v.contacts[i];
        if(!c->active || i==owner || c->jump_out>0) continue;
        float d=ray_sphere(origin,dir,c->position,game_ship(c->ship_class)->radius);
        if(d>=0 && d<=*range) {*range=d;hit=i;}
    }
    for(int i=0;i<18;i++) if(cg->v.asteroids[i].active) {
        game_asteroid *a=&cg->v.asteroids[i];float d=ray_sphere(origin,dir,a->position,a->radius);
        if(d>=0 && d<=*range) {*range=d;*rock=i;hit=-2;}
    }
    return hit;
}
static void rock_damage(int rock,float damage) {
    if(rock<0) return;
    game_asteroid *a=&cg->v.asteroids[rock];a->hull-=damage;
    if(a->hull<=0) {a->active=false;cg_ores(a->position,cg->v.systems[cg->v.system].seed+(uint32_t)rock);}
}
static game_damage_type damage_kind(int kind) {
    return kind<=WEAPON_BEAM?DAMAGE_THERMAL:kind<=WEAPON_SLUG?DAMAGE_KINETIC:DAMAGE_EXPLOSIVE;
}
static int aim_module(int hit,int subtarget,uint32_t id) {
    return hit>=0 && cg->v.contacts[hit].id==id?subtarget:-1;
}
static bool ordnance(int kind,int owner,int target,l3_vec origin,l3_vec dir,float damage,int subtarget) {
    bool player=owner<0;const cg_weapon_rule *w=&rules()->weapons[kind];
    if(kind>=WEAPON_SEEKER) {
        for(int i=0;i<CG_MISSILES;i++) if(cg->missiles[i].life<=0) {
            cg->missiles[i]=(cg_missile){.p=origin,.v=l3_scale(dir,w->speed),.life=w->life,
                .target=target,.player=player,.kind=(uint8_t)kind,.damage=damage,
                .target_id=target>=0?cg->v.contacts[target].id:0,
                .owner_id=owner>=0?cg->v.contacts[owner].id:0,.owner=owner,.subtarget=subtarget};
            return true;
        }
    } else {
        for(int i=CG_MISSILES;i<GAME_PROJECTILES;i++) if(cg->v.projectiles[i].life<=0) {
            cg->v.projectiles[i]=(game_projectile){.position=origin,.velocity=l3_scale(dir,w->speed),
                .life=w->life,.kind=(uint8_t)kind,.player=player};
            cg->shots[i].damage=damage;cg->shots[i].owner=owner;cg->shots[i].subtarget=subtarget;
            cg->shots[i].id=owner>=0?cg->v.contacts[owner].id:0;
            cg->shots[i].target_id=target>=0?cg->v.contacts[target].id:0;
            return true;
        }
    }
    return false;
}
/* Historical one-shot API queues a seeker request while the selected contact
   is acquiring its two-second lock; no ammunition is consumed until launch. */
bool game_missile(void) {
    if(!cg || !flying() || !cg->v.player.missiles) return false;
    int t=cg->v.selected;
    if(t<0 || t>=GAME_CONTACTS || !cg->v.contacts[t].active) return false;
    if(cg->selected_slot!=t || cg->selected_id!=cg->v.contacts[t].id) {
        cg->selected_slot=t;cg->selected_id=cg->v.contacts[t].id;
        cg->v.subtarget=-1;cg->v.player.seeker_lock=0;
    }
    cg->missile_pending=true;return true;
}
bool game_ecm(void) {
    if(!cg || !flying() || !(cg->v.player.equipment&(1u<<EQ_ECM)) || cg->v.player.energy<20 ||
        cg->v.player.modules[MODULE_POWER]<=0) return false;
    cg->v.player.energy-=20;cg->ecm=1.5f;
    for(int i=0;i<CG_MISSILES;i++) if(cg_len(l3_sub(cg->missiles[i].p,cg->v.player.position))<700) {
        cg->missiles[i].life=0;cg->v.projectiles[i].life=0;
    }
    cg->missile_pending=false;cg_combat_log("ECM pulse. Missiles neutralized.");return true;
}
static bool weapon_fire(int kind,int owner,int target,l3_vec dir,float damage,int subtarget) {
    l3_vec pos=owner<0?cg->v.player.position:cg->v.contacts[owner].position;
    if(kind==WEAPON_RATTLE || (owner<0 && kind==WEAPON_SLUG) || kind>=WEAPON_SEEKER) {
        float radius=owner<0?game_player_ship()->radius:game_ship(cg->v.contacts[owner].ship_class)->radius;
        return ordnance(kind,owner,target,l3_add(pos,l3_scale(dir,radius+1)),dir,damage,subtarget);
    }
    float range=cg->rules.laser_range;int rock;
    int hit=trace(pos,dir,&range,owner,&rock);
    cg_beam_add(pos,l3_add(pos,l3_scale(dir,range)),owner<0);
    if(hit!=-2) cg_damage_typed(hit,damage,damage_kind(kind),pos,owner<0,
        aim_module(hit,subtarget,target>=0?cg->v.contacts[target].id:0));
    rock_damage(rock,damage);return true;
}
static bool selected_lock(float dt) {
    game_view *v=&cg->v;game_player *p=&v->player;int i=v->selected;
    uint32_t id=i>=0 && i<GAME_CONTACTS && v->contacts[i].active?v->contacts[i].id:0;
    if(i!=cg->selected_slot || id!=cg->selected_id) {
        p->seeker_lock=0;v->subtarget=-1;cg->missile_pending=false;
        for(int j=0;j<GAME_CONTACTS;j++) if(!v->contacts[j].scanned) v->contacts[j].scan_progress=0;
        cg->selected_id=id;cg->selected_slot=i;
    }
    if(!id) {p->seeker_lock=0;return false;}
    game_contact *c=&v->contacts[i];l3_vec delta=l3_sub(c->position,p->position);
    float range=cg_len(delta);
    bool visible=flying() && p->modules[MODULE_SENSORS]>0 && p->modules[MODULE_POWER]>0 &&
        range<=rules()->defence[CF_SCAN_RANGE]*p->modules[MODULE_SENSORS] &&
        game_poi_detectable(p->position,c->position) && !v->scanner_scrambled;
    if(visible) {
        c->scan_progress=fminf(1,c->scan_progress+dt/rules()->defence[CF_SCAN_TIME]);
        if(c->scan_progress>=.99999f && !c->scanned) {c->scan_progress=1;c->scanned=true;cg_combat_log("Contact scan complete.");}
    } else if(!c->scanned) c->scan_progress=0;
    bool lock=visible && range<=cg->rules.laser_range && l3_dot(l3_norm(delta),cg_forward())>.94f;
    p->seeker_lock=lock?fminf(1,p->seeker_lock+dt/rules()->defence[CF_LOCK_TIME]):0;
    if(p->seeker_lock>=.99999f) p->seeker_lock=1;
    return p->seeker_lock>=1;
}
/* Solve |relative + velocity*t| = projectile_speed*t; shots have no inherited velocity. */
static l3_vec lead_point(const game_contact *c,int kind) {
    l3_vec point=c->position;
    if(kind!=WEAPON_RATTLE && kind!=WEAPON_SLUG) return point;
    float speed=rules()->weapons[kind].speed;
    l3_vec r=l3_sub(point,cg->v.player.position), v=c->velocity;
    float a=l3_dot(v,v)-speed*speed, b=2*l3_dot(r,v), cc=l3_dot(r,r), t=-1;
    if(fabsf(a)<.001f) { if(fabsf(b)>.001f) t=-cc/b; }
    else {
        float det=b*b-4*a*cc;
        if(det>=0) {
            float root=sqrtf(det), t1=(-b-root)/(2*a), t2=(-b+root)/(2*a);
            if(t1>0) t=t1;
            if(t2>0 && (t<0 || t2<t)) t=t2;
        }
    }
    return t>0 && isfinite(t)?l3_add(point,l3_scale(v,t)):point;
}
static l3_vec player_aim(int kind,bool observe) {
    game_view *v=&cg->v;l3_vec nose=cg_forward(),point=l3v(0,0,0),dir=nose;
    bool valid=false,assisted=false;
    int target=v->selected;
    if(target>=0 && target<GAME_CONTACTS && v->contacts[target].active) {
        point=lead_point(&v->contacts[target],kind);
        l3_vec delta=l3_sub(point,v->player.position);
        float distance=cg_len(delta);
        valid=distance>.001f && l3_dot(l3_sub(v->contacts[target].position,v->player.position),nose)>0 &&
            l3_dot(delta,nose)>0;
        if(valid && kind<=WEAPON_SLUG && cg->aim_assist>0) {
            dir=l3_scale(delta,1/distance);
            assisted=l3_dot(dir,nose)>=cosf(cg->rules.aim_assist_deg[cg->aim_assist]*CG_RAD);
        }
    }
    if(observe) {v->aim_point=point;v->aim_valid=valid;v->aim_assisted=assisted;}
    return assisted?dir:nose;
}
void cg_aim_observe(void) {
    game_player *p=&cg->v.player;
    int kind=WEAPON_PULSE;
    /* Prefer the held fire group, then the first fitted non-missile mount. */
    for(int pass=0;pass<2;pass++) {
        for(int i=0;i<p->mounts;i++) {
            game_mount *m=&p->weapons[i];
            if(m->kind>WEAPON_SLUG || (!pass && !(m->group?p->firing2:p->firing))) continue;
            player_aim(m->kind,true);return;
        }
    }
    player_aim(kind,true);
}
/* Slew in world space by a bounded angle; return to the nose without a target. */
static l3_vec slew(l3_vec from,l3_vec to,float step) {
    float angle=acosf(cg_clamp(l3_dot(from,to),-1,1));
    if(angle<=step || angle<.0001f) return to;
    l3_vec side=l3_sub(to,l3_scale(from,cosf(angle)));
    if(cg_len(side)<.0001f) side=l3_cross(from,fabsf(from.y)<.9f?l3v(0,1,0):l3v(1,0,0));
    return l3_norm(l3_add(l3_scale(from,cosf(step)),l3_scale(l3_norm(side),sinf(step))));
}
static l3_vec mount_aim(int slot,float dt,int *target) {
    game_view *v=&cg->v;game_player *p=&v->player;game_mount *m=&p->weapons[slot];
    l3_vec nose=cg_forward(),desired=nose;bool valid=false;
    v->turret_on[slot]=false;
    if(m->mode==MOUNT_FIXED) {v->turret_aim[slot]=nose;return player_aim(m->kind,false);}
    float cone=(m->mode==MOUNT_GIMBAL?cg->rules.gimbal_deg:cg->rules.turret_deg)*CG_RAD;
    int t=v->selected;
    bool selected=t>=0 && t<GAME_CONTACTS && v->contacts[t].active;
    if(!selected && m->mode==MOUNT_TURRET) {
        float nearest=INFINITY;t=-1;
        for(int i=0;i<GAME_CONTACTS;i++) {
            game_contact *c=&v->contacts[i];if(!c->active || !c->hostile || c->jump_out>0) continue;
            l3_vec delta=l3_sub(lead_point(c,m->kind),p->position);float d=cg_len(delta);
            if(d>.001f && d<nearest && l3_dot(l3_scale(delta,1/d),nose)>=cosf(cone)) {t=i;nearest=d;}
        }
    }
    if(t>=0 && t<GAME_CONTACTS && v->contacts[t].active && v->contacts[t].jump_out<=0) {
        l3_vec delta=l3_sub(lead_point(&v->contacts[t],m->kind),p->position);float d=cg_len(delta);
        if(d>.001f && l3_dot(l3_scale(delta,1/d),nose)>=cosf(cone)) {
            desired=l3_scale(delta,1/d);valid=true;*target=t;
        }
    }
    if(m->mode==MOUNT_GIMBAL) {v->turret_aim[slot]=desired;return desired;}
    l3_vec previous=v->turret_aim[slot];
    if(cg_len(previous)<.5f) previous=nose;
    /* A ship turn cannot leave a turret pointing beyond its mechanical stop. */
    if(l3_dot(previous,nose)<cosf(cone)) previous=slew(nose,previous,cone);
    l3_vec dir=slew(previous,desired,cg->rules.turret_rate_dps*CG_RAD*dt);
    v->turret_aim[slot]=dir;v->turret_on[slot]=valid;
    return dir;
}
static void player_weapons(float dt,bool locked) {
    game_player *p=&cg->v.player;int target=cg->v.selected;
    if(p->cruising) {p->firing=p->firing2=false;cg->missile_pending=false;}
    bool ready=flying() && p->modules[MODULE_WEAPONS]>0 && p->modules[MODULE_POWER]>0;
    for(int i=0;i<GAME_MOUNTS;i++) {
        game_mount *m=&p->weapons[i];cg->mount_cooldown[i]=fmaxf(0,cg->mount_cooldown[i]-dt);
        m->firing=false;m->heat=fmaxf(0,m->heat-dt*cg->rules.cooling*.01f);
        int aim_target=target;
        l3_vec direction=m->kind<WEAPON_COUNT && i<p->mounts?mount_aim(i,dt,&aim_target):cg_forward();
        if(i>=p->mounts || m->kind>=WEAPON_COUNT) cg->v.turret_on[i]=false;
        bool held=m->group==0?p->firing:p->firing2;
        if(i>=p->mounts || m->kind>=WEAPON_COUNT || !held || !ready) {m->charge=0;continue;}
            const cg_weapon_rule *w=&rules()->weapons[m->kind];
        if(m->kind==WEAPON_SLUG) m->charge=fminf(1,m->charge+dt); /* exactly one second */
        else if(m->kind==WEAPON_SEEKER) m->charge=p->seeker_lock;
        if(cg->mount_cooldown[i]>0 || (m->kind==WEAPON_SLUG && m->charge<1) ||
            (m->kind==WEAPON_SEEKER && !locked) || p->weapon_capacitor<w->capacitor ||
            p->energy<w->capacitor || p->heat>=150 || (m->kind==WEAPON_RATTLE && !m->ammo) ||
            (m->kind>=WEAPON_SEEKER && !p->missiles)) continue;
        /* Damaged weapons omit a seeded fraction of attempted shots. */
        if(p->modules[MODULE_WEAPONS]<1 && cg_unit(&cg->rng)>p->modules[MODULE_WEAPONS]) {
            cg->mount_cooldown[i]=w->interval;continue;
        }
        if(m->mode==MOUNT_TURRET && !cg->v.turret_on[i]) continue;
        if(!weapon_fire(m->kind,-1,aim_target,direction,w->damage*cg->rules.mount_damage[m->mode],
            aim_target==target?cg->v.subtarget:-1)) continue;
        cg_weapons_hot();
        p->weapon_capacitor-=w->capacitor;p->energy-=w->capacitor;p->heat+=w->heat;
        m->heat=cg_clamp(m->heat+w->heat*.01f,0,1.5f);m->firing=true;
        cg->mount_cooldown[i]=w->interval;
        if(m->kind==WEAPON_RATTLE) m->ammo--;
        if(m->kind>=WEAPON_SEEKER) p->missiles--;
        if(m->kind==WEAPON_SLUG) m->charge=0;
    }
    if(cg->missile_pending && ready && locked && p->missiles) {
        const cg_weapon_rule *w=&rules()->weapons[WEAPON_SEEKER];
        if(p->weapon_capacitor>=w->capacitor && p->energy>=w->capacitor && p->heat<150 &&
            weapon_fire(WEAPON_SEEKER,-1,target,cg_forward(),w->damage,cg->v.subtarget)) {
            cg_weapons_hot();
            p->weapon_capacitor-=w->capacitor;p->energy-=w->capacitor;p->heat+=w->heat;
            p->missiles--;cg->missile_pending=false;cg_combat_log("SEEKER away.");
        }
    }
}
static void npc_weapons(int i,float dt) {
    game_contact *c=&cg->v.contacts[i];cg_combat_contact *f=&cg->fighters[i];
    int target=cg_encounter_enemy(i);
    bool ready=flying() && c->jump_out<=0 && target!=-2 &&
        c->hull>=game_ship(c->ship_class)->hull*.25f && f->modules[MODULE_WEAPONS]>0 && f->modules[MODULE_POWER]>0;
    l3_vec dest=target<0?cg->v.player.position:cg->v.contacts[target].position;
    l3_vec delta=l3_sub(dest,c->position);float distance=cg_len(delta);
    ready=ready && distance<420 && game_poi_detectable(c->position,dest);
    uint32_t id=target<0?UINT32_MAX:cg->v.contacts[target].id;
    if(!ready || cg->npc_lock_id[i]!=id || (target<0 && cg->v.player.chaff_left>0) || f->modules[MODULE_SENSORS]<=0)
        cg->npc_lock[i]=0;
    else cg->npc_lock[i]=fminf(1,cg->npc_lock[i]+dt/rules()->defence[CF_LOCK_TIME]);
    cg->npc_lock_id[i]=id;
    for(int j=0;j<f->mounts;j++) {
        game_mount *m=&f->weapons[j];
        if(m->kind>=WEAPON_COUNT) {m->firing=false;m->charge=0;continue;}
        const cg_weapon_rule *w=&rules()->weapons[m->kind];
        f->cooldown[j]=fmaxf(0,f->cooldown[j]-dt);m->firing=false;m->heat=fmaxf(0,m->heat-dt*.22f);
        if(!ready) {m->charge=0;continue;}
        if(m->kind==WEAPON_SLUG) m->charge=fminf(1,m->charge+dt);
        if(m->kind==WEAPON_SEEKER) m->charge=cg->npc_lock[i];
        if(f->cooldown[j]>0 || (m->kind==WEAPON_SLUG && m->charge<1) ||
            (m->kind==WEAPON_SEEKER && m->charge<1) ||
            ((m->kind==WEAPON_RATTLE || m->kind>=WEAPON_SEEKER) && !m->ammo) ||
            f->capacitor<w->capacitor || f->heat>=150) continue;
        float accuracy=target<0 && cg->v.player.rank<3?rules()->defence[CF_EARLY_ACCURACY]:rules()->defence[CF_ACCURACY];
        accuracy*=f->modules[MODULE_WEAPONS]*f->modules[MODULE_SENSORS];
        if(target<0 && cg->v.player.chaff_left>0) accuracy*=.1f;
        l3_vec dir=l3_norm(delta);
        if(cg_unit(&cg->rng)>accuracy && m->kind!=WEAPON_SEEKER) {
            l3_vec side=l3_norm(l3_cross(dir,fabsf(dir.y)<.9f?l3v(0,1,0):l3v(1,0,0)));
            dir=l3_norm(l3_add(delta,l3_scale(side,40+cg_unit(&cg->rng)*60)));
        }
        float damage=cg->rules.npc_damage*w->damage/(rules()->weapons[WEAPON_PULSE].damage/.12f*1.8f)/f->mounts;
        if(c->ship_class==SH_DRONE) damage*=2;
        if(!weapon_fire(m->kind,i,target,dir,damage,-1)) continue;
        f->capacitor-=w->capacitor;f->heat+=w->heat;m->heat=cg_clamp(m->heat+w->heat*.01f,0,1.5f);
        m->firing=true;f->cooldown[j]=w->interval;
        if(m->kind==WEAPON_RATTLE || m->kind>=WEAPON_SEEKER) m->ammo--;
        if(m->kind==WEAPON_SLUG) m->charge=0;
        if(target<0) f->last_attack=cg->v.clock;
    }
    memcpy(c->weapons,f->weapons,sizeof(c->weapons));
}
static void projectiles(float dt) {
    game_player *p=&cg->v.player;cg->v.missile_warning=false;cg->v.missile_range=0;
    for(int i=0;i<CG_MISSILES;i++) {
        cg_missile *m=&cg->missiles[i];game_projectile *view=&cg->v.projectiles[i];
        if(m->life<=0) {view->life=0;continue;}
        bool seeker=m->kind!=WEAPON_DUMBFIRE;
        if(m->target>=0 && (!cg->v.contacts[m->target].active ||
            (m->target_id && cg->v.contacts[m->target].id!=m->target_id))) m->lost_lock=true;
        if(m->owner>=0 && m->owner_id && cg->v.contacts[m->owner].id!=m->owner_id) {
            m->life=0;view->life=0;continue;
        }
        if(seeker && !m->lost_lock) {
            l3_vec dest=m->target>=0?cg->v.contacts[m->target].position:p->position;
            l3_vec delta=l3_sub(dest,m->p);float speed=rules()->weapons[WEAPON_SEEKER].speed;
            m->v=l3_add(m->v,l3_scale(l3_sub(l3_scale(l3_norm(delta),speed),m->v),fminf(1,dt*4)));
        }
        float travel=cg_len(m->v)*fminf(dt,m->life);int rock;
        int hit=trace(m->p,l3_norm(m->v),&travel,m->player?-1:m->owner,&rock);
        l3_vec start=m->p;m->p=l3_add(m->p,l3_scale(l3_norm(m->v),travel));m->life=fmaxf(0,m->life-dt);
        if(hit!=-2 || rock>=0) {
            if(hit!=-2) cg_damage_typed(hit,m->damage>0?m->damage:120,DAMAGE_EXPLOSIVE,start,m->player,
                aim_module(hit,m->subtarget,m->target_id));
            rock_damage(rock,m->damage);m->life=0;
        }
        *view=(game_projectile){.position=m->p,.velocity=m->v,.life=m->life,
            .kind=(uint8_t)(seeker?WEAPON_SEEKER:WEAPON_DUMBFIRE),.player=m->player};
        if(m->life>0 && seeker && !m->player && m->target==-1 && !m->lost_lock && !p->dead) {
            float range=cg_len(l3_sub(m->p,p->position));
            if(!cg->v.missile_warning || range<cg->v.missile_range) {cg->v.missile_warning=true;cg->v.missile_range=range;}
        }
    }
    for(int i=CG_MISSILES;i<GAME_PROJECTILES;i++) {
        game_projectile *s=&cg->v.projectiles[i];
        if(s->life<=0) continue;
        int owner=cg->shots[i].owner;
        if(owner>=0 && cg->v.contacts[owner].id!=cg->shots[i].id) {s->life=0;continue;}
        float travel=cg_len(s->velocity)*fminf(dt,s->life);int rock;
        int hit=trace(s->position,l3_norm(s->velocity),&travel,owner,&rock);
        l3_vec start=s->position;s->position=l3_add(s->position,l3_scale(l3_norm(s->velocity),travel));
        s->life=fmaxf(0,s->life-dt);
        if(hit!=-2 || rock>=0) {
            if(hit!=-2) cg_damage_typed(hit,cg->shots[i].damage,DAMAGE_KINETIC,start,s->player,
                aim_module(hit,cg->shots[i].subtarget,cg->shots[i].target_id));
            rock_damage(rock,cg->shots[i].damage);s->life=0;
        }
    }
}
static void player_defence(float dt) {
    game_player *p=&cg->v.player;
    p->energy=fminf(100,p->energy+((p->equipment&(1u<<EQ_ENERGY))?14:7)*dt*p->modules[MODULE_POWER]);
    float recharge=rules()->distributor[CD_WEP_BASE]+p->pips[2]*rules()->distributor[CD_WEP_STEP];
    p->weapon_capacitor=fminf(100,p->weapon_capacitor+dt*recharge*p->modules[MODULE_POWER]);
    p->heat=fmaxf(0,p->heat-cg->rules.cooling*dt);
    float rate=sys_rate(p)*p->modules[MODULE_SHIELDS]*p->modules[MODULE_POWER];
    float eng=rules()->distributor[CD_ENG_BASE]+p->pips[1]*rules()->distributor[CD_ENG_STEP];
    p->boost_cooldown=fmaxf(0,p->boost_cooldown-dt*eng*cg_drive_scale(p));
    p->boost_left=fmaxf(0,p->boost_left-dt);p->chaff_left=fmaxf(0,p->chaff_left-dt);
    for(int i=0;i<p->hit_count;i++) p->hit_directions[i].age=fminf(60,p->hit_directions[i].age+dt);
    for(int i=0;i<2;i++) {
        float *s=i?&p->shield_aft:&p->shield_fore;
        if(p->modules[MODULE_SHIELDS]<=0 || p->modules[MODULE_POWER]<=0) {*s=0;continue;}
        if(*s<=0 && p->shield_reboot[i]<=0) p->shield_reboot[i]=rules()->defence[CF_REBOOT]/fmaxf(.1f,sys_rate(p));
        if(p->shield_reboot[i]>0) {
            p->shield_reboot[i]=fmaxf(0,p->shield_reboot[i]-dt*rate/fmaxf(.1f,sys_rate(p)));
            if(p->shield_reboot[i]<=0) {*s=shield_max()*.5f;cg->v.combat_events|=COMBAT_SHIELD_UP;cg_combat_log("Shield ring rebooted.");}
        } else if(p->energy>25) *s=fminf(shield_max(),*s+dt*2*rate*(p->rank<3?rules()->defence[CF_EARLY_REGEN]:1)*(cg->v.in_nebula?.25f:1));
    }
    if(!p->docked && !p->dead) {
        heat_sync(p);
        if(p->heat_level>1) p->hull=fmaxf(0,p->hull-dt*rules()->defence[CF_HEAT_DAMAGE]*(p->heat_level-1));
        if(p->canopy<=0) p->oxygen=fmaxf(0,p->oxygen-dt/(.25f+.75f*p->modules[MODULE_LIFE]));
        if(p->canopy<=0 && p->oxygen<=0) p->hull=0;
        if(p->modules[MODULE_POWER]<=0) p->hull=0;
        if(p->modules[MODULE_CARGO]<=0 && !cg->cargo_spilled) {
            for(int g=0;g<GAME_GOODS;g++) if(p->cargo[g]) {p->cargo[g]--;spill(p->position,g);break;}
            cg->cargo_spilled=true;cg_combat_log("Cargo hatch lost. Freight spilled.");
        }
    }
}
void cg_combat(float dt) {
    if(!cg || !isfinite(dt) || dt<=0) return;
    cg_combat_player_ensure(&cg->v.player);
    cg->ecm=fmaxf(0,cg->ecm-dt);
    for(int i=0;i<CG_BEAMS;i++) cg->beams[i].life=fmaxf(0,cg->beams[i].life-dt);
    for(int i=0;i<CG_DEBRIS;i++) if(cg->debris[i].life>0) {
        cg->debris[i].life=fmaxf(0,cg->debris[i].life-dt);
        cg->debris[i].p=l3_add(cg->debris[i].p,l3_scale(cg->debris[i].v,dt));
    }
    for(int i=0;i<CG_CANS;i++) cg->cans[i].life=fmaxf(0,cg->cans[i].life-dt);
    cg->v.player.weapons_hot=fmaxf(0,cg->v.player.weapons_hot-dt);
    if(cg->v.player.weapons_hot<.00001f) cg->v.player.weapons_hot=0;
    if(cg->v.player.firing || cg->v.player.firing2) cg_weapons_hot();
    player_defence(dt);
    for(int i=0;i<GAME_CONTACTS;i++) {
        game_contact *c=&cg->v.contacts[i];if(!c->active) continue;
        if(!cg->fighters[i].initialized || cg->fighters[i].id!=c->id) cg_combat_contact_init(i);
    }
    bool lock=selected_lock(dt);cg_aim_observe();player_weapons(dt,lock);
    for(int i=0;i<GAME_CONTACTS;i++) {
        game_contact *c=&cg->v.contacts[i];if(!c->active) continue;
        cg_combat_contact *f=&cg->fighters[i];
        f->capacitor=fminf(100,f->capacitor+dt*7*f->modules[MODULE_POWER]);f->heat=fmaxf(0,f->heat-cg->rules.cooling*dt);
        if(f->modules[MODULE_SHIELDS]<=0) c->shield=0;
        else {
            if(c->shield<=0 && f->reboot<=0) f->reboot=rules()->defence[CF_REBOOT];
            if(f->reboot>0) {
            f->reboot=fmaxf(0,f->reboot-dt);
            if(f->reboot<=0) c->shield=game_ship(c->ship_class)->shield*.5f;
            } else if(c->shield>0) c->shield=fminf(game_ship(c->ship_class)->shield,c->shield+dt*2);
        }
        c->shield_reboot=f->reboot;
        npc_weapons(i,dt);c->attacking_player=cg->v.clock-f->last_attack<1.5f;
        c->threat=(uint8_t)(c->attacking_player?3:c->hostile?2:cg_encounter_enemy(i)!=-2?1:0);
        if(c->scanned) {
            c->wanted=(c->hostile && !cg->provoked[i] && !cg->police_origin[i]) ||
                (cg->encounter[i].kind==CE_SMUGGLERS && c->role==ROLE_TRADER);
            c->bounty=c->wanted?(uint32_t)(game_ship(c->ship_class)->weapon*8+game_ship(c->ship_class)->hull):0;
            snprintf(c->rank_text,sizeof(c->rank_text),"%s",game_rank((int)(cg_hash(c->id)%9)));
            memcpy(c->modules,f->modules,sizeof(c->modules));
        }
    }
    projectiles(dt);cg_combat_status();
}
