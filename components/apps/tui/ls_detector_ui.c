#include "ls_detector_ui.h"
#include "ls_braille.h"
#include "ls_detector.h"
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
static tui_rect list_area, keys[6];
static int first, visible, row_height;
static bool test_waiting;
static uint32_t test_sequence;
static EXT_RAM_BSS_ATTR ls_detector_status_t cached;
static int64_t refresh_at;
static EXT_RAM_BSS_ATTR uint16_t eye_ring[7],eye_cross[7];
static bool eye_ready;
static void options_art(tui_surface *,tui_rect);

static const uint8_t colors[]={TUI_YELLOW,TUI_MAGENTA,TUI_CYAN,TUI_GREEN,TUI_RED};
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
    if(o->arg<3)return !!(s.sources&(1<<o->arg));
    if(o->arg<6)return !!(s.sinks&(1<<(o->arg-3)));
    ls_detector_rule_t r;if(!ui_rule(&r))return 0;
    return o->arg==6?r.enabled:!!(r.sinks&(1<<(o->arg-7)));
}
static void set(const ls_opt_t *o,int v) {
    ls_detector_status_t s;ls_detector_status(&s);
    if(o->arg==10) {if(v)s.sources|=DET_LORA;else s.sources&=~DET_LORA;ls_detector_policy(s.sources,s.sinks);return;}
    if(o->arg<6) {
        uint8_t *bits=o->arg<3?&s.sources:&s.sinks;unsigned b=1u<<(o->arg%3);
        if(v)*bits|=b;else *bits&=~b;ls_detector_policy(s.sources,s.sinks);
    } else {
        ls_detector_rule_t r;if(!ls_detector_rule(rule_pick,&r))return;
        if(o->arg==6)r.enabled=v;else {unsigned b=1u<<(o->arg-7);if(v)r.sinks|=b;else r.sinks&=~b;}
        ls_detector_rule_set(rule_pick,&r);
    }
}
static double num(const ls_opt_t *o) {ls_detector_rule_t r;return o->arg==0?rule_pick+1:ui_rule(&r)?(o->arg==2?r.rule_cooldown_s:r.cooldown_s):0;}
static void set_num(const ls_opt_t *o,double v) {
    if(o->arg==0) {ls_detector_status_t s;ls_detector_status(&s);if(v>=1 && v<=s.rules)rule_pick=(unsigned)v-1;}
    else {ls_detector_rule_t r;if(ls_detector_rule(rule_pick,&r)){if(o->arg==2)r.rule_cooldown_s=(uint32_t)v;else r.cooldown_s=(uint32_t)v;ls_detector_rule_set(rule_pick,&r);}}
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
#define TOGGLE(label_,arg_) {.label=label_,.kind=LS_OPT_TOGGLE,.arg=arg_,.get=get,.set=set}
static const ls_opt_t SOURCES[]={TOGGLE("BLE Remote ID",0),TOGGLE("SWEEP BLE",1),TOGGLE("SWEEP Wi-Fi (cached only)",2),TOGGLE("LoRa drone-band activity",10)};
static const ls_opt_t RULES[]={
    {.label="Rule (session edits)",.kind=LS_OPT_LEVEL,.arg=0,.num=num,.set_num=set_num,.lo=1,.hi=64,.step=1,.show=rule_name},
    TOGGLE("Enabled",6),
    {.label="Cooldown",.kind=LS_OPT_LEVEL,.arg=1,.num=num,.set_num=set_num,.lo=0,.hi=86400,.step=10,.unit="seconds"},
    {.label="Rule rate window",.kind=LS_OPT_LEVEL,.arg=2,.num=num,.set_num=set_num,.lo=0,.hi=86400,.step=10,.unit="seconds"},
    TOGGLE("Banner",7),TOGGLE("Vibrate",8),TOGGLE("SD metadata",9),
    {.label="Reload detector rules",.kind=LS_OPT_ACTION,.act=action,.arg=0},
    {.label="Reload SWEEP classifier",.kind=LS_OPT_ACTION,.act=action,.arg=2},
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
        ls_radio_chip(sf,a,x,y+9,"WiFi",!!(s.sources&DET_WIFI),TUI_YELLOW);
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
        if(e->test)continue;
        int levels=(a.h-4)*4;
        int level=(e->rssi+110)*levels/80;
        if(level<1)level=1;
        if(level>levels)level=levels;
        int x=a.x+2+i*plot_w/(n?n:1);
        for(int y=0;y<(level+3)/4;y++) {
            unsigned dots=ls_braille_column(level-y*4,0);
            ls_braille_put(sf,a,x,a.y+a.h-3-y,(int16_t)(0x2800|dots),INK(e->category<5?colors[e->category]:TUI_CYAN));
        }
    }
    ls_safe_line(sf,a,a.y+a.h-2,count?"RSSI -110..-30 / dots per match":"History fills only from real events",LS_ATTR_DIM);
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
        if(test_waiting)for(int i=0;i<count;i++)if(view[i].test && view[i].sequence>test_sequence) {
            selected=view[i];detail=true;test_waiting=false;break;
        }
    }
    const ls_detector_status_t *s=&cached;char text[160];
    bool wide=a.w>=90;
    int head_h=wide?10:12;
    tui_rect head=tui_rect_make(a.x,a.y,a.w,head_h);
    ls_radio_bezel(sf,head,detail?" MATCH INSPECTOR ":" DETECTOR / PASSIVE WATCH ");
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
    ls_radio_chip(sf,head,x,lamp_y,"LoRa",!!(s->sources&DET_LORA),TUI_BLUE|TUI_BRIGHT);
    if(wide) {
        tui_rect stats=tui_rect_make(a.x+43,a.y+2,a.w-45,7);
        snprintf(text,sizeof(text),"%u observations    %u rules    queue %u",(unsigned)s->observations,s->rules,s->queued);
        ls_safe_line(sf,stats,stats.y,text,INK(TUI_CYAN));
        snprintf(text,sizeof(text),"%u suppressed      %u dropped",(unsigned)s->suppressed,(unsigned)s->dropped);
        ls_safe_line(sf,stats,stats.y+2,text,INK(TUI_YELLOW));
        snprintf(text,sizeof(text),"LoRa %s / lamps = policy enabled",s->lora);
        ls_safe_line(sf,stats,stats.y+4,text,LS_ATTR_DIM);
    }
    /* The same filled keys as REC and the radio deck, with explicit shortcuts. */
    const char *caps[]={s->running?"STOP":"START",detail?"BACK":"DETAIL","OPTIONS","PREV","NEXT","TEST"};
    const char *badges="sdo[]t";
    int cols=wide?6:3,kh=4,control_h=wide?4:8;
    int cy=a.y+a.h-control_h;
    ls_safe_line(sf,a,cy-1,s->error[0]?s->error:test_waiting?"TEST queued / waiting for worker":"S run  D detail  O options  [ ] select  T test",INK(s->error[0]?(strncmp(s->error,"No SD rules",11)?TUI_RED:TUI_YELLOW):TUI_CYAN));
    for(int i=0;i<6;i++) {
        int c=i%cols,r=i/cols,x0=a.x+c*a.w/cols,x1=a.x+(c+1)*a.w/cols;
        keys[i]=tui_rect_make(x0+1,cy+r*kh,x1-x0-2,kh-1);
        ls_freq_key_draw(sf,keys[i],caps[i],badges[i],i==0?TUI_GREEN:i==5?TUI_YELLOW:TUI_CYAN,i==1&&detail);
    }
    tui_rect body=tui_rect_make(a.x,a.y+head_h,a.w,cy-a.y-head_h-1);
    tui_rect content=body;
    if(wide) {
        content.w=body.w*3/5;
        tui_rect side=tui_rect_make(content.x+content.w+1,body.y,body.w-content.w-1,body.h);
        if(!detail)history_plot(sf,side);
        else {
            const ls_detector_event_t *e=&selected;
            ls_radio_bezel(sf,side," EVENT METADATA ");
            snprintf(text,sizeof(text),"Entity %016" PRIx64,e->entity);
            ls_safe_line(sf,side,side.y+1,text,INK(TUI_CYAN));
            snprintf(text,sizeof(text),"Uptime %lld s / grouped %u",(long long)(e->at/1000000),(unsigned)e->coalesced);
            ls_safe_line(sf,side,side.y+3,text,LS_ATTR_DIM);
            if(e->source==DET_LORA)snprintf(text,sizeof(text),"%u MHz ~%u Hz / %u hits/min",e->band,e->rate_guess,(unsigned)e->hits_per_min);
            else snprintf(text,sizeof(text),"%s",e->test?"Operator test / no RF sample":e->validated?"Validated message layout":"Category match / unvalidated identity");
            ls_safe_line(sf,side,side.y+5,text,INK(TUI_YELLOW));
            snprintf(text,sizeof(text),"Dropped %u / suppressed %u",(unsigned)e->dropped,(unsigned)e->suppressed);
            ls_safe_line(sf,side,side.y+7,text,INK(TUI_MAGENTA));
        }
    }
    else {
        int ph=body.h>=30?12:6;
        history_plot(sf,tui_rect_make(body.x,body.y,body.w,ph));
        content.y+=ph;content.h-=ph;
    }
    ls_radio_bezel(sf,content,detail?(selected.test?" SINK TEST RESULT ":" SELECTED MATCH "):" RECENT MATCHES / TAP TO INSPECT ");
    tui_rect inner=tui_rect_make(content.x+1,content.y+1,content.w-2,content.h-2);
    if(detail) {
        const ls_detector_event_t *e=&selected;uint8_t hue=e->category<5?colors[e->category]:TUI_CYAN;
        int y=inner.y;
        if(wide) {
            tui_rect facts=tui_rect_make(inner.x,y,inner.w/2,inner.h);
            tui_rect sinks=tui_rect_make(inner.x+inner.w/2,y,inner.w-inner.w/2,inner.h);
            ls_safe_line(sf,facts,y,e->reason,INK(hue|TUI_BRIGHT));
            snprintf(text,sizeof(text),"%s / %s",e->test?"OPERATOR TEST":ls_sweep_category(e->category),e->test?"queues":ls_detector_source(e->source));
            ls_safe_line(sf,facts,y+2,text,INK(hue));
            snprintf(text,sizeof(text),e->test?"No RF sample":"%d dBm",e->rssi);
            ls_safe_line(sf,facts,y+4,text,INK(TUI_WHITE|TUI_BRIGHT));
            if(!e->test) {
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
        snprintf(text,sizeof(text),"%s / %s",e->test?"OPERATOR TEST":ls_sweep_category(e->category),e->test?"no RF sample":ls_detector_source(e->source));
        ls_safe_line(sf,inner,y+2,text,INK(hue));
        if(!e->test) {
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
        else snprintf(text,sizeof(text),"Uptime %lld s / %s",(long long)(e->at/1000000),e->test?"operator test":e->validated?"validated layout":"category match");
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
        ls_safe_line(sf,inner,y+16,"OPTIONS selects sources and sinks",LS_ATTR_DIM);
        ls_safe_line(sf,inner,y+18,"TEST checks enabled alert queues",INK(TUI_CYAN));
    } else {
        snprintf(text,sizeof(text),"%u suppressed / %u dropped%s",(unsigned)s->suppressed,(unsigned)s->dropped,test_waiting?" / TEST queued":"");
        ls_safe_line(sf,inner,inner.y,text,INK(TUI_YELLOW));
        list_area=tui_rect_make(inner.x,inner.y+2,inner.w,inner.h-2);
        row_height=4;visible=list_area.h/row_height;if(visible<1)visible=1;
        first=(pick/visible)*visible;
        for(int i=first;i<count && i<first+visible;i++) {
            const ls_detector_event_t *e=&view[i];uint8_t hue=e->category<5?colors[e->category]:TUI_CYAN;
            tui_rect row=tui_rect_make(list_area.x,list_area.y+(i-first)*row_height,list_area.w,3);
            ls_fill_dither(sf,row,i==pick?LS_DITHER_MEDIUM:LS_DITHER_LIGHT,hue);
            uint8_t ink=INK(TUI_WHITE|TUI_BRIGHT);
            /* Text sits on a solid field, with the category retained in its border. */
            tui_rect textrow=tui_rect_make(row.x+1,row.y,row.w-2,row.h);
            snprintf(text,sizeof(text),"%s  %s",e->test?"TEST":ls_sweep_category(e->category),e->reason);
            ls_safe_line(sf,textrow,row.y,text,ink);
            snprintf(text,sizeof(text),"#%u / %s / %d dBm",(unsigned)e->sequence,e->test?"sink queues":ls_detector_source(e->source),e->rssi);
            ls_safe_line(sf,textrow,row.y+2,text,INK(hue|TUI_BRIGHT));
        }
    }
}
static bool key(ls_tk_t k,char ch) {
    if(k==LS_TK_CHAR && ls_opt_key(&ctx,ch))return true;
    if(k==LS_TK_CHAR && (ch=='s'||ch=='S')) {ls_detector_start(!ls_detector_running());refresh_at=0;return true;}
    if(k==LS_TK_CHAR && (ch=='t'||ch=='T')) {ls_opt_t o={.arg=1};action(&o);return true;}
    if((k==LS_TK_CHAR && (ch=='d'||ch=='D')) || k==LS_TK_ENTER) {if(detail)detail=false;else if(count) {selected=view[pick];detail=true;}return true;}
    if(k==LS_TK_ESC && detail) {detail=false;return true;}
    if(k==LS_TK_UP || (k==LS_TK_CHAR && ch=='[')) {if(pick)pick--;if(detail&&count)selected=view[pick];return true;}
    if(k==LS_TK_DOWN || (k==LS_TK_CHAR && ch==']')) {if(pick+1<count)pick++;if(detail&&count)selected=view[pick];return true;}
    return false;
}
static bool touch(int x,int y) {
    for(int i=0;i<6;i++)if(keys[i].w && tui_rect_contains(keys[i],x,y))return key(LS_TK_CHAR,"sdo[]t"[i]);
    if(!detail && list_area.w && tui_rect_contains(list_area,x,y)) {
        int row=(y-list_area.y)/row_height,i=first+row;
        if(row<visible && i<count && (y-list_area.y)%row_height<3){pick=i;selected=view[i];detail=true;}
        return true;
    }
    return false;
}
static void enter(void) {ls_detector_init();ls_sweep_wake();refresh_at=0;}
const ls_tui_screen_t ls_scr_detector={.name="DETECTOR",.enter=enter,.hint="S run  D detail/back  O options  [ ] select  T test",.draw=draw,.key=key,.touch=touch};
