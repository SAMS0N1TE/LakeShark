/* Exploration producers: fixed PSRAM pools, cached files and receiver snapshots.
 * No renderer, radio commands, blocking IO or per-update allocations. */
#include "internal.h"

enum { E_TOOTHY=1, E_HARBOUR=2, E_BEACON=4, E_STAR=8, E_NIGHT=16,
    E_COMET=32, E_LUNCH=64, E_HARMONY=128 };
enum { SCAN=0, SCOOP, SENSOR, GRAVITY, ION };
#define EXPLORE_BYTES 1536
static const char defaults[] =
#include "explore_default.inc"
;

/* Deliberately small JSON subset: printable ASCII strings and nonnegative numbers.
 * config.c's reader is private; this independent schema also carries lore. */
static const char *space(const char *p) {
    while (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n') p++;
    return p;
}
static bool take(const char **p,char c) {
    *p=space(*p); if(**p!=c) return false; ++*p; return true;
}
static bool string(const char **p,char *out,int cap) {
    if(!take(p,'"')) return false;
    int n=0;
    while(**p && **p!='"') {
        char c=*(*p)++;
        if(c=='\\') {c=*(*p)++;if(c!='\\' && c!='"') return false;}
        if(c<32 || c>126 || n>=cap-1) return false;
        out[n++]=c;
    }
    out[n]=0;return n && take(p,'"');
}
static bool number(const char **p,float *out,float lo,float hi,bool integral) {
    *p=space(*p); const char *start=*p,*q=start;
    if(*q<'0' || *q>'9') return false;
    if(*q=='0') q++; else while(*q>='0' && *q<='9') q++;
    if(*q=='.') {q++;if(*q<'0' || *q>'9') return false;while(*q>='0' && *q<='9') q++;}
    if(*q=='e' || *q=='E') {
        q++;if(*q=='+' || *q=='-') q++;
        if(*q<'0' || *q>'9') return false;
        while(*q>='0' && *q<='9') q++;
    }
    char *end;float f=strtof(start,&end);
    if(end!=q || !isfinite(f) || f<lo || f>hi || (integral && f!=floorf(f))) return false;
    *p=q;*out=f;return true;
}
static bool parse(const char *p,cg_poi_rules *r) {
    unsigned seen=0;float v;
    if(!take(&p,'{')) return false;
    do {
        char key[12];if(!string(&p,key,sizeof(key)) || !take(&p,':')) return false;
        unsigned bit=!strcmp(key,"version")?1:!strcmp(key,"tuning")?2:
            !strcmp(key,"kinds")?4:!strcmp(key,"ranks")?8:0;
        if(!bit || (seen&bit)) return false;
        seen|=bit;
        if(bit==1) {if(!number(&p,&v,1,1,true)) return false;}
        else if(bit==2) {
            float t[5];
            if(!take(&p,'[')) return false;
            for(int i=0;i<5;i++) {
                float lo=i==SENSOR?500:i==ION?8:1;
                float hi=i==SCAN?30:i==SCOOP?60:i==SENSOR?5000:i==GRAVITY?1000:100;
                if((i && !take(&p,',')) || !number(&p,&t[i],lo,hi,false)) return false;
            }
            if(!take(&p,']')) return false;
            r->scan_seconds=t[SCAN];r->scoop_seconds=t[SCOOP];r->scanner_range=t[SENSOR];
            r->gravity=t[GRAVITY];r->ion_drain=t[ION];
        } else if(bit==4) {
            int weights=0;
            if(!take(&p,'[')) return false;
            for(int i=0;i<POI_KIND_COUNT;i++) {
                cg_poi_kind *k=&r->kinds[i];
                if((i && !take(&p,',')) || !take(&p,'[') || !string(&p,k->name,21) ||
                    !take(&p,',') || !take(&p,'[')) return false;
                for(int j=0;j<3;j++) if((j && !take(&p,',')) || !string(&p,k->text[j],31)) return false;
                if(!take(&p,']') || !take(&p,',') || !number(&p,&k->radius,20,1000,false) ||
                    !take(&p,',') || !number(&p,&k->size,1,1000,false) || !take(&p,',') ||
                    !number(&p,&v,0,15,true)) return false;
                k->colour=(uint8_t)v;
                if(!take(&p,',') || !number(&p,&v,1,100000,true)) return false;
                k->reward=(uint32_t)v;
                if(!take(&p,',') || !number(&p,&v,0,100,true) || !take(&p,']')) return false;
                k->weight=(uint8_t)v;weights+=k->weight;
            }
            if(!weights || !take(&p,']')) return false;
        } else {
            if(!take(&p,'[')) return false;
            for(int i=0;i<9;i++) {
                if((i && !take(&p,',')) || !take(&p,'[') || !number(&p,&v,0,1538,true) ||
                    (!i && v!=0) || (i && v<=r->ranks[i-1]) || !take(&p,',') ||
                    !string(&p,r->rank_names[i],21) || !take(&p,']')) return false;
                r->ranks[i]=(uint32_t)v;
            }
            if(!take(&p,']')) return false;
        }
        if(take(&p,'}')) break;
        if(!take(&p,',')) return false;
    } while(*p);
    return seen==15 && !*space(p);
}
bool cg_poi_config(void) {
    char *b=(char *)cg->save_buffer;int fd=lap_file_open("explore.json","r");
    bool valid;
    if(fd<0) {
        valid=parse(defaults,&cg->poi.rules);
        fd=lap_file_open("explore.json","w");
        if(fd>=0) {lap_file_write(fd,defaults,sizeof(defaults)-1);lap_file_close(fd);}
    } else {
        int n=lap_file_read(fd,b,sizeof(cg->save_buffer)-1);lap_file_close(fd);
        valid=n>0 && n<(int)sizeof(cg->save_buffer)-1;
        if(valid) {b[n]=0;valid=parse(b,&cg->poi.rules);}
    }
    if(!valid) parse(defaults,&cg->poi.rules);
    cg->poi.configured=true;
    return valid;
}
static void rank_update(void) {
    uint8_t r=0;while(r<8 && cg->v.discoveries.count>=cg->poi.rules.ranks[r+1]) r++;
    cg->v.discoveries.rank=r;
}
const char *game_explorer_rank(int rank) {
    return cg && rank>=0 && rank<9 ? cg->poi.rules.rank_names[rank] : "";
}
static void make(game_poi *p,game_poi_kind kind,uint32_t seed,l3_vec position) {
    memset(p,0,sizeof(*p));p->kind=kind;p->seed=seed;p->position=position;
    const cg_poi_kind *k=&cg->poi.rules.kinds[kind];
    p->radius=k->radius;p->size=k->size;p->colour=k->colour;
    p->scan_range=k->radius+220;
    memcpy(p->name,k->name,sizeof(p->name));memcpy(p->scan_text,k->text,sizeof(p->scan_text));
    p->variant=(uint8_t)(cg_hash(seed)%3);
    if(kind==POI_RELIC) p->variant=0;
    if(kind==POI_COMET || kind==POI_ION_STORM) {
        p->velocity=l3_scale(l3_norm(l3v((float)(seed%31)-15,3,(float)((seed/31)%29)-14)),
            kind==POI_COMET?12:5);
        p->tail=l3_scale(l3_norm(p->velocity),-1);
    }
}
int game_poi_generate(uint32_t seed,game_poi out[GAME_POIS]) {
    if(!cg || !cg->poi.configured || !out) return 0;
    memset(out,0,sizeof(*out)*GAME_POIS);
    uint32_t rng=cg_hash(seed^0x504f4931);int n=(int)(cg_rand(&rng)%5),total=0;
    for(int k=0;k<POI_KIND_COUNT;k++) total+=cg->poi.rules.kinds[k].weight;
    for(int i=0;i<n;i++) {
        int roll=(int)(cg_rand(&rng)%(uint32_t)total),kind=0;
        while(roll>=cg->poi.rules.kinds[kind].weight) roll-=cg->poi.rules.kinds[kind++].weight;
        /* Separate angular sectors keep even large regions from overlapping. */
        float angle=(i+.12f+cg_unit(&rng)*.18f)*(2*CG_PI/4);
        float dist=4200+cg_unit(&rng)*2200;
        /* Above the planetary lanes: a survey target cannot be inside a world. */
        l3_vec pos=l3v(cosf(angle)*dist,1600+cg_unit(&rng)*600,sinf(angle)*dist);
        make(&out[i],(game_poi_kind)kind,cg_rand(&rng),pos);
    }
    return n;
}

/* CXP1: fixed 1536 bytes, little endian, galaxy-bound, checksum over payload.
 * Commander slots remain independent; credits use the existing commander save. */
static uint32_t sum(const unsigned char *p,int n) {
    uint32_t h=2166136261u;while(n--) h=(h^*p++)*16777619u;return h;
}
static void put(unsigned char **p,uint32_t n) {
    for(int i=0;i<4;i++) {*(*p)++=(uint8_t)n;n>>=8;}
}
static uint32_t get(const unsigned char **p) {
    uint32_t n=0;for(int i=0;i<4;i++) n|=(uint32_t)*(*p)++<<(i*8);return n;
}
static cg_poi_marks *marks(void) {return &cg->poi.marks[cg->v.hidden_system?GAME_SYSTEMS:cg->v.system];}
bool game_explore_save(void) {
    if(!cg || !cg->poi.loaded) return false;
    unsigned char *start=cg->poi.buffer,*p=start;
    memset(start,0,EXPLORE_BYTES);memcpy(p,"CXP1",4);p+=4;
    put(&p,EXPLORE_BYTES);put(&p,cg->v.galaxy_seed);put(&p,cg->v.discoveries.count);
    put(&p,cg->v.found_eggs);put(&p,cg->v.relic_chain);put(&p,cg->poi.route_step);
    put(&p,cg->v.hidden_system?(uint32_t)cg->v.system+1:(uint32_t)cg->poi.saved_hidden);
    for(int i=0;i<=GAME_SYSTEMS;i++) {
        *p++=cg->poi.marks[i].discovered;*p++=cg->poi.marks[i].scanned;*p++=cg->poi.marks[i].answered;
    }
    memcpy(p,cg->v.discoveries.names,sizeof(cg->v.discoveries.names));p+=sizeof(cg->v.discoveries.names);
    for(int i=0;i<16;i++) put(&p,cg->poi.radio_ids[i]);
    put(&p,cg->poi.radio_done);
    unsigned char *end=start+EXPLORE_BYTES-4;put(&end,sum(start,EXPLORE_BYTES-4));
    int fd=lap_file_open("explore.sav","w");
    bool ok=fd>=0 && lap_file_write(fd,start,EXPLORE_BYTES)==EXPLORE_BYTES;
    if(fd>=0) lap_file_close(fd);
    if(ok) cg->poi.dirty=false;
    return ok;
}
bool cg_poi_load(void) {
    cg_poi_state *s=&cg->poi;game_view *v=&cg->v;
    memset(s->marks,0,sizeof(s->marks));memset(s->radio_ids,0,sizeof(s->radio_ids));
    s->radio_done=0;
    memset(&v->discoveries,0,sizeof(v->discoveries));v->found_eggs=0;v->relic_chain=0;
    s->route_step=0;s->hidden_pending=false;s->saved_hidden=0;s->loaded=true;s->galaxy=v->galaxy_seed;s->dirty=true;
    int fd=lap_file_open("explore.sav","r"),n=0;
    if(fd>=0) {n=lap_file_read(fd,s->buffer,EXPLORE_BYTES);lap_file_close(fd);}
    const unsigned char *b=s->buffer,*end=b+EXPLORE_BYTES-4;
    if(n!=EXPLORE_BYTES || lap_file_size("explore.sav")!=EXPLORE_BYTES ||
        memcmp(b,"CXP1",4) || get(&end)!=sum(b,EXPLORE_BYTES-4)) return false;
    b+=4;if(get(&b)!=EXPLORE_BYTES || get(&b)!=v->galaxy_seed) return false;
    uint32_t count=get(&b),eggs=get(&b),chain=get(&b),route=get(&b),hidden=get(&b);
    if(count>1044 || eggs>255 || chain>5 || route>2 || hidden>GAME_SYSTEMS || (hidden && !(eggs&E_HARBOUR))) return false;
    const unsigned char *entries=b;
    unsigned scans=0;
    for(int i=0;i<=GAME_SYSTEMS;i++) {
        unsigned d=*b++,saw=*b++,a=*b++;
        if((d|saw|a)>15 || (saw&~d) || (a&~saw)) return false;
        for(unsigned bit=1;bit<=8;bit<<=1) scans+=(saw&bit)!=0;
    }
    const unsigned char *names=b;
    for(int i=0;i<16;i++,b+=21) {
        if(b[20]) return false;
        for(int j=0;j<20 && b[j];j++) if(b[j]<32 || b[j]>126) return false;
    }
    const unsigned char *radios=b;
    uint32_t paid=0;
    for(int i=0;i<16;i++) {uint32_t id=get(&b);if(id&0x80000000u) {scans++;paid|=1u<<i;}}
    uint32_t done=get(&b);
    if(scans!=count || (done&~paid)) return false;
    b=entries;
    for(int i=0;i<=GAME_SYSTEMS;i++) {
        s->marks[i].discovered=*b++;s->marks[i].scanned=*b++;s->marks[i].answered=*b++;
    }
    memcpy(v->discoveries.names,names,sizeof(v->discoveries.names));
    b=radios;for(int i=0;i<16;i++) s->radio_ids[i]=get(&b);
    s->radio_done=done;
    v->discoveries.count=count;v->found_eggs=eggs;v->relic_chain=(uint8_t)chain;
    s->route_step=(uint8_t)route;s->saved_hidden=(int)hidden;
    s->dirty=false;rank_update();return true;
}
static int special(int which) {
    int n=(int)(cg_hash(cg->v.galaxy_seed^0x45474753)%GAME_SYSTEMS);
    /* Three unique egg locations, also disjoint from the five chain stones. */
    for(int k=0;k<=which;k++) {
        for(;;) {
            bool used=false;for(int j=0;j<5;j++) used|=n==cg->poi.chain[j];
            if(!used) break;
            n=(n+1)%GAME_SYSTEMS;
        }
        if(k!=which) n=(n+1)%GAME_SYSTEMS;
    }
    return n;
}
static void routes(void) {
    uint32_t r=cg_hash(cg->v.galaxy_seed^0x52454c49);
    for(int i=0;i<5;i++) cg->poi.chain[i]=(int)((r+i*53u)%GAME_SYSTEMS);
    int from=0;
    for(int j=0;j<3;j++) {
        float best=1e9f;int pick=-1;
        for(int i=1;i<GAME_SYSTEMS;i++) {
            bool used=i==from;
            for(int k=0;k<j;k++) used|=i==cg->poi.route[k];
            float d=game_distance(&cg->v.systems[from],&cg->v.systems[i]);
            if(!used && d<=7 && d<best) {best=d;pick=i;}
        }
        cg->poi.route[j]=pick;
        if(pick<0) break;
        from=pick;
    }
}
static void egg(unsigned bit,const char *message) {
    if(cg->v.found_eggs&bit) return;
    cg->v.found_eggs|=bit;cg->poi.dirty=true;cg_award(250);cg_message(message);
}
void cg_poi_route(int destination) {
    cg_poi_state *s=&cg->poi;
    s->saved_hidden=0;
    if(cg->v.system==0 && destination==s->route[0]) s->route_step=1;
    else if(s->route_step && destination==s->route[s->route_step]) s->route_step++;
    else s->route_step=0;
    if(s->route_step==3) {
        s->route_step=0;s->hidden_pending=true;
        egg(E_HARBOUR,"A hidden harbour: Codfather's Rest.");
    }
    s->dirty=true;
}
void cg_poi_arrive(void) {
    game_view *v=&cg->v;cg_poi_state *s=&cg->poi;
    if(!s->configured) v->config_valid=cg_poi_config() && v->config_valid;
    routes();
    if(!s->loaded || s->galaxy!=v->galaxy_seed) cg_poi_load();
    v->hidden_system=s->hidden_pending || s->saved_hidden==v->system+1;s->hidden_pending=false;
    if(v->hidden_system) s->saved_hidden=v->system+1;
    v->hidden_system_found=(v->found_eggs&E_HARBOUR)!=0;
    memset(v->exploration_station,0,sizeof(v->exploration_station));
    uint32_t seed=v->systems[v->system].seed;
    if(v->hidden_system) {
        seed=cg_hash(v->galaxy_seed^0x48494445);
        game_system_contents(seed,&v->contents);
        snprintf(v->exploration_station,21,"Codfather's Rest");
    }
    v->poi_count=game_poi_generate(seed,v->pois);
    int forced=-1,variant=0;
    for(int i=0;i<5;i++) if(v->system==s->chain[i]) {forced=POI_RELIC;variant=i+2;}
    if(v->system==special(0)) forced=POI_TOOTHY;
    if(v->system==special(1)) forced=POI_BEACON;
    if(v->system==special(2)) {forced=POI_RELIC;variant=1;}
    if(v->hidden_system) {forced=POI_GENERATION;variant=0;}
    if(forced>=0) {
        make(&v->pois[0],(game_poi_kind)forced,cg_hash(seed^0x53454352),l3v(4400,1800,-3400));
        v->pois[0].variant=(uint8_t)variant;
        if(!v->poi_count) v->poi_count=1;
        if(forced==POI_RELIC && variant==1) {
            snprintf(v->pois[0].name,21,"Third-watch stone");
            snprintf(v->pois[0].scan_text[1],31,"Answers only 03:00 to 04:00.");
        }
        if(forced==POI_BEACON && s->route[2]>=0) {
            snprintf(v->pois[0].scan_text[0],31,"LAKESHARK: HOME > %.9s",v->systems[s->route[0]].name);
            snprintf(v->pois[0].scan_text[1],31,"%.12s > %.12s",v->systems[s->route[1]].name,v->systems[s->route[2]].name);
            snprintf(v->pois[0].scan_text[2],31,"Three jumps. A hidden harbour.");
        }
    }
    cg_poi_marks *m=marks();
    for(int i=0;i<v->poi_count;i++) {
        v->pois[i].discovered=(m->discovered&(1u<<i))!=0;
        v->pois[i].scanned=(m->scanned&(1u<<i))!=0;
        v->pois[i].answered=(m->answered&(1u<<i))!=0;
        v->pois[i].scan_progress=v->pois[i].scanned?1:0;
    }
    memset(s->scoop,0,sizeof(s->scoop));s->activated=0;s->shield_valid=false;
    s->radio_poll=1;s->star_hold=s->quiet_hold=0;
    v->scanning_poi=v->signal_poi=-1;v->poi_scan_progress=v->signal_bearing=0;
    v->in_nebula=v->scanner_scrambled=false;v->nebula_density=0;v->scanner_scale=1;
    v->relic_waypoint=v->relic_chain<5?s->chain[v->relic_chain]:-1;
    /* game_init arrives once with GPS's galaxy, then loads the commander's
     * galaxy. Preserve an existing file until that second arrival has happened.
     * Missing/short files can be primed now; valid-sized caches already fit. */
    if(cg->save_seq || lap_file_size("explore.sav")<EXPLORE_BYTES) game_explore_save();
}

bool game_poi_detectable(l3_vec observer,l3_vec target) {
    if(!cg) return true;
    float range=cg->poi.rules.scanner_range*cg->v.scanner_scale;
    return l3_dot(l3_sub(observer,target),l3_sub(observer,target))<=range*range;
}
static int radio_slot(uint32_t id,bool create) {
    uint32_t key=cg_hash(id)&0x7fffffffu;if(!key) key=1;
    for(int i=0;i<16;i++) if((cg->poi.radio_ids[i]&0x7fffffffu)==key) return i;
    if(create) for(int i=0;i<16;i++) if(!cg->poi.radio_ids[i]) {
        cg->poi.radio_ids[i]=key;cg->poi.dirty=true;return i;
    }
    return -1; /* bounded per-galaxy live discoveries, never evict paid sources */
}
static void radio(void) {
    game_view *v=&cg->v;
    for(int i=0;i<GAME_CONTACTS && v->poi_count<GAME_POIS;i++) {
        game_contact *c=&v->contacts[i];
        if(!c->active || !c->live || (c->squawk!=7700 && c->squawk!=7600 && c->squawk!=7500)) continue;
        uint32_t id=cg_hash(c->id^(uint32_t)c->squawk);if(!id) id=1;
        bool exists=false;
        for(int j=0;j<v->poi_count;j++) exists|=v->pois[j].source_id==id;
        if(exists) continue;
        int slot=radio_slot(id,true);if(slot<0) continue;
        game_poi *p=&v->pois[v->poi_count++];make(p,POI_SIGNAL,id,c->position);
        p->source_id=id;p->squawk=c->squawk;p->variant=c->squawk==7700?2:c->squawk==7600?0:1;
        p->scanned=(cg->poi.radio_ids[slot]&0x80000000u)!=0;p->discovered=p->scanned;
        p->answered=(cg->poi.radio_done&(1u<<slot))!=0;p->scan_progress=p->scanned?1:0;
        snprintf(p->name,21,"Signal %04u",(unsigned)c->squawk);
        snprintf(p->scan_text[0],31,"Radio source %04u",(unsigned)c->squawk);
    }
}
static bool canisters(game_poi *p,int good) {
    bool made=false;
    for(int i=0,n=0;i<CG_CANS && n<3;i++) if(cg->cans[i].life<=0) {
        cg->cans[i]=(cg_can){l3_add(p->position,l3v(n*9-9,0,10)),180,good};n++;made=true;
    }
    return made;
}
static bool contact(game_poi *p,game_role role) {
    for(int i=16;i<GAME_CONTACTS;i++) if(!cg->v.contacts[i].active) {
        cg_spawn(i,0x70000000u|(p->seed&0xffffff),SH_PROBE,role,p->position);
        cg->v.contacts[i].hostile=role==ROLE_PIRATE;
        cg->v.contacts[i].squawk=role==ROLE_RESCUE?7700:0;
        snprintf(cg->v.contacts[i].name,16,"%s",role==ROLE_PIRATE?"Wreck sentry":"Lost Lantern");
        return true;
    }
    return false;
}
static void log_scan(game_poi *p,int index) {
    game_view *v=&cg->v;
    if(p->scanned) return;
    p->discovered=true;p->scanned=true;p->scan_progress=1;
    if(p->source_id) {
        int slot=radio_slot(p->source_id,false);
        if(slot<0 || (cg->poi.radio_ids[slot]&0x80000000u)) return;
        cg->poi.radio_ids[slot]|=0x80000000u;
    } else {
        marks()->scanned|=(uint8_t)(1u<<index);
        marks()->discovered|=(uint8_t)(1u<<index);
    }
    memmove(v->discoveries.names[1],v->discoveries.names[0],15*21);
    memcpy(v->discoveries.names[0],p->name,21);v->discoveries.count++;rank_update();
    cg_award(cg->poi.rules.kinds[p->kind].reward);
    cg->poi.dirty=true;cg_message(p->scan_text[0]);
    if(p->kind==POI_TOOTHY) egg(E_TOOTHY,"Old Toothy grins. A gold tooth for you.");
    if(p->kind==POI_BEACON) egg(E_BEACON,"LAKESHARK acknowledges its finder.");
    if(p->kind==POI_BINARY && cg->v.player.speed<1 && !cg->v.player.firing)
        egg(E_HARMONY,"Two suns hum in perfect harmony.");
}
static void relic(game_poi *p,int i,float d,float dt) {
    game_view *v=&cg->v;bool allowed=true;
    if(p->variant==1) {lap_time_t t={0};lap_time(&t);allowed=t.hour==3;}
    if(p->variant>=2) allowed=v->relic_chain==p->variant-2;
    if(p->answered || !allowed || d>p->radius+40) {p->hold_progress=0;return;}
    p->hold_progress=fminf(1,p->hold_progress+dt/5);
    if(p->hold_progress<.99999f || !p->scanned) return;
    p->answered=true;marks()->answered|=(uint8_t)(1u<<i);cg->poi.dirty=true;
    cg_award(1200);cg_message("The stone answers: follow the quiet current.");
    if(p->variant==1) egg(E_NIGHT,"Third watch: even the dark has a dawn.");
    if(p->variant>=2 && v->relic_chain<5) {
        v->relic_chain++;v->relic_waypoint=v->relic_chain<5?cg->poi.chain[v->relic_chain]:-1;
        if(v->relic_waypoint>=0) {
            snprintf(p->scan_text[2],31,"Next stone: %.15s",v->systems[v->relic_waypoint].name);
            cg_message(p->scan_text[2]);
        } else {cg_award(10000);cg_message("Five voices: the gulf remembers your name.");}
    }
}
static void salvage(game_poi *p,int i,float d) {
    if(d>p->scan_range || p->answered) return;
    bool done=false;
    if(p->kind==POI_SIGNAL) {
        done=p->variant==0?canisters(p,11):contact(p,p->variant==1?ROLE_PIRATE:ROLE_RESCUE);
    } else if(p->kind==POI_WRECK || p->kind==POI_RUINS || p->kind==POI_GENERATION) {
        /* Retry if contact/canister pools are occupied; no stolen live slots. */
        if(!(cg->poi.activated&(1u<<i))) {
            if(!canisters(p,11)) return;
            cg->poi.activated|=(uint8_t)(1u<<i);
            if(p->variant==2) cg->v.player.missiles=(uint8_t)fminf(4,cg->v.player.missiles+1);
        }
        done=p->variant!=1 || contact(p,ROLE_PIRATE);
        if(p->kind==POI_WRECK && cg->v.player.cargo[0])
            egg(E_LUNCH,"Wreck lunchbox: thanks for bringing food.");
    }
    if(done) {
        p->answered=true;cg->poi.dirty=true;
        if(!p->source_id) marks()->answered|=(uint8_t)(1u<<i);
        else {int slot=radio_slot(p->source_id,false);if(slot>=0) cg->poi.radio_done|=1u<<slot;}
    }
}
void cg_poi_update(float dt) {
    if(!cg || !isfinite(dt) || dt<=0) return;
    game_view *v=&cg->v;game_player *player=&v->player;cg_poi_state *s=&cg->poi;
    if(!s->loaded) cg_poi_arrive();
    s->radio_poll+=dt;
    if(s->radio_poll>=1) {s->radio_poll=fmodf(s->radio_poll,1);radio();}
    v->in_nebula=false;v->nebula_density=0;v->scanner_scrambled=false;v->scanner_scale=1;
    v->scanning_poi=v->signal_poi=-1;v->poi_scan_progress=v->signal_bearing=0;
    v->hidden_system_found=(v->found_eggs&E_HARBOUR)!=0;
    bool flying=!player->docked && !player->dead && v->jump==JUMP_IDLE;
    float signal=1e20f;
    for(int i=0;i<v->poi_count;i++) {
        game_poi *p=&v->pois[i];
        /* Move only during active simulation; wrap locally, preserving the seed. */
        if(flying) {
            p->position=l3_add(p->position,l3_scale(p->velocity,dt));
            if(cg_len(p->position)>10000) p->velocity=l3_scale(p->velocity,-1);
            if(p->kind==POI_COMET) p->tail=l3_scale(l3_norm(p->velocity),-1);
        }
        float d=cg_len(l3_sub(player->position,p->position));
        if(p->kind==POI_SIGNAL && !p->answered && d<signal) {
            signal=d;v->signal_poi=i;
            l3_vec delta=l3_sub(p->position,player->position);
            l3_vec right=l3_quat_rotate(player->attitude,l3v(1,0,0));
            v->signal_bearing=atan2f(l3_dot(delta,right),l3_dot(delta,cg_forward()))/CG_RAD;
        }
        if(!flying) {p->hold_progress=0;if(!p->scanned) p->scan_progress=0;continue;}
        if(p->kind==POI_NEBULA && d<p->radius) {
            v->in_nebula=true;v->nebula_density=fmaxf(v->nebula_density,1-d/p->radius);
        }
        if(p->kind==POI_ION_STORM && d<p->radius) {
            v->scanner_scrambled=true;player->energy=fmaxf(0,player->energy-s->rules.ion_drain*dt);
        }
        if(p->kind==POI_DARK_STAR && d<p->radius) {
            if(d<p->radius*.12f) {
                player->hull=0;player->dead=true;player->firing=false;
                cg_message("Beyond the dark rim. Commander lost.");flying=false;
            } else {
                l3_vec pull=l3_norm(l3_sub(p->position,player->position));
                float accel=s->rules.gravity*(1-d/p->radius);
                player->velocity=l3_add(player->velocity,l3_scale(pull,accel*dt));
                player->speed=cg_len(player->velocity);
            }
        }
        if(p->kind==POI_PULSAR && d<p->radius)
            player->energy=fmaxf(0,player->energy-8*dt);
        bool tail=false;
        if(p->kind==POI_COMET) {
            l3_vec delta=l3_sub(player->position,p->position);float along=l3_dot(delta,p->tail);
            tail=along>0 && along<800 && cg_len(l3_sub(delta,l3_scale(p->tail,along)))<p->radius;
            if(tail && player->throttle==0) {
                s->quiet_hold+=dt;
                if(s->quiet_hold>=5) egg(E_COMET,"Quiet wake: a comet carries no timetable.");
            } else s->quiet_hold=0;
        }
        if((tail || (p->kind==POI_NEBULA && d<p->radius)) &&
            (player->equipment&(1u<<EQ_SCOOP)) && game_cargo_used()<game_cargo_capacity()) {
            s->scoop[i]+=dt;
            if(s->scoop[i]>=s->rules.scoop_seconds) {
                s->scoop[i]-=s->rules.scoop_seconds;
                /* Existing catalog: radioactives = rare ion gas; minerals = ice. */
                player->cargo[tail?11:2]++;cg_message(tail?"Comet ice secured as minerals.":"Rare ion gas secured.");
            }
        } else s->scoop[i]=0;
    }
    v->scanner_scale=v->in_nebula?.5f:1;
    s->shield_fore=player->shield_fore;s->shield_aft=player->shield_aft;s->shield_valid=true;
    if(flying && v->in_nebula) {
        /* Existing combat uses a 420-unit visual envelope. Cloud cover halves it.
         * AI consumers can also use game_poi_detectable for their search logic. */
        for(int i=0;i<GAME_CONTACTS;i++) {
            game_contact *c=&v->contacts[i];
            if(c->active && c->role==ROLE_PIRATE && cg_len(l3_sub(c->position,player->position))>210)
                cg->cooldown[i]=fmaxf(cg->cooldown[i],.2f);
        }
    }
    float nearest=1e20f;
    for(int i=0;i<v->poi_count;i++) {
        game_poi *p=&v->pois[i];float d=cg_len(l3_sub(player->position,p->position));
        if(!flying) continue;
        if(d<s->rules.scanner_range*v->scanner_scale && !p->discovered) {
            p->discovered=true;s->dirty=true;
            if(!p->source_id) marks()->discovered|=(uint8_t)(1u<<i);
        }
        if(!p->scanned && d<p->scan_range*v->scanner_scale && !v->scanner_scrambled && d<nearest) {
            nearest=d;v->scanning_poi=i;
        }
        if(p->kind==POI_RELIC) relic(p,i,d,dt);
        if(p->scanned) salvage(p,i,d);
    }
    for(int i=0;i<v->poi_count;i++) {
        game_poi *p=&v->pois[i];
        if(i==v->scanning_poi) {
            p->scan_progress=fminf(1,p->scan_progress+dt/s->rules.scan_seconds);
            if(p->scan_progress>=.99999f) log_scan(p,i);
            v->poi_scan_progress=p->scan_progress;
        } else if(!p->scanned) p->scan_progress=0;
    }
    /* The star test is measured from its surface, outside the fatal core. */
    float star_distance=cg_len(l3_sub(player->position,v->contents.star))-v->contents.star_radius;
    if(flying && star_distance>=v->contents.star_radius*.02f && star_distance<=50 && player->throttle==0) {
        s->star_hold+=dt;if(s->star_hold>=9.9999f) egg(E_STAR,"The star says: take a breath, little shark.");
    } else s->star_hold=0;
    if(s->dirty) game_explore_save();
}
