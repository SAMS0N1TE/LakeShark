/* Shipyard rules through the real commander cache; no screen or hardware. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
#include <time.h>
static unsigned checks, allocations;
#define CHECK(x) do {checks++;assert(x);} while(0)
void cg_render_reset(void) {}
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
void *__wrap_malloc(size_t n) {allocations++;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s) {allocations++;return __real_calloc(n,s);}
void *__wrap_realloc(void *p,size_t n) {allocations++;return __real_realloc(p,n);}
static void fit(game_class c) {
    game_player *p=&cg->v.player;
    memset(p,0,sizeof(*p));cg_ship_fit(p,c);p->attitude=(l3_quat){0,0,0,1};
    p->hull=game_ship(c)->hull;p->shield_fore=p->shield_aft=game_ship(c)->shield;
    p->fuel=p->fuel_max;p->energy=100;p->docked=true;p->credits=1000000;
    p->equipment=(1u<<EQ_LASER)|(1u<<EQ_DOCKING);p->lasers=1;
    memset(cg->v.missions,0,sizeof(cg->v.missions));
}
static void stock(game_class c) {
    const cg_yard_hull *h=&cg->rules.yard.hulls[c];
    game_system *s=&cg->v.systems[cg->v.system];s->tech=12;
    for(int g=0;g<8;g++) if(h->government_mask&(1u<<g)) {s->government=(uint8_t)g;break;}
    game_yard_offer out[11];
    for(int day=0;day<1000;day++) {
        cg->v.market_day=(uint32_t)day;CHECK(game_shipyard(out,11)==11);
        if(out[c].available) return;
    }
    CHECK(false);
}
static void offers(void) {
    CHECK(game_player_class()==SH_MAKO && game_player_ship()==game_ship(SH_MAKO));
    CHECK(cg->v.player.fuel_max==7 && cg->v.player.seats==4 && cg->v.player.pylons==4);
    game_yard_offer a[11],b[11];unsigned changes=0,military=0,pirate=0;
    for(int tech=1;tech<=12;tech++) for(int gov=0;gov<8;gov++) for(int day=0;day<16;day++) {
        game_system *s=&cg->v.systems[cg->v.system];s->tech=(uint8_t)tech;s->government=(uint8_t)gov;
        s->seed=1234;cg->v.market_day=(uint32_t)day;
        CHECK(game_shipyard(a,11)==11);cg->rng+=123;CHECK(game_shipyard(b,11)==11);
        for(int c=0;c<11;c++) {
            const cg_yard_hull *h=&cg->rules.yard.hulls[c];
            bool expected=c!=SH_MAKO && tech>=h->min_tech && (h->government_mask&(1u<<gov)) &&
                cg_hash(s->seed^cg_hash(day)^((uint32_t)c*7919u))%100<h->stock_percent;
            CHECK(a[c].available==expected && a[c].available==b[c].available && a[c].price==b[c].price);
            CHECK(a[c].ship_class==(game_class)c && strlen(a[c].pitch)>0 && strlen(a[c].pitch)<=40);
            if(a[c].available && (c==SH_BLACKTIP || c==SH_GOBLIN)) {CHECK(tech>=9 && gov>=5);military++;}
            if(a[c].available && c==SH_TIGER) {CHECK(gov<=1);pirate++;}
        }
        s->seed++;CHECK(game_shipyard(b,11)==11);
        for(int c=0;c<11;c++) changes+=a[c].available!=b[c].available;
    }
    CHECK(changes && military && pirate);
    CHECK(!game_shipyard(NULL,11) && !game_shipyard(a,0));
    CHECK(game_shipyard(a,1)==1);cg->v.player.docked=false;CHECK(!game_shipyard(a,11));
    cg->v.player.docked=true;cg->v.player.dead=true;CHECK(!game_shipyard(a,11));
    CHECK(!game_buy_ship(SH_DOGFISH));
}
static void transactions(void) {
    game_player *p=&cg->v.player;fit(SH_MAKO);p->hull=65;
    CHECK(cg_ship_trade_in()==13500);stock(SH_THRESHER);
    game_yard_offer out[11];CHECK(game_shipyard(out,11)==11);
    CHECK(out[SH_THRESHER].price==110000-13500 && out[SH_THRESHER].trade_in==13500);
    p->cargo[6]=12;p->equipment|=(1u<<EQ_ECM)|(1u<<EQ_CARGO)|(1u<<EQ_SHIELD);
    p->missiles=4;p->lasers=2;p->fuel=3;
    uint32_t credits=p->credits,eq=p->equipment;
    CHECK(game_buy_ship(SH_THRESHER));CHECK(p->credits==credits-96500 && p->hull==220);
    CHECK(game_cargo_used()==12 && game_cargo_capacity()==115 && p->equipment==eq);
    CHECK(p->missiles==4 && p->lasers==2 && p->mounts==2 && p->fuel==3 && p->fuel_max==9);
    CHECK(game_passenger_seats()==12 && game_refuel_cost()==72);
    credits=p->credits;CHECK(game_refuel() && p->fuel==9 && p->credits==credits-72);
    p->hull=200;CHECK(game_repair_cost()==160);CHECK(game_repair() && p->hull==220);
    stock(SH_DOGFISH);p->cargo[0]=14; /* 26 total; extension fits only 25 */
    game_player before=*p;CHECK(!game_buy_ship(SH_DOGFISH));CHECK(!memcmp(&before,p,sizeof(before)));
    CHECK(strstr(cg->v.messages[0],"Cargo will not fit"));p->cargo[0]--;
    game_mission *m=&cg->v.missions[0];m->kind=MISSION_PASSENGER;m->amount=2;m->accepted=true;
    before=*p;CHECK(!game_buy_ship(SH_DOGFISH));CHECK(!memcmp(&before,p,sizeof(before)));
    CHECK(strstr(cg->v.messages[0],"Passengers will not fit"));m->amount=1;
    credits=p->credits;CHECK(game_buy_ship(SH_DOGFISH));CHECK(p->credits==credits+58000);
    CHECK(p->equipment==eq && p->missiles==1 && p->lasers==1 && p->mounts==1 && p->fuel==5);
    CHECK(game_cargo_capacity()==25 && game_passenger_seats()==0);
    game_equipment_offer e;CHECK(game_equipment_info(EQ_MISSILE,&e) && !e.available);
    CHECK(game_equipment_info(EQ_LASER,&e) && e.owned && !e.available);
    CHECK(!game_buy_ship((game_class)-1) && !game_buy_ship((game_class)11) && !game_buy_ship(SH_DOGFISH));
    fit(SH_MAKO);stock(SH_WHALE_SHARK);p->credits=0;before=*p;
    CHECK(!game_buy_ship(SH_WHALE_SHARK) && !memcmp(&before,p,sizeof(before)));
    p->credits=1000000;CHECK(game_buy_ship(SH_WHALE_SHARK));
    CHECK(game_equip(EQ_LASER) && p->lasers==2 && !game_equip(EQ_LASER));
    for(int i=0;i<6;i++) CHECK(game_equip(EQ_MISSILE));
    CHECK(!game_equip(EQ_MISSILE));
    CHECK(p->missiles==6);stock(SH_COOKIECUTTER);CHECK(game_buy_ship(SH_COOKIECUTTER));
    CHECK(!p->missiles && p->lasers==1 && !game_equip(EQ_MISSILE));
}
static uint32_t u32(const unsigned char *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v) {for(int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));}
static void write_slot(unsigned char b[8192],uint32_t length) {
    uint32_t hash=2166136261u;for(uint32_t i=16;i<length;i++) hash=(hash^b[i])*16777619u;
    put(b+4,length);put(b+8,cg->save_seq+1);put(b+12,hash);
    int fd=lap_file_open("commander0.dat","w");CHECK(fd>=0);
    CHECK(lap_file_write(fd,b,8192)==8192);lap_file_close(fd);
}
static void persistence(void) {
    game_player *p=&cg->v.player;
    for(int c=0;c<11;c++) {
        fit((game_class)c);p->credits=54321;p->hull*=.5f;p->fuel*=.5f;
        p->cargo[6]=(uint16_t)game_cargo_capacity();p->missiles=p->pylons;p->lasers=p->mounts;
        cg->v.system=3;CHECK(game_save());game_player saved=*p;
        fit(SH_MAKO);CHECK(game_load());CHECK(game_player_class()==(game_class)c && cg->yard_station==3);
        CHECK(p->credits==saved.credits && p->hull==saved.hull && p->fuel==saved.fuel);
        CHECK(p->cargo[6]==saved.cargo[6] && p->missiles==saved.missiles && p->lasers==saved.lasers);
        CHECK(p->fuel_max==saved.fuel_max && p->seats==saved.seats && p->mounts==saved.mounts);
    }
    fit(SH_MAKO);cg->v.registry_count=0;CHECK(game_save());
    unsigned char old[8192];memcpy(old,cg->save_buffer,sizeof(old));
    uint32_t prefix=u32(old+4)-16;
    for(int version=3;version>=1;version--) {
        old[3]=(unsigned char)('0'+version);
        write_slot(old,version==3?prefix:version==2?188+358:188);
        fit(SH_DOGFISH);CHECK(game_load() && game_player_class()==SH_MAKO);
        CHECK(p->fuel_max==7 && p->seats==4 && p->lasers==1);
    }
    fit(SH_WHALE_SHARK);CHECK(game_save());CHECK(game_save());memcpy(old,cg->save_buffer,sizeof(old));
    uint32_t length=u32(old+4);put(old+length-12,11);write_slot(old,length);
    CHECK(game_load() && game_player_class()==SH_WHALE_SHARK); /* invalid newest slot falls back */
    fit(SH_WHALE_SHARK);cg->v.registry_count=128;
    for(int i=0;i<128;i++) {game_registry_entry *r=&cg->v.registry[i];memset(r,0,sizeof(*r));r->id=i+1;}
    CHECK(game_save() && u32(cg->save_buffer+4)<=8192 && game_load());
}
static void insurance(void) {
    game_player *p=&cg->v.player;fit(SH_WHALE_SHARK);cg->v.system=3;
    CHECK(game_save());CHECK(game_insurance_cost()==19000 && !game_rebuy());
    cg_arrive(7);p->credits=50000;p->fuel=2;p->cargo[0]=20;p->missiles=6;p->lasers=2;
    p->equipment|=1u<<EQ_ECM;p->hull=0;p->dead=true;p->firing=true;
    cg->v.missions[0]=(game_mission){.kind=MISSION_PASSENGER,.amount=3,.accepted=true,.destination=3};
    CHECK(game_rebuy());CHECK(game_player_class()==SH_WHALE_SHARK && p->credits==31000);
    CHECK(cg->v.system==3 && p->docked && !p->dead && !p->firing && !game_cargo_used());
    CHECK(p->hull==400 && p->missiles==6 && p->lasers==2 && (p->equipment&(1u<<EQ_ECM)));
    CHECK(p->fuel==2 && game_passenger_seats()==32 && !cg->v.missions[0].done);
    CHECK(game_save() && game_load() && game_player_class()==SH_WHALE_SHARK);
    p->credits=0;p->hull=0;p->dead=true;p->cargo[0]=2;CHECK(game_rebuy());
    CHECK(game_player_class()==SH_DOGFISH && !p->credits && !game_cargo_used() && p->hull==70);
    CHECK(p->lasers==1 && !p->missiles && p->docked && game_insurance_cost()==400);
    CHECK(game_save() && game_load() && game_player_class()==SH_DOGFISH);
    fit(SH_MAKO);p->credits=1;p->dead=true;p->hull=0;CHECK(game_rebuy());
    CHECK(game_player_class()==SH_DOGFISH && p->credits==1);
    fit(SH_MAKO);p->credits=12345;p->cargo[4]=2;CHECK(game_save());
    p->dead=true;p->hull=0;p->credits=0;CHECK(game_load());
    CHECK(game_player_class()==SH_MAKO && p->credits==12345 && p->cargo[4]==2 && !p->dead);
}
static void configuration(void) {
    static const char defaults[] =
#include "rules_default.inc"
    ;
    char buf[4096];CHECK(sizeof(defaults)<sizeof(buf));memcpy(buf,defaults,sizeof(defaults));
    char *p=strstr(buf,"8000,5,1,1,1,1,255,100");CHECK(p);p[9]='3';
    int fd=lap_file_open("rules.json","w");CHECK(fd>=0);lap_file_write(fd,buf,strlen(buf));lap_file_close(fd);
    CHECK(!cg_config() && cg->rules.yard.hulls[0].mounts==1);
    memcpy(buf,defaults,sizeof(defaults));p=strstr(buf,"\"trade_percent\":60");CHECK(p);p[16]='0';
    fd=lap_file_open("rules.json","w");CHECK(fd>=0);lap_file_write(fd,buf,strlen(buf));lap_file_close(fd);
    CHECK(!cg_config() && cg->rules.yard.trade_percent==60);
    fd=lap_file_open("rules.json","w");CHECK(fd>=0);lap_file_write(fd,defaults,sizeof(defaults)-1);lap_file_close(fd);
    CHECK(cg_config());
    game_yard_offer out[11];unsigned start=allocations;clock_t t=clock();uint32_t digest=0;
    for(int i=0;i<100000;i++) {game_shipyard(out,11);digest+=(uint32_t)out[i%11].price;}
    CHECK(allocations==start);printf("100k offer queries: %.3f us/query, 0 allocations; digest %u; state %u bytes\n",
        1e6*(double)(clock()-t)/CLOCKS_PER_SEC/100000,digest,(unsigned)sizeof(*cg));
}
int main(int argc,char **argv) {
    CHECK(argc==2);adsb_state_init();ls_lap_api_begin("carrier-shipyard",1,0);
    ls_lap_sys_begin();ls_lap_api_callback(true);CHECK(game_init() && cg->v.config_valid);
    if(!strcmp(argv[1],"offers")) offers();else if(!strcmp(argv[1],"transactions")) transactions();
    else if(!strcmp(argv[1],"persistence")) persistence();else if(!strcmp(argv[1],"insurance")) insurance();
    else if(!strcmp(argv[1],"config")) configuration();else CHECK(false);
    printf("shipyard %s: %u checks passed\n",argv[1],checks);
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
