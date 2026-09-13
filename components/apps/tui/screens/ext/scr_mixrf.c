#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_field.h"
#include "../../ls_picker.h"
#include "../../ls_motion.h"
#include "ls_mixrf.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int button_focus=-1, button_slot;
extern const ls_tui_screen_t ls_scr_nfc;
static bool card_suite;
static ls_mixrf_status_t state;
static uint32_t frequency=433920000;
static bool enabled;
static bool scan_enabled,view_24,view_nfc,nfc_enabled;
static float trace[48];
static uint32_t last_samples;
static char feedback[64];
static bool provider(ls_field_sample_t *out)
{
    ls_mixrf_status_t s;ls_mixrf_snapshot(&s);
    if(!s.receiving)return false;
    out->frequency=s.frequency;out->packets=0;
    out->rssi=s.rssi;out->snr=NAN;out->signal_valid=true;out->radio_valid=true;
    return true;
}
static bool provider_24(ls_field_sample_t *out)
{
    ls_mixrf_status_t s;ls_mixrf_snapshot(&s);
    if(!s.scanning || !s.sweeps)return false;
    out->frequency=2400000000u+(uint32_t)s.channel*1000000u;
    out->packets=0;out->rssi=out->snr=NAN;
    out->signal_valid=false;out->radio_valid=true;
    return true;
}
static bool provider_nfc(ls_field_sample_t *out)
{
    ls_mixrf_status_t s;ls_mixrf_snapshot(&s);
    if(!(s.nfc_watching && s.nfc_samples) && !(s.card_scanning && s.card_polls))return false;
    out->frequency=13560000;out->packets=0;out->rssi=out->snr=NAN;
    out->signal_valid=false;out->radio_valid=true;return true;
}
static void band_done(int i)
{
    static const uint32_t hz[]={315000000,433920000,868350000,915000000};
    if(i<0 || i>3)return;
    frequency=hz[i];
    if(enabled && !ls_mixrf_receive(true,frequency))snprintf(feedback,sizeof(feedback),"Receiver unavailable");
}
static void action(int i)
{
    feedback[0]=0;
    if(i==3 && view_nfc){card_suite=true;ls_scr_nfc.enter();return;}
    if(i==3 && (view_nfc?!((state.nfc_watching && state.nfc_samples) || (state.card_scanning && state.card_polls)):view_24?!(state.scanning && state.sweeps):!state.receiving))return;
    if(i==0)ls_mixrf_start();
    else if(i==1) {bool on=!enabled;if(ls_mixrf_receive(on,frequency))enabled=on;}
    else if(i==2) {
        ls_picker_open("CC1101 RECEIVE BAND",band_done);
        ls_picker_add("315 MHz","315 MHz RF path");ls_picker_add("433.920 MHz","434 MHz RF path");
        ls_picker_add("868.350 MHz","868/915 RF path");ls_picker_add("915 MHz","868/915 RF path");
    } else if(i==3 && view_nfc && state.card_scanning) {
        char text[192];snprintf(text,sizeof(text),"NFC-A detector: %s, last ATQA %04X; %lu polls, %lu valid responses. Card UID and sectors not read.",state.card_present?"card present":"no current response",state.card_atqa,(unsigned long)state.card_polls,(unsigned long)state.card_hits);
        bool ok=ls_field_mark_radio("NFC card observation",text,LS_FIELD_NFC,13560000,0);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy");
    } else if(i==3 && view_nfc) {
        char text[128];snprintf(text,sizeof(text),"Passive NFC field %s; %lu sampled arrivals.",state.nfc_field?"present":"absent",(unsigned long)state.nfc_events);
        ls_field_mark_radio("NFC field observation",text,LS_FIELD_NFC,13560000,0);
    } else if(i==3 && view_24 && state.sweeps) {
        unsigned peak=0;
        for(unsigned n=1;n<LS_MIXRF_CHANNELS;n++)if(state.occupancy[n]>state.occupancy[peak])peak=n;
        char text[192];snprintf(text,sizeof(text),"2.4 GHz energy survey: %lu sweeps, %lu threshold hits. "
            "Highest recent activity at %u MHz: %u%% of smoothed samples. RPD threshold about -64 dBm; no packets decoded.",
            (unsigned long)state.sweeps,(unsigned long)state.energy_hits,2400+peak,state.occupancy[peak]*100/255);
        bool ok=ls_field_mark_radio("2.4 GHz observation",text,LS_FIELD_NRF24,2400000000u+peak*1000000u,0);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy");
    } else if(i==3 && state.receiving) {
        char text[160];snprintf(text,sizeof(text),"CC1101 channel energy at %.4f MHz: approximately %.1f dBm. "
            "Receiver estimate, not calibrated; no packet decoded.",state.frequency/1e6,state.rssi);
        bool ok=ls_field_mark_radio("CC1101 observation",text,LS_FIELD_CC1101,state.frequency,0);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy");
    } else if(i==4) {bool on=!scan_enabled;if(ls_mixrf_scan(on)){scan_enabled=on;view_24=true;view_nfc=false;}}
    else if(i==5){if(view_nfc)view_nfc=false;else if(view_24){view_24=false;view_nfc=true;}else view_24=true;}
    else if(i==6){bool on=!nfc_enabled;if(ls_mixrf_card_scan(on)){nfc_enabled=on;view_nfc=true;view_24=false;}}
    else if(i==7){ls_mixrf_receive(false,frequency);ls_mixrf_scan(false);ls_mixrf_nfc_watch(false);ls_mixrf_card_scan(false);enabled=scan_enabled=nfc_enabled=false;}
}
static void enter(void)
{
    button_focus=-1;button_slot=0;
    ls_field_start();ls_field_watch(true);ls_field_provider(LS_FIELD_CC1101,provider);
    ls_field_provider(LS_FIELD_NRF24,provider_24);ls_field_provider(LS_FIELD_NFC,provider_nfc);ls_mixrf_start();
    feedback[0]=0;
    for(int i=0;i<48;i++)trace[i]=-120;
}
static void leave(void){ls_field_watch(false);}
static void draw(tui_surface *sf,tui_rect a)
{
    if(card_suite){ls_scr_nfc.draw(sf,a);return;}
    if(a.w<30 || a.h<22){ls_panel_notice(sf,a,"MIX-RF","Enlarge the pane","Receive monitor keeps its state");return;}
    ls_mixrf_snapshot(&state);
    enabled=state.receive_requested;scan_enabled=state.scan_requested;nfc_enabled=state.card_requested;
    if(state.samples!=last_samples) {
        for(int i=0;i<47;i++)trace[i]=trace[i+1];
        trace[47]=state.receiving?state.rssi:-120;last_samples=state.samples;
    }
    ls_btn_t buttons[]={{"PROBE","RADIOS",'p',false,state.busy || enabled || scan_enabled || nfc_enabled || state.nfc_requested},
        {"MONITOR",enabled?"ON":"OFF",'m',enabled,!state.cc},
        {"BAND","CC1101",'b',false,false},{view_nfc?"CARDS":"MARK",view_nfc?"SUITE":"NOTE",'j',false,view_nfc?false:view_24?!(state.scanning && state.sweeps):!state.receiving},
        {"2.4 SCAN",scan_enabled?"ON":"OFF",'s',scan_enabled,!state.nrf || state.busy},
        {"VIEW",view_nfc?"NFC":view_24?"2.4 GHz":"SUB-GHZ",'v',view_24||view_nfc,false},
        {"NFC",nfc_enabled?"SCAN":"OFF",'n',nfc_enabled,!state.nfc || state.busy},
        {"STOP","ALL",'x',false,!(enabled || scan_enabled || nfc_enabled || state.nfc_requested)}};
    int h=ls_btn_raised_height(a,8);
    ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,h),buttons,8,button_focus);
    tui_rect panel=tui_rect_make(a.x,a.y+h,a.w,a.h-h-2);
    ls_panel_box(sf,panel,"KEYBOARD RADIOS / RECEIVE",TUI_CYAN);
    ls_motion_busy(sf,panel,state.busy || state.receiving || state.scanning || state.card_scanning);
    tui_rect chart=panel;
    bool wide=panel.w>=90;
    if(wide) { panel.w=panel.w/2; chart.x=panel.x+panel.w;chart.w-=panel.w;ls_panel_box(sf,chart,view_nfc?"NFC / RECEIVE HISTORY":view_24?"2.4 GHz ACTIVITY":"CHANNEL ENERGY",TUI_CYAN); }
    char line[80];
    snprintf(line,sizeof(line),"%s / enable %s",state.keyboard?"detected":"absent",state.power?"on":"off");
    ls_kv(sf,panel,1,"KEYBOARD",line,LS_ATTR_DIM);
    snprintf(line,sizeof(line),"%s v%02X",state.cc?"identified":"unavailable",state.cc_version);
    ls_kv(sf,panel,3,"CC1101",line,LS_ATTR_DIM);
    ls_kv(sf,panel,4,"NRF24",state.nrf?"register check passed":"unavailable",LS_ATTR_DIM);
    snprintf(line,sizeof(line),"%s ID %02X",state.nfc?"identified":"unavailable",state.nfc_identity);
    ls_kv(sf,panel,5,"NFC",line,LS_ATTR_DIM);
    if(view_nfc)snprintf(line,sizeof(line),"%lu polls / %lu replies",(unsigned long)state.card_polls,(unsigned long)state.card_hits);
    else if(view_24)snprintf(line,sizeof(line),"%lu sweeps / %lu hits",(unsigned long)state.sweeps,(unsigned long)state.energy_hits);
    else snprintf(line,sizeof(line),"%.4f MHz",frequency/1e6);
    ls_kv(sf,panel,7,view_nfc?"13.56 MHz":view_24?"SURVEY":"MONITOR",line,LS_ATTR_DIM);
    if(view_nfc && (state.card_requested || state.card_polls))snprintf(line,sizeof(line),"%s / ATQA %04X",state.card_present?"CARD PRESENT":state.card_scanning?"waiting for card":"stopped",state.card_atqa);
    else if(view_nfc)snprintf(line,sizeof(line),"%s",state.nfc_watching?(state.nfc_field?"FIELD PRESENT":"waiting for reader field"):"off");
    else if(view_24)snprintf(line,sizeof(line),"%s / %u MHz",state.scanning?"listening":"off",2400+state.channel);
    else if(state.receiving)snprintf(line,sizeof(line),"%c ~%.1f dBm",ls_motion_pip(true),state.rssi);
    else snprintf(line,sizeof(line),"%s",enabled?"starting / no fresh sample":"off");
    ls_kv(sf,panel,8,"ENERGY",line,LS_ATTR_DIM);
    if(view_nfc && chart.h>(wide?13:23)) {
        int top=chart.y+(wide?2:10),bottom=chart.y+chart.h-5;
        const char *banner=state.card_present?"((( CARD DETECTED )))":state.card_scanning?"[ MOVE CARD SLOWLY / HOLD 1 SECOND ]":state.nfc_field?"[ EXTERNAL FIELD PRESENT ]":"[ NFC DETECTOR OFF ]";
        tui_put_str(sf,chart,chart.x+2,top,banner,state.card_present?TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
        tui_put_str(sf,chart,chart.x+2,top+1,"RX level 0 <--------------------> 15",LS_ATTR_DIM);
        int rows=bottom-top-2;
        if(rows>LS_MIXRF_CARD_HISTORY)rows=LS_MIXRF_CARD_HISTORY;
        int width=chart.w-8;
        for(int age=0;age<rows;age++) {
            int y=bottom-age;
            tui_put_char(sf,chart,chart.x+2,y,age?'|':'>',LS_ATTR_DIM);
            if(age>=state.card_count)continue;
            int i=(state.card_head+LS_MIXRF_CARD_HISTORY-1-age)%LS_MIXRF_CARD_HISTORY;
            unsigned result=state.card_result[i];
            uint8_t color=result==3?TUI_GREEN:result==2?TUI_WHITE:result==1?TUI_YELLOW:TUI_CYAN;
            int fill=state.card_level[i]*width/15;
            for(int x=0;x<width;x++)
                tui_put_char(sf,chart,chart.x+4+x,y,x<fill?(age<3?'#':':'):'.',x<fill?TUI_ATTR(color|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
            tui_put_char(sf,chart,chart.x+chart.w-3,y,result==3?'C':result==2?'R':result==1?'!':'-',TUI_ATTR(color|TUI_BRIGHT,TUI_BLACK));
        }
        tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-3,"! noise  R reply  C confirmed / newest below",LS_ATTR_DIM);
        tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-2,"13.56 MHz RX history / uncalibrated level",LS_ATTR_DIM);
    } else if(chart.h>(wide?13:20)) {
        int bottom=chart.y+chart.h-5,height=wide?chart.h-7:chart.h-16;
        for(int x=0;x<chart.w-4;x++) {
            int n=x*(view_24?LS_MIXRF_CHANNELS:48)/(chart.w-4);
            float level=view_24?state.occupancy[n]/255.0f:(trace[n]+120)/100;
            if(view_24) {
                int end=(x+1)*LS_MIXRF_CHANNELS/(chart.w-4);
                for(int bin=n+1;bin<end;bin++)
                    if(state.occupancy[bin]/255.0f>level)level=state.occupancy[bin]/255.0f;
            }
            if(level<0)level=0;
            if(level>1)level=1;
            int y=bottom-(int)(level*height);
            if(view_24)for(int row=y;row<=bottom;row++)
                tui_put_char(sf,chart,chart.x+2+x,row,'|',TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
            else tui_put_char(sf,chart,chart.x+2+x,y,'*',TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK));
        }
        tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-3,view_24?"2400 MHz <--- activity ---> 2483 MHz":"CC1101 energy; packets not decoded",LS_ATTR_DIM);
        tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-2,view_24?"RPD ~-64 dBm threshold; no packet IDs":"V switches survey / NFC field views",LS_ATTR_DIM);
    }
    ls_safe_line(sf,a,a.y+a.h-2,state.status,LS_ATTR_DIM);
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:"M RX | S scan | N NFC | V view | X stop",LS_ATTR_DIM);
}
static bool key(ls_tk_t k,char c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if(card_suite){if(k==LS_TK_ESC){card_suite=false;return true;}return ls_scr_nfc.key(k,c);}
    if(k==LS_TK_CHAR && c=='c'){card_suite=true;ls_scr_nfc.enter();return true;}
    if(ls_btn_navigate(k,&button_slot,&button_focus,false))return true;
    if(k==LS_TK_ENTER){if(ls_btn_enabled(0,button_focus))action(button_focus);return true;}
    if(k!=LS_TK_CHAR)return false;
    if(c=='f'){ls_mixrf_nfc_watch(!state.nfc_requested);view_nfc=true;view_24=false;return true;}
    const char *p=strchr("pmbjsvnx",c);
    if(!c || !p)return false;
    action((int)(p-"pmbjsvnx"));return true;
}
static bool touch(int x,int y){if(card_suite){if(!ls_scr_nfc.touch(x,y))card_suite=false;return true;}int i=ls_btn_hit(x,y);if(i>=0)action(i);return true;}
const ls_tui_screen_t ls_scr_mixrf={.name="MIX-RF",.hint="M monitor  B band  C NFC cards",.enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch};
