#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_numpad.h"
#include "../../ls_picker.h"
#include "../../ls_motion.h"
#include "../../ls_field.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "ls_mesh.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int button_focus=-1, button_slot;
static EXT_RAM_BSS_ATTR rec_watch_status_t s;
static int selected;
static char feedback[80], peers[LS_MESH_MAX_PEERS][17];
static tui_rect list;
static bool touch_nav;
static ls_fresh_t arrivals;
static int setup_item;
static const double bands[]={152.600,154.785,315.000,433.920,868.350,915.000};
static void tune(double mhz)
{
    if(!isfinite(mhz) || mhz<24 || mhz>1766) {snprintf(feedback,sizeof(feedback),"Enter 24-1766 MHz; receiver must support it");return;}
    if(rec_watch_enabled()) {snprintf(feedback,sizeof(feedback),"Stop WATCH before changing frequency");return;}
    rec_disarm();rec_set_freq((uint32_t)(mhz*1e6+.5));
}
static void peer_done(int i)
{
    if(i==0)rec_watch_alert_target("");
    else if(i<=LS_MESH_MAX_PEERS && peers[i-1][0])rec_watch_alert_target(peers[i-1]);
}
static void band_done(int i) {if(i>=0 && i<6)tune(bands[i]);}
static void setup_value(double value)
{
    static const double low[]={0,2,4,10,6},high[]={255,2000,10000,30000,4096};
    if(rec_watch_enabled()) {snprintf(feedback,sizeof(feedback),"Stop WATCH before changing capture setup");return;}
    if(setup_item<0 || setup_item>4 || !isfinite(value) || value<low[setup_item] || value>high[setup_item]) {
        snprintf(feedback,sizeof(feedback),"Value outside the displayed range");return;
    }
    rec_disarm();
    int n=(int)(value+.5);
    if(setup_item==0)rec_set_thresh(n);
    else if(setup_item==1)rec_set_gap_ms(n);
    else if(setup_item==2)rec_set_min_pulse(n);
    else if(setup_item==3)rec_set_max_span((uint32_t)n*1000);
    else rec_set_min_edges(n);
}
static void setup_done(int i)
{
    if(rec_watch_enabled()) {snprintf(feedback,sizeof(feedback),"Stop WATCH before changing capture setup");return;}
    if(i==5) {
        rec_disarm();rec_set_thresh(0);rec_set_gap_ms(30);rec_set_min_pulse(40);
        rec_set_max_span(8000000);rec_set_min_edges(6);
        snprintf(feedback,sizeof(feedback),"Capture defaults restored");return;
    }
    if(i<0 || i>4)return;
    setup_item=i;
    const char *title[]={"THRESHOLD 0=AUTO,1-255","END GAP 2-2000 ms","MIN PULSE 4-10000 us","MAX SPAN 10-30000 ms","MIN EDGES 6-4096"};
    const char *unit[]={"magnitude","ms","us","ms","edges"};
    double value[]={rec_get_thresh(),rec_get_gap_ms(),rec_get_min_pulse(),rec_get_max_span()/1000.,rec_get_min_edges()};
    ls_numpad_open(title[i],unit[i],value[i],setup_value);
}
static void action(int i)
{
    feedback[0]=0;
    if(i==0) {
        bool on=!rec_watch_enabled();
        if(rec_watch_enable(on)) {if(on)rec_arm_request();else rec_disarm();}
        else snprintf(feedback,sizeof(feedback),"Archive loading; try WATCH again shortly");
    } else if(i==1) ls_numpad_open("WATCH FREQUENCY","MHz",rec_get_freq()/1e6,tune);
    else if(i==2 && selected<s.count) {
        if(!rec_watch_request_pin(s.event[selected].id,!s.event[selected].pinned))
            snprintf(feedback,sizeof(feedback),"Pin request busy");
    } else if(i==3) {
        memset(peers,0,sizeof(peers));
        ls_picker_open("DETECTION DM TARGET",peer_done);
        ls_picker_add("OFF","No detection messages");
        ls_mesh_peer_t peer;
        for(int j=0;j<LS_MESH_MAX_PEERS && ls_mesh_peer_at(j,&peer);j++) {
            snprintf(peers[j],sizeof(peers[j]),"%s",peer.id);
            ls_picker_add(peer.name[0]?peer.name:peer.id,"New patterns; max one/min; Mesh TX must be armed");
        }
    } else if(i==4 && selected<s.count) {
        if(!rec_watch_request_export(s.event[selected].id)) snprintf(feedback,sizeof(feedback),"Export busy or pattern unavailable");
    } else if(i==5 && selected<s.count) {
        const rec_watch_event_t *e=&s.event[selected];
        char title[48],text[480];
        snprintf(title,sizeof(title),"SubGHz pattern #%lu",(unsigned long)e->id);
        snprintf(text,sizeof(text),"RTL OOK timing pattern; not a decoded device identity. "
            "%.4f MHz, %u edges, %.2f ms, %lu observations. "
            "First: boot %08lx uptime %llu ms. Last: boot %08lx uptime %llu ms. "
            "GPS and motion attachment describe this bookmark, not the original capture. "
            "Export the representative from SUB-GHZ for pulse data.",e->frequency/1e6,e->edges,e->span_us/1000.,
            (unsigned long)e->count,(unsigned long)e->first_boot,(unsigned long long)e->first_ms,
            (unsigned long)e->last_boot,(unsigned long long)e->last_ms);
        bool ok=ls_field_mark_radio(title,text,LS_FIELD_RTL,e->frequency,e->count);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy; try again shortly");
    } else if(i==6) {
        ls_picker_open("RECEIVE FREQUENCY",band_done);
        for(int j=0;j<6;j++) {
            char label[32];snprintf(label,sizeof(label),"%.4f MHz",bands[j]);
            ls_picker_add(label,j<2?"OOK only; no FSK":"Receive preset");
        }
    } else if(i==7) {
        ls_picker_open("CAPTURE SETUP",setup_done);
        ls_picker_add("Threshold","0 = automatic");
        ls_picker_add("End gap","Silence to end");
        ls_picker_add("Minimum pulse","Glitch filter");
        ls_picker_add("Maximum capture","4096 edges max");
        ls_picker_add("Minimum edges","Reject fragments");
        ls_picker_add("Restore defaults","Auto / 30ms gap");
    }
}
static void enter(void) { button_focus=-1;button_slot=0;rec_watch_start();ls_field_start();ls_field_watch(true);feedback[0]=0;}
static void leave(void) {ls_field_watch(false);}
static void waveform(tui_surface *sf,tui_rect a)
{
    ls_panel_box(sf,a,"SELECTED PULSE TIMING",TUI_CYAN);
    if(selected>=s.count || a.h<8)return;
    int n=s.event[selected].edges;if(n>48)n=48;
    uint64_t total=0;
    for(int i=0;i<n;i++)total+=s.preview[selected][i]<0?-(int64_t)s.preview[selected][i]:s.preview[selected][i];
    if(!total)return;
    uint64_t end=0;int edge=0;
    for(int x=0;x<a.w-4;x++) {
        uint64_t t=(uint64_t)x*total/(a.w-4);
        while(edge<n-1 && end+(uint64_t)(s.preview[selected][edge]<0?-(int64_t)s.preview[selected][edge]:s.preview[selected][edge])<=t) {
            end+=s.preview[selected][edge]<0?-(int64_t)s.preview[selected][edge]:s.preview[selected][edge];edge++;
        }
        int y=a.y+(s.preview[selected][edge]>0?2:4);
        tui_put_char(sf,a,a.x+2+x,y,'-',TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK));
    }
    char text[90];snprintf(text,sizeof(text),"First %d/%u edges | %.2f ms shown",n,s.event[selected].edges,total/1000.);
    tui_put_str(sf,a,a.x+2,a.y+a.h-3,text,LS_ATTR_DIM);
    tui_put_str(sf,a,a.x+2,a.y+a.h-2,"Timing only; OOK envelope, not decoded data",LS_ATTR_DIM);
}
static void draw(tui_surface *sf,tui_rect a)
{
    if(a.w<24 || a.h<18) {ls_panel_notice(sf,a,"SUB-GHZ","Enlarge the pane","WATCH keeps its state");return;}
    rec_watch_snapshot(&s);
    uint8_t fresh=ls_fresh(&arrivals,s.received,600);
    touch_nav=a.w<90 && a.h>35;
    if(touch_nav){
        ls_btn_t nav[]={{"PREV","PATTERN",',',false,!s.count || selected==0},{"NEXT","PATTERN",'.',false,selected+1>=s.count}};
        ls_btn_bar_raised_slot(sf,tui_rect_make(a.x,a.y+a.h-5,a.w,5),nav,2,-1,LS_BTN_SLOT_WATERFALL);
        a.h-=5;
    }
    if(selected>=s.count)selected=s.count?s.count-1:0;
    rec_hub_status_t rx;rec_get_hub_status(&rx);
    ls_btn_t btn[]={{"WATCH",s.enabled?"ON":"OFF",'w',s.enabled,!s.ready},
        {"TUNE","MHz",'f',false,s.enabled},
        {"PIN",selected<s.count && s.event[selected].pinned?"KEPT":"KEEP",'p',selected<s.count && s.event[selected].pinned,!s.count},
        {"ALERTS",s.alerts?"DM":"OFF",'a',s.alerts,false},
        {"EXPORT",".SUB",'e',false,!s.count || s.exporting},
        {"JOURNAL","MARK",'j',false,!s.count},
        {"BANDS","PRESET",'b',false,s.enabled},
        {"SETUP","CAPTURE",'s',false,s.enabled}};
    int h=ls_btn_raised_height(a,8);
    ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,h),btn,8,button_focus);
    tui_rect body=tui_rect_make(a.x,a.y+h,a.w,a.h-h-3);
    ls_panel_box(sf,body,"PATTERN WATCH / RTL OOK",TUI_CYAN);
    ls_motion_busy(sf,body,s.exporting || (s.enabled && rx.receiver_streaming));
    char line[110];
    snprintf(line,sizeof(line),"%c %.4f MHz | %s",ls_motion_pip(s.enabled && rx.receiver_streaming),
        rx.freq_hz/1e6,!s.enabled?"STOPPED":rx.receiver_streaming?"LISTENING":"RX UNAVAILABLE");
    tui_put_str(sf,body,body.x+2,body.y+1,line,LS_ATTR_DIM);
    snprintf(line,sizeof(line),"%lu captures  %lu skipped  %d/16 patterns",(unsigned long)s.received,(unsigned long)s.dropped,s.count);
    tui_put_str(sf,body,body.x+2,body.y+2,line,ls_fresh_attr(fresh,TUI_GREEN|TUI_BRIGHT,TUI_WHITE,TUI_BLACK));
    list=tui_rect_make(body.x+2,body.y+4,body.w-4,body.h-9);
    if(body.h>24) {
        list.h=(body.h-18)/2*2;
        waveform(sf,tui_rect_make(body.x+1,list.y+list.h+1,body.w-2,body.h-list.h-10));
    }
    int rows=list.h/2;if(rows<1)rows=1;
    int first=selected/rows*rows;
    if(!s.count)tui_put_str(sf,body,list.x,list.y,"WATCH groups repeats; pin useful patterns.",LS_ATTR_DIM);
    for(int i=0;i<rows && first+i<s.count;i++) {
        const rec_watch_event_t *e=&s.event[first+i];int y=list.y+i*2;
        if(first+i==selected)ls_fill_dither(sf,tui_rect_make(list.x,y,list.w,2),LS_DITHER_LIGHT,TUI_CYAN);
        snprintf(line,sizeof(line),"%s #%lu %.4fMHz x%lu",e->pinned?"[*]":"[ ]",(unsigned long)e->id,e->frequency/1e6,(unsigned long)e->count);
        tui_put_str(sf,list,list.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        snprintf(line,sizeof(line),"%u edges  %.1fms  %s",e->edges,e->span_us/1000.,rec_end_reason_name(e->end_reason));
        tui_put_str(sf,list,list.x,y+1,line,LS_ATTR_DIM);
    }
    if(body.h>12) {
        snprintf(line,sizeof(line),"DM queued %lu | refused %lu | limited %lu",(unsigned long)s.alert_sent,(unsigned long)s.alert_failed,(unsigned long)s.alert_suppressed);
        tui_put_str(sf,body,body.x+2,body.y+body.h-4,line,LS_ATTR_DIM);
        tui_put_str(sf,body,body.x+2,body.y+body.h-3,"One channel. Pattern match != device ID.",LS_ATTR_DIM);
        tui_put_str(sf,body,body.x+2,body.y+body.h-2,"E export pulses | J save to Journal",LS_ATTR_DIM);
    }
    ls_safe_line(sf,a,a.y+a.h-3,s.storage,LS_ATTR_DIM);
    ls_safe_line(sf,a,a.y+a.h-2,s.export_status[0]?s.export_status:"Other RTL modes stop WATCH; Mesh can stay on.",LS_ATTR_DIM);
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"W watch | E export | J journal",LS_ATTR_DIM);
}
static bool key(ls_tk_t k,char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if(k==LS_TK_LEFT || k==LS_TK_RIGHT || k==LS_TK_TAB)return ls_btn_navigate(k,&button_slot,&button_focus,false);
    if(k==LS_TK_ENTER && button_focus>=0){if(ls_btn_enabled(0,button_focus))action(button_focus);return true;}
    if(k==LS_TK_UP || k==LS_TK_DOWN || k==LS_TK_BACKSPACE)button_focus=-1;
    if(k==LS_TK_UP){if(selected)selected--;return true;}
    if(k==LS_TK_DOWN){if(selected+1<s.count)selected++;return true;}
    if(k!=LS_TK_CHAR || !ch)return false;
    if(ch==','){if(selected)selected--;return true;}if(ch=='.'){if(selected+1<s.count)selected++;return true;}
    const char *p=strchr("wfpaejbs",ch);if(!p)return false;action((int)(p-"wfpaejbs"));return true;
}
static bool touch(int x,int y)
{
    if(touch_nav){int i=ls_btn_hit_slot(x,y,LS_BTN_SLOT_WATERFALL);if(i>=0)return key(LS_TK_CHAR,i?'.':',');}
    int i=ls_btn_hit(x,y);if(i>=0){action(i);return true;}
    if(x>=list.x && x<list.x+list.w && y>=list.y && y<list.y+list.h) {
        int rows=list.h/2;if(rows<1)rows=1;
        int n=selected/rows*rows+(y-list.y)/2;if(n<s.count)selected=n;
    }
    return true;
}
const ls_tui_screen_t ls_scr_subghz={.radio="REC",.name="SUB-GHZ",.hint="W watch  F tune  E export  J journal",.enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch};
