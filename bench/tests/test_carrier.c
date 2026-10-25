/* Focused system tests; production cache, receiver API, flight and renderer. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include "walk.h"
#include <assert.h>
#include <limits.h>
extern uint32_t (*ls_lap_api_host_clock)(void);
static uint32_t ms=120000;
static uint32_t clock_ms(void){return ms;}
static void step(int n) {for(int i=0;i<n;i++){ms+=40;game_update(.04f);}}
static void setup(void) {
    ls_lap_api_host_clock=clock_ms;adsb_state_init();ls_lap_api_begin("carrier-test",1,ms);ls_lap_sys_begin();ls_lap_api_callback(true);
    assert(game_init());assert(cg->v.config_valid);
}
static void universe(void) {
    game_system *a=malloc(sizeof(game_system)*256),*b=malloc(sizeof(game_system)*256);assert(a&&b);
    game_universe(123,a);game_universe(123,b);assert(!memcmp(a,b,sizeof(game_system)*256));
    game_universe(456,b);assert(memcmp(a,b,sizeof(game_system)*256));
    for(int i=0;i<256;i++) {assert(a[i].name[0]&&strlen(a[i].name)<16&&a[i].description[0]);assert(a[i].government<8&&a[i].economy<8&&a[i].tech>=1&&a[i].tech<=12&&a[i].population>0);assert(a[i].x>=0&&a[i].x<=64&&a[i].y>=0&&a[i].y<=64);}
    assert(game_gps_seed(false,43,-71)==game_gps_seed(false,0,0));assert(game_gps_seed(true,0,0)==game_gps_seed(true,.0001f,.0001f));assert(game_gps_seed(true,43,-71)!=game_gps_seed(true,44,-71));
    game_contents x,y;game_system_contents(123,&x);game_system_contents(123,&y);assert(!memcmp(&x,&y,sizeof(x)));
    for(int i=0;i<256;i++) {game_system_contents(a[i].seed,&x);assert(x.planet_count>=1&&x.planet_count<=5);for(int p=0;p<x.planet_count;p++)assert(x.planets[p].radius>0&&x.planets[p].orbit>x.planets[p].radius);}
    for(int c=0;c<SH_COUNT;c++) {const l3_mesh *m=game_mesh((game_class)c);assert(m&&m->nv>=12&&m->nv<=80);for(int j=0;j<m->ntri*3;j++)assert(m->idx[j]<m->nv);}
    assert(game_distance(&a[0],&a[0])==0);free(a);free(b);puts("universe: determinism, 256 systems, 17 meshes, GPS cells passed");
}
static void traffic(void) {
    lap_aircraft_t a={.icao=1};static const char *cats[]={"A1","A2","A3","A4","A7","B1","B2","B4","B6"};
    game_class expected[]={SH_SIDEWINDER,SH_ADDER,SH_COBRA,SH_COBRA,SH_DRONE,SH_PROBE,SH_PROBE,SH_WORM,SH_DRONE};
    for(int i=0;i<9;i++){strcpy(a.category,cats[i]);assert(game_aircraft_class(&a)==expected[i]);}
    strcpy(a.category,"A5");assert(game_aircraft_class(&a)==SH_ANACONDA||game_aircraft_class(&a)==SH_BOA);
    strcpy(a.category,"A6");assert(game_aircraft_class(&a)==SH_VIPER||game_aircraft_class(&a)==SH_KRAIT);
    a.category[0]=0;strcpy(a.callsign,"ABC123");assert(game_aircraft_class(&a)==SH_PYTHON&&game_aircraft_faction(&a)>0);
    int f=game_aircraft_faction(&a);strcpy(a.callsign,"ABC999");assert(game_aircraft_faction(&a)==f);
    strcpy(a.callsign,"N123AB");assert(game_aircraft_class(&a)==SH_ADDER&&game_aircraft_faction(&a)==0);
    a.callsign[0]=0;assert(game_aircraft_class(&a)==SH_ASP);
    strcpy(a.category,"A7");strcpy(a.callsign,"TEST");assert(game_aircraft_class(&a)==SH_MORAY);
    for(uint32_t i=1;i<100;i++)assert(!game_aircraft_hostile(i,7)||game_aircraft_hostile(i,0));
    adsb_aircraft_t *rx=adsb_state_find_or_create(0x123456);assert(rx);rx->active=true;rx->last_seen_us=(int64_t)ms*1000;strcpy(rx->callsign,"ABC123");rx->emitter_tc=4;rx->emitter_ca=3;
    adsb_state_set_squawk(rx,7500);adsb_state_set_squawk(rx,7500);cg->poll=1;cg_traffic(0);cg_contacts();
    assert(cg->v.contacts[0].active&&cg->v.contacts[0].live&&cg->v.contacts[0].hostile&&cg->v.contacts[0].ship_class==SH_COBRA);
    int registry=cg->v.registry_count;cg->poll=1;cg_traffic(0);assert(cg->v.registry_count==registry);
    adsb_state_set_squawk(rx,7700);adsb_state_set_squawk(rx,7700);cg->poll=1;cg_traffic(0);assert(cg->v.contacts[0].role==ROLE_RESCUE);
    adsb_state_set_squawk(rx,7600);adsb_state_set_squawk(rx,7600);cg->poll=1;cg_traffic(0);assert(cg->v.contacts[0].role==ROLE_DERELICT);
    bool can=false;for(int i=0;i<CG_CANS;i++)can|=cg->cans[i].life>0;assert(can);
    cg_damage(0,10000,cg->v.player.position,true);cg->poll=1;cg_traffic(0);assert(!cg->v.contacts[0].active);
    puts("traffic: 16 category/callsign rules, faction, hostility, 3 squawk events, registry and tombstone passed");
}
static void trade(void) {
    game_system s=cg->v.systems[0];int p=game_price(&s,6,123);assert(p>0&&p==game_price(&s,6,123));
    bool varied=false;for(int d=124;d<140;d++)varied|=game_price(&s,6,d)!=p;assert(varied);
    s.economy=7;int hi=game_price(&s,6,123);s.economy=0;assert(hi>game_price(&s,6,123));
    for(int e=0;e<8;e++)for(int g=0;g<16;g++){s.economy=e;assert(game_price(&s,g,123)>0);}
    game_player *player=&cg->v.player;player->docked=true;unsigned cash=player->credits;int price=cg->v.market[0].price,stock=cg->v.market[0].stock;
    assert(!game_market_buy(-1,1)&&!game_market_buy(0,-1)&&!game_market_buy(0,INT_MAX));
    assert(game_market_buy(0,2)&&player->credits==cash-2*price&&player->cargo[0]==2&&cg->v.market[0].stock==stock-2);
    assert(!game_market_sell(0,3)&&game_market_sell(0,2)&&player->credits==cash&&player->cargo[0]==0);
    player->docked=false;assert(!game_market_buy(0,1)&&!game_refuel()&&!game_repair());player->docked=true;
    player->fuel=3;assert(game_refuel()&&player->fuel==7);player->hull=100;assert(game_repair()&&player->hull==game_ship(SH_COBRA)->hull);
    player->credits=100000;assert(game_equip(EQ_CARGO)&&game_cargo_capacity()==35&&!game_equip(EQ_CARGO));
    assert(game_equip(EQ_MISSILE)&&player->missiles==4&&!game_equip(EQ_MISSILE));
    puts("trade: deterministic/daily/economy pricing, 128 positive prices, atomic bounds, equipment, fuel and repair passed");
}
static void save_load(void) {
    game_player *p=&cg->v.player;p->credits=54321;p->cargo[6]=3;p->equipment|=1u<<EQ_ECM;p->kills=24;p->rank=3;p->legal=7;p->fuel=4.5f;
    assert(game_save());unsigned sequence=cg->save_seq;int count=cg->v.registry_count;uint32_t seed=cg->v.galaxy_seed;
    p->credits=1;p->cargo[6]=0;p->legal=0;p->fuel=0;cg->v.galaxy_seed=42;
    assert(game_load());p=&cg->v.player;assert(p->credits==54321&&p->cargo[6]==3&&p->legal==7&&p->fuel==4.5f&&p->rank==3&&p->kills==24&&(p->equipment&(1u<<EQ_ECM)));
    assert(cg->v.galaxy_seed==seed&&cg->v.registry_count==count&&cg->save_seq==sequence);
    p->credits=67890;assert(game_save());int slot=cg->save_seq&1;
    int fd=lap_file_open(slot?"commander1.dat":"commander0.dat","w");assert(fd>=0);assert(lap_file_write(fd,"broken",6)==6);lap_file_close(fd);
    assert(game_load()&&cg->v.player.credits==54321);
    fd=lap_file_open(slot?"commander0.dat":"commander1.dat","w");assert(fd>=0);lap_file_write(fd,"broken",6);lap_file_close(fd);
    cg->v.player.credits=123;assert(!game_load()&&cg->v.player.credits==123);
    puts("save: commander/registry round trip, checksum fallback and non-mutating failure passed");
}
static void flight(void) {
    game_context();assert(!cg->v.player.docked);game_throttle(1);game_stick(.5f,.3f);step(30);assert(cg->v.player.speed>50);
    l3_quat q=cg->v.player.attitude;game_stick(0,0);step(5);assert(fabsf(q.x-cg->v.player.attitude.x)<1e-5f);assert(cg->v.player.mass_locked);
    cg->v.player.position=l3v(110,30,-250);cg->v.player.velocity=l3v(0,0,0);game_throttle(0);cg_contacts();cg_actions();assert(cg->v.action==ACT_DOCK);game_context();assert(cg->v.player.docking);
    {int n=0;while(!cg->v.player.docked&&n<2000){step(1);n++;}printf("flight: docking computer took %.1f s\n",n*.04f);}assert(cg->v.player.docked&&cg->v.save_ok);
    puts("flight: launch, inertial throttle, attitude hold, mass lock, rotated-slot docking and autosave passed");
}
static void combat(void) {
    game_player *p=&cg->v.player;p->docked=false;p->position=l3v(0,0,1100);p->attitude=(l3_quat){0,0,0,1};p->throttle=0;
    memset(cg->v.contacts,0,sizeof(cg->v.contacts));cg->poll=-100;game_contact *c=&cg->v.contacts[16];c->active=true;c->id=0x1000010;c->ship_class=SH_SIDEWINDER;c->hostile=true;c->role=ROLE_DERELICT;c->position=l3v(0,0,1000);c->hull=10;c->shield=0;c->attitude=(l3_quat){0,0,0,1};cg->anchor[16]=c->position;
    game_fire(true);cg_combat(.04f);assert(!c->active&&p->kills==1&&p->credits>10000&&p->heat>0);game_fire(false);
    int debris=0,cans=0;for(int i=0;i<CG_DEBRIS;i++)debris+=cg->debris[i].life>0;for(int i=0;i<CG_CANS;i++)cans+=cg->cans[i].life>0;assert(debris>0&&cans>0);
    float fore=p->shield_fore,aft=p->shield_aft;cg_damage(-1,10,l3v(0,0,1000),false);assert(p->shield_fore==fore-10&&p->shield_aft==aft);
    cg_damage(-1,10,l3v(0,0,1200),false);assert(p->shield_aft==aft-10);
    c->active=true;c->hull=200;c->shield=0;c->position=l3v(0,0,1000);cg->v.selected=16;assert(game_missile());for(int i=0;i<65;i++)cg_combat(.04f);assert(c->hull<200); /* Seekers now require 2 s acquisition before travel. */
    p->equipment|=1u<<EQ_ECM;p->energy=100;assert(game_missile()&&game_ecm());for(int i=0;i<CG_MISSILES;i++)assert(cg->missiles[i].life<=0);
    cg->cans[0]=(cg_can){p->position,20,0};int cargo=p->cargo[0];game_context();assert(p->cargo[0]==cargo+1);
    puts("combat: beam kill, bounty/rank, heat, debris/cargo, fore/aft shields, homing missile, ECM and scoop passed");
}
static void jump_point(l3_vec direction, float error) {
    l3_vec nose=l3v(0,0,-1),axis=l3_cross(nose,direction);
    float angle=acosf(cg_clamp(l3_dot(nose,direction),-1,1))/CG_RAD;
    l3_quat aligned=l3_quat_axis(cg_len(axis)>.00001f?l3_norm(axis):l3v(0,1,0),angle);
    cg->v.player.attitude=l3_quat_mul(aligned,l3_quat_axis(l3v(1,0,0),error));
}
static void jump(void) {
    l3_vec direction;
    assert(!game_jump_direction(&direction)&&game_jump_alignment()==180.f);
    int target=-1;for(int i=1;i<GAME_SYSTEMS;i++)if(game_distance(&cg->v.systems[0],&cg->v.systems[i])<=7){target=i;break;}assert(target>=0);
    assert(!game_plot_jump(-1)&&!game_plot_jump(256)&&!game_plot_jump(0));assert(game_plot_jump(target));
    assert(game_jump_direction(&direction)&&fabsf(cg_len(direction)-1)<.00001f);
    assert(fabsf(asinf(direction.y)/CG_RAD)<25);
    l3_vec chart=l3v(cg->v.systems[target].x-cg->v.systems[0].x,0,cg->v.systems[0].y-cg->v.systems[target].y);
    assert(l3_dot(l3_norm(chart),l3_norm(l3v(direction.x,0,direction.z)))>.99999f);
    game_player *p=&cg->v.player;
    float fuel=p->fuel,ly=cg->v.plotted_ly;
    p->docked=false;p->position=l3v(0,0,1100);p->throttle=0;
    game_stick(0,0);game_yaw(0);cg_flight(0);cg_actions();assert(cg->v.action==ACT_JUMP);
    jump_point(direction,180);assert(game_jump_alignment()>179.9f);
    for(int i=0;i<10;i++) {
        step(1);l3_vec next;assert(game_jump_direction(&next));
        assert(next.x==direction.x&&next.y==direction.y&&next.z==direction.z);
    }
    char align[80];snprintf(align,sizeof(align),"Align with %s to jump.",cg->v.systems[target].name);
    game_context();assert(cg->v.jump==JUMP_IDLE&&cg->v.jump_progress==0);
    assert(!strcmp(cg->v.messages[0],align)&&p->fuel==fuel);
    /* Existing drive and mass-lock refusals take priority over alignment. */
    p->weapons_hot=1;game_context();assert(cg->v.jump==JUMP_IDLE&&!strcmp(cg->v.messages[0],"Weapons hot"));p->weapons_hot=0;
    p->mass_locked=true;game_context();assert(cg->v.jump==JUMP_IDLE&&!strcmp(cg->v.messages[0],"Weapons hot"));p->mass_locked=false;
    jump_point(direction,GAME_JUMP_ALIGN_DEG+.1f);game_context();
    assert(cg->v.jump==JUMP_IDLE&&!strcmp(cg->v.messages[0],align));
    jump_point(direction,GAME_JUMP_ALIGN_DEG-.1f);game_context();assert(cg->v.jump==JUMP_CHARGE);
    assert(!game_plot_jump(target));
    jump_point(direction,GAME_JUMP_HOLD_DEG-.1f);step(10);
    assert(cg->v.jump==JUMP_CHARGE&&cg->v.jump_progress>0);
    jump_point(direction,GAME_JUMP_HOLD_DEG+.1f);step(1);
    assert(cg->v.jump==JUMP_IDLE&&cg->v.jump_progress==0&&p->fuel==fuel&&cg->v.plotted==target);
    assert(!strcmp(cg->v.messages[0],"Jump aborted: lost alignment."));
    jump_point(direction,0);game_context();assert(cg->v.jump==JUMP_CHARGE);step(5);
    jump_point(direction,180);p->mass_locked=true;cg_jump(.04f);p->mass_locked=false;
    assert(cg->v.jump==JUMP_IDLE&&cg->v.jump_progress==0&&p->fuel==fuel);
    assert(!strcmp(cg->v.messages[0],"Jump aborted: mass lock."));
    jump_point(direction,0);assert(game_jump_alignment()<.1f);
    game_context();assert(cg->v.jump==JUMP_CHARGE);step(80);assert(cg->v.jump==JUMP_TUNNEL);
    jump_point(direction,180);step(60);assert(cg->v.system==target&&cg->v.jump==JUMP_IDLE&&fabsf(p->fuel-(fuel-ly))<.001f);
    assert(!game_jump_direction(&direction)&&game_jump_alignment()==180.f);
    puts("jump: stable unit direction, start/hold alignment, refusal priority, abort, charge, fuel debit, tunnel and different-system arrival passed");
}
static void render(void) {
    l3_scene *s=l3_create(45,40);assert(s);game_render(s);l3_stats_t stats;l3_stats(s,&stats);assert(!stats.dropped&&stats.shapes>0);
    for(int i=0;i<40;i++){cg_arrive(i);game_render(s);l3_stats(s,&stats);assert(!stats.dropped);}
    assert(l3_resize(s,80,20));game_render(s);l3_stats(s,&stats);assert(!stats.dropped);
    printf("render: portrait/landscape and 40 system changes passed; state %u bytes; last %u us, %u triangles, %u shapes\n",(unsigned)sizeof(*cg),(unsigned)stats.us_total,(unsigned)stats.triangles,(unsigned)stats.shapes);l3_destroy(s);
}
/* Independent geometry checks, including the circle's clearance from the
 * ellipse (minimise distance by angle, rather than the production solver). */
static float walk_test_y(float x, float z) {
    const walk_box *b; int n=walk_boxes(&b);
    if(!walk_hull_on_pad())return 0;
    for(int i=0;i<n;i++)if(b[i].kind==WB_RAMP && fabsf(x-b[i].c.x)<=b[i].half.x &&
        fabsf(z-b[i].c.z)<=b[i].half.z)
        return (b[i].c.y+b[i].half.y)*(b[i].c.z+b[i].half.z-z)/(2*b[i].half.z);
    return 0;
}
static bool walk_test_clear(float x,float z) {
    const walk_box *b; int n=walk_boxes(&b);float y=walk_test_y(x,z);
    for(int i=0;i<n;i++)if(b[i].solid && b[i].c.y+b[i].half.y>y+.00001f && b[i].c.y-b[i].half.y<y+1.8f) {
        float dx=fmaxf(0,fabsf(x-b[i].c.x)-b[i].half.x),dz=fmaxf(0,fabsf(z-b[i].c.z)-b[i].half.z);
        if(dx*dx+dz*dz<WALK_BODY*WALK_BODY-.000001f)return false;
    }
    const walk_hull *h=walk_hull_on_pad();
    if(h) {
        float px=fabsf(x-h->centre.x),pz=fabsf(z-h->centre.z),a=h->half_width,bh=h->half_length;
        if(px*px/(a*a)+pz*pz/(bh*bh)<1)return false;
        if(px<a+WALK_BODY && pz<bh+WALK_BODY) {
            float lo=0,hi=CG_PI/2;
            for(int i=0;i<40;i++) {
                float u=(2*lo+hi)/3,v=(lo+2*hi)/3;
                float ux=a*cosf(u)-px,uz=bh*sinf(u)-pz,vx=a*cosf(v)-px,vz=bh*sinf(v)-pz;
                if(ux*ux+uz*uz<vx*vx+vz*vz)hi=v;else lo=u;
            }
            float t=(lo+hi)/2,dx=a*cosf(t)-px,dz=bh*sinf(t)-pz;
            if(dx*dx+dz*dz<WALK_BODY*WALK_BODY-.000001f)return false;
        }
    }
    return true;
}
static void walk_test_safe(const walk_state *w) {
    assert(walk_test_clear(w->feet.x,w->feet.z));
    assert(fabsf(w->feet.y-walk_test_y(w->feet.x,w->feet.z))<.0001f);
    assert(w->bob>=0 && w->bob<1 && w->moved>=0);
    l3_vec eye=walk_eye(w);assert(eye.x==w->feet.x && eye.z==w->feet.z);
    assert(fabsf(eye.y-w->feet.y-WALK_EYE)<=.03001f);
}
static int walk_test_count(int kind) {
    const walk_box *b;int n=walk_boxes(&b),count=0;
    for(int i=0;i<n;i++)count+=b[i].kind==kind;
    return count;
}
/* Flood the actual free floor, checking the midpoint of each edge too.
 * A 40 cm grid resolves the 1.8 m doors with body clearance on both sides. */
static void walk_test_paths(const walk_state *w) {
    static unsigned char cells[256*384];
    static int queue[256*384];
    const walk_box *b;const walk_spot *s;walk_boxes(&b);int ns=walk_spots(&s);
    float hw=b[0].half.x-.2f,hl=b[0].half.z-.2f;
    int nx=(int)ceilf(2*hw/.4f),nz=(int)ceilf(2*hl/.4f);
    assert(nx<=256 && nz<=384);memset(cells,0,sizeof(cells));
    float sx=2*hw/nx,sz=2*hl/nz;
    for(int iz=0;iz<nz;iz++)for(int ix=0;ix<nx;ix++)
        cells[iz*nx+ix]=walk_test_clear(-hw+(ix+.5f)*sx,-hl+(iz+.5f)*sz)?1:0;
    int entrance=-1;float distance=1e30f;
    for(int i=0;i<nx*nz;i++)if(cells[i]) {
        float dx=-hw+(i%nx+.5f)*sx-w->feet.x,dz=-hl+(i/nx+.5f)*sz-w->feet.z;
        if(dx*dx+dz*dz<distance){entrance=i;distance=dx*dx+dz*dz;}
    }
    assert(entrance>=0 && distance<.4f*.4f);
    int head=0,tail=0;queue[tail++]=entrance;cells[entrance]=2;
    const int dx[4]={-1,1,0,0},dz[4]={0,0,-1,1};
    while(head<tail) {
        int at=queue[head++],ix=at%nx,iz=at/nx;
        for(int d=0;d<4;d++) {
            int xx=ix+dx[d],zz=iz+dz[d];
            if(xx<0||xx>=nx||zz<0||zz>=nz)continue;
            int next=zz*nx+xx;
            if(cells[next]!=1 || !walk_test_clear(-hw+(ix+.5f+.5f*dx[d])*sx,-hl+(iz+.5f+.5f*dz[d])*sz))continue;
            cells[next]=2;queue[tail++]=next;
        }
    }
    for(int i=0;i<ns;i++) {
        assert(walk_test_clear(s[i].p.x,s[i].p.z));
        assert(fabsf(s[i].p.x)<hw && fabsf(s[i].p.z)<hl);
        bool reachable=false;
        for(int q=0;q<tail;q++) {
            float xx=-hw+(queue[q]%nx+.5f)*sx,zz=-hl+(queue[q]/nx+.5f)*sz;
            float dxs=xx-s[i].p.x,dzs=zz-s[i].p.z;
            if(dxs*dxs+dzs*dzs<.25f){reachable=true;break;}
        }
        if(!reachable)fprintf(stderr,"unreachable walk spot: %s\n",s[i].label);
        assert(reachable);
    }
}
static void walk_test_walls(walk_state entrance) {
    const walk_box *b;int n=walk_boxes(&b);
    float hw=b[0].half.x-.2f,hl=b[0].half.z-.2f;
    for(int i=0;i<n;i++)if(b[i].solid &&
        (b[i].kind==WB_WALL || b[i].kind==WB_WINDOW || b[i].kind==WB_DOOR)) {
        bool tested=false,along_x=b[i].half.x<b[i].half.z;
        float extent=along_x?b[i].half.z:b[i].half.x;
        for(int side=-1;side<=1;side+=2) {
            for(float t=-extent+.05f;t<extent;t+=.2f) {
                float x=b[i].c.x+(along_x?side*(b[i].half.x+WALK_BODY+.16f):t);
                float z=b[i].c.z+(along_x?t:side*(b[i].half.z+WALK_BODY+.16f));
                if(fabsf(x)>hw-WALK_BODY || fabsf(z)>hl-WALK_BODY || !walk_test_clear(x,z))continue;
                walk_state w=entrance;w.feet=l3v(x,walk_test_y(x,z),z);
                w.yaw=along_x?(side<0?90:-90):(side<0?180:0);
                for(int frame=0;frame<100;frame++) {walk_step(&w,1,0,0,0,.1f);walk_test_safe(&w);}
                assert(w.moved<.001f);tested=true;break;
            }
        }
        if(!tested)fprintf(stderr,"no clear wall approach: place %d, box %d at %.2f,%.2f half %.2f,%.2f\n",entrance.place,i,b[i].c.x,b[i].c.z,b[i].half.x,b[i].half.z);
        assert(tested);
    }
}
static void walk_test_layout(walk_place place,const walk_ship *ship) {
    static walk_box saved_boxes[WALK_BOXES];
    static walk_spot saved_spots[WALK_SPOTS];
    walk_hull saved_hull={0};walk_state w,again;
    walk_enter(&w,place,ship);walk_test_safe(&w);
    const walk_box *b;const walk_spot *s;int nb=walk_boxes(&b),ns=walk_spots(&s);
    assert(nb>0 && nb<WALK_BOXES-16 && ns>0 && ns<WALK_SPOTS);
    memcpy(saved_boxes,b,nb*sizeof(*b));memcpy(saved_spots,s,ns*sizeof(*s));
    if(place==WALK_HANGAR){assert(walk_hull_on_pad());saved_hull=*walk_hull_on_pad();}
    else assert(!walk_hull_on_pad());
    for(int i=0;i<nb;i++) {
        assert(b[i].kind<WB_KIND_COUNT && b[i].half.x>0 && b[i].half.y>0 && b[i].half.z>0);
        if(b[i].kind==WB_RAMP || b[i].kind==WB_PAD || b[i].kind==WB_HAZARD || b[i].kind==WB_LIGHT)assert(!b[i].solid);
    }
    for(int i=0;i<ns;i++) {
        assert(s[i].kind<WS_KIND_COUNT && s[i].reach>0 && strlen(s[i].label)>0 && strlen(s[i].label)<=19);
        for(const char *p=s[i].label;*p;p++)assert(!(*p>='a' && *p<='z'));
        walk_state at=w;at.feet=l3v(s[i].p.x,walk_test_y(s[i].p.x,s[i].p.z),s[i].p.z);
        if(s[i].kind!=WS_RAMP_UP && s[i].kind!=WS_RAMP_DOWN) {
            at.spot=-1;assert(walk_use(&at,ship)==s[i].kind && at.place==place);
        }
    }
    if(place==WALK_SHIP) {
        int bunks=ship->hull_radius>9 && ship->seats>0?(ship->seats>8?8:ship->seats):0;
        assert(walk_test_count(WB_BUNK)==bunks);
        assert((walk_test_count(WB_ENGINE)>0)==(ship->hull_radius>4 && ship->has_engines_room));
    }
    walk_test_paths(&w);walk_test_walls(w);
    walk_enter(&again,place,ship);assert(!memcmp(&w,&again,sizeof(w)));
    assert(walk_boxes(&b)==nb && walk_spots(&s)==ns);
    assert(!memcmp(saved_boxes,b,nb*sizeof(*b)) && !memcmp(saved_spots,s,ns*sizeof(*s)));
    if(place==WALK_HANGAR)assert(!memcmp(&saved_hull,walk_hull_on_pad(),sizeof(saved_hull)));
    /* Replaying movement must also produce identical state, including bob. */
    for(int i=0;i<240;i++)walk_step(&w,(i%3)*.5f,(i%5-2)*.5f,2,i%2?1:-1,.1f);
    walk_test_safe(&w);
    for(int i=0;i<240;i++)walk_step(&again,(i%3)*.5f,(i%5-2)*.5f,2,i%2?1:-1,.1f);
    assert(!memcmp(&w,&again,sizeof(w)));
}
static void walk_tests(void) {
    const walk_ship ships[]={{3,10,5,0,false},{8,20,10,2,true},{16,200,100,6,true}};
    for(int i=0;i<3;i++) {
        walk_test_layout(WALK_HANGAR,&ships[i]);walk_test_layout(WALK_SHIP,&ships[i]);
        walk_state w;walk_ship cargo=ships[i];cargo.cargo_used=0;
        walk_enter(&w,WALK_SHIP,&cargo);assert(walk_test_count(WB_CRATE)==0);
        cargo.cargo_used=cargo.cargo_capacity/2;walk_enter(&w,WALK_SHIP,&cargo);int half=walk_test_count(WB_CRATE);
        cargo.cargo_used=cargo.cargo_capacity;walk_enter(&w,WALK_SHIP,&cargo);int full=walk_test_count(WB_CRATE);
        assert(half>0 && full>half && full<=24);
        cargo.cargo_used=INT_MAX;walk_enter(&w,WALK_SHIP,&cargo);assert(walk_test_count(WB_CRATE)==full);
        cargo.cargo_used=-1;walk_enter(&w,WALK_SHIP,&cargo);assert(walk_test_count(WB_CRATE)==0);
        cargo.cargo_capacity=0;cargo.cargo_used=10;walk_enter(&w,WALK_SHIP,&cargo);assert(walk_test_count(WB_CRATE)==0);
        /* Full holds and the maximum bunk count must leave every room reachable. */
        cargo=ships[i];cargo.cargo_used=cargo.cargo_capacity;cargo.seats=INT_MAX;
        walk_test_layout(WALK_SHIP,&cargo);
        walk_enter(&w,WALK_HANGAR,&ships[i]);l3_vec foot=w.feet;
        assert(walk_use(&w,&ships[i])==-1); /* Facing away from the ramp. */
        walk_step(&w,0,0,-180,0,0);assert(walk_use(&w,&ships[i])==WS_RAMP_UP);
        assert(w.place==WALK_SHIP && w.yaw==0);walk_test_safe(&w);
        walk_step(&w,0,0,180,0,0);assert(walk_use(&w,&ships[i])==WS_RAMP_DOWN);
        assert(w.place==WALK_HANGAR && w.yaw==180 && !memcmp(&foot,&w.feet,sizeof(foot)));walk_test_safe(&w);
        walk_step(&w,1,0,-180,0,.1f);
        for(int frame=0;frame<20;frame++){walk_step(&w,1,0,0,0,.1f);walk_test_safe(&w);}
        assert(w.feet.y>.3f && w.feet.y<.8f);
        for(int frame=0;frame<100;frame++){walk_step(&w,1,0,0,0,.1f);walk_test_safe(&w);}
        assert(w.moved<.001f); /* Stopped by the hull, not teleported aboard. */
    }
    /* Speed, axes, diagonal normalisation, angle clamps, bob and wall slide. */
    walk_state w;walk_enter(&w,WALK_HANGAR,&ships[0]);w.feet=l3v(12,0,12);w.yaw=0;
    walk_step(&w,1,0,0,100,.1f);assert(fabsf(w.feet.z-11.84f)<.0001f && w.pitch==70);
    assert(fabsf(w.moved-.16f)<.0001f && fabsf(w.bob-.16f/1.4f)<.0001f);
    l3_vec before=w.feet;walk_step(&w,1,1,0,-200,.1f);
    assert(w.pitch==-70 && w.feet.x>before.x && w.feet.z<before.z && fabsf(w.moved-.16f)<.0001f);
    before=w.feet;walk_step(&w,1,0,90,0,.1f);assert(w.feet.x>before.x && fabsf(w.feet.z-before.z)<.0001f);
    before=w.feet;float bob=w.bob;walk_step(&w,0,0,0,0,.1f);
    assert(w.moved==0 && w.bob==bob && !memcmp(&before,&w.feet,sizeof(before)));
    walk_step(&w,1,0,0,0,-1);assert(w.moved==0);
    w.feet=l3v(21.64f,0,12);w.yaw=0;before=w.feet;
    for(int i=0;i<30;i++){walk_step(&w,1,1,0,0,.1f);walk_test_safe(&w);}
    assert(w.feet.z<before.z-2 && w.feet.x<22-WALK_BODY+.0001f);
    /* Facing cone and stale spot indices: USE recomputes its target. */
    const walk_spot *s;int ns=walk_spots(&s),yard=-1;
    for(int i=0;i<ns;i++)if(s[i].kind==WS_YARD)yard=i;
    assert(yard>=0);w.feet=l3v(s[yard].p.x-1,0,s[yard].p.z);w.yaw=90;
    walk_step(&w,0,0,0,0,0);assert(w.spot==yard);
    walk_step(&w,0,0,59,0,0);assert(w.spot==yard);
    walk_step(&w,0,0,2,0,0);assert(w.spot==-1);
    w.spot=yard;assert(walk_use(&w,&ships[0])==-1);
    /* Radius clamps, absent engine rooms and no passenger cabin. */
    walk_ship edge={0,0,0,-3,false};walk_test_layout(WALK_HANGAR,&edge);walk_test_layout(WALK_SHIP,&edge);
    edge.hull_radius=100;walk_test_layout(WALK_HANGAR,&edge);walk_test_layout(WALK_SHIP,&edge);
    puts("walk: three ship sizes, layouts/paths, walls/hull, ramps, cargo, movement and determinism passed");
}
int main(int argc,char **argv) {
    assert(argc==2);setup();
    if(!strcmp(argv[1],"universe"))universe();else if(!strcmp(argv[1],"traffic"))traffic();else if(!strcmp(argv[1],"trade"))trade();else if(!strcmp(argv[1],"save"))save_load();else if(!strcmp(argv[1],"flight"))flight();else if(!strcmp(argv[1],"combat"))combat();else if(!strcmp(argv[1],"jump"))jump();else if(!strcmp(argv[1],"render"))render();else if(!strcmp(argv[1],"walk"))walk_tests();else assert(0);
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
