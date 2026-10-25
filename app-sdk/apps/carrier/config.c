/* Small bounded schema reader for numeric rules; app file cache, never SD IO.
 */
#include "internal.h"
/* Bump with every retune of the defaults: an older file on the card is set aside and replaced. */
#define RULES_VERSION 6
static const char defaults[] =
#include "rules_default.inc"
    ;
/* No libc ctype imports in the native app. Only JSON whitespace is accepted. */
static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
        p++;
    return p;
}
static bool token(const char **p, char c) {
    *p = ws(*p);
    if (**p != c)
        return false;
    ++*p;
    return true;
}
static bool number(const char **p, float *out) {
    const char *start=ws(*p), *q=start;
    if(*q=='-') q++;
    if(*q=='0') q++;
    else {
        if(*q<'1' || *q>'9') return false;
        while(*q>='0' && *q<='9') q++;
    }
    if(*q=='.') {
        q++;if(*q<'0' || *q>'9') return false;
        while(*q>='0' && *q<='9') q++;
    }
    if(*q=='e' || *q=='E') {
        q++;if(*q=='+' || *q=='-') q++;
        if(*q<'0' || *q>'9') return false;
        while(*q>='0' && *q<='9') q++;
    }
    char *end;
    float v=strtof(start,&end);
    if(end!=q || !isfinite(v)) return false;
    *p=q;*out=v;return true;
}
static bool encounter_string(const char **p, char out[41]);
/* shipyard v1 is an additive rules-v1 section; each row follows the hull ID. */
static bool parse_yard(const char **p, cg_yard_rules *r) {
    static const char *const keys[]={"version","trade_percent","insurance_percent","hulls"};
    unsigned seen=0;
    if(!token(p,'{')) return false;
    do {
        if(!token(p,'"')) return false;
        int k=-1;
        for(int i=0;i<4;i++) {size_t n=strlen(keys[i]);
            if(!strncmp(*p,keys[i],n) && (*p)[n]=='"') {k=i;*p+=n+1;break;}}
        if(k<0 || (seen&(1u<<k)) || !token(p,':')) return false;
        seen|=1u<<k;
        float v;
        if(k!=3) {
            if(!number(p,&v) || v!=floorf(v) || v<1 || v>100 || (k==0 && v!=1)) return false;
            if(k==1) r->trade_percent=(uint8_t)v;
            if(k==2) r->insurance_percent=(uint8_t)v;
        } else {
            if(!token(p,'[')) return false;
            for(int i=0;i<11;i++) {
                cg_yard_hull *h=&r->hulls[i];
                if((i && !token(p,',')) || !token(p,'[')) return false;
                for(int j=0;j<8;j++) {
                    if((j && !token(p,',')) || !number(p,&v) || !isfinite(v)) return false;
                    if(j!=1 && v!=floorf(v)) return false;
                    switch(j) {
                    case 0:if(v<1 || v>10000000) return false;h->price=(uint32_t)v;break;
                    case 1:if(v<1 || v>32) return false;h->fuel_max=v;break;
                    case 2:if(v<0 || v>CG_MISSILES) return false;h->pylons=(uint8_t)v;break;
                    case 3:if(v<1 || v>(i==0?1:GAME_MOUNTS)) return false;h->mounts=(uint8_t)v;break;
                    case 4:if(v<0 || v>100) return false;h->seats=(uint8_t)v;break;
                    case 5:if(v<1 || v>12) return false;h->min_tech=(uint8_t)v;break;
                    case 6:if(v<1 || v>255) return false;h->government_mask=(uint8_t)v;break;
                    case 7:if(v<1 || v>100) return false;h->stock_percent=(uint8_t)v;break;
                    }
                }
                if(!token(p,',') || !encounter_string(p,h->pitch) || !token(p,']')) return false;
            }
            if(!token(p,']')) return false;
        }
        *p=ws(*p);
        if(**p=='}') {++*p;break;}
        if(!token(p,',')) return false;
    } while(**p);
    return seen==15;
}
/* Strict fixed-shape combat tuning. No generic JSON tree or heap allocation. */
static bool combat_row(const char **p, float *out, int n, float low, float high, bool integer) {
    if(!token(p,'[')) return false;
    for(int i=0;i<n;i++) {
        if((i && !token(p,',')) || !number(p,&out[i]) || out[i]<low || out[i]>high ||
            (integer && out[i]!=floorf(out[i]))) return false;
    }
    return token(p,']');
}
static bool parse_combat(const char **p, cg_combat_rules *r) {
    static const char *const keys[]={"damage","weapons","distributor","defence","supplies","supply_price","npc_weapons"};
    unsigned seen=0;
    if(!token(p,'{')) return false;
    do {
        if(!token(p,'"')) return false;
        int k=-1;
        for(int i=0;i<7;i++) {size_t n=strlen(keys[i]);
            if(!strncmp(*p,keys[i],n) && (*p)[n]=='"') {k=i;*p+=n+1;break;}}
        if(k<0 || (seen&(1u<<k)) || !token(p,':')) return false;
        seen|=1u<<k;
        if(k<=1) {
            if(!token(p,'[')) return false;
            for(int i=0;i<(k==0?3:WEAPON_COUNT);i++) {
                if(i && !token(p,',')) return false;
                if(k==0) {if(!combat_row(p,r->damage[i],2,.01f,4,false)) return false;}
                else {
                    float a[10];
                    if(!combat_row(p,a,10,0,1000000,false) || a[0]<.01f || a[0]>1000 ||
                        a[1]<.04f || a[1]>10 || a[2]>100 || a[3]>100 || a[4]>2000 ||
                        a[5]>30 || a[6]>10000 || a[6]!=floorf(a[6]) || a[7]!=floorf(a[7]) ||
                        a[8]<1 || a[8]>12 || a[8]!=floorf(a[8]) || a[9]>1 || a[9]!=floorf(a[9]) ||
                        ((i==WEAPON_RATTLE || i==WEAPON_SLUG || i>=WEAPON_SEEKER) && (a[4]<=0 || a[5]<=0)) ||
                        (i==WEAPON_RATTLE && a[6]<1)) return false;
                    r->weapons[i]=(cg_weapon_rule){a[0],a[1],a[2],a[3],a[4],a[5],
                        (uint16_t)a[6],(uint32_t)a[7],(uint8_t)a[8],(uint8_t)a[9]};
                }
            }
            if(!token(p,']')) return false;
        } else if(k==2) {
            if(!combat_row(p,r->distributor,CD_COUNT,0,10,false)) return false;
            /* Keep resistance and speed/turn factors positive at every setting. */
            if(r->distributor[CD_RESIST]>.4f || r->distributor[CD_SPEED]>.4f ||
                r->distributor[CD_TURN]>.4f) return false;
        } else if(k==3) {
            if(!combat_row(p,r->defence,CF_COUNT,.001f,2000,false) ||
                r->defence[CF_CELL]>1 || r->defence[CF_ACCURACY]>1 ||
                r->defence[CF_EARLY_ACCURACY]>1 || r->defence[CF_EARLY_REGEN]<1 ||
                r->defence[CF_EARLY_REGEN]>4) return false;
        } else if(k==6) {
            if(!token(p,'[')) return false;
            for(int i=0;i<3;i++) {
                float a[GAME_MOUNTS];
                if((i && !token(p,',')) || !combat_row(p,a,GAME_MOUNTS,0,255,true)) return false;
                for(int j=0;j<GAME_MOUNTS;j++) {
                    if(a[j]>=WEAPON_COUNT && (a[j]!=WEAPON_NONE || j==0)) return false;
                    r->npc_weapons[i][j]=(uint8_t)a[j];
                }
            }
            if(!token(p,']')) return false;
        } else {
            float a[3];
            if(!combat_row(p,a,3,k==4?0:1,k==4?255:10000,true)) return false;
            for(int i=0;i<3;i++) (k==4?r->supplies:r->supply_price)[i]=(uint16_t)a[i];
        }
        *p=ws(*p);
        if(**p=='}') {++*p;break;}
        if(!token(p,',')) return false;
    } while(**p);
    return seen==127;
}
static bool parse(const char *p, cg_rules *r) {
    static const char *const keys[] = {
        "version",     "ships",   "commodity_base", "economy_slope", "equipment_cost", "laser_dps",
        "laser_range", "cooling", "fuel_price",     "repair_price",  "npc_damage", "shipyard", "combat", "reverse", "aim_assist_deg", "weapons_cool_s", "gimbal_deg", "turret_deg", "turret_rate_dps", "mount_damage", "mount_price"};
    unsigned seen = 0;
    if (!token(&p, '{'))
        return false;
    do {
        if (!token(&p, '"'))
            return false;
        int k = -1;
        for (int i = 0; i < 21; i++) {
            size_t n = strlen(keys[i]);
            if (!strncmp(p, keys[i], n) && p[n] == '"') {
                k = i;
                p += n + 1;
                break;
            }
        }
        if (k < 0 || (seen & (1u << k)) || !token(&p, ':'))
            return false;
        seen |= 1u << k;
        float v;
        if(k>=19) {
            if(!combat_row(&p,k==19?r->mount_damage:r->mount_price,3,.01f,k==19?4:10,false)) return false;
        } else if(k>=15) {
            float *out=k==15?&r->weapons_cool_s:k==16?&r->gimbal_deg:k==17?&r->turret_deg:&r->turret_rate_dps;
            if(!number(&p,out) || *out<=0 || *out>(k==15?60:k==18?1000:90)) return false;
        } else if (k == 14) {
            if(!combat_row(&p,r->aim_assist_deg,3,0,90,false) || r->aim_assist_deg[0]!=0 ||
                r->aim_assist_deg[1]>r->aim_assist_deg[2]) return false;
        } else if (k == 13) {
            if(!number(&p,&r->reverse) || r->reverse<0 || r->reverse>1) return false;
        } else if (k == 12) {
            if(!parse_combat(&p,&r->combat)) return false;
        } else if (k == 11) {
            if(!parse_yard(&p,&r->yard)) return false;
        } else if (k == 1) {
            if (!token(&p, '['))
                return false;
            for (int i = 0; i < 13; i++) {
                if (i && !token(&p, ','))
                    return false;
                if (!token(&p, '['))
                    return false;
                game_ship_stats *s = &r->ships[i];
                for (int j = 0; j < 7; j++) {
                    if (j && !token(&p, ','))
                        return false;
                    if (!number(&p, &v) || v < 0 || v > 1000)
                        return false;
                    switch (j) {
                    case 0:
                        s->speed = v;
                        break;
                    case 1:
                        s->turn = v;
                        break;
                    case 2:
                        s->hull = v;
                        break;
                    case 3:
                        s->shield = v;
                        break;
                    case 4:
                        s->weapon = v;
                        break;
                    case 5:
                        s->radius = v;
                        break;
                    case 6:
                        if (v != floorf(v))
                            return false;
                        s->cargo = (int)v;
                        break;
                    }
                }
                if (s->hull < 1 || s->radius < 1 || s->speed < 1 || s->turn < 1 || !token(&p, ']'))
                    return false;
            }
            if (!token(&p, ']'))
                return false;
        } else if (k >= 2 && k <= 4) {
            int *a = k == 2   ? r->base
                     : k == 3 ? r->slope
                              : r->equipment,
                n = k == 4 ? EQ_COUNT : GAME_GOODS;
            if (!token(&p, '['))
                return false;
            for (int j = 0; j < n; j++) {
                if (j && !token(&p, ','))
                    return false;
                if (!number(&p, &v) || v != floorf(v) || v < (k == 3 ? -100 : 0) || v > 100000)
                    return false;
                a[j] = (int)v;
            }
            if (!token(&p, ']'))
                return false;
        } else {
            if (!number(&p, &v))
                return false;
            if (k == 0) {
                if (v != RULES_VERSION)
                    return false;
                r->version = (int)v;
            } else {
                if (v <= 0 || v > 10000)
                    return false;
                switch (k) {
                case 5:
                    r->laser_dps = v;
                    break;
                case 6:
                    r->laser_range = v;
                    break;
                case 7:
                    r->cooling = v;
                    break;
                case 8:
                    r->fuel_price = v;
                    break;
                case 9:
                    r->repair_price = v;
                    break;
                case 10:
                    r->npc_damage = v;
                    break;
                }
            }
        }
        p = ws(p);
        if (*p == '}') {
            p++;
            break;
        }
        if (!token(&p, ','))
            return false;
    } while (*p);
    return seen == 2097151 && !*ws(p);
}
bool cg_config(void) {
    /* Shark names, size and temper matched to each hull's job. */
    static const char *const names[] = {"Dogfish", "Houndshark", "Mako Mk III", "Thresher", "Whale Shark",
                                        "Basker",  "Blacktip",   "Tiger",       "Hammerhead", "Goblin",
                                        "Cookiecutter", "Lantern", "Frill"};
    char *buf = (char *)cg->save_buffer;
    bool valid = true;
    int fd = lap_file_open("rules.json", "r");
    if (fd >= 0) {
        int n = lap_file_read(fd, buf, sizeof(cg->save_buffer) - 1);
        lap_file_close(fd);
        if (n <= 0 || n == (int)sizeof(cg->save_buffer) - 1)
            valid = false;
        else {
            buf[n] = 0;
            valid = parse(buf, &cg->rules) && cg->rules.version == RULES_VERSION;
        }
    } else {
        valid = parse(defaults, &cg->rules);
        fd = lap_file_open("rules.json", "w");
        if (fd >= 0) {
            lap_file_write(fd, defaults, sizeof(defaults) - 1);
            lap_file_close(fd);
        }
    }
    if (!valid) {
        /* an older or hand-broken file: keep it aside and write the current rules, so an
           update never leaves the game on a file it no longer reads */
        parse(defaults, &cg->rules);
        int old = lap_file_open("rules.old.json", "w");
        if (old >= 0 && fd >= 0) {
            int in = lap_file_open("rules.json", "r");
            if (in >= 0) {
                int n = lap_file_read(in, buf, sizeof(cg->save_buffer) - 1);
                if (n > 0) lap_file_write(old, buf, (size_t)n);
                lap_file_close(in);
            }
        }
        if (old >= 0) lap_file_close(old);
        int out = lap_file_open("rules.json", "w");
        if (out >= 0) {
            lap_file_write(out, defaults, sizeof(defaults) - 1);
            lap_file_close(out);
        }
    }
    for (int i = 0; i < 13; i++)
        cg->rules.ships[i].name = names[i];
    if(!cg->v.player.fuel_max) {
        cg_ship_fit(&cg->v.player,SH_MAKO);
        cg->v.player.lasers=1;
    } else cg_ship_fit(&cg->v.player,game_player_class());
    return valid;
}

/* part 2: same bounded numeric JSON reader, independent versioned rules. */
static bool parse_systems(const char *p, cg_system_rules *r) {
    static const char *const keys[] = {"version", "day_seconds", "scan_seconds", "heat_gain",
        "heat_cool", "scoop_rate", "heat_damage", "event_percent", "mission_days",
        "mission_reward", "fine", "trade_percent", "rank"};
    unsigned seen = 0;
    if (!token(&p, '{')) return false;
    do {
        if (!token(&p, '"')) return false;
        int k = -1;
        for (int i=0;i<13;i++) {
            size_t n=strlen(keys[i]);
            if (!strncmp(p,keys[i],n) && p[n]=='"') { k=i; p+=n+1; break; }
        }
        if (k<0 || (seen & (1u<<k)) || !token(&p, ':')) return false;
        seen |= 1u<<k;
        float v;
        if (k==12) {
            if (!token(&p,'[')) return false;
            for (int i=0;i<9;i++) {
                if ((i && !token(&p,',')) || !number(&p,&v) || v!=floorf(v) || v<0 || v>1000000 ||
                    (!i && v!=0) || (i && v<=r->rank[i-1])) return false;
                r->rank[i]=(uint32_t)v;
            }
            if (!token(&p,']')) return false;
        } else {
            if (!number(&p,&v) || v<=0 || v>10000 || (k==0 && v!=1) ||
                ((k==1 || k==2 || k>=7) && v!=floorf(v)) ||
                (k==2 && v<60) || (k==7 && (v<100 || v>300)) ||
                (k==8 && v>365) || (k==11 && v>50)) return false;
            switch(k) {
            case 1:r->day_seconds=v;break; case 2:r->scan_seconds=v;break;
            case 3:r->heat_gain=v;break; case 4:r->heat_cool=v;break;
            case 5:r->scoop_rate=v;break; case 6:r->heat_damage=v;break;
            case 7:r->event_percent=(int)v;break; case 8:r->mission_days=(int)v;break;
            case 9:r->mission_reward=(int)v;break; case 10:r->fine=(int)v;break;
            case 11:r->trade_percent=(int)v;break;
            }
        }
        p=ws(p);
        if (*p=='}') {p++;break;}
        if (!token(&p,',')) return false;
    } while (*p);
    return seen==8191 && !*ws(p);
}
bool cg_system_config(void) {
    static const char defaults2[] =
#include "systems_default.inc"
    ;
    char *buf=(char *)cg->save_buffer;
    int fd=lap_file_open("systems.json","r");
    bool valid;
    if (fd<0) {
        valid=parse_systems(defaults2,&cg->systems);
        fd=lap_file_open("systems.json","w");
        if (fd>=0) {lap_file_write(fd,defaults2,sizeof(defaults2)-1);lap_file_close(fd);}
    } else {
        int n=lap_file_read(fd,buf,sizeof(cg->save_buffer)-1);lap_file_close(fd);
        valid=n>0 && n<(int)sizeof(cg->save_buffer)-1;
        if(valid) {buf[n]=0;valid=parse_systems(buf,&cg->systems);}
    }
    if(!valid) parse_systems(defaults2,&cg->systems);
    return valid;
}

/* Encounters v1: strict keys/array bounds, printable 40-character radio lines.
 * Parsed once at entry through the existing PSRAM file cache. */
static bool encounter_string(const char **p, char out[41]) {
    if (!token(p, '"')) return false;
    int n=0;
    while (**p && **p!='"') {
        unsigned char c=(unsigned char)*(*p)++;
        if (n==40 || c<32 || c>126 || c=='\\') return false;
        out[n++]=(char)c;
    }
    out[n]=0;
    return n>0 && token(p, '"');
}
static bool parse_encounters(const char *p, cg_encounter_rules *r) {
    static const char *const keys[]={"version","cadence","deep_scale","safe_radius",
        "chatter_seconds","weights","messages","chatter"};
    unsigned seen=0;
    if (!token(&p,'{')) return false;
    do {
        if (!token(&p,'"')) return false;
        int k=-1;
        for (int i=0;i<8;i++) {
            size_t n=strlen(keys[i]);
            if (!strncmp(p,keys[i],n) && p[n]=='"') {k=i;p+=n+1;break;}
        }
        if (k<0 || (seen&(1u<<k)) || !token(&p,':')) return false;
        seen|=1u<<k;
        if (k==0 || k==2 || k==3) {
            float v;
            if (!number(&p,&v)) return false;
            if (k==0 && v!=1) return false;
            if (k==2) {if(v<.25f || v>1) return false;r->deep_scale=v;}
            if (k==3) {if(v<200 || v>2000) return false;r->safe_radius=v;}
        } else {
            int n=k==1?6:k==4?2:k==7?CG_CHATTER_LINES:CG_ENCOUNTER_KINDS;
            if (!token(&p,'[')) return false;
            for (int i=0;i<n;i++) {
                if (i && !token(&p,',')) return false;
                if (k>=6) {
                    if (!encounter_string(&p,k==6?r->messages[i]:r->chatter[i])) return false;
                } else {
                    float v;
                    if (!number(&p,&v) || v!=floorf(v) ||
                        v<(k==1?10:k==4?5:0) || v>(k==1?600:k==4?300:100)) return false;
                    if (k==1) r->cadence[i]=v;
                    if (k==4) r->chatter_seconds[i]=v;
                    if (k==5) r->weights[i]=(int)v;
                }
            }
            if (!token(&p,']')) return false;
        }
        p=ws(p);
        if (*p=='}') {p++;break;}
        if (!token(&p,',')) return false;
    } while (*p);
    int sum=0;for(int i=0;i<CG_ENCOUNTER_KINDS;i++) sum+=r->weights[i];
    return seen==255 && !*ws(p) && sum>0 && r->cadence[0]<=r->cadence[1] &&
        r->cadence[2]<=r->cadence[3] && r->cadence[4]<=r->cadence[5] &&
        r->chatter_seconds[0]<=r->chatter_seconds[1];
}
bool cg_encounter_config(void) {
    static const char defaults3[] =
#include "../../../data/carrier/encounters_default.inc"
    ;
    char *buf=(char *)cg->save_buffer;
    int fd=lap_file_open("encounters.json","r");
    bool valid;
    if (fd<0) {
        valid=parse_encounters(defaults3,&cg->encounters);
        fd=lap_file_open("encounters.json","w");
        if(fd>=0) {lap_file_write(fd,defaults3,sizeof(defaults3)-1);lap_file_close(fd);}
    } else {
        int n=lap_file_read(fd,buf,sizeof(cg->save_buffer)-1);lap_file_close(fd);
        valid=n>0 && n<(int)sizeof(cg->save_buffer)-1;
        if(valid) {buf[n]=0;valid=parse_encounters(buf,&cg->encounters);}
    }
    if(!valid) parse_encounters(defaults3,&cg->encounters);
    return valid;
}
