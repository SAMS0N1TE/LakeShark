/* Headless CARRIER pilot. Only game.h drives/observes the game; host wrappers
 * provide repeatable GPS/time, and the real LAP cache supplies rules/saves.
 * Usage: carrier_bot [hours=4] [seeds=3] [csv=bot.csv] [rules=data/carrier]
 * CSV snapshots are every ten minutes; .txt contains individual/mean results.
 * Prices on the chart and POI/rock coordinates are perfect public-view knowledge:
 * this measures a careful, informed pilot, not human navigation/discovery time. */
#include "game.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cg_render_reset(void) {} /* backend link boundary, as in carrier tests */
extern uint32_t (*ls_lap_api_host_clock)(void);
static uint32_t milliseconds, seed_number;
static uint32_t now(void) { return milliseconds; }
bool __wrap_lap_gps(lap_gps_t *g) {
    memset(g,0,sizeof(*g));g->fix=true;
    g->lat=35+(double)seed_number*1.73;g->lon=-75+(double)seed_number*2.39;
    return true;
}
void __wrap_lap_time(lap_time_t *t) {
    memset(t,0,sizeof(*t));t->year=2026;t->month=1;t->day=1;
    t->hour=12+(milliseconds/3600000)%12;t->unix=1767268800+milliseconds/1000;
}
typedef enum { TRADER,MINER,HUNTER,COURIER,EXPLORER,MIXED,STRATEGIES } strategy;
static const char *names[]={"TRADER","MINER","HUNTER","COURIER","EXPLORER","MIXED"};
typedef struct {
    strategy strategy,job;
    int destination,good,rock,old_system,visits[GAME_SYSTEMS];
    bool returning,departing,was_dead,outage;
    double time,sortie,first_kit,first_hull,mid,top,early_seconds;
    unsigned deaths,early_deaths,outages,done,failed,jumps;
    uint64_t spent;
    uint32_t mission_ids[6];unsigned mission_flags[6];
    l3_vec ore;float scoop_until;
} pilot;
static float length(l3_vec v) { return sqrtf(l3_dot(v,v)); }
static float distance(l3_vec a,l3_vec b) { return length(l3_sub(a,b)); }
static float clamp(float x,float a,float b) { return fmaxf(a,fminf(b,x)); }
static l3_vec forward(const game_view *v) {
    return l3_quat_rotate(v->player.attitude,l3v(0,0,-1));
}
/* Angular feedback uses local coordinates; no attitude/position writes. */
static float aim(const game_view *v,l3_vec target) {
    l3_vec delta=l3_sub(target,v->player.position);
    l3_quat q=v->player.attitude;q.x=-q.x;q.y=-q.y;q.z=-q.z;
    l3_vec local=l3_quat_rotate(q,delta);
    float yaw=-atan2f(local.x,-local.z),pitch=atan2f(local.y,hypotf(local.x,local.z));
    float turn=game_player_ship()->turn*.01745329252f*(v->player.cruising?.4f:1);
    game_yaw(clamp(yaw*3/turn,-1,1));game_stick(clamp(pitch*3/turn,-1,1),0);
    return l3_dot(l3_norm(delta),forward(v));
}
static l3_vec avoid(const game_view *v,l3_vec target,l3_vec centre,float radius) {
    l3_vec delta=l3_sub(target,v->player.position);float len=length(delta);
    if(len<1) return target;
    l3_vec dir=l3_scale(delta,1/len),off=l3_sub(centre,v->player.position);
    float along=l3_dot(off,dir);
    if(along<=0 || along>=len) return target;
    l3_vec nearest=l3_add(v->player.position,l3_scale(dir,along));
    if(distance(nearest,centre)>radius) return target;
    l3_vec side=l3_sub(nearest,centre);
    if(length(side)<1) side=l3v(0,1,0);
    return l3_add(centre,l3_scale(l3_norm(side),radius+350));
}
static void fly(const game_view *v,l3_vec target,float stop,bool cruise) {
    float d=distance(target,v->player.position);
    target=avoid(v,target,v->contents.star,v->contents.star_radius+700);
    for(int i=0;i<v->contents.planet_count;i++)
        target=avoid(v,target,v->contents.planets[i].position,v->contents.planets[i].radius+250);
    float alignment=aim(v,target),top=game_player_ship()->speed;
    if(v->interdiction.active) {
        aim(v,l3_add(v->player.position,l3_scale(v->interdiction.escape,1000)));
        game_throttle(.7f);return;
    }
    float braking=top*fmaxf(1,v->player.cruise_factor)*2+stop+300;
    if(v->player.cruising && (d<braking || alignment<.94f || !cruise)) game_cruise(false);
    else if(cruise && !v->player.cruising && !v->player.mass_locked && d>3500 && alignment>.998f)
        game_cruise(true);
    float throttle=clamp((d-stop)/(top*1.5f),0,1);
    if(alignment<.98f) throttle*=clamp((alignment-.3f)/.68f,0,1);
    game_throttle(throttle);
}
static void observe(pilot *b,const game_view *v) {
    if(v->player.rank<3) b->early_seconds+=.04;
    if(v->player.dead && !b->was_dead) {
        b->deaths++;if(v->player.rank<3) b->early_deaths++;
    }
    b->was_dead=v->player.dead;
    bool stranded=v->player.docked && game_refuel_cost()>v->player.credits;
    if(stranded && !b->outage) b->outages++;
    b->outage=stranded;
    for(int i=0;i<6;i++) {
        const game_mission *m=&v->missions[i];
        uint32_t id=m->target_id^m->deadline^(uint32_t)m->kind;
        if(id!=b->mission_ids[i]) {b->mission_ids[i]=id;b->mission_flags[i]=0;}
        if(m->done && !(b->mission_flags[i]&1)) {b->done++;b->mission_flags[i]|=1;}
        if(m->failed && !(b->mission_flags[i]&2)) {b->failed++;b->mission_flags[i]|=2;}
    }
    if(v->system!=b->old_system) {
        b->old_system=v->system;b->visits[v->system]++;b->jumps++;
        b->departing=false;b->returning=b->job==TRADER || b->job==COURIER;
        b->sortie=b->time;b->rock=-1;
    }
}
static bool buy_kit(pilot *b,int equipment) {
    game_equipment_offer offer;
    if(!game_equipment_info(equipment,&offer) || offer.owned || !offer.available) return false;
    /* A careful pilot keeps 2000cr for fuel, repairs and working capital. */
    if(game_state()->player.credits<2000+offer.price) return false;
    if(!game_equip(equipment)) return false;
    b->spent+=offer.price;if(b->first_kit<0) b->first_kit=b->time/60;
    return true;
}
static void upgrade(pilot *b) {
    if(game_player_class()==SH_WHALE_SHARK) return;
    game_yard_offer offers[11];int n=game_shipyard(offers,11);
    bool mid=game_player_class()==SH_THRESHER || game_player_class()==SH_HAMMERHEAD;
    game_class target=mid?SH_WHALE_SHARK:
        (b->strategy==TRADER || b->strategy==MINER || b->strategy==EXPLORER || b->strategy==MIXED)?SH_THRESHER:SH_HAMMERHEAD;
    for(int i=0;i<n;i++) if(offers[i].ship_class==target && offers[i].available &&
        offers[i].price>0 && game_state()->player.credits>(uint32_t)offers[i].price+2000) {
        uint32_t price=offers[i].price;
        if(game_buy_ship(target)) {
            b->spent+=price;if(b->first_hull<0) b->first_hull=b->time/60;
            if(mid && b->top<0) b->top=b->time/60;else if(!mid && b->mid<0) b->mid=b->time/60;
        }
    }
    if(b->strategy==MINER || b->strategy==EXPLORER) buy_kit(b,EQ_SCOOP);
    else if(b->strategy==TRADER || b->strategy==MIXED) buy_kit(b,EQ_CARGO);
    else buy_kit(b,EQ_SHIELD);
    if(b->first_kit>=0) {buy_kit(b,EQ_ENERGY);buy_kit(b,EQ_SCOOP);}
}
/* Choose a reachable route using the public deterministic price preview.
 * No waiting at a station for daily repricing, illegal freight or downgrades. */
static void route(pilot *b,const game_view *v) {
    b->destination=-1;b->good=-1;float best=-1e20f;
    for(int i=0;i<GAME_SYSTEMS;i++) {
        float ly=game_distance(&v->systems[v->system],&v->systems[i]);
        if(i==v->system || ly>v->player.fuel || ly<.25f) continue;
        float score=0;
        if(b->job==TRADER || b->job==MIXED) {
            for(int g=0;g<GAME_GOODS;g++) {
                if(v->market[g].illegal) continue;
                int amount=(int)fminf(game_cargo_capacity(),v->market[g].stock);
                amount=(int)fminf(amount,(float)(v->player.credits>1000?v->player.credits-1000:0)/v->market[g].price);
                float profit=amount*(game_price(&v->systems[i],g,v->market_day+1)-v->market[g].price);
                if(profit>score) {score=profit;}
            }
        } else score=1000/(1+b->visits[i])+(float)((i*37+seed_number*13)%101);
        if(b->job==MINER) {game_contents c;game_system_contents(v->systems[i].seed,&c);if(c.belt) score+=2000;}
        if(b->job==HUNTER || b->job==COURIER) for(int m=0;m<6;m++) {
            const game_mission *mission=&v->missions[m];
            bool suitable=b->job==HUNTER?mission->kind==MISSION_BOUNTY:
                (mission->kind==MISSION_COURIER || mission->kind==MISSION_DELIVERY || mission->kind==MISSION_PASSENGER);
            if(suitable && !mission->done && !mission->failed && mission->destination==i) score+=mission->reward*4;
        }
        score-=ly*12;
        if(score>best) {best=score;b->destination=i;}
    }
    if(b->destination<0) return;
    if(b->job==TRADER || b->job==MIXED) {
        float profit=0;
        for(int g=0;g<GAME_GOODS;g++) if(!v->market[g].illegal) {
            int amount=(int)fminf(game_cargo_capacity(),v->market[g].stock);
            amount=(int)fminf(amount,(float)(v->player.credits>1000?v->player.credits-1000:0)/v->market[g].price);
            float p=amount*(game_price(&v->systems[b->destination],g,v->market_day+1)-v->market[g].price);
            if(p>profit) {profit=p;b->good=g;}
        }
        if(b->good>=0) {
            int amount=(int)fminf(game_cargo_capacity(),v->market[b->good].stock);
            amount=(int)fminf(amount,(float)(v->player.credits>1000?v->player.credits-1000:0)/v->market[b->good].price);
            game_market_buy(b->good,amount);
        }
    }
    if(b->job==COURIER || b->job==HUNTER) for(int m=0;m<6;m++) {
        const game_mission *mission=&v->missions[m];
        if(mission->destination==b->destination && (b->job==HUNTER?mission->kind==MISSION_BOUNTY:
            (mission->kind==MISSION_COURIER || mission->kind==MISSION_DELIVERY || mission->kind==MISSION_PASSENGER)))
            game_mission_accept(m);
    }
}
static void dock(pilot *b,const game_view *v) {
    (void)b;game_fire(false);game_cruise(false);
    if(v->player.docking) {game_stick(0,0);game_yaw(0);game_throttle(0);return;}
    if(distance(v->player.position,v->contents.station)<800 && v->action==ACT_DOCK) game_context();
    else fly(v,l3_add(v->contents.station,l3v(0,0,-300)),200,true);
}
static void tick(pilot *b) {
    const game_view *v=game_state();const game_player *p=&v->player;
    observe(b,v);game_fire(false);
    if(p->dead) {
        /* Rebuy replaces failed board slots during arrival; count lost contracts
         * before that public transaction so their failures are not missed. */
        for(int i=0;i<6;i++) if(v->missions[i].accepted && !v->missions[i].done && !v->missions[i].failed)
            b->failed++;
        game_rebuy();b->returning=false;b->departing=false;b->destination=-1;return;
    }
    if(v->jump!=JUMP_IDLE) {game_throttle(0);game_stick(0,0);game_yaw(0);return;}
    if(p->docked) {
        for(int g=0;g<GAME_GOODS;g++) if(p->cargo[g]) game_market_sell(g,p->cargo[g]);
        game_refuel();game_repair();upgrade(b);
        b->job=b->strategy==MIXED?(strategy)((b->jumps/3)%5):b->strategy;
        route(b,v);b->returning=false;b->departing=true;b->sortie=b->time;
        game_context();game_throttle(1);return;
    }
    if(b->returning || p->hull<game_player_ship()->hull*.55f ||
       (p->shield_fore<15 && p->shield_aft<30)) {b->returning=true;dock(b,v);return;}
    if(b->departing) {
        if(b->destination<0) {b->returning=true;return;}
        if(!p->mass_locked && game_plot_jump(b->destination)) {
            l3_vec direction;
            game_throttle(0);
            if(game_jump_direction(&direction)) {
                aim(v,l3_add(p->position,l3_scale(direction,1000)));
                if(game_jump_alignment()<=GAME_JUMP_ALIGN_DEG) {
                    game_stick(0,0);game_yaw(0);game_context();
                }
            }
        } else fly(v,l3v(0,800,-1800),100,false);
        return;
    }
    if(b->job==MINER) {
        if(game_cargo_used()>=game_cargo_capacity() || b->time-b->sortie>600) {b->returning=true;return;}
        if(b->scoop_until>b->time) {
            fly(v,b->ore,0,false);if(v->action==ACT_SCOOP) game_context();return;
        }
        if(b->rock>=0 && !v->asteroids[b->rock].active) {
            b->scoop_until=(float)b->time+8;b->rock=-1;return;
        }
        if(b->rock<0) {
            float best=1e20f;
            for(int i=0;i<18;i++) if(v->asteroids[i].active) {
                float d=distance(v->asteroids[i].position,p->position);
                if(d<best) {best=d;b->rock=i;}
            }
        }
        if(b->rock<0) {b->returning=true;return;}
        const game_asteroid *a=&v->asteroids[b->rock];b->ore=a->position;
        fly(v,a->position,a->radius+90,false);
        if(distance(a->position,p->position)<500 && aim(v,a->position)>.9999f) game_fire(true);
    } else if(b->job==HUNTER) {
        int target=-1;float best=1e20f;bool contract=false;
        for(int m=0;m<6;m++) if(v->missions[m].accepted && !v->missions[m].done &&
            !v->missions[m].failed && v->missions[m].kind==MISSION_BOUNTY && !v->missions[m].ready) contract=true;
        for(int i=0;i<GAME_CONTACTS;i++) if(v->contacts[i].active && v->contacts[i].hostile) {
            float d=v->contacts[i].range;if(d<best) {best=d;target=i;}
        }
        if(target>=0) {
            const game_contact *c=&v->contacts[target];game_target_nearest(true);
            fly(v,l3_add(c->position,l3_scale(c->velocity,.2f)),180,false);
            float alignment=aim(v,c->position);
            if(best<600 && alignment>.9998f) game_fire(true);
        } else if(!contract || b->time-b->sortie>500) b->returning=true;
        else fly(v,l3v(80,0,1700),100,false);
    } else if(b->job==EXPLORER) {
        int poi=-1;float best=1e20f;
        for(int i=0;i<v->poi_count;i++) if(!v->pois[i].scanned && v->pois[i].kind!=POI_ION_STORM) {
            float d=distance(v->pois[i].position,p->position);
            if(d<best) {best=d;poi=i;}
        }
        if(poi<0 || b->time-b->sortie>500) {b->returning=true;return;}
        const game_poi *point=&v->pois[poi];
        l3_vec away=l3_norm(l3_sub(p->position,point->position));
        /* Nebula halves scan range; approach the centre; dangerous cores keep a margin. */
        float stand=point->kind==POI_NEBULA?point->scan_range*.32f:point->scan_range*.82f;
        fly(v,l3_add(point->position,l3_scale(away,stand)),0,true);
    } else b->returning=true;
}
static bool cache_rules(const char *root) {
    const char *files[]={"rules.json","systems.json","encounters.json","explore.json"};
    for(int i=0;i<4;i++) {
        char path[1024],data[16384];snprintf(path,sizeof(path),"%s/%s",root,files[i]);
        FILE *f=fopen(path,"rb");if(!f) return false;
        size_t n=fread(data,1,sizeof(data),f);fclose(f);if(n==sizeof(data)) return false;
        int fd=lap_file_open(files[i],"w");if(fd<0) return false;
        bool ok=lap_file_write(fd,data,n)==(int)n;lap_file_close(fd);if(!ok) return false;
    }
    return true;
}
static bool market_audit(void) {
    /* A same-station buy/sell cycle must preserve cash, stock and cargo. */
    for(int g=0;g<GAME_GOODS;g++) {
        const game_view *v=game_state();uint32_t cash=v->player.credits;
        if(v->market[g].illegal) continue; /* selling contraband changes legal status */
        int stock=v->market[g].stock,cargo=v->player.cargo[g];
        if(!game_market_buy(g,1) || !game_market_sell(g,1) ||
            v->player.credits!=cash || v->market[g].stock!=stock || v->player.cargo[g]!=cargo) return false;
    }
    return true;
}
static void snapshot(FILE *f,const pilot *b,int level) {
    const game_view *v=game_state();uint64_t assets=v->player.credits+b->spent;
    for(int g=0;g<GAME_GOODS;g++) assets+=(uint64_t)v->player.cargo[g]*v->market[g].price;
    fprintf(f,"%s,%u,%s,%.2f,%u,%llu,%u,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        names[b->strategy],seed_number,level==1?"NORMAL":"BUSY",b->time/60,v->player.credits,
        (unsigned long long)assets,b->deaths,b->deaths/fmax(b->time/3600,.0001),
        b->early_deaths/fmax(b->early_seconds/3600,.0001),b->first_kit,b->first_hull,b->mid,b->top,
        v->player.rank,v->player.combat_score,v->discoveries.rank,b->outages,b->done,b->failed,
        v->discoveries.count,b->jumps,v->player.ship_class);fflush(f);
}
int main(int argc,char **argv) {
    bool smoke=argc>1 && !strcmp(argv[1],"--smoke");
    char *end=NULL;double hours=argc>1 && !smoke?strtod(argv[1],&end):4;
    if(end && *end) return 2;
    long count=argc>2?strtol(argv[2],&end,10):3;
    if(argc>2 && *end) return 2;
    int seeds=(int)count;if(smoke) {hours=.1;seeds=1;}
    const char *csv=argc>3?argv[3]:"bot.csv",*rules=argc>4?argv[4]:"data/carrier";
    if(!isfinite(hours) || hours<=0 || hours>48 || count<1 || count>24) {fprintf(stderr,"hours 0..48, seeds 1..24\n");return 2;}
    FILE *out=fopen(csv,"w");char path[1024];snprintf(path,sizeof(path),"%s.txt",csv);
    FILE *summary=fopen(path,"w");if(!out || !summary) return 2;
    fprintf(out,"strategy,seed,traffic,minutes,credits,credits_plus_spend_and_cargo,deaths,deaths_h,early_deaths_h,kit_min,hull_min,mid_min,top_min,combat_rank,combat_score,explorer_rank,fuel_outages,missions_done,missions_failed,pois,jumps,hull\n");
    fprintf(summary,"%g hours/run; %d seeds; 25 Hz; SIM ONLY NORMAL/BUSY. -1 = not reached.\nStarting Mako + 10000cr; kit/hulls preserve 2000cr reserve. Fuel outage = docked but unable to afford fuel.\nPerfect chart-price/POI knowledge. Income = credits + purchased kit/hulls + cargo mark-to-market - 10000; maintenance remains an expense.\nSame-station roundtrip audit: 14 legal goods per run. Mission counters track slot identity/deadline transitions, including rebuy failures.\n",hours,seeds);
    for(int s=0;s<STRATEGIES;s++) {
        double income=0,kit=0,mid=0,death=0,early=0,early_hours=0,pois=0,missions=0;int nk=0,nm=0;
        for(int level=1;level<=2;level++) for(int seed=1;seed<=seeds;seed++) {
            seed_number=(uint32_t)seed;milliseconds=120000;ls_lap_api_host_clock=now;
            adsb_state_init();ls_lap_api_begin("carrier-bot",1,milliseconds);ls_lap_sys_begin();ls_lap_api_callback(true);
            if(!cache_rules(rules) || !game_init() || !game_state()->config_valid) {fprintf(stderr,"rules/init failed\n");return 1;}
            if(!market_audit()) {fprintf(stderr,"market roundtrip failed\n");return 1;}
            game_set_traffic(1);game_set_encounters(level);
            pilot b={.strategy=(strategy)s,.job=(strategy)s,.destination=-1,.good=-1,.rock=-1,
                .old_system=0,.first_kit=-1,.first_hull=-1,.mid=-1,.top=-1};b.visits[0]=1;
            uint64_t frames=(uint64_t)(hours*3600*25);
            for(uint64_t frame=0;frame<frames;frame++) {
                b.time=(double)frame/25;tick(&b);milliseconds+=40;game_update(.04f);
                if((frame+1)%15000==0 || frame+1==frames) {b.time=(double)(frame+1)/25;snapshot(out,&b,level);}
            }
            const game_view *v=game_state();double value=v->player.credits+(double)b.spent-10000;
            if(smoke && (b.jumps==0 || b.deaths || !isfinite(v->clock) ||
                (s==HUNTER && !v->player.kills) || (s==EXPLORER && !v->discoveries.count))) {
                fprintf(stderr,"pilot smoke failed: %s level %d\n",names[s],level);return 1;
            }
            for(int g=0;g<GAME_GOODS;g++) value+=v->player.cargo[g]*v->market[g].price;
            fprintf(summary,"%s seed=%d %s income/h=%.0f kit=%.1f hull=%.1f mid=%.1f top=%.1f deaths=%u early=%u/%.2fh rank=%u score=%u explore=%u scans=%u fuel=%u missions=%u/%u jumps=%u\n",
                names[s],seed,level==1?"NORMAL":"BUSY",value/hours,b.first_kit,b.first_hull,b.mid,b.top,
                b.deaths,b.early_deaths,b.early_seconds/3600,v->player.rank,v->player.combat_score,
                v->discoveries.rank,v->discoveries.count,b.outages,b.done,b.failed,b.jumps);fflush(summary);
            income+=value/hours;death+=b.deaths/hours;early+=b.early_deaths;early_hours+=b.early_seconds/3600;
            if(b.first_kit>=0) {kit+=b.first_kit;nk++;}if(b.mid>=0) {mid+=b.mid;nm++;}
            pois+=v->discoveries.count;missions+=b.done;
            game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();
        }
        int n=seeds*2;
        fprintf(summary,"MEAN %s income/h=%.0f kit=%.1f(%d/%d) mid=%.1f(%d/%d) deaths/h=%.3f early/h=%.3f scans=%.1f missions=%.1f\n",
            names[s],income/n,nk?kit/nk:-1,nk,n,nm?mid/nm:-1,nm,n,death/n,early/fmax(early_hours,.0001),pois/n,missions/n);fflush(summary);
    }
    fclose(out);fclose(summary);printf("Wrote %s and %s\n",csv,path);return 0;
}
