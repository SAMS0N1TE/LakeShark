/* Real exploration module with cached LAP files and deterministic board time. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
#include <time.h>
void cg_render_reset(void) {}
static int hour=12;
static unsigned allocations;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
void *__wrap_malloc(size_t n) {allocations++;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s) {allocations++;return __real_calloc(n,s);}
void *__wrap_realloc(void *p,size_t n) {allocations++;return __real_realloc(p,n);}
void __wrap_lap_time(lap_time_t *out) {memset(out,0,sizeof(*out));out->hour=hour;out->synced=true;}
static void write_file(const char *name,const void *buf,int n) {
    int fd=lap_file_open(name,"w");assert(fd>=0);assert(lap_file_write(fd,buf,n)==n);lap_file_close(fd);
}
static void tick(int n) {for(int i=0;i<n;i++) cg_poi_update(.04f);}
static game_poi *only(game_poi_kind kind) {
    game_poi out[GAME_POIS];bool found=false;
    for(uint32_t s=0;s<100000 && !found;s++) {
        int n=game_poi_generate(s,out);
        for(int i=0;i<n;i++) if(out[i].kind==kind) {
            cg->v.pois[0]=out[i];found=true;break;
        }
    }
    assert(found);cg->v.poi_count=1;
    cg->v.player.docked=cg->v.player.dead=false;cg->v.player.throttle=0;
    cg->v.player.velocity=l3v(0,0,0);cg->v.player.energy=100;cg->v.player.hull=100;
    cg->v.player.position=cg->v.pois[0].position;cg->v.player.speed=0;
    memset(cg->v.contacts,0,sizeof(cg->v.contacts));memset(cg->cans,0,sizeof(cg->cans));
    memset(cg->poi.scoop,0,sizeof(cg->poi.scoop));cg->poi.activated=0;
    memset(cg->poi.marks,0,sizeof(cg->poi.marks));memset(&cg->v.discoveries,0,sizeof(cg->v.discoveries));
    cg->poi.shield_valid=false;cg->poi.quiet_hold=0;
    cg->v.found_eggs=0;return &cg->v.pois[0];
}
static void generation(void) {
    game_poi a[6],b[6];unsigned seen=0,zero=0;
    for(uint32_t s=0;s<10000;s++) {
        int n=game_poi_generate(s,a);assert(n>=0 && n<=4);
        assert(game_poi_generate(s,b)==n && !memcmp(a,b,sizeof(a)));zero+=n==0;
        for(int i=0;i<n;i++) {
            game_poi *p=&a[i];seen|=1u<<p->kind;
            float d=cg_len(p->position);assert(d>4000 && d<7000);
            assert(p->radius>=20 && p->radius<=1000 && p->scan_range>p->radius && p->colour<=15);
            assert(strlen(p->name)<=20 && p->name[0]);
            for(int l=0;l<3;l++) assert(strlen(p->scan_text[l])<=30);
            for(int j=0;j<i;j++) assert(cg_len(l3_sub(p->position,a[j].position))>p->radius+a[j].radius);
            assert(!p->scanned && !p->discovered);
        }
    }
    assert(zero && (seen&2047)==2047);assert(game_poi_generate(0,NULL)==0);
    puts("generation: 10000 seeds repeat exactly, 0-4, separated regions and all eleven natural kinds");
}
static void nebula(void) {
    game_poi *p=only(POI_NEBULA);game_player *player=&cg->v.player;
    player->shield_fore=player->shield_aft=50;player->equipment|=1u<<EQ_SCOOP;
    cg_poi_update(.04f);assert(cg->v.in_nebula && cg->v.nebula_density==1 && cg->v.scanner_scale==.5f);
    assert(!game_poi_detectable(player->position,l3_add(player->position,l3v(800,0,0))));
    float start=player->shield_fore;
    for(int i=0;i<100;i++) {cg_combat(.04f);cg_poi_update(.04f);}
    assert(fabsf(player->shield_fore-start-3)<.02f && player->cargo[2]==1); /* Early ranks now regenerate 3/s; nebula keeps one quarter. */
    cg_spawn(16,777,SH_PROBE,ROLE_PIRATE,l3_add(player->position,l3v(300,0,0)));
    cg->cooldown[16]=.1f;cg_poi_update(.04f);float shield=player->shield_fore+player->shield_aft;
    cg_combat(.04f);assert(player->shield_fore+player->shield_aft>=shield);
    player->position=l3_add(p->position,l3v(p->radius+5,0,0));cg_poi_update(.04f);
    assert(!cg->v.in_nebula && cg->v.nebula_density==0 && cg->v.scanner_scale==1);
    assert(game_poi_detectable(player->position,l3_add(player->position,l3v(800,0,0))));
    puts("nebula: density, half sensors, quarter shield recharge, scoop and concealment suppresses distant guns");
}
static void gravity(void) {
    game_poi *p=only(POI_DARK_STAR);game_player *player=&cg->v.player;
    player->position=l3_add(p->position,l3v(p->radius*.5f,0,0));
    cg_poi_update(.04f);assert(player->velocity.x<0 && player->speed>0 && !player->dead);
    player->position=l3_add(p->position,l3v(p->radius+5,0,0));player->velocity=l3v(0,0,0);
    cg_poi_update(.04f);assert(cg_len(player->velocity)==0);
    player->position=p->position;cg_poi_update(.04f);assert(player->dead && player->hull==0);
    unsigned cash=player->credits;tick(200);assert(player->credits==cash);
    puts("gravity: velocity bends only in range; core is fatal and a lost commander earns nothing");
}
static void comet(void) {
    game_poi *p=only(POI_COMET);game_player *player=&cg->v.player;player->equipment|=1u<<EQ_SCOOP;
    l3_vec start=p->position;
    for(int i=0;i<130;i++) {
        player->position=l3_add(p->position,l3_scale(p->tail,300));cg_poi_update(.04f);
    }
    assert(cg_len(l3_sub(start,p->position))>50 && fabsf(cg_len(p->tail)-1)<.001f);
    assert(l3_dot(p->tail,p->velocity)<0 && player->cargo[11]==1 && (cg->v.found_eggs&32));
    player->position=l3_sub(p->position,l3_scale(p->tail,300));tick(110);assert(player->cargo[11]==1);
    puts("comet: real motion, opposite tail vector, scoop restricted to wake, quiet-wake egg");
}
static void relics(void) {
    game_poi *p=only(POI_RELIC);p->variant=2;cg->v.relic_chain=0;
    tick(70);assert(!p->answered && p->hold_progress>.5f);
    cg->v.player.position=l3_add(p->position,l3v(2000,0,0));tick(1);assert(p->hold_progress==0);
    cg->v.player.position=p->position;tick(124);assert(!p->answered);tick(2);
    assert(p->answered && cg->v.relic_chain==1 && cg->v.relic_waypoint==cg->poi.chain[1]);
    unsigned cash=cg->v.player.credits;tick(200);assert(cg->v.player.credits==cash);
    for(int j=1;j<5;j++) {
        cg_arrive(cg->poi.chain[j]);p=&cg->v.pois[0];assert(p->kind==POI_RELIC && p->variant==j+2);
        cg->v.player.position=p->position;tick(126);assert(cg->v.relic_chain==j+1 && p->answered);
    }
    assert(cg->v.relic_waypoint==-1);
    only(POI_RELIC);p=&cg->v.pois[0];p->variant=4;cg->v.relic_chain=0;tick(150);assert(!p->answered);
    puts("relics: continuous five-second hold, reset, one reward, ordered five-system chain and final grant");
}
static void signals(void) {
    cg->v.poi_count=0;cg->v.player.docked=false;cg->v.player.throttle=0;
    /* Go through the real ADS-B snapshot -> traffic -> POI producer. */
    adsb_aircraft_t *rx=adsb_state_find_or_create(0xa12345);assert(rx);
    rx->last_seen_us=(int64_t)lap_millis()*1000;strcpy(rx->callsign,"ABC123");
    adsb_state_set_squawk(rx,7700);adsb_state_set_squawk(rx,7700);
    cg->poll=1;cg_traffic(0);cg_contacts();
    cg->poi.radio_poll=1;tick(1);assert(cg->v.poi_count==1);
    game_poi *p=&cg->v.pois[0];assert(p->kind==POI_SIGNAL && p->squawk==7700 && p->source_id && p->variant==2);
    tick(30);assert(cg->v.poi_count==1 && cg->v.signal_poi==0 && isfinite(cg->v.signal_bearing));
    cg->v.player.position=p->position;tick(80);assert(p->scanned && p->answered);
    bool rescue=false;for(int i=16;i<GAME_CONTACTS;i++) rescue|=cg->v.contacts[i].active && cg->v.contacts[i].role==ROLE_RESCUE;
    assert(rescue);unsigned count=cg->v.discoveries.count,cash=cg->v.player.credits;
    assert(game_explore_save());assert(cg_poi_load());cg_poi_arrive();
    cg->v.poi_count=0;cg->poi.radio_poll=1;tick(1);assert(cg->v.pois[0].scanned);
    tick(100);assert(cg->v.discoveries.count==count && cg->v.player.credits==cash);
    for(int i=0;i<2;i++) {
        cg->v.poi_count=0;adsb_state_set_squawk(rx,i?7500:7600);adsb_state_set_squawk(rx,i?7500:7600);
        cg->poll=1;cg_traffic(0);cg->poi.radio_poll=1;tick(1);
        assert(cg->v.poi_count==1 && cg->v.pois[0].squawk==(i?7500:7600));
        p=&cg->v.pois[0];cg->v.player.position=p->position;tick(80);assert(p->answered);
        if(i) {bool hostile=false;for(int j=16;j<GAME_CONTACTS;j++) hostile|=cg->v.contacts[j].hostile;assert(hostile);}
        else {bool cargo=false;for(int j=0;j<CG_CANS;j++) cargo|=cg->cans[j].life>0;assert(cargo);}
    }
    /* Paid scanning and resolved salvage are independent persistence facts. */
    rx=adsb_state_find_or_create(0xa12346);assert(rx);
    rx->last_seen_us=(int64_t)lap_millis()*1000;strcpy(rx->callsign,"ABC456");
    adsb_state_set_squawk(rx,7700);adsb_state_set_squawk(rx,7700);
    for(int i=16;i<GAME_CONTACTS;i++) cg_spawn(i,(uint32_t)i,SH_PROBE,ROLE_TRADER,l3v(0,0,0));
    cg->poll=1;cg_traffic(0);cg->v.poi_count=0;cg->poi.radio_poll=1;tick(1);
    uint32_t id=cg_hash(0xa12346u^7700u);p=NULL;
    for(int i=0;i<cg->v.poi_count;i++) if(cg->v.pois[i].source_id==id) p=&cg->v.pois[i];
    assert(p);cg->v.player.position=p->position;tick(80);assert(p->scanned && !p->answered);
    cash=cg->v.player.credits;count=cg->v.discoveries.count;
    assert(game_explore_save() && cg_poi_load());cg_poi_arrive();
    cg->v.poi_count=0;cg->poi.radio_poll=1;tick(1);p=NULL;
    for(int i=0;i<cg->v.poi_count;i++) if(cg->v.pois[i].source_id==id) p=&cg->v.pois[i];
    assert(p && p->scanned && !p->answered);
    cg->v.contacts[23].active=false;tick(1);assert(p->answered);
    assert(cg->v.player.credits==cash && cg->v.discoveries.count==count);
    puts("signals: actual 7700/7600/7500 aircraft, bearing, rescue/salvage/ambush and no replay payout");
}
static void salvage(void) {
    game_poi *p=only(POI_WRECK);p->variant=1;cg->v.player.cargo[0]=1;
    for(int i=16;i<GAME_CONTACTS;i++) cg_spawn(i,(uint32_t)i,SH_PROBE,ROLE_TRADER,l3v(0,0,100));
    tick(80);assert(p->scanned && !p->answered);
    int cans=0;for(int i=0;i<CG_CANS;i++) cans+=cg->cans[i].life>0;assert(cans==3);
    cg->v.contacts[23].active=false;tick(1);assert(p->answered && cg->v.contacts[23].hostile);
    assert(cg->v.found_eggs&64);p=only(POI_WRECK);p->variant=2;cg->v.player.missiles=1;
    tick(80);assert(p->answered && cg->v.player.missiles==2);tick(100);assert(cg->v.player.missiles==2);
    p=only(POI_RUINS);p->variant=0;tick(80);assert(p->answered);
    p=only(POI_GENERATION);p->variant=0;unsigned cash=cg->v.player.credits;tick(80);
    assert(p->answered && cg->v.player.credits>=cash+6000 && strlen(p->scan_text[0])<=30);
    puts("salvage: bounded canisters, occupied contact retry, hostile sentry, one missile cache, ruins and ark lore");
}
static void weather(void) {
    game_poi *p=only(POI_ION_STORM);l3_vec position=p->position;tick(1);
    assert(cg->v.scanner_scrambled && cg->v.player.energy<100 && p->scan_progress==0);
    assert(cg_len(l3_sub(p->position,position))>0);cg->v.player.position=l3_add(p->position,l3v(p->radius+50,0,0));
    tick(80);assert(!cg->v.scanner_scrambled && p->scanned);
    p=only(POI_PULSAR);tick(1);assert(cg->v.player.energy<100);tick(80);assert(p->scanned);
    p=only(POI_BINARY);tick(80);assert(p->scanned && (cg->v.found_eggs&128));
    puts("weather: moving ion region drains and blocks scans; pulsar drains; binary scan/harmony grant");
}
static void scan(void) {
    game_poi *p=only(POI_BINARY);unsigned cash=cg->v.player.credits;cg->v.player.speed=2;
    tick(30);assert(p->discovered && !p->scanned && cg->v.poi_scan_progress>0);
    cg->v.player.position=l3_add(p->position,l3v(2000,0,0));tick(1);assert(p->scan_progress==0);
    cg->v.player.position=p->position;tick(76);
    assert(p->scanned && cg->v.discoveries.count==1 && cg->v.player.credits==cash+550);
    assert(!strcmp(cg->v.discoveries.names[0],p->name));tick(100);assert(cg->v.player.credits==cash+550);
    assert(game_explore_save());assert(cg_poi_load());assert(cg->v.discoveries.count==1);
    /* Arrive in many real systems and verify the rolling last-16 contract. */
    for(int i=0;i<60 && cg->v.discoveries.count<20;i++) {
        cg_arrive(i);if(!cg->v.poi_count) continue;p=&cg->v.pois[0];
        p->kind=POI_BINARY;p->velocity=l3v(0,0,0);cg->v.player.position=p->position;tick(76);
    }
    assert(cg->v.discoveries.count>=20 && cg->v.discoveries.names[15][0] && cg->v.discoveries.rank>=3);
    assert(!strcmp(game_explorer_rank(0),"Drifter") && !strcmp(game_explorer_rank(8),"Cartographer"));
    puts("scan: continuous hold, one payout, last sixteen names and nine configurable Explorer ranks");
}
static void persistence(void) {
    game_poi *p=only(POI_BINARY);
    int system=-1;for(int i=0;i<256;i++) {
        cg_arrive(i);if(cg->v.poi_count && cg->v.pois[0].kind==POI_BINARY) {system=i;break;}
    }
    assert(system>=0);p=&cg->v.pois[0];cg->v.player.position=p->position;tick(80);assert(p->scanned);
    cg->v.relic_chain=3;cg->v.found_eggs=17;cg->poi.route_step=2;assert(game_explore_save());
    unsigned char saved[1536];int fd=lap_file_open("explore.sav","r");assert(fd>=0);
    assert(lap_file_read(fd,saved,sizeof(saved))==sizeof(saved));lap_file_close(fd);
    assert(cg_poi_load() && cg->v.relic_chain==3 && cg->v.found_eggs==17 && cg->poi.route_step==2);
    assert(cg->v.discoveries.count==1 && cg->poi.marks[cg->v.system].scanned==1);
    /* GPS can move between sessions; game_init visits GPS before commander load. */
    uint32_t galaxy=cg->v.galaxy_seed,sequence=cg->save_seq;
    cg->save_seq=0;cg->v.galaxy_seed^=12345;game_universe(cg->v.galaxy_seed,cg->v.systems);cg_arrive(0);
    assert(!cg->v.discoveries.count);
    cg->v.galaxy_seed=galaxy;game_universe(galaxy,cg->v.systems);cg_arrive(system);cg->save_seq=sequence;
    assert(cg->v.discoveries.count==1 && cg->v.relic_chain==3 && cg->v.found_eggs==17);
    assert(lap_file_remove("explore.sav"));assert(!cg_poi_load() && !cg->v.discoveries.count);
    write_file("explore.sav","CXP0",4);assert(!cg_poi_load() && !cg->v.relic_chain);
    write_file("explore.sav",saved,1535);assert(!cg_poi_load());
    saved[40]^=1;write_file("explore.sav",saved,1536);assert(!cg_poi_load());saved[40]^=1;
    write_file("explore.sav",saved,1536);cg->v.galaxy_seed++;assert(!cg_poi_load());cg->v.galaxy_seed--;
    assert(cg_poi_load());cg_poi_arrive();assert(cg->v.pois[0].scanned);
    puts("persistence: exact round trip, discovery masks, quest/eggs/route, missing, old, truncated, corrupt and foreign files");
}
static void eggs(void) {
    int toothy=0,beacon=0,night=0;uint32_t seed=cg->v.galaxy_seed;
    for(int i=0;i<256;i++) {
        cg_arrive(i);
        if(!cg->v.poi_count) continue;
        game_poi *p=&cg->v.pois[0];cg->v.player.position=p->position;
        if(p->kind==POI_TOOTHY) {toothy++;tick(80);assert(cg->v.found_eggs&1);}
        if(p->kind==POI_BEACON) {beacon++;tick(80);assert(cg->v.found_eggs&4);}
        if(p->kind==POI_RELIC && p->variant==1) {
            night++;hour=2;tick(130);assert(!p->answered);hour=3;tick(126);
            assert(p->answered && (cg->v.found_eggs&16));
            p->answered=false;p->hold_progress=0;hour=4;tick(130);assert(!p->answered);
        }
    }
    assert(toothy==1 && beacon==1 && night==1);
    cg_arrive(0);assert(!cg->v.hidden_system && !cg->v.hidden_system_found);
    assert(cg->poi.route[2]>0);int route[3];memcpy(route,cg->poi.route,sizeof(route));
    cg_poi_route(route[1]);cg_arrive(route[1]);assert(!cg->v.hidden_system);
    cg_arrive(0);
    for(int i=0;i<3;i++) {
        assert(game_distance(&cg->v.systems[cg->v.system],&cg->v.systems[route[i]])<=7);
        cg_poi_route(route[i]);cg_arrive(route[i]);
    }
    assert(cg->v.hidden_system && cg->v.hidden_system_found && (cg->v.found_eggs&2));
    assert(!strcmp(cg->v.exploration_station,"Codfather's Rest") && cg->v.pois[0].kind==POI_GENERATION);
    assert(cg->v.galaxy_seed==seed);assert(game_explore_save());
    /* Reproduce game_init: first arrival at zero, then commander arrival. */
    cg->poi.loaded=false;cg_arrive(0);cg_arrive(route[2]);assert(cg->v.hidden_system);
    game_poi *p=only(POI_BINARY);(void)p;cg->v.poi_count=0;
    cg->v.player.position=l3_add(cg->v.contents.star,l3v(cg->v.contents.star_radius+30,0,0));
    tick(249);assert(!(cg->v.found_eggs&8));tick(2);assert(cg->v.found_eggs&8);
    unsigned cash=cg->v.player.credits;tick(300);assert(cg->v.player.credits==cash);
    puts("eggs: unique Toothy/beacon/night stone, local hour boundaries, reachable secret sequence, fish harbour, reload and star stillness");
}
static void config(void) {
    assert(cg_poi_config());float scan=cg->poi.rules.scan_seconds;cg_poi_rules expected=cg->poi.rules;
    const char *bad[]={"{\"version\":0}","{\"version\":1,\"version\":1}","{\"version\":NaN}","{}"};
    for(int i=0;i<4;i++) {write_file("explore.json",bad[i],(int)strlen(bad[i]));assert(!cg_poi_config());assert(cg->poi.rules.scan_seconds==scan);}
    assert(lap_file_remove("explore.json"));assert(cg_poi_config());
    FILE *f=fopen(EXPLORE_DEFAULTS,"rb");assert(f);char b[8192];int n=(int)fread(b,1,sizeof(b),f);fclose(f);
    write_file("explore.json",b,n);assert(cg_poi_config() && !memcmp(&expected,&cg->poi.rules,sizeof(expected)));
    puts("config: missing defaults, shipped JSON, old/duplicate/nonfinite/missing schema rejection and safe fallback");
}
static void limits(void) {
    only(POI_BINARY);
    for(int i=1;i<6;i++) {
        cg->v.pois[i]=cg->v.pois[0];cg->v.pois[i].position=l3_add(cg->v.player.position,l3v(i*3000,0,0));
        cg->v.pois[i].source_id=(uint32_t)i; /* out-of-range radio-like entries */
    }
    cg->v.poi_count=6;unsigned start=allocations;clock_t before=clock();
    const int steps=1000000;tick(steps);double elapsed=(double)(clock()-before)/CLOCKS_PER_SEC;
    assert(start==allocations);assert(sizeof(cg->poi.buffer)==1536);
    printf("limits: six POIs, zero allocations across discovery and %d ticks; %.3f us/tick; exploration pool %u bytes\n",
        steps,elapsed*1000000/steps,(unsigned)sizeof(cg->poi));
}
int main(int argc,char **argv) {
    assert(argc==2);adsb_state_init();ls_lap_api_begin("carrier-poi",1,0);ls_lap_sys_begin();ls_lap_api_callback(true);
    assert(game_init() && cg->v.config_valid && cg->poi.configured);
    if(!strcmp(argv[1],"generation")) generation();else if(!strcmp(argv[1],"nebula")) nebula();
    else if(!strcmp(argv[1],"gravity")) gravity();else if(!strcmp(argv[1],"comet")) comet();
    else if(!strcmp(argv[1],"relics")) relics();else if(!strcmp(argv[1],"signals")) signals();
    else if(!strcmp(argv[1],"salvage")) salvage();else if(!strcmp(argv[1],"weather")) weather();
    else if(!strcmp(argv[1],"scan")) scan();else if(!strcmp(argv[1],"persistence")) persistence();
    else if(!strcmp(argv[1],"eggs")) eggs();else if(!strcmp(argv[1],"config")) config();
    else if(!strcmp(argv[1],"limits")) limits();else assert(0);
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
