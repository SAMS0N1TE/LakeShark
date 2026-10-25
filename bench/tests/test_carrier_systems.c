/* Part 2 backend outcomes; sensor wrappers supply deterministic worker snapshots. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
/* Backend suites need only the renderer invalidation hook. */
void cg_render_reset(void) {}
static uint32_t ms, scans, allocations;
static int aps, mesh_count, ble_count;
extern uint32_t (*ls_lap_api_host_clock)(void);
static uint32_t now(void) { return ms; }
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
void *__wrap_malloc(size_t n) {allocations++;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t z) {allocations++;return __real_calloc(n,z);}
void *__wrap_realloc(void *p,size_t n) {allocations++;return __real_realloc(p,n);}
int __wrap_lap_wifi_aps(lap_wifi_ap_t *out,int max) {(void)out;return aps<max?aps:max;}
bool __wrap_lap_wifi_scan(void) {scans++;return true;}
void __wrap_lap_ble_scan(bool on) {(void)on;}
int __wrap_lap_ble_devices(lap_ble_dev_t *out,int max) {
    for(int i=0;i<ble_count && i<max;i++) {memset(&out[i],0,sizeof(out[i]));out[i].addr[0]=(uint8_t)(i+1);}
    return ble_count<max?ble_count:max;
}
int __wrap_lap_mesh_nodes(lap_mesh_node_t *out,int max) {
    for(int i=0;i<mesh_count && i<max;i++) {memset(&out[i],0,sizeof(out[i]));snprintf(out[i].id,17,"node%d",i);snprintf(out[i].name,20,"LakeShark %u",(unsigned)i%10);}
    return mesh_count<max?mesh_count:max;
}
void __wrap_lap_time(lap_time_t *out) {memset(out,0,sizeof(*out));out->hour=19;out->unix=1790000000;}
static void setup(void) {
    ls_lap_api_host_clock=now;adsb_state_init();ls_lap_api_begin("carrier-part2",1,0);ls_lap_sys_begin();ls_lap_api_callback(true);
    assert(game_init());assert(cg->v.config_valid);cg->poll=-1000;
}
static void step(int n) {for(int i=0;i<n;i++) {ms+=40;game_update(.04f);}}
static game_contact *contact(game_role role,uint32_t id) {
    cg->v.player.docked=false;cg->v.player.position=l3v(0,0,1100);cg->v.player.throttle=0;cg->v.player.velocity=l3v(0,0,0);
    cg_spawn(0,id,role==ROLE_POLICE?SH_VIPER:SH_COBRA,role,l3v(0,0,1060));cg->npc_control[0]=false;
    cg->v.selected=0;cg_contacts();return &cg->v.contacts[0];
}
static void talk(void) {
    bool traded=false,news=false;
    for(uint32_t id=1;id<100 && (!traded || !news);id++) {
        contact(ROLE_TRADER,id);cg_actions();assert(cg->v.action==ACT_HAIL);game_context();
        game_talk a=cg->v.talk;game_talk_close();game_context();assert(!memcmp(&a,&cg->v.talk,sizeof(a)));
        for(int j=0;j<a.line_count;j++) assert(strlen(a.lines[j])<=30);
        for(int j=0;j<a.choice_count;j++) assert(strlen(a.choices[j])<=12);
        if(a.price>0 && !traded) {
            unsigned cash=cg->v.player.credits;int n=cg->v.player.cargo[a.good];assert(game_talk_choose(0));
            assert(cg->v.player.credits==cash-(unsigned)a.price && cg->v.player.cargo[a.good]==n+a.amount && cg_friendly(id));traded=true;
            cg_talk_open(0);assert(!game_talk_choose(0));game_talk_close();
        } else {assert(game_talk_choose(1));if(!a.price) news=true;}
    }
    assert(traded && news);
    contact(ROLE_POLICE,110);cg->v.player.legal=20;cg->v.player.cargo[5]=2;cg_talk_open(0);
    unsigned cash=cg->v.player.credits;int fine=cg->v.talk.price;assert(game_talk_choose(0));
    assert(cg->v.player.credits==cash-(unsigned)fine && !cg->v.player.legal && !cg->v.player.cargo[5]);
    cg->v.player.cargo[9]=1;cg_talk_open(0);assert(game_talk_choose(1));assert(cg->v.player.legal && cg->v.contacts[0].hostile);
    cg->v.player.cargo[9]=0;cg->v.player.legal=0;cg_talk_open(0);assert(strstr(cg->v.talk.lines[0],"clear"));assert(game_talk_choose(1));
    contact(ROLE_PIRATE,120);cg->v.player.cargo[0]=6;cg_talk_open(0);int cargo=game_cargo_used(),share=cg->v.talk.amount;
    assert(game_talk_choose(0));assert(game_cargo_used()==cargo-share && !cg->v.contacts[0].active);
    memset(cg->v.contacts,0,sizeof(cg->v.contacts));memset(cg->dead_until,0,sizeof(cg->dead_until));
    contact(ROLE_PIRATE,121);cg_talk_open(0);assert(game_talk_choose(1));int count=0;
    for(int i=0;i<GAME_CONTACTS;i++) count+=cg->v.contacts[i].active && cg->v.contacts[i].role==ROLE_PIRATE;
    assert(count>=2 && count<=4);
    contact(ROLE_RESCUE,130);cg->v.player.cargo[0]=1;cg_talk_open(0);assert(game_talk_choose(0));assert(cg->rescue_target==0);
    cg->rescue_target=-1;cg->v.contacts[0].squawk=7500;cg_talk_open(0);assert(game_talk_choose(0));
    bool bounty=false;for(int i=0;i<6;i++) bounty|=cg->v.missions[i].accepted && cg->v.missions[i].kind==MISSION_BOUNTY;
    assert(bounty && !cg->v.contacts[0].hostile);assert(!game_talk_choose(-1));
    puts("talk: deterministic merchant trade/news, police fine/run/clean, pirate pay/refuse, rescue and hijack passed");
}
static void missions(void) {
    cg->v.player.docked=true;cg_bulletin();assert(game_mission_accept(0));assert(game_mission_accept(1));assert(game_mission_accept(2));
    assert(!game_mission_accept(3) && !game_mission_accept(-1) && !game_mission_accept(0));
    game_mission saved[6];memcpy(saved,cg->v.missions,sizeof(saved));cg->v.market_day=12;cg->v.player.combat_score=77;cg->v.player.cabin_heat=.5f;
    assert(game_save());memset(cg->v.missions,0,sizeof(saved));cg->v.market_day=99;assert(game_load());
    assert(!memcmp(saved,cg->v.missions,sizeof(saved)) && cg->v.market_day==12 && cg->v.player.combat_score==77 && cg->v.player.cabin_heat==.5f);
    for(int i=0;i<3;i++) cg->v.missions[i].deadline=30;
    unsigned cash=cg->v.player.credits;game_mission *m=&cg->v.missions[0];cg->v.system=m->destination;int n=cg->v.player.cargo[m->good];
    cg_mission_dock();assert(m->done && cg->v.player.credits==cash+m->reward && cg->v.player.cargo[m->good]==n-m->amount);
    cash=cg->v.player.credits;cg_mission_dock();assert(cg->v.player.credits==cash);
    m=&cg->v.missions[1];cg->v.system=m->destination;cg_mission_dock();assert(m->done);
    m=&cg->v.missions[2];cg->v.system=m->destination;cg_mission_dock();assert(!m->done);
    cg_mission_kill(m->target_id);cg_mission_dock();assert(m->done);
    assert(game_mission_accept(3));m=&cg->v.missions[3];cg->v.system=m->destination;cg_mission_rescue(m->target_id);cg_mission_dock();assert(m->done);
    assert(game_mission_accept(4));m=&cg->v.missions[4];m->deadline=cg->v.market_day;cg_day();assert(m->failed && !m->done);
    assert(game_save());assert(game_load());assert(cg->v.missions[4].failed);
    puts("missions: accept cap, all four payouts, single award, deadline and save round trip passed");
}
static void mining(void) {
    for(uint32_t r=0;r<100;r++) {int a,b,c;assert(game_mining_yield(r,0,&a)==game_mining_yield(r,8,&b) && a==b);assert(game_mining_yield(r,16,&c)>game_mining_yield(r,1,&b));assert(c>=11 && c<=14);}
    memset(cg->cans,0,sizeof(cg->cans));aps=16;cg->sensor_seconds=1;cg_system_update(.04f);assert(cg->wifi_count==16);
    game_player *p=&cg->v.player;p->docked=false;p->position=l3v(0,0,1100);p->attitude=(l3_quat){0,0,0,1};
    memset(cg->v.contacts,0,sizeof(cg->v.contacts));memset(cg->v.asteroids,0,sizeof(cg->v.asteroids));
    cg->v.asteroids[0]=(game_asteroid){l3v(0,0,1060),15,1,true};p->firing=true;cg_combat(.04f);p->firing=false;
    assert(!cg->v.asteroids[0].active);int cans=0;for(int i=0;i<CG_CANS;i++) cans+=cg->cans[i].life>0;assert(cans==5);
    p->equipment|=1u<<EQ_SCOOP;p->fuel=1;p->position=l3_add(cg->v.contents.star,l3v(cg->v.contents.star_radius*1.3f,0,0));
    float hull=p->hull;for(int i=0;i<400;i++) cg_system_update(.04f);assert(p->fuel>1 && p->cabin_heat>.85f && p->hull<hull);
    p->position=l3v(0,0,1100);float heat=p->cabin_heat;cg_system_update(.2f);assert(p->cabin_heat<heat);
    memset(cg->cans,0,sizeof(cg->cans));ble_count=2;cg->sensor_seconds=1;cg_system_update(.04f);cg->sensor_seconds=1;cg_system_update(.04f);
    cans=0;for(int i=0;i<CG_CANS;i++) cans+=cg->cans[i].life>0;assert(cans==2);
    puts("mining: average/no-WiFi, richness yields, laser breakage, star heat/refuel, BLE dedup passed");
}
static void beacons(void) {
    mesh_count=4;cg->sensor_seconds=1;cg_system_update(.04f);int count=0,slot=-1;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active && cg->v.contacts[i].role==ROLE_BEACON) {count++;slot=i;}
    assert(count==4);game_player *p=&cg->v.player;p->docked=false;p->position=cg->v.contacts[slot].position;p->fuel=1;
    cg->v.selected=slot;cg_contacts();game_context();assert(p->fuel==7);p->fuel=2;game_context();assert(p->fuel==2);
    cg_day();game_context();assert(p->fuel==7);p->fuel=2;uint32_t used_id=cg->v.contacts[slot].id;assert(game_save());assert(game_load());
    cg->sensor_seconds=1;cg_system_update(.04f);for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].role==ROLE_BEACON && cg->v.contacts[i].active && cg->v.contacts[i].id==used_id) slot=i;
    p=&cg->v.player;p->docked=false;p->position=cg->v.contacts[slot].position;assert(!cg_beacon_use(slot) && p->fuel==2);
    assert(cg->v.registry[cg->v.registry_count-1].name[0]);
    puts("beacons: four named nodes, context refuel, daily limit and saved limit passed");
}
static void packs(void) {
    bool seen[4]={0};for(uint32_t seed=0;seed<100;seed++) {
        int n=game_pirate_pack(seed,0);assert(n>=1 && n<=3);seen[n]=true;assert(game_pirate_pack(seed,1)==n);
        for(int g=2;g<8;g++) assert(game_pirate_pack(seed,g)==1);
    }assert(seen[1] && seen[2] && seen[3]);
    /* pack spawning now belongs to the encounter director: test_carrier_encounters covers it */
    game_contact *c=contact(ROLE_TRADER,555);cg_friend(555);cg_spawn(0,555,SH_COBRA,ROLE_PIRATE,c->position);assert(!c->hostile && cg_enemy(0)==-2);
    cg_spawn(20,777,SH_COBRA,ROLE_TRADER,l3v(0,0,-70));cg->lane_out[20]=false;cg_flight(.04f);assert(!cg->v.contacts[20].active);
    puts("packs: government bounds, session friendship and NPC docking passed");
}
static void ranks(void) {
    for(int r=0;r<9;r++) {assert(game_combat_rank(cg->systems.rank[r])==r);if(r) assert(game_combat_rank(cg->systems.rank[r]-1)==r-1);}
    game_contact *c=contact(ROLE_PIRATE,444);c->shield=0;c->hull=1;unsigned cash=cg->v.player.credits;cg_damage(0,10,cg->v.player.position,true);
    assert(cg->v.player.combat_score>1 && cg->v.player.credits>cash);
    c=contact(ROLE_POLICE,445);c->shield=0;c->hull=1;cg_damage(0,10,cg->v.player.position,true);assert(cg->v.player.legal==255);
    cg_day();assert(cg->v.player.legal==255);cg->v.player.legal=3;cg_day();assert(cg->v.player.legal==2);
    cg->v.player.rank=0;cg->v.player.shield_fore=cg->v.player.shield_aft=0;float hull=cg->v.player.hull;cg_damage(-1,10,l3v(0,0,0),false);assert(cg->v.player.hull>hull-10);
    puts("ranks: nine thresholds, danger-weighted kill/bounty, police fugitive, decay, forgiving damage passed");
}
static void events(void) {
    bool seen[5]={0};game_system s=cg->v.systems[0];
    for(uint32_t d=0;d<1000;d++) {
        game_event e=game_system_event(s.seed,d);assert(e==game_system_event(s.seed,d));seen[e]=true;
        for(int g=0;g<GAME_GOODS;g++) {
            int pct=game_event_price_percent(e,g);assert(pct==100 || pct==160);
            int variation=(int)(cg_hash(s.seed^cg_hash(d)^((uint32_t)g*7919))%21)-10;
            int base=cg->rules.base[g]+cg->rules.slope[g]*((int)s.economy-3)+((int)s.government-3)*2+(int)s.tech*(g==6?3:1);
            base+=base*variation/100;int expected=(int)fmaxf(1,base*pct/100);assert(game_price(&s,g,d)==expected);
        }
    }
    for(int i=0;i<5;i++) assert(seen[i]);
    assert(game_event_price_percent(EVENT_FESTIVAL,3)==160 && game_event_price_percent(EVENT_FESTIVAL,4)==160);
    assert(game_event_price_percent(EVENT_FAMINE,0)==160 && game_event_price_percent(EVENT_WAR,9)==160 && game_event_price_percent(EVENT_WAR,8)==160 && game_event_price_percent(EVENT_PLAGUE,2)==160);
    cg->v.systems[0].tech=10;cg_arrive(0);assert(cg->v.contents.station_kind==1);cg->v.systems[0].tech=9;cg_arrive(0);assert(!cg->v.contents.station_kind);
    cg_system_update(.04f);assert(cg->v.local_hour==19 && strlen(cg->v.event_text)<=30);
    unsigned start=allocations;step(100);assert(allocations==start);
    puts("events: five seeded events, exact price effects, tech stations, board hour and zero frame allocations passed");
}
static void time_rules(void) {
    cg_system_update(.04f);unsigned started=scans;assert(started==1);
    for(int i=0;i<100;i++) cg_system_update(.2f);
    assert(scans==started);
    ms+=59999;cg_system_update(.04f);assert(scans==started);
    ms++;cg_system_update(.04f);assert(scans==started+1);
    unsigned day=cg->v.market_day;ms+=600000;cg_system_update(.04f);assert(cg->v.market_day==day);
    cg->v.player.docked=false;ms+=599999;cg_system_update(.04f);assert(cg->v.market_day==day);
    ms++;cg_system_update(.04f);assert(cg->v.market_day==day+1);
    cg->v.plotted=1;cg->v.jump=JUMP_TUNNEL;cg->v.jump_progress=2;cg_jump(.04f);assert(cg->v.market_day==day+2);
    puts("time: monotonic ten-minute flight days, dock pause, jump days and one scan/minute passed");
}
static void system_config(void) {
    static const char defaults[] =
#include "systems_default.inc"
    ;
    char buf[1024];memcpy(buf,defaults,sizeof(defaults));
    char *scan=strstr(buf,"\"scan_seconds\":60");assert(scan);scan[strlen("\"scan_seconds\":")]='5';scan[strlen("\"scan_seconds\":")+1]='9';
    int fd=lap_file_open("systems.json","w");assert(fd>=0);assert(lap_file_write(fd,buf,strlen(buf))==(int)strlen(buf));lap_file_close(fd);
    assert(!cg_system_config() && cg->systems.scan_seconds==60);
    fd=lap_file_open("systems.json","w");assert(fd>=0);lap_file_write(fd,defaults,sizeof(defaults)-1);lap_file_close(fd);
    assert(cg_system_config() && cg->systems.rank[8]==6400);
    puts("system_config: valid defaults and sub-minute scan rejection passed");
}
static void save_capacity(void) {
    cg->v.registry_count=GAME_REGISTRY;
    for(int i=0;i<GAME_REGISTRY;i++) {
        game_registry_entry *e=&cg->v.registry[i];memset(e,0,sizeof(*e));e->id=1000+i;e->visits=1;
        memcpy(e->callsign,"FULL",5);memcpy(e->name,"Full registry",14);e->refuel_day=cg->v.market_day+1;
    }
    assert(game_save());assert(game_load());assert(cg->v.registry_count==128 && cg->v.registry[127].refuel_day==cg->v.market_day+1);
    /* Derive an actual CRR1 payload by removing only the CRR2 appendices. */
    unsigned char legacy[8192]={0};memcpy(legacy,cg->save_buffer,188);memcpy(legacy,"CRR1",4);
    for(int i=0;i<128;i++) memcpy(legacy+188+i*31,cg->save_buffer+188+i*55,31);
    uint32_t length=188+128*31,hash=2166136261u;
    for(uint32_t i=16;i<length;i++) hash=(hash^legacy[i])*16777619u;
    for(int i=0;i<4;i++) {legacy[4+i]=(uint8_t)(length>>(8*i));legacy[8+i]=(uint8_t)((cg->save_seq+1)>>(8*i));legacy[12+i]=(uint8_t)(hash>>(8*i));}
    int fd=lap_file_open("commander0.dat","w");assert(fd>=0);assert(lap_file_write(fd,legacy,8192)==8192);lap_file_close(fd);
    assert(game_load() && cg->v.registry_count==128 && cg->v.player.cabin_heat==0 && !cg->v.missions[0].accepted);
    puts("save_capacity: full registry CRR2 and CRR1 migration passed");
}
static void targets(void) {
    cg->v.player.docked=true;assert(game_mission_accept(2));game_mission *m=&cg->v.missions[2];
    cg_arrive(m->destination);cg_system_update(.04f);int slot=-1;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active && cg->v.contacts[i].id==m->target_id) slot=i;
    assert(slot>=0 && cg->v.contacts[slot].role==ROLE_PIRATE && strstr(m->title,cg->v.contacts[slot].name));
    cg_damage(slot,10000,cg->v.player.position,true);assert(m->ready);
    cg->v.player.docked=true;cg_mission_dock();assert(m->done);
    assert(game_mission_accept(3));m=&cg->v.missions[3];cg_arrive(m->destination);cg_system_update(.04f);slot=-1;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active && cg->v.contacts[i].id==m->target_id) slot=i;
    assert(slot>=0 && cg->v.contacts[slot].squawk==7700);
    game_player *p=&cg->v.player;p->cargo[0]=1;p->throttle=0;
    p->position=l3_add(cg->v.contacts[slot].position,l3v(0,0,110));cg->v.selected=slot;cg_contacts();game_context();
    assert(cg->v.talk.open && game_talk_choose(0));step(10);assert(cg->rescue_target==slot && cg->v.rescue_progress==0);
    p->position=l3_add(cg->v.contacts[slot].position,l3v(0,0,40));step(130);assert(m->ready && !p->cargo[0]);
    p->docked=true;cg_mission_dock();assert(m->done);
    puts("targets: named pirate spawn/kill and 7700 conversation/approach/transfer/dock passed");
}
int main(int argc,char **argv) {
    assert(argc==2);setup();
    if(!strcmp(argv[1],"talk")) talk();else if(!strcmp(argv[1],"missions")) missions();else if(!strcmp(argv[1],"mining")) mining();
    else if(!strcmp(argv[1],"beacons")) beacons();else if(!strcmp(argv[1],"packs")) packs();else if(!strcmp(argv[1],"ranks")) ranks();else if(!strcmp(argv[1],"events")) events();else if(!strcmp(argv[1],"time")) time_rules();else if(!strcmp(argv[1],"system_config")) system_config();else if(!strcmp(argv[1],"save_capacity")) save_capacity();else if(!strcmp(argv[1],"targets")) targets();else assert(0);
    printf("part2 state: %u bytes\n",(unsigned)sizeof(*cg));game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
