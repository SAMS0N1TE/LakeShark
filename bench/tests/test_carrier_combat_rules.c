/* Combat rules through the real app cache; seeded, bounded and screen-free. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
#include <time.h>
static unsigned checks,allocations;
#define CHECK(x) do { checks++; if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);assert(x);} } while(0)
#define NEAR(a,b) CHECK(fabsf((a)-(b))<.002f)
void cg_render_reset(void) {}
void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);void *__real_realloc(void *,size_t);
void *__wrap_malloc(size_t n) {allocations++;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s) {allocations++;return __real_calloc(n,s);}
void *__wrap_realloc(void *p,size_t n) {allocations++;return __real_realloc(p,n);}
static game_player *player(void) {return &cg->v.player;}
static void reset(void) {
    game_player *p=player();memset(p,0,sizeof(*p));cg_ship_fit(p,SH_MAKO);p->lasers=1;
    p->equipment=(1u<<EQ_LASER)|(1u<<EQ_DOCKING);p->credits=1000000;p->energy=100;
    p->hull=game_player_ship()->hull;p->shield_fore=p->shield_aft=100;
    p->attitude=(l3_quat){0,0,0,1};p->missiles=4;p->fuel=7;
    cg_combat_player_init(p);cg_combat_reset();cg_cruise_reset();game_aim_assist(0);
    memset(cg->v.contacts,0,sizeof(cg->v.contacts));memset(cg->fighters,0,sizeof(cg->fighters));
    memset(cg->encounter,0,sizeof(cg->encounter));memset(cg->mapped,0,sizeof(cg->mapped));
    memset(cg->v.asteroids,0,sizeof(cg->v.asteroids));memset(cg->cans,0,sizeof(cg->cans));
    cg->v.contents.planet_count=0;cg->v.contents.station=l3v(100000,0,0);
    cg->v.contents.star=l3v(0,100000,0);cg->v.contents.star_radius=100;
    cg->v.poi_count=0;cg->v.scanner_scrambled=false;cg->v.in_nebula=false;
    cg->v.selected=-1;cg->v.combat_events=0;cg->v.combat_log_count=0;cg->v.clock=0;
    cg->v.jump=JUMP_IDLE;cg->v.traffic_mode=1;
    for(int i=0;i<GAME_CONTACTS;i++) cg->encounter_enemy[i]=-2;
}
static game_contact *contact(int i,game_role role,float z) {
    cg_spawn(i,100+i,SH_MAKO,role,l3v(0,0,z));return &cg->v.contacts[i];
}
static void tick(int n) {for(int i=0;i<n;i++) {cg->v.clock+=.04f;cg_combat(.04f);}}
static void mount(int slot,int kind,int group) {
    player()->weapons[slot]=(game_mount){.kind=(uint8_t)kind,.group=(uint8_t)group,
        .ammo=cg->rules.combat.weapons[kind].ammo};
}
static void reverse_rules(void) {
    reset();game_player *p=player();NEAR(game_reverse_max(),.35f);
    game_throttle(-10);NEAR(p->throttle,-.35f);
    p->boost_left=100;p->cruise_factor=25;
    for(int i=0;i<250;i++) cg_flight(.04f);
    CHECK(p->position.z>0 && p->velocity.z>0 && p->reversing);
    NEAR(p->speed,game_player_ship()->speed*.35f);
    CHECK(!game_cruise(true) && !p->cruising);NEAR(p->throttle,0);
    CHECK(game_cruise(true));game_throttle(-.2f);cg_flight(.04f);CHECK(!p->cruising);
    game_throttle(10);NEAR(p->throttle,1);
    /* Speed remains a magnitude while backing away from the station approach. */
    reset();p->position=l3v(0,0,-100);cg->v.contents.station=l3v(0,0,0);
    p->attitude=l3_quat_euler(180,0,0);game_throttle(-1);
    for(int i=0;i<100;i++) cg_flight(.04f);
    CHECK(p->position.z < -100 && p->speed>0 && p->reversing && !p->docked);
    CHECK(game_save());game_throttle(0);CHECK(game_load());NEAR(p->throttle,-.35f);
}
static void turret_rules(void);
static void aim_rules(void) {
    turret_rules();
    for(int kind=WEAPON_PULSE;kind<=WEAPON_SLUG;kind++) {
        for(int level=0;level<=2;level++) {
            reset();float degrees=level==2?10:3;
            game_contact *c=contact(0,ROLE_DERELICT,-500);
            c->position.x=500*tanf(degrees*CG_RAD);cg->v.selected=0;c->shield=0;
            mount(0,kind,0);game_aim_assist(level);tick(1);
            CHECK(cg->v.aim_valid && cg->v.aim_assisted==(level>0));
            float hull=c->hull;game_fire(true);tick(kind==WEAPON_SLUG?25:1);
            game_fire(false);tick(25);
            CHECK((c->hull<hull)==(level>0));
        }
    }
    reset();game_contact *c=contact(0,ROLE_DERELICT,500);cg->v.selected=0;
    game_aim_assist(2);game_fire(true);tick(1);CHECK(!cg->v.aim_valid && !cg->v.aim_assisted);
    reset();c=contact(0,ROLE_DERELICT,-300);c->velocity=l3v(30,0,0);cg->v.selected=0;
    mount(0,WEAPON_RATTLE,0);game_aim_assist(1);tick(1);
    CHECK(cg->v.aim_valid && cg->v.aim_assisted && cg->v.aim_point.x>c->position.x);
    l3_vec delta=l3_sub(cg->v.aim_point,player()->position);
    float t=cg_len(delta)/cg->rules.combat.weapons[WEAPON_RATTLE].speed;
    NEAR(cg->v.aim_point.x,c->position.x+c->velocity.x*t);
    game_fire(true);tick(1);CHECK(cg->v.projectiles[CG_MISSILES].velocity.x>0);
    /* Faster receding targets have no intercept; use current position. */
    c->velocity=l3v(0,0,-2000);game_fire(false);tick(1);NEAR(cg->v.aim_point.z,c->position.z);
    c->position.x=100;c->velocity=l3v(0,0,0);tick(1);CHECK(!cg->v.aim_assisted);
    cg->v.selected=-1;tick(1);CHECK(!cg->v.aim_valid && !cg->v.aim_assisted);
}
static void hot_rules(void) {
    for(int group=0;group<2;group++) {
        reset();game_player *p=player();
        if(cg->v.system==1) cg->v.system=0;
        cg->v.systems[1]=cg->v.systems[cg->v.system];
        CHECK(game_plot_jump(1));
        if(group) game_fire2(true);else game_fire(true);
        CHECK(!game_cruise(true));CHECK(!strcmp(game_drive_refusal(),"Weapons hot"));
        game_context();CHECK(cg->v.jump==JUMP_IDLE);tick(75);
        CHECK(!game_cruise(true));game_context();CHECK(cg->v.jump==JUMP_IDLE);
        if(group) game_fire2(false);else game_fire(false);
        tick(49);CHECK(p->weapons_hot>0 && !game_cruise(true));
        game_context();CHECK(cg->v.jump==JUMP_IDLE);
        tick(1);CHECK(!game_drive_refusal());CHECK(game_cruise(true));game_cruise(false);
        l3_vec direction;CHECK(game_jump_direction(&direction));
        p->attitude=l3_quat_euler(atan2f(-direction.x,-direction.z)/CG_RAD,
            asinf(cg_clamp(direction.y,-1,1))/CG_RAD,0);
        game_context();CHECK(cg->v.jump==JUMP_CHARGE);
        if(group) game_fire2(true);else game_fire(true);
        CHECK(cg->v.jump==JUMP_IDLE && cg->v.jump_progress==0);
        if(group) game_fire2(false);else game_fire(false);
        tick(51);CHECK(game_cruise(true));p->cruise_factor=20;p->velocity=l3v(0,0,-2000);
        if(group) game_fire2(true);else game_fire(true);
        CHECK(!p->cruising && cg->cruise.bleed>0);NEAR(cg->cruise.bleed_factor,20);
        cg_flight(.04f);CHECK(p->cruise_factor>1 && p->cruise_factor<20);
    }
    /* A successful queued missile also starts the cooldown without a held trigger. */
    reset();contact(0,ROLE_DERELICT,-100);cg->v.selected=0;tick(51);
    CHECK(game_missile());tick(1);CHECK(player()->weapons_hot>1.9f);
}
static void turret_rules(void) {
    for(int mode=MOUNT_FIXED;mode<=MOUNT_TURRET;mode++) {
        for(int angle=8;angle<=180;angle=angle==8?20:angle==20?60:180) {
            reset();game_player *p=player();game_contact *c=contact(0,ROLE_DERELICT,-300);
            c->position=l3v(300*sinf(angle*CG_RAD),0,-300*cosf(angle*CG_RAD));c->shield=0;
            cg->v.selected=0;p->weapons[0].mode=(uint8_t)mode;float hull=c->hull;
            if(mode==MOUNT_GIMBAL) game_aim_assist(2);
            tick(25);game_fire(true);tick(1);game_fire(false);
            bool hit=mode==MOUNT_TURRET?angle<90:mode==MOUNT_GIMBAL?angle==8:false;
            CHECK((c->hull<hull)==hit);
            if(hit) NEAR(hull-c->hull,cg->rules.combat.weapons[0].damage*cg->rules.mount_damage[mode]*.8f);
            if(angle==180) break;
        }
    }
    reset();game_player *p=player();game_contact *c=contact(0,ROLE_DERELICT,-150);
    c->hostile=true;c->shield=0;c->position.x=260;p->weapons[0].mode=MOUNT_TURRET;
    tick(1);CHECK(cg->v.turret_on[0]);CHECK(l3_dot(cg->v.turret_aim[0],cg_forward())>cosf(5*CG_RAD));
    tick(20);float hull=c->hull;tick(2);NEAR(c->hull,hull); /* tracking never auto-fires */
    game_fire(true);tick(1);CHECK(c->hull<hull);game_fire(false);
    l3_vec before=cg->v.turret_aim[0];c->position.x=-260;tick(1);
    CHECK(acosf(cg_clamp(l3_dot(before,cg->v.turret_aim[0]),-1,1))/CG_RAD<4.81f);
    CHECK(cg->v.turret_aim[0].x>0); /* crossing target lags */
    /* Secondary turrets still require their own trigger. Selection beats proximity. */
    reset();p=player();c=contact(0,ROLE_DERELICT,-150);c->position.x=260;c->shield=0;
    game_contact *other=contact(1,ROLE_DERELICT,-100);other->hostile=true;other->shield=0;
    cg->v.selected=0;p->weapons[0].mode=MOUNT_TURRET;p->weapons[0].group=1;
    tick(20);hull=c->hull;float other_hull=other->hull;
    game_fire(true);tick(1);NEAR(c->hull,hull);game_fire(false);
    game_fire2(true);tick(1);CHECK(c->hull<hull);NEAR(other->hull,other_hull);game_fire2(false);
    /* An explicitly selected target behind prevents fallback to a hostile ahead. */
    c->position=l3v(0,0,100);hull=other->hull;tick(20);game_fire2(true);tick(1);
    NEAR(other->hull,hull);CHECK(!cg->v.turret_on[0]);
    reset();p=player();CHECK(!game_mount_mode(0,MOUNT_TURRET));p->docked=true;
    unsigned cash=p->credits;CHECK(game_mount_mode(0,MOUNT_GIMBAL));CHECK(p->credits==cash-3000);
    game_equipment_offer offer;CHECK(game_mount_info(0,&offer));CHECK(offer.mode==MOUNT_GIMBAL && offer.mode_price==8000);
    CHECK(game_mount_mode(0,MOUNT_TURRET));CHECK(p->credits==cash-9000);
    p->credits=0;CHECK(game_mount_mode(0,MOUNT_FIXED));CHECK(p->credits==9000);
    p->credits=1;CHECK(!game_mount_mode(0,MOUNT_TURRET) && p->weapons[0].mode==MOUNT_FIXED);
    p->credits=100000;CHECK(game_mount_mode(0,MOUNT_TURRET));CHECK(game_save());
    p->weapons[0].mode=MOUNT_FIXED;CHECK(game_load());CHECK(p->weapons[0].mode==MOUNT_TURRET);
}
static void damage(void) {
    for(int type=0;type<3;type++) {
        reset();game_contact *c=contact(0,ROLE_PIRATE,-100);float max=c->hull;
        cg_damage_typed(0,10,(game_damage_type)type,player()->position,true,-1);
        NEAR(c->shield,100-10*cg->rules.combat.damage[type][0]);NEAR(c->hull,max);
        c->shield=0;cg_damage_typed(0,10,(game_damage_type)type,player()->position,true,-1);
        NEAR(c->hull,max-10*cg->rules.combat.damage[type][1]);
        c->hull=max;c->shield=5*cg->rules.combat.damage[type][0];
        cg_damage_typed(0,10,(game_damage_type)type,player()->position,true,-1);
        NEAR(c->hull,max-5*cg->rules.combat.damage[type][1]);
    }
}
static void weapons(void) {
    hot_rules();
    reset();game_contact *c=contact(0,ROLE_DERELICT,-180);c->shield=0;float hull=c->hull;
    mount(0,WEAPON_RATTLE,0);game_fire(true);tick(1);CHECK(c->hull==hull);
    CHECK(player()->weapons[0].ammo==399 && player()->weapons[0].firing);
    CHECK(cg->v.projectiles[CG_MISSILES].life>0 && cg->v.projectiles[CG_MISSILES].velocity.z<0);
    game_fire(false);tick(10);CHECK(c->hull<hull);
    reset();c=contact(0,ROLE_DERELICT,-100);c->shield=0;hull=c->hull;
    mount(0,WEAPON_SLUG,0);game_fire(true);tick(24);NEAR(c->hull,hull);
    CHECK(player()->weapons[0].charge>.95f);tick(1);CHECK(player()->weapons[0].charge==0);game_fire(false);tick(3);CHECK(c->hull<hull);
    reset();c=contact(0,ROLE_DERELICT,-100);mount(0,WEAPON_BEAM,0);game_fire(true);
    float cap=player()->weapon_capacitor;tick(3);CHECK(c->shield<100 && player()->heat>0 && player()->weapon_capacitor<cap);
    reset();c=contact(0,ROLE_DERELICT,-250);cg->v.selected=0;mount(0,WEAPON_SEEKER,1);
    game_fire2(true);tick(49);CHECK(player()->missiles==4 && player()->seeker_lock<1);
    tick(1);CHECK(player()->missiles==3 && cg->missiles[0].life>0);
    reset();contact(0,ROLE_DERELICT,-200);mount(0,WEAPON_DUMBFIRE,1);game_fire2(true);tick(1);
    CHECK(player()->missiles==3 && cg->missiles[0].kind==WEAPON_DUMBFIRE);
    reset();contact(0,ROLE_DERELICT,-200);cg->v.selected=0;CHECK(game_missile());tick(49);
    CHECK(player()->missiles==4);tick(1);CHECK(player()->missiles==3);
    reset();mount(0,WEAPON_RATTLE,0);player()->weapons[0].ammo=0;game_fire(true);tick(1);
    CHECK(!player()->weapons[0].firing);player()->weapons[0].ammo=1;player()->weapon_capacitor=0;
    player()->pips[2]=0;tick(1);CHECK(player()->weapons[0].ammo==1);
}
static void mounts(void) {
    reset();game_player *p=player();p->docked=true;cg->v.systems[cg->v.system].tech=12;
    CHECK(game_fit_weapon(1,WEAPON_RATTLE));CHECK(p->weapons[1].ammo==400 && p->weapons[1].group==1);
    CHECK(!game_fit_weapon(-1,WEAPON_BEAM) && !game_fit_weapon(4,WEAPON_BEAM));
    CHECK(game_weapon_group(1,0) && !game_weapon_group(1,2));
    p->credits=0;game_player before=*p;CHECK(!game_fit_weapon(0,WEAPON_BEAM));CHECK(!memcmp(&before,p,sizeof(before)));
    p->credits=100000;p->weapons[1].ammo=10;p->chaff=1;p->shield_cells=1;p->heat_sinks=1;p->missiles=0;
    unsigned cost=game_rearm_cost();CHECK(cost>0);p->credits=cost-1;before=*p;
    CHECK(!game_rearm() && !memcmp(&before,p,sizeof(before)));p->credits=cost;CHECK(game_rearm() && !p->credits);
    CHECK(p->weapons[1].ammo==400 && p->missiles==p->pylons && p->shield_cells==3 && p->chaff==6 && p->heat_sinks==4);
    p->docked=false;contact(0,ROLE_DERELICT,-400);game_fire(true);game_fire2(false);tick(1);
    CHECK(p->weapons[0].firing && p->weapons[1].firing);game_fire(false);game_fire2(true);tick(1);
    CHECK(!p->weapons[0].firing && !p->weapons[1].firing);
}
static void distributor(void) {
    reset();game_player *p=player();
    for(int n=0;n<400;n++) {
        game_pip(n%3);NEAR(p->pips[0]+p->pips[1]+p->pips[2],6);
        for(int i=0;i<3;i++) CHECK(p->pips[i]>=0 && p->pips[i]<=4 && p->pips[i]*2==floorf(p->pips[i]*2));
    }
    game_pips_preset(1);NEAR(p->pips[0],4);game_pips_preset(2);NEAR(p->pips[2],4);
    game_pips_preset(3);NEAR(p->pips[1],4);game_pips_preset(0);
    cg_damage_typed(-1,10,DAMAGE_THERMAL,l3v(0,0,-100),false,-1);float balanced=100-p->shield_fore;
    p->shield_fore=100;game_pips_preset(1);cg_damage_typed(-1,10,DAMAGE_THERMAL,l3v(0,0,-100),false,-1);
    CHECK(100-p->shield_fore<balanced);
    p->weapon_capacitor=0;game_pips_preset(1);tick(25);float slow=p->weapon_capacitor;
    p->weapon_capacitor=0;game_pips_preset(2);tick(25);CHECK(p->weapon_capacitor>slow*3);
    game_pips_preset(0);p->throttle=1;for(int i=0;i<200;i++) cg_flight(.04f);float speed=p->speed;
    game_pips_preset(3);for(int i=0;i<200;i++) cg_flight(.04f);CHECK(p->speed>speed);
}
static void defence(void) {
    reset();game_player *p=player();cg_damage_typed(-1,100,DAMAGE_KINETIC,l3v(0,0,-100),false,-1);
    cg_damage(-1,100,l3v(0,0,-100),false);CHECK(p->shield_fore==0 && p->shield_reboot[0]>0);
    CHECK(cg->v.combat_events&COMBAT_SHIELD_DOWN);tick(190);CHECK(p->shield_fore==0);
    tick(12);CHECK(p->shield_fore>=50 && (cg->v.combat_events&COMBAT_SHIELD_UP));
    p->shield_fore=30;float heat=p->heat;CHECK(game_shield_cell());CHECK(p->shield_fore>=80 && p->heat>heat && p->shield_cells==2);
    CHECK(game_boost() && !game_boost());float cool=p->boost_cooldown;game_pips_preset(3);tick(25);
    CHECK(p->boost_cooldown<cool-1);
    cg->missiles[0]=(cg_missile){.p=l3v(0,0,-300),.v=l3v(0,0,180),.life=10,.target=-1,.kind=WEAPON_SEEKER,.owner=1};
    CHECK(game_chaff() && cg->missiles[0].lost_lock && p->chaff_left==5 && !game_chaff());
    p->heat=140;p->cabin_heat=.9f;CHECK(game_heat_sink() && p->heat==50 && p->heat_level<.6f);
    p->heat=150;float hull=p->hull;tick(1);CHECK(p->hull<hull && p->heat_level>1 && p->heat_level<=1.5f);
    p->equipment|=1u<<EQ_ECM;CHECK(game_ecm() && cg->missiles[0].life==0);
    p->docked=true;CHECK(!game_boost() && !game_chaff() && !game_heat_sink() && !game_shield_cell() && !game_ecm());
}
static void modules(void) {
    reset();game_contact *c=contact(16,ROLE_DERELICT,-100);c->shield=0;c->scanned=true;
    c->velocity=l3v(4,0,0);cg_damage_typed(16,60,DAMAGE_KINETIC,player()->position,true,MODULE_DRIVES);
    CHECK(c->modules[MODULE_DRIVES]==0);float x=c->position.x;cg_flight(1);NEAR(c->position.x,x+4);
    c->hull=130;cg_damage_typed(16,60,DAMAGE_KINETIC,player()->position,true,MODULE_CARGO);
    CHECK(c->modules[MODULE_CARGO]==0 && cg->cans[0].life>0);
    c->hull=130;cg_damage_typed(16,60,DAMAGE_KINETIC,player()->position,true,MODULE_POWER);CHECK(!c->active);
    reset();player()->modules[MODULE_DRIVES]=0;player()->velocity=l3v(3,0,0);player()->throttle=1;cg_flight(1);NEAR(player()->position.x,3);
    player()->modules[MODULE_WEAPONS]=0;game_fire(true);tick(1);CHECK(!player()->weapons[0].firing);
    player()->modules[MODULE_CARGO]=0;player()->cargo[0]=4;tick(3);CHECK(player()->cargo[0]==3);
    player()->modules[MODULE_POWER]=0;tick(1);CHECK(player()->dead);
    reset();c=contact(0,ROLE_PIRATE,-200);cg->fighters[0].modules[MODULE_WEAPONS]=0;tick(50);
    CHECK(!c->attacking_player && !c->weapons[0].firing);
}
static void canopy(void) {
    reset();game_player *p=player();p->rank=3;p->shield_fore=p->shield_aft=0;
    cg->rules.combat.defence[CF_CANOPY_DAMAGE]=20;
    cg_damage_typed(-1,8,DAMAGE_KINETIC,l3v(0,0,100),false,-1);NEAR(p->canopy,1);
    cg_damage_typed(-1,8,DAMAGE_KINETIC,l3v(0,0,-100),false,-1);CHECK(p->canopy==0 && (cg->v.combat_events&COMBAT_CANOPY));
    tick(25);CHECK(p->oxygen<300 && p->oxygen>298);
    float hull=p->hull;p->hull=130;cg->alarm_hull=130;cg_damage(-1,120,l3v(0,0,100),false);
    CHECK((cg->v.combat_events&(COMBAT_HULL_50|COMBAT_HULL_25|COMBAT_HULL_10))==(COMBAT_HULL_50|COMBAT_HULL_25|COMBAT_HULL_10));
    game_combat_ack(COMBAT_HULL_50);CHECK(!(cg->v.combat_events&COMBAT_HULL_50));
    p->hull=hull;p->dead=false;p->oxygen=.02f;tick(1);CHECK(p->dead);
    reset();p->docked=true;p->canopy=0;p->modules[MODULE_DRIVES]=.5f;CHECK(game_repair_cost()>0);
    CHECK(game_repair() && p->canopy==1 && p->modules[MODULE_DRIVES]==1);
    p->canopy=0;p->oxygen=1;cg_combat_dock();CHECK(p->canopy==1 && p->oxygen==300);
}
static void scanning(void) {
    reset();game_contact *c=contact(0,ROLE_PIRATE,-300);cg->v.selected=0;tick(60);
    CHECK(!c->scanned && c->scan_progress>.9f && !c->wanted && !c->bounty && !c->rank_text[0]);
    for(int i=0;i<GAME_MODULES;i++) CHECK(c->modules[i]==0);
    tick(3);CHECK(c->scanned && c->wanted && c->bounty>0 && c->rank_text[0] && c->modules[0]==1);
    game_subtarget_next();CHECK(cg->v.subtarget==MODULE_POWER);for(int i=0;i<GAME_MODULES;i++) game_subtarget_next();CHECK(cg->v.subtarget==-1);
    unsigned bounty=c->bounty,cash=player()->credits;cg_damage(0,1000,player()->position,true);CHECK(player()->credits==cash+bounty);
    c=contact(0,ROLE_TRADER,-300);CHECK(!c->scanned && !c->bounty);tick(20);
    contact(1,ROLE_TRADER,-200);cg->v.selected=1;tick(1);CHECK(c->scan_progress==0 && cg->v.subtarget==-1);
    player()->modules[MODULE_SENSORS]=0;tick(100);CHECK(!cg->v.contacts[1].scanned);
    cg_contacts();CHECK(!cg->v.contacts[1].detectable && cg->v.selected==-1);
}
static void hud(void) {
    reset();contact(0,ROLE_PIRATE,-200);tick(1);CHECK(cg->v.contacts[0].attacking_player && cg->v.contacts[0].threat==3);
    cg->encounter_enemy[0]=-2;cg->v.contacts[0].role=ROLE_DERELICT;cg->v.contacts[0].hostile=false;
    tick(40);CHECK(!cg->v.contacts[0].attacking_player);
    for(int i=0;i<10;i++) cg_damage(-1,1,l3v((float)i+1,0,-100),false);
    CHECK(player()->hit_count==GAME_HIT_DIRECTIONS);NEAR(cg_len(player()->hit_directions[0].direction),1);
    tick(1);CHECK(player()->hit_directions[0].age>0);
    cg->missiles[0]=(cg_missile){.p=l3v(0,0,-300),.v=l3v(0,0,180),.life=10,.target=-1,.kind=WEAPON_SEEKER,.owner=1};
    tick(1);CHECK(cg->v.missile_warning && cg->v.missile_range>0 && cg->v.projectiles[0].life>0);
    CHECK(game_chaff());tick(1);CHECK(!cg->v.missile_warning);
    for(int i=0;i<9;i++) {char line[20];snprintf(line,sizeof(line),"Event %d",i);cg_combat_log(line);}
    CHECK(cg->v.combat_log_count==6 && !strcmp(cg->v.combat_log[0],"Event 8") && !strcmp(cg->v.combat_log[5],"Event 3"));
}
static float incoming(int rank) {
    reset();player()->rank=(uint8_t)rank;contact(0,ROLE_PIRATE,-200);cg->rng=1234;float amount=0;
    for(int i=0;i<1000;i++) {player()->shield_fore=player()->shield_aft=100;tick(1);
        amount+=200-player()->shield_fore-player()->shield_aft;}
    return amount;
}
static void balance(void) {
    float early=incoming(0),normal=incoming(3);CHECK(early<normal && early>0);
    reset();player()->shield_fore=50;tick(25);float early_regen=player()->shield_fore-50;
    player()->rank=3;player()->shield_fore=50;tick(25);CHECK(early_regen>player()->shield_fore-50);
    printf("seeded incoming 40s: early %.3f, normal %.3f; early regen %.3f/s\n",early,normal,early_regen);
}
static void persistence(void) {
    reset();game_player *p=player();p->docked=true;cg->v.systems[cg->v.system].tech=12;
    CHECK(game_fit_weapon(1,WEAPON_RATTLE));p->weapons[1].ammo=123;p->modules[MODULE_SENSORS]=.4f;
    p->canopy=.3f;p->shield_cells=1;p->heat=140;game_pips_preset(2);
    CHECK(game_save());p->weapons[1].ammo=0;p->modules[MODULE_SENSORS]=1;
    CHECK(game_load());CHECK(p->weapons[1].ammo==123 && p->weapons[1].kind==WEAPON_RATTLE);
    NEAR(p->modules[MODULE_SENSORS],.4f);NEAR(p->canopy,.3f);CHECK(p->shield_cells==1 && p->heat==140 && p->pips[2]==4);
    CHECK(game_save());unsigned char bytes[8192];memcpy(bytes,cg->save_buffer,8192);bytes[8060]^=1;
    int fd=lap_file_open(cg->save_seq&1?"commander1.dat":"commander0.dat","w");CHECK(fd>=0);
    CHECK(lap_file_write(fd,bytes,8192)==8192);lap_file_close(fd);CHECK(game_load() && p->weapons[1].ammo==123);
}
static void limits(void) {
    reset();mount(0,WEAPON_RATTLE,0);game_fire(true);contact(0,ROLE_DERELICT,-600);
    unsigned start=allocations;clock_t t=clock();
    for(int i=0;i<100000;i++) {player()->weapons[0].ammo=400;player()->weapon_capacitor=100;
        player()->energy=100;player()->heat=0;tick(1);}
    CHECK(allocations==start);
    printf("combat: %.3f us/frame, %.3f us/s at 25 Hz, 0 allocations; state %u bytes\n",
        1e6*(double)(clock()-t)/CLOCKS_PER_SEC/100000,25e6*(double)(clock()-t)/CLOCKS_PER_SEC/100000,(unsigned)sizeof(*cg));
    for(int i=0;i<GAME_PROJECTILES;i++) CHECK(isfinite(cg->v.projectiles[i].position.x));
}
static void legacy_write(unsigned char bytes[8192],int version,bool yard) {
    unsigned length=188+(version>=2?358:0)+(version==3?6*65:0)+(yard?16:0);
    bytes[3]=(unsigned char)('0'+version);memset(bytes+8000,0,108);
    uint32_t hash=2166136261u;
    for(unsigned i=16;i<length;i++) hash=(hash^bytes[i])*16777619u;
    uint32_t header[]={length,77,hash};
    for(int i=0;i<3;i++) for(int j=0;j<4;j++) bytes[4+i*4+j]=(uint8_t)(header[i]>>(j*8));
    for(int slot=0;slot<2;slot++) {
        int fd=lap_file_open(slot?"commander1.dat":"commander0.dat","w");CHECK(fd>=0);
        CHECK(lap_file_write(fd,bytes,8192)==8192);lap_file_close(fd);
    }
}
static void migration(void) {
    reset();player()->docked=true;CHECK(game_mount_mode(0,MOUNT_TURRET));CHECK(game_save());
    unsigned char old[8192];memcpy(old,cg->save_buffer,sizeof(old));memcpy(old+8000,"CBT1",4);
    uint32_t sum=2166136261u;
    for(int i=8016;i<8108;i++) sum=(sum^old[i])*16777619u;
    for(int i=0;i<4;i++) old[8012+i]=(uint8_t)(sum>>(i*8));
    for(int slot=0;slot<2;slot++) {
        int fd=lap_file_open(slot?"commander1.dat":"commander0.dat","w");CHECK(fd>=0);
        CHECK(lap_file_write(fd,old,8192)==8192);lap_file_close(fd);
    }
    CHECK(game_load());CHECK(player()->weapons[0].kind==WEAPON_PULSE);
    for(int i=0;i<GAME_MOUNTS;i++) CHECK(player()->weapons[i].mode==MOUNT_FIXED);

    for(int version=1;version<=3;version++) {
        reset();game_player *p=player();p->docked=true;cg->v.registry_count=0;
        memset(cg->v.missions,0,sizeof(cg->v.missions));CHECK(game_save());
        unsigned char bytes[8192];memcpy(bytes,cg->save_buffer,sizeof(bytes));
        legacy_write(bytes,version,false);memset(p,0,sizeof(*p));CHECK(game_load());
        CHECK(p->docked && p->mounts==2 && p->lasers==1);
        for(int i=0;i<GAME_MOUNTS;i++) CHECK(p->weapons[i].mode==MOUNT_FIXED);
        for(int i=0;i<GAME_MODULES;i++) NEAR(p->modules[i],1);
        CHECK(p->weapons[0].kind==WEAPON_PULSE && p->weapons[1].kind==WEAPON_NONE);
        CHECK(p->weapon_capacitor==100 && p->pips[0]==2 && p->pips[1]==2 && p->pips[2]==2);
        CHECK(p->shield_fore==100 && p->shield_aft==100 && p->shield_reboot[0]==0 && p->shield_reboot[1]==0);
        game_context();CHECK(!p->docked);game_throttle(1);game_update(.2f);
        CHECK(!p->dead && p->hull>0 && p->speed>8);
        /* A jump/reset must preserve genuine damage rather than heal zero modules. */
        p->modules[MODULE_WEAPONS]=0;p->weapon_capacitor=17;
        cg_arrive(1);NEAR(p->modules[MODULE_WEAPONS],0);NEAR(p->weapon_capacitor,17);
    }
    reset();game_player *p=player();p->docked=true;cg->v.registry_count=0;
    cg_ship_fit(p,SH_THRESHER);p->lasers=2;cg_combat_player_init(p);
    CHECK(game_save());unsigned char bytes[8192];memcpy(bytes,cg->save_buffer,sizeof(bytes));
    legacy_write(bytes,3,true);CHECK(game_load());
    CHECK(p->ship_class==SH_THRESHER && p->mounts==2 && p->lasers==2);
    CHECK(p->weapons[0].kind==WEAPON_PULSE && p->weapons[1].kind==WEAPON_PULSE);
    /* Legacy fixture players may enter flight, targeting or fitting before an update. */
    memset(p,0,sizeof(*p));p->equipment=1u<<EQ_LASER;p->hull=p->energy=100;
    p->attitude=(l3_quat){0,0,0,1};p->throttle=1;cg_flight(.04f);
    CHECK(p->modules[MODULE_DRIVES]==1 && p->speed>0 && p->mounts==2);
    NEAR(p->modules[MODULE_POWER],1);
    /* Every game spawn uses the same full-health baseline and hull mount count. */
    for(int type=0;type<13;type++) for(int role=ROLE_TRADER;role<=ROLE_BEACON;role++) {
        cg_spawn(16,1000+type*10+role,(game_class)type,(game_role)role,l3v(0,0,-100));
        cg_combat_contact *f=&cg->fighters[16];
        CHECK(f->initialized && f->capacitor==100);
        CHECK(f->mounts==(type==SH_DRONE?2:type<11?cg->rules.yard.hulls[type].mounts:1));
        for(int i=0;i<GAME_MODULES;i++) NEAR(f->modules[i],1);
        for(int i=0;i<GAME_MOUNTS;i++) CHECK(i<f->mounts?f->weapons[i].kind<WEAPON_COUNT:f->weapons[i].kind==WEAPON_NONE);
    }
    /* Even an id-zero fixture contact is distinct from an uninitialized private slot. */
    memset(&cg->fighters[16],0,sizeof(cg->fighters[16]));cg->v.contacts[16].id=0;
    cg_combat(.04f);CHECK(cg->fighters[16].initialized);NEAR(cg->fighters[16].modules[MODULE_POWER],1);
    reset();p->rank=3;p->shield_fore=50;cg->v.in_nebula=true;tick(25);
    NEAR(p->shield_fore,50.5f);
    game_pips_preset(1);p->shield_fore=50;tick(25);
    NEAR(p->shield_fore,50+.5f*(cg->rules.combat.distributor[CD_SYS_BASE]+4*cg->rules.combat.distributor[CD_SYS_STEP]));
}
static void config(void) {
    static const char defaults[] =
#include "rules_default.inc"
    ;
    CHECK(cg->rules.version==6);char data[4096];memcpy(data,defaults,sizeof(defaults));
    char *p=strstr(data,"\"damage\":[[1.2,0.8]");CHECK(p);p[11]='9';
    int fd=lap_file_open("rules.json","w");CHECK(fd>=0);lap_file_write(fd,data,strlen(data));lap_file_close(fd);
    CHECK(!cg_config());NEAR(cg->rules.combat.damage[0][0],1.2f);
    memcpy(data,defaults,sizeof(defaults));p=strstr(data,"\"defence\":[8,");CHECK(p);p[11]='0';
    fd=lap_file_open("rules.json","w");CHECK(fd>=0);lap_file_write(fd,data,strlen(data));lap_file_close(fd);CHECK(!cg_config());
}
int main(int argc,char **argv) {
    CHECK(argc==2);adsb_state_init();ls_lap_api_begin("carrier-combat-rules",1,0);ls_lap_sys_begin();ls_lap_api_callback(true);
    CHECK(game_init() && cg->v.config_valid);
    if(!strcmp(argv[1],"damage")) damage();else if(!strcmp(argv[1],"weapons")) weapons();
    else if(!strcmp(argv[1],"mounts")) mounts();else if(!strcmp(argv[1],"distributor")) distributor();
    else if(!strcmp(argv[1],"defence")) defence();else if(!strcmp(argv[1],"modules")) modules();
    else if(!strcmp(argv[1],"canopy")) canopy();else if(!strcmp(argv[1],"scanning")) scanning();
    else if(!strcmp(argv[1],"hud")) hud();else if(!strcmp(argv[1],"balance")) balance();
    else if(!strcmp(argv[1],"persistence")) persistence();else if(!strcmp(argv[1],"limits")) limits();
    else if(!strcmp(argv[1],"reverse")) reverse_rules();else if(!strcmp(argv[1],"aim")) aim_rules();
    else if(!strcmp(argv[1],"config")) config();else if(!strcmp(argv[1],"migration")) migration();else CHECK(false);
    printf("combat %s: %u checks passed\n",argv[1],checks);
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
