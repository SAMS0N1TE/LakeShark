/* Conversations, contracts and sensor-fed resources. Fixed pools in cg_state;
 * receiver APIs copy worker snapshots, never wait for radio or storage. */
#include "internal.h"
enum { TALK_TRADE=1, TALK_NEWS, TALK_SCAN, TALK_CLEAR, TALK_PIRATE, TALK_RESCUE, TALK_HIJACK, TALK_BEACON };
game_event game_system_event(uint32_t seed,uint32_t day) {
    uint32_t r=cg_hash(seed ^ cg_hash(day));
    return r%3 ? EVENT_NONE : (game_event)(1+(r/3)%4);
}
int game_event_price_percent(game_event e,int good) {
    bool up=(e==EVENT_FESTIVAL && (good==3 || good==4)) || (e==EVENT_FAMINE && good==0) ||
        (e==EVENT_WAR && (good==8 || good==9)) || (e==EVENT_PLAGUE && good==2);
    /* Existing 16-good catalog has no medicine: radioactives are medical isotopes. */
    return up && cg ? cg->systems.event_percent : 100;
}
uint8_t game_combat_rank(uint32_t score) {
    uint8_t r=0;
    if(cg) while(r<8 && score>=cg->systems.rank[r+1]) r++;
    return r;
}
int game_pirate_pack(uint32_t seed,int government) {
    return government<=1 ? 1+(int)(cg_hash(seed)%3) : 1;
}
int game_mining_yield(uint32_t seed,int wifi,int *good) {
    int richness=wifi<=0 ? 8 : (int)cg_clamp((float)wifi,1,16);
    uint32_t r=cg_hash(seed);
    if(good) *good=(int)(r%32)<richness ? 12+(int)((r/32)%3) : 11;
    return 1+richness/4;
}
void cg_ores(l3_vec p,uint32_t seed) {
    int good,n=game_mining_yield(seed,cg->wifi_count,&good);
    for(int i=0;i<CG_CANS && n;i++) if(cg->cans[i].life<=0) {
        cg->cans[i]=(cg_can){l3_add(p,l3v(n*3-6,2,0)),180,good,0}; n--;
    }
    cg_message("Ore canisters released.");
}
bool cg_friendly(uint32_t id) {
    for(int i=0;i<cg->friend_count;i++) if(cg->friendly[i]==id) return true;
    return false;
}
void cg_friend(uint32_t id) {
    if(!cg_friendly(id) && cg->friend_count<GAME_REGISTRY) cg->friendly[cg->friend_count++]=id;
    for(int i=0;i<GAME_CONTACTS;i++) if(cg->v.contacts[i].id==id) {
        cg->v.contacts[i].hostile=false;cg->provoked[i]=false;
    }
}
void cg_name(game_contact *c) {
    /* Syllables are computed, with no growing name tables. */
    uint32_t r=cg_hash(c->id);
    const char *vowels="aeiou",*cons="bdgjklmnrstv";
    char a[5]={cons[r%12],vowels[(r/12)%5],cons[(r/60)%12],0,0};
    char b[6]={cons[(r/720)%12],vowels[(r/8640)%5],cons[(r/43200)%12], 'a',0,0};
    a[0]-=32;b[0]-=32;
    snprintf(c->name,sizeof(c->name),"%s %s",a,b);
}
static void event_text(void) {
    static const char *const texts[]={"Quiet trading day","Festival: luxuries + liquor","Famine: food needed",
        "War: arms + alloys needed","Plague: medical isotopes"};
    snprintf(cg->v.event_text,31,"%s",texts[game_system_event(cg->v.systems[cg->v.system].seed,cg->v.market_day)]);
}
static int near_system(uint32_t seed) {
    int n=0;
    for(int i=0;i<GAME_SYSTEMS;i++) if(i!=cg->v.system &&
        game_distance(&cg->v.systems[i],&cg->v.systems[cg->v.system])<=cg->v.player.fuel_max) n++;
    if(!n) return cg->v.system;
    int pick=(int)(seed%(uint32_t)n);
    for(int i=0;i<GAME_SYSTEMS;i++) if(i!=cg->v.system &&
        game_distance(&cg->v.systems[i],&cg->v.systems[cg->v.system])<=cg->v.player.fuel_max && !pick--) return i;
    return cg->v.system;
}
void cg_mission_offer(int i,game_mission *m) {
    uint32_t r=cg_hash(cg->v.systems[cg->v.system].seed ^ cg_hash(cg->v.market_day) ^ (uint32_t)i);
    memset(m,0,sizeof(*m));m->kind=(game_mission_kind)(i<4?i:4+(int)((cg->v.market_day*2+(uint32_t)i-4)%5));
    m->destination=near_system(r);m->reward=cg->systems.mission_reward*(1+r%3);
    m->deadline=cg->v.market_day+cg->systems.mission_days;
    m->good=r%5;m->amount=1+(r/5)%3;m->target_id=0x70000000u | (r & 0x0fffffffu);
    static const char *const title[]={"Deliver supplies","Carry sealed parcel","Hunt wanted pirate","Rescue distress ship"};
    if(m->kind<=MISSION_RESCUE) snprintf(m->title,25,"%s",title[m->kind]);
    snprintf(m->brief,61,"%.24s to %.9s; due day %u.",m->title,cg->v.systems[m->destination].name,(unsigned)m->deadline);
    if(m->kind==MISSION_BOUNTY) {
        game_contact target={0};target.id=m->target_id;cg_name(&target);
        snprintf(m->title,25,"Hunt %.15s",target.name);
    }
    if(m->kind==MISSION_PASSENGER) {
        game_contact guest={0};guest.id=r;cg_name(&guest);
        static const char *const why[]={"family reunion","clinic appointment","tidal concert","new teaching post"};
        snprintf(m->title,25,"Passage: %.15s",guest.name);
        snprintf(m->brief,61,"%.15s: %s; %u seats.",guest.name,why[(r/17)%4],m->amount);
    } else if(m->kind==MISSION_SURVEY) {
        m->destination=cg->v.system;m->target_id=(r&1)?0:1;
        snprintf(m->title,25,"Survey %.12s %s",cg->v.systems[m->destination].name,m->target_id?"1":"star");
        snprintf(m->brief,61,"Approach %.15s %s within 400 of surface.",cg->v.systems[m->destination].name,m->target_id?"1":"star");
    } else if(m->kind==MISSION_SALVAGE) {
        m->destination=cg->v.system;m->good=11;
        snprintf(m->title,25,"Recover lost survey tins");
        snprintf(m->brief,61,"Scoop %u marked tins; derelict contact near port.",m->amount);
    } else if(m->kind==MISSION_SMUGGLING) {
        m->good=(r&1)?5:9;
        snprintf(m->title,25,"Unlisted night freight");
        snprintf(m->brief,61,"%u t %.12s to %.15s; evade scans.",m->amount,cg->v.market[m->good].name,cg->v.systems[m->destination].name);
    } else if(m->kind==MISSION_ESCORT) {
        m->destination=cg->v.system;
        snprintf(m->title,25,"Bring the tea boat home");
        snprintf(m->brief,61,"Follow marked trader to port; stay within 400.");
    }
}
void cg_bulletin(void) {
    for(int i=0;i<6;i++) {
        if(cg->v.missions[i].accepted) continue;
        game_mission offer;cg_mission_offer(i,&offer);cg->v.missions[i]=offer;
    }
}
int game_passenger_seats(void) {
    if(!cg) return 0;
    int seats=cg->v.player.seats;
    for(int i=0;i<6;i++) {game_mission *m=&cg->v.missions[i];
        if(m->kind==MISSION_PASSENGER && m->accepted && !m->done && !m->failed) seats-=m->amount;
    }
    return seats>0?seats:0;
}
bool game_mission_accept(int i) {
    if(!cg || !cg->v.player.docked || cg->v.player.dead || i<0 || i>=6) return false;
    game_mission *m=&cg->v.missions[i];int n=0;
    for(int j=0;j<6;j++) n+=cg->v.missions[j].accepted && !cg->v.missions[j].done && !cg->v.missions[j].failed;
    if(n>=3 || m->accepted || m->done || m->failed || m->deadline<cg->v.market_day) return false;
    if(m->kind==MISSION_PASSENGER && m->amount>game_passenger_seats()) return false;
    if(m->kind==MISSION_SALVAGE && !(cg->v.player.equipment&(1u<<EQ_SCOOP))) return false;
    if(m->kind==MISSION_DELIVERY || m->kind==MISSION_SMUGGLING) {
        if(game_cargo_used()+m->amount>game_cargo_capacity()) return false;
        cg->v.player.cargo[m->good]+=m->amount;
    }
    m->accepted=true;cg_message("Contract accepted.");return true;
}
void cg_mission_dock(void) {
    for(int i=0;i<6;i++) {
        game_mission *m=&cg->v.missions[i];
        if(!m->accepted || m->done || m->failed) continue;
        if(cg->v.market_day>m->deadline) {m->failed=true;cg_message("Contract deadline missed.");continue;}
        if(m->destination!=cg->v.system) continue;
        bool freight=m->kind==MISSION_DELIVERY || m->kind==MISSION_SMUGGLING || m->kind==MISSION_SALVAGE;
        if(freight && cg->v.player.cargo[m->good]<m->amount) continue;
        if((m->kind==MISSION_BOUNTY || m->kind==MISSION_RESCUE || m->kind==MISSION_SURVEY ||
            m->kind==MISSION_SALVAGE || m->kind==MISSION_ESCORT) && !m->ready) continue;
        if(freight) cg->v.player.cargo[m->good]-=m->amount;
        cg_award(m->reward);m->done=true;cg_message("Contract paid at station.");
    }
    cg_bulletin();
}
void cg_mission_kill(uint32_t id) {
    for(int i=0;i<6;i++) {
        game_mission *m=&cg->v.missions[i];
        if(m->accepted && !m->failed && m->kind==MISSION_BOUNTY && m->target_id==id) m->ready=true;
    }
}
void cg_mission_rescue(uint32_t id) {
    for(int i=0;i<6;i++) {
        game_mission *m=&cg->v.missions[i];
        if(m->accepted && !m->failed && m->kind==MISSION_RESCUE && m->target_id==id) m->ready=true;
    }
}
static void days(uint32_t n) {
    if(n>0xfffffffdu-cg->v.market_day) n=0xfffffffdu-cg->v.market_day;
    cg->v.market_day+=n;
    if(cg->v.player.legal && cg->v.player.legal<128)
        cg->v.player.legal=(uint8_t)(n>=cg->v.player.legal ? 0 : cg->v.player.legal-n);
    for(int i=0;i<6;i++) {
        game_mission *m=&cg->v.missions[i];
        if(m->accepted && !m->done && !m->failed && cg->v.market_day>m->deadline) {
            m->failed=true;cg_message("Contract deadline missed.");
        }
    }
    event_text();cg_market();cg_bulletin();cg_news();game_talk_close();
}
void cg_day(void) { days(1); }
void cg_system_arrive(void) {
    cg->v.contents.station_kind=cg->v.systems[cg->v.system].tech>=10;
    game_talk_close();memset(cg->talk_used,0,sizeof(cg->talk_used));
    memset(cg->npc_control,0,sizeof(cg->npc_control));
    memset(cg->mapped,0,sizeof(cg->mapped));memset(cg->ble_seen,0,sizeof(cg->ble_seen));
    for(int i=0;i<18;i++) {
        uint32_t r=cg_hash(cg->v.systems[cg->v.system].seed+(uint32_t)i);
        float a=i*CG_PI/9, radius=1700+cg_unit(&r)*400;
        cg->v.asteroids[i]=(game_asteroid){l3v(sinf(a)*radius,-90+cg_unit(&r)*180,cosf(a)*radius),
            12+cg_unit(&r)*25,40,cg->v.contents.belt};
    }
    for(int i=0;i<6;i++) if(cg->v.missions[i].done || cg->v.missions[i].failed) cg->v.missions[i].accepted=false;
    for(int i=0;i<6;i++) {game_mission *m=&cg->v.missions[i];
        if(m->accepted && !m->done && !m->failed && !m->ready && m->kind==MISSION_ESCORT &&
            m->destination!=cg->v.system) {m->failed=true;cg_message("Escort abandoned in transit.");}
    }
    event_text();cg_bulletin();cg_news();cg->sensor_seconds=1;
}
static int free_contact(void) {
    for(int i=GAME_CONTACTS-1;i>=0;i--) if(!cg->v.contacts[i].active && cg->dead_until[i]<=cg->v.clock) return i;
    return -1;
}
static uint32_t text_id(const char *s,int n) {
    uint32_t h=2166136261u;for(int i=0;i<n && s[i];i++) h=(h^(uint8_t)s[i])*16777619u;
    return h | 0x80000000u;
}
bool cg_beacon_use(int who) {
    game_contact *c=&cg->v.contacts[who];
    if(c->role!=ROLE_BEACON || cg_len(l3_sub(c->position,cg->v.player.position))>80) return false;
    int r=-1;for(int i=0;i<cg->v.registry_count;i++) if(cg->v.registry[i].id==c->id) r=i;
    if(r<0) r=cg_register(c);
    if(r<0 || cg->v.registry[r].refuel_day==cg->v.market_day+1) {cg_message("Beacon used today.");return false;}
    cg->v.registry[r].refuel_day=cg->v.market_day+1;cg->v.player.fuel=cg->v.player.fuel_max;
    cg_message("Beacon fuel transfer complete.");return true;
}
static void sensors(void) {
    cg->wifi_count=lap_wifi_aps(cg->wifi,16);
    int n=lap_mesh_nodes(cg->nodes,4);
    for(int j=0;j<GAME_CONTACTS;j++) if(cg->v.contacts[j].active && cg->v.contacts[j].role==ROLE_BEACON) {
        bool found=false;for(int i=0;i<n;i++) if(cg->v.contacts[j].id==text_id(cg->nodes[i].id,16)) found=true;
        if(!found) cg->v.contacts[j].active=false;
    }
    for(int i=0;i<n;i++) {
        lap_mesh_node_t *node=&cg->nodes[i];uint32_t id=text_id(node->id,16);int slot=-1;
        for(int j=0;j<GAME_CONTACTS;j++) if(cg->v.contacts[j].active && cg->v.contacts[j].id==id) slot=j;
        if(slot<0) {slot=free_contact();if(slot<0) break;
            cg_spawn(slot,id,SH_PROBE,ROLE_BEACON,l3_add(cg->v.contents.station,l3v((i-1)*80,60,-260)));}
        game_contact *c=&cg->v.contacts[slot];
        for(int k=0;k<15;k++) c->name[k]=node->name[k] && (node->name[k]<32 || node->name[k]>126) ? '?' : node->name[k];
        c->name[15]=0;memcpy(c->callsign,c->name,8);c->callsign[8]=0;
        int reg=cg_register(c);
        if(reg>=0) {for(int k=0;k<19;k++) cg->v.registry[reg].name[k]=node->name[k] && (node->name[k]<32 || node->name[k]>126) ? '?' : node->name[k];cg->v.registry[reg].name[19]=0;}
    }
    n=lap_ble_devices(cg->ble,24);
    for(int i=0;i<n;i++) {
        uint32_t id=2166136261u;for(int k=0;k<6;k++) id=(id^cg->ble[i].addr[k])*16777619u;
        if(!id) id=1;
        bool seen=false;int empty=-1;
        for(int k=0;k<24;k++) {if(cg->ble_seen[k]==id) seen=true;if(!cg->ble_seen[k]) empty=k;}
        if(seen || empty<0) continue;
        for(int k=0;k<CG_CANS;k++) if(cg->cans[k].life<=0) {
            cg->cans[k]=(cg_can){l3_add(cg->v.contents.station,l3v((i%6)*14-35,20,-180-i*5)),180,(int)(id%5),0};
            cg->ble_seen[empty]=id;break;
        }
    }
    lap_time_t time={0};lap_time(&time);cg->v.local_hour=(uint8_t)cg_clamp((float)time.hour,0,23);
    /* Ensure accepted named targets exist; real aircraft are never relocated. */
    for(int i=0;i<6;i++) {
        game_mission *m=&cg->v.missions[i];
        if(!m->accepted || m->done || m->failed || m->ready || m->destination!=cg->v.system ||
            (m->kind!=MISSION_BOUNTY && m->kind!=MISSION_RESCUE && m->kind!=MISSION_SALVAGE && m->kind!=MISSION_ESCORT)) continue;
        bool exists=false;for(int j=0;j<GAME_CONTACTS;j++) if(cg->v.contacts[j].active && cg->v.contacts[j].id==m->target_id) exists=true;
        if(exists) continue;
        int slot=free_contact();if(slot<0) continue;
        game_role role=m->kind==MISSION_BOUNTY?ROLE_PIRATE:m->kind==MISSION_SALVAGE?ROLE_DERELICT:
            m->kind==MISSION_ESCORT?ROLE_TRADER:ROLE_RESCUE;
        cg_spawn(slot,m->target_id,m->kind==MISSION_BOUNTY?SH_KRAIT:SH_ADDER,role,l3v(80,0,780));
        if(m->kind==MISSION_ESCORT) {cg->lane_out[slot]=false;cg_friend(m->target_id);}
        if(m->kind==MISSION_RESCUE) cg->v.contacts[slot].squawk=7700;
        else if(m->kind==MISSION_BOUNTY) {cg_name(&cg->v.contacts[slot]);memcpy(m->title,"Hunt ",5);memcpy(m->title+5,cg->v.contacts[slot].name,16);}
    }
}
void cg_mission_observe(void) {
    game_player *p=&cg->v.player;
    /* The existing tap-to-scoop action sets life to zero. Observe it here too. */
    for(int k=0;k<CG_CANS;k++) if(cg->cans[k].life==0 && cg->cans[k].mission_id) {
        for(int i=0;i<6;i++) {game_mission *m=&cg->v.missions[i];
            if(m->kind==MISSION_SALVAGE && m->accepted && !m->done && !m->failed &&
                m->target_id==cg->cans[k].mission_id && m->progress<m->amount &&
                p->cargo[m->good]>m->progress && cg_len(l3_sub(p->position,cg->cans[k].p))<28) {
                m->progress++;m->ready=m->progress==m->amount;
            }
        }
        cg->cans[k].mission_id=0;
    }
    for(int i=0;i<6;i++) {
        game_mission *m=&cg->v.missions[i];
        if(!m->accepted || m->done || m->failed) continue;
        if(m->kind==MISSION_SMUGGLING && p->cargo[m->good]<m->amount) {
            m->failed=true;cg_message("Unlisted freight lost.");continue;
        }
        if(p->dead || p->docked || m->ready || m->destination!=cg->v.system) continue;
        if(m->kind==MISSION_SURVEY) {
            l3_vec pos=cg->v.contents.star;float radius=cg->v.contents.star_radius;
            if(m->target_id) {unsigned body=m->target_id-1;
                if(body>=(unsigned)cg->v.contents.planet_count) continue;
                pos=cg->v.contents.planets[body].position;radius=cg->v.contents.planets[body].radius;
            }
            if(cg_len(l3_sub(p->position,pos))<radius+400) {m->ready=true;cg_message("Survey recorded. Return to port.");}
        } else if(m->kind==MISSION_ESCORT || m->kind==MISSION_SALVAGE) {
            for(int j=0;j<GAME_CONTACTS;j++) {
                game_contact *c=&cg->v.contacts[j];if(c->id!=m->target_id) continue;
                if(m->kind==MISSION_ESCORT) {
                    if(c->hull<=0) {m->failed=true;cg_message("Escort ship lost.");}
                    else if(cg_len(l3_sub(c->position,cg->v.contents.station))<100 &&
                        cg_len(l3_sub(p->position,c->position))<400) {m->ready=true;cg_message("Escort reached port.");}
                } else if(c->active && cg_len(l3_sub(c->position,p->position))<100) {
                    int left=m->amount-m->progress;
                    for(int k=0;k<CG_CANS;k++) if(cg->cans[k].life>0 && cg->cans[k].mission_id==m->target_id) left--;
                    for(int k=0;k<CG_CANS && left>0;k++) if(cg->cans[k].life<=0) {
                        cg->cans[k]=(cg_can){l3_add(c->position,l3v(left*5,0,0)),180,m->good,m->target_id};left--;
                    }
                }
                break;
            }
        }
    }
}
void cg_system_update(float dt) {
    game_player *p=&cg->v.player;
    cg->sensor_seconds+=dt;
    uint32_t now=lap_millis(),elapsed=now-cg->last_tick_ms;cg->last_tick_ms=now;
    if(!cg->scan_started || now-cg->last_scan_ms>=(uint32_t)(cg->systems.scan_seconds*1000)) {
        cg->last_scan_ms=now;cg->scan_started=true;lap_wifi_scan(); /* nonblocking request to existing app worker */
    }
    cg_mission_observe();
    if(cg->sensor_seconds>=1) {cg->sensor_seconds=0;sensors();}
    if(!p->docked && !p->dead) {
        cg->flight_seconds+=(float)elapsed*.001f;
        uint32_t n=(uint32_t)(cg->flight_seconds/cg->systems.day_seconds);
        if(n) {cg->flight_seconds-=n*cg->systems.day_seconds;days(n);}
    }
    if(!p->docked && !p->dead && (p->equipment&(1u<<EQ_SCOOP)))
        for(int i=0;i<CG_CANS;i++) if(cg->cans[i].life>0 && game_cargo_used()<game_cargo_capacity() &&
            cg_len(l3_sub(cg->cans[i].p,p->position))<28) {
            p->cargo[cg->cans[i].good]++;cg->cans[i].life=0;
            for(int j=0;j<6;j++) {game_mission *m=&cg->v.missions[j];
                if(m->kind==MISSION_SALVAGE && m->accepted && !m->done && !m->failed &&
                    m->target_id==cg->cans[i].mission_id && m->progress<m->amount) {
                    m->progress++;m->ready=m->progress==m->amount;
                }
            }
            cg->cans[i].mission_id=0;
        }
    bool skim=!p->docked && !p->dead && cg->v.jump==JUMP_IDLE &&
        cg_len(l3_sub(p->position,cg->v.contents.star))<cg->v.contents.star_radius*1.4f;
    if(skim && (p->equipment&(1u<<EQ_SCOOP))) {
        p->fuel=fminf(p->fuel_max,p->fuel+dt*cg->systems.scoop_rate);
        p->cabin_heat=fminf(1,p->cabin_heat+dt*cg->systems.heat_gain);
    } else p->cabin_heat=fmaxf(0,p->cabin_heat-dt*cg->systems.heat_cool);
    if(p->cabin_heat>.85f) p->hull=fmaxf(0,p->hull-dt*cg->systems.heat_damage);
    if(cg->v.talk.open && (p->docked || p->dead || !cg->v.contacts[cg->v.talk.who].active ||
        cg->v.contacts[cg->v.talk.who].id!=cg->talk_id ||
        cg_len(l3_sub(cg->v.contacts[cg->v.talk.who].position,p->position))>=700)) game_talk_close();
}
void game_talk_close(void) {if(cg) cg->v.talk.open=false;}
static void choices(const char *a,const char *b) {
    snprintf(cg->v.talk.choices[0],13,"%s",a);snprintf(cg->v.talk.choices[1],13,"%s",b);
}
void cg_talk_open(int who) {
    if(who<0 || who>=GAME_CONTACTS || !cg->v.contacts[who].active) return;
    game_contact *c=&cg->v.contacts[who];game_talk *t=&cg->v.talk;
    memset(t,0,sizeof(*t));t->open=true;t->who=who;t->line_count=2;t->choice_count=2;
    cg->talk_id=c->id;cg->talk_day=cg->v.market_day;
    uint32_t r=cg_hash(c->id ^ cg_hash(cg->v.market_day) ^ cg->v.systems[cg->v.system].seed);
    choices("Accept","Decline");
    if(c->role==ROLE_BEACON) {
        cg_beacon_use(who);cg->talk_kind=TALK_BEACON;
        snprintf(t->lines[0],31,"Local news: %.15s",cg->v.systems[cg->v.system].name);
        snprintf(t->lines[1],31,"%.30s",cg->v.event_text);choices("Refuel","Goodbye");
    } else if(c->squawk==7500 || c->emergency==5) {
        cg->talk_kind=TALK_HIJACK;snprintf(t->lines[0],31,"Help! Pirates hold our crew.");
        snprintf(t->lines[1],31,"Hunt our escort for a bounty.");
    } else if(c->role==ROLE_RESCUE) {
        cg->talk_kind=TALK_RESCUE;snprintf(t->lines[0],31,"Distress! Send food supplies.");
        snprintf(t->lines[1],31,"Hold within 80 for 5 seconds.");
    } else if(c->role==ROLE_POLICE) {
        bool dirty=cg->v.player.legal || cg->v.player.cargo[5] || cg->v.player.cargo[9];
        cg->talk_kind=dirty?TALK_SCAN:TALK_CLEAR;t->price=cg->systems.fine*(1+(cg->v.player.legal>0));
        snprintf(t->lines[0],31,dirty?"Scan: contraband or offender.":"Registry clear. Safe travels.");
        snprintf(t->lines[1],31,dirty?"Fine: %d cr":"Wanted pirate contract: %d cr",dirty?t->price:cg->systems.mission_reward);
        choices(dirty?"Pay fine":"Contract",dirty?"Run":"Goodbye");
        if(!dirty && r%2) {snprintf(t->lines[1],31,"Keep the station lanes safe.");choices("Thanks","Goodbye");}
        t->amount=(!dirty && !(r%2));
    } else if(c->role==ROLE_PIRATE) {
        cg->talk_kind=TALK_PIRATE;t->amount=(game_cargo_used()+2)/3;
        t->price=cg->systems.fine*(1+c->faction%3);t->choice_count=3;
        snprintf(t->lines[0],31,"Surrender %d t cargo.",t->amount);snprintf(t->lines[1],31,"Or bribe us: %d cr",t->price);choices("Pay cargo","Refuse");
        snprintf(t->choices[2],13,"Bribe");
    } else if(c->role==ROLE_DERELICT || r%3==0) {
        cg->talk_kind=TALK_NEWS;
        int dst=near_system(r);t->good=r%GAME_GOODS;
        snprintf(t->lines[0],31,"News from %.15s",cg->v.systems[dst].name);
        if(r&1) snprintf(t->lines[1],31,"%.12s: %d cr",cg->v.market[t->good].name,game_price(&cg->v.systems[dst],t->good,cg->v.market_day));
        else {
            int slot=-1;
            for(int j=0;j<GAME_CONTACTS;j++) if(cg->v.contacts[j].active && cg->v.contacts[j].role==ROLE_DERELICT) {slot=j;break;}
            if(slot<0) {slot=free_contact();if(slot>=0) cg_spawn(slot,cg_hash(r)|0x50000000u,SH_ADDER,ROLE_DERELICT,l3v(160,0,840));}
            if(slot>=0) snprintf(t->lines[1],31,"Wreck x%.0f y%.0f z%.0f",cg->v.contacts[slot].position.x,cg->v.contacts[slot].position.y,cg->v.contacts[slot].position.z);
            else snprintf(t->lines[1],31,"%.12s: %d cr",cg->v.market[t->good].name,game_price(&cg->v.systems[dst],t->good,cg->v.market_day));
        }
        choices("Thanks","Goodbye");
    } else {
        cg->talk_kind=TALK_TRADE;t->good=r%GAME_GOODS;t->amount=1+(r/16)%3;
        int bias=(c->faction&1)?cg->systems.trade_percent:-cg->systems.trade_percent;
        t->price=game_price(&cg->v.systems[cg->v.system],t->good,cg->v.market_day)*t->amount*(100+bias)/100;
        snprintf(t->lines[0],31,"Offer: %d t %.15s",t->amount,cg->v.market[t->good].name);
        snprintf(t->lines[1],31,"Total price: %d cr",t->price);
    }
    /* Retain both actionable lines, with a fresh opening conditioned on state. */
    memcpy(t->lines[2],t->lines[1],31);memcpy(t->lines[1],t->lines[0],31);t->line_count=3;
    uint32_t voice=cg_hash(r ^ (uint32_t)c->faction*7919u ^ (uint32_t)cg->v.player.legal*31u ^
        (uint32_t)cg->v.systems[cg->v.system].government*113u ^ (cg_friendly(c->id)?0x48454c50u:0));
    if(c->role==ROLE_POLICE && cg->talk_kind==TALK_CLEAR)
        snprintf(t->lines[0],31,"clear: %.23s",cg_lore_open(c->role,voice));
    else snprintf(t->lines[0],31,"%s",cg_lore_open(c->role,voice));
}
static bool conversation_contract(game_mission_kind kind,uint32_t id) {
    int count=0,slot=-1;
    for(int i=0;i<6;i++) {
        game_mission *m=&cg->v.missions[i];
        if(m->accepted && !m->done && !m->failed && m->target_id==id) return false;
        if(m->accepted && !m->done && !m->failed) count++;
        if(!m->accepted || m->done || m->failed) slot=i;
    }
    if(count>=3 || slot<0) return false;
    game_mission *m=&cg->v.missions[slot];memset(m,0,sizeof(*m));m->accepted=true;
    m->kind=kind;m->target_id=id;m->destination=cg->v.system;m->reward=cg->systems.mission_reward;
    m->deadline=cg->v.market_day+cg->systems.mission_days;
    snprintf(m->title,25,kind==MISSION_RESCUE?"Rescue distress ship":"Deputy lane watch");
    snprintf(m->brief,61,kind==MISSION_RESCUE?"Bring food to the marked distress contact.":"Deputised: stop the marked pirate, then report to port.");return true;
}
static bool pack_help(int who,bool hijack) {
    game_contact *c=&cg->v.contacts[who];int n=hijack ? game_pirate_pack(c->id,0) : 1+(int)(cg_hash(c->id)%2);
    uint32_t target=cg_hash(c->id)|0x60000000u;
    if(hijack && !conversation_contract(MISSION_BOUNTY,target)) return false;
    for(int k=0;k<n;k++) {
        int slot=free_contact();if(slot<0) break;
        cg_spawn(slot,target+(uint32_t)k,SH_KRAIT,ROLE_PIRATE,l3_add(c->position,l3v(24+k*18,10,-30)));
    }
    if(hijack) {c->hostile=false;c->role=ROLE_TRADER;cg_friend(c->id);}
    return true;
}
bool game_talk_choose(int i) {
    if(!cg || !cg->v.talk.open || i<0 || i>=cg->v.talk.choice_count) return false;
    game_talk *t=&cg->v.talk;game_contact *c=&cg->v.contacts[t->who];game_player *p=&cg->v.player;
    if(!c->active || c->id!=cg->talk_id || p->dead || p->docked || cg->talk_day!=cg->v.market_day ||
        cg_len(l3_sub(c->position,p->position))>=700) {game_talk_close();return false;}
    if(i==2 && cg->talk_kind==TALK_PIRATE) {
        if(p->credits<(uint32_t)t->price) return false;
        p->credits-=t->price;cg_friend(c->id);cg->talk_used[t->who]=c->id;
        c->active=false;cg->dead_until[t->who]=cg->v.clock+120;
    } else if(i==0) switch(cg->talk_kind) {
    case TALK_TRADE:
        if(cg->talk_used[t->who]==c->id || p->credits<(uint32_t)t->price || game_cargo_used()+t->amount>game_cargo_capacity()) return false;
        p->credits-=t->price;p->cargo[t->good]+=t->amount;cg->talk_used[t->who]=c->id;cg_friend(c->id);cg_message("Trade complete. Fly safe.");break;
    case TALK_SCAN:
        if(p->credits<(uint32_t)t->price) return false;
        p->credits-=t->price;p->cargo[5]=p->cargo[9]=0;p->legal=0;c->hostile=false;cg_mission_observe();break;
    case TALK_CLEAR:
        if(t->amount && !conversation_contract(MISSION_BOUNTY,cg_hash(c->id)|0x60000000u)) return false;
        break;
    case TALK_PIRATE: {
        if(game_cargo_used()<t->amount) return false;
        int left=t->amount;for(int g=0;g<GAME_GOODS && left;g++) {int n=p->cargo[g]<left?p->cargo[g]:left;p->cargo[g]-=n;left-=n;}
        c->active=false;cg->dead_until[t->who]=cg->v.clock+120;break;
    }
    case TALK_RESCUE:
        if(!p->cargo[0]) return false;
        cg->rescue_target=t->who;cg->rescue=-1;cg->v.rescue_progress=0;break;
    case TALK_HIJACK:
        if(cg->talk_used[t->who]==c->id || free_contact()<0) return false;
        if(!pack_help(t->who,true)) return false;
        cg->talk_used[t->who]=c->id;break;
    case TALK_BEACON:cg_beacon_use(t->who);break;
    default:break;
    } else if(cg->talk_kind==TALK_SCAN) {
        p->legal=(uint8_t)fmaxf(20,p->legal);c->hostile=true;cg->provoked[t->who]=true;
    } else if(cg->talk_kind==TALK_PIRATE) {c->hostile=true;cg->provoked[t->who]=true;pack_help(t->who,false);}
    uint32_t voice=cg_hash(c->id ^ cg->v.market_day);
    int mood=i==1?1:(cg->talk_kind==TALK_TRADE || cg->talk_kind==TALK_PIRATE || cg->talk_kind==TALK_SCAN)?0:cg_friendly(c->id)?2:p->legal?3:
        cg->v.systems[cg->v.system].government<2?4:(c->faction&1)?6:5;
    cg_message(cg_lore_outcome(mood,voice));
    game_talk_close();return true;
}
/* Shared by flight and weapons: police protect traders, pirate packs pick the
 * same nearest victim unless the commander has provoked them. */
int cg_enemy(int i) {
    game_contact *c=&cg->v.contacts[i];
    if(cg_friendly(c->id) || c->role==ROLE_BEACON || c->role==ROLE_RESCUE || c->squawk==7500 || c->emergency==5) return -2;
    if(c->role==ROLE_POLICE) {
        if(cg->v.player.legal) return -1;
        for(int j=0;j<GAME_CONTACTS;j++) if(cg->v.contacts[j].active && cg->v.contacts[j].role==ROLE_PIRATE && !cg_friendly(cg->v.contacts[j].id)) return j;
        return -2;
    }
    if(c->hostile) {
        if(cg->provoked[i]) return -1;
        for(int j=0;j<GAME_CONTACTS;j++) if(cg->v.contacts[j].active && cg->v.contacts[j].role==ROLE_TRADER && !cg->v.contacts[j].squawk) return j;
        return -1;
    }
    return -2;
}
