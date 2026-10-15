#include "ls_detector_ui.h"
#include "ls_braille.h"
#include "ls_detector.h"
#include "ls_detector_air.h"
#include "ls_sweep_app.h"
#include "ls_options.h"
#include "ls_radio_panel.h"
#include "ls_glyph.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#define INK(c) TUI_ATTR((c),TUI_BLACK)
static EXT_RAM_BSS_ATTR ls_detector_event_t view[LS_DETECTOR_HISTORY],selected;
static unsigned rule_pick;
static int pick,count;
static bool detail;
/* AIRCRAFT view: the shared ADS-B table, with the rules each aircraft meets. */
static bool air_view;
static int air_pick;
static size_t air_n;
static EXT_RAM_BSS_ATTR ls_detector_air_row_t air_rows[LS_DETECTOR_AIR_SLOTS],air_selected;
static tui_rect list_area, keys[7];
static int first, visible, row_height;
static bool test_waiting;
static uint32_t test_sequence;
static EXT_RAM_BSS_ATTR ls_detector_status_t cached;
static int64_t refresh_at;
static EXT_RAM_BSS_ATTR uint16_t eye_ring[7],eye_cross[7];
static bool eye_ready;
static void options_art(tui_surface *,tui_rect);

static const uint8_t colors[]={TUI_YELLOW,TUI_MAGENTA,TUI_CYAN,TUI_GREEN,TUI_RED};
static uint8_t event_hue(const ls_detector_event_t *e) {
    return e->source==DET_ADSB?(ls_detector_air_alarm(e->squawk,e->emergency)?TUI_RED:TUI_BLUE|TUI_BRIGHT):e->category<5?colors[e->category]:TUI_CYAN;
}
static const char *event_label(const ls_detector_event_t *e) {return e->source==DET_ADSB?"AIRCRAFT":ls_sweep_category(e->category);}
static EXT_RAM_BSS_ATTR ls_detector_rule_t rule_view;
static unsigned rule_view_index;
static bool rule_view_valid;
static bool ui_rule(ls_detector_rule_t *r) {
    if(rule_view_index!=rule_pick){rule_view_index=rule_pick;memset(&rule_view,0,sizeof(rule_view));}
    ls_detector_rule_snapshot(rule_pick,&rule_view);
    rule_view_valid=rule_view.id[0]!=0;
    if(rule_view_valid)*r=rule_view;
    return rule_view_valid;
}
static int get(const ls_opt_t *o) {
    ls_detector_status_t s=cached;size_t n;ls_detector_snapshot(&s,NULL,0,&n);
    if(o->arg==10)return !!(s.sources&DET_LORA);
    if(o->arg==11)return !!(s.sources&DET_ADSB);
    if(o->arg<3)return !!(s.sources&(1<<o->arg));
    if(o->arg<6)return !!(s.sinks&(1<<(o->arg-3)));
    ls_detector_rule_t r;if(!ui_rule(&r))return 0;
    return o->arg==6?r.enabled:!!(r.sinks&(1<<(o->arg-7)));
}
static void set(const ls_opt_t *o,int v) {
    ls_detector_status_t s;ls_detector_status(&s);
    if(o->arg==10) {if(v)s.sources|=DET_LORA;else s.sources&=~DET_LORA;ls_detector_policy(s.sources,s.sinks);return;}
    if(o->arg==11) {if(v)s.sources|=DET_ADSB;else s.sources&=~DET_ADSB;ls_detector_policy(s.sources,s.sinks);return;}
    if(o->arg<6) {
        uint8_t *bits=o->arg<3?&s.sources:&s.sinks;unsigned b=1u<<(o->arg%3);
        if(v)*bits|=b;else *bits&=~b;ls_detector_policy(s.sources,s.sinks);
    } else {
        ls_detector_rule_t r;if(!ls_detector_rule(rule_pick,&r))return;
        if(o->arg==6)r.enabled=v;else {unsigned b=1u<<(o->arg-7);if(v)r.sinks|=b;else r.sinks&=~b;}
        ls_detector_rule_set(rule_pick,&r);
    }
}
static double num(const ls_opt_t *o) {
    ls_detector_rule_t r;if(o->arg==0)return rule_pick+1;if(!ui_rule(&r))return 0;
    return o->arg==2?(double)r.rule_cooldown_s:o->arg==3?(double)r.air_km:o->arg==4?(double)(r.air_alt_below_ft<0?0:r.air_alt_below_ft):(double)r.cooldown_s;
}
static void set_num(const ls_opt_t *o,double v) {
    if(o->arg==0) {ls_detector_status_t s;ls_detector_status(&s);if(v>=1 && v<=s.rules)rule_pick=(unsigned)v-1;}
    else {
        ls_detector_rule_t r;
        if(ls_detector_rule(rule_pick,&r)){
            if(o->arg==2)r.rule_cooldown_s=(uint32_t)v;else if(o->arg==3)r.air_km=(uint16_t)v;else if(o->arg==4)r.air_alt_below_ft=(int32_t)v;else r.cooldown_s=(uint32_t)v;
            ls_detector_rule_set(rule_pick,&r);
        }
    }
}
static void rule_name(const ls_opt_t *o,char *out,size_t n) {(void)o;ls_detector_rule_t r;snprintf(out,n,"%s",ui_rule(&r)?r.id:"No rules");}
static void action(const ls_opt_t *o) {
    if(o->arg==2)ls_sweep_reload_request();
    else {
        if(o->arg==1){test_waiting=true;test_sequence=count?view[0].sequence:0;detail=false;}
        ls_detector_request(o->arg==1);
    }
    refresh_at=0;
}
static const char *why_km(const ls_opt_t *o) {(void)o;ls_detector_rule_t r;return ui_rule(&r) && (r.air_mask&DET_AIR_KM)?NULL:"This rule has no distance test";}
static const char *why_alt(const ls_opt_t *o) {(void)o;ls_detector_rule_t r;return ui_rule(&r) && (r.air_mask&DET_AIR_ALT)?NULL:"This rule has no altitude test";}
#define TOGGLE(label_,arg_) {.label=label_,.kind=LS_OPT_TOGGLE,.arg=arg_,.get=get,.set=set}
static const ls_opt_t SOURCES[]={TOGGLE("BLE Remote ID",0),TOGGLE("SWEEP BLE",1),TOGGLE("SWEEP Wi-Fi (cached only)",2),TOGGLE("LoRa drone-band activity",10),
    TOGGLE("ADS-B aircraft (table only)",11)};
static const ls_opt_t RULES[]={
    {.label="Rule (session edits)",.kind=LS_OPT_LEVEL,.arg=0,.num=num,.set_num=set_num,.lo=1,.hi=64,.step=1,.show=rule_name},
    TOGGLE("Enabled",6),
    {.label="Cooldown",.kind=LS_OPT_LEVEL,.arg=1,.num=num,.set_num=set_num,.lo=0,.hi=86400,.step=10,.unit="seconds"},
    {.label="Rule rate window",.kind=LS_OPT_LEVEL,.arg=2,.num=num,.set_num=set_num,.lo=0,.hi=86400,.step=10,.unit="seconds"},
    TOGGLE("Banner",7),TOGGLE("Vibrate",8),TOGGLE("SD metadata",9),
    {.label="Reload detector rules",.kind=LS_OPT_ACTION,.act=action,.arg=0},
    {.label="Reload SWEEP classifier",.kind=LS_OPT_ACTION,.act=action,.arg=2},
    {.label="Aircraft: within km",.kind=LS_OPT_LEVEL,.arg=3,.num=num,.set_num=set_num,.lo=1,.hi=1000,.step=1,.unit="km from a fresh GPS fix",.why_not=why_km},
    {.label="Aircraft: below ft (baro)",.kind=LS_OPT_LEVEL,.arg=4,.num=num,.set_num=set_num,.lo=0,.hi=60000,.step=100,.unit="feet, barometric as reported",.why_not=why_alt},
};
static const ls_opt_t ALERTS[]={TOGGLE("Allow quiet banners",3),TOGGLE("Allow vibration",4),{.label="Test selected global sinks",.kind=LS_OPT_ACTION,.act=action,.arg=1,.leaves=true}};
static const ls_opt_t LOGGING[]={TOGGLE("SD metadata (no MAC/location)",5)};
static const ls_opt_ctx_t source_ctx={.name="Sources",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(SOURCES)};
static const ls_opt_ctx_t rules_ctx={.name="Rules",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(RULES)};
static const ls_opt_ctx_t alert_ctx={.name="Alerts",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(ALERTS)};
static const ls_opt_ctx_t log_ctx={.name="Logging",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(LOGGING)};
static const ls_opt_t OPTIONS[]={
    {.label="Sources",.kind=LS_OPT_MENU,.sub=&source_ctx},
    {.label="Rules",.kind=LS_OPT_MENU,.sub=&rules_ctx},
    {.label="Alerts",.kind=LS_OPT_MENU,.sub=&alert_ctx},
    {.label="Logging",.kind=LS_OPT_MENU,.sub=&log_ctx},
};
static const ls_opt_ctx_t ctx={.name="DETECTOR",.job=-1,.radio=LS_RSEL_NONE,.back_to_screen=true,.menu_arrows=true,.refresh_ms=250,.covers_screen=true,.draw_header=options_art,LS_OPT_ROWS(OPTIONS)};
/* A fixed receiver-eye emblem, built from block cells without a bitmap buffer. */
static void emblem(tui_surface *sf,tui_rect a,bool live) {
    if(!eye_ready) {
        for(int y=0;y<7;y++)for(int x=0;x<13;x++) {
            int dx=x-6,dy=y-3,d=dx*dx+dy*dy*3;
            if(d>=24 && d<=40)eye_ring[y]|=1u<<x;
            else if((x==6 && y>0 && y<6) || (y==3 && x>3 && x<9))eye_cross[y]|=1u<<x;
        }
        eye_ready=true;
    }
    for(int y=0;y<7;y++)for(int x=0;x<13;x++) {
        if((eye_ring[y]|eye_cross[y])&(1u<<x))
            tui_put_char(sf,a,a.x+x,a.y+y,LS_TUI_BLOCK_FULL,
                INK(eye_ring[y]&(1u<<x)?TUI_BLUE|TUI_BRIGHT:live?TUI_GREEN|TUI_BRIGHT:TUI_CYAN));
    }
    tui_put_char(sf,a,a.x+6,a.y+3,LS_TUI_BLOCK_FULL,INK(live?TUI_YELLOW|TUI_BRIGHT:TUI_WHITE));
}
static void options_art(tui_surface *sf,tui_rect a) {
    ls_detector_status_t s=cached;size_t n;ls_detector_snapshot(&s,NULL,0,&n);
    bool narrow=a.w<40;
    emblem(sf,tui_rect_make(a.x+1,a.y,13,7),s.running);
    tui_rect info=narrow?tui_rect_make(a.x,a.y+8,a.w,a.h-8):tui_rect_make(a.x+15,a.y,a.w-16,7);
    char text[80];
    ls_safe_line(sf,info,info.y,"DETECTION POLICY",INK(TUI_CYAN|TUI_BRIGHT));
    snprintf(text,sizeof(text),"%u rules / revision %u",s.rules,(unsigned)s.revision);
    ls_safe_line(sf,info,info.y+2,text,INK(TUI_WHITE));
    ls_safe_line(sf,info,info.y+4,"Session edits / SD defaults",INK(TUI_YELLOW));
    ls_safe_line(sf,info,info.y+6,"Tap row / < > step / BACK",LS_ATTR_DIM);
    int y=a.y+(narrow?16:9);
    if(y+3<a.y+a.h) {
        snprintf(text,sizeof(text),"%u observations / %u matches",(unsigned)s.observations,(unsigned)s.matches);
        ls_safe_line(sf,a,y,text,INK(TUI_GREEN));
        snprintf(text,sizeof(text),"%u suppressed / %u dropped",(unsigned)s.suppressed,(unsigned)s.dropped);
        ls_safe_line(sf,a,y+2,text,INK(TUI_YELLOW));
    }
    if(y+10<a.y+a.h) {
        ls_safe_line(sf,a,y+5,"SOURCES -> RULES -> ALERT QUEUES",INK(TUI_CYAN|TUI_BRIGHT));
        ls_safe_line(sf,a,y+7,"Source lamps show enabled policy",LS_ATTR_DIM);
        int x=a.x+1;
        x=ls_radio_chip(sf,a,x,y+9,"BLE",!!(s.sources&DET_BLE),TUI_CYAN);
        x=ls_radio_chip(sf,a,x,y+9,"RID",!!(s.sources&DET_RID),TUI_MAGENTA);
        x=ls_radio_chip(sf,a,x,y+9,"WiFi",!!(s.sources&DET_WIFI),TUI_YELLOW);
        ls_radio_chip(sf,a,x,y+9,"ADSB",!!(s.sources&DET_ADSB) && strcmp(s.adsb,"OFF"),TUI_BLUE|TUI_BRIGHT);
    }
    if(y+18<a.y+a.h) {
        ls_safe_line(sf,a,y+13,"Metadata only / no raw RF capture",INK(TUI_GREEN));
        ls_safe_line(sf,a,y+15,"No MAC address or location in log",LS_ATTR_DIM);
        ls_safe_line(sf,a,y+17,"SWEEP recording is independent",INK(TUI_YELLOW));
    }
    if(y+26<a.y+a.h) {
        ls_safe_line(sf,a,y+21,"TEST reports queue acceptance",INK(TUI_CYAN));
        ls_safe_line(sf,a,y+23,"FAILED means the sink refused",INK(TUI_RED));
        ls_safe_line(sf,a,y+25,"BLOCKED means global policy is off",INK(TUI_YELLOW));
    }
}
static const char *sink_state(const ls_detector_event_t *e,unsigned bit) {
    return e->failed&bit?"FAILED":e->delivered&bit?"ACCEPTED":e->allowed&bit?"NOT ACCEPTED":e->selected&bit?"BLOCKED":"NOT SELECTED";
}
static void history_plot(tui_surface *sf,tui_rect a) {
    ls_radio_bezel(sf,a," MATCH SIGNAL / dBm ");
    ls_safe_line(sf,a,a.y+1,count?"OLDER  ------------------>  NEWER":"No samples / waiting for a match",LS_ATTR_DIM);
    int plot_w=a.w-4;tui_rect hole;
    if(ls_tui_cutout(&hole) && hole.x>a.x && a.x+a.w>hole.x &&
       a.y<hole.y+hole.h && a.y+a.h>hole.y)plot_w=hole.x-a.x-3;
    if(plot_w<1)return;
    int n=count<plot_w?count:plot_w;
    for(int i=0;i<n;i++) {
        const ls_detector_event_t *e=&view[n-1-i];
        if(e->test || e->source==DET_ADSB)continue;
        int levels=(a.h-4)*4;
        int level=(e->rssi+110)*levels/80;
        if(level<1)level=1;
        if(level>levels)level=levels;
        int x=a.x+2+i*plot_w/(n?n:1);
        for(int y=0;y<(level+3)/4;y++) {
            unsigned dots=ls_braille_column(level-y*4,0);
            ls_braille_put(sf,a,x,a.y+a.h-3-y,(int16_t)(0x2800|dots),INK(event_hue(e)));
        }
    }
    ls_safe_line(sf,a,a.y+a.h-2,count?"RSSI -110..-30 / dots per match":"History fills only from real events",LS_ATTR_DIM);
}
/* ---- AIRCRAFT view: the shared ADS-B table, read only ---------------------- */
static const char *emergency_word(unsigned s) {
    static const char *const w[]={"none","general","medical","low fuel","no radio","unlawful","downed"};
    return s<7?w[s]:"?";
}
static const char *category_word(const ls_detector_air_t *a) {
    int bit=ls_detector_air_category_bit(a->emitter_tc,a->emitter_ca);
    return bit<0?"-":ls_detector_air_category_name((unsigned)bit);
}
static uint8_t air_hue(const ls_detector_air_row_t *r) {
    return ls_detector_air_alarm(r->a.squawk,r->a.emergency)?TUI_RED:r->rules?TUI_GREEN:TUI_CYAN;
}
/* A small airliner drawn from line characters. Red when an alarm is up. */
static void plane_art(tui_surface *sf,tui_rect clip,int x,int y,bool alarm) {
    static const char *const art[7]={
        "      ^      ",
        "     /|\\     ",
        "    / | \\    ",
        " --o--+--o-- ",
        "   '--|--'   ",
        "      |      ",
        "    __|__    "};
    for(int r=0;r<7;r++)for(int c=0;art[r][c];c++) {
        char ch=art[r][c];if(ch==' ')continue;
        uint8_t hue=ch=='o'?TUI_YELLOW|TUI_BRIGHT:ch=='-'||ch=='\''?TUI_BLUE|TUI_BRIGHT:ch=='_'?TUI_MAGENTA|TUI_BRIGHT:alarm?TUI_RED|TUI_BRIGHT:TUI_CYAN|TUI_BRIGHT;
        tui_put_char(sf,clip,x+c,y+r,ch,INK(hue));
    }
}
static unsigned air_alerts(void) {
    unsigned n=0;for(size_t i=0;i<air_n;i++)n+=air_rows[i].rules!=0;return n;
}
/* A line that routes around the camera hole when it shares the hole's rows. */
static void hole_line(tui_surface *sf,tui_rect t,int row,const char *text,uint8_t attr) {
    tui_rect hole;
    if(ls_tui_cutout(&hole) && row>=hole.y && row<hole.y+hole.h && hole.x>t.x && t.x+t.w>hole.x)t.w=hole.x-t.x-1;
    ls_safe_line(sf,t,row,text,attr);
}
static void air_panel(tui_surface *sf,tui_rect a,const ls_detector_status_t *s) {
    ls_radio_bezel(sf,a," AIRCRAFT WATCH / SHARED TABLE ");
    bool on=strcmp(s->adsb,"OFF")!=0;
    int tx=a.x+2;
    if(a.h>=11 && a.w>=34) {
        unsigned alarms=0;for(size_t i=0;i<air_n;i++)alarms+=ls_detector_air_alarm(air_rows[i].a.squawk,air_rows[i].a.emergency);
        plane_art(sf,a,a.x+2,a.y+2,alarms!=0);
        tx=a.x+17;
    }
    tui_rect t=tui_rect_make(tx,a.y,a.x+a.w-tx-1,a.h);
    char text[80];
    if(on)snprintf(text,sizeof(text),"ADS-B %s  receiving",s->adsb);
    else snprintf(text,sizeof(text),"ADS-B SOURCE OFF");
    hole_line(sf,t,a.y+1,text,INK(on?TUI_GREEN|TUI_BRIGHT:TUI_YELLOW|TUI_BRIGHT));
    if(on)snprintf(text,sizeof(text),"%u aircraft / 60 s",(unsigned)air_n);
    else snprintf(text,sizeof(text),"table not read");
    hole_line(sf,t,a.y+3,text,INK(TUI_CYAN));
    unsigned alerts=air_alerts();
    snprintf(text,sizeof(text),"%u meeting a rule now",alerts);
    hole_line(sf,t,a.y+5,text,INK(alerts?TUI_GREEN|TUI_BRIGHT:TUI_WHITE));
    hole_line(sf,t,a.y+7,"Read only: no radio",LS_ATTR_DIM);
}
static void air_age(const ls_detector_air_t *a,char *out,size_t n) {
    int64_t age=(esp_timer_get_time()-a->last_seen_us)/1000000;
    if(age<0)age=0;
    snprintf(out,n,"%ds",(int)age);
}
static float air_fresh(const ls_detector_air_t *a) {
    float f=1.0f-(float)(esp_timer_get_time()-a->last_seen_us)/(float)LS_DETECTOR_AIR_STALE_US;
    return f<0?0:f>1?1:f;
}
static void air_empty(tui_surface *sf,tui_rect inner,const ls_detector_status_t *s) {
    int y=inner.y+1;
    bool on=strcmp(s->adsb,"OFF")!=0;
    ls_safe_line(sf,inner,y,on?"LISTENING / no aircraft in the last minute":"SOURCE OFF / no ADS-B receiver is running",INK(on?TUI_GREEN|TUI_BRIGHT:TUI_YELLOW|TUI_BRIGHT));
    ls_safe_line(sf,inner,y+3,"DETECTOR claims no radio.",LS_ATTR_DIM);
    ls_safe_line(sf,inner,y+5,"Start ADS-B in its own app;",INK(TUI_CYAN));
    ls_safe_line(sf,inner,y+6,"this list then fills by itself.",INK(TUI_CYAN));
    if(inner.h>=12)ls_safe_line(sf,inner,y+9,"[ ADS-B app ]--table-->[ DETECTOR ]",INK(TUI_BLUE|TUI_BRIGHT));
}
static void air_rows_draw(tui_surface *sf,tui_rect inner,const ls_detector_status_t *s) {
    if(!air_n) {air_empty(sf,inner,s);list_area=tui_rect_make(0,0,0,0);return;}
    char text[100],age[16];
    snprintf(text,sizeof(text),"%u rule matches now / ENTER or tap for detail",air_alerts());
    ls_safe_line(sf,inner,inner.y,text,INK(TUI_YELLOW));
    list_area=tui_rect_make(inner.x,inner.y+2,inner.w,inner.h-2);
    row_height=4;visible=list_area.h/row_height;if(visible<1)visible=1;
    if(air_pick>=(int)air_n)air_pick=(int)air_n-1;
    first=(air_pick/visible)*visible;
    for(int i=first;i<(int)air_n && i<first+visible;i++) {
        const ls_detector_air_row_t *r=&air_rows[i];uint8_t hue=air_hue(r);
        tui_rect row=tui_rect_make(list_area.x,list_area.y+(i-first)*row_height,list_area.w,3);
        ls_fill_dither(sf,row,i==air_pick?LS_DITHER_MEDIUM:LS_DITHER_LIGHT,hue);
        tui_rect tr=tui_rect_make(row.x+1,row.y,row.w-2,row.h);
        const char *id=r->a.callsign[0]?r->a.callsign:NULL;char hex[8];snprintf(hex,sizeof(hex),"%06X",(unsigned)r->a.icao);
        char sq[24]="";
        if(r->a.squawk)snprintf(sq,sizeof(sq),"  sq%04u",r->a.squawk);
        snprintf(text,sizeof(text),"%-8s %-17s%s%s%s",id?id:hex,category_word(&r->a),sq,r->a.emergency?"  EMG":"",r->rules?"  MATCH":"");
        ls_safe_line(sf,tr,row.y,text,INK(TUI_WHITE|TUI_BRIGHT));
        char alt[16]="alt ?",dist[16]="-- km";
        if(r->a.altitude_ft!=INT32_MIN)snprintf(alt,sizeof(alt),"%dft",(int)r->a.altitude_ft);
        if(r->a.distance_x10_km!=LS_DETECTOR_AIR_NO_DISTANCE)snprintf(dist,sizeof(dist),"%u.%ukm",r->a.distance_x10_km/10,r->a.distance_x10_km%10);
        air_age(&r->a,age,sizeof(age));
        snprintf(text,sizeof(text),"%-9s %-8s %4s",alt,dist,age);
        ls_safe_line(sf,tr,row.y+2,text,INK(hue|TUI_BRIGHT));
        /* Freshness is real: full when just heard, empty at the 60 s limit. */
        if(tr.w>=40)ls_meter_ascii(sf,tr,tr.x+tr.w-14,row.y+2,12,air_fresh(&r->a),TUI_GREEN);
    }
}
static void air_detail_draw(tui_surface *sf,tui_rect inner,bool wide) {
    const ls_detector_air_row_t *r=&air_selected;const ls_detector_air_t *a=&r->a;
    char line[10][80];uint8_t ink[10];int n=0;
    uint8_t hue=air_hue(r);
    snprintf(line[n],sizeof(line[n]),"%s  (%06X)",a->callsign[0]?a->callsign:"no callsign",(unsigned)a->icao);ink[n++]=INK(hue|TUI_BRIGHT);
    snprintf(line[n],sizeof(line[n]),"%s%s / category %s",a->sources&2?(a->sources&1?"1090+UAT":"UAT"):"1090",a->qualifier?" (non-ICAO address)":"",category_word(a));ink[n++]=INK(TUI_CYAN);
    if(a->squawk)snprintf(line[n],sizeof(line[n]),"Squawk %04u%s",a->squawk,ls_detector_air_alarm(a->squawk,0)?"  EMERGENCY CODE":"");
    else snprintf(line[n],sizeof(line[n]),"Squawk unknown");
    ink[n++]=INK(ls_detector_air_alarm(a->squawk,0)?TUI_RED|TUI_BRIGHT:TUI_WHITE);
    snprintf(line[n],sizeof(line[n]),"Emergency status: %s",emergency_word(a->emergency));ink[n++]=INK(a->emergency?TUI_RED|TUI_BRIGHT:TUI_WHITE);
    if(a->altitude_ft!=INT32_MIN)snprintf(line[n],sizeof(line[n]),"Altitude %d ft (barometric)",(int)a->altitude_ft);
    else snprintf(line[n],sizeof(line[n]),"Altitude unknown");
    ink[n++]=INK(TUI_YELLOW);
    if(a->distance_x10_km!=LS_DETECTOR_AIR_NO_DISTANCE)snprintf(line[n],sizeof(line[n]),"Distance %u.%u km from your fix",a->distance_x10_km/10,a->distance_x10_km%10);
    else snprintf(line[n],sizeof(line[n]),"Distance unknown");
    ink[n++]=INK(TUI_YELLOW);
    char age[16];air_age(a,age,sizeof(age));
    snprintf(line[n],sizeof(line[n]),"Heard %s ago",age);ink[n++]=INK(TUI_GREEN);
    /* Rules this aircraft meets right now, by name. */
    char names[64]="";
    for(unsigned i=0;i<LS_DETECTOR_RULES && i<64;i++)if(r->rules&(UINT64_C(1)<<i)) {
        ls_detector_rule_t rule;
        if(ls_detector_rule_snapshot(i,&rule)) {
            size_t k=strlen(names);
            snprintf(names+k,sizeof(names)-k,"%s%s",k?",":"",rule.id);
        }
    }
    snprintf(line[n],sizeof(line[n]),"Rules: %s",names[0]?names:"none now");ink[n++]=INK(r->rules?TUI_GREEN|TUI_BRIGHT:TUI_WHITE);
    int half=wide?(n+1)/2:n,stride=(!wide && inner.h>=2*n)?2:1;
    tui_rect left=wide?tui_rect_make(inner.x,inner.y,inner.w/2,inner.h):inner;
    tui_rect right=tui_rect_make(inner.x+inner.w/2,inner.y,inner.w-inner.w/2,inner.h);
    for(int i=0;i<n;i++) {
        tui_rect col=wide&&i>=half?right:left;int row=wide?(i%half)*2:i*stride;
        ls_safe_line(sf,col,inner.y+row,line[i],ink[i]);
    }
    if(!wide && inner.h>=2*n+2) {
        ls_meter_ascii(sf,inner,inner.x+2,inner.y+2*n,inner.w-4,air_fresh(a),TUI_GREEN);
    }
}
static void draw(tui_surface *sf,tui_rect a) {
    memset(keys,0,sizeof(keys));list_area=tui_rect_make(0,0,0,0);
    if(a.w<30 || a.h<(a.w>=90?24:38)) {ls_panel_notice(sf,a,"DETECTOR","More screen space needed","Use a smaller font");return;}
    int64_t now=esp_timer_get_time();
    if(!refresh_at || now>=refresh_at) {
        size_t n;
        if(ls_detector_snapshot(&cached,view,LS_DETECTOR_HISTORY,&n)) {
            count=(int)n;refresh_at=now+250000;
        }
        if(pick>=count)pick=count?count-1:0;
        if(air_view) {
            size_t k;
            if(ls_detector_air_view(air_rows,LS_DETECTOR_AIR_SLOTS,&k)) {
                air_n=k;
                /* An open inspector follows its aircraft, not its row. */
                for(size_t i=0;detail && i<k;i++)if(air_rows[i].a.icao==air_selected.a.icao && air_rows[i].a.qualifier==air_selected.a.qualifier)air_selected=air_rows[i];
            }
            if(air_pick>=(int)air_n)air_pick=air_n?(int)air_n-1:0;
        }
        if(test_waiting)for(int i=0;i<count;i++)if(view[i].test && view[i].sequence>test_sequence) {
            selected=view[i];detail=true;test_waiting=false;break;
        }
    }
    const ls_detector_status_t *s=&cached;char text[160];
    bool wide=a.w>=90;
    int head_h=wide?10:12;
    tui_rect head=tui_rect_make(a.x,a.y,a.w,head_h);
    ls_radio_bezel(sf,head,air_view?(detail?" AIRCRAFT INSPECTOR ":" DETECTOR / AIRCRAFT WATCH "):detail?" MATCH INSPECTOR ":" DETECTOR / PASSIVE WATCH ");
    emblem(sf,tui_rect_make(a.x+2,a.y+2,13,7),s->running);
    tui_rect number=tui_rect_make(a.x+17,a.y+2,wide?22:a.w-19,7);
    snprintf(text,sizeof(text),"%u",(unsigned)s->matches);
    ls_radio_digits_draw(sf,number,0,text,INK(TUI_WHITE|TUI_BRIGHT));
    ls_safe_line(sf,number,number.y+6,"MATCHES / SESSION",INK(TUI_CYAN));
    if(!wide)ls_safe_line(sf,head,a.y+9,"SOURCE POLICY",LS_ATTR_DIM);
    int lamp_y=a.y+(wide?8:10);
    int x=a.x+(wide?43:2);
    x=ls_radio_chip(sf,head,x,lamp_y,s->running?"RUN":"OFF",s->running,TUI_GREEN);
    x=ls_radio_chip(sf,head,x,lamp_y,"BLE",!!(s->sources&DET_BLE),TUI_CYAN);
    x=ls_radio_chip(sf,head,x,lamp_y,"RID",!!(s->sources&DET_RID),TUI_MAGENTA);
    x=ls_radio_chip(sf,head,x,lamp_y,"WiFi",!!(s->sources&DET_WIFI),TUI_YELLOW);
    x=ls_radio_chip(sf,head,x,lamp_y,"LoRa",!!(s->sources&DET_LORA),TUI_BLUE|TUI_BRIGHT);
    ls_radio_chip(sf,head,x,lamp_y,"ADSB",!!(s->sources&DET_ADSB) && strcmp(s->adsb,"OFF"),TUI_WHITE|TUI_BRIGHT);
    if(wide) {
        tui_rect stats=tui_rect_make(a.x+43,a.y+2,a.w-45,7);
        snprintf(text,sizeof(text),"%u observations    %u rules    queue %u",(unsigned)s->observations,s->rules,s->queued);
        ls_safe_line(sf,stats,stats.y,text,INK(TUI_CYAN));
        snprintf(text,sizeof(text),"%u suppressed      %u dropped",(unsigned)s->suppressed,(unsigned)s->dropped);
        ls_safe_line(sf,stats,stats.y+2,text,INK(TUI_YELLOW));
        snprintf(text,sizeof(text),"LoRa %s / ADS-B %s / lamps = policy",s->lora,s->adsb);
        ls_safe_line(sf,stats,stats.y+4,text,LS_ATTR_DIM);
    }
    /* The same filled keys as REC and the radio deck, with explicit shortcuts. */
    const char *caps[]={s->running?"STOP":"START",detail?"BACK":"DETAIL","OPTIONS",air_view?"MATCHES":"AIRCRAFT","PREV","NEXT","TEST"};
    const char *badges="sdoa[]t";
    int cols=wide?7:4,kh=4,control_h=wide?4:8;
    int cy=a.y+a.h-control_h;
    ls_safe_line(sf,a,cy-1,s->error[0]?s->error:test_waiting?"TEST queued / waiting for worker":"S run D detail O opts A aircraft [ ] pick T test",INK(s->error[0]?(strncmp(s->error,"No SD rules",11)?TUI_RED:TUI_YELLOW):TUI_CYAN));
    for(int i=0;i<7;i++) {
        int c=i%cols,r=i/cols,x0=a.x+c*a.w/cols,x1=a.x+(c+1)*a.w/cols;
        keys[i]=tui_rect_make(x0+1,cy+r*kh,x1-x0-2,kh-1);
        ls_freq_key_draw(sf,keys[i],caps[i],badges[i],i==0?TUI_GREEN:i==6?TUI_YELLOW:i==3?TUI_BLUE|TUI_BRIGHT:TUI_CYAN,(i==1&&detail)||(i==3&&air_view));
    }
    tui_rect body=tui_rect_make(a.x,a.y+head_h,a.w,cy-a.y-head_h-1);
    tui_rect content=body;
    if(wide) {
        content.w=body.w*3/5;
        tui_rect side=tui_rect_make(content.x+content.w+1,body.y,body.w-content.w-1,body.h);
        if(air_view)air_panel(sf,side,s);
        else if(!detail)history_plot(sf,side);
        else {
            const ls_detector_event_t *e=&selected;
            ls_radio_bezel(sf,side," EVENT METADATA ");
            snprintf(text,sizeof(text),"Entity %016" PRIx64,e->entity);
            ls_safe_line(sf,side,side.y+1,text,INK(TUI_CYAN));
            snprintf(text,sizeof(text),"Uptime %lld s / grouped %u",(long long)(e->at/1000000),(unsigned)e->coalesced);
            ls_safe_line(sf,side,side.y+3,text,LS_ATTR_DIM);
            if(e->source==DET_LORA)snprintf(text,sizeof(text),"%u MHz ~%u Hz / %u hits/min",e->band,e->rate_guess,(unsigned)e->hits_per_min);
            else snprintf(text,sizeof(text),"%s",e->test?"Operator test / no RF sample":e->source==DET_ADSB?"ADS-B table / broadcast identity":e->validated?"Validated message layout":"Category match / unvalidated identity");
            ls_safe_line(sf,side,side.y+5,text,INK(TUI_YELLOW));
            snprintf(text,sizeof(text),"Dropped %u / suppressed %u",(unsigned)e->dropped,(unsigned)e->suppressed);
            ls_safe_line(sf,side,side.y+7,text,INK(TUI_MAGENTA));
        }
    }
    else {
        int ph=body.h>=30?12:6;
        if(air_view)air_panel(sf,tui_rect_make(body.x,body.y,body.w,ph),s);
        else history_plot(sf,tui_rect_make(body.x,body.y,body.w,ph));
        content.y+=ph;content.h-=ph;
    }
    ls_radio_bezel(sf,content,air_view?(detail?" AIRCRAFT / TABLE DETAIL ":" AIRCRAFT / TAP TO INSPECT "):detail?(selected.test?" SINK TEST RESULT ":" SELECTED MATCH "):" RECENT MATCHES / TAP TO INSPECT ");
    tui_rect inner=tui_rect_make(content.x+1,content.y+1,content.w-2,content.h-2);
    if(air_view) {
        if(detail)air_detail_draw(sf,inner,wide);else air_rows_draw(sf,inner,s);
        return;
    }
    if(detail) {
        const ls_detector_event_t *e=&selected;uint8_t hue=event_hue(e);
        int y=inner.y;
        if(wide) {
            tui_rect facts=tui_rect_make(inner.x,y,inner.w/2,inner.h);
            tui_rect sinks=tui_rect_make(inner.x+inner.w/2,y,inner.w-inner.w/2,inner.h);
            ls_safe_line(sf,facts,y,e->reason,INK(hue|TUI_BRIGHT));
            snprintf(text,sizeof(text),"%s / %s",e->test?"OPERATOR TEST":event_label(e),e->test?"queues":ls_detector_source(e->source));
            ls_safe_line(sf,facts,y+2,text,INK(hue));
            if(e->source==DET_ADSB)ls_detector_air_event_text(e,text,sizeof(text));
            else snprintf(text,sizeof(text),e->test?"No RF sample":"%d dBm",e->rssi);
            ls_safe_line(sf,facts,y+4,text,INK(TUI_WHITE|TUI_BRIGHT));
            if(!e->test && e->source!=DET_ADSB) {
                ls_meter_ascii(sf,facts,facts.x+2,y+5,facts.w-4,(e->rssi+110)/80.f,TUI_GREEN);
                ls_safe_line(sf,facts,y+6,"-110 / -70 / -30 dBm",LS_ATTR_DIM);
            }
            snprintf(text,sizeof(text),"#%u / revision %u",(unsigned)e->sequence,(unsigned)e->revision);
            ls_safe_line(sf,facts,y+7,text,LS_ATTR_DIM);
            const char *labels[]={"BANNER","VIBRATE","SD LOG"};
            for(int i=0;i<3;i++) {
                unsigned bit=1u<<i;
                snprintf(text,sizeof(text),"%s  %s",labels[i],sink_state(e,bit));
                ls_safe_line(sf,sinks,y+2*i,text,INK(e->failed&bit?TUI_RED:e->delivered&bit?TUI_GREEN:TUI_YELLOW));
            }
            ls_safe_line(sf,sinks,y+6,"Accepted = queued",LS_ATTR_DIM);
            ls_safe_line(sf,sinks,y+7,"Not physical delivery",LS_ATTR_DIM);
            return;
        }
        ls_safe_line(sf,inner,y,e->reason,INK(hue|TUI_BRIGHT));
        snprintf(text,sizeof(text),"%s / %s",e->test?"OPERATOR TEST":event_label(e),e->test?"no RF sample":ls_detector_source(e->source));
        ls_safe_line(sf,inner,y+2,text,INK(hue));
        if(e->source==DET_ADSB) {
            ls_detector_air_event_text(e,text,sizeof(text));
            ls_safe_line(sf,inner,y+4,text,INK(TUI_WHITE|TUI_BRIGHT));
            int cb=ls_detector_air_category_bit(e->emitter_tc,e->emitter_ca);
            snprintf(text,sizeof(text),"%06X / %s / sequence %u",(unsigned)e->icao,cb<0?"category not given":ls_detector_air_category_name((unsigned)cb),(unsigned)e->sequence);
            ls_safe_line(sf,inner,y+5,text,LS_ATTR_DIM);
        } else if(!e->test) {
            snprintf(text,sizeof(text),"%d dBm   /   sequence %u",e->rssi,(unsigned)e->sequence);
            ls_safe_line(sf,inner,y+4,text,INK(TUI_WHITE|TUI_BRIGHT));
            ls_meter_ascii(sf,inner,inner.x+2,y+5,inner.w-4,(e->rssi+110)/80.f,TUI_GREEN);
            ls_safe_line(sf,inner,y+6,"-110 / -70 / -30 dBm",LS_ATTR_DIM);
        } else ls_safe_line(sf,inner,y+4,"No radio transmission requested",INK(TUI_GREEN));
        y+=8;
        const char *names[]={"BANNER","VIBRATE","SD LOG"};
        for(int i=0;i<3;i++) {
            unsigned bit=1u<<i;
            snprintf(text,sizeof(text),"%-8s  %s",names[i],sink_state(e,bit));
            ls_safe_line(sf,inner,y+i*2,text,INK(e->failed&bit?TUI_RED:e->delivered&bit?TUI_GREEN:TUI_YELLOW));
        }
        y+=7;
        ls_safe_line(sf,inner,y,"Accepted = queued, not delivery",LS_ATTR_DIM);
        snprintf(text,sizeof(text),"Entity %016" PRIx64,e->entity);ls_safe_line(sf,inner,y+2,text,LS_ATTR_DIM);
        snprintf(text,sizeof(text),"Revision %u / grouped %u",(unsigned)e->revision,(unsigned)e->coalesced);ls_safe_line(sf,inner,y+4,text,INK(TUI_CYAN));
        if(e->source==DET_LORA)snprintf(text,sizeof(text),"%u MHz ~%u Hz / %u hits/min",e->band,e->rate_guess,(unsigned)e->hits_per_min);
        else snprintf(text,sizeof(text),"Uptime %lld s / %s",(long long)(e->at/1000000),e->test?"operator test":e->source==DET_ADSB?"aircraft table":e->validated?"validated layout":"category match");
        ls_safe_line(sf,inner,y+6,text,INK(TUI_YELLOW));
        snprintf(text,sizeof(text),"Dropped %u / suppressed %u",(unsigned)e->dropped,(unsigned)e->suppressed);
        ls_safe_line(sf,inner,y+8,text,INK(TUI_MAGENTA));
    } else if(!count) {
        int y=inner.y+1;
        ls_safe_line(sf,inner,y,s->running?"ARMED / waiting for rule matches":"STOPPED / tap START to monitor",INK(TUI_GREEN|TUI_BRIGHT));
        ls_safe_line(sf,inner,y+3,"No detections recorded this session",LS_ATTR_DIM);
        snprintf(text,sizeof(text),"%u rules / %u observations",s->rules,(unsigned)s->observations);ls_safe_line(sf,inner,y+6,text,INK(TUI_CYAN));
        ls_safe_line(sf,inner,y+9,"BLE / Wi-Fi use SWEEP observations",LS_ATTR_DIM);
        ls_safe_line(sf,inner,y+11,"RID requires validated messages",INK(TUI_MAGENTA));
        snprintf(text,sizeof(text),"LoRa %s",s->lora);ls_safe_line(sf,inner,y+13,text,INK(TUI_YELLOW));
        snprintf(text,sizeof(text),"ADS-B %s / needs another app",s->adsb);ls_safe_line(sf,inner,y+15,text,INK(TUI_BLUE|TUI_BRIGHT));
        ls_safe_line(sf,inner,y+18,"OPTIONS selects sources and sinks",LS_ATTR_DIM);
        ls_safe_line(sf,inner,y+20,"TEST checks enabled alert queues",INK(TUI_CYAN));
    } else {
        snprintf(text,sizeof(text),"%u suppressed / %u dropped%s",(unsigned)s->suppressed,(unsigned)s->dropped,test_waiting?" / TEST queued":"");
        ls_safe_line(sf,inner,inner.y,text,INK(TUI_YELLOW));
        list_area=tui_rect_make(inner.x,inner.y+2,inner.w,inner.h-2);
        row_height=4;visible=list_area.h/row_height;if(visible<1)visible=1;
        first=(pick/visible)*visible;
        for(int i=first;i<count && i<first+visible;i++) {
            const ls_detector_event_t *e=&view[i];uint8_t hue=event_hue(e);
            tui_rect row=tui_rect_make(list_area.x,list_area.y+(i-first)*row_height,list_area.w,3);
            ls_fill_dither(sf,row,i==pick?LS_DITHER_MEDIUM:LS_DITHER_LIGHT,hue);
            uint8_t ink=INK(TUI_WHITE|TUI_BRIGHT);
            /* Text sits on a solid field, with the category retained in its border. */
            tui_rect textrow=tui_rect_make(row.x+1,row.y,row.w-2,row.h);
            snprintf(text,sizeof(text),"%s  %s",e->test?"TEST":event_label(e),e->reason);
            ls_safe_line(sf,textrow,row.y,text,ink);
            if(e->source==DET_ADSB) {char air[64];ls_detector_air_event_text(e,air,sizeof(air));snprintf(text,sizeof(text),"#%u / adsb / %s",(unsigned)e->sequence,air);}
            else snprintf(text,sizeof(text),"#%u / %s / %d dBm",(unsigned)e->sequence,e->test?"sink queues":ls_detector_source(e->source),e->rssi);
            ls_safe_line(sf,textrow,row.y+2,text,INK(hue|TUI_BRIGHT));
        }
    }
}
/* The view shown decides which list the select keys, DETAIL and a tap act on. */
static int list_count(void) {return air_view?(int)air_n:count;}
static void select_row(int i) {
    if(air_view){air_pick=i;air_selected=air_rows[i];}
    else{pick=i;selected=view[i];}
}
static bool key(ls_tk_t k,char ch) {
    if(k==LS_TK_CHAR && ls_opt_key(&ctx,ch))return true;
    if(k==LS_TK_CHAR && (ch=='s'||ch=='S')) {ls_detector_start(!ls_detector_running());refresh_at=0;return true;}
    if(k==LS_TK_CHAR && (ch=='t'||ch=='T')) {ls_opt_t o={.arg=1};action(&o);return true;}
    if(k==LS_TK_CHAR && (ch=='a'||ch=='A')) {air_view=!air_view;detail=false;refresh_at=0;return true;}
    int *at=air_view?&air_pick:&pick,n=list_count();
    if((k==LS_TK_CHAR && (ch=='d'||ch=='D')) || k==LS_TK_ENTER) {if(detail)detail=false;else if(n) {select_row(*at);detail=true;}return true;}
    if(k==LS_TK_ESC && detail) {detail=false;return true;}
    if(k==LS_TK_UP || (k==LS_TK_CHAR && ch=='[')) {if(*at)(*at)--;if(detail&&n)select_row(*at);return true;}
    if(k==LS_TK_DOWN || (k==LS_TK_CHAR && ch==']')) {if(*at+1<n)(*at)++;if(detail&&n)select_row(*at);return true;}
    return false;
}
static bool touch(int x,int y) {
    for(int i=0;i<7;i++)if(keys[i].w && tui_rect_contains(keys[i],x,y))return key(LS_TK_CHAR,"sdoa[]t"[i]);
    if(!detail && list_area.w && tui_rect_contains(list_area,x,y)) {
        int row=(y-list_area.y)/row_height,i=first+row;
        if(row<visible && i<list_count() && (y-list_area.y)%row_height<3){select_row(i);detail=true;}
        return true;
    }
    return false;
}
static void enter(void) {ls_detector_init();ls_sweep_wake();refresh_at=0;}
const ls_tui_screen_t ls_scr_detector={.name="DETECTOR",.enter=enter,.hint="S run  D detail/back  O options  A aircraft  [ ] select  T test",.draw=draw,.key=key,.touch=touch};
