/* Commander formats CRR1/CRR2/CRR3 + YRD1: little endian, versioned, length + checksum, two
 * slots. lap_file_* only touches the PSRAM cache; firmware's existing IO worker
 * flushes. */
#include "internal.h"
/* CBT2 appends mount modes to CBT1 in reserved padding, preserving CRR/YRD1.
 * It has its own checksum and binds to both sequence and primary checksum. */
#define COMBAT_OFFSET 8000
#define COMBAT_BYTES 112
_Static_assert(188 + GAME_REGISTRY * 55 + 358 + 6*65 + 16 <= COMBAT_OFFSET, "commander overlaps combat");
_Static_assert(COMBAT_OFFSET+COMBAT_BYTES<=8192, "combat pool too small");
static void put32(unsigned char **p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        *(*p)++ = (uint8_t)v;
        v >>= 8;
    }
}
static uint32_t get32(const unsigned char **p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)*(*p)++ << (i * 8);
    return v;
}
static void putf(unsigned char **p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}
static float getf(const unsigned char **p) {
    uint32_t v = get32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static uint32_t checksum(const unsigned char *b, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
static void combat_write(uint32_t sequence,uint32_t primary) {
    const game_player *p=&cg->v.player;
    if(p->pips[0]+p->pips[1]+p->pips[2]!=6) return; /* legacy state */
    unsigned char *b=cg->save_buffer+COMBAT_OFFSET;
    memcpy(b,"CBT2",4);b+=4;put32(&b,sequence);put32(&b,primary);b+=4;
    for(int i=0;i<3;i++) putf(&b,p->pips[i]);
    for(int i=0;i<GAME_MODULES;i++) putf(&b,p->modules[i]);
    putf(&b,p->canopy);putf(&b,p->oxygen);putf(&b,p->weapon_capacitor);
    for(int i=0;i<2;i++) putf(&b,p->shield_reboot[i]);
    putf(&b,p->boost_left);putf(&b,p->boost_cooldown);putf(&b,p->chaff_left);
    *b++=p->shield_cells;*b++=p->chaff;*b++=p->heat_sinks;*b++=0;
    for(int i=0;i<GAME_MOUNTS;i++) {
        *b++=p->weapons[i].kind;*b++=p->weapons[i].group;
        *b++=(uint8_t)p->weapons[i].ammo;*b++=(uint8_t)(p->weapons[i].ammo>>8);
    }
    for(int i=0;i<GAME_MOUNTS;i++) *b++=p->weapons[i].mode;
    b=cg->save_buffer+COMBAT_OFFSET+12;
    put32(&b,checksum(cg->save_buffer+COMBAT_OFFSET+16,COMBAT_BYTES-16));
}
static bool combat_read(game_player *p,uint32_t sequence,uint32_t primary) {
    cg_combat_player_init(p);
    const unsigned char *b=cg->save_buffer+COMBAT_OFFSET;
    bool modes=!memcmp(b,"CBT2",4);
    if(!modes && memcmp(b,"CBT1",4)) return true; /* CRR1/2/3 before combat */
    b+=4;uint32_t seq=get32(&b),base=get32(&b),sum=get32(&b);
    if(seq!=sequence || base!=primary) return true; /* older padding after migration */
    if(sum!=checksum(b,(modes?COMBAT_BYTES:108)-16)) return false;
    float total=0;
    for(int i=0;i<3;i++) {float v=getf(&b);
        if(!isfinite(v) || v<0 || v>4 || v*2!=floorf(v*2)) return false;
        total+=v;p->pips[i]=v;}
    if(total!=6) return false;
    for(int i=0;i<GAME_MODULES;i++) {float v=getf(&b);
        if(!isfinite(v) || v<0 || v>1) return false;
        p->modules[i]=v;}
    float a[8];
    for(int i=0;i<8;i++) {a[i]=getf(&b);if(!isfinite(a[i]) || a[i]<0) return false;}
    if(a[0]>1 || a[1]>cg->rules.combat.defence[CF_OXYGEN] || a[2]>100 ||
        a[3]>cg->rules.combat.defence[CF_REBOOT]*10 || a[4]>cg->rules.combat.defence[CF_REBOOT]*10 ||
        a[5]>cg->rules.combat.defence[CF_BOOST_TIME] || a[6]>cg->rules.combat.defence[CF_BOOST_COOLDOWN] ||
        a[7]>cg->rules.combat.defence[CF_CHAFF]) return false;
    p->canopy=a[0];p->oxygen=a[1];p->weapon_capacitor=a[2];p->shield_reboot[0]=a[3];p->shield_reboot[1]=a[4];
    p->boost_left=a[5];p->boost_cooldown=a[6];p->chaff_left=a[7];
    if(b[0]>cg->rules.combat.supplies[0] || b[1]>cg->rules.combat.supplies[1] ||
        b[2]>cg->rules.combat.supplies[2] || b[3]) return false;
    p->shield_cells=b[0];p->chaff=b[1];p->heat_sinks=b[2];b+=4;
    int lasers=0;
    for(int i=0;i<GAME_MOUNTS;i++) {
        int kind=*b++,group=*b++;unsigned ammo=b[0]+((unsigned)b[1]<<8);b+=2;
        if((kind>=WEAPON_COUNT && kind!=WEAPON_NONE) || group>1 ||
            (kind<WEAPON_COUNT && ammo>cg->rules.combat.weapons[kind].ammo) ||
            (i>=p->mounts && kind!=WEAPON_NONE)) return false;
        p->weapons[i]=(game_mount){.kind=(uint8_t)kind,.group=(uint8_t)group,.ammo=(uint16_t)ammo};
        if(i<p->mounts && kind<=WEAPON_BEAM) lasers++;
    }
    if(modes) for(int i=0;i<GAME_MOUNTS;i++) {
        if(*b>MOUNT_TURRET) return false;
        p->weapons[i].mode=*b++;
    }
    p->heat_level=cg_clamp(fmaxf(p->heat*.01f,p->cabin_heat),0,1.5f);
    return lasers==p->lasers;
}
static bool floats_ok(const game_player *p) {
    const float a[] = {p->position.x, p->position.y,  p->position.z, p->velocity.x, p->velocity.y,
                       p->velocity.z, p->attitude.x,  p->attitude.y, p->attitude.z, p->attitude.w,
                       p->throttle,   p->shield_fore, p->shield_aft, p->hull,       p->energy,
                       p->heat,       p->fuel};
    for (unsigned i = 0; i < sizeof(a) / sizeof(a[0]); i++)
        if (!isfinite(a[i]) || fabsf(a[i]) > 1e7f)
            return false;
    float q = p->attitude.x * p->attitude.x + p->attitude.y * p->attitude.y +
              p->attitude.z * p->attitude.z + p->attitude.w * p->attitude.w;
    return q > .9f && q < 1.1f && p->throttle >= -cg->rules.reverse && p->throttle <= 1 && p->fuel >= 0 &&
           p->fuel <= p->fuel_max && p->hull >= 0 && p->hull <= game_ship((game_class)p->ship_class)->hull &&
           p->shield_fore >= 0 && p->shield_fore <= fmaxf(150,game_ship((game_class)p->ship_class)->shield*1.5f) && p->shield_aft >= 0 &&
           p->shield_aft <= fmaxf(150,game_ship((game_class)p->ship_class)->shield*1.5f) && p->energy >= 0 && p->energy <= 100 && p->heat >= 0 &&
           p->heat <= 150;
}
static void write_player(unsigned char **b, const game_player *p) {
    put32(b, p->credits);
    put32(b, p->kills);
    put32(b, p->equipment);
    putf(b, p->position.x);
    putf(b, p->position.y);
    putf(b, p->position.z);
    putf(b, p->velocity.x);
    putf(b, p->velocity.y);
    putf(b, p->velocity.z);
    putf(b, p->attitude.x);
    putf(b, p->attitude.y);
    putf(b, p->attitude.z);
    putf(b, p->attitude.w);
    putf(b, p->throttle);
    putf(b, p->shield_fore);
    putf(b, p->shield_aft);
    putf(b, p->hull);
    putf(b, p->energy);
    putf(b, p->heat);
    putf(b, p->fuel);
    for (int i = 0; i < GAME_GOODS; i++)
        put32(b, p->cargo[i]);
    put32(b, p->missiles);
    put32(b, p->legal);
    put32(b, p->rank);
    put32(b, (p->docked ? 1u : 0) | (p->motion_steer ? 2u : 0) | (p->dead ? 4u : 0));
}
static bool read_player(const unsigned char **b, game_player *p) {
    memset(p, 0, sizeof(*p));
    cg_ship_fit(p,SH_MAKO); /* saves without the shipyard trailer */
    p->credits = get32(b);
    p->kills = get32(b);
    p->equipment = get32(b);
    p->position.x = getf(b);
    p->position.y = getf(b);
    p->position.z = getf(b);
    p->velocity.x = getf(b);
    p->velocity.y = getf(b);
    p->velocity.z = getf(b);
    p->attitude.x = getf(b);
    p->attitude.y = getf(b);
    p->attitude.z = getf(b);
    p->attitude.w = getf(b);
    p->throttle = getf(b);
    p->shield_fore = getf(b);
    p->shield_aft = getf(b);
    p->hull = getf(b);
    p->energy = getf(b);
    p->heat = getf(b);
    p->fuel = getf(b);
    int cargo = 0;
    for (int i = 0; i < GAME_GOODS; i++) {
        uint32_t n = get32(b);
        if (n > 1000)
            return false;
        p->cargo[i] = (uint16_t)n;
        cargo += (int)n;
    }
    uint32_t missiles = get32(b), legal = get32(b), rank = get32(b), flags = get32(b);
    if (missiles > CG_MISSILES || legal > 255 || rank > 8 || flags > 7 || p->credits > 1000000000u ||
        p->equipment > 255 || cargo>1015)
        return false;
    p->missiles = missiles;
    p->legal = legal;
    p->rank = rank;
    p->docked = (flags & 1) != 0;
    p->motion_steer = (flags & 2) != 0;
    p->dead = (flags & 4) != 0;
    p->lasers=(p->equipment&(1u<<EQ_LASER))?1:0;
    return true; /* hull-dependent validation follows the optional trailer */
}
static int read_slot(int i) {
    int fd = lap_file_open(i ? "commander1.dat" : "commander0.dat", "r");
    if (fd < 0)
        return 0;
    int n = lap_file_read(fd, cg->save_buffer, sizeof(cg->save_buffer));
    lap_file_close(fd);
    return n;
}
static bool decode(int n, bool apply, uint32_t *sequence) {
    const unsigned char *b = cg->save_buffer;
    bool lore = !memcmp(b, "CRR3", 4);
    bool part2 = lore || !memcmp(b, "CRR2", 4);
    if (n != 8192 || (!part2 && memcmp(b, "CRR1", 4)))
        return false;
    b += 4;
    uint32_t length = get32(&b), seq = get32(&b), sum = get32(&b);
    if (length < 188 || length > sizeof(cg->save_buffer) || checksum(b, (int)length - 16) != sum)
        return false;
    uint32_t seed = get32(&b), system = get32(&b), count = get32(&b);
    uint32_t prefix=188 + count * (part2 ? 55 : 31) + (part2 ? 358 : 0) + (lore ? 6*65 : 0);
    bool yard=lore && length==prefix+16;
    if (system >= GAME_SYSTEMS || count > GAME_REGISTRY || (length != prefix && !yard))
        return false;
    game_player player;
    if (!read_player(&b, &player))
        return false;
    const unsigned char *entries = b;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = get32(&b), visits = get32(&b);
        (void)visits;
        if (!id || b[8] || b[9] >= 13 || b[10] >= 13)
            return false;
        for (int j = 0; j < 8 && b[j]; j++)
            if (b[j] < 32 || b[j] > 126)
                return false;
        b += 11;
        get32(&b);
        uint32_t speed = get32(&b), live = get32(&b);
        if (speed > 32767 || live > 1)
            return false;
        if(part2) {
            if(b[19]) return false;
            for(int j=0;j<19 && b[j];j++) if(b[j]<32 || b[j]>126) return false;
            b+=24;
        }
    }
    game_mission missions[6];memset(missions,0,sizeof(missions));
    uint32_t day=0,score=player.kills > cg->systems.rank[player.rank] ? player.kills : cg->systems.rank[player.rank];float cabin=0,flight=0;
    if(part2) {
        day=get32(&b);score=get32(&b);cabin=getf(&b);flight=getf(&b);
        if(day>=0xfffffffeu || !isfinite(cabin) || cabin<0 || cabin>1 ||
            !isfinite(flight) || flight<0 || flight>10000) return false;
        int accepted=0;
        for(int i=0;i<6;i++) {
            game_mission *m=&missions[i];
            uint32_t kind=get32(&b),dst=get32(&b);
            m->reward=get32(&b);m->deadline=get32(&b);m->target_id=get32(&b);
            uint32_t amount=get32(&b),good=get32(&b),flags=get32(&b);
            if(kind>(lore?8:MISSION_RESCUE) || dst>=GAME_SYSTEMS || m->reward>1000000 ||
                amount>35 || good>=GAME_GOODS || flags>15 || b[24]) return false;
            for(int j=0;j<24 && b[j];j++) if(b[j]<32 || b[j]>126) return false;
            m->kind=(game_mission_kind)kind;m->destination=(int)dst;m->amount=(uint16_t)amount;m->good=(uint8_t)good;
            m->accepted=flags&1;m->done=(flags&2)!=0;m->failed=(flags&4)!=0;m->ready=(flags&8)!=0;
            if((m->done && m->failed) || ((m->done || m->failed || m->ready) && !m->accepted)) return false;
            if(m->accepted && !m->done && !m->failed) accepted++;
            memcpy(m->title,b,25);b+=25;
        }
        if(accepted>3) return false;
    }
    unsigned seats=0;
    if(lore) {
        for(int i=0;i<6;i++) {
            game_mission *m=&missions[i];
            if(b[60]) return false;
            for(int j=0;j<60 && b[j];j++) if(b[j]<32 || b[j]>126) return false;
            memcpy(m->brief,b,61);b+=61;uint32_t progress=get32(&b);
            if(progress>m->amount || (m->kind!=MISSION_SALVAGE && progress) ||
                (m->kind==MISSION_SURVEY && m->target_id>5) ||
                (m->kind==MISSION_SMUGGLING && m->good!=5 && m->good!=9)) return false;
            m->progress=(uint16_t)progress;
            if(m->kind==MISSION_PASSENGER && m->accepted && !m->done && !m->failed) seats+=m->amount;
        }
    } else if(part2) {
        for(int i=0;i<6;i++) snprintf(missions[i].brief,61,"%.24s; report to %.15s.",missions[i].title,"destination");
    }
    uint32_t station=system;
    if(yard) {
        if(memcmp(b,"YRD1",4)) return false;
        b+=4;uint32_t hull=get32(&b),lasers=get32(&b);station=get32(&b);
        if(hull>=11 || lasers>GAME_MOUNTS || station>=GAME_SYSTEMS) return false;
        cg_ship_fit(&player,(game_class)hull);player.lasers=(uint8_t)lasers;
    }
    if(seats>(unsigned)player.seats || !cg_ship_valid(&player) || !floats_ok(&player)) return false;
    if ((uint32_t)(b - cg->save_buffer) != length)
        return false;
    if(!combat_read(&player,seq,sum)) return false;
    if (sequence)
        *sequence = seq;
    if (!apply)
        return true;
    cg->v.galaxy_seed = seed;
    cg->rng = seed;
    game_universe(seed, cg->v.systems);
    cg->v.market_day=day;
    memset(cg->v.missions,0,sizeof(cg->v.missions));
    cg_arrive((int)system);
    cg->v.player = player;
    cg_combat_reset();
    cg->yard_station=(int)station;
    cg->v.player.combat_score=score;cg->v.player.cabin_heat=cabin;cg->flight_seconds=flight;cg->last_tick_ms=lap_millis();
    if(part2) memcpy(cg->v.missions,missions,sizeof(missions));
    cg_news();
    cg->v.registry_count = (int)count;
    cg->save_seq = seq;
    b = entries;
    for (uint32_t i = 0; i < count; i++) {
        game_registry_entry *e = &cg->v.registry[i];
        memset(e, 0, sizeof(*e));
        e->id = get32(&b);
        e->visits = get32(&b);
        memcpy(e->callsign, b, 9);
        b += 9;
        e->ship_class = *b++;
        e->faction = *b++;
        e->altitude_ft = (int32_t)get32(&b);
        e->speed_kt = (int16_t)get32(&b);
        e->live = get32(&b) != 0;
        if(part2) {memcpy(e->name,b,20);b+=20;e->refuel_day=get32(&b);}
        else memcpy(e->name,e->callsign,9);
    }
    return true;
}
bool game_save(void) {
    if (!cg || cg->v.player.dead)
        return false;
    cg_combat_player_ensure(&cg->v.player);
    memset(cg->save_buffer, 0, sizeof(cg->save_buffer));
    unsigned char *b = cg->save_buffer + 16;
    put32(&b, cg->v.galaxy_seed);
    put32(&b, cg->v.system);
    put32(&b, cg->v.registry_count);
    write_player(&b, &cg->v.player);
    for (int i = 0; i < cg->v.registry_count; i++) {
        const game_registry_entry *e = &cg->v.registry[i];
        put32(&b, e->id);
        put32(&b, e->visits);
        memcpy(b, e->callsign, 9);
        b += 9;
        *b++ = e->ship_class;
        *b++ = e->faction;
        put32(&b, (uint32_t)e->altitude_ft);
        put32(&b, (uint32_t)e->speed_kt);
        put32(&b, e->live);
        memcpy(b,e->name,20);b+=20;put32(&b,e->refuel_day);
    }
    put32(&b,cg->v.market_day);put32(&b,cg->v.player.combat_score);
    putf(&b,cg->v.player.cabin_heat);putf(&b,cg->flight_seconds);
    for(int i=0;i<6;i++) {
        const game_mission *m=&cg->v.missions[i];
        put32(&b,m->kind);put32(&b,m->destination);put32(&b,m->reward);put32(&b,m->deadline);
        put32(&b,m->target_id);put32(&b,m->amount);put32(&b,m->good);
        put32(&b,(m->accepted?1:0)|(m->done?2:0)|(m->failed?4:0)|(m->ready?8:0));
        memcpy(b,m->title,25);b+=25;
    }
    for(int i=0;i<6;i++) {
        memcpy(b,cg->v.missions[i].brief,61);b+=61;put32(&b,cg->v.missions[i].progress);
    }
    memcpy(b,"YRD1",4);b+=4;
    put32(&b,game_player_class());put32(&b,cg->v.player.lasers);
    put32(&b,cg->v.player.docked?cg->v.system:cg->yard_station);
    uint32_t length = (uint32_t)(b - cg->save_buffer),
             sum = checksum(cg->save_buffer + 16, (int)length - 16);
    b = cg->save_buffer;
    memcpy(b, "CRR3", 4);
    b += 4;
    put32(&b, length);
    put32(&b, ++cg->save_seq);
    put32(&b, sum);
    combat_write(cg->save_seq,sum);
    int fd = lap_file_open(cg->save_seq & 1 ? "commander1.dat" : "commander0.dat", "w");
    cg->v.save_ok = fd >= 0 && lap_file_write(fd, cg->save_buffer, sizeof(cg->save_buffer)) ==
                                   (int)sizeof(cg->save_buffer);
    if (fd >= 0)
        lap_file_close(fd);
    if(cg->v.save_ok && cg->v.player.docked) cg->yard_station=cg->v.system;
    return cg->v.save_ok;
}
bool game_load(void) {
    if (!cg)
        return false;
    uint32_t s0 = 0, s1 = 0;
    bool ok0 = decode(read_slot(0), false, &s0), ok1 = decode(read_slot(1), false, &s1);
    if (!ok0 && !ok1)
        return false;
    int best = ok1 && (!ok0 || (int32_t)(s1 - s0) > 0);
    bool ok = decode(read_slot(best), true, NULL);
    cg->v.save_ok = ok;
    return ok;
}
