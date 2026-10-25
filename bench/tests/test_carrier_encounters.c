/* Encounter director outcomes using real contact/flight/combat pools.
 * Receiver and heap wrappers make mode boundaries and allocation bounds exact. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
#include <time.h>
void cg_render_reset(void) {} /* This backend fixture does not create a scene. */
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
static unsigned allocations,checks,radio_calls;
void *__wrap_malloc(size_t n) {allocations++;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t z) {allocations++;return __real_calloc(n,z);}
void *__wrap_realloc(void *p,size_t n) {allocations++;return __real_realloc(p,n);}
static lap_aircraft_t aircraft[16];
static int aircraft_count;
int __wrap_lap_aircraft(lap_aircraft_t *out,int max) {
    radio_calls++;int n=aircraft_count<max?aircraft_count:max;
    memcpy(out,aircraft,(size_t)n*sizeof(*out));return n;
}
#define CHECK(c) do {checks++;assert(c);} while(0)
static void empty(void) {
    memset(cg->v.contacts,0,sizeof(cg->v.contacts));
    memset(cg->encounter,0,sizeof(cg->encounter));
    memset(cg->dead_until,0,sizeof(cg->dead_until));
    memset(cg->mapped,0,sizeof(cg->mapped));
    memset(cg->cans,0,sizeof(cg->cans));
    for(int i=0;i<GAME_CONTACTS;i++) cg->encounter_enemy[i]=-2;
}
static void setup(void) {
    adsb_state_init();ls_lap_api_begin("carrier-encounters",1,0);ls_lap_sys_begin();ls_lap_api_callback(true);
    CHECK(game_init());CHECK(cg->v.config_valid);
    CHECK(cg->v.traffic_mode==0 && cg->v.encounter_level==1);
    empty();game_player *p=&cg->v.player;p->docked=false;p->dead=false;p->position=l3v(0,0,1100);
    p->velocity=l3v(0,0,0);p->throttle=0;p->legal=0;
    memset(p->cargo,0,sizeof(p->cargo));
    cg_encounters(0);cg->ambient_due=100000;cg->encounter_due=100000;
}
static void second(void) {cg->v.clock+=1;cg_encounters(1);}
static unsigned cadence_run(int level,bool deep,uint32_t *digest) {
    empty();cg->encounter_system=-1;cg->v.player.position=l3v(0,0,deep?4000:1100);
    game_set_encounters(level);cg_encounters(0);cg->ambient_due=100000;
    unsigned before=cg->encounter_count;uint32_t hash=0;
    for(int s=0;s<1800;s++) {
        second();
        for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active)
            hash=cg_hash(hash^(uint32_t)cg->encounter[i].kind^(uint32_t)s);
        empty();
    }
    *digest=hash;return cg->encounter_count-before;
}
static void cadence(void) {
    unsigned counts[3];uint32_t a,b;
    for(int level=0;level<3;level++) {
        counts[level]=cadence_run(level,false,&a);
        CHECK(cadence_run(level,false,&b)==counts[level]);CHECK(a==b);
        int lo=level==0?180:level==1?60:25,hi=level==0?300:level==1?120:45;
        CHECK(counts[level]>=1800u/(unsigned)(hi+1));CHECK(counts[level]<=1800u/(unsigned)lo);
        printf("level %d: %u encounters / 30 min\n",level,counts[level]);
    }
    CHECK(counts[0]<counts[1] && counts[1]<counts[2]);
    CHECK(cadence_run(1,true,&a)>counts[1]);
    cg->encounter_due=10;cg->v.player.docked=true;
    for(int i=0;i<300;i++) second();CHECK(cg->encounter_due==10);
    cg->v.player.docked=false;cg->v.jump=JUMP_CHARGE;
    for(int i=0;i<300;i++) second();CHECK(cg->encounter_due==10);
    cg->v.jump=JUMP_IDLE;cg->v.player.position=l3v(0,0,-100);empty();
    for(int i=CE_PIRATES;i<=CE_SALVAGE;i++)
        if(i!=CE_CONVOY && i!=CE_PATROL) CHECK(!cg_encounter_spawn((cg_encounter_kind)i));
    CHECK(cg_encounter_spawn(CE_PATROL));
}
static int count(void) {int n=0;for(int i=0;i<GAME_CONTACTS;i++) n+=cg->v.contacts[i].active;return n;}
static void modes(void) {
    game_set_traffic(1);unsigned polls=radio_calls;
    cg->encounter_due=1;cg->ambient_due=0;
    for(int i=0;i<200;i++) {cg->v.clock+=.04f;cg_traffic(.04f);}
    CHECK(radio_calls==polls);CHECK(count()>0);CHECK(cg->encounter_count>0);
    game_set_traffic(2);CHECK(count()==0);unsigned start=cg->encounter_count;
    for(int i=0;i<600;i++) {second();cg->poll=1;cg_traffic(0);}
    CHECK(count()==0);CHECK(cg->encounter_count==start);
    cg_spawn(0,12345,SH_KRAIT,ROLE_PIRATE,l3v(0,0,1000));CHECK(count()==0);
    CHECK(cg_register(&cg->v.contacts[0])==-1);
    aircraft_count=2;aircraft[0]=(lap_aircraft_t){.icao=0x123456,.range_km=1};
    strcpy(aircraft[0].callsign,"ABC123");strcpy(aircraft[0].category,"A3");
    aircraft[1]=(lap_aircraft_t){.icao=0xF00000,.range_km=1};
    cg->poll=1;cg_traffic(0);CHECK(count()==1 && cg->v.contacts[0].live);
    game_set_traffic(1);CHECK(count()==0);game_set_traffic(0);cg->poll=1;cg_traffic(0);CHECK(count()==2);
    game_set_traffic(-1);game_set_encounters(3);CHECK(cg->v.traffic_mode==0 && cg->v.encounter_level==1);
}
static void pool(void) {
    game_set_traffic(1);empty();unsigned start=allocations;
    for(int run=0;run<4000;run++) {
        cg_encounter_spawn((cg_encounter_kind)(1+run%CG_ENCOUNTER_KINDS));
        CHECK(count()<=GAME_CONTACTS);
        if(run%17==0) empty();
    }
    CHECK(allocations==start);empty();
    for(int i=0;i<GAME_CONTACTS;i++) cg_spawn(i,cg->next_id++,SH_BOA,ROLE_TRADER,l3v(i*30,0,1100));
    CHECK(count()==GAME_CONTACTS);CHECK(!cg_encounter_spawn(CE_DRONES));
    game_set_traffic(0);aircraft_count=16;
    for(int i=0;i<16;i++) {aircraft[i]=(lap_aircraft_t){.icao=(uint32_t)(0x100000+i),.range_km=2};strcpy(aircraft[i].category,"A3");}
    cg->poll=1;cg_traffic(0);int live=0;
    for(int i=0;i<GAME_CONTACTS;i++) live+=cg->v.contacts[i].active && cg->v.contacts[i].live;
    CHECK(live==16 && count()==24);
    for(int run=0;run<30;run++) {cg->poll=1;cg_traffic(0);CHECK(count()==24);}
    for(int i=0;i<GAME_CONTACTS;i++) {cg->v.contacts[i].live=true;cg->mapped[i]=true;}
    cg->v.contacts[23].live=false;cg->v.contacts[23].id=0xF00000;
    aircraft_count=1;aircraft[0].icao=0x123abc;cg->poll=1;cg_traffic(0);
    CHECK(cg->v.contacts[23].id==0x123abc && cg->v.contacts[23].live && count()==24);
}
static void formations(void) {
    CHECK(cg_encounter_spawn(CE_CONVOY));int leader=-1;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->encounter[i].kind==CE_CONVOY && !cg->encounter[i].leader) leader=i;
    CHECK(leader>=0);uint32_t id=cg->v.contacts[leader].id;
    for(int frame=0;frame<600;frame++) {
        cg_flight(.04f);
        for(int i=0;i<GAME_CONTACTS;i++) if(cg->encounter[i].leader==id) {
            l3_vec expected=l3_add(cg->v.contacts[leader].position,l3_quat_rotate(cg->v.contacts[leader].attitude,cg->encounter[i].offset));
            CHECK(cg_len(l3_sub(cg->v.contacts[i].position,expected))<.01f);
        }
    }
    int victim=leader;for(int i=0;i<GAME_CONTACTS;i++) if(cg->encounter[i].leader==id && !cg->encounter[i].escort) {victim=i;break;}
    cg_damage(victim,1,cg->v.player.position,true);
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].id==id || cg->encounter[i].leader==id) CHECK(cg->encounter[i].fleeing);
    /* Destroy/reuse leader slot: old wingmen must not follow its replacement. */
    cg->v.contacts[leader].active=false;cg_spawn(leader,cg->next_id++,SH_ADDER,ROLE_TRADER,l3v(0,0,1100));
    cg_flight(.04f);
    for(int i=0;i<GAME_CONTACTS;i++) CHECK(cg->encounter[i].leader!=id);
}
static void engagement(void) {
    empty();cg->v.player.position.y=-200; /* Keep the commander outside the NPC firing ray. */
    cg_spawn(16,10001,SH_VIPER,ROLE_POLICE,l3v(0,0,1100));
    cg_spawn(17,10002,SH_KRAIT,ROLE_PIRATE,l3v(100,0,1100));
    cg_spawn(18,10003,SH_BOA,ROLE_TRADER,l3v(80,0,1100));
    CHECK(cg_encounter_enemy(16)==17);CHECK(cg_encounter_enemy(17)==16);
    float shield=cg->v.contacts[17].shield,trader_shield=cg->v.contacts[18].shield;
    cg_combat(.04f);
    CHECK(cg->v.contacts[17].shield==shield && cg->v.contacts[18].shield<trader_shield); /* Collision rays now strike the intervening trader first. */
    CHECK(cg->v.contacts[16].weapons[0].firing);
    cg->v.contacts[18].position.y+=200;
    for(int i=0;i<25;i++) cg_combat(.04f);
    CHECK(cg->v.contacts[17].shield<shield);
    empty();CHECK(cg_encounter_spawn(CE_PATROL));
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active) cg->v.contacts[i].position=l3_add(cg->v.player.position,l3v(0,0,-100));
    cg->v.player.cargo[5]=1;for(int i=0;i<4;i++) second();CHECK(cg->v.player.legal>=20);
    int police=-1;for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active) police=i;
    CHECK(cg_encounter_enemy(police)==-1);
    empty();cg->v.player.legal=0;CHECK(cg_encounter_spawn(CE_DRONES));
    cg_spawn(0,333,SH_VIPER,ROLE_POLICE,l3v(0,0,1100));second();
    int drone=-1;for(int i=0;i<GAME_CONTACTS;i++) if(cg->encounter[i].kind==CE_DRONES) drone=i;
    CHECK(drone>=0 && cg_encounter_enemy(drone)==0);CHECK(cg_encounter_enemy(0)>=0);
    CHECK(cg->v.contacts[drone].hull==game_ship(SH_DRONE)->hull*2);
    empty();CHECK(cg_encounter_spawn(CE_SMUGGLERS));
    int smuggler=-1;police=-1;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active) {
        if(cg->v.contacts[i].role==ROLE_POLICE) police=i;else smuggler=i;
    }
    CHECK(smuggler>=0 && police>=0);unsigned cash=cg->v.player.credits;
    cg_damage(smuggler,10000,cg->v.player.position,true);
    CHECK(cg->v.player.legal==0 && cg->v.player.credits>cash);
    cg_damage(police,1,cg->v.player.position,true);CHECK(cg->v.player.legal>0);
}
static void departures(void) {
    CHECK(cg_encounter_spawn(CE_EXPLORER));int slot=-1;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active) slot=i;
    CHECK(slot>=0);cg->v.contacts[slot].position=cg->anchor[slot];
    cg_flight(.04f);CHECK(cg->v.contacts[slot].active && cg->v.contacts[slot].jump_out>.7f);
    for(int i=0;i<10;i++) cg_encounters(.04f);CHECK(cg->v.contacts[slot].active && cg->v.contacts[slot].jump_out>0);
    for(int i=0;i<12;i++) cg_encounters(.04f);CHECK(!cg->v.contacts[slot].active && cg->v.contacts[slot].jump_out==0);
    empty();CHECK(cg_encounter_spawn(CE_CONVOY));int lead=-1;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active && !cg->encounter[i].leader) lead=i;
    CHECK(lead>=0);cg_depart(lead);cg_flight(.04f);
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active) CHECK(cg->v.contacts[i].jump_out>0);
    /* Aircraft loss has a departure signal too. */
    empty();aircraft_count=1;aircraft[0]=(lap_aircraft_t){.icao=0x100001,.range_km=1};
    cg->poll=1;cg_traffic(0);aircraft_count=0;cg->contact_age[0]=31;cg->poll=-100;
    cg_traffic(.04f);CHECK(cg->v.contacts[0].active && cg->v.contacts[0].jump_out>0);
}
static void life(void) {
    for(int kind=CE_PIRATES;kind<=CE_SALVAGE;kind++) {
        empty();cg->v.contents.belt=true;cg->v.asteroids[0]=(game_asteroid){l3v(0,0,1700),20,40,true};
        CHECK(cg_encounter_spawn((cg_encounter_kind)kind));CHECK(count()>=1 && count()<=6);
        if(kind==CE_DISTRESS) for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].active) CHECK(cg->v.contacts[i].role==ROLE_RESCUE);
        if(kind==CE_DERELICT || kind==CE_SALVAGE) {int n=0;for(int i=0;i<CG_CANS;i++) n+=cg->cans[i].life>0;CHECK(n==3);}
        if(kind==CE_PROSPECTOR) {
            for(int i=0;i<55;i++) second();CHECK(!cg->v.asteroids[0].active);
        }
    }
    empty();cg->ambient_due=0;for(int i=0;i<60;i++) second();CHECK(count()>=2 && count()<=5);
    unsigned before=allocations;clock_t start=clock();
    for(int frame=0;frame<15000;frame++) {cg->v.clock+=.04f;cg_traffic(.04f);cg_flight(.04f);}
    CHECK(allocations==before);
    printf("15000 traffic/flight frames: %.2f us/frame on host; state %u bytes\n",
        1e6*(double)(clock()-start)/CLOCKS_PER_SEC/15000,(unsigned)sizeof(*cg));
}
static void config(void) {
    CHECK(CG_CHATTER_LINES>=60);
    for(int i=0;i<CG_CHATTER_LINES;i++) {
        CHECK(strlen(cg->encounters.chatter[i])>0 && strlen(cg->encounters.chatter[i])<=40);
        for(int j=0;j<i;j++) CHECK(strcmp(cg->encounters.chatter[i],cg->encounters.chatter[j]));
    }
    for(int i=0;i<CG_ENCOUNTER_KINDS;i++) CHECK(strlen(cg->encounters.messages[i])<=40);
    int fd=lap_file_open("encounters.json","w");CHECK(fd>=0);
    const char *bad="{\"version\":2}";CHECK(lap_file_write(fd,bad,(int)strlen(bad))==(int)strlen(bad));lap_file_close(fd);
    CHECK(!cg_encounter_config());CHECK(cg->encounters.cadence[0]==180 && cg->encounters.cadence[5]==45);
    empty();cg->v.player.position=l3v(0,0,1100);CHECK(cg_encounter_spawn(CE_CONVOY));
    cg->chatter_due=0;second();int last=cg->last_chatter;CHECK(last>=0);
    for(int i=0;i<30;i++) {cg->chatter_due=0;second();CHECK(cg->last_chatter!=last);last=cg->last_chatter;CHECK(strlen(cg->v.messages[0])<=40);}
}
int main(int argc,char **argv) {
    CHECK(argc==2);setup();
    if(!strcmp(argv[1],"cadence")) cadence();else if(!strcmp(argv[1],"modes")) modes();
    else if(!strcmp(argv[1],"pool")) pool();else if(!strcmp(argv[1],"formations")) formations();
    else if(!strcmp(argv[1],"engagement")) engagement();else if(!strcmp(argv[1],"departures")) departures();
    else if(!strcmp(argv[1],"life")) life();else if(!strcmp(argv[1],"config")) config();else CHECK(false);
    printf("encounters %s: %u checks passed\n",argv[1],checks);
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
