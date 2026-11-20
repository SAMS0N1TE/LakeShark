/* Seeded encounter director and steering. No IO, allocation or drawing here.
 * Decisions/target searches run at 1 Hz; flight uses fixed per-contact state. */
#include "internal.h"
static float distance(l3_vec a,l3_vec b) {return cg_len(l3_sub(a,b));}
static float between(float a,float b) {return a+(b-a)*cg_unit(&cg->encounter_rng);}
static float interval(void) {
    int n=cg->v.encounter_level*2;
    return between(cg->encounters.cadence[n],cg->encounters.cadence[n+1]);
}
static bool enabled(void) {
    return cg->v.traffic_mode!=2 && !cg->v.player.docked && !cg->v.player.dead && cg->v.jump==JUMP_IDLE;
}
static int leader_slot(int i) {
    uint32_t id=cg->encounter[i].leader;
    if(id) for(int j=0;j<GAME_CONTACTS;j++)
        if(cg->v.contacts[j].active && cg->v.contacts[j].id==id && cg->encounter[j].kind) return j;
    return -1;
}
void cg_depart(int i) {
    game_contact *c=&cg->v.contacts[i];
    if(c->active && c->jump_out<=0) {
        c->jump_out=.8f;
        c->velocity=l3v(0,0,0);
        c->speed=0;
    }
}
void game_set_traffic(int mode) {
    if(!cg || mode<0 || mode>2 || mode==cg->v.traffic_mode) return;
    cg->v.traffic_mode=mode;
    for(int i=0;i<GAME_CONTACTS;i++) {
        game_contact *c=&cg->v.contacts[i];
        if((mode==1 && cg->mapped[i]) || (mode==2 && !c->live)) {
            c->active=false;c->jump_out=0;cg->mapped[i]=false;
            memset(&cg->encounter[i],0,sizeof(cg->encounter[i]));
        }
    }
    cg->poll=1;cg->ambient_due=0;cg->encounter_due=interval();
    cg_contacts();cg->v.revision++;
}
void game_set_encounters(int level) {
    if(!cg || level<0 || level>2) return;
    if(cg->v.encounter_level!=level) {
        cg->v.encounter_level=level;cg->encounter_due=interval();cg->v.revision++;
    }
}
static void salvage(l3_vec p,int n) {
    for(int k=0;k<CG_CANS && n;k++) if(cg->cans[k].life<=0) {
        cg->cans[k]=(cg_can){l3_add(p,l3v(n*9-12,4,-18)),240,(int)(cg_rand(&cg->encounter_rng)%GAME_GOODS)};
        n--;
    }
}
static l3_vec lane_end(void) {
    game_planet *p=&cg->v.contents.planets[0];
    l3_vec d=l3_norm(l3_sub(p->position,cg->v.contents.station));
    return l3_sub(p->position,l3_scale(d,p->radius+220));
}
bool cg_encounter_spawn(cg_encounter_kind kind) {
    if(!enabled() || kind<=CE_NONE || kind>CE_LANE) return false;
    game_view *v=&cg->v;
    if(distance(v->player.position,v->contents.station)<cg->encounters.safe_radius &&
        kind!=CE_PATROL && kind!=CE_CONVOY && kind!=CE_LANE) return false;
    int rock=-1;
    if(kind==CE_PROSPECTOR) {
        if(!v->contents.belt) return false;
        for(int i=0;i<18;i++) if(v->asteroids[i].active) {rock=i;break;}
        if(rock<0) return false;
    }
    uint32_t r=cg_rand(&cg->encounter_rng);
    int n=1;
    if(kind==CE_PIRATES) n=game_pirate_pack(r,v->systems[v->system].government);
    if(kind==CE_CONVOY) n=3+(int)(r%3); /* 2-4 freighters plus escort */
    if(kind==CE_PATROL || kind==CE_HUNTERS) n=2;
    if(kind==CE_SMUGGLERS) n=4;
    if(kind==CE_DRONES) n=3+(int)(r%4);
    int slots[6],found=0;
    /* Prefer the old synthetic bank; borrow empty receiver slots as needed. */
    for(int k=0;k<GAME_CONTACTS && found<n;k++) {
        int i=(16+k)%GAME_CONTACTS;
        if(!v->contacts[i].active && cg->dead_until[i]<=v->clock) slots[found++]=i;
    }
    if(found<n) return false; /* groups are atomic, never truncated */
    float angle=between(0,2*CG_PI);
    l3_vec origin=l3_add(v->player.position,l3v(sinf(angle)*330,between(-70,70),cosf(angle)*330));
    if(kind==CE_PROSPECTOR) origin=l3_add(v->asteroids[rock].position,l3v(80,20,0));
    if(kind!=CE_PATROL && kind!=CE_CONVOY && kind!=CE_LANE &&
        distance(origin,v->contents.station)<cg->encounters.safe_radius+150)
        origin=l3_add(v->contents.station,l3_scale(l3_norm(l3_sub(v->player.position,v->contents.station)),cg->encounters.safe_radius+250));
    bool outbound=(r&8)!=0;
    if(kind==CE_LANE) {
        l3_vec start=l3_add(v->contents.station,l3v(0,0,-70));
        origin=outbound?start:lane_end();
        if(r%3==0) origin=l3_add(start,l3_scale(l3_sub(lane_end(),start),between(.15f,.85f)));
    }
    if(kind==CE_PATROL && (r&4)) origin=l3_add(v->contents.station,l3v(0,0,-80));
    for(int k=0;k<n;k++) {
        game_class type=SH_COBRA;
        game_role role=ROLE_TRADER;
        bool escort=false;
        switch(kind) {
        case CE_PIRATES:type=SH_KRAIT;role=ROLE_PIRATE;break;
        case CE_CONVOY:escort=k==n-1;type=escort?SH_VIPER:((r+k)&1?SH_BOA:SH_ANACONDA);break;
        case CE_PATROL:type=SH_VIPER;role=ROLE_POLICE;break;
        case CE_HUNTERS:type=SH_MORAY;break;
        case CE_DERELICT:type=SH_PYTHON;role=ROLE_DERELICT;break;
        case CE_DISTRESS:type=SH_ADDER;role=ROLE_RESCUE;break;
        case CE_SMUGGLERS:type=k<2?SH_ADDER:SH_VIPER;role=k<2?ROLE_TRADER:ROLE_POLICE;break;
        case CE_PROSPECTOR:type=SH_PYTHON;break;
        case CE_DRONES:type=SH_DRONE;role=ROLE_PIRATE;break;
        case CE_EXPLORER:type=SH_ASP;break;
        case CE_SALVAGE:type=SH_PROBE;role=ROLE_DERELICT;break; /* sensors own ROLE_BEACON */
        case CE_LANE:type=(r&1)?SH_ADDER:SH_BOA;break;
        default:break;
        }
        int i=slots[k];
        l3_vec offset=l3v(k?(k&1?1:-1)*(35+18*k):0,k?12:0,k?45+22*k:0);
        if(kind==CE_SMUGGLERS && k>=2) offset=l3v((k-2)*55-25,18,220);
        cg_spawn(i,cg->next_id++,type,role,l3_add(origin,offset));
        game_contact *c=&v->contacts[i];
        cg_encounter_contact *e=&cg->encounter[i];
        e->kind=(uint8_t)kind;e->life=between(180,300);e->phase=between(0,2*CG_PI);
        e->offset=offset;e->escort=escort;e->rock=rock;
        if(k && kind!=CE_SMUGGLERS) e->leader=v->contacts[slots[0]].id;
        if(kind==CE_SMUGGLERS && (k==1 || k==3)) e->leader=v->contacts[slots[k-1]].id;
        if(kind==CE_SMUGGLERS && k==3) e->offset=l3v(45,0,35);
        if(kind==CE_DRONES) {c->hull*=2;c->shield*=2;}
        if(kind==CE_HUNTERS) c->hostile=v->player.legal>0;
        if(kind==CE_SMUGGLERS && k<2) e->fleeing=true;
        if(kind==CE_DISTRESS) c->squawk=7700;
        cg->lane_out[i]=outbound;
        cg->anchor[i]=kind==CE_LANE?lane_end():
            l3_add(origin,l3_scale(l3_norm(l3_sub(origin,v->contents.station)),1000));
        snprintf(c->callsign,sizeof(c->callsign),"SIM%05u",(unsigned)(c->id%100000));
        cg_register(c);
    }
    if(kind==CE_DERELICT || kind==CE_SALVAGE) salvage(origin,3);
    if(kind!=CE_LANE) {cg->encounter_count++;cg_message(cg->encounters.messages[kind-1]);}
    return true;
}
/* Resolve enemies once a second. The old policy remains the fallback for
 * mission/radio ships; a local police/pirate fight takes priority over freight. */
static int enemy(int i) {
    game_view *v=&cg->v;
    game_contact *c=&v->contacts[i];
    cg_encounter_contact *e=&cg->encounter[i];
    if(!c->active || c->jump_out>0 || c->role==ROLE_DERELICT || c->role==ROLE_BEACON || c->role==ROLE_RESCUE) return -2;
    if(e->kind!=CE_DRONES && cg_friendly(c->id)) return -2;
    if(e->kind==CE_HUNTERS && v->player.legal) return -1;
    if(cg->provoked[i] && c->hostile) return -1;
    int best=-2;float near=800;
    for(int j=0;j<GAME_CONTACTS;j++) {
        game_contact *other=&v->contacts[j];
        if(j==i || !other->active || other->jump_out>0) continue;
        cg_encounter_contact *oe=&cg->encounter[j];
        bool drones=e->kind==CE_DRONES,alien=oe->kind==CE_DRONES;
        bool fight=(drones && !alien) || (!drones && alien) ||
            (c->role==ROLE_POLICE && (other->role==ROLE_PIRATE || (oe->kind==CE_SMUGGLERS && other->role==ROLE_TRADER))) ||
            (c->role==ROLE_PIRATE && other->role==ROLE_POLICE) ||
            (e->escort && other->role==ROLE_PIRATE);
        if(!fight || (cg_friendly(other->id) && !drones && !alien)) continue;
        float d=distance(c->position,other->position);
        if(d<near) {near=d;best=j;}
    }
    if(best!=-2) return best;
    if(e->kind==CE_DRONES) return -1;
    if(e->kind==CE_HUNTERS || e->fleeing || e->kind==CE_EXPLORER || e->kind==CE_PROSPECTOR) return -2;
    return cg_enemy(i);
}
int cg_encounter_enemy(int i) {
    game_contact *c=&cg->v.contacts[i];
    if(c->jump_out>0) return -2;
    if(c->role==ROLE_POLICE) {
        if(cg->v.player.legal) return -1;
        if(cg->encounter_enemy[i]==-1 && !cg->provoked[i]) return -2;
    }
    if(cg->encounter[i].kind==CE_HUNTERS && !cg->v.player.legal) return -2;
    if(cg->provoked[i] && c->hostile) return -1;
    if(!cg->encounter[i].kind) return enemy(i);
    int j=cg->encounter_enemy[i];
    if(j>=0 && (!cg->v.contacts[j].active || cg->v.contacts[j].jump_out>0)) return -2;
    return j;
}
void cg_encounter_hit(int i,bool player) {
    cg_encounter_contact *e=&cg->encounter[i];
    if(!e->kind) return;
    uint32_t leader=e->leader?e->leader:cg->v.contacts[i].id;
    for(int j=0;j<GAME_CONTACTS;j++) {
        cg_encounter_contact *f=&cg->encounter[j];
        if(!cg->v.contacts[j].active || !f->kind ||
            (j!=i && cg->v.contacts[j].id!=leader && f->leader!=leader)) continue;
        if(f->kind==CE_CONVOY || f->kind==CE_LANE || f->kind==CE_EXPLORER ||
            f->kind==CE_PROSPECTOR || f->kind==CE_SMUGGLERS) f->fleeing=true;
        if(player && (f->escort || f->kind==CE_PIRATES || f->kind==CE_HUNTERS)) {
            cg->provoked[j]=true;cg->v.contacts[j].hostile=true;
        }
        cg->encounter_enemy[j]=enemy(j);
    }
}
static cg_encounter_kind choose(void) {
    game_view *v=&cg->v;game_system *s=&v->systems[v->system];
    unsigned cargo=0;
    for(int g=0;g<GAME_GOODS;g++) cargo+=(unsigned)v->player.cargo[g]*(unsigned)v->market[g].price;
    int weights[CG_ENCOUNTER_KINDS],sum=0;
    bool safe=distance(v->player.position,v->contents.station)<cg->encounters.safe_radius;
    for(int i=0;i<CG_ENCOUNTER_KINDS;i++) {
        int kind=i+1,w=cg->encounters.weights[i];
        switch(kind) {
        case CE_PIRATES:w*=1+(7-s->government)/2+(int)(cargo/2000>4?4:cargo/2000)+v->player.rank/3;break;
        case CE_CONVOY:w*=1+(7-s->economy)/2;break;
        case CE_PATROL:w*=1+s->government/2;break;
        case CE_HUNTERS:w=v->player.legal>=20?w*(1+v->player.legal/32):0;break;
        case CE_SMUGGLERS:w*=1+s->government/3;break;
        case CE_PROSPECTOR: w=v->contents.belt?w*(1+s->economy/2):0;break;
        case CE_DRONES:w*=1+v->player.rank/3;break;
        case CE_EXPLORER:w*=1+s->tech/4;break;
        default:break;
        }
        if(safe && kind!=CE_PATROL && kind!=CE_CONVOY) w=0;
        weights[i]=w;sum+=w;
    }
    if(!sum) return CE_NONE;
    int pick=(int)(cg_rand(&cg->encounter_rng)%(uint32_t)sum);
    for(int i=0;i<CG_ENCOUNTER_KINDS;i++) {pick-=weights[i];if(pick<0) return (cg_encounter_kind)(i+1);}
    return CE_NONE;
}
static void chatter(void) {
    unsigned banks=distance(cg->v.player.position,cg->v.contents.station)<1100?1u:0u;
    for(int i=0;i<GAME_CONTACTS;i++) {
        game_contact *c=&cg->v.contacts[i];
        if(!c->active || distance(c->position,cg->v.player.position)>900) continue;
        if(c->role==ROLE_POLICE) banks|=2;
        if(c->role==ROLE_TRADER) banks|=4;
        if(c->role==ROLE_PIRATE && cg->encounter[i].kind!=CE_DRONES) banks|=8;
    }
    if(!banks) return;
    int bank=(int)(cg_rand(&cg->encounter_rng)%4);
    while(!(banks&(1u<<bank))) bank=(bank+1)%4;
    int line=bank*16+(int)(cg_rand(&cg->encounter_rng)%16);
    if(line==cg->last_chatter) line=bank*16+(line+1)%16;
    cg->last_chatter=line;cg_message(cg->encounters.chatter[line]);
}
/* Reuse the validated, level-scaled encounter cadence; no extra tuning table. */
void cg_cruise_director(float dt) {
    game_view *v = &cg->v;
    if (!enabled() || !v->player.cruising || v->interdiction.active ||
        distance(v->player.position, v->contents.station) <= fmaxf(1800, cg->encounters.safe_radius)) return;
    if (cg->cruise.due <= 0) cg->cruise.due = interval();
    cg->cruise.due -= dt;
    if (cg->cruise.due > 0) return;
    cg->cruise.due = interval();
    /* Contacts must still be local to the pursuit, not across the system. */
    for (int i = 0; i < GAME_CONTACTS; i++)
        if (distance(v->contacts[i].position, v->player.position) < 1200 && cg_interdict(i)) return;
    if (!cg_encounter_spawn(v->player.legal >= 128 ? CE_PATROL : CE_PIRATES)) return;
    for (int i = 0; i < GAME_CONTACTS; i++)
        if (distance(v->contacts[i].position, v->player.position) < 1200 && cg_interdict(i)) return;
}
void cg_encounters(float dt) {
    game_view *v=&cg->v;
    if(cg->encounter_system!=v->system) {
        cg->encounter_system=v->system;
        cg->encounter_rng=cg_hash(v->systems[v->system].seed ^ v->galaxy_seed);
        memset(cg->encounter,0,sizeof(cg->encounter));
        for(int i=0;i<GAME_CONTACTS;i++) cg->encounter_enemy[i]=-2;
        cg->encounter_due=interval();cg->ambient_due=0;cg->encounter_tick=0;
        cg->chatter_due=between(cg->encounters.chatter_seconds[0],cg->encounters.chatter_seconds[1]);
    }
    for(int i=0;i<GAME_CONTACTS;i++) {
        game_contact *c=&v->contacts[i];
        if(c->active && c->jump_out>0) {
            c->jump_out=fmaxf(0,c->jump_out-dt);
            if(c->jump_out<=0) {c->active=false;cg->dead_until[i]=v->clock+5;}
        }
    }
    cg->encounter_tick+=dt;
    if(cg->encounter_tick<1) return;
    float elapsed=cg->encounter_tick;cg->encounter_tick=0;
    /* NPC fights continue to select targets even with encounter spawning off. */
    for(int i=0;i<GAME_CONTACTS;i++) cg->encounter_enemy[i]=enemy(i);
    if(!enabled()) return;
    cg_cruise_director(elapsed);
    float rate=distance(v->player.position,v->contents.station)>1800?1/cg->encounters.deep_scale:1;
    cg->encounter_due-=elapsed*rate;
    if(cg->encounter_due<=0) {
        cg_encounter_spawn(choose());cg->encounter_due=interval();
    }
    cg->ambient_due-=elapsed;
    if(cg->ambient_due<=0) {
        game_system *s=&v->systems[v->system];
        int desired=2+(s->population>2)+(s->population>5)+(s->economy<3),lanes=0;
        if(desired>5) desired=5;
        for(int i=0;i<GAME_CONTACTS;i++) lanes+=v->contacts[i].active && cg->encounter[i].kind==CE_LANE;
        for(int k=lanes;k<desired;k++) if(!cg_encounter_spawn(CE_LANE)) break;
        cg->ambient_due=between(6,12);
    }
    cg->chatter_due-=elapsed;
    if(cg->chatter_due<=0) {
        chatter();cg->chatter_due=between(cg->encounters.chatter_seconds[0],cg->encounters.chatter_seconds[1]);
    }
    for(int i=0;i<GAME_CONTACTS;i++) {
        game_contact *c=&v->contacts[i];cg_encounter_contact *e=&cg->encounter[i];
        if(!c->active || !e->kind || c->jump_out>0) continue;
        e->life-=elapsed;
        if(e->life<=0 && distance(c->position,v->contents.station)>cg->encounters.safe_radius) cg_depart(i);
        if(c->role==ROLE_POLICE && distance(c->position,v->player.position)<180) {
            e->scan+=elapsed;
            if(!e->scanned && e->scan>=3) {
                e->scanned=true;
                if(v->player.cargo[5] || v->player.cargo[9]) v->player.legal=(uint8_t)fmaxf(20,v->player.legal);
                c->hostile=v->player.legal>0;
                cg_message(c->hostile?"Police scan: offender, halt now.":"Police scan: registry is clear.");
            }
        } else e->scan=0;
        if(e->kind==CE_PROSPECTOR && e->rock>=0 && v->asteroids[e->rock].active &&
            distance(c->position,v->asteroids[e->rock].position)<130) {
            e->phase+=elapsed;
            if(e->phase>=12) {
                e->phase=0;game_asteroid *a=&v->asteroids[e->rock];
                cg_beam_add(c->position,a->position,false);a->hull-=10;
                if(a->hull<=0) {a->active=false;cg_ores(a->position,c->id);}
            }
        }
        if(c->role==ROLE_TRADER && !e->escort) {
            for(int j=0;j<GAME_CONTACTS;j++) {
                game_contact *h=&v->contacts[j];
                if(h->active && (h->role==ROLE_PIRATE || cg->encounter[j].kind==CE_DRONES) &&
                    distance(c->position,h->position)<500) {cg_encounter_hit(i,false);break;}
            }
        }
    }
    for(int i=0;i<GAME_CONTACTS;i++) cg->encounter_enemy[i]=enemy(i);
}
static l3_quat face(l3_vec d) {
    return l3_quat_euler(-atan2f(d.x,-d.z)/CG_RAD,atan2f(d.y,hypotf(d.x,d.z))/CG_RAD,0);
}
bool cg_encounter_flight(int i,float dt) {
    game_view *v=&cg->v;game_contact *c=&v->contacts[i];cg_encounter_contact *e=&cg->encounter[i];
    if(c->jump_out>0) return true;
    /* An active pursuit owns this ship's flight until the contest resolves. */
    if(v->interdiction.active && v->interdiction.who==i &&
        c->id==cg->cruise.who_id && !c->live && !cg->mapped[i]) {
        c->position=l3_sub(v->player.position,l3_scale(cg_forward(),180));
        c->velocity=v->player.velocity;c->speed=v->player.speed;c->attitude=v->player.attitude;
        return true;
    }
    if(!e->kind) return false;
    int leader=leader_slot(i);
    if(e->leader && leader<0) {e->leader=0;e->fleeing=true;}
    if(leader>=0) {
        game_contact *l=&v->contacts[leader];
        if(l->jump_out>0) {cg_depart(i);return true;}
        if(!e->escort || cg_encounter_enemy(i)==-2) {
            c->position=l3_add(l->position,l3_quat_rotate(l->attitude,e->offset));
            c->attitude=l->attitude;c->velocity=l->velocity;c->speed=l->speed;
            return true;
        }
    }
    if(e->kind==CE_DERELICT || e->kind==CE_SALVAGE || c->role==ROLE_RESCUE) {
        c->velocity=e->kind==CE_DERELICT?l3v(.6f,.12f,-.3f):l3v(0,0,0);
        c->speed=cg_len(c->velocity);c->position=l3_add(c->position,l3_scale(c->velocity,dt));return true;
    }
    int foe=cg_encounter_enemy(i);
    l3_vec target=cg->anchor[i];
    bool run=e->fleeing || c->hull<game_ship(c->ship_class)->hull*.25f;
    bool docking=false;
    if(run && !e->escort) {
        if(distance(c->position,v->contents.station)<1500 && e->kind!=CE_SMUGGLERS) docking=true;
        else if(distance(c->position,cg->anchor[i])<45) {cg_depart(i);return true;}
    } else if(foe!=-2) {
        l3_vec victim=foe<0?v->player.position:v->contacts[foe].position;
        target=victim;
        if(c->role==ROLE_PIRATE || e->kind==CE_HUNTERS) {
            /* Orbit changes with actual flight integration; no decorative motion. */
            e->phase+=dt*game_ship(c->ship_class)->speed/190;
            target=l3_add(victim,l3v(cosf(e->phase)*135,sinf(e->phase*.5f)*35,sinf(e->phase)*135));
        }
    } else if(e->kind==CE_LANE) {
        if(cg->lane_out[i]) {
            /* Clear the physical slot before joining the planetary traffic lane. */
            target=distance(c->position,v->contents.station)<170?l3_add(v->contents.station,l3v(0,0,-220)):lane_end();
            if(distance(c->position,lane_end())<70) {cg_depart(i);return true;}
        } else docking=true;
    } else if(e->kind==CE_CONVOY) docking=true;
    else if(e->kind==CE_PATROL || (e->kind==CE_SMUGGLERS && c->role==ROLE_POLICE)) {
        target=l3_add(v->contents.station,l3v(cg->lane_out[i]?700:-700,80,-350));
        if(distance(c->position,target)<50) cg->lane_out[i]=!cg->lane_out[i];
    } else if(e->kind==CE_PROSPECTOR) {
        if(e->rock>=0 && v->asteroids[e->rock].active) target=l3_add(v->asteroids[e->rock].position,l3v(80,20,0));
        else {cg_depart(i);return true;}
    } else if(distance(c->position,target)<45) {cg_depart(i);return true;}
    if(docking) {
        l3_vec approach=l3_add(v->contents.station,l3v(0,0,-90));
        if(distance(c->position,approach)<20) cg->lane_out[i]=false;
        target=cg->lane_out[i]?approach:l3_add(v->contents.station,l3v(0,0,-40));
        /* Only enter the slot after reaching the approach axis. */
        if(fabsf(c->position.x-v->contents.station.x)>15 || fabsf(c->position.y-v->contents.station.y)>15) target=approach;
        if(distance(c->position,v->contents.station)<50) {
            c->active=false;cg->dead_until[i]=v->clock+10;return true;
        }
    }
    if(c->role!=ROLE_POLICE && c->role!=ROLE_TRADER &&
        distance(target,v->contents.station)<cg->encounters.safe_radius+150) {
        l3_vec away=l3_sub(c->position,v->contents.station);
        if(cg_len(away)<1) away=l3v(0,0,1);
        target=l3_add(v->contents.station,l3_scale(l3_norm(away),cg->encounters.safe_radius+160));
    }
    l3_vec delta=l3_sub(target,c->position);
    c->attitude=l3_quat_slerp(c->attitude,face(delta),fminf(1,dt*game_ship(c->ship_class)->turn/30));
    float speed=game_ship(c->ship_class)->speed*(run?.65f:.28f);
    if(e->kind==CE_LANE) speed*=.65f;
    speed=fminf(speed,cg_len(delta)*.9f);
    c->speed=speed;c->velocity=l3_scale(l3_norm(delta),speed);
    c->position=l3_add(c->position,l3_scale(c->velocity,dt));
    return true;
}
