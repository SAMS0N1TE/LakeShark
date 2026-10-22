#include "ls_rid_ui.h"
#include "ls_i18n.h"
#include "ls_rid.h"
#include "ls_wireless.h"
#pragma weak ls_wireless_rid_request
#pragma weak ls_wireless_rid_status
#include "ls_options.h"
#include "ls_field.h"
#include "ls_geo.h"
#include "ls_compass_art.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define INK(c) TUI_ATTR((c),TUI_BLACK)
#define WHITE INK(TUI_WHITE|TUI_BRIGHT)
#define CYAN INK(TUI_CYAN|TUI_BRIGHT)
static EXT_RAM_BSS_ATTR ls_rid_drone_t view[LS_RID_MAX], selected;
static EXT_RAM_BSS_ATTR double ranges[LS_RID_MAX], bearings[LS_RID_MAX];
static EXT_RAM_BSS_ATTR ls_dot_t dots[160*120];
static int order[LS_RID_MAX], count, pick, sort, units, alert, rows=1;
static uint32_t selected_serial, notice_generation;
static bool detail, rx_requested;
static ls_rid_rx_t receiver(void) {
    return ls_wireless_rid_status?ls_wireless_rid_status():LS_RID_RX_UNAVAILABLE;
}
static const char *receiver_name(void) {
    static const char *const names[]={"OFF","UNAVAILABLE","STARTING","START FAILED","PAUSED / CONNECTING","LISTENING"};
    return ls_tr(names[receiver()]);
}
static void receive_start(const ls_opt_t *o) {
    (void)o; if(ls_wireless_rid_request) {rx_requested=true;ls_wireless_rid_request(true);}
}
static void receive_stop(const ls_opt_t *o) {
    (void)o; if(ls_wireless_rid_request) ls_wireless_rid_request(false); rx_requested=false;
}
static tui_rect list_area;
static int get(const ls_opt_t *o) { return o->arg==0?sort:o->arg==1?units:alert; }
static void set(const ls_opt_t *o,int v) {
    if(o->arg==0) sort=v; else if(o->arg==1) units=v;
    else { alert=v; notice_generation=ls_rid_generation(); }
}
static void clear(const ls_opt_t *o) { (void)o; ls_rid_clear(); detail=false; pick=0; }
static const char *const SORT[]={"DISTANCE","RSSI","AGE"};
static const char *const UNITS[]={"METRIC","IMPERIAL"};
static const ls_opt_t DISPLAY[]={
    {.label="SORT",.kind=LS_OPT_CYCLE,.arg=0,.names=SORT,.n=3,.get=get,.set=set},
    {.label="UNITS",.kind=LS_OPT_CYCLE,.arg=1,.names=UNITS,.n=2,.get=get,.set=set},
};
static const ls_opt_ctx_t DISPLAY_CTX={.name="DISPLAY",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(DISPLAY)};
static const ls_opt_t ALERTS[]={
    {.label="NEW DRONE ALERT",.kind=LS_OPT_TOGGLE,.arg=2,.get=get,.set=set},
};
static const ls_opt_ctx_t ALERT_CTX={.name="ALERTS",.job=-1,.radio=LS_RSEL_NONE,LS_OPT_ROWS(ALERTS)};
static const ls_opt_t OPTIONS[]={
    {.label="DISPLAY",.kind=LS_OPT_MENU,.sub=&DISPLAY_CTX},
    {.label="ALERTS",.kind=LS_OPT_MENU,.sub=&ALERT_CTX},
    {.label="CLEAR TABLE",.kind=LS_OPT_ACTION,.act=clear},
    {.label="START BLE RX",.kind=LS_OPT_ACTION,.act=receive_start},
    {.label="RELEASE BLE RX",.kind=LS_OPT_ACTION,.act=receive_stop},
};
static const ls_opt_ctx_t CTX={.name="DRONES",.job=-1,.radio=LS_RSEL_NONE,.back_to_screen=true,LS_OPT_ROWS(OPTIONS)};
static void line(tui_surface *sf,tui_rect a,int r,const char *s,uint8_t ink) {
    ls_safe_line(sf,a,a.y+r,ls_tr(s),ink);
}
static void measure(char *out,size_t n,double m) {
    if(!isfinite(m)) snprintf(out,n,"--");
    else if(units) snprintf(out,n,"%.0f ft",m*3.28084);
    else snprintf(out,n,"%.0f m",m);
}
static bool before(int a,int b) {
    if(sort==0 && ranges[a]!=ranges[b]) return ranges[a]<ranges[b];
    if(sort==2 && view[a].last_us!=view[b].last_us) return view[a].last_us>view[b].last_us;
    return view[a].rssi>view[b].rssi;
}
static void refresh(int64_t now) {
    count=(int)ls_rid_snapshot(view,LS_RID_MAX,now);
    ls_field_sample_t fix; ls_field_sample_snapshot(&fix);
    for(int i=0;i<count;i++) {
        ranges[i]=INFINITY; bearings[i]=NAN; order[i]=i;
        if(fix.gps_valid && view[i].position_valid && ls_rid_fresh(&view[i],1,now))
            ls_geo_bearing_range(fix.lat,fix.lon,view[i].lat,view[i].lon,&bearings[i],&ranges[i]);
    }
    for(int i=1;i<count;i++) { int v=order[i],j=i; while(j>0 && before(v,order[j-1])) {order[j]=order[j-1];j--;} order[j]=v; }
    if(pick>=count) pick=count?count-1:0;
    if(selected_serial && !detail) for(int i=0;i<count;i++) if(view[order[i]].serial==selected_serial) {pick=i;break;}
    if(count && !detail) selected_serial=view[order[pick]].serial;
    if(detail) for(int i=0;i<count;i++) if(view[i].serial==selected_serial) selected=view[i];
}
static void dot(int w,int h,int x,int y,uint8_t colour) {
    if(x>=0 && y>=0 && x<w && y<h) dots[y*w+x]=(ls_dot_t){colour,true};
}
static void radar(tui_surface *sf,tui_rect a,int64_t now) {
    ls_panel_box(sf,a,"RADAR / RX",TUI_GREEN);
    tui_rect pic=tui_rect_make(a.x+1,a.y+2,a.w-2,a.h-5);
    int w=pic.w*2,h=pic.h*3,cw=10,ch=17;
    if(w<4 || h<4 || w*h>160*120) return;
    ls_tui_geometry(NULL,NULL,&cw,&ch);
    double radius=fmin(pic.w*cw,pic.h*ch)*.45;
    double rx=radius*2/cw, ry=radius*3/ch;
    memset(dots,0,(size_t)w*h*sizeof(*dots));
    for(int ring=1;ring<=3;ring++) for(int i=0;i<360;i++) {
        double t=i*.01745329252;
        dot(w,h,w/2+(int)lround(sin(t)*rx*ring/3),h/2-(int)lround(cos(t)*ry*ring/3),ring==3?TUI_GREEN:TUI_CYAN);
    }
    int64_t heard=-1;
    for(int i=0;i<count;i++) if(view[i].last_us>heard) heard=view[i].last_us;
    bool pulse=receiver()==LS_RID_RX_LISTENING && heard>=0 && now>=heard && now-heard<1000000;
    double phase=(now-heard)*6.2831853/1000000;
    for(int trail=0;pulse && trail<16;trail++) for(int r=1;r<=80;r++) {
        if(trail && (r+trail)%4) continue;
        double t=phase-trail*.025;
        dot(w,h,w/2+(int)lround(sin(t)*rx*r/80),h/2-(int)lround(cos(t)*ry*r/80),trail?TUI_GREEN:TUI_GREEN|TUI_BRIGHT);
    }
    for(int i=0;i<count;i++) {
        int rank=0; for(int j=0;j<count;j++) if(view[j].rssi>view[i].rssi) rank++;
        bool geo=isfinite(bearings[i]);
        double t=geo?bearings[i]*.01745329252:i*2.39996323;
        double r=geo?fmin(.92,.15+ranges[i]/2000):.2+.65*(rank+1)/(count+1.0);
        int x=w/2+(int)lround(sin(t)*rx*r),y=h/2-(int)lround(cos(t)*ry*r);
        uint8_t ink=view[i].serial==selected_serial?TUI_WHITE|TUI_BRIGHT:TUI_YELLOW|TUI_BRIGHT;
        for(int dx=-2;dx<=2;dx++) for(int dy=-2;dy<=2;dy++) if(dx*dx+dy*dy<=4) dot(w,h,x+dx,y+dy,ink);
    }
    ls_dots_blit(sf,pic,dots,w);
    tui_put_char(sf,a,pic.x+pic.w/2,pic.y+pic.h/2,'@',WHITE);
    line(sf,a,a.h-2,"GPS bearing / else RSSI rank only",LS_ATTR_DIM);
}
static const char *status_name(unsigned status) {
    static const char *const names[]={"UNDECLARED","ON GROUND","AIRBORNE","EMERGENCY","RID FAILURE"};
    return status<5?names[status]:"RESERVED";
}
static void detail_line(tui_surface *sf,tui_rect a,int r,const char *s,uint8_t ink) {
    s=ls_tr(s);
    if(ls_tui_is_wide()) {
        int half=a.w/2;
        if(r>=13) {a.x+=half;a.w-=half;r-=11;} else a.w=half;
    }
    line(sf,a,r,s,ink);
}
static void show_detail(tui_surface *sf,tui_rect a,int64_t now) {
    ls_rid_drone_t *d=&selected; char s[160],x[24],y[24],z[24];
    bool flight=ls_rid_fresh(d,1,now), system=ls_rid_fresh(d,4,now), operator=ls_rid_fresh(d,5,now);
    uint8_t flight_ink=flight?WHITE:LS_ATTR_DIM, system_ink=system?WHITE:LS_ATTR_DIM;
    const char *history=flight?"":" / STALE";
    ls_panel_box(sf,a,"DRONE DETAIL / UNVERIFIED",TUI_CYAN);
    if(ls_tui_is_wide()) ls_panel_box(sf,tui_rect_make(a.x+a.w/2,a.y,a.w-a.w/2,a.h),"OPERATOR / SYSTEM",TUI_CYAN);
    detail_line(sf,a,2,d->uas_id[0]?d->uas_id:"Awaiting Basic ID",WHITE);
    snprintf(s,sizeof(s),"%02X:%02X:%02X:%02X:%02X:%02X / %d dBm",d->mac[5],d->mac[4],d->mac[3],d->mac[2],d->mac[1],d->mac[0],d->rssi); detail_line(sf,a,4,s,CYAN);
    snprintf(s,sizeof(s),"Seen %.0fs ago / %s%s",(now-d->last_us)/1e6,(d->messages&2)?(flight?status_name(d->status):"FLIGHT STALE"):"FLIGHT --",now-d->last_us>=60000000LL?" / EXPIRED":""); detail_line(sf,a,5,s,WHITE);
    snprintf(s,sizeof(s),"UA %s %.5f, %.5f%s",d->position_valid?"GPS":"--",d->lat,d->lon,history); detail_line(sf,a,7,s,flight_ink);
    measure(x,sizeof(x),d->geo_m); measure(y,sizeof(y),d->baro_m);
    snprintf(s,sizeof(s),"GEO %s / BARO %s%s",x,y,history); detail_line(sf,a,8,s,flight_ink);
    measure(x,sizeof(x),d->height_m); snprintf(s,sizeof(s),"HEIGHT %s / %s%s",x,d->height_agl?"AGL":"ABOVE TAKEOFF",history); detail_line(sf,a,9,s,flight_ink);
    snprintf(s,sizeof(s),"Speed %.1f %s / track %.0f deg%s",d->speed_mps*(units?2.23694:1),units?"mph":"m/s",d->track_deg,history); detail_line(sf,a,10,s,flight_ink);
    snprintf(s,sizeof(s),"Vertical %.1f %s / fix age %.0fs%s",d->vertical_mps*(units?3.28084:1),units?"ft/s":"m/s",(d->messages&2)?(now-d->location_us)/1e6:NAN,history); detail_line(sf,a,11,s,LS_ATTR_DIM);
    snprintf(s,sizeof(s),"Operator %s %.5f, %.5f%s",d->operator_valid?"GPS":"--",d->operator_lat,d->operator_lon,system?"":" / STALE"); detail_line(sf,a,13,s,system_ink);
    const char *type=d->operator_location_type==0?"TAKEOFF":d->operator_location_type==1?"LIVE GNSS":d->operator_location_type==2?"FIXED":"UNKNOWN";
    measure(x,sizeof(x),d->operator_geo_m);
    if(d->messages&16) snprintf(s,sizeof(s),"%s / GEO %s / System %.0fs%s",type,x,(now-d->system_us)/1e6,system?"":" STALE");
    else snprintf(s,sizeof(s),"System -- / GEO --");
    detail_line(sf,a,14,s,LS_ATTR_DIM);
    if(d->messages&32) snprintf(s,sizeof(s),"Operator ID: %s / %.0fs%s",d->operator_id[0]?d->operator_id:"--",(now-d->operator_us)/1e6,operator?"":" STALE");
    else snprintf(s,sizeof(s),"Operator ID: --");
    detail_line(sf,a,15,s,operator?WHITE:LS_ATTR_DIM);
    measure(x,sizeof(x),d->area_radius_m); snprintf(s,sizeof(s),"Area: %u aircraft / radius %s",d->area_count,x); detail_line(sf,a,17,s,system_ink);
    measure(y,sizeof(y),d->area_floor_m); measure(z,sizeof(z),d->area_ceiling_m); snprintf(s,sizeof(s),"Floor %s / ceiling %s",y,z); detail_line(sf,a,18,s,system_ink);
    if(d->system_version) snprintf(s,sizeof(s),"Class type %u / category %u / class %u",d->classification_type,d->category,d->class_id);
    else snprintf(s,sizeof(s),"Class type %u / category -- / class --",d->classification_type);
    detail_line(sf,a,19,s,system_ink);
    snprintf(s,sizeof(s),"Self ID: %s",d->self_id[0]?d->self_id:"--"); detail_line(sf,a,21,s,WHITE);
    snprintf(s,sizeof(s),"Auth %s / type %u page %u",d->messages&4?"SEEN, UNVERIFIED":"ABSENT",d->auth_type,d->auth_page); detail_line(sf,a,22,s,LS_ATTR_DIM);
}
static void draw_body(tui_surface *sf,tui_rect a,int64_t now);
void ls_rid_draw(tui_surface *sf,tui_rect a) {
    int64_t now=esp_timer_get_time(); refresh(now);
    if(a.w<26 || a.h<18) { ls_panel_notice(sf,a,"DRONES","More screen space needed","Use a smaller font"); return; }
    int bar_h=ls_tui_is_wide()?3:5;
    ls_btn_t buttons[]={ {detail?"BACK":"DETAIL",detail?"LIST":"SELECTED",detail?'b':'d',false,!detail&&!count,false},
        {receiver()==LS_RID_RX_LISTENING?"PREV":"RECEIVE",receiver()==LS_RID_RX_LISTENING?"DRONE":"START / R",receiver()==LS_RID_RX_LISTENING?'[':'r',false,detail || (receiver()==LS_RID_RX_LISTENING && !count),false},
        {"NEXT","DRONE",']',false,detail||!count,false},ls_opt_button(&CTX) };
    ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y+a.h-bar_h,a.w,bar_h),buttons,4,-1);
    a.h-=bar_h;
    draw_body(sf,a,now);
}
void ls_rid_draw_body(tui_surface *sf,tui_rect a) {
    int64_t now=esp_timer_get_time(); refresh(now);
    if(a.w<26 || a.h<12) { ls_panel_notice(sf,a,"REMOTE ID","More screen space needed","Use a smaller font"); return; }
    draw_body(sf,a,now);
}
bool ls_rid_in_detail(void) { return detail; }
void ls_rid_close_detail(void) { detail=false; }
static void draw_body(tui_surface *sf,tui_rect a,int64_t now) {
    if(detail) { show_detail(sf,a,now); return; }
    char s[128]; snprintf(s,sizeof(s),ls_tr("DRONES / ESP32-C6 / %d active"),count); line(sf,a,0,s,CYAN);
    snprintf(s,sizeof(s),ls_tr("BLE %s / expire after 60s"),receiver_name()); line(sf,a,1,s,LS_ATTR_DIM);
    bool wide=ls_tui_is_wide();
    int radar_h=wide?a.h-3:(a.h-3)/2, radar_w=wide?a.w/2:a.w;
    radar(sf,tui_rect_make(a.x,a.y+3,radar_w,radar_h),now);
    list_area=wide?tui_rect_make(a.x+radar_w,a.y+3,a.w-radar_w,a.h-3):tui_rect_make(a.x,a.y+3+radar_h,a.w,a.h-3-radar_h);
    ls_panel_box(sf,list_area,"HEARD DRONES",TUI_CYAN);
    snprintf(s,sizeof(s),ls_tr("ID / RANGE / RSSI / AGE  [%s]"),ls_tr(SORT[sort])); line(sf,list_area,1,s,LS_ATTR_DIM);
    rows=(list_area.h-3)/3; if(rows<1) rows=1;
    if(!count) { line(sf,list_area,3,receiver()==LS_RID_RX_LISTENING?"Listening for Remote ID...":receiver_name(),WHITE); line(sf,list_area,5,"R Receive / O receiver options",LS_ATTR_DIM); return; }
    int start=pick/rows*rows;
    for(int i=start;i<count && i<start+rows;i++) {
        ls_rid_drone_t *d=&view[order[i]]; int r=3+(i-start)*3; char range[24];
        measure(range,sizeof(range),ranges[order[i]]);
        snprintf(s,sizeof(s),"%c %s",i==pick?'>':' ',d->uas_id[0]?d->uas_id:ls_tr("Awaiting ID")); line(sf,list_area,r,s,i==pick?CYAN:WHITE);
        snprintf(s,sizeof(s),"  %s / %d dBm / %.0fs%s",range,d->rssi,(now-d->last_us)/1e6,ls_rid_fresh(d,1,now) && d->status==3?ls_tr(" / EMERGENCY"):""); line(sf,list_area,r+1,s,LS_ATTR_DIM);
    }
}
static void open_detail(void) {
    if(!count) return;
    selected=view[order[pick]]; selected_serial=selected.serial; detail=true;
}
bool ls_rid_key(ls_tk_t k,char ch) {
    if(ls_opt_key(&CTX,ch)) return true;
    if(k==LS_TK_ESC || ch=='b' || ch=='B') {
        if(detail) {detail=false;return true;}
        if(k==LS_TK_ESC) return false;
        ls_tui_screen_show(0);return true;
    }
    if(ch=='r' || ch=='R') {receive_start(NULL);return true;}
    if(k==LS_TK_ENTER || ch=='d' || ch=='D') {open_detail();return true;}
    if(!detail && (k==LS_TK_UP || k==LS_TK_DOWN || ch=='[' || ch==']') && count) {
        pick=(pick+count+(k==LS_TK_UP || ch=='['?-1:1))%count; selected_serial=view[order[pick]].serial;return true;
    }
    return false;
}
bool ls_rid_touch(int x,int y) {
    int b=ls_btn_hit(x,y); if(b==0) {if(detail) detail=false;else open_detail();return true;}
    if(b==1 || b==2) {return ls_rid_key(LS_TK_CHAR,b==1?(receiver()==LS_RID_RX_LISTENING?'[':'r'):']');}
    if(b==3) {ls_opt_open(&CTX);return true;}
    if(!detail && x>=list_area.x && x<list_area.x+list_area.w && y>=list_area.y+3 && y<list_area.y+list_area.h) {
        int i=pick/rows*rows+(y-list_area.y-3)/3;
        if(i<count) {pick=i;open_detail();} return true;
    }
    return false;
}
bool ls_rid_touch_body(int x,int y) {
    if(!detail && x>=list_area.x && x<list_area.x+list_area.w && y>=list_area.y+3 && y<list_area.y+list_area.h) {
        int i=pick/rows*rows+(y-list_area.y-3)/3;
        if(i<count) {pick=i;open_detail();} return true;
    }
    return false;
}
bool ls_rid_notice(ls_notice_t *out) {
    uint32_t g=ls_rid_generation(); if(g==notice_generation) return false;
    uint32_t added=g-notice_generation; notice_generation=g;
    if(!alert) return false;
    snprintf(out->title,sizeof(out->title),"DRONES");
    /* Ukrainian says the count after a colon, which needs no plural ending. */
    snprintf(out->body,sizeof(out->body),ls_tr("%lu new Remote ID source%s"),(unsigned long)added,
             ls_i18n_lang()!=LS_LANG_EN||added==1?"":"s");
    out->screen=ls_tui_screen_index_of(&ls_scr_drones); out->hue=TUI_CYAN; out->accent=TUI_CYAN; out->notify_visible=true;
    return true;
}
/* The DRONES screen (ls_drone_ui.c) carries Remote ID as one of its pages. */
void ls_rid_leave(void) {detail=false; if(rx_requested) receive_stop(NULL);}
