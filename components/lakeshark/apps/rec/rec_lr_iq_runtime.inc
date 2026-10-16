/* REC worker owns storage and session lifetime. Consumer only copies. */
#include "rec_lr_iq.h"
#include "rec_sidecar.h"
#include "rec_unique_name.h"
#include "ls_time.h"
#include "ls_lora_lr20xx.h"
#include "fsk_packet.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

static EXT_RAM_BSS_ATTR uint8_t s_iq_data[REC_LR_IQ_BYTES];
static atomic_bool s_iq_full, s_iq_stop;
static size_t s_iq_used;
static unsigned s_iq_warm;
static double s_iq_floor,s_iq_power;
static unsigned s_iq_rise,s_iq_ring_at,s_iq_ring_count;
/* CPU copies only (the I/Q pump callback), no DMA: PSRAM, not the internal DMA pool. */
EXT_RAM_BSS_ATTR static uint8_t s_iq_ring[1250*4];
static bool s_iq_triggered;
static char s_iq_result[256];
static uint32_t s_iq_hz,s_iq_rate=REC_LR_IQ_TX_RATE;
static int s_iq_mode,s_iq_dbm; /* 0 record, 1 info, 2 test, 3 replay, 4 dump, 5 FSK test */
static char s_iq_path[160];
/* Packet decode of the last capture: edges and packets live off the stack. */
#define IQ_PKT_EDGES 4096
static EXT_RAM_BSS_ATTR int32_t s_iq_pkt_edges[IQ_PKT_EDGES];
static EXT_RAM_BSS_ATTR fsk_pkt_t s_iq_pkts[4];
static char s_iq_packet[64];

bool rec_watch_iq_packet(char *out,size_t len)
{
    portENTER_CRITICAL(&s_lock);
    const bool have=s_iq_packet[0]!=0;
    if(out && len)snprintf(out,len,"%s",s_iq_packet);
    portEXIT_CRITICAL(&s_lock);
    return have;
}

/* `lead` then "2-FSK 2400 bd sync D391x2 4 B .. CRC 9435 ok" after `out`,
   or nothing when no packet is in the samples. Returns how many were found. */
static int iq_packets(const uint8_t *iq,size_t bytes,const char *lead,char *out,size_t len)
{
    const size_t n=fsk_pkt_iq16_edges((const int16_t *)iq,bytes/4,REC_LR_IQ_RATE,s_iq_pkt_edges,IQ_PKT_EDGES,NULL);
    const int found=n?fsk_pkt_from_edges(s_iq_pkt_edges,n,s_iq_pkts,4):0;
    for(int i=0;i<found;i++) {
        size_t used=strlen(out);
        if(used+3>=len)break;
        snprintf(out+used,len-used,"%s",i?" | ":lead);
        used=strlen(out);
        fsk_pkt_describe(&s_iq_pkts[i],out+used,len-used);
    }
    return found;
}

bool rec_watch_iq_status(char *out,size_t len)
{
    portENTER_CRITICAL(&s_lock);
    bool busy=s_iq_busy;
    if(out && len)snprintf(out,len,"%s",s_iq_result);
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

static bool iq_request(int mode,const char *path,uint32_t hz,int dbm,uint32_t rate)
{
    if(mode!=1 && mode!=3 && mode!=4 && (!ls_lora_rx_range_ok(ls_lora_caps(),hz,hz) || ls_lora_chip()!=LS_LORA_CHIP_LR20XX))return false;
    if(path && strlen(path)>=sizeof(s_iq_path))return false;
    if(!rec_watch_start())return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=!s_iq_busy && !s_watch_waiting && !s_status.enabled && !s_scan_busy && !s_scan_min &&
        !s_learn_busy && !s_learn_secs && !s_replay_busy && !s_replay_id && !s_replay_path[0];
    if(ok) {
        s_iq_busy=true;s_iq_pending=true;s_iq_hz=hz;s_iq_mode=mode;s_iq_dbm=dbm;s_iq_rate=rate;
        snprintf(s_iq_path,sizeof(s_iq_path),"%s",path?path:"");
        atomic_store(&s_iq_stop,false);
        snprintf(s_iq_result,sizeof(s_iq_result),mode?"I/Q diagnostic queued":"Armed: transmit within 30 s; captures 262 ms");
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

bool rec_watch_iq_record(uint32_t hz) { return iq_request(0,NULL,hz,0,REC_LR_IQ_TX_RATE); }
bool rec_watch_iq_dump(const char *path) { return path && *path && iq_request(4,path,0,0,REC_LR_IQ_TX_RATE); }
bool rec_watch_iq_info(const char *path) { return path && *path && iq_request(1,path,0,0,REC_LR_IQ_TX_RATE); }
bool rec_watch_iq_test(uint32_t hz,int dbm,uint32_t rate)
{ return rec_lr_iq_rate_supported(rate) && dbm>=-9 && dbm<=10 && iq_request(2,NULL,hz,dbm,rate); }

bool rec_watch_iq_test_fsk(uint32_t hz,int dbm,uint32_t rate)
{ return rec_lr_iq_rate_supported(rate) && dbm>=-9 && dbm<=10 && iq_request(5,NULL,hz,dbm,rate); }

bool rec_watch_iq_replay(const char *path,int dbm,uint32_t rate)
{ return path && *path && dbm>=-9 && dbm<=10 && rec_lr_iq_rate_supported(rate) && iq_request(3,path,0,dbm,rate); }

void rec_watch_iq_stop(void) { atomic_store(&s_iq_stop,true); }

static void iq_consume(void *ctx,const uint8_t *data,size_t bytes,bool spare)
{
    (void)ctx;(void)spare;
    if(!data || atomic_load(&s_iq_full))return;
    for(size_t i=0;i+3<bytes;i+=4) {
        int si=(int16_t)(data[i]|(unsigned)data[i+1]<<8);
        int sq=(int16_t)(data[i+2]|(unsigned)data[i+3]<<8);
        if(!s_iq_triggered) {
            memcpy(s_iq_ring+4*s_iq_ring_at,data+i,4);
            s_iq_ring_at=(s_iq_ring_at+1)%1250;
            if(s_iq_ring_count<1250)s_iq_ring_count++;
            s_iq_power+=(double)si*si+(double)sq*sq;
            s_iq_warm++;
            /* Use the lowest 1 ms energy in the initial 20 ms, then require three 1 ms
               windows >=10 dB above it. OOK off gaps survive averaging. */
            if(s_iq_warm<=2500) {
                if(s_iq_warm%125==0) {
                    double window=s_iq_power/125;
                    if(s_iq_warm==125 || window<s_iq_floor)s_iq_floor=window;
                    s_iq_power=0;
                }
                continue;
            }
            if((s_iq_warm-2500)%125)continue;
            double floor=s_iq_floor>1?s_iq_floor:1;
            s_iq_rise=s_iq_power/125>=10*floor?s_iq_rise+1:0;
            s_iq_power=0;
            if(s_iq_rise<3)continue;
            s_iq_triggered=true;
            for(unsigned j=0;j<s_iq_ring_count;j++) {
                unsigned at=(s_iq_ring_at+1250-s_iq_ring_count+j)%1250;
                memcpy(s_iq_data+s_iq_used,s_iq_ring+4*at,4);s_iq_used+=4;
            }
            continue;
        }
        memcpy(s_iq_data+s_iq_used,data+i,4);s_iq_used+=4;
        if(s_iq_used==sizeof(s_iq_data)) {atomic_store(&s_iq_full,true);break;}
    }
}

static void iq_record(uint32_t hz,char *result,size_t len)
{
    uint64_t total,free_bytes;
    if(esp_vfs_fat_info("/sdcard",&total,&free_bytes)!=ESP_OK || free_bytes<400000 || strncmp(rec_dir(),"/sdcard/",8)) {
        snprintf(result,len,"I/Q: SD missing or full");return;
    }
    if(lr20xx_stream_source2_active()) {snprintf(result,len,"I/Q: radio busy");return;}
    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!atomic_load(&s_iq_stop) && !ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)vTaskDelay(1);
    if(atomic_load(&s_iq_stop)) {ls_mesh_radio_hold(false);snprintf(result,len,"I/Q: stopped");return;}
    if(!ls_mesh_radio_held()) {ls_mesh_radio_hold(false);snprintf(result,len,"I/Q: radio busy");return;}
    s_iq_used=0;s_iq_warm=0;s_iq_floor=s_iq_power=0;s_iq_triggered=false;
    s_iq_rise=s_iq_ring_at=s_iq_ring_count=0;
    atomic_store(&s_iq_full,false);
    esp_err_t err=lr_wf_start_consumer(hz,iq_consume,NULL);
    lr_wf_stats stats={0};
    if(err==ESP_OK) {
        const int64_t end=esp_timer_get_time()+30000000;
        while(!atomic_load(&s_iq_full) && !atomic_load(&s_iq_stop) && esp_timer_get_time()<end) {
            lr_wf_get_stats(&stats);if(stats.done)break;
            vTaskDelay(1);
        }
        lr_wf_stop_stats(&stats);
        err=stats.error;
    }
    ls_mesh_radio_hold(false);
    if(err!=ESP_OK || stats.gaps) {
        snprintf(result,len,"I/Q: stream failed/gapped; not saved (%s)",esp_err_to_name(err));return;
    }
    if(!s_iq_triggered || !s_iq_used) {snprintf(result,len,"I/Q: no signal captured");return;}
    char timed[64],base[64],path[192];
    mkdir(rec_dir(),0775);
    ls_time_render_filename(timed,sizeof(timed),"lriq");
    if(rec_pick_unique_name(rec_dir(),timed,".sub",base,sizeof(base))) {
        snprintf(result,len,"I/Q: no free filename");return;
    }
    snprintf(path,sizeof(path),"%s/%s.sub",rec_dir(),base);
    rec_sidecar_t sc={.freq_hz=hz,.sample_rate=REC_LR_IQ_RATE,.bw_hz=31250,
        .span_us=(uint32_t)(s_iq_used/4*8),.gain_tenths=0};
    ls_time_render_stamp(sc.time,sizeof(sc.time));
    snprintf(sc.radio,sizeof(sc.radio),"LR2021");
    bool ok=rec_lr_iq_save(path,hz,s_iq_data,s_iq_used);
    if(ok && rec_sidecar_write(rec_dir(),base,&sc)) {unlink(path);ok=false;}
    snprintf(result,len,ok?"Saved %s (I/Q, %lu us)":"I/Q save failed: %s (%lu us)",path,(unsigned long)sc.span_us);
    char brief[64]="";
    if(iq_packets(s_iq_data,s_iq_used,"; ",result,len)>0)fsk_pkt_describe_short(&s_iq_pkts[0],brief,sizeof(brief));
    portENTER_CRITICAL(&s_lock);
    snprintf(s_iq_packet,sizeof(s_iq_packet),"%s",brief);
    portEXIT_CRITICAL(&s_lock);
}

static void iq_send(uint32_t hz,int dbm,size_t bytes,uint32_t rate,char *result,size_t len)
{
    const char *why=subghz_tx_radio_refusal(hz,dbm);
    if(why) {snprintf(result,len,"Replay refused: %s",why);return;}
    if(lr20xx_stream_source2_active()) {snprintf(result,len,"Replay: radio busy");return;}
    const int64_t end=esp_timer_get_time()+1000000;
    while(!ls_mesh_radio_hold(true) && esp_timer_get_time()<end)vTaskDelay(1);
    if(!ls_mesh_radio_held()) {ls_mesh_radio_hold(false);snprintf(result,len,"Replay: radio busy");return;}
    esp_err_t err=ls_lora_iq_send_rate_reason(s_iq_data,bytes,hz,dbm,rate,&why);
    ls_mesh_radio_hold(false);
    if(err==ESP_OK)snprintf(result,len,"Sent LR2021 I/Q once: %lu us, %d dBm",(unsigned long)((uint64_t)(bytes/2)*1000000/rate),dbm);
    else if(why)snprintf(result,len,"Replay: %s",why);
    else snprintf(result,len,"I/Q replay failed: %s (requires LR2021 ROM 1.24)",esp_err_to_name(err));
}

static void iq_replay_rate(const char *path,int dbm,uint32_t rate,char *result,size_t len)
{
    int64_t start=esp_timer_get_time();
    uint32_t hz;size_t bytes;
    if(!rec_lr_iq_load(path,&hz,s_iq_data,sizeof(s_iq_data),&bytes)) {
        snprintf(result,len,"Replay: invalid/truncated LR2021 I/Q file");return;
    }
    char dir[160],base[64];
    const char *slash=strrchr(path,'/'),*name=slash?slash+1:path;
    size_t n=strlen(name);
    if(n<5 || n-4>=sizeof(base) || (slash && (size_t)(slash-path)>=sizeof(dir))) {
        snprintf(result,len,"Replay: invalid I/Q path");return;
    }
    memcpy(base,name,n-4);base[n-4]=0;
    if(slash) {memcpy(dir,path,(size_t)(slash-path));dir[slash-path]=0;}
    else strcpy(dir,".");
    rec_sidecar_t sc;
    if(!rec_sidecar_read(dir,base,&sc) || strcmp(sc.radio,"LR2021") || sc.freq_hz!=hz || sc.sample_rate!=REC_LR_IQ_RATE) {
        snprintf(result,len,"Replay: missing/mismatched LR2021 sidecar");return;
    }
    unsigned load_ms=(unsigned)((esp_timer_get_time()-start)/1000);
    start=esp_timer_get_time();rec_lr_iq_classifier_t report;
    bytes=rec_lr_iq_convert_report(s_iq_data,bytes,s_iq_data,sizeof(s_iq_data),rate,&report);
    unsigned convert_ms=(unsigned)((esp_timer_get_time()-start)/1000);
    if(!bytes) {
        snprintf(result,len,"Replay: empty I/Q signal");return;
    }
    iq_send(hz,dbm,bytes,report.rate,result,len);
    size_t used=strlen(result);
    if(!strncmp(result,"Sent",4) && used<len)snprintf(result+used,len-used,
        ", path=%s rate=%lu bimodal=%.3f pulses=%u clustered=%.3f phase=%.4f sep=%.0fHz shares=%.3f/%.3f mid=%u load=%u ms convert=%u ms",
        report.ook?"OOK-regen":"IQ",(unsigned long)report.rate,report.bimodal,report.pulses,report.clustered,report.phase,report.tone_separation_hz,
        report.tone_share[0],report.tone_share[1],report.mid_burst_switches,load_ms,convert_ms);
}

static void iq_replay(const char *path,int dbm,char *result,size_t len)
{ iq_replay_rate(path,dbm,REC_LR_IQ_TX_RATE,result,len); }

/* Runs on the REC worker, never the UI/console command task. Yield each line
   so USB/UART backpressure does not monopolize the CPU; stop cancels promptly. */
static bool iq_dump_console(void *ctx,const char *line)
{
    (void)ctx;
    if(atomic_load(&s_iq_stop))return false;
    if(fputs(line,stdout)==EOF || fflush(stdout)==EOF)return false;
    vTaskDelay(1);
    return true;
}

static void iq_diagnostic(char *result,size_t len)
{
    if(s_iq_mode==3){iq_replay_rate(s_iq_path,s_iq_dbm,s_iq_rate,result,len);return;}
    if(s_iq_mode==2 || s_iq_mode==5) {
        size_t bytes=s_iq_mode==5?rec_lr_iq_test_fsk(s_iq_data,sizeof(s_iq_data),s_iq_rate):
            rec_lr_iq_test(s_iq_data,sizeof(s_iq_data),s_iq_rate);
        if(!bytes){snprintf(result,len,"I/Q: invalid test signal/rate");return;}
        iq_send(s_iq_hz,s_iq_dbm,bytes,s_iq_rate,result,len);return;
    }
    uint32_t hz;size_t bytes;rec_lr_iq_stats_t stats;
    if(!rec_lr_iq_load(s_iq_path,&hz,s_iq_data,sizeof(s_iq_data),&bytes)) {
        snprintf(result,len,"I/Q %s: invalid/truncated file",s_iq_mode==4?"dump":"info");return;
    }
    if(s_iq_mode==4) {
        bool ok=rec_lr_iq_dump(s_iq_data,bytes,hz,iq_dump_console,NULL);
        snprintf(result,len,"I/Q dump: %s",ok?"complete":"stopped/output failed");return;
    }
    if(!rec_lr_iq_stats(s_iq_data,bytes,&stats)) {
        snprintf(result,len,"I/Q info: stats allocation failed");return;
    }
    snprintf(result,len,"OOK pulses=%u widths=%u/%u us bits=%s\n"
        "samples=%lu rate=%u Hz freq=%lu Hz |IQ| peak=%.1f median=%.1f above(mid)=%.4f",
        stats.pulses,stats.short_us,stats.long_us,stats.bits[0]?stats.bits:"-",
        (unsigned long)stats.samples,REC_LR_IQ_RATE,(unsigned long)hz,stats.peak,stats.median,
        (double)stats.above/stats.samples);
    const size_t used=strlen(result);
    if(used+16<len) {
        if(!iq_packets(s_iq_data,bytes,"\npacket: ",result,len))snprintf(result+used,len-used,"\npacket: none");
    }
}
