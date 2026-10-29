#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_field.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_motion.h"
#include "ls_mixrf.h"
#include "ls_mixrf_control.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ls_textpack.h"

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
static char hint[96];
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
    ls_mixrf_snapshot(&state);
    if(state.cc_owner!=LS_MIXRF_OWNER_NONE && state.cc_owner!=LS_MIXRF_OWNER_MONITOR) {
        snprintf(feedback,sizeof(feedback),"%s",ls_mixrf_cc_refusal(&state));return;
    }
    if(enabled && !ls_mixrf_receive(true,hz[i])) {
        ls_mixrf_snapshot(&state);snprintf(feedback,sizeof(feedback),"%s",ls_mixrf_cc_refusal(&state));return;
    }
    frequency=hz[i];
}
/* OPTIONS holds the CC1101 receive band. */
static const char *const BAND_NAME[]={"315 MHz","433.920 MHz","868.350 MHz","915 MHz"};
static const uint32_t BAND_HZ[]={315000000,433920000,868350000,915000000};
static int o_band(const ls_opt_t *o){(void)o;for(int i=0;i<4;i++)if(BAND_HZ[i]==frequency)return i;return -1;}
static void o_set_band(const ls_opt_t *o,int v){(void)o;band_done(v);}
static void o_band_show(const ls_opt_t *o,char *out,size_t n){(void)o;snprintf(out,n,"%.3f MHz",frequency/1e6);}
static const ls_opt_t OPT_CC1101[]={
    {.label="RECEIVE BAND",.kind=LS_OPT_CYCLE,.names=BAND_NAME,.n=4,.get=o_band,.set=o_set_band,.show=o_band_show}};
static const ls_opt_ctx_t CTX_CC1101={.name="MONITOR",.job=-1,.radio=LS_RSEL_CC1101,LS_OPT_ROWS(OPT_CC1101)};

/* RADIO picks which of the board's three radios the view shows. */
static ls_rsel_radio_t viewed(void) { return view_nfc?LS_RSEL_NFC:view_24?LS_RSEL_NRF24:LS_RSEL_CC1101; }
static const ls_opt_ctx_t *mixrf_options(void) { return viewed()==LS_RSEL_CC1101?&CTX_CC1101:NULL; }
static void radio_chosen(ls_rsel_radio_t r)
{
    view_24=r==LS_RSEL_NRF24;
    view_nfc=r==LS_RSEL_NFC;
}
static void cards_open(void)
{
    ls_mixrf_snapshot(&state);
    if(!state.nfc || state.busy){LS_SNPUTS(t_scr_mixrf_nfc_reader_unavailable_use, feedback, sizeof(feedback), "NFC reader unavailable; use PROBE");return;}
    card_suite=true;ls_scr_nfc.enter();
    LS_SNPUTS(t_scr_mixrf_i_scan_r_read, hint, sizeof(hint), "I scan  R read  A/B keys  S save  J mark  Esc back");
}
static void nfc_mode(int i)
{
    ls_mixrf_card_scan(false);ls_mixrf_nfc_watch(false);
    if(i==1)ls_mixrf_card_scan(true);
    if(i==2)ls_mixrf_nfc_watch(true);
    radio_chosen(LS_RSEL_NFC);ls_rsel_set(LS_RSEL_MIXRF,LS_RSEL_NFC);
}
static void action(int i)
{
    feedback[0]=0;
    if(i==3 && (view_nfc?!((state.nfc_watching && state.nfc_samples) || (state.card_scanning && state.card_polls)):view_24?!(state.scanning && state.sweeps):!state.receiving))return;
    if(i==0)ls_mixrf_start();
    else if(i==1) {bool on=!enabled;if(ls_mixrf_receive(on,frequency))enabled=on;
        else {ls_mixrf_snapshot(&state);snprintf(feedback,sizeof(feedback),"%s",ls_mixrf_cc_refusal(&state));}}
    else if(i==2) {cards_open();}
    else if(i==3 && view_nfc && state.card_scanning) {
        char text[192];LS_SNPRINTF(t_scr_mixrf_nfc_a_detector_s, text,sizeof(text),"NFC-A detector: %s, last ATQA %04X; %lu polls, %lu valid responses. Card UID and sectors not read.",state.card_present?"card present":"no current response",state.card_atqa,(unsigned long)state.card_polls,(unsigned long)state.card_hits);
        bool ok=ls_field_mark_radio(LS_TEXT(t_scr_mixrf_nfc_card_observation, "NFC card observation"),text,LS_FIELD_NFC,13560000,0);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy");
    } else if(i==3 && view_nfc) {
        char text[128];LS_SNPRINTF(t_scr_mixrf_passive_nfc_field_s, text,sizeof(text),"Passive NFC field %s; %lu sampled arrivals.",state.nfc_field?"present":"absent",(unsigned long)state.nfc_events);
        bool ok=ls_field_mark_radio(LS_TEXT(t_scr_mixrf_nfc_field_observation, "NFC field observation"),text,LS_FIELD_NFC,13560000,0);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy");
    } else if(i==3 && view_24 && state.sweeps) {
        unsigned peak=0;
        for(unsigned n=1;n<LS_MIXRF_CHANNELS;n++)if(state.occupancy[n]>state.occupancy[peak])peak=n;
        char text[192];LS_SNPRINTF(t_scr_mixrf_2_4_ghz_energy, text,sizeof(text),"2.4 GHz energy survey: %lu sweeps, %lu threshold hits. "
            "Highest recent activity at %u MHz: %u%% of smoothed samples. RPD threshold about -64 dBm; no packets decoded.",
            (unsigned long)state.sweeps,(unsigned long)state.energy_hits,2400+peak,state.occupancy[peak]*100/255);
        bool ok=ls_field_mark_radio("2.4 GHz observation",text,LS_FIELD_NRF24,2400000000u+peak*1000000u,0);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy");
    } else if(i==3 && state.receiving) {
        char text[160];LS_SNPRINTF(t_scr_mixrf_cc1101_channel_energy_at, text,sizeof(text),"CC1101 channel energy at %.4f MHz: approximately %.1f dBm. "
            "Receiver estimate, not calibrated; no packet decoded.",state.frequency/1e6,state.rssi);
        bool ok=ls_field_mark_radio("CC1101 observation",text,LS_FIELD_CC1101,state.frequency,0);
        snprintf(feedback,sizeof(feedback),"%s",ok?"Note queued; check Journal save status":"Journal busy");
    } else if(i==4) {bool on=!scan_enabled;if(ls_mixrf_scan(on)){scan_enabled=on;view_24=true;view_nfc=false;}
        else {ls_mixrf_snapshot(&state);snprintf(feedback,sizeof(feedback),state.scan_owner!=LS_MIXRF_OWNER_NONE?"nRF24 in use by %s":"nRF24 unavailable",ls_mixrf_owner_name(state.scan_owner));}}
    else if(i==5){ls_rsel_radio_t r=view_nfc?LS_RSEL_CC1101:view_24?LS_RSEL_NFC:LS_RSEL_NRF24;radio_chosen(r);ls_rsel_set(LS_RSEL_MIXRF,r);}
    else if(i==6){ls_picker_open(LS_TEXT(t_scr_mixrf_nfc_observation_mode, "NFC OBSERVATION MODE"),nfc_mode);
        ls_picker_add("OFF","No NFC observation");ls_picker_add("SCAN",LS_TEXT(t_scr_mixrf_active_nfc_a_card, "Active NFC-A card requests"));
        ls_picker_add("FIELD",LS_TEXT(t_scr_mixrf_passive_external_reader_field, "Passive external reader field"));
        ls_picker_select(state.nfc_requested?2:state.card_requested?1:0);}
    else if(i==7){ls_mixrf_receive(false,frequency);ls_mixrf_scan(false);ls_mixrf_nfc_watch(false);ls_mixrf_card_scan(false);enabled=scan_enabled=nfc_enabled=false;}
    else if(i==8)ls_rsel_open(LS_RSEL_MIXRF,radio_chosen);
    else if(i==9)ls_opt_open(mixrf_options());
}
static void enter(void)
{
    button_focus=-1;button_slot=0;card_suite=false;
    radio_chosen(ls_rsel_get(LS_RSEL_MIXRF));
    LS_SNPUTS(t_scr_mixrf_m_monitor_r_radio, hint, sizeof(hint), "M monitor  R radio  O options  C cards  J mark  N NFC mode");
    ls_rsel_track(LS_RSEL_MIXRF,viewed);
    ls_field_start();ls_field_watch(true);ls_field_provider(LS_FIELD_CC1101,provider);
    ls_field_provider(LS_FIELD_NRF24,provider_24);ls_field_provider(LS_FIELD_NFC,provider_nfc);ls_mixrf_start();
    feedback[0]=0;
    for(int i=0;i<48;i++)trace[i]=NAN;
    last_samples=0;
}
static void leave(void){card_suite=false;ls_field_watch(false);}
static void draw(tui_surface *sf,tui_rect a)
{
    if(card_suite){ls_scr_nfc.draw(sf,a);return;}
    if(a.w<30 || a.h<17){ls_panel_notice(sf,a,"MIX-RF","Enlarge the pane",LS_TEXT(t_scr_mixrf_receive_monitor_keeps_its, "Receive monitor keeps its state"));return;}
    LS_SNPUTS(t_scr_mixrf_m_monitor_r_radio, hint, sizeof(hint), "M monitor  R radio  O options  C cards  J mark  N NFC mode");
    ls_mixrf_snapshot(&state);
    enabled=state.receive_requested && state.cc_owner==LS_MIXRF_OWNER_MONITOR;scan_enabled=state.scan_requested && state.scan_owner==LS_MIXRF_OWNER_MONITOR;nfc_enabled=state.card_requested;
    if(state.samples!=last_samples) {
        for(int i=0;i<47;i++)trace[i]=trace[i+1];
        trace[47]=state.receiving?state.rssi:NAN;last_samples=state.samples;
    }
    bool held=state.cc_owner!=LS_MIXRF_OWNER_NONE && state.cc_owner!=LS_MIXRF_OWNER_MONITOR;
    ls_btn_t buttons[]={{"PROBE","RADIOS",'p',false,state.busy || state.receive_requested || state.scan_requested || nfc_enabled || state.nfc_requested},
        {"MONITOR",enabled?"ON":"OFF",'m',enabled,!state.cc || held},
        {"CARDS","NFC",'c',false,!state.nfc || state.busy},{"MARK","NOTE",'j',false,view_nfc?!((state.nfc_watching && state.nfc_samples) || (state.card_scanning && state.card_polls)):view_24?!(state.scanning && state.sweeps):!state.receiving},
        {"2.4 SCAN",scan_enabled?"ON":"OFF",'s',scan_enabled,!state.nrf || state.busy},
        {"VIEW",view_nfc?"NFC":view_24?"2.4 GHz":"SUB-GHZ",'v',view_24||view_nfc,false},
        {"NFC",state.nfc_requested?"FIELD":nfc_enabled?"SCAN":"OFF",'n',nfc_enabled || state.nfc_requested,!state.nfc || state.busy},
        {"STOP","ALL",'x',false,!(enabled || scan_enabled || nfc_enabled || state.nfc_requested)},
        ls_rsel_button(LS_RSEL_MIXRF),ls_opt_button(mixrf_options())};
    buttons[9].dim=!mixrf_options();
    const int nb=10;
    int h=ls_btn_raised_height(a,nb);
    if(a.h<22 && a.w>=72) h=6;
    ls_btn_bar_raised(sf,tui_rect_make(a.x,a.y,a.w,h),buttons,nb,button_focus);
    tui_rect panel=tui_rect_make(a.x,a.y+h,a.w,a.h-h-2);
    ls_panel_box(sf,panel,LS_TEXT(t_scr_mixrf_keyboard_radios_receive, "KEYBOARD RADIOS / RECEIVE"),TUI_CYAN);
    ls_motion_busy(sf,panel,state.busy || state.receiving || state.scanning || state.card_scanning);
    tui_rect chart=panel;
    bool wide=panel.w>=90;
    if(wide) { panel.w=panel.w/2; chart.x=panel.x+panel.w;chart.w-=panel.w;ls_panel_box(sf,chart,view_nfc?"NFC / RECEIVE HISTORY":view_24?"2.4 GHz ACTIVITY":"CHANNEL ENERGY",TUI_CYAN); }
    char line[80];
    snprintf(line,sizeof(line),"%s / enable %s",state.keyboard?"detected":"absent",state.power?"on":"off");
    ls_kv(sf,panel,1,"KEYBOARD",line,LS_ATTR_DIM);
    if(state.cc)snprintf(line,sizeof(line),"identified v%02X",state.cc_version);
    else snprintf(line,sizeof(line),"%s",state.busy?"probing":"unavailable");
    ls_kv(sf,panel,3,"CC1101",line,LS_ATTR_DIM);
    ls_kv(sf,panel,4,"NRF24",state.nrf?"register check passed":state.busy?"probing":"unavailable",LS_ATTR_DIM);
    if(state.nfc)snprintf(line,sizeof(line),"identified ID %02X",state.nfc_identity);
    else snprintf(line,sizeof(line),"%s",state.busy?"probing":"unavailable");
    ls_kv(sf,panel,5,"NFC",line,LS_ATTR_DIM);
    if(view_nfc && state.nfc_requested)LS_SNPRINTF(t_scr_mixrf_lu_samples_lu_arrivals, line,sizeof(line),"%lu samples / %lu arrivals",(unsigned long)state.nfc_samples,(unsigned long)state.nfc_events);
    else if(view_nfc)LS_SNPRINTF(t_scr_mixrf_lu_polls_lu_replies, line,sizeof(line),"%lu polls / %lu replies",(unsigned long)state.card_polls,(unsigned long)state.card_hits);
    else if(view_24)LS_SNPRINTF(t_scr_mixrf_lu_sweeps_lu_hits, line,sizeof(line),"%lu sweeps / %lu hits",(unsigned long)state.sweeps,(unsigned long)state.energy_hits);
    else snprintf(line,sizeof(line),"%.4f MHz",(state.receive_requested?state.frequency:frequency)/1e6);
    ls_kv(sf,panel,a.h<22?6:7,view_nfc?"13.56 MHz":view_24?"SURVEY":"MONITOR",line,LS_ATTR_DIM);
    if(view_nfc && !state.nfc_requested && (state.card_requested || state.card_polls)){
        if(state.card_atqa)snprintf(line,sizeof(line),"%s / last ATQA %04X",state.card_present?"CARD PRESENT":state.card_scanning?"waiting for card":"stopped",state.card_atqa);
        else snprintf(line,sizeof(line),"%s / ATQA --",state.card_scanning?"waiting for card":"stopped");
    }
    else if(view_nfc)snprintf(line,sizeof(line),"%s",state.nfc_watching?(state.nfc_field?"FIELD PRESENT":"waiting for reader field"):"off");
    else if(view_24)snprintf(line,sizeof(line),"%s / %u MHz",state.scanning?"listening":"off",2400+state.channel);
    else if(state.receiving)snprintf(line,sizeof(line),"%c ~%.1f dBm",ls_motion_pip(true),state.rssi);
    else snprintf(line,sizeof(line),"%s",enabled?"starting / no fresh sample":"off");
    ls_kv(sf,panel,a.h<22?7:8,"ENERGY",line,LS_ATTR_DIM);
    if(view_nfc && chart.h>(wide?13:23)) {
        int top=chart.y+(wide?2:10),bottom=chart.y+chart.h-5;
        const char *banner=state.card_present?"((( CARD DETECTED )))":state.card_scanning?"[ MOVE CARD SLOWLY / HOLD 1 SECOND ]":state.nfc_watching?(state.nfc_field?"[ EXTERNAL FIELD PRESENT ]":"[ WAITING FOR READER FIELD ]"):"[ NFC DETECTOR OFF ]";
        tui_put_str(sf,chart,chart.x+2,top,banner,state.card_present?TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
        if(!state.nfc_requested){
        tui_put_str(sf,chart,chart.x+2,top+1,LS_TEXT(t_scr_mixrf_rx_level_0_15, "RX level 0 <--------------------> 15"),LS_ATTR_DIM);
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
        tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-3,LS_TEXT(t_scr_mixrf_noise_r_reply_c, "! noise  R reply  C confirmed / newest below"),LS_ATTR_DIM);
        tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-2,LS_TEXT(t_scr_mixrf_13_56_mhz_rx, "13.56 MHz RX history / uncalibrated level"),LS_ATTR_DIM);
        }
    } else if(!view_nfc && chart.h>(wide?13:20)) {
        int bottom=chart.y+chart.h-5,height=wide?chart.h-7:chart.h-16;
        for(int x=0;x<chart.w-4;x++) {
            if(view_24?!state.sweeps:!last_samples)continue;
            int n=x*(view_24?LS_MIXRF_CHANNELS:48)/(chart.w-4);
            if(!view_24 && !isfinite(trace[n]))continue;
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
    ls_safe_line(sf,a,a.y+a.h-2,held?ls_mixrf_cc_refusal(&state):state.status,LS_ATTR_DIM);
    ls_safe_line(sf,a,a.y+a.h-1,feedback[0]?feedback:ls_tui_keyboard_mode()?"M RX | S scan | N NFC | V view | X stop":"",LS_ATTR_DIM);
}
static bool key(ls_tk_t k,char c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if(card_suite){if(k==LS_TK_ESC){card_suite=false;return true;}return ls_scr_nfc.key(k,c);}
    if(k==LS_TK_CHAR && c=='c'){cards_open();return true;}
    if(ls_btn_navigate(k,&button_slot,&button_focus,false))return true;
    if(k==LS_TK_ENTER){if(ls_btn_enabled(0,button_focus))action(button_focus);return true;}
    if(k!=LS_TK_CHAR)return false;
    if(c=='f'){nfc_mode(state.nfc_requested?0:2);return true;}
    if(c=='b'){ls_opt_open(mixrf_options());return true;}
    if(ls_opt_key(mixrf_options(),c))return true;
    const char *p=strchr("pmbjsvnxr",c);
    if(!c || !p)return false;
    action((int)(p-"pmbjsvnxr"));return true;
}
static bool touch(int x,int y){if(card_suite){if(!ls_scr_nfc.touch(x,y))card_suite=false;return true;}int i=ls_btn_hit(x,y);if(i>=0)action(i);return true;}
const ls_tui_screen_t ls_scr_mixrf={.name="MIX-RF",.hint=hint,.enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch};
