/* Real flight/encounter/combat rules; no renderer or hardware boundaries. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
#include <time.h>
void cg_render_reset(void) {}
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
static unsigned allocations, checks;
void *__wrap_malloc(size_t n) { allocations++; return __real_malloc(n); }
void *__wrap_calloc(size_t n,size_t z) { allocations++; return __real_calloc(n,z); }
void *__wrap_realloc(void *p,size_t n) { allocations++; return __real_realloc(p,n); }
#define CHECK(c) do { checks++; assert(c); } while (0)
static float top(void) { return game_ship(SH_COBRA)->speed; }
static void reset(void) {
    memset(&cg->v.player,0,sizeof(cg->v.player));
    memset(cg->v.contacts,0,sizeof(cg->v.contacts));
    memset(cg->v.asteroids,0,sizeof(cg->v.asteroids));
    memset(cg->encounter,0,sizeof(cg->encounter));
    memset(cg->mapped,0,sizeof(cg->mapped));
    memset(cg->dead_until,0,sizeof(cg->dead_until));
    memset(cg->beams,0,sizeof(cg->beams));
    memset(cg->missiles,0,sizeof(cg->missiles));
    game_player *p=&cg->v.player;
    p->attitude=(l3_quat){0,0,0,1}; p->hull=p->energy=p->shield_fore=p->shield_aft=100;
    p->equipment=1u<<EQ_LASER; p->missiles=3; p->throttle=1;
    p->velocity=l3v(0,0,-top()); p->speed=top();
    cg->v.contents.station=l3v(1000000,0,0);
    cg->v.contents.star=l3v(-1000000,0,0); cg->v.contents.star_radius=100;
    cg->v.contents.planet_count=0; cg->v.jump=JUMP_IDLE; cg->v.traffic_mode=1;
    cg->v.selected=-1; cg->fire_cooldown=0;
    cg_cruise_reset();
}
static void tick(int n) { for(int i=0;i<n;i++) cg_flight(.04f); }
static void refusal(const char *why) {
    CHECK(!game_cruise(true)); CHECK(!cg->v.player.cruising);
    CHECK(!strcmp(cg->v.player.cruise_reason,why));
    CHECK(strlen(cg->v.player.cruise_reason)<=24);
}
static void refusals(void) {
    game_player *p=&cg->v.player;
    CHECK(p->cruise_factor==1 && !p->cruising);
    p->docked=true; refusal("Docked"); p->docked=false;
    p->dead=true; refusal("Commander lost"); p->dead=false;
    cg->v.jump=JUMP_CHARGE; refusal("Jumping"); cg->v.jump=JUMP_TUNNEL;
    refusal("Jumping"); cg->v.jump=JUMP_IDLE;
    p->docking=true; refusal("Docking"); p->docking=false;
    cg->v.contents.station=l3v(0,0,649); refusal("Mass locked");
    cg->v.contents.station=l3v(1000000,0,0);
    cg->v.contents.planet_count=1;
    cg->v.contents.planets[0]=(game_planet){.position={0,0,599},.radius=100};
    refusal("Mass locked"); cg->v.contents.planet_count=0;
    cg_damage(-1,1,l3v(0,0,-100),false); refusal("Shields charging");
    p->throttle=0; tick(124); refusal("Shields charging"); tick(2);
    CHECK(game_cruise(true)); CHECK(game_cruise(false));
}
static void spool(void) {
    game_player *p=&cg->v.player; CHECK(game_cruise(true));
    float prev=1;
    for(int i=1;i<=100;i++) {
        tick(1); float t=i/100.0f, expected=1+24*t*t*(3-2*t);
        CHECK(fabsf(p->cruise_factor-expected)<.001f);
        CHECK(p->cruise_factor>=prev && p->cruise_factor<=25); prev=p->cruise_factor;
    }
    CHECK(fabsf(p->speed-top()*25)<.1f); tick(100);
    CHECK(p->cruise_factor==25 && p->cruise_charge==1);
    game_throttle(.25f); tick(150); CHECK(fabsf(p->speed-top()*25*.25f)<1);
    game_throttle(0); tick(200); CHECK(p->speed<.02f && p->cruising);
    reset(); p->pitch=1; tick(1); l3_vec normal=cg_forward();
    reset(); p->pitch=1; CHECK(game_cruise(true)); tick(1);
    CHECK(fabsf(cg_forward().y/normal.y-.4f)<.001f);
    cg_spawn(16,cg->next_id++,SH_KRAIT,ROLE_PIRATE,l3v(0,0,-100));
    cg->v.selected=16; game_fire(true); cg_combat(.04f);
    CHECK(p->firing && !p->cruising && cg->cruise.bleed>0);
    CHECK(!strcmp(p->cruise_reason,"Weapons hot"));
    CHECK(!game_cruise(true));
}
static void safety(void) {
    game_player *p=&cg->v.player;
    CHECK(game_cruise(true)); tick(100);
    cg->v.contents.station=l3_add(p->position,l3v(649,0,0)); tick(1);
    CHECK(!p->cruising && !strcmp(p->cruise_reason,"Mass locked"));
    for(int body=0;body<3;body++) {
        for(int rate=1;rate<=4;rate++) {
            reset(); CHECK(game_cruise(true)); tick(100);
            l3_vec centre=l3_add(p->position,l3v(0,0,-15000));
            if(body==0) cg->v.contents.station=centre;
            else if(body==1) {
                cg->v.contents.planet_count=1;
                cg->v.contents.planets[0]=(game_planet){.position=centre,.radius=1000};
                strcpy(cg->v.systems[cg->v.system].name,"Esker Reach");
            } else {cg->v.contents.star=centre;cg->v.contents.star_radius=1000;}
            bool dropped=false;
            for(int i=0;i<1000;i++) {
                cg_flight(.01f*rate);
                if(!p->cruising) {dropped=true;game_throttle(0);}
                CHECK(p->hull==100 && !p->dead);
                CHECK(cg_len(l3_sub(p->position,centre))>(body?1015:48));
            }
            CHECK(dropped && p->cruise_factor==1);
            CHECK(!strcmp(p->cruise_reason,body==0?"Station approach":body==1?"Too close to Esker Reach":"Star approach"));
        }
    }
    reset(); CHECK(game_cruise(true)); tick(100);
    cg->v.contents.planet_count=1;
    cg->v.contents.planets[0]=(game_planet){.position=l3_add(p->position,l3v(0,0,599)),.radius=100};
    tick(1); CHECK(!p->cruising && p->mass_locked);
    reset(); CHECK(game_cruise(true)); cg->v.jump=JUMP_CHARGE; tick(1); CHECK(!p->cruising);
    reset(); CHECK(game_cruise(true)); p->docked=true; tick(1); CHECK(!p->cruising);
    reset(); CHECK(game_cruise(true)); p->dead=true; tick(1); CHECK(!p->cruising);
}
static void bleed(void) {
    game_player *p=&cg->v.player;
    CHECK(game_cruise(true)); tick(100); float speed=p->speed;
    CHECK(game_cruise(false)); CHECK(p->speed==speed && p->cruise_factor==25);
    tick(1); CHECK(p->speed<speed && p->speed>top()*20);
    float last=p->speed;
    for(int i=0;i<37;i++) {tick(1);CHECK(p->speed<=last+.001f);last=p->speed;}
    CHECK(p->cruise_factor==1 && fabsf(p->speed-top())<.01f);
    CHECK(game_cruise(true)); tick(50); CHECK(game_cruise(false)); tick(10);
    float factor=p->cruise_factor; CHECK(game_cruise(true)); tick(1);
    CHECK(p->cruise_factor>=factor && p->cruise_factor<25);
}
static void eta(void) {
    game_player *p=&cg->v.player;
    l3_vec target=l3_add(p->position,l3v(0,0,-11000));
    CHECK(fabsf(game_cruise_eta(target)-11000/top())<.001f);
    CHECK(game_cruise(true)); tick(50);target=l3_add(p->position,l3v(0,0,-11000));
    CHECK(fabsf(game_cruise_eta(target)-11000/(top()*13))<.001f);
    game_throttle(.5f); CHECK(fabsf(game_cruise_eta(target)-22000/(top()*13))<.001f);
    CHECK(game_cruise(false));CHECK(fabsf(game_cruise_eta(target)-22000/top())<.001f);
    game_throttle(0);CHECK(isinf(game_cruise_eta(target)));CHECK(game_cruise_eta(p->position)==0);
    CHECK(isinf(game_cruise_eta(l3v(NAN,0,0))));
}
static l3_quat face(l3_vec d) {
    return l3_quat_euler(-atan2f(d.x,-d.z)/CG_RAD,atan2f(d.y,hypotf(d.x,d.z))/CG_RAD,0);
}
static void pirate(void) {
    cg_spawn(16,cg->next_id++,SH_KRAIT,ROLE_PIRATE,l3v(0,0,300));
    cg->encounter[16].kind=CE_PIRATES;cg->encounter[16].life=300;
    CHECK(game_cruise(true)); CHECK(cg_interdict(16));
}
static void interdiction(void) {
    game_player *p=&cg->v.player; pirate();
    for(int i=0;i<310 && cg->v.interdiction.active;i++) {
        p->attitude=face(cg->v.interdiction.escape); tick(1);
        CHECK(cg->v.interdiction.tug>=-1 && cg->v.interdiction.tug<=1);
        CHECK(cg->v.interdiction.progress>=0 && cg->v.interdiction.progress<=1);
        CHECK(fabsf(cg_len(cg->v.interdiction.escape)-1)<.001f);
    }
    CHECK(!cg->v.interdiction.active && p->cruising && cg->v.contacts[16].jump_out>0);
    CHECK(cg->v.interdiction.tug==1 && cg->v.interdiction.progress==1);
    reset(); pirate();
    for(int i=0;i<310 && cg->v.interdiction.active;i++) {
        p->attitude=face(l3_scale(cg->v.interdiction.escape,-1));tick(1);
    }
    CHECK(!cg->v.interdiction.active && !p->cruising && cg->cruise.stall==2);
    CHECK(p->speed<=top() && !strcmp(p->cruise_reason,"Interdicted"));
    CHECK(cg_len(l3_sub(p->position,cg->v.contacts[16].position))<250);
    refusal("Engine stalled"); p->velocity=l3v(0,0,0);
    tick(50);CHECK(p->speed==0);tick(2);CHECK(p->speed>0);
    reset(); pirate();tick(50);CHECK(game_cruise(false));
    CHECK(!p->cruising && !cg->v.interdiction.active && cg->cruise.stall==0);
    CHECK(p->speed<=top() && cg_len(l3_sub(p->position,cg->v.contacts[16].position))<250);
    reset();pirate();cg->v.contacts[16].id++;tick(1);
    CHECK(!cg->v.interdiction.active && p->cruising);
    reset();pirate();cg->v.contacts[16].live=true;tick(1);
    CHECK(!cg->v.interdiction.active && p->cruising);
    reset();pirate();cg->v.contacts[16].live=true;
    l3_vec live_position=cg->v.contacts[16].position;
    CHECK(game_cruise(false));
    CHECK(!cg->v.interdiction.active && !p->cruising);
    CHECK(cg_len(l3_sub(cg->v.contacts[16].position,live_position))==0);
}
static void director(void) {
    cg_spawn(16,cg->next_id++,SH_KRAIT,ROLE_PIRATE,l3v(0,0,300));
    CHECK(game_cruise(true)); cg->v.contacts[16].live=true;
    CHECK(!cg_interdict(16)); cg->v.contacts[16].live=false;cg->mapped[16]=true;
    CHECK(!cg_interdict(16));cg->mapped[16]=false;
    cg->v.contacts[16].role=ROLE_TRADER;CHECK(!cg_interdict(16));
    cg->v.contacts[16].role=ROLE_POLICE;CHECK(!cg_interdict(16));
    cg->v.player.legal=127;CHECK(!cg_interdict(16));
    cg->v.player.legal=128;CHECK(cg_interdict(16));
    reset();CHECK(game_cruise(true));cg->v.contents.station=l3v(0,0,1700);
    cg->cruise.due=.5f;cg_cruise_director(1);CHECK(!cg->v.interdiction.active && cg->cruise.due==.5f);
    cg->v.contents.station=l3v(1000000,0,0);cg->v.traffic_mode=2;
    cg_cruise_director(1);CHECK(!cg->v.interdiction.active && cg->cruise.due==.5f);
    cg->v.traffic_mode=1;cg_cruise_director(1);CHECK(cg->v.interdiction.active);
    for(int level=0;level<3;level++) {
        reset();game_set_encounters(level);CHECK(game_cruise(true));cg->cruise.due=0;
        cg_cruise_director(1);
        CHECK(cg->cruise.due>=cg->encounters.cadence[level*2]-1);
        CHECK(cg->cruise.due<=cg->encounters.cadence[level*2+1]);
    }
}
static void limits(void) {
    CHECK(game_cruise(true));unsigned before=allocations;clock_t start=clock();
    for(int i=0;i<10000;i++) {tick(1);cg_combat(.04f);}
    CHECK(allocations==before);
    printf("10000 flight/combat steps: %.3f CPU s; cruise state %u bytes; allocation delta %u\n",
        (double)(clock()-start)/CLOCKS_PER_SEC,(unsigned)sizeof(cg->cruise),allocations-before);
}
int main(int argc,char **argv) {
    CHECK(argc==2);adsb_state_init();ls_lap_api_begin("carrier-cruise",1,0);
    ls_lap_sys_begin();ls_lap_api_callback(true);CHECK(game_init());reset();
    if(!strcmp(argv[1],"refusals"))refusals();else if(!strcmp(argv[1],"spool"))spool();
    else if(!strcmp(argv[1],"safety"))safety();else if(!strcmp(argv[1],"bleed"))bleed();
    else if(!strcmp(argv[1],"eta"))eta();else if(!strcmp(argv[1],"interdiction"))interdiction();
    else if(!strcmp(argv[1],"director"))director();else if(!strcmp(argv[1],"limits"))limits();else CHECK(false);
    printf("cruise %s: %u assertions passed\n",argv[1],checks);
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
