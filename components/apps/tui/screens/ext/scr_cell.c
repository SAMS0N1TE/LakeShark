#include "esp_attr.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_app.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_braille.h"
#include "../../ls_value.h"
#include "../../ls_action.h"
#include "cell_monitor.h"
#include "cell_report.h"
#include "cell_performance.h"
#include "cell_iq.h"
#include "ls_mesh.h"
#include "ls_gps.h"
#include "ls_imu_status_policy.h"
#include "../../ls_theme.h"
#include "../../ls_tui.h"
#include "ls_sdcard.h"
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include "esp_timer.h"
#include "ls_textpack.h"

static void frequency_readout(tui_surface *sf,tui_rect area,int y,uint32_t hz,uint8_t ink)
{
    static const uint8_t digits[10][5]={
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
    char text[24];if(hz)snprintf(text,sizeof(text),"%.3f",hz/1e6);else snprintf(text,sizeof(text),"---.---");
    int x=area.x+2;
    for(const char *c=text;*c;c++) {
        if(*c=='.'){tui_put_char(sf,area,x,y+4,LS_TUI_BLOCK_FULL,ink);x+=2;continue;}
        for(int row=0;row<5;row++)for(int col=0;col<3;col++)
            if((*c=='-' && row==2) || (*c>='0' && *c<='9' && (digits[*c-'0'][row]&(4>>col))))
                tui_put_char(sf,area,x+col,y+row,LS_TUI_BLOCK_FULL,ink);
        x+=4;
    }
    tui_put_str(sf,area,x+1,y+4,"MHz",ink);
}

static unsigned band;
static int focus=-1,slot;
static cell_status_t status;
static bool opened;
static char mode_hint[128];
static bool manual_site;
static char report_peers[LS_MESH_MAX_PEERS][17];
static uint32_t hrf_frequency=739000000,hrf_rate=8000000,hrf_ms=80;
static bool auto_multi;
static char control_message[80];
static ls_imu_status_policy_t imu_display;
static const uint32_t hrf_centers[]={739000000,751000000,881500000,1981250000,1992500000,2150000000};
static const uint32_t hrf_rates[]={8000000,10000000,19200000,20000000};
static void hrf_choose_frequency(int i){if(i>=0 && i<6)hrf_frequency=hrf_centers[i];}
/* "B12 / EARFCN 5110" for a frequency on the band table, "B2" when it is
 * between two raster points. */
static void band_label(uint32_t hz,char *out,size_t n)
{
    int b=cell_band_for_hz(hz);unsigned e;
    if(b<0)LS_SNPUTS(t_scr_cell_outside_the_lte_bands, out, n, "outside the LTE bands");
    else if(cell_hz_to_earfcn((unsigned)b,hz,&e))snprintf(out,n,"B%u / EARFCN %u",cell_bands[b].band,e);
    else snprintf(out,n,"B%u",cell_bands[b].band);
}
static void hrf_choose_rate(int i){if(i>=0 && i<4)hrf_rate=hrf_rates[i];}
static void hrf_choose_duration(int i){if(i>=0 && i<3)hrf_ms=i==0?30:i==1?80:100;}
static void performance_choose(int i){if(i==1)cell_performance_reboot(!cell_performance_active());}
static void performance_picker(void)
{
    ls_picker_open(cell_performance_active()?"RETURN TO NORMAL OS?":"HIGH RATE / RESTART REQUIRED",performance_choose);
    ls_picker_add("Stay here","Cancel");
    ls_picker_add(cell_performance_active()?"Restart normal OS":"Restart into HackRF",cell_performance_active()?"Return to survey":"GPS + IMU evidence");
}
static void report_choose(int i)
{
    if(i<0 || i>LS_MESH_MAX_PEERS)return;
    bool saved=cell_report_target(i?report_peers[i-1]:"");
    snprintf(control_message,sizeof(control_message),saved?"Report selection saved":"Report changed in RAM; SD save failed");
}
static void choose(int i) {if(i>=0 && (unsigned)i<cell_band_count) band=(unsigned)i;}
static void enter(void) {snprintf(mode_hint,sizeof(mode_hint),"%s",cell_performance_active()?"S auto/stop C once B channel V view O options H exit":"S go/stop B band L learn D load G site E LTE N cells P rate T report R radio O opts H exit");opened=true;memset(&imu_display,0,sizeof(imu_display));cell_monitor_init();cell_report_init();ls_tui_radio_want(NULL);ls_gps_start();ls_imu_start();}
static void leave(void) {opened=false;cell_iq_stop();cell_monitor_wait_stopped();}
static bool request(int cmd)
{
    if(!opened) return false;
    control_message[0]=0;
    if(cmd!=CELL_LOAD) ls_tui_radio_want(NULL);
    return cell_monitor_request(cmd,band,manual_site);
}
static void view_choose(int i)
{
    if(i==0)ls_tui_set_daylight(!ls_tui_daylight());
    if(i==1){ls_tui_set_font_index(ls_tui_font_index()==2?0:2);ls_tui_screen_request_regrid();}
    if(i==2)ls_tui_set_crisp_text(!ls_tui_crisp_text());
}
static void action(int i);
static void open_cells(void);
/* OPTIONS. The survey's: which band, where the site is, where reports go.
   HIGH RATE's: the capture plan, as rows that show what is set. Neither
   changes under a running capture. */
static const char *o_cell_busy(const ls_opt_t *o)
{
    (void)o;
    cell_monitor_get(&status);
    return status.busy ? "Stop the survey first" : NULL;
}
static void o_band(const ls_opt_t *o) { (void)o; action(1); }
static void o_band_show(const ls_opt_t *o,char *out,size_t n)
{
    (void)o;
    snprintf(out,n,"%s",band<cell_band_count?cell_bands[band].name:"none");
}
static const char *const SITE[]={"GPS","LOCAL"};
static int o_site(const ls_opt_t *o){(void)o;return manual_site;}
static void o_set_site(const ls_opt_t *o,int v){(void)o;manual_site=v!=0;}
static void o_report(const ls_opt_t *o){(void)o;action(6);}
static void o_report_show(const ls_opt_t *o,char *out,size_t n){(void)o;cell_report_status(out,n);}
static void history_choose(int i){(void)i;}
static void o_history(const ls_opt_t *o)
{
    (void)o;
    static EXT_RAM_BSS_ATTR char lines[8][32],details[8][32],record[512];
    unsigned count=0,next=0;
    FILE *f=ls_sdcard_mounted()?fopen("/sdcard/cell/lte.jsonl","rb"):NULL;
    if(f) {
        if(fseek(f,0,SEEK_END)==0) {
            long size=ftell(f);
            if(size>16384) {fseek(f,size-16384,SEEK_SET);fgets(record,sizeof(record),f);}
            else rewind(f);
            while(fgets(record,sizeof(record),f)) {
                if(!strstr(record,"\"kind\":\"LTE_SYNC\""))continue;
                char *hz=strstr(record,"\"hz\":"),*pci=strstr(record,"\"pci\":"),*hits=strstr(record,"\"hits\":");
                unsigned long frequency;int id,repeats;
                if(!hz || !pci || !hits || sscanf(hz+5,"%lu",&frequency)!=1 ||
                   sscanf(pci+6,"%d",&id)!=1 || sscanf(hits+7,"%d",&repeats)!=1)continue;
                snprintf(lines[next],sizeof(lines[next]),"%.3f MHz / PCI %d",frequency/1e6,id);
                LS_SNPRINTF(t_scr_cell_d_repeats_saved_on, details[next],sizeof(details[next]),"%d repeats / saved on SD",repeats);
                next=(next+1)%8;if(count<8)count++;
            }
        }
        fclose(f);
    }
    ls_picker_open(LS_TEXT(t_scr_cell_recent_sd_lte_pci, "RECENT SD LTE / PCI ONLY"),history_choose);
    if(!count)ls_picker_add(LS_TEXT(t_scr_cell_no_saved_lte_observations, "No saved LTE observations"),ls_sdcard_mounted()?"No recent sync records":"SD missing");
    for(unsigned i=0;i<count;i++) {unsigned index=(next+7-i)%8;ls_picker_add(lines[index],details[index]);}
}
static const char *const SCAN[]={"GATED","FULL"};
static int o_scan(const ls_opt_t *o){(void)o;cell_status_t s;cell_monitor_get(&s);return !s.gated;}
static void o_set_scan(const ls_opt_t *o,int v){(void)o;cell_monitor_set_full_scan(v!=0);}
static const char *const MIBS[]={"OFF","ON"};
static int o_mib(const ls_opt_t *o){(void)o;cell_status_t s;cell_monitor_get(&s);return s.mib_on;}
static void o_set_mib(const ls_opt_t *o,int v){(void)o;cell_monitor_set_mib(v!=0);}
static void o_ppm(const ls_opt_t *o){(void)o;cell_monitor_clear_ppm();}
static void o_ppm_show(const ls_opt_t *o,char *out,size_t n)
{
    (void)o;
    cell_status_t s;cell_monitor_get(&s);
    if(!s.ppm_known)snprintf(out,n,"unknown");
    else LS_SNPRINTF(t_scr_cell_1f_tuner_d_clear, out,n,"%+.1f / tuner %+d: CLEAR",(double)s.ppm,s.ppm_value);
}
static void o_cells(const ls_opt_t *o){(void)o;open_cells();}
static void o_cells_show(const ls_opt_t *o,char *out,size_t n)
{
    (void)o;
    cell_status_t s;cell_monitor_get(&s);
    snprintf(out,n,"%u found",s.lte_count);
}
static void o_report_test(const ls_opt_t *o)
{
    (void)o;
    char *args[]={"cellreport","test"};cell_report_command(2,args);
    cell_report_status(control_message,sizeof(control_message));
}
static const ls_opt_t OPT_CELL[]={
    {.label="BAND",.kind=LS_OPT_ACTION,.act=o_band,.show=o_band_show,.why_not=o_cell_busy},
    {.label="SITE",.kind=LS_OPT_TOGGLE,.names=SITE,.get=o_site,.set=o_set_site,.why_not=o_cell_busy},
    {.label="LTE SCAN",.kind=LS_OPT_TOGGLE,.names=SCAN,.get=o_scan,.set=o_set_scan,.why_not=o_cell_busy},
    {.label="LTE MIB",.kind=LS_OPT_TOGGLE,.names=MIBS,.get=o_mib,.set=o_set_mib,.why_not=o_cell_busy},
    {.label="DONGLE PPM",.kind=LS_OPT_ACTION,.act=o_ppm,.show=o_ppm_show,.why_not=o_cell_busy},
    {.label="ALL CELLS",.kind=LS_OPT_ACTION,.act=o_cells,.show=o_cells_show},
    {.label="LORA REPORTS",.kind=LS_OPT_ACTION,.act=o_report,.show=o_report_show},
    {.label="RECENT SD LTE",.kind=LS_OPT_ACTION,.act=o_history},
    {.label="REPORT TEST / HEALTH",.kind=LS_OPT_ACTION,.act=o_report_test,.show=o_report_show},
};
static const ls_opt_ctx_t CTX_CELL={.name="CELL",.job=LS_RSEL_CELL,.radio=LS_RSEL_NONE,LS_OPT_ROWS(OPT_CELL),.tag="SURVEY"};

/* The capture and GPS snapshots this screen reads are static and in PSRAM:
   on the TUI's 6 KB internal stack they leave a HIGH RATE redraw too little
   room. Only the TUI task draws, so one copy each is enough. */
static const char *o_hrf_busy(const ls_opt_t *o)
{
    (void)o;
    static EXT_RAM_BSS_ATTR cell_iq_status_t iq;cell_iq_get_status(&iq);
    return iq.busy?"Stop the capture first":NULL;
}
static const char *const PLAN[]={"selected channel","six presets"};
static int o_plan(const ls_opt_t *o){(void)o;return auto_multi;}
static void o_set_plan(const ls_opt_t *o,int v){(void)o;auto_multi=v!=0;}
static const char *const HRF_CH[]={"739.000 MHz","751.000 MHz","881.500 MHz","1981.250 MHz","1992.500 MHz","2150.000 MHz"};
static int o_hrf_ch(const ls_opt_t *o){(void)o;for(int i=0;i<6;i++)if(hrf_centers[i]==hrf_frequency)return i;return -1;}
static void o_set_hrf_ch(const ls_opt_t *o,int v){(void)o;hrf_choose_frequency(v);}
static void o_hrf_ch_show(const ls_opt_t *o,char *out,size_t n)
{
    (void)o;
    char label[32];band_label(hrf_frequency,label,sizeof(label));
    snprintf(out,n,"%.3f MHz / %s",hrf_frequency/1e6,label);
}
static const char *const RATE[]={"8.0 MS/s","10.0 MS/s","19.2 MS/s","20.0 MS/s"};
static int o_rate(const ls_opt_t *o){(void)o;for(int i=0;i<4;i++)if(hrf_rates[i]==hrf_rate)return i;return 0;}
static void o_set_rate(const ls_opt_t *o,int v){(void)o;hrf_choose_rate(v);}
static const char *const LENGTH[]={"30 ms","80 ms","100 ms"};
static int o_length(const ls_opt_t *o){(void)o;return hrf_ms==30?0:hrf_ms==80?1:2;}
static void o_set_length(const ls_opt_t *o,int v){(void)o;hrf_choose_duration(v);}
static void o_rate_show(const ls_opt_t *o,char *out,size_t n){(void)o;snprintf(out,n,"%.1f MS/s%s",hrf_rate/1e6,hrf_rate==8000000?"":" EXPERIMENTAL");}
static const ls_opt_t OPT_HRF[]={
    {.label="AUTO PLAN",.kind=LS_OPT_TOGGLE,.names=PLAN,.get=o_plan,.set=o_set_plan,.why_not=o_hrf_busy},
    {.label="CHANNEL",.kind=LS_OPT_CYCLE,.names=HRF_CH,.n=6,.get=o_hrf_ch,.set=o_set_hrf_ch,.show=o_hrf_ch_show,.why_not=o_hrf_busy},
    {.label="ONCE: SAMPLE RATE",.kind=LS_OPT_CYCLE,.names=RATE,.n=4,.get=o_rate,.set=o_set_rate,.show=o_rate_show,.why_not=o_hrf_busy},
    {.label="ONCE: LENGTH",.kind=LS_OPT_CYCLE,.names=LENGTH,.n=3,.get=o_length,.set=o_set_length,.why_not=o_hrf_busy},
};
static const ls_opt_ctx_t CTX_HRF={.name="HIGH RATE",.job=-1,.radio=LS_RSEL_SDR_HACKRF,LS_OPT_ROWS(OPT_HRF),.tag="CAPTURE"};

static void action(int i)
{
    if(cell_performance_active()) {
        static EXT_RAM_BSS_ATTR cell_iq_status_t iq;cell_iq_get_status(&iq);
        if(i==0 && iq.busy){cell_iq_stop();return;}
        if(i==3){ls_picker_open("DISPLAY",view_choose);ls_picker_add(ls_tui_daylight()?"Dark theme":"Light theme","Change now");ls_picker_add(ls_tui_font_index()==2?"Compact text":"Large text",LS_TEXT(t_scr_cell_change_text_touch_targets, "Change text + touch targets"));ls_picker_add(ls_tui_crisp_text()?"Smooth text":"Crisp text",LS_TEXT(t_scr_cell_change_edge_rendering, "Change edge rendering"));return;}
        if(iq.busy)return;
        control_message[0]=0;
        if(i==0){if(!cell_iq_auto_begin(hrf_frequency,auto_multi))LS_SNPUTS(t_scr_cell_auto_needs_mounted_sd, control_message, sizeof(control_message), "Auto needs mounted SD + idle HackRF");}
        if(i==1){if(!cell_iq_hackrf_begin(hrf_frequency,hrf_rate,hrf_ms))LS_SNPUTS(t_scr_cell_cannot_start_capture, control_message, sizeof(control_message), "Cannot start capture");}
        if(i==2){ls_picker_open("RECEIVE CHANNEL",hrf_choose_frequency);for(int n=0;n<6;n++){char text[32];snprintf(text,sizeof(text),"%.3f MHz",hrf_centers[n]/1e6);char label[32];band_label(hrf_centers[n],label,sizeof(label));ls_picker_add(text,label);}}
        if(i==4)ls_opt_open(&CTX_HRF);
        if(i==5)performance_picker();
        return;
    }
    cell_monitor_get(&status);
    if(i==0) {if(status.busy)cell_monitor_stop();else request(CELL_WATCH);}
    else if(i==1 && !status.busy) {
        ls_picker_open(LS_TEXT(t_scr_cell_cellular_downlink_band, "CELLULAR DOWNLINK BAND"),choose);
        for(unsigned b=0;b<cell_band_count;b++) {
            char text[48];unsigned first,last;cell_band_earfcns(b,&first,&last);
            LS_SNPRINTF(t_scr_cell_0f_0f_mhz_earfcn, text,sizeof(text),"%.0f-%.0f MHz EARFCN %u-%u",cell_bands[b].low/1e6,cell_bands[b].high/1e6,first,last);
            ls_picker_add(cell_bands[b].name,text);
        }
    } else if(i==2 && !status.busy) request(CELL_LEARN);
    else if(i==3 && !status.busy) request(CELL_LOAD);
    else if(i==4 && !status.busy) manual_site=!manual_site;
    else if(i==5) {
        const ls_app_t *a=ls_app_by_id("home");
        if(a) ls_tui_screen_show(ls_tui_screen_index_of(a->screen));
    } else if(i==6) {
        ls_picker_open(LS_TEXT(t_scr_cell_lora_report_destination, "LORA REPORT DESTINATION"),report_choose);
        ls_picker_add("Reports OFF",LS_TEXT(t_scr_cell_stop_automatic_addressed_reports, "Stop automatic addressed reports"));
        for(int n=0;n<LS_MESH_MAX_PEERS;n++) {
            ls_mesh_peer_t peer;if(!ls_mesh_peer_at(n,&peer))break;
            snprintf(report_peers[n],sizeof(report_peers[n]),"%s",peer.id);
            ls_picker_add(peer.name[0]?peer.name:peer.id,peer.id);
        }
    } else if(i==7 && !status.busy)request(CELL_LTE);
    else if(i==8 && !status.busy)performance_picker();
    /* The survey is the RTL-SDR's; the list is the one every app has, and
       says why for the rest. */
    else if(i==9)ls_rsel_open(LS_RSEL_CELL,NULL);
    else if(i==10)ls_opt_open(&CTX_CELL);
}
static void draw_high_rate(tui_surface *sf,tui_rect area)
{
    static EXT_RAM_BSS_ATTR cell_iq_status_t iq;cell_iq_get_status(&iq);
    int64_t now=esp_timer_get_time();
    if(ls_imu_status_poll_due(&imu_display,now)) {
        ls_imu_sample_t imu;
        bool ok=ls_imu_read(&imu);
        ls_imu_status_record(&imu_display,now,ok,ok && imu.mag_valid);
    }
    bool imu_ok=ls_imu_status_recent(&imu_display,now);
    const char *imu_text=imu_ok?(imu_display.mag_valid?"READY":"6 axes / mag missing"):
        imu_display.good?"STALE":"UNAVAILABLE";
    uint8_t cyan=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK),white=TUI_ATTR(TUI_WHITE,TUI_BLACK),yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK);
    int controls=ls_btn_raised_height(area,6);
    if(ls_tui_is_wide() && area.h>=24 && controls<5)controls=5;
    /* Rounded portrait corners reduce the real bar width to two columns.
     * Reserve three full-height rows so labels and hints remain separate. */
    if(area.w<48 && area.h>=35)controls=15;
    tui_rect body=tui_rect_make(area.x,area.y,area.w,area.h-controls);
    int y=body.y;char text[120];
    if(body.h<24) {
        ls_safe_line(sf,body,y++,LS_TEXT(t_scr_cell_cell_watch_auto_lte, "CELL WATCH / AUTO LTE OBSERVATIONS"),cyan);
        LS_SNPRINTF(t_scr_cell_s_3f_mhz_lu, text,sizeof(text),"%s / %.3f MHz / %lu attempts / %lu MIB",
            iq.stopping?"STOPPING":iq.busy?"RUNNING":"READY",(iq.busy?iq.active_hz:hrf_frequency)/1e6,
            (unsigned long)iq.attempts,(unsigned long)iq.decoded);
        ls_safe_line(sf,body,y++,text,yellow);
        LS_SNPRINTF(t_scr_cell_s_1fm_lums_hackrf, text,sizeof(text),"%s %.1fM %lums HackRF",
            iq.attempts?(iq.automatic?"Last AUTO":"Last ONCE"):"ONCE plan",
            (iq.attempts?iq.rate:hrf_rate)/1e6,(unsigned long)(iq.attempts?iq.requested_ms:hrf_ms));ls_safe_line(sf,body,y++,text,white);
        LS_SNPRINTF(t_scr_cell_last_3f_mhz_pci, text,sizeof(text),"Last %.3f MHz / PCI %d / MIB %s / CRC x%d",iq.hz/1e6,
            iq.lte_found?iq.lte.pci:-1,iq.mib_found?"decoded":iq.mib_checked?"CRC failed":"not checked",iq.mib.frames);
        if(!iq.attempts)LS_SNPUTS(t_scr_cell_no_completed_attempt_yet, text, sizeof(text), "No completed attempt yet");
        else if(!iq.complete)LS_SNPRINTF(t_scr_cell_last_capture_failed_s, text,sizeof(text),"Last capture failed: %s",iq.message);
        ls_safe_line(sf,body,y++,text,cyan);
        static EXT_RAM_BSS_ATTR ls_gps_state_t gps;ls_gps_get(&gps);
        bool fix=cell_gps_valid(gps.fix,gps.last_fix_us,esp_timer_get_time(),gps.lat_deg,gps.lon_deg,gps.hdop);
        LS_SNPRINTF(t_scr_cell_gps_s_u_sats, text,sizeof(text),"GPS %s (%u sats) / 9 AXIS %s / SD %s",fix?"FIX":"SEEKING",gps.sats_visible,
            imu_text,ls_sdcard_mounted()?"READY":"MISSING");ls_safe_line(sf,body,y++,text,white);
        if(y<body.y+body.h-2)ls_safe_line(sf,body,y++,control_message[0]?control_message:iq.message,white);
        ls_safe_line(sf,body,body.y+body.h-1,LS_TEXT(t_scr_cell_detection_unvalidated_identity_and, "Detection unvalidated / identity and SIB1 still missing"),yellow);
        goto render_controls;
    }
    ls_safe_line(sf,body,y++,LS_TEXT(t_scr_cell_cell_watch_passive_lte, "CELL WATCH / PASSIVE LTE"),cyan);
    const char *phase=iq.phase==CELL_IQ_FILTERING?"Filtering samples":iq.phase==CELL_IQ_SYNCHRONIZING?"Finding LTE cell":iq.phase==CELL_IQ_DECODING_MIB?"Checking broadcast CRC":iq.phase==CELL_IQ_SAVING?"Saving evidence":iq.phase==CELL_IQ_WAITING?"Waiting for next capture":"Receiving samples";
    ls_safe_line(sf,body,y++,iq.stopping?"STOPPING / finishing this save":iq.busy?phase:"READY / start Auto or Once",yellow);
    uint32_t shown=iq.busy?iq.active_hz:hrf_frequency;
    if(body.w>=32 && body.h>=26){frequency_readout(sf,body,y+1,shown,cyan);y+=7;}
    else {snprintf(text,sizeof(text),"%.3f MHz",shown/1e6);ls_safe_line(sf,body,y++,text,cyan);}
    snprintf(text,sizeof(text),"Auto: %s",(iq.busy?iq.multi:auto_multi)?"6 preset channels":"selected channel");ls_safe_line(sf,body,y++,text,white);
    LS_SNPRINTF(t_scr_cell_s_1fm_lums_hackrf, text,sizeof(text),"%s %.1fM %lums HackRF",iq.attempts?(iq.automatic?"Last AUTO":"Last ONCE"):"ONCE plan",(iq.attempts?iq.rate:hrf_rate)/1e6,(unsigned long)(iq.attempts?iq.requested_ms:hrf_ms));
    ls_safe_line(sf,body,y++,text,white);
    LS_SNPRINTF(t_scr_cell_lu_attempts_lu_mib, text,sizeof(text),"%lu attempts / %lu MIB decodes",(unsigned long)iq.attempts,(unsigned long)iq.decoded);ls_safe_line(sf,body,y++,text,cyan);
    if(iq.lte_found)LS_SNPRINTF(t_scr_cell_last_3f_mhz_pci_2, text,sizeof(text),"Last: %.3f MHz / PCI %d",iq.hz/1e6,iq.lte.pci);
    else snprintf(text,sizeof(text),!iq.attempts?"No completed attempt yet":!iq.complete?"Last capture failed":iq.lte_checked?"No LTE sync in last capture":"LTE analysis not checked");
    ls_safe_line(sf,body,y++,text,white);
    if(iq.mib_found)LS_SNPRINTF(t_scr_cell_mib_1f_mhz_crc, text,sizeof(text),"MIB: %.1f MHz / CRC x%d",iq.mib.n_rb==6?1.4:iq.mib.n_rb/5.,iq.mib.frames);
    else snprintf(text,sizeof(text),"MIB: %s",iq.mib_checked?"CRC not confirmed":"not checked");
    ls_safe_line(sf,body,y++,text,cyan);
    if(body.h>=25)y++;
    static EXT_RAM_BSS_ATTR ls_gps_state_t gps;ls_gps_get(&gps);
    bool fix=cell_gps_valid(gps.fix,gps.last_fix_us,esp_timer_get_time(),gps.lat_deg,gps.lon_deg,gps.hdop);
    LS_SNPRINTF(t_scr_cell_gps_s_u_seen, text,sizeof(text),"GPS %s / %u seen / %u used",fix?"FIX":ls_gps_running()?"SEEKING":"OFF",gps.sats_visible,gps.sats_used);ls_safe_line(sf,body,y++,text,fix?cyan:yellow);
    if(fix){snprintf(text,sizeof(text),"%.5f, %.5f",gps.lat_deg,gps.lon_deg);ls_safe_line(sf,body,y++,text,white);}
    snprintf(text,sizeof(text),"9 AXIS: %s",imu_text);ls_safe_line(sf,body,y++,text,imu_ok && imu_display.mag_valid?cyan:yellow);
    LS_SNPRINTF(t_scr_cell_sd_s_iq_llu, text,sizeof(text),"SD %s / IQ %llu of 256 MiB",ls_sdcard_mounted()?"READY":"MISSING",(unsigned long long)(iq.raw_bytes/(1024*1024)));ls_safe_line(sf,body,y++,text,white);
    if(control_message[0])ls_safe_line(sf,body,y++,control_message,yellow);
    else if(iq.message[0])ls_safe_line(sf,body,y++,iq.message,white);
    if(y<body.y+body.h-3)ls_safe_line(sf,body,y++,LS_TEXT(t_scr_cell_start_before_travel_stop, "Start before travel; Stop to end."),white);
    ls_safe_line(sf,body,body.y+body.h-2,LS_TEXT(t_scr_cell_detection_not_yet_validated, "Detection: not yet validated"),yellow);
    ls_safe_line(sf,body,body.y+body.h-1,LS_TEXT(t_scr_cell_identity_sib1_still_missing, "Identity / SIB1 still missing"),yellow);
render_controls:;
    ls_btn_t buttons[]={
        {iq.busy?"STOP":"AUTO",iq.busy?"save + end":"repeat",'S',iq.busy,false},
        {"ONCE","single",'C',false,iq.busy},
        {"CHANNEL","tune",'B',false,iq.busy},
        {"VIEW","theme/size",'V',false,false},
        ls_opt_button(&CTX_HRF),
        {"EXIT","normal OS",'H',false,iq.busy}};
    buttons[4].dim=iq.busy;
    if(body.h<24)for(unsigned i=0;i<6;i++)buttons[i].value=NULL;
    ls_btn_bar_raised(sf,tui_rect_make(area.x,area.y+area.h-controls,area.w,controls),buttons,6,focus);
}
/* The LTE search's cells, strongest first: the list is the worker's, copied
 * here once a frame into PSRAM (only the TUI task draws). */
static EXT_RAM_BSS_ATTR cell_lte_t shown[CELL_MAX_CELLS];
static uint8_t order[CELL_MAX_CELLS];
static unsigned shown_n;
static tui_rect table_area;     /* where the list was last drawn; a tap there opens it */
static void load_cells(bool same_setup,bool lte)
{
    shown_n=same_setup && lte?cell_monitor_cells(shown,CELL_MAX_CELLS):0;
    for(unsigned i=0;i<shown_n;i++) {
        unsigned j=i;
        while(j && shown[order[j-1]].rsrp10<shown[i].rsrp10){order[j]=order[j-1];j--;}
        order[j]=(uint8_t)i;
    }
}
static const char *bw_text(int n_rb)
{
    switch(n_rb) {
        case 6: return "1.4M"; case 15: return "3M"; case 25: return "5M";
        case 50: return "10M"; case 75: return "15M"; case 100: return "20M";
    }
    return "?";
}
static uint8_t power_ink(int rsrp10)
{
    return rsrp10==CELL_NO_POWER?TUI_WHITE:rsrp10>=-550?(TUI_GREEN|TUI_BRIGHT):
           rsrp10>=-700?(TUI_YELLOW|TUI_BRIGHT):(TUI_RED|TUI_BRIGHT);
}
/* A mast whose arcs light as cells are heard: none, one, two, three. */
static void draw_tower(tui_surface *sf,tui_rect clip,int x,int y,unsigned lit)
{
    uint8_t off=LS_ATTR_FAINT,outer=lit>=3?TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK):off;
    uint8_t mid=lit>=2?TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK):off,core=lit>=1?TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK):off;
    tui_put_str(sf,clip,x,y,"((",outer);tui_put_char(sf,clip,x+2,y,'(',mid);tui_put_char(sf,clip,x+3,y,'o',core);
    tui_put_char(sf,clip,x+4,y,')',mid);tui_put_str(sf,clip,x+5,y,"))",outer);
    tui_put_char(sf,clip,x+3,y+1,'|',TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
    uint8_t leg=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK);
    tui_put_char(sf,clip,x+2,y+2,'/',leg);tui_put_char(sf,clip,x+3,y+2,'|',TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));tui_put_char(sf,clip,x+4,y+2,'\\',leg);
    tui_put_str(sf,clip,x+1,y+3,"/_|_\\",TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK));
}
/* The occupancy or survey trace as braille dots, two columns and four levels
 * to a cell, coloured by what the search made of it. */
static void draw_plot(tui_surface *sf,tui_rect plot,const cell_status_t *s,bool lte,bool same)
{
    uint8_t cyan=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK),green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    uint8_t magenta=TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK),yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK);
    uint8_t white=TUI_ATTR(TUI_WHITE,TUI_BLACK),faint=LS_ATTR_FAINT;
    for(int row=0;row<plot.h;row+=2)for(int col=0;col<plot.w;col+=6)
        tui_put_char(sf,plot,plot.x+col,plot.y+row,'.',faint);
    if(!(same && s->spectrum_count)) {
        tui_put_str(sf,plot,plot.x+1,plot.y+plot.h/2,lte?"Occupancy appears after the first pass":"Spectrum appears after a full pass",white);
        return;
    }
    const int levels=plot.h*4;
    for(int x=0;x<plot.w;x++) {
        int height[2];uint8_t marks=0;
        for(int half=0;half<2;half++) {
            unsigned begin=(unsigned)(2*x+half)*s->spectrum_count/(unsigned)(2*plot.w);
            unsigned end=(unsigned)(2*x+half+1)*s->spectrum_count/(unsigned)(2*plot.w);
            if(end<=begin)end=begin+1;
            if(end>s->spectrum_count)end=s->spectrum_count;
            int value=CELL_MISSING;
            for(unsigned i=begin;i<end;i++) {if(s->spectrum[i]>value)value=s->spectrum[i];marks|=s->mark[i];}
            height[half]=value==CELL_MISSING?0:(value+110)*levels/80;
            if(height[half]<0)height[half]=0;
            if(height[half]>levels)height[half]=levels;
            if(value!=CELL_MISSING && height[half]==0)height[half]=1;
        }
        uint8_t ink=(marks&4)?magenta:(marks&1)?green:cyan;
        for(int row=0;row<plot.h;row++) {
            int base=(plot.h-1-row)*4;unsigned bits=0;
            for(int half=0;half<2;half++) {
                int k=height[half]-base;
                if(k>4)k=4;
                if(k>0)bits|=ls_braille_column((unsigned)k,half);
            }
            if(bits)ls_braille_put(sf,plot,plot.x+x,plot.y+row,(int16_t)(0x2800|bits),TUI_ATTR(ink,TUI_BLACK));
        }
        if(!lte && s->compared) {
            int reference=CELL_MISSING;
            unsigned begin=(unsigned)x*s->spectrum_count/(unsigned)plot.w,end=(unsigned)(x+1)*s->spectrum_count/(unsigned)plot.w;
            if(end<=begin)end=begin+1;
            for(unsigned i=begin;i<end && i<s->spectrum_count;i++)if(s->reference[i]>reference)reference=s->reference[i];
            if(reference!=CELL_MISSING) {
                int rh=(reference+110)*(plot.h-1)/80;if(rh<0)rh=0;if(rh>=plot.h)rh=plot.h-1;
                tui_put_char(sf,plot,plot.x+x,plot.y+plot.h-1-rh,'-',yellow);
            }
        }
    }
}
/* One row under the trace: which columns were occupied, searched, and gave a cell. */
static void draw_marks(tui_surface *sf,tui_rect plot,int y,const cell_status_t *s)
{
    plot=tui_rect_make(plot.x,y,plot.w,1);
    for(int x=0;x<plot.w;x++) {
        unsigned begin=(unsigned)x*96/(unsigned)plot.w,end=(unsigned)(x+1)*96/(unsigned)plot.w;
        if(end<=begin)end=begin+1;
        uint8_t m=0;
        for(unsigned i=begin;i<end && i<96;i++)m|=s->mark[i];
        if(m&4)tui_put_char(sf,plot,plot.x+x,y,'*',TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK));
        else if(m&2)tui_put_char(sf,plot,plot.x+x,y,'+',TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        else if(m&1)tui_put_char(sf,plot,plot.x+x,y,'=',TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK));
        else tui_put_char(sf,plot,plot.x+x,y,'-',LS_ATTR_FAINT);
    }
}
static void draw_chart(tui_surface *sf,tui_rect chart,const cell_status_t *s,bool lte,bool same,unsigned band)
{
    uint8_t white=TUI_ATTR(TUI_WHITE,TUI_BLACK),yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK),faint=LS_ATTR_FAINT;
    ls_panel_box(sf,chart,lte?"BAND OCCUPANCY / CELLS":"RF POWER / LAST COMPLETE PASS",lte?TUI_MAGENTA:TUI_CYAN);
    tui_rect plot=tui_rect_make(chart.x+2,chart.y+1,chart.w-4,chart.h-4);
    char text[48];
    draw_plot(sf,plot,s,lte,same);
    const uint32_t low=cell_bands[band].low,span=cell_bands[band].high-low;
    if(lte && same) {
        /* A cell's PCI above its carrier, staggered so neighbours stay legible. */
        /* Strongest first, on one of two rows, and only where there is room. */
        uint8_t used[2][128];memset(used,0,sizeof(used));
        for(unsigned k=0;k<shown_n && plot.w<=128;k++) {
            const cell_lte_t *c=&shown[order[k]];
            if(c->hz<low || c->hz>=low+span)continue;
            int x=plot.x+(int)((uint64_t)(c->hz-low)*(plot.w-1)/span);
            snprintf(text,sizeof(text),"%u",c->pci);
            int len=(int)strlen(text),at=x-len/2;
            if(at<plot.x)at=plot.x;
            if(at+len>plot.x+plot.w)at=plot.x+plot.w-len;
            for(int row=0;row<2;row++) {
                bool free_=true;
                for(int i=-1;i<=len && free_;i++) {int col=at-plot.x+i;if(col>=0 && col<plot.w && used[row][col])free_=false;}
                if(!free_)continue;
                for(int i=0;i<len;i++)used[row][at-plot.x+i]=1;
                tui_put_str(sf,plot,at,plot.y+row,text,TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK));
                break;
            }
        }
        draw_marks(sf,plot,plot.y+plot.h,s);
    }
    if(s->busy && same && s->frequency>=low && s->frequency<low+span) {
        int x=plot.x+(int)((uint64_t)(s->frequency-low)*(plot.w-1)/span);
        tui_put_char(sf,plot,x,plot.y+plot.h-1,'^',yellow);
    }
    int legend=chart.y+chart.h-2;
    if(lte) {
        int x=chart.x+2;
        tui_put_str(sf,chart,x,legend,"=",TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK));tui_put_str(sf,chart,x+1,legend," carrier  ",white);
        tui_put_str(sf,chart,x+11,legend,"+",yellow);tui_put_str(sf,chart,x+12,legend," searched  ",white);
        tui_put_str(sf,chart,x+23,legend,"*",TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK));tui_put_str(sf,chart,x+24,legend," cell",white);
    } else tui_put_str(sf,chart,chart.x+2,legend,"-110..-30 dBFS / dashes: baseline",faint);
    snprintf(text,sizeof(text),"%.0f MHz",low/1e6);
    tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-1,text,white);
    snprintf(text,sizeof(text),"%.0f",(low+span/2)/1e6);
    tui_put_str(sf,chart,chart.x+chart.w/2-(int)strlen(text)/2,chart.y+chart.h-1,text,white);
    snprintf(text,sizeof(text),"%.0f MHz",(low+span)/1e6);
    tui_put_str(sf,chart,chart.x+chart.w-(int)strlen(text)-2,chart.y+chart.h-1,text,white);
}
/* PCI, carrier, level and what the MIB said, one row a cell. */
static int draw_cells(tui_surface *sf,tui_rect area,int y,int last)
{
    uint8_t cyan=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK),white=TUI_ATTR(TUI_WHITE,TUI_BLACK);
    uint8_t magenta=TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK),green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    table_area=tui_rect_make(area.x,y,area.w,0);
    tui_put_str(sf,area,area.x+1,y,"MHz      EARFCN PCI  POWER           BW  ANT",LS_ATTR_FAINT);
    y++;
    unsigned drawn=0;
    for(unsigned k=0;k<shown_n && y<=last-(k+1<shown_n?1:0);k++,y++,drawn++) {
        const cell_lte_t *c=&shown[order[k]];
        char text[24];int x=area.x+1;
        snprintf(text,sizeof(text),"%-8.3f",c->hz/1e6);tui_put_str(sf,area,x,y,text,white);x+=9;
        snprintf(text,sizeof(text),"%5u",c->earfcn);tui_put_str(sf,area,x,y,text,cyan);x+=7;
        snprintf(text,sizeof(text),"%3u",c->pci);tui_put_str(sf,area,x,y,text,magenta);x+=5;
        uint8_t ink=power_ink(c->rsrp10);
        if(c->rsrp10==CELL_NO_POWER) {
            tui_put_str(sf,area,x,y,"[--------] --",LS_ATTR_FAINT);
        } else {
            ls_meter_ascii(sf,area,x,y,10,(c->rsrp10/10.f+100.f)/70.f,ink);
            snprintf(text,sizeof(text),"%d",(int)lroundf(c->rsrp10/10.f));
            tui_put_str(sf,area,x+11,y,text,TUI_ATTR(ink,TUI_BLACK));
        }
        x+=16;
        snprintf(text,sizeof(text),"%4s",c->mib==CELL_MIB_OK?bw_text(c->n_rb):"-");
        tui_put_str(sf,area,x,y,text,c->mib==CELL_MIB_OK?green:LS_ATTR_FAINT);x+=5;
        if(c->mib==CELL_MIB_OK){snprintf(text,sizeof(text),"%dx",c->ports);tui_put_str(sf,area,x,y,text,white);}
        else tui_put_char(sf,area,x,y,c->mib==CELL_MIB_FAILED?'?':'-',LS_ATTR_FAINT);
    }
    table_area.h=y-table_area.y;
    if(drawn<shown_n) {
        char text[40];LS_SNPRINTF(t_scr_cell_u_more_tap_here, text,sizeof(text),"+%u more: tap here or N for the list",shown_n-drawn);
        tui_put_str(sf,area,area.x+1,y++,text,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
        table_area.h=y-table_area.y;
    }
    return y;
}
static void draw(tui_surface *sf,tui_rect area)
{
    if(cell_performance_active()){draw_high_rate(sf,area);return;}
    cell_monitor_get(&status);
    bool same_setup=status.band==band && status.manual_site==manual_site;
    bool lte=status.lte;
    static EXT_RAM_BSS_ATTR ls_gps_state_t gps;ls_gps_get(&gps);
    bool fix=cell_gps_valid(gps.fix,gps.last_fix_us,esp_timer_get_time(),gps.lat_deg,gps.lon_deg,gps.hdop);
    uint8_t white=TUI_ATTR(TUI_WHITE,TUI_BLACK), cyan=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK);
    uint8_t yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK),green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    uint8_t magenta=TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK),faint=LS_ATTR_FAINT;
    int controls=ls_btn_raised_height(area,11);
    if(ls_tui_is_wide() && area.h>=24 && controls<5)controls=5;
    tui_rect body=tui_rect_make(area.x,area.y,area.w,area.h-controls);
    bool wide=body.w>=90;
    tui_rect left=wide?tui_rect_make(body.x,body.y,body.w/2-1,body.h):body;
    tui_rect right=wide?tui_rect_make(body.x+body.w/2+1,body.y,body.w-body.w/2-1,body.h):body;
    char text[100];int y=left.y;
    load_cells(same_setup,lte);
    draw_tower(sf,left,left.x+left.w-9,left.y,same_setup && lte?status.lte_count:0);
    ls_safe_line(sf,left,y++,lte?"CELL WATCH / LTE CELL SEARCH":"CELL WATCH / SPECTRUM OBSERVATORY",cyan);
    unsigned first,last_earfcn;cell_band_earfcns(band,&first,&last_earfcn);
    snprintf(text,sizeof(text),"%s",cell_bands[band].name);
    tui_put_str(sf,left,left.x+1,y,text,white);
    snprintf(text,sizeof(text),"EARFCN %u-%u",first,last_earfcn);
    tui_put_str(sf,left,left.x+3+(int)strlen(cell_bands[band].name),y,text,yellow);
    y++;
    snprintf(text,sizeof(text),"%s",status.busy?"RECEIVING":same_setup && status.frequency?"CAPTURE HELD":"READY");
    tui_put_str(sf,left,left.x+1,y,text,status.busy?green:white);
    if(lte && same_setup && (status.busy || status.phase)) {
        static const char *const phase[]={"","OCCUPANCY","PSS / SSS","MIB"};
        tui_put_str(sf,left,left.x+3+(int)strlen(text),y,phase[status.phase<4?status.phase:0],magenta);
    }
    y++;
    if(!wide && body.w>=36 && body.h>=32) {
        frequency_readout(sf,left,y+1,same_setup?status.frequency:0,cyan);y+=7;
        if(same_setup && status.frequency) {
            unsigned e;int b=cell_band_for_hz(status.frequency);
            if(b>=0 && cell_hz_to_earfcn((unsigned)b,status.frequency,&e)) {
                snprintf(text,sizeof(text),"EARFCN %u",e);
                tui_put_str(sf,left,left.x+left.w-(int)strlen(text)-2,y-3,text,yellow);
            }
        }
    } else {
        if(same_setup && status.frequency)snprintf(text,sizeof(text),"%.4f MHz",status.frequency/1e6);
        else snprintf(text,sizeof(text),"---.---- MHz");
        ls_safe_line(sf,left,y++,text,cyan);
    }
    unsigned completed=same_setup?status.tune:0,total=same_setup?status.tunes:0;
    int mw=left.w-14;if(mw>40)mw=40;
    ls_meter_ascii(sf,left,left.x+1,y,mw,total?(float)completed/total:0.f,TUI_GREEN|TUI_BRIGHT);
    snprintf(text,sizeof(text),"%3u%%",total?completed*100/total:0);
    tui_put_str(sf,left,left.x+2+mw,y,text,green);
    y++;
    LS_SNPRINTF(t_scr_cell_s_u_u_1f, text,sizeof(text),"%s %u/%u  |  %.1f s  |  %s",lte?"POINTS":"TUNES",completed,total,
        (lte?status.scan_ms:status.elapsed_ms)/1000.,status.busy?"IN PROGRESS":completed?"FINISHED":"READY");
    ls_safe_line(sf,left,y++,text,white);
    ls_safe_line(sf,left,y++,control_message[0]?control_message:status.message,yellow);
    if(lte) {
        if(status.ppm_known) {
            LS_SNPRINTF(t_scr_cell_dongle_1f_ppm_u, text,sizeof(text),"DONGLE %+.1f ppm / %u cell%s / +-%.1f / %s",(double)status.ppm,status.ppm_cells,
                status.ppm_cells==1?"":"s",(double)status.ppm_spread,
                status.ppm_applied?"APPLIED":"not applied");
            if(status.ppm_applied)snprintf(text+strlen(text),sizeof(text)-strlen(text)," %+d",status.ppm_value);
        } else LS_SNPUTS(t_scr_cell_dongle_clock_unknown_until, text, sizeof(text), "DONGLE clock unknown until a cell is heard");
        ls_safe_line(sf,left,y++,text,status.ppm_known?(status.ppm_applied?green:yellow):faint);
        if(same_setup && (status.plateaus || status.candidates)) {
            LS_SNPRINTF(t_scr_cell_s_u_carriers_u, text,sizeof(text),"%s: %u carriers / %u points to search%s",status.gated?"GATED":"FULL",status.plateaus,
                status.candidates,status.mib_on?" / MIB on":" / MIB off");
        } else snprintf(text,sizeof(text),"%s search%s",status.gated?"GATED":"FULL",status.mib_on?" / MIB on":" / MIB off");
        ls_safe_line(sf,left,y++,text,white);
    }
    cell_report_status(text,sizeof(text));ls_safe_line(sf,left,y++,text,cyan);

    int chart_h=left.y+left.h-y-3;
    if(chart_h>(wide?16:body.h>=46?14:9))chart_h=wide?16:body.h>=46?14:9;
    if(chart_h>=8) {
        tui_rect chart=tui_rect_make(left.x+1,y+1,left.w-2,chart_h);
        draw_chart(sf,chart,&status,lte,same_setup,band);
        y=chart.y+chart.h+1;
    }
    int last=(wide?right.y+right.h:left.y+left.h)-4;
    int top=wide?right.y:y;
    if(lte) {
        LS_SNPRINTF(t_scr_cell_u_cells_u_searched, text,sizeof(text),"%u cells / %u searched / %u usable / %u discarded",same_setup?status.lte_count:0,
            same_setup?status.lte_attempted:0,same_setup?status.lte_usable:0,same_setup?status.lte_discarded:0);
        ls_safe_line(sf,wide?right:left,top++,text,cyan);
        if(same_setup && (status.mib_tried || status.mib_on)) {
            LS_SNPRINTF(t_scr_cell_mib_u_of_u, text,sizeof(text),"MIB %u of %u decoded%s",status.mib_done,status.mib_tried,status.mib_on?"":" (off)");
            ls_safe_line(sf,wide?right:left,top++,text,white);
        }
        if(shown_n)top=draw_cells(sf,wide?right:left,top,last);
        else {table_area=tui_rect_make(0,0,0,0);ls_safe_line(sf,wide?right:left,top++,LS_TEXT(t_scr_cell_waiting_for_repeated_cell, "Waiting for repeated cell sync"),faint);}
        if(top<=last+1)ls_safe_line(sf,wide?right:left,top++,LS_TEXT(t_scr_cell_pci_power_bandwidth_only, "PCI, power, bandwidth only; identity needs SIB."),white);
    } else {
        table_area=tui_rect_make(0,0,0,0);
        snprintf(text,sizeof(text),"%s | %s | %s",status.baseline && same_setup?(status.baseline_saved?"SD BASELINE":"RAM BASELINE"):"BASELINE NEEDED",manual_site?"LOCAL":fix?"GPS FIX":"GPS NO FIX",status.busy?(status.quiet?"IMU QUIET":"MOTION/IMU STALE"):"READY");
        ls_safe_line(sf,wide?right:left,top++,text,cyan);
        LS_SNPRINTF(t_scr_cell_u_passes_u_persistent, text,sizeof(text),"%u passes  /  %u persistent changes  /  %.1fs",status.passes,status.changed,status.elapsed_ms/1000.);
        ls_safe_line(sf,wide?right:left,top++,text,status.changed?yellow:white);
        ls_safe_line(sf,wide?right:left,top++,LS_TEXT(t_scr_cell_mhz_dbfs_rise_persistent, "MHz        dBFS   rise   persistent"),faint);
        for(unsigned i=0;same_setup && i<status.rows && top<=last;i++) {
            char delta[12];if(status.compared)snprintf(delta,sizeof(delta),"%+d",status.row[i].delta);else snprintf(delta,sizeof(delta),"--");
            LS_SNPRINTF(t_scr_cell_9_3f_4d_4s, text,sizeof(text),"%9.3f  %4d   %4s   %s",status.row[i].hz/1e6,status.row[i].power,delta,status.row[i].flagged?"REVIEW":"-");
            ls_safe_line(sf,wide?right:left,top++,text,status.row[i].flagged?yellow:white);
        }
    }
    ls_safe_line(sf,body,body.y+body.h-2,LS_TEXT(t_scr_cell_rf_changes_do_not, "RF changes do not prove a simulator."),yellow);
    ls_btn_t buttons[]={
        {status.busy?"STOP":"START","survey",'S',status.busy,false},
        {"BAND","range",'B',false,status.busy},
        {"LEARN","3 passes",'L',false,status.busy},
        {"LOAD","SD",'D',false,status.busy},
        {"SITE",manual_site?"LOCAL":"GPS",'G',manual_site,status.busy},
        {"HOME","exit",'H',false,false},
        {"REPORT","LoRa",'T',false,false},
        {"LTE","search",'E',false,status.busy},
        {"HIGH RATE","restart",'P',false,status.busy},
        ls_rsel_button(LS_RSEL_CELL),
        ls_opt_button(&CTX_CELL),
    };
    ls_btn_bar_raised(sf,tui_rect_make(area.x,area.y+area.h-controls,area.w,controls),buttons,11,focus);
}
static void cells_choose(int i){(void)i;}
/* Every cell found, strongest first, one row each with what is known of it. */
static void open_cells(void)
{
    cell_monitor_get(&status);
    load_cells(status.band==band && status.manual_site==manual_site,status.lte);
    char title[48];
    snprintf(title,sizeof(title),"LTE CELLS / %s",cell_bands[band].name);
    ls_picker_open(title,cells_choose);
    if(!shown_n){ls_picker_add("No cells yet",LS_TEXT(t_scr_cell_press_e_to_search, "Press E to search this band"));return;}
    for(unsigned k=0;k<shown_n;k++) {
        const cell_lte_t *c=&shown[order[k]];
        char label[LS_PICKER_TEXT],detail[LS_PICKER_DETAIL],power[12]="--",width[16]="BW ?";
        snprintf(label,sizeof(label),"%.3f MHz  PCI %u",c->hz/1e6,c->pci);
        if(c->rsrp10!=CELL_NO_POWER)snprintf(power,sizeof(power),"%.0fdB",c->rsrp10/10.);
        if(c->mib==CELL_MIB_OK)snprintf(width,sizeof(width),"%s %dp",bw_text(c->n_rb),c->ports);
        else if(c->mib==CELL_MIB_FAILED)snprintf(width,sizeof(width),"no MIB");
        snprintf(detail,sizeof(detail),"%u %s %s",c->earfcn,power,width);
        ls_picker_add(label,detail);
    }
}

static bool key(ls_tk_t k,char ch)
{
    if(k==LS_TK_ESC) {action(5);return true;}
    if(ls_btn_navigate(k,&slot,&focus,false)) return true;
    if(k==LS_TK_ENTER && focus>=0 && ls_btn_enabled(slot,focus)) {action(focus);return true;}
    if(k==LS_TK_CHAR && (ch=='n' || ch=='N') && !cell_performance_active()){open_cells();return true;}
    if(k==LS_TK_CHAR) {
        int i=ls_btn_shortcut(ch,LS_BTN_SLOT_SCREEN);
        if(i>=0) {action(i);return true;}
    }
    return false;
}
static bool touch(int col,int row)
{
    int i=ls_btn_hit(col,row);
    if(i<0 && !cell_performance_active() && table_area.h && tui_rect_contains(table_area,col,row)){open_cells();return true;}
    if(i<0)return false;
    action(i);return true;
}
static bool value_busy(ls_val_t *out)
{out->kind=LS_VAL_BOOL;if(cell_performance_active()){static EXT_RAM_BSS_ATTR cell_iq_status_t iq;cell_iq_get_status(&iq);out->i=iq.busy;}else {cell_status_t s;cell_monitor_get(&s);out->i=s.busy;}return true;}
static bool value_changed(ls_val_t *out)
{if(cell_performance_active())return false;cell_status_t s;cell_monitor_get(&s);if(!s.compared)return false;out->kind=LS_VAL_INT;out->i=s.changed;return true;}
static bool value_frequency(ls_val_t *out)
{if(cell_performance_active()){static EXT_RAM_BSS_ATTR cell_iq_status_t iq;cell_iq_get_status(&iq);if(!iq.busy || !iq.active_hz)return false;out->kind=LS_VAL_FLOAT;out->f=iq.active_hz/1e6f;return true;}cell_status_t s;cell_monitor_get(&s);if(!s.busy || !s.frequency)return false;out->kind=LS_VAL_FLOAT;out->f=s.frequency/1e6f;return true;}
static ls_act_status_t start(const ls_args_t *in,ls_val_t *out)
{(void)in;(void)out;if(!opened)return LS_ACT_BUSY;return (cell_performance_active()?cell_iq_auto_begin(hrf_frequency,auto_multi):request(CELL_WATCH))?LS_ACT_OK:LS_ACT_BUSY;}
static ls_act_status_t stop(const ls_args_t *in,ls_val_t *out)
{(void)in;(void)out;cell_iq_stop();cell_monitor_stop();return LS_ACT_OK;}
void ls_cell_publish(void)
{
    ls_value_publish("cell.busy","",value_busy);
    ls_value_publish("cell.changed_bins","bins",value_changed);
    ls_value_publish("cell.frequency","MHz",value_frequency);
    ls_action_register("cell.start","",LS_CAP_TUNE,start,LS_TEXT(t_scr_cell_start_visible_cell_mode, "start visible CELL mode; open CELL first"));
    ls_action_register("cell.stop","",LS_CAP_TUNE,stop,LS_TEXT(t_scr_cell_stop_all_cell_workers, "stop all CELL workers"));
}
const ls_tui_screen_t ls_scr_cell={
    .name="CELL WATCH",.hint=mode_hint,
    .enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch,
};
