#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_app.h"
#include "../../ls_picker.h"
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
static bool manual_site;
static char report_peers[LS_MESH_MAX_PEERS][17];
static uint32_t hrf_frequency=739000000,hrf_rate=8000000,hrf_ms=80;
static bool auto_multi;
static char control_message[80];
static ls_imu_status_policy_t imu_display;
static const uint32_t hrf_centers[]={739000000,751000000,881500000,1981250000,1992500000,2150000000};
static const uint32_t hrf_rates[]={8000000,10000000,19200000,20000000};
static void hrf_choose_frequency(int i){if(i>=0 && i<6)hrf_frequency=hrf_centers[i];}
static void hrf_choose_rate(int i){if(i>=0 && i<4)hrf_rate=hrf_rates[i];}
static void hrf_choose_duration(int i){if(i>=0 && i<3)hrf_ms=i==0?30:i==1?80:100;}
static void performance_choose(int i){if(i==1)cell_performance_reboot(!cell_performance_active());}
static void performance_picker(void)
{
    ls_picker_open(cell_performance_active()?"RETURN TO NORMAL OS?":"HIGH RATE / RESTART REQUIRED",performance_choose);
    ls_picker_add("Stay here","Cancel");
    ls_picker_add(cell_performance_active()?"Restart normal OS":"Restart into HackRF","GPS + IMU + LoRa");
}
static void report_choose(int i)
{if(i==0)cell_report_target("");else if(i>0 && i<=LS_MESH_MAX_PEERS)cell_report_target(report_peers[i-1]);}
static void choose(int i) {if(i>=0 && (unsigned)i<cell_band_count) band=(unsigned)i;}
static void enter(void) {opened=true;memset(&imu_display,0,sizeof(imu_display));cell_monitor_init();cell_report_init();ls_tui_radio_want(NULL);ls_gps_start();ls_imu_start();}
static void leave(void) {opened=false;cell_iq_stop();cell_monitor_wait_stopped();}
static bool request(int cmd)
{
    if(!opened) return false;
    if(cmd!=CELL_LOAD) ls_tui_radio_want(NULL);
    return cell_monitor_request(cmd,band,manual_site);
}
static void view_choose(int i)
{
    if(i==0)ls_tui_set_daylight(!ls_tui_daylight());
    if(i==1){ls_tui_set_font_index(ls_tui_font_index()==2?0:2);ls_tui_screen_request_regrid();}
    if(i==2)ls_tui_set_crisp_text(!ls_tui_crisp_text());
}
static void more_choose(int i)
{
    if(i==0)auto_multi=!auto_multi;
    if(i==1){ls_picker_open("ONCE: SAMPLE RATE",hrf_choose_rate);for(int n=0;n<4;n++){char text[32];snprintf(text,sizeof(text),"%.1f MS/s",hrf_rates[n]/1e6);ls_picker_add(text,n==0?"Validated for MIB":"Experimental / may lose samples");}}
    if(i==2){ls_picker_open("ONCE: CAPTURE LENGTH",hrf_choose_duration);ls_picker_add("30 ms","Short burst");ls_picker_add("80 ms","Recommended");ls_picker_add("100 ms","Longer burst");}
    if(i==3){ls_picker_open("REPORT DESTINATION",report_choose);ls_picker_add("Reports OFF","Disable reports");for(int n=0;n<LS_MESH_MAX_PEERS;n++){ls_mesh_peer_t peer;if(!ls_mesh_peer_at(n,&peer))break;snprintf(report_peers[n],sizeof(report_peers[n]),"%s",peer.id);ls_picker_add(peer.name[0]?peer.name:peer.id,peer.id);}}
}
static void action(int i)
{
    if(cell_performance_active()) {
        cell_iq_status_t iq;cell_iq_get_status(&iq);
        if(i==0 && iq.busy){cell_iq_stop();return;}
        if(i==3){ls_picker_open("DISPLAY",view_choose);ls_picker_add(ls_tui_daylight()?"Dark theme":"Light theme","Change now");ls_picker_add(ls_tui_font_index()==2?"Compact text":"Large text","Change text + touch targets");ls_picker_add(ls_tui_crisp_text()?"Smooth text":"Crisp text","Change edge rendering");return;}
        if(iq.busy)return;
        control_message[0]=0;
        if(i==0){if(!cell_iq_auto_begin(hrf_frequency,auto_multi))snprintf(control_message,sizeof(control_message),"Auto needs mounted SD + idle HackRF");}
        if(i==1){if(!cell_iq_hackrf_begin(hrf_frequency,hrf_rate,hrf_ms))snprintf(control_message,sizeof(control_message),"Cannot start capture");}
        if(i==2){ls_picker_open("RECEIVE CHANNEL",hrf_choose_frequency);for(int n=0;n<6;n++){char text[32];snprintf(text,sizeof(text),"%.3f MHz",hrf_centers[n]/1e6);ls_picker_add(text,"Preset / not full band coverage");}}
        if(i==4){ls_picker_open("CAPTURE OPTIONS",more_choose);ls_picker_add(auto_multi?"Auto: six presets":"Auto: selected channel","Tap to switch plan");ls_picker_add("Once: sample rate","Auto always uses 8 MS/s");ls_picker_add("Once: capture length","Auto always uses 80 ms");ls_picker_add("LoRa reporting","High-rate results stay on SD");}
        if(i==5)performance_picker();
        return;
    }
    cell_monitor_get(&status);
    if(i==0) {if(status.busy)cell_monitor_stop();else request(CELL_WATCH);}
    else if(i==1 && !status.busy) {
        ls_picker_open("CELLULAR DOWNLINK BAND",choose);
        for(unsigned b=0;b<cell_band_count;b++) {
            char text[64];snprintf(text,sizeof(text),"%.0f - %.0f MHz",cell_bands[b].low/1e6,cell_bands[b].high/1e6);
            ls_picker_add(cell_bands[b].name,text);
        }
    } else if(i==2 && !status.busy) request(CELL_LEARN);
    else if(i==3 && !status.busy) request(CELL_LOAD);
    else if(i==4 && !status.busy) manual_site=!manual_site;
    else if(i==5) {
        const ls_app_t *a=ls_app_by_id("home");
        if(a) ls_tui_screen_show(ls_tui_screen_index_of(a->screen));
    } else if(i==6) {
        ls_picker_open("LORA REPORT DESTINATION",report_choose);
        ls_picker_add("Reports OFF","Stop automatic addressed reports");
        for(int n=0;n<LS_MESH_MAX_PEERS;n++) {
            ls_mesh_peer_t peer;if(!ls_mesh_peer_at(n,&peer))break;
            snprintf(report_peers[n],sizeof(report_peers[n]),"%s",peer.id);
            ls_picker_add(peer.name[0]?peer.name:peer.id,peer.id);
        }
    } else if(i==7 && !status.busy)request(CELL_LTE);
    else if(i==8 && !status.busy)performance_picker();
}
static void draw_high_rate(tui_surface *sf,tui_rect area)
{
    cell_iq_status_t iq;cell_iq_get_status(&iq);
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
    /* Rounded portrait corners reduce the real bar width to two columns.
     * Reserve three full-height rows so labels and hints remain separate. */
    if(area.w<48 && area.h>=35)controls=15;
    tui_rect body=tui_rect_make(area.x,area.y,area.w,area.h-controls);
    int y=body.y;char text[120];
    if(body.h<24) {
        ls_safe_line(sf,body,y++,"CELL WATCH / AUTO LTE OBSERVATIONS",cyan);
        snprintf(text,sizeof(text),"%s / %.3f MHz / %lu attempts / %lu MIB",
            iq.stopping?"STOPPING":iq.busy?"RUNNING":"READY",(iq.busy?iq.active_hz:hrf_frequency)/1e6,
            (unsigned long)iq.attempts,(unsigned long)iq.decoded);
        ls_safe_line(sf,body,y++,text,yellow);
        snprintf(text,sizeof(text),"Auto: %s / 8 MS/s, 80 ms bursts / processing gaps",
            (iq.busy?iq.multi:auto_multi)?"six presets":"selected channel");ls_safe_line(sf,body,y++,text,white);
        snprintf(text,sizeof(text),"Last %.3f MHz / PCI %d / MIB %s / CRC x%d",iq.hz/1e6,
            iq.lte_found?iq.lte.pci:-1,iq.mib_found?"decoded":"unconfirmed",iq.mib.frames);ls_safe_line(sf,body,y++,text,cyan);
        ls_gps_state_t gps;ls_gps_get(&gps);
        bool fix=gps.fix && gps.last_fix_us>0 && esp_timer_get_time()-gps.last_fix_us<5000000;
        snprintf(text,sizeof(text),"GPS %s (%u sats) / 9 AXIS %s / SD %s",fix?"FIX":"SEEKING",gps.sats_visible,
            imu_text,ls_sdcard_mounted()?"READY":"MISSING");ls_safe_line(sf,body,y++,text,white);
        if(y<body.y+body.h-2)ls_safe_line(sf,body,y++,control_message[0]?control_message:iq.message,white);
        ls_safe_line(sf,body,body.y+body.h-1,"Detection unvalidated / identity and SIB1 still missing",yellow);
        goto render_controls;
    }
    ls_safe_line(sf,body,y++,"CELL WATCH / PASSIVE LTE",cyan);
    const char *phase=iq.phase==CELL_IQ_FILTERING?"Filtering samples":iq.phase==CELL_IQ_SYNCHRONIZING?"Finding LTE cell":iq.phase==CELL_IQ_DECODING_MIB?"Checking broadcast CRC":iq.phase==CELL_IQ_SAVING?"Saving evidence":iq.phase==CELL_IQ_WAITING?"Waiting for next capture":"Receiving samples";
    ls_safe_line(sf,body,y++,iq.stopping?"STOPPING / finishing this save":iq.busy?phase:"READY / start Auto or Once",yellow);
    uint32_t shown=iq.busy?iq.active_hz:hrf_frequency;
    if(body.w>=32 && body.h>=26){frequency_readout(sf,body,y+1,shown,cyan);y+=7;}
    else {snprintf(text,sizeof(text),"%.3f MHz",shown/1e6);ls_safe_line(sf,body,y++,text,cyan);}
    snprintf(text,sizeof(text),"Auto: %s",(iq.busy?iq.multi:auto_multi)?"6 preset channels":"selected channel");ls_safe_line(sf,body,y++,text,white);
    ls_safe_line(sf,body,y++,"8 MS/s / 80 ms repeated bursts",white);
    snprintf(text,sizeof(text),"%lu attempts / %lu MIB decodes",(unsigned long)iq.attempts,(unsigned long)iq.decoded);ls_safe_line(sf,body,y++,text,cyan);
    if(iq.lte_found)snprintf(text,sizeof(text),"Last: %.3f MHz / PCI %d",iq.hz/1e6,iq.lte.pci);
    else snprintf(text,sizeof(text),iq.lte_checked?"No LTE sync in last capture":"LTE evidence: waiting");
    ls_safe_line(sf,body,y++,text,white);
    if(iq.mib_found)snprintf(text,sizeof(text),"MIB: %.1f MHz / CRC x%d",iq.mib.n_rb==6?1.4:iq.mib.n_rb/5.,iq.mib.frames);
    else snprintf(text,sizeof(text),"MIB: %s",iq.mib_checked?"CRC not confirmed":"not decoded");
    ls_safe_line(sf,body,y++,text,cyan);
    if(body.h>=25)y++;
    ls_gps_state_t gps;ls_gps_get(&gps);
    bool fix=gps.fix && gps.last_fix_us>0 && esp_timer_get_time()-gps.last_fix_us<5000000;
    snprintf(text,sizeof(text),"GPS %s / %u seen / %u used",fix?"FIX":ls_gps_running()?"SEEKING":"OFF",gps.sats_visible,gps.sats_used);ls_safe_line(sf,body,y++,text,fix?cyan:yellow);
    if(fix){snprintf(text,sizeof(text),"%.5f, %.5f",gps.lat_deg,gps.lon_deg);ls_safe_line(sf,body,y++,text,white);}
    snprintf(text,sizeof(text),"9 AXIS: %s",imu_text);ls_safe_line(sf,body,y++,text,imu_ok && imu_display.mag_valid?cyan:yellow);
    snprintf(text,sizeof(text),"SD %s / IQ %llu of 256 MiB",ls_sdcard_mounted()?"READY":"MISSING",(unsigned long long)(iq.raw_bytes/(1024*1024)));ls_safe_line(sf,body,y++,text,white);
    if(control_message[0])ls_safe_line(sf,body,y++,control_message,yellow);
    else if(iq.message[0])ls_safe_line(sf,body,y++,iq.message,white);
    if(y<body.y+body.h-3)ls_safe_line(sf,body,y++,"Start before travel; Stop to end.",white);
    ls_safe_line(sf,body,body.y+body.h-2,"Detection: not yet validated",yellow);
    ls_safe_line(sf,body,body.y+body.h-1,"Identity / SIB1 still missing",yellow);
render_controls:;
    ls_btn_t buttons[]={
        {iq.busy?"STOP":"AUTO",iq.busy?"save + end":"repeat",'S',iq.busy,false},
        {"ONCE","single",'C',false,iq.busy},
        {"CHANNEL","tune",'B',false,iq.busy},
        {"VIEW","theme/size",'V',false,false},
        {"OPTIONS","setup",'O',false,iq.busy},
        {"EXIT","normal OS",'H',false,iq.busy}};
    if(body.h<24)for(unsigned i=0;i<6;i++)buttons[i].value=NULL;
    ls_btn_bar_raised(sf,tui_rect_make(area.x,area.y+area.h-controls,area.w,controls),buttons,6,focus);
}
static void draw(tui_surface *sf,tui_rect area)
{
    if(cell_performance_active()){draw_high_rate(sf,area);return;}
    cell_monitor_get(&status);
    bool same_setup=status.band==band && status.manual_site==manual_site;
    uint8_t white=TUI_ATTR(TUI_WHITE,TUI_BLACK), cyan=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK);
    uint8_t yellow=TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK), faint=LS_ATTR_FAINT;
    int controls=ls_btn_raised_height(area,9);
    tui_rect body=tui_rect_make(area.x,area.y,area.w,area.h-controls);
    char text[100];int y=body.y;
    ls_safe_line(sf,body,y++,status.lte?"CELL WATCH / PHYSICAL CELL SEARCH":"CELL WATCH / SPECTRUM OBSERVATORY",cyan);
    snprintf(text,sizeof(text),"%s  |  %s",cell_bands[band].name,status.busy?"RECEIVING":"CAPTURE HELD");
    ls_safe_line(sf,body,y++,text,white);
    if(body.w>=36 && body.h>=32) {
        frequency_readout(sf,body,y+1,same_setup?status.frequency:0,cyan);y+=7;
    } else {
        snprintf(text,sizeof(text),"%.4f MHz",same_setup?status.frequency/1e6:0.);
        ls_safe_line(sf,body,y++,text,cyan);
    }
    int progress_width=body.w-4;
    unsigned completed=same_setup?status.tune:0,total=same_setup?status.tunes:0;
    int fill=total?(int)((uint64_t)completed*progress_width/total):0;
    if(fill>progress_width)fill=progress_width;
    int pulse=(int)(esp_timer_get_time()/160000)%4;
    for(int x=0;x<progress_width;x++)tui_put_char(sf,body,body.x+2+x,y,
        x<fill?LS_TUI_BLOCK_FULL:LS_TUI_SHADE_25,x<fill?cyan:faint);
    if(status.busy && fill<progress_width)tui_put_char(sf,body,body.x+2+fill,y,"|/-\\"[pulse],cyan);
    y++;
    snprintf(text,sizeof(text),"%s %u/%u  |  %u%%  |  %s",status.lte?"FREQUENCIES":"TUNES",completed,total,
        total?completed*100/total:0,status.busy?"IN PROGRESS":completed?"FINISHED":"READY");
    ls_safe_line(sf,body,y++,text,white);
    ls_safe_line(sf,body,y++,status.message,yellow);
    cell_report_status(text,sizeof(text));ls_safe_line(sf,body,y++,text,cyan);

    if(body.h>=32) {
        int height=body.h>=46?14:9;
        tui_rect chart=tui_rect_make(body.x+2,y+1,body.w-4,height);
        ls_panel_box(sf,chart,status.lte?"CELL SYNC / SSS MATCH":"RF POWER / LAST COMPLETE PASS",TUI_CYAN);
        tui_rect plot=tui_rect_make(chart.x+2,chart.y+2,chart.w-4,chart.h-4);
        for(int row=0;row<plot.h;row++)for(int col=0;col<plot.w;col++)
            if(row%3==0 && col%3==0)tui_put_char(sf,plot,plot.x+col,plot.y+row,'.',faint);
        if(same_setup && status.lte) {
            for(unsigned i=0;i<status.lte_count;i++) {
                double fraction=((double)status.cell[i].hz-cell_bands[band].low)/(cell_bands[band].high-cell_bands[band].low);
                int x=plot.x+(int)(fraction*(plot.w-1));
                int h=(int)(fminf(1,status.cell[i].sss)*(plot.h-2));
                for(int row=0;row<h;row++)tui_put_char(sf,plot,x,plot.y+plot.h-1-row,LS_TUI_SHADE_75,cyan);
                snprintf(text,sizeof(text),"%d",status.cell[i].pci);
                int label=x-1;if(label>plot.x+plot.w-4)label=plot.x+plot.w-4;
                tui_put_str(sf,plot,label,plot.y+plot.h-h-2,text,white);
            }
            if(!status.lte_count)tui_put_str(sf,plot,plot.x+1,plot.y+plot.h/2,"Waiting for repeated cell sync",white);
        } else if(same_setup && status.spectrum_count) {
            for(int x=0;x<plot.w;x++) {
                unsigned begin=(unsigned)x*status.spectrum_count/plot.w;
                unsigned end=(unsigned)(x+1)*status.spectrum_count/plot.w;
                if(end<=begin)end=begin+1;
                if(end>status.spectrum_count)end=status.spectrum_count;
                int value=CELL_MISSING,reference=CELL_MISSING;
                for(unsigned i=begin;i<end;i++){if(status.spectrum[i]>value)value=status.spectrum[i];if(status.reference[i]>reference)reference=status.reference[i];}
                if(value==CELL_MISSING)continue;
                int h=(value+110)*(plot.h-1)/80;if(h<0)h=0;if(h>=plot.h)h=plot.h-1;
                for(int row=0;row<h;row++)tui_put_char(sf,plot,plot.x+x,plot.y+plot.h-1-row,LS_TUI_SHADE_25,cyan);
                tui_put_char(sf,plot,plot.x+x,plot.y+plot.h-1-h,LS_TUI_TRACE(4),cyan);
                if(status.compared && reference!=CELL_MISSING) {
                    int rh=(reference+110)*(plot.h-1)/80;if(rh<0)rh=0;if(rh>=plot.h)rh=plot.h-1;
                    tui_put_char(sf,plot,plot.x+x,plot.y+plot.h-1-rh,'.',yellow);
                }
            }
        } else tui_put_str(sf,plot,plot.x+1,plot.y+plot.h/2,"Spectrum appears after a full pass",white);
        if(status.busy && same_setup && status.frequency) {
            double fraction=((double)status.frequency-cell_bands[band].low)/(cell_bands[band].high-cell_bands[band].low);
            int x=plot.x+(int)(fraction*(plot.w-1));
            tui_put_char(sf,plot,x,plot.y,'v',yellow);
            for(int row=1;row<plot.h;row+=2)tui_put_char(sf,plot,x,plot.y+row,':',faint);
        }
        snprintf(text,sizeof(text),"%.0f MHz",cell_bands[band].low/1e6);
        tui_put_str(sf,chart,chart.x+2,chart.y+chart.h-1,text,white);
        snprintf(text,sizeof(text),"%.0f MHz",cell_bands[band].high/1e6);
        tui_put_str(sf,chart,chart.x+chart.w-9,chart.y+chart.h-1,text,white);
        y=chart.y+chart.h+1;
    }
    if(status.lte) {
        snprintf(text,sizeof(text),"%u CELL%s  /  %s  /  %.1fs per check",status.lte_count,status.lte_count==1?"":"S",status.sd?"SD LOG":"NO SD LOG",status.elapsed_ms/1000.);
        ls_safe_line(sf,body,y++,text,cyan);
        for(unsigned i=0;i<status.lte_count && y<body.y+body.h-3;i++) {
            snprintf(text,sizeof(text),"%.3f MHz   PCI %3d   %d repeats   %.2f",status.cell[i].hz/1e6,status.cell[i].pci,status.cell[i].hits,(double)status.cell[i].sss);
            ls_safe_line(sf,body,y++,text,white);
        }
        ls_safe_line(sf,body,y++,"PCI only. Network identity needs SIB decoding.",white);
    } else {
        snprintf(text,sizeof(text),"SD %s  |  %s  |  %s",status.baseline && same_setup?"BASELINE READY":"BASELINE NEEDED",manual_site?"LOCAL":"GPS AUTO",status.quiet?"IMU QUIET":"MOTION/IMU?");
        ls_safe_line(sf,body,y++,text,cyan);
        snprintf(text,sizeof(text),"%u passes  /  %u persistent changes  /  %.1fs",status.passes,status.changed,status.elapsed_ms/1000.);
        ls_safe_line(sf,body,y++,text,status.changed?yellow:white);
        ls_safe_line(sf,body,y++,"MHz        dBFS   rise   persistent",white);
        for(unsigned i=0;same_setup && i<status.rows && y<body.y+body.h-3;i++) {
            char delta[12];if(status.compared)snprintf(delta,sizeof(delta),"%+d",status.row[i].delta);else snprintf(delta,sizeof(delta),"--");
            snprintf(text,sizeof(text),"%9.3f  %4d   %4s   %s",status.row[i].hz/1e6,status.row[i].power,delta,status.row[i].flagged?"REVIEW":"-");
            ls_safe_line(sf,body,y++,text,status.row[i].flagged?yellow:white);
        }
        ls_safe_line(sf,body,y++,"Plot: -110 to -30 dBFS / dots: SD baseline",white);
    }
    ls_safe_line(sf,body,body.y+body.h-2,"RF changes do not prove a simulator.",yellow);
    ls_btn_t buttons[]={
        {status.busy?"STOP":"START","survey",'S',status.busy,false},
        {"BAND","range",'B',false,status.busy},
        {"LEARN","3 passes",'L',false,status.busy},
        {"LOAD","SD",'R',false,status.busy},
        {"SITE",manual_site?"LOCAL":"GPS",'G',manual_site,status.busy},
        {"HOME","exit",'H',false,false},
        {"REPORT","LoRa",'T',false,false},
        {"LTE","search",'E',false,status.busy},
        {"HIGH RATE","restart",'P',false,status.busy},
    };
    ls_btn_bar_raised(sf,tui_rect_make(area.x,area.y+area.h-controls,area.w,controls),buttons,9,focus);
}
static bool key(ls_tk_t k,char ch)
{
    if(k==LS_TK_ESC) {action(5);return true;}
    if(ls_btn_navigate(k,&slot,&focus,false)) return true;
    if(k==LS_TK_ENTER && focus>=0 && ls_btn_enabled(slot,focus)) {action(focus);return true;}
    if(k==LS_TK_CHAR) {
        int i=ls_btn_shortcut(ch,LS_BTN_SLOT_SCREEN);
        if(i>=0) {action(i);return true;}
    }
    return false;
}
static bool touch(int col,int row)
{
    int i=ls_btn_hit(col,row);if(i<0)return false;action(i);return true;
}
static bool value_busy(ls_val_t *out)
{cell_status_t s;cell_monitor_get(&s);out->kind=LS_VAL_BOOL;out->i=s.busy;return true;}
static bool value_changed(ls_val_t *out)
{cell_status_t s;cell_monitor_get(&s);if(!s.compared)return false;out->kind=LS_VAL_INT;out->i=s.changed;return true;}
static bool value_frequency(ls_val_t *out)
{cell_status_t s;cell_monitor_get(&s);if(!s.busy || !s.frequency)return false;out->kind=LS_VAL_FLOAT;out->f=s.frequency/1e6f;return true;}
static ls_act_status_t start(const ls_args_t *in,ls_val_t *out)
{(void)in;(void)out;return request(CELL_WATCH)?LS_ACT_OK:LS_ACT_BUSY;}
static ls_act_status_t stop(const ls_args_t *in,ls_val_t *out)
{(void)in;(void)out;cell_monitor_stop();return LS_ACT_OK;}
void ls_cell_publish(void)
{
    ls_value_publish("cell.busy","",value_busy);
    ls_value_publish("cell.changed_bins","bins",value_changed);
    ls_value_publish("cell.frequency","MHz",value_frequency);
    ls_action_register("cell.start","",LS_CAP_TUNE,start,"start survey; open CELL WATCH first");
    ls_action_register("cell.stop","",LS_CAP_TUNE,stop,"stop passive cellular survey");
}
const ls_tui_screen_t ls_scr_cell={
    .name="CELL WATCH",.hint="S capture/scan  B freq/band  T report  H exit",
    .enter=enter,.leave=leave,.draw=draw,.key=key,.touch=touch,
};
