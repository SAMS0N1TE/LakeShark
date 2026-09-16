#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_numpad.h"
#include "../../ls_picker.h"
#include "../../ls_motion.h"
#include "../../ls_field.h"
#include "rec_state.h"
#include "rec_watch.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
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
static tui_rect pulse_hit;
static int pulse_selected;
static uint64_t pulse_total;
static bool details;
static ls_fresh_t arrivals;
static int setup_item;
static uint32_t source_frequency[2]={433920000,433920000};
extern void ls_scr_rec_tools(void);
static const char *source_name(void) { return rec_watch_source()==REC_SOURCE_CC1101?"CC1101":"RTL"; }
static const double bands[]={152.600,154.785,315.000,433.920,868.350,915.000};
static void tune(double mhz)
{
    if(rec_watch_source()==REC_SOURCE_CC1101 && !((mhz>=300 && mhz<=348)||(mhz>=387 && mhz<=464)||(mhz>=779 && mhz<=928))) {
        snprintf(feedback,sizeof(feedback),"CC1101: 300-348 / 387-464 / 779-928 MHz");return;
    }
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
        if(rec_watch_enable(on)) {if(on && rec_watch_source()==REC_SOURCE_RTL){ls_tui_radio_want("REC");rec_arm_request();}else if(!on)rec_disarm();}
        else {rec_watch_snapshot(&s);snprintf(feedback,sizeof(feedback),"%s",!s.ready?"Loading SD history; try WATCH when it appears":"Check source, PROBE and frequency; stop MIX-RF monitor");}
    } else if(i==10) {details=!details;
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
        snprintf(text,sizeof(text),"OOK timing pattern; not a decoded device identity. "
            "%.4f MHz, %u edges, %.2f ms, %lu observations. "
            "First: boot %08lx uptime %llu ms. Last: boot %08lx uptime %llu ms. "
            "GPS and motion attachment describe this bookmark, not the original capture. "
            "Export the representative from SUB-GHZ for pulse data.",e->frequency/1e6,e->edges,e->span_us/1000.,
            (unsigned long)e->count,(unsigned long)e->first_boot,(unsigned long long)e->first_ms,
            (unsigned long)e->last_boot,(unsigned long long)e->last_ms);
        size_t used=strlen(text);
        if(s.decoded[selected].repeats)snprintf(text+used,sizeof(text)-used," OOK24 payload %06lX, %u matching frames.",(unsigned long)s.decoded[selected].value,s.decoded[selected].repeats);
        bool ok=ls_field_mark_radio(title,text,e->source==REC_SOURCE_CC1101?LS_FIELD_CC1101:LS_FIELD_RTL,e->frequency,e->count);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy; try again shortly");
    } else if(i==6) {
        ls_picker_open("RECEIVE FREQUENCY",band_done);
        for(int j=0;j<6;j++) {
            char label[32];snprintf(label,sizeof(label),"%.4f MHz",bands[j]);
            ls_picker_add(label,j<2?"OOK only; no FSK":"Receive preset");
        }
    } else if(i==8) {
        if(rec_watch_enabled()){snprintf(feedback,sizeof(feedback),"Stop WATCH before changing source");return;}
        source_frequency[rec_watch_source()]=rec_get_freq();
        rec_disarm();
        rec_source_t next=rec_watch_source()==REC_SOURCE_RTL?REC_SOURCE_CC1101:REC_SOURCE_RTL;
        if(rec_watch_select_source(next)) {
            rec_set_freq(source_frequency[next]);
            snprintf(feedback,sizeof(feedback),"%s selected; WATCH starts capture",source_name());
        }
    } else if(i==9) {if(rec_watch_source()==REC_SOURCE_RTL && !rec_watch_enabled())ls_scr_rec_tools();
    } else if(i==7) {
        if(rec_watch_source()==REC_SOURCE_CC1101){snprintf(feedback,sizeof(feedback),"CC: OOK 650kHz, 40us filter, 30ms gap, 1024 edges");return;}
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
    ls_panel_box(sf,a,"PULSE INSPECTOR",TUI_CYAN);
    pulse_hit=tui_rect_make(0,0,0,0);pulse_total=0;
    if(selected>=s.count || a.h<9)return;
    const rec_watch_event_t *e=&s.event[selected];
    int n=e->edges;if(n>48)n=48;
    uint32_t low=UINT32_MAX,high=0;
    for(int i=0;i<n;i++) {
        uint32_t d=s.preview[selected][i]<0?-(int64_t)s.preview[selected][i]:s.preview[selected][i];
        pulse_total+=d;if(d<low)low=d;if(d>high)high=d;
    }
    if(!pulse_total)return;
    if(pulse_selected>=n)pulse_selected=0;
    char text[110];
    snprintf(text,sizeof(text),"%s #%lu / %s",e->source==REC_SOURCE_CC1101?"CC1101":"RTL",(unsigned long)e->id,
       e->end_reason==REC_END_SPAN || e->end_reason==REC_END_EDGES?"TRUNCATED":s.decoded[selected].repeats?"OOK24 VERIFIED":e->count>1?"REPEAT MATCH":"RAW CANDIDATE");
    tui_put_str(sf,a,a.x+2,a.y+1,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    int w=a.w-10;if(w<2)return;
    pulse_hit=tui_rect_make(a.x+7,a.y+2,w,4);
    tui_put_str(sf,a,a.x+1,a.y+2,"HIGH",LS_ATTR_DIM);
    tui_put_str(sf,a,a.x+1,a.y+4,"LOW",LS_ATTR_DIM);
    uint64_t end=0;int edge=0,previous=-1,previous_edge=0;
    for(int x=0;x<w;x++) {
        uint64_t t=(uint64_t)x*pulse_total/w;
        while(edge<n-1 && end+(uint64_t)(s.preview[selected][edge]<0?-(int64_t)s.preview[selected][edge]:s.preview[selected][edge])<=t) {
            end+=s.preview[selected][edge]<0?-(int64_t)s.preview[selected][edge]:s.preview[selected][edge];edge++;
        }
        int y=a.y+(s.preview[selected][edge]>0?2:4);
        uint8_t color=TUI_ATTR((edge==pulse_selected?TUI_YELLOW:TUI_GREEN)|TUI_BRIGHT,TUI_BLACK);
        if(previous>=0 && (previous!=y || edge!=previous_edge)) {
            tui_put_char(sf,a,pulse_hit.x+x,a.y+2,'+',color);
            tui_put_char(sf,a,pulse_hit.x+x,a.y+3,'|',color);
            tui_put_char(sf,a,pulse_hit.x+x,a.y+4,'+',color);
        } else tui_put_char(sf,a,pulse_hit.x+x,y,'-',color);
        previous=y;previous_edge=edge;
    }
    snprintf(text,sizeof(text),"0 -> %.2f ms | first %d/%u edges",pulse_total/1000.,n,e->edges);
    tui_put_str(sf,a,a.x+2,a.y+6,text,LS_ATTR_DIM);
    snprintf(text,sizeof(text),"Tap trace: #%d %s %lu us",pulse_selected+1,s.preview[selected][pulse_selected]>0?"HIGH":"LOW",(unsigned long)(s.preview[selected][pulse_selected]<0?-(int64_t)s.preview[selected][pulse_selected]:s.preview[selected][pulse_selected]));
    tui_put_str(sf,a,a.x+2,a.y+7,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    if(a.h>10) {
        snprintf(text,sizeof(text),"Range %lu..%lu us / %s",(unsigned long)low,(unsigned long)high,e->end_reason==REC_END_GAP?"GAP end":"LIMIT end");
        tui_put_str(sf,a,a.x+2,a.y+8,text,LS_ATTR_DIM);
    }
    if(a.h>12)tui_put_str(sf,a,a.x+2,a.y+10,"| = transition(s) within one time cell",LS_ATTR_DIM);
    if(a.h>11 && s.decoded[selected].repeats) {
        snprintf(text,sizeof(text),"OOK24 %06lX / %u frames / %u us unit",(unsigned long)s.decoded[selected].value,s.decoded[selected].repeats,s.decoded[selected].unit_us);
        tui_put_str(sf,a,a.x+2,a.y+9,text,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
}
static void draw(tui_surface *sf,tui_rect a)
{
    if(a.w<24 || a.h<17) {ls_panel_notice(sf,a,"SUB-GHZ","Enlarge the pane","WATCH keeps its state");return;}
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
    ls_mixrf_status_t cc;ls_mixrf_snapshot(&cc);
    bool cc_source=rec_watch_source()==REC_SOURCE_CC1101;
    if(cc_source){rx.freq_hz=rec_get_freq();rx.receiver_streaming=cc.capturing && cc.receiving;}
    ls_btn_t btn[]={{"WATCH",s.enabled?"ON":"OFF",'w',s.enabled,!s.ready},
        {"TUNE","MHz",'f',false,s.enabled},
        {"PIN",selected<s.count && s.event[selected].pinned?"KEPT":"KEEP",'p',selected<s.count && s.event[selected].pinned,!s.count},
        {"ALERTS",s.alerts?"DM":"OFF",'a',s.alerts,false},
        {"EXPORT",".SUB",'e',false,!s.count || s.exporting},
        {"JOURNAL","MARK",'j',false,!s.count},
        {"BANDS","PRESET",'b',false,s.enabled},
        {"SETUP","CAPTURE",'s',false,s.enabled},
        {"SOURCE",source_name(),'r',false,s.enabled},
        {"TOOLS","RTL",'d',false,cc_source || s.enabled},{"DETAILS",details?"ON":"OFF",'i',details,false}};
    int h=a.w>90?4:ls_btn_raised_height(a,11);
    /* Four rows of buttons, each with separate label/value lines. The
       three-row key format clips WATCH OFF to WATCH OF at the large font. */
    if(a.w>=32 && a.w<38 && a.h>=36) h=16;
    if(a.h<22 && a.w>=72) h=6;
    ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,h),btn,11,button_focus);
    tui_rect body=tui_rect_make(a.x,a.y+h,a.w,a.h-h-1);
    tui_rect content=tui_rect_make(body.x+1,body.y+1,body.w-2,body.h-2);
    ls_panel_box(sf,body,cc_source?"PASSIVE WATCH / CC1101 OOK":"PASSIVE WATCH / RTL OOK",TUI_CYAN);
    ls_motion_busy(sf,body,s.exporting || (s.enabled && rx.receiver_streaming));
    char line[110];
    snprintf(line,sizeof(line),"%c %.4f MHz | %s",ls_motion_pip(s.enabled && rx.receiver_streaming),
        rx.freq_hz/1e6,!s.enabled?"STOPPED":rx.receiver_streaming?"LISTENING":"RX UNAVAILABLE");
    tui_put_str(sf,content,body.x+2,body.y+1,line,LS_ATTR_DIM);
    snprintf(line,sizeof(line),"HEALTH %s | %s | RX %lu LOST %lu",
        (s.dropped || (cc_source && cc.raw_overflows))?"LOSS":s.enabled&&!rx.receiver_streaming?"NO RX":"OK",
        s.save_failed?"SD FAIL":s.pending_save?"PENDING":s.saved?"SAVED":"RAM",
        (unsigned long)s.received,(unsigned long)(s.dropped+(cc_source?cc.raw_overflows:0)));
    tui_put_str(sf,content,body.x+2,body.y+2,line,TUI_ATTR((s.dropped||s.save_failed||s.pending_save||!s.saved?TUI_YELLOW:TUI_GREEN)|TUI_BRIGHT,TUI_BLACK));
    if(s.count) {
        const rec_watch_event_t *e=&s.event[selected];
        snprintf(line,sizeof(line),"LAST up %.3fs",e->last_ms/1000.0);
    } else snprintf(line,sizeof(line),"LAST -- / no captures");
    tui_put_str(sf,content,body.x+2,body.y+3,line,LS_ATTR_DIM);
    list=tui_rect_make(body.x+2,body.y+4,body.w-4,body.h-6);
    if(body.h<12) list.h=body.h-6;
    if(body.w>90 && body.h>12) {
        list.w=(body.w-6)/2;
        waveform(sf,tui_rect_make(list.x+list.w+2,body.y+3,body.w-list.w-5,body.h-4));
    } else if(body.h>24) {
        list.h=(body.h-18)/2*2;
        waveform(sf,tui_rect_make(body.x+1,list.y+list.h+1,body.w-2,body.h-list.h-10));
    }
    int rows=list.h/2;if(rows<1)rows=1;
    int first=selected/rows*rows;
    if(!s.count)tui_put_str(sf,content,list.x,list.y,"No patterns for this receiver. Start WATCH.",LS_ATTR_DIM);
    for(int i=0;i<rows && first+i<s.count;i++) {
        const rec_watch_event_t *e=&s.event[first+i];int y=list.y+i*2;
        if(first+i==selected)ls_fill_dither(sf,tui_rect_make(list.x,y,list.w,2),LS_DITHER_LIGHT,TUI_CYAN);
        snprintf(line,sizeof(line),"%s #%lu %.4fMHz x%lu",e->last_boot==s.boot_id?"[RX]":"[SD]",(unsigned long)e->id,e->frequency/1e6,(unsigned long)e->count);
        tui_put_str(sf,list,list.x,y,line,TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
        if(s.decoded[first+i].repeats)snprintf(line,sizeof(line),"%s OOK24 %06lX / %u repeats",e->source==REC_SOURCE_CC1101?"CC":"RTL",(unsigned long)s.decoded[first+i].value,s.decoded[first+i].repeats);
        else snprintf(line,sizeof(line),"%s %u edges %.1fms",e->end_reason!=REC_END_GAP?"LIMIT":e->count>1?"REPEAT":"RAW?",e->edges,e->span_us/1000.);
        tui_put_str(sf,list,list.x,y+1,line,LS_ATTR_DIM);
    }
    if(details) {
        tui_rect info=tui_rect_make(body.x+1,body.y+3,body.w-2,body.h-4);
        tui_fill(sf,info,' ',LS_ATTR_DIM);
        ls_panel_box(sf,info,"CAPTURE DETAILS",TUI_CYAN);
        tui_put_str(sf,info,info.x+2,info.y+1,s.storage,LS_ATTR_DIM);
        snprintf(line,sizeof(line),"Loss: %lu skipped / CC buffer overflows %lu",(unsigned long)s.dropped,(unsigned long)cc.raw_overflows);
        tui_put_str(sf,info,info.x+2,info.y+2,line,LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+3,"RAW? unverified / REPEAT timing match",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+4,"[SD] earlier boot / [RX] this boot",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+5,"Pattern match is not a device identity.",LS_ATTR_DIM);
        tui_put_str(sf,info,info.x+2,info.y+6,cc_source?"CC: 650kHz OOK / 40us filter / 30ms gap":"RTL: configurable OOK pulse capture",LS_ATTR_DIM);
        snprintf(line,sizeof(line),"DM queued %lu / refused %lu / limited %lu",(unsigned long)s.alert_sent,(unsigned long)s.alert_failed,(unsigned long)s.alert_suppressed);
        tui_put_str(sf,info,info.x+2,info.y+7,line,LS_ATTR_DIM);
    }
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:s.export_status,LS_ATTR_DIM);

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
    const char *keys="wfpaejbsrdi";
    const char *p=strchr(keys,ch);if(!p)return false;action((int)(p-keys));return true;
}
static bool touch(int x,int y)
{
    if(touch_nav){int i=ls_btn_hit_slot(x,y,LS_BTN_SLOT_WATERFALL);if(i>=0)return key(LS_TK_CHAR,i?'.':',');}
    int i=ls_btn_hit(x,y);if(i>=0){action(i);return true;}
    if(!details && pulse_total && tui_rect_contains(pulse_hit,x,y)) {
        uint64_t t=(uint64_t)(x-pulse_hit.x)*pulse_total/pulse_hit.w,sum=0;
        int n=s.event[selected].edges;if(n>48)n=48;
        for(int j=0;j<n;j++) {sum+=s.preview[selected][j]<0?-(int64_t)s.preview[selected][j]:s.preview[selected][j];if(sum>t){pulse_selected=j;break;}}
        return true;
    }
    if(x>=list.x && x<list.x+list.w && y>=list.y && y<list.y+list.h) {
        int rows=list.h/2;if(rows<1)rows=1;
        int n=selected/rows*rows+(y-list.y)/2;if(n<s.count)selected=n;
    }
    return true;
}
const ls_tui_screen_t ls_scr_subghz={.radio="REC",.name="SUB-GHZ",.hint="W watch  F tune  E export  J journal",.enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch};
