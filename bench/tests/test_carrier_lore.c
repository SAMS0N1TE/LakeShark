/* Lore contracts through production data, contacts, missions and file cache. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
void cg_render_reset(void) {}
static void strings(void) {
    unsigned unique=0;
    for(int i=0;i<GAME_SYSTEMS;i++) {
        assert(strlen(cg->v.systems[i].description)<=86);
        bool first=true;
        for(int j=0;j<i;j++) if(!strcmp(cg->v.systems[i].description,cg->v.systems[j].description)) first=false;
        unique+=first;
        game_contents c;game_system_contents(cg->v.systems[i].seed,&c);
        assert(c.station_name[0] && strlen(c.station_name)<20);
    }
    assert(unique>=230);printf("descriptions: %u/256 distinct\n",unique);
    uint32_t galaxy=cg->v.galaxy_seed;
    for(unsigned sample=0;sample<8;sample++) {
        game_universe(cg_hash(sample),cg->v.systems);unsigned distinct=0;
        for(int i=0;i<GAME_SYSTEMS;i++) {
            assert(strlen(cg->v.systems[i].description)<=86);bool first=true;
            for(int j=0;j<i;j++) if(!strcmp(cg->v.systems[i].description,cg->v.systems[j].description)) first=false;
            distinct+=first;
        }
        assert(distinct>=230);
    }
    game_universe(galaxy,cg->v.systems);
    for(int e=0;e<8;e++) {
        cg->v.systems[cg->v.system].economy=e;
        for(int g=0;g<GAME_GOODS;g++) assert(strlen(game_good_note(g))>0 && strlen(game_good_note(g))<=30);
    }
    assert(!*game_good_note(-1) && !*game_good_note(GAME_GOODS));
    for(int r=0;r<6;r++) {
        char seen[12][31]={{0}};int n=0;
        for(uint32_t id=1;id<2000 && n<12;id++) {
            cg->v.player.docked=false;cg->v.player.legal=0;memset(cg->v.player.cargo,0,sizeof(cg->v.player.cargo));
            cg_spawn(0,id,SH_ADDER,(game_role)r,l3v(0,0,1100));cg->v.player.position=l3v(0,0,1100);
            cg_talk_open(0);game_talk *t=&cg->v.talk;
            assert(t->open && t->line_count<=3 && t->choice_count<=3);
            for(int j=0;j<t->line_count;j++) assert(strlen(t->lines[j])<=30);
            for(int j=0;j<t->choice_count;j++) assert(strlen(t->choices[j])<=12);
            bool found=false;for(int j=0;j<n;j++) if(!strcmp(seen[j],t->lines[0])) found=true;
            if(!found) {assert(n<12);strcpy(seen[n++],t->lines[0]);}
        }
        assert(n==12);printf("role %d: 12/12 openings reachable\n",r);
    }
    for(int outcome=0;outcome<7;outcome++) for(int variant=0;variant<4;variant++)
        assert(strlen(cg_lore_outcome(outcome,variant))<=30);
    game_talk_close();
    for(uint32_t day=0;day<20;day++) {
        cg->v.market_day=day;cg_system_arrive();char news[4][41];memcpy(news,cg->v.news,sizeof(news));
        cg->rng+=17;cg->v.missions[0].accepted=true;cg_news();assert(!memcmp(news,cg->v.news,sizeof(news)));
        cg->v.missions[0].accepted=false;
        for(int j=0;j<4;j++) assert(strlen(news[j])>0 && strlen(news[j])<=40);
        for(int j=0;j<6;j++) assert(strlen(cg->v.missions[j].title)<=24 && strlen(cg->v.missions[j].brief)<=60);
    }
    puts("string limits, 28 outcome variants, deterministic daily news: passed");
}
static game_mission *offer(game_mission_kind kind) {
    memset(cg->v.missions,0,sizeof(cg->v.missions));memset(cg->v.contacts,0,sizeof(cg->v.contacts));
    memset(cg->cans,0,sizeof(cg->cans));memset(cg->dead_until,0,sizeof(cg->dead_until));
    memset(cg->v.player.cargo,0,sizeof(cg->v.player.cargo));
    cg->v.player.docked=true;cg->v.player.dead=false;cg->v.player.equipment|=1u<<EQ_SCOOP;
    cg->v.market_day=0;cg->v.system=0;game_system_contents(cg->v.systems[0].seed,&cg->v.contents);
    game_mission *m=&cg->v.missions[4];m->kind=kind;m->destination=0;m->amount=2;
    m->good=kind==MISSION_SMUGGLING?5:11;m->target_id=kind==MISSION_SURVEY?1:0x71234567;
    m->reward=700;m->deadline=4;strcpy(m->title,"A test contract");strcpy(m->brief,"Original test briefing.");
    return m;
}
static void round_trip(void) {
    game_mission saved[6];memcpy(saved,cg->v.missions,sizeof(saved));
    assert(game_save());assert(!memcmp(cg->save_buffer,"CRR3",4));
    memset(cg->v.missions,0,sizeof(saved));assert(game_load());assert(!memcmp(saved,cg->v.missions,sizeof(saved)));
}
static int target_slot(game_mission *m) {
    for(int j=0;j<GAME_CONTACTS;j++) if(cg->v.contacts[j].active && cg->v.contacts[j].id==m->target_id) return j;
    return -1;
}
static void contracts(void) {
    for(int kind=4;kind<=8;kind++) {
        game_mission *m=offer((game_mission_kind)kind);int cargo=game_cargo_used();
        assert(game_mission_accept(4));
        if(kind==4) assert(game_cargo_used()==cargo && game_passenger_seats()==2);
        if(kind==7) assert(cg->v.player.cargo[5]==2);
        round_trip();m=&cg->v.missions[4];cg->v.player.docked=false;
        if(kind==5) {
            cg->v.player.position=l3_add(cg->v.contents.planets[0].position,l3v(cg->v.contents.planets[0].radius+300,0,0));
            cg_mission_observe();assert(m->ready);
        } else if(kind==6 || kind==8) {
            cg->sensor_seconds=1;cg_system_update(0);int j=target_slot(m);assert(j>=0);
            assert(cg->v.contacts[j].role==(kind==6?ROLE_DERELICT:ROLE_TRADER));
            if(kind==6) {
                cg->v.player.position=cg->v.contacts[j].position;cg_system_update(0);
                assert(m->progress==2 && m->ready && cg->v.player.cargo[11]==2);
                round_trip();m=&cg->v.missions[4];assert(m->progress==2 && m->ready);
            } else {
                cg->v.contacts[j].position=l3v(0,0,-70);cg->v.player.position=l3v(0,0,-300);
                cg_mission_observe();assert(m->ready);
            }
        }
        uint32_t cash=cg->v.player.credits;cg->v.player.docked=true;cg_mission_dock();
        assert(m->done && cg->v.player.credits==cash+700);
        cash=cg->v.player.credits;cg_mission_dock();assert(cg->v.player.credits==cash);
        round_trip();
        m=offer((game_mission_kind)kind);assert(game_mission_accept(4));cg->v.market_day=5;cg_mission_dock();
        assert(m->failed && !m->done);round_trip();
        printf("mission %d: accept/complete/fail/save passed\n",kind);
    }
    game_mission *m=offer(MISSION_PASSENGER);m->amount=5;assert(!game_mission_accept(4));
    m=offer(MISSION_SURVEY);m->target_id=0;assert(game_mission_accept(4));cg->v.player.docked=false;
    cg->v.player.position=l3_add(cg->v.contents.star,l3v(cg->v.contents.star_radius+300,0,0));cg_mission_observe();assert(m->ready);
    m=offer(MISSION_ESCORT);assert(game_mission_accept(4));cg->v.player.docked=false;cg->sensor_seconds=1;cg_system_update(0);
    int j=target_slot(m);assert(j>=0);cg->v.contacts[j].hull=0;cg->v.contacts[j].active=false;cg_mission_observe();assert(m->failed);
    m=offer(MISSION_SMUGGLING);assert(game_mission_accept(4));cg->v.player.docked=false;
    cg->v.player.position=l3v(0,0,1100);cg_spawn(0,99,SH_VIPER,ROLE_POLICE,l3v(0,0,1100));cg_talk_open(0);
    assert(game_talk_choose(0) && m->failed && cg->v.player.cargo[5]==0);
    cg_spawn(0,100,SH_KRAIT,ROLE_PIRATE,l3v(0,0,1100));cg_talk_open(0);uint32_t cash=cg->v.player.credits;
    int bribe=cg->v.talk.price;assert(game_talk_choose(2));assert(cg->v.player.credits==cash-(uint32_t)bribe && cg_friendly(100));
    m=offer(MISSION_SALVAGE);assert(game_mission_accept(4));cg->v.player.docked=false;cg->v.player.position=l3v(0,0,1100);
    /* Unrelated canisters never advance the marked contract. */
    cg->cans[0]=(cg_can){cg->v.player.position,20,11,0};cg_system_update(0);assert(m->progress==0);
    cg->cans[0]=(cg_can){cg->v.player.position,20,11,m->target_id};
    cg->v.plotted=-1;cg->v.selected=-1;game_context();cg_mission_observe();assert(m->progress==1 && !m->ready);
    round_trip();m=&cg->v.missions[4];assert(m->progress==1 && !m->ready);
    m=offer(MISSION_ESCORT);assert(game_mission_accept(4));cg_arrive(1);assert(m->failed);
    offer(MISSION_DELIVERY);memset(cg->v.missions,0,sizeof(cg->v.missions));cg->v.player.docked=false;
    cg->v.player.legal=0;cg->v.player.position=l3v(0,0,1100);bool deputised=false;
    for(uint32_t id=1;id<100 && !deputised;id++) {
        cg_spawn(0,id,SH_VIPER,ROLE_POLICE,cg->v.player.position);cg_talk_open(0);
        if(cg->v.talk.amount) {assert(game_talk_choose(0));deputised=true;}
    }
    assert(deputised);bool contract=false;
    for(int i=0;i<6;i++) contract|=cg->v.missions[i].accepted && cg->v.missions[i].kind==MISSION_BOUNTY && cg->v.missions[i].brief[0];
    assert(contract);
    puts("seats, star survey, escort loss, scan confiscation, pirate bribe: passed");
}
static void legacy(void) {
    offer(MISSION_DELIVERY);assert(game_mission_accept(4));assert(game_save());
    /* CRR3 retains the exact CRR2 prefix; strip just the lore appendix. */
    unsigned char old[8192];memcpy(old,cg->save_buffer,sizeof(old));memcpy(old,"CRR2",4);
    uint32_t length=188+cg->v.registry_count*55+358,hash=2166136261u,seq=cg->save_seq+1;
    for(uint32_t i=16;i<length;i++) hash=(hash^old[i])*16777619u;
    for(int i=0;i<4;i++) {old[4+i]=length>>(8*i);old[8+i]=seq>>(8*i);old[12+i]=hash>>(8*i);}
    int fd=lap_file_open("commander0.dat","w");assert(fd>=0);assert(lap_file_write(fd,old,sizeof(old))==sizeof(old));lap_file_close(fd);
    assert(game_load() && cg->v.missions[4].accepted && cg->v.missions[4].kind==MISSION_DELIVERY);
    assert(cg->v.missions[4].brief[0] && !cg->v.missions[4].progress);
    puts("CRR2 migration: accepted contract retained (CRR1 covered by carrier_save_capacity)");
}
int main(void) {
    adsb_state_init();ls_lap_api_begin("carrier-lore",1,0);ls_lap_sys_begin();ls_lap_api_callback(true);
    assert(game_init());strings();contracts();legacy();
    printf("state: %u bytes\n",(unsigned)sizeof(*cg));
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
