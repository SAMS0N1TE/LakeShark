#include "esp_attr.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_app.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_freqs.h"
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

#include "../../ls_radio_panel.h"
#include "../../ls_motion.h"

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
static const uint32_t hrf_rates[]={8000000,10000000,19200000,20000000};
static void hrf_choose_frequency(int i){if(i>=0 && i<CELL_HRF_CHANNELS)hrf_frequency=cell_hrf_centers[i];}
/* "B13 / EARFCN 5230" for a frequency on the band table, "B2/B25" when it is
 * between two raster points. HIGH RATE has no survey window to go by, so an
 * overlap shows both bands: 763.0 is B14 in the US and B28 in Argentina. */
static void band_label(uint32_t hz,char *out,size_t n)
{
    if(!cell_band_label(hz,-1,out,n))LS_SNPUTS(t_scr_cell_outside_the_lte_bands, out, n, "outside the LTE bands");
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
static void enter(void) {snprintf(mode_hint,sizeof(mode_hint),"%s",cell_performance_active()?"S auto/stop C once B channel O options (V view H exit)":"S go/stop B band L learn E LTE N cells O options (D load G site T report P rate R radio H home)");opened=true;memset(&imu_display,0,sizeof(imu_display));cell_monitor_init();cell_report_init();ls_tui_radio_want(NULL);ls_gps_start();ls_imu_start();}
static void leave(void) {opened=false;cell_iq_stop();cell_monitor_wait_stopped();}
static bool request(int cmd)
{
    if(!opened) return false;
    control_message[0]=0;
    /* Indoors, or before the first fix, START used to answer "GPS NO FIX:
       wait or select SITE: LOCAL" and do nothing - in the field a press that
       does nothing. A survey with no fix is a survey of this spot, so it
       becomes one, and says so; SITE in OPTIONS puts GPS back. */
    if((cmd==CELL_WATCH || cmd==CELL_LEARN) && !manual_site) {
        static EXT_RAM_BSS_ATTR ls_gps_state_t gps;
        ls_gps_get(&gps);
        if(!cell_gps_valid(gps.fix,gps.last_fix_us,esp_timer_get_time(),gps.lat_deg,gps.lon_deg,gps.hdop)) {
            manual_site=true;
            snprintf(control_message,sizeof(control_message),"%s","No GPS fix: surveying this spot as LOCAL");
        }
    }
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
/* SAVE TO FREQS: the carrier the survey is on, as LTE, into mine.csv. */
static void o_freqs(const ls_opt_t *o)
{
    (void)o;
    cell_monitor_get(&status);
    ls_freqs_offer(status.frequency, "LTE", NULL, "cell", control_message, sizeof(control_message));
}
static void o_freqs_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    cell_monitor_get(&status);
    if (status.frequency) snprintf(out, n, "%.3f MHz", status.frequency / 1e6);
    else snprintf(out, n, "nothing tuned");
}
static const char *o_freqs_why(const ls_opt_t *o)
{
    (void)o;
    cell_monitor_get(&status);
    return status.frequency ? NULL : "Run a pass first";
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
/* The names follow cell_gains, and a baseline is only compared at the gain it
   was learned at, which the row says when it is not the usual one. */
static const char *const GAIN[CELL_GAINS]={"7.7 dB","14.4 dB","19.7 dB","25.4 dB","29.7 dB","37.2 dB"};
static int o_gain(const ls_opt_t *o)
{
    (void)o;cell_status_t s;cell_monitor_get(&s);
    for(int i=0;i<CELL_GAINS;i++)if(cell_gains[i]==s.gain)return i;
    return -1;
}
static void o_set_gain(const ls_opt_t *o,int v){(void)o;if(v>=0 && v<CELL_GAINS)cell_monitor_set_gain(cell_gains[v]);}
static void o_gain_show(const ls_opt_t *o,char *out,size_t n)
{
    int i=o_gain(o);
    if(i<0)snprintf(out,n,"?");
    else if(cell_gains[i]==CELL_GAIN)snprintf(out,n,"%s",GAIN[i]);
    else LS_SNPRINTF(t_scr_cell_s_learn_at_this_gain, out,n,"%s / LEARN at this gain",GAIN[i]);
}
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
/* What left the eleven-key bar: each row runs the action its key ran. */
static void o_act(const ls_opt_t *o){action(o->arg);}
static const char *o_lte_why(const ls_opt_t *o){(void)o;cell_monitor_get(&status);return status.busy?"Stop the survey first":status.lte?NULL:NULL;}
static void o_lte_show(const ls_opt_t *o,char *out,size_t n){(void)o;cell_monitor_get(&status);snprintf(out,n,"%s",status.lte?"searched":"run one");}
static void o_load_show(const ls_opt_t *o,char *out,size_t n){(void)o;cell_monitor_get(&status);snprintf(out,n,"%s",status.baseline?(status.baseline_saved?"on SD":"in RAM"):"none");}
static void o_radio_show(const ls_opt_t *o,char *out,size_t n){(void)o;snprintf(out,n,"%s",ls_rsel_button(LS_RSEL_CELL).value);}
static const ls_opt_t OPT_CELL[]={
    {.label="BAND",.kind=LS_OPT_ACTION,.act=o_band,.show=o_band_show,.why_not=o_cell_busy},
    {.label="LTE SEARCH",.kind=LS_OPT_ACTION,.arg=7,.act=o_act,.show=o_lte_show,.leaves=true,.why_not=o_lte_why},
    {.label="LOAD BASELINE",.kind=LS_OPT_ACTION,.arg=3,.act=o_act,.show=o_load_show,.why_not=o_cell_busy},
    {.label="SITE",.kind=LS_OPT_TOGGLE,.names=SITE,.get=o_site,.set=o_set_site,.why_not=o_cell_busy},
    {.label="RTL GAIN",.kind=LS_OPT_CYCLE,.names=GAIN,.n=CELL_GAINS,.get=o_gain,.set=o_set_gain,.show=o_gain_show,.why_not=o_cell_busy},
    {.label="LTE SCAN",.kind=LS_OPT_TOGGLE,.names=SCAN,.get=o_scan,.set=o_set_scan,.why_not=o_cell_busy},
    {.label="LTE MIB",.kind=LS_OPT_TOGGLE,.names=MIBS,.get=o_mib,.set=o_set_mib,.why_not=o_cell_busy},
    {.label="DONGLE PPM",.kind=LS_OPT_ACTION,.act=o_ppm,.show=o_ppm_show,.why_not=o_cell_busy},
    {.label="ALL CELLS",.kind=LS_OPT_ACTION,.act=o_cells,.show=o_cells_show},
    {.label="LORA REPORTS",.kind=LS_OPT_ACTION,.act=o_report,.show=o_report_show},
    {.label="RECENT SD LTE",.kind=LS_OPT_ACTION,.act=o_history},
    {.label="REPORT TEST / HEALTH",.kind=LS_OPT_ACTION,.act=o_report_test,.show=o_report_show},
    {.label="HIGH RATE",.kind=LS_OPT_ACTION,.arg=8,.act=o_act,.leaves=true,.why_not=o_cell_busy},
    {.label="RADIO",.kind=LS_OPT_ACTION,.arg=9,.act=o_act,.show=o_radio_show,.leaves=true},
    {.label="SAVE TO FREQS",.kind=LS_OPT_ACTION,.act=o_freqs,.show=o_freqs_show,.why_not=o_freqs_why},
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
static const char *const PLAN[]={"selected channel","all nine presets"};
static int o_plan(const ls_opt_t *o){(void)o;return auto_multi;}
static void o_set_plan(const ls_opt_t *o,int v){(void)o;auto_multi=v!=0;}
static const char *const HRF_CH[CELL_HRF_CHANNELS]={"739.000 MHz","751.000 MHz","881.500 MHz","1981.250 MHz","1992.500 MHz","2150.000 MHz",
    "763.000 MHz","773.000 MHz","785.500 MHz"};
static int o_hrf_ch(const ls_opt_t *o){(void)o;for(int i=0;i<CELL_HRF_CHANNELS;i++)if(cell_hrf_centers[i]==hrf_frequency)return i;return -1;}
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
    {.label="CHANNEL",.kind=LS_OPT_CYCLE,.names=HRF_CH,.n=CELL_HRF_CHANNELS,.get=o_hrf_ch,.set=o_set_hrf_ch,.show=o_hrf_ch_show,.why_not=o_hrf_busy},
    {.label="ONCE: SAMPLE RATE",.kind=LS_OPT_CYCLE,.names=RATE,.n=4,.get=o_rate,.set=o_set_rate,.show=o_rate_show,.why_not=o_hrf_busy},
    {.label="ONCE: LENGTH",.kind=LS_OPT_CYCLE,.names=LENGTH,.n=3,.get=o_length,.set=o_set_length,.why_not=o_hrf_busy},
    {.label="DISPLAY",.kind=LS_OPT_ACTION,.arg=3,.act=o_act,.leaves=true},
    {.label="EXIT TO NORMAL OS",.kind=LS_OPT_ACTION,.arg=5,.act=o_act,.leaves=true,.why_not=o_hrf_busy},
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
        if(i==2){ls_picker_open("RECEIVE CHANNEL",hrf_choose_frequency);for(int n=0;n<CELL_HRF_CHANNELS;n++){char text[32];snprintf(text,sizeof(text),"%.3f MHz",cell_hrf_centers[n]/1e6);char label[32];band_label(cell_hrf_centers[n],label,sizeof(label));ls_picker_add(text,label);}}
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
    int controls=ls_btn_raised_height(area,4);
    if(ls_tui_is_wide() && area.h>=24 && controls<5)controls=5;
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
    if(body.w>=32 && body.h>=26){ls_radio_frequency_draw(sf,body,y+1-body.y,shown,cyan);tui_put_str(sf,body,body.x+body.w-5,y+5,"MHz",cyan);y+=7;}
    else {snprintf(text,sizeof(text),"%.3f MHz",shown/1e6);ls_safe_line(sf,body,y++,text,cyan);}
    snprintf(text,sizeof(text),"Auto: %s",(iq.busy?iq.multi:auto_multi)?"9 preset channels":"selected channel");ls_safe_line(sf,body,y++,text,white);
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
    /* VIEW (V) and EXIT (H) are OPTIONS rows now, and still answer their keys. */
    ls_btn_t buttons[]={
        {iq.busy?"STOP":"AUTO",iq.busy?"save + end":"repeat",'S',iq.busy,false},
        {"ONCE","single",'C',false,iq.busy},
        {"CHANNEL","tune",'B',false,iq.busy},
        ls_opt_button(&CTX_HRF)};
    if(body.h<24)for(unsigned i=0;i<4;i++)buttons[i].value=NULL;
    ls_btn_bar_raised(sf,tui_rect_make(area.x,area.y+area.h-controls,area.w,controls),buttons,4,focus);
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
/* HOW HIGH, AGAINST WHAT.

   Against the pass's own noise floor, not a fixed dBFS range. The receiver
   alone reads about -68..-57 dBFS in a 25 kHz bin peak-held over its tune,
   so on -110..-30 its noise was more than half the plot and a carrier 15 dB
   up barely cleared it: the "huge noise" was the scale. The plot now runs
   from CHART_BELOW dB under the floor to CHART_DB above that, the floor is a
   dotted line, and a column within CHART_NOISE dB of it is faint - that is
   the receiver, and it should read as a calm baseline. Carriers the plateau
   reading found are green, as in the LTE view; anything else clear of the
   noise is cyan. Without a floor (no pass yet) the old fixed range stands. */
#define CHART_DB 60
#define CHART_BELOW 6
#define CHART_NOISE 5
typedef struct {int lo,levels;bool relative;} chart_scale_t;
static chart_scale_t chart_scale(const cell_status_t *s,int rows)
{
    if(s->floor!=CELL_MISSING) return (chart_scale_t){s->floor-CHART_BELOW,rows*4,true};
    return (chart_scale_t){-110,rows*4,false};
}
static int chart_level(chart_scale_t c,int db)
{
    int h=(db-c.lo)*c.levels/(c.relative?CHART_DB:80);
    return h<0?0:h>c.levels?c.levels:h;
}
/* The occupancy or survey trace as bars: one column a cell, eighth-block
 * tops, rising from the floor line, coloured by what the search made of it.
 * The receiver's own noise (within CHART_NOISE of the floor) is faint, so
 * it reads as a calm baseline and a carrier towers over it. */
static void draw_plot(tui_surface *sf,tui_rect plot,const cell_status_t *s,bool lte,bool same)
{
    uint8_t cyan=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK),green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    uint8_t magenta=TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK),yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK);
    uint8_t white=TUI_ATTR(TUI_WHITE,TUI_BLACK),faint=LS_ATTR_FAINT;
    if(!(same && s->spectrum_count)) {
        ls_fill_dither(sf,plot,LS_DITHER_LIGHT,lte?TUI_MAGENTA:TUI_CYAN);
        ls_dither_label(sf,plot,plot.h/2,lte?"Occupancy appears after the first pass":"Spectrum appears after a full pass",white);
        return;
    }
    chart_scale_t scale=chart_scale(s,plot.h*2);   /* levels in eighths */
    const int floor_e=scale.relative?chart_level(scale,s->floor):0;
    /* The floor: one thin line, the same in every column. */
    const int floor_row=plot.h-1-floor_e/8;
    for(int x=0;x<plot.w;x++)tui_put_char(sf,plot,plot.x+x,plot.y+floor_row,LS_TUI_TRACE(1),TUI_ATTR(TUI_CYAN,TUI_BLACK));
    for(int x=0;x<plot.w;x++) {
        unsigned begin=(unsigned)x*s->spectrum_count/(unsigned)plot.w,end=(unsigned)(x+1)*s->spectrum_count/(unsigned)plot.w;
        if(end<=begin)end=begin+1;
        if(end>s->spectrum_count)end=s->spectrum_count;
        int value=CELL_MISSING,reference=CELL_MISSING;uint8_t marks=0;
        for(unsigned i=begin;i<end;i++) {if(s->spectrum[i]>value)value=s->spectrum[i];if(s->reference[i]>reference)reference=s->reference[i];marks|=s->mark[i];}
        if(value==CELL_MISSING)continue;
        int top=chart_level(scale,value);
        if(top<=floor_e+1)top=floor_e+2;          /* a reading always shows */
        uint8_t ink=(marks&4)?magenta:(marks&1)?green:
            scale.relative && value<=s->floor+CHART_NOISE?faint:cyan;
        for(int row=0;row<plot.h;row++) {
            const int base=(plot.h-1-row)*8;
            int lo=floor_e+1>base?floor_e+1:base,hi=top<base+8?top:base+8;
            if(hi<=lo)continue;
            tui_put_char(sf,plot,plot.x+x,plot.y+row,LS_TUI_TRACE(hi-base),ink);
        }
        /* A baseline that was only receiver noise is the floor line
           already; a dash there would only jitter between two rows. */
        if(!lte && s->compared && reference!=CELL_MISSING && !(scale.relative && reference<=s->floor+CHART_NOISE)) {
            int rh=chart_level(scale,reference)/8;if(rh>=plot.h)rh=plot.h-1;
            tui_put_char(sf,plot,plot.x+x,plot.y+plot.h-1-rh,'-',yellow);
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
    int legend=chart.y+chart.h-2,x=chart.x+2;
    uint8_t green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    if(lte) {
        tui_put_str(sf,chart,x,legend,"=",green);tui_put_str(sf,chart,x+1,legend," carrier  ",white);
        tui_put_str(sf,chart,x+11,legend,"+",yellow);tui_put_str(sf,chart,x+12,legend," searched  ",white);
        tui_put_str(sf,chart,x+23,legend,"*",TUI_ATTR(TUI_MAGENTA|TUI_BRIGHT,TUI_BLACK));tui_put_str(sf,chart,x+24,legend," cell",white);
        x+=31;
    } else {
        tui_put_str(sf,chart,x,legend,"=",green);tui_put_str(sf,chart,x+1,legend," carrier",white);x+=11;
        if(same && s->compared){tui_put_str(sf,chart,x,legend,"-",yellow);tui_put_str(sf,chart,x+1,legend," baseline",white);x+=12;}
    }
    /* What the dotted line is, where there is room for it. */
    if(same && s->spectrum_count && s->floor!=CELL_MISSING) {
        snprintf(text,sizeof(text),"floor %d dBFS",s->floor);
        int at=chart.x+chart.w-2-(int)strlen(text);
        if(at>=x)tui_put_str(sf,chart,at,legend,text,faint);
    }
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
/* The bar's four keys and which action each one is; the rest of the old
   eleven answer their letters and sit in OPTIONS. */
static const int BAR_ACT[4]={0,1,2,10};
static const char KEY_ACT[]="sbldghtep";   /* 0..8; r 9, o 10 */
/* The header band: the band and its EARFCN range, the mast whose arcs light
   as cells are heard (in a survey: as the pass goes), the frequency in the
   house digits, a rail of chips for the state, and the pass meter. Returns
   the row under it. */
static int header(tui_surface *sf,tui_rect a,bool lte,bool same,bool fix,bool wide)
{
    const uint8_t hue=lte?TUI_MAGENTA:TUI_CYAN;
    uint8_t white=TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK),yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK),green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK);
    const int h=wide?6:11;
    tui_rect box=tui_rect_make(a.x,a.y,a.w,h);
    ls_panel_box(sf,box,lte?"CELL WATCH / LTE CELL SEARCH":"CELL WATCH / SPECTRUM OBSERVATORY",hue);
    const unsigned completed=same?status.tune:0,total=same?status.tunes:0;
    unsigned lit=same && lte?status.lte_count:0;
    if(!lte && status.busy && total)lit=completed*3/total;
    draw_tower(sf,box,box.x+box.w-9,box.y+1,lit);
    char text[64];unsigned first,last;cell_band_earfcns(band,&first,&last);
    tui_put_str(sf,box,box.x+2,box.y+1,cell_bands[band].name,white);
    snprintf(text,sizeof(text),"EARFCN %u-%u",first,last);
    tui_put_str(sf,box,box.x+4+(int)strlen(cell_bands[band].name),box.y+1,text,yellow);
    int y=box.y+2;
    const uint32_t hz=same?status.frequency:0;
    if(!wide) {
        tui_rect digits=tui_rect_make(box.x+1,box.y,box.w-12,box.h);
        if(hz)ls_radio_frequency_draw(sf,digits,2,hz,TUI_ATTR(hue|TUI_BRIGHT,TUI_BLACK));
        else ls_radio_digits_draw(sf,digits,2,"0.0000",LS_ATTR_FAINT);
        tui_put_str(sf,box,box.x+box.w-9,box.y+6,hz?"MHz":"idle",hz?white:LS_ATTR_FAINT);
        y+=5;
        /* By the window being swept: 795 MHz in B28 hi is EARFCN 9580,
         * not the B20 6190 the table's first match would give. */
        unsigned e,b;
        if(hz && cell_bands_at(hz,(int)band,&b,1) && cell_hz_to_earfcn(b,hz,&e)) {
            snprintf(text,sizeof(text),"EARFCN %u",e);
            tui_put_str(sf,box,box.x+2,y,text,yellow);
        }
        y++;
    } else {
        if(hz)snprintf(text,sizeof(text),"%.4f MHz",hz/1e6);else snprintf(text,sizeof(text),"---.---- MHz");
        tui_put_str(sf,box,box.x+2,y++,text,TUI_ATTR(hue|TUI_BRIGHT,TUI_BLACK));
    }
    /* Chips: what it is doing, where it thinks it is, what it has. */
    int x=box.x+2;
    x=ls_radio_chip(sf,box,x,y,status.busy?"RECEIVING":same && hz?"HELD":"READY",status.busy,TUI_GREEN)+1;
    if(lte && same && (status.busy || status.phase)) {
        static const char *const phase[]={"","OCCUPANCY","PSS / SSS","MIB"};
        x=ls_radio_chip(sf,box,x,y,phase[status.phase<4?status.phase:0],true,TUI_MAGENTA)+1;
    }
    x=ls_radio_chip(sf,box,x,y,manual_site?"LOCAL":fix?"GPS FIX":"NO FIX",manual_site||fix,manual_site?TUI_YELLOW:TUI_CYAN)+1;
    if(!lte)x=ls_radio_chip(sf,box,x,y,status.baseline && same?(status.baseline_saved?"SD BASELINE":"RAM BASELINE"):"NO BASELINE",status.baseline && same,TUI_YELLOW)+1;
    else x=ls_radio_chip(sf,box,x,y,status.mib_on?"MIB ON":"MIB OFF",status.mib_on,TUI_CYAN)+1;
    y++;
    int mw=box.w-14;if(mw>40)mw=40;
    ls_meter_ascii(sf,box,box.x+2,y,mw,total?(float)completed/total:0.f,TUI_GREEN|TUI_BRIGHT);
    snprintf(text,sizeof(text),"%3u%%",total?completed*100/total:0);
    tui_put_str(sf,box,box.x+3+mw,y,text,green);
    (void)x;
    return box.y+box.h;
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
    uint8_t yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK),green=TUI_ATTR(TUI_GREEN|TUI_BRIGHT,TUI_BLACK),faint=LS_ATTR_FAINT;
    int controls=ls_btn_raised_height(area,4);
    if(ls_tui_is_wide() && area.h>=24 && controls<5)controls=5;
    tui_rect body=tui_rect_make(area.x,area.y,area.w,area.h-controls);
    bool wide=body.w>=90;
    tui_rect left=wide?tui_rect_make(body.x,body.y,body.w/2-1,body.h):body;
    tui_rect right=wide?tui_rect_make(body.x+body.w/2+1,body.y,body.w-body.w/2-1,body.h):body;
    char text[100];
    load_cells(same_setup,lte);
    int y=header(sf,left,lte,same_setup,fix,wide);
    unsigned completed=same_setup?status.tune:0,total=same_setup?status.tunes:0;
    LS_SNPRINTF(t_scr_cell_s_u_u_1f, text,sizeof(text),"%s %u/%u  |  %.1f s  |  %s",lte?"POINTS":"TUNES",completed,total,
        (lte?status.scan_ms:status.elapsed_ms)/1000.,status.busy?"IN PROGRESS":completed?"FINISHED":"READY");
    ls_safe_line(sf,left,y++,text,white);
    /* Clipping outranks the worker's message: the pass was neither learned
       nor compared, and the next one is no better until the gain comes down.
       It stays up through that next sweep, until a pass comes back clean. */
    if(!control_message[0] && same_setup && status.overload) {
        LS_SNPRINTF(t_scr_cell_rtl_overloaded_at_lower, text,sizeof(text),"RTL overloaded at %lu MHz - lower gain",
            (unsigned long)((status.overload_hz+500000)/1000000));
        ls_safe_line(sf,left,y++,text,TUI_ATTR(TUI_RED|TUI_BRIGHT,TUI_BLACK));
    } else ls_safe_line(sf,left,y++,control_message[0]?control_message:status.message,yellow);
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
        /* Only when it is not the usual gain: a baseline is per gain. */
        if(status.gain!=CELL_GAIN)snprintf(text+strlen(text),sizeof(text)-strlen(text)," | %.1f dB",status.gain/10.);
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
    /* LOAD, SITE, HOME, REPORT, LTE, HIGH RATE and RADIO are OPTIONS rows
       now, and still answer D G H T E P R. */
    ls_btn_t buttons[]={
        {status.busy?"STOP":"START",lte?"search":"survey",'S',status.busy,false},
        {"BAND","range",'B',false,status.busy},
        {"LEARN","3 passes",'L',false,status.busy},
        ls_opt_button(&CTX_CELL),
    };
    ls_btn_bar_raised(sf,tui_rect_make(area.x,area.y+area.h-controls,area.w,controls),buttons,4,focus);
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

static const int HRF_ACT[4]={0,1,2,4};
static bool key(ls_tk_t k,char ch)
{
    if(k==LS_TK_ESC) {action(5);return true;}
    if(ls_btn_navigate(k,&slot,&focus,false)) return true;
    const bool high=cell_performance_active();
    if(k==LS_TK_ENTER && focus>=0 && focus<4 && ls_btn_enabled(slot,focus)) {action(high?HRF_ACT[focus]:BAR_ACT[focus]);return true;}
    if(k!=LS_TK_CHAR)return false;
    if(ch>='A' && ch<='Z')ch+='a'-'A';
    if(high) {
        const char *p=strchr("scbvoh",ch);
        if(ch && p){action((int)(p-"scbvoh"));return true;}
        return false;
    }
    if(ch=='n'){open_cells();return true;}
    if(ch=='r'){action(9);return true;}
    if(ch=='o'){action(10);return true;}
    const char *p=strchr(KEY_ACT,ch);
    if(ch && p){action((int)(p-KEY_ACT));return true;}
    return false;
}
static bool touch(int col,int row)
{
    int i=ls_btn_hit(col,row);
    if(i<0 && !cell_performance_active() && table_area.h && tui_rect_contains(table_area,col,row)){open_cells();return true;}
    if(i<0 || i>=4)return false;
    action(cell_performance_active()?HRF_ACT[i]:BAR_ACT[i]);return true;
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
