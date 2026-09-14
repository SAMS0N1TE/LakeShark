/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded IQ acquisition for validating cellular broadcast reception.
 * Captures are ADC samples, never decoded subscriber traffic. */
#include "cell_iq.h"
#include "lte_sync.h"
#include "lte_resample.h"
#include "radio_endpoint.h"
#include "ls_gps.h"
#include "ls_sdcard.h"
#include "rtl-sdr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static uint8_t *capture;
static size_t captured;
static uint32_t frequency, rate, duration_us, crc;
static uint64_t dropped;
static bool complete,replay;
static bool timing_rejected;
static const char *capture_source="NONE";
static size_t replay_expected;
static bool ui_busy;
static int ui_phase;
static cell_iq_status_t ui_status;
static uint32_t ui_hz,ui_rate,ui_ms;
static bool sync_yield(void *arg)
{
    int64_t *last=arg,now=esp_timer_get_time();
    if(now-*last>20000){vTaskDelay(1);*last=now;}
    return false;
}

static bool number(const char *s, uint32_t *out)
{
    char *end; unsigned long v=strtoul(s,&end,10);
    if(!s[0] || *end || s[0]=='-' || v>UINT32_MAX) return false;
    *out=(uint32_t)v;return true;
}
static void status(void)
{
    printf("CELLIQ hz=%lu rate=%lu bytes=%u elapsed_us=%lu bps=%llu dropped=%llu complete=%d crc=%08lx source=%s\n",
        (unsigned long)frequency,(unsigned long)rate,(unsigned)captured,
        (unsigned long)duration_us,
        (unsigned long long)(duration_us?(uint64_t)captured*1000000/duration_us:0),
        (unsigned long long)dropped,complete,(unsigned long)crc,replay?"REPLAY":capture_source);
}
static void acquire(uint32_t hz,uint32_t requested_rate,uint32_t ms,bool hackrf)
{
    const char *endpoint=hackrf?LS_RADIO_ENDPOINT_HACKRF_USB:LS_RADIO_ENDPOINT_RTL_USB;
    ls_radio_requirements_t req={.required_caps=LS_RADIO_RX_IQ_U8,
        .min_hz=hz,.max_hz=hz,.sample_rate_hz=requested_rate,
        .iq_format=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id=endpoint};
    ls_radio_session_t *session=NULL;
    bool stream_started=false;
    ls_radio_err_t err=ls_radio_acquire("cell-iq",&req,&session);
    if(err!=LS_RADIO_OK) {printf("CELLIQ requested_source=%s result=%s\n",endpoint,ls_radio_err_name(err));return;}
    free(capture);capture=NULL;captured=0;complete=false;crc=0;duration_us=0;dropped=0;
    timing_rejected=false;
    frequency=hz;rate=requested_rate;replay=false;replay_expected=0;
    capture_source=hackrf?"HACKRF":"RTL";
    size_t wanted=(size_t)((uint64_t)rate*2*ms/1000);
    capture=heap_caps_malloc(wanted,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!capture) {err=LS_RADIO_ERR_NO_MEMORY;goto done;}
    ls_radio_iq_config_t cfg={.center_hz=hz,.sample_rate_hz=rate,
        .bandwidth_hz=rate,.gain_mode=LS_RADIO_GAIN_MANUAL,.gain_tenths_db=hackrf?640:297},actual;
    err=ls_radio_iq_configure(session,&cfg,&actual);
    if(err!=LS_RADIO_OK) goto done;
    if(actual.sample_rate_hz!=requested_rate){err=LS_RADIO_ERR_UNSUPPORTED;goto done;}
    rate=actual.sample_rate_hz;
    err=ls_radio_iq_start(session);
    if(err!=LS_RADIO_OK) goto done;
    stream_started=true;
    /* Drain the initial tuner/USB window, then measure a fresh capture. */
    size_t discard=0;
    int64_t deadline=esp_timer_get_time()+3000000;
    while(discard<65536 && esp_timer_get_time()<deadline) {
        size_t n=0;err=ls_radio_iq_read(session,capture,4096,100,&n);
        if(err!=LS_RADIO_OK) break;
        discard+=n;
    }
    if(err!=LS_RADIO_OK || discard<65536) goto done;
    /* This legacy-named diagnostic reads the common USB IQ ring counter;
     * it covers both adapters. HackRF also rejects gaps in its read method. */
    int64_t started=esp_timer_get_time();deadline=started+10000000;
    while(captured<wanted && esp_timer_get_time()<deadline) {
        size_t n=0,need=wanted-captured;if(need>16384)need=16384;
        err=ls_radio_iq_read(session,capture+captured,need,100,&n);
        if(err!=LS_RADIO_OK) break;
        captured+=n;
    }
    duration_us=(uint32_t)(esp_timer_get_time()-started);
    dropped=rtlsdr_stream_dropped();
    if(err==LS_RADIO_OK && captured!=wanted)err=LS_RADIO_ERR_TIMEOUT;
    /* A slow host can backpressure HackRF and lose samples inside the radio
     * without overflowing our ring. Reject implausible acquisition timing. */
    timing_rejected=duration_us>ms*1250+5000;
    complete=err==LS_RADIO_OK && captured==wanted && dropped==0 && !timing_rejected;
    if(timing_rejected && err==LS_RADIO_OK)err=LS_RADIO_ERR_TIMEOUT;
done:
    /* Include startup/discard losses when acquisition fails before the window. */
    if(stream_started)dropped=rtlsdr_stream_dropped();
    if(dropped)complete=false;
    ls_radio_iq_stop(session);ls_radio_release(session);
    if(captured) crc=esp_rom_crc32_le(0,capture,captured);
    printf("CELLIQ result=%s\n",ls_radio_err_name(err));status();
}
static void dump(void)
{
    static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    status();
    for(size_t off=0;off<captured;off+=48) {
        size_t n=captured-off;if(n>48)n=48;
        char line[65];size_t j=0;
        for(size_t i=0;i<n;i+=3) {
            uint32_t x=(uint32_t)capture[off+i]<<16;
            if(i+1<n)x|=(uint32_t)capture[off+i+1]<<8;
            if(i+2<n)x|=capture[off+i+2];
            line[j++]=b64[x>>18];line[j++]=b64[(x>>12)&63];
            line[j++]=i+1<n?b64[(x>>6)&63]:'=';
            line[j++]=i+2<n?b64[x&63]:'=';
        }
        line[j]=0;printf("CIQ %u %s\n",(unsigned)off,line);
        if(off%3072==0)vTaskDelay(1);
    }
    printf("CELLIQ END bytes=%u crc=%08lx\n",(unsigned)captured,(unsigned long)crc);
}
static void analyze(cell_iq_status_t *result)
{
    result->lte.pci=-1;
    if(!complete || !capture)return;
    int64_t start=esp_timer_get_time(),last=start;
    const uint8_t *input=capture;
    uint8_t *converted=NULL;
    size_t samples=captured/2,limit=(size_t)((uint64_t)rate*80/1000);
    if(samples>limit)samples=limit;
    if(rate!=1920000) {
        __atomic_store_n(&ui_phase,CELL_IQ_FILTERING,__ATOMIC_RELEASE);
        size_t count=lte_resample_count(samples,rate);
        if(count<28800)return;
        converted=heap_caps_malloc(2*count,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        void *filter=heap_caps_malloc(lte_resample_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!converted || !filter){free(converted);free(filter);return;}
        samples=lte_resample_u8(capture,samples,rate,converted,count,filter,sync_yield,&last);
        free(filter);input=converted;
    }
    if(samples<28800 || samples>153600){free(converted);return;}
    __atomic_store_n(&ui_phase,CELL_IQ_SYNCHRONIZING,__ATOMIC_RELEASE);
    void *ws=heap_caps_malloc(lte_sync_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!ws){free(converted);return;}
    result->lte_checked=true;
    result->lte_found=lte_sync_find(input,samples,ws,&result->lte,sync_yield,&last);
    result->analysis_ms=(uint32_t)((esp_timer_get_time()-start)/1000);
    printf("LTESYNC execution=P4 source=%s found=%d pci=%d hits=%d pairs=%d cfo=%d pss=%.3f sss=%.3f elapsed_ms=%lu input_rate=%lu\n",
        replay?"REPLAY":capture_source,result->lte_found,result->lte.pci,result->lte.hits,result->lte.pairs,result->lte.cfo_hz,
        (double)result->lte.pss_score,(double)result->lte.sss_score,(unsigned long)result->analysis_ms,(unsigned long)rate);
    int frame=lte_sync_frame_start(ws);free(ws);
    if(result->lte_found) {
        __atomic_store_n(&ui_phase,CELL_IQ_DECODING_MIB,__ATOMIC_RELEASE);
        ws=heap_caps_malloc(lte_mib_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(ws) {
            int64_t mib_start=esp_timer_get_time();
            result->mib_checked=true;
            result->mib_found=lte_mib_find(input,samples,result->lte.pci,frame,result->lte.cfo_hz,ws,&result->mib,sync_yield,&last);
            result->mib_ms=(uint32_t)((esp_timer_get_time()-mib_start)/1000);
            printf("LTEMIB execution=P4 source=%s found=%d n_rb=%d ports=%d sfn=%d frames=%d phich_duration=%d phich_resource=%d payload=%06lx elapsed_ms=%lu workspace=%u\n",
                replay?"REPLAY":capture_source,result->mib_found,result->mib.n_rb,result->mib.antenna_ports,
                result->mib.sfn,result->mib.frames,result->mib.phich_duration,result->mib.phich_resource,
                (unsigned long)result->mib.payload,(unsigned long)result->mib_ms,(unsigned)lte_mib_workspace_size());
            free(ws);
        }
    }
    result->analysis_ms=(uint32_t)((esp_timer_get_time()-start)/1000);
    free(converted);
}
static void capture_worker(void *unused)
{
    (void)unused;
    cell_iq_status_t result={0};
    result.lte.pci=-1;
    result.hz=ui_hz;result.rate=ui_rate;
    result.imu_valid=ls_imu_read(&result.imu);
    result.heading=result.imu_valid && result.imu.mag_valid?ls_imu_heading():0;
    ls_gps_state_t gps;ls_gps_get(&gps);
    result.gps_valid=gps.fix && gps.last_fix_us>0 && esp_timer_get_time()-gps.last_fix_us<5000000;
    result.latitude=gps.lat_deg;result.longitude=gps.lon_deg;
    /* An unavailable endpoint must not leave an older capture marked current. */
    complete=false;captured=0;dropped=0;duration_us=0;timing_rejected=false;
    acquire(ui_hz,ui_rate,ui_ms,true);
    result.complete=complete;result.bytes=captured;result.elapsed_us=duration_us;result.dropped=dropped;
    snprintf(result.message,sizeof(result.message),complete?"Capture complete / held in RAM":"Capture failed or incomplete / not saved");
    if(timing_rejected)snprintf(result.message,sizeof(result.message),"USB capture too slow / select a lower rate");
    for(unsigned bin=0;bin<64 && complete;bin++) {
        size_t begin=bin*captured/64,end=(bin+1)*captured/64;
        unsigned peak=0;
        for(size_t i=begin;i<end;i++){unsigned a=abs((int)capture[i]-128);if(a>peak)peak=a;}
        result.envelope[bin]=(uint8_t)peak;
    }
    vTaskPrioritySet(NULL,2); /* Analysis and file writes yield to interaction. */
    analyze(&result);
    __atomic_store_n(&ui_phase,CELL_IQ_SAVING,__ATOMIC_RELEASE);
    if(complete && ls_sdcard_mounted()) {
        mkdir("/sdcard/cell",0775);
        char path[112];
        snprintf(path,sizeof(path),"/sdcard/cell/hrf_%08lx_%08lx.cu8",(unsigned long)time(NULL),(unsigned long)(esp_timer_get_time()/1000));
        FILE *f=fopen(path,"wb");
        bool saved=false;
        if(f){saved=fwrite(capture,1,captured,f)==captured;if(fclose(f)!=0)saved=false;}
        if(saved) {
            strncat(path,".json",sizeof(path)-strlen(path)-1);f=fopen(path,"w");
            bool meta=false;
            if(f) {
                meta=fprintf(f,"{\"source\":\"HackRF-P4\",\"format\":\"cu8\",\"frequency_hz\":%lu,\"rate\":%lu,\"bytes\":%u,\"crc32\":\"%08lx\",\"dropped\":%llu,\"imu_valid\":%s,\"accel_g\":[%.5f,%.5f,%.5f],\"gyro_dps\":[%.5f,%.5f,%.5f],\"mag_valid\":%s,\"mag_uT\":[%.5f,%.5f,%.5f],\"heading_deg\":%.3f,\"gps_valid\":%s,\"latitude\":%.7f,\"longitude\":%.7f,\"capture_elapsed_us\":%lu,\"lte\":{\"execution\":\"P4\",\"checked\":%s,\"found\":%s,\"pci\":%d,\"hits\":%d,\"pairs\":%d,\"cfo_hz\":%d,\"pss\":%.5f,\"sss\":%.5f,\"analysis_ms\":%lu,\"identity_decoded\":false,\"mib\":{\"checked\":%s,\"found\":%s,\"n_rb\":%d,\"antenna_ports\":%d,\"sfn\":%d,\"crc_frames\":%d,\"first_frame\":%d,\"phich_duration\":%d,\"phich_resource\":%d,\"payload\":%lu,\"elapsed_ms\":%lu}}}\n",
                    (unsigned long)frequency,(unsigned long)rate,(unsigned)captured,(unsigned long)crc,(unsigned long long)dropped,
                    result.imu_valid?"true":"false",result.imu.ax,result.imu.ay,result.imu.az,result.imu.gx,result.imu.gy,result.imu.gz,
                    result.imu.mag_valid?"true":"false",result.imu.mx,result.imu.my,result.imu.mz,result.heading,
                    result.gps_valid?"true":"false",result.latitude,result.longitude,(unsigned long)result.elapsed_us,
                    result.lte_checked?"true":"false",result.lte_found?"true":"false",result.lte.pci,result.lte.hits,result.lte.pairs,result.lte.cfo_hz,
                    (double)result.lte.pss_score,(double)result.lte.sss_score,(unsigned long)result.analysis_ms,
                    result.mib_checked?"true":"false",result.mib_found?"true":"false",result.mib.n_rb,result.mib.antenna_ports,
                    result.mib.sfn,result.mib.frames,result.mib.first_frame,result.mib.phich_duration,result.mib.phich_resource,
                    (unsigned long)result.mib.payload,(unsigned long)result.mib_ms)>0;
                if(fclose(f)!=0)meta=false;
            }
            snprintf(result.message,sizeof(result.message),meta?"IQ + GPS/9-axis context saved on SD":"IQ saved / metadata write failed");
        } else snprintf(result.message,sizeof(result.message),"Capture held in RAM / SD write failed");
    }
    ui_status=result;
    __atomic_store_n(&ui_busy,false,__ATOMIC_RELEASE);
    vTaskDelete(NULL);
}
bool cell_iq_hackrf_begin(uint32_t hz,uint32_t sps,uint32_t ms)
{
    if(hz<600000000 || hz>2690000000u || sps<2000000 || sps>20000000 || ms<20 || ms>1000 || (uint64_t)sps*2*ms/1000>4000000)return false;
    bool expected=false;
    if(!__atomic_compare_exchange_n(&ui_busy,&expected,true,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    ui_hz=hz;ui_rate=sps;ui_ms=ms;
    __atomic_store_n(&ui_phase,CELL_IQ_RECEIVING,__ATOMIC_RELEASE);
    if(xTaskCreatePinnedToCore(capture_worker,"cell_hrf",6144,NULL,8,NULL,0)!=pdPASS) {
        __atomic_store_n(&ui_busy,false,__ATOMIC_RELEASE);return false;
    }
    return true;
}
void cell_iq_get_status(cell_iq_status_t *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));
    if(__atomic_load_n(&ui_busy,__ATOMIC_ACQUIRE)){out->busy=true;out->phase=__atomic_load_n(&ui_phase,__ATOMIC_ACQUIRE);return;}
    *out=ui_status;
}
static int cell_iq_run(int argc,char **argv)
{
    uint32_t hz,sps,ms;
    if(argc==3 && !strcmp(argv[1],"replay-begin") && number(argv[2],&ms) && ms>=57600 && ms<=307200 && !(ms&1)) {
        free(capture);capture=heap_caps_malloc(ms,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        captured=0;complete=false;replay=true;replay_expected=capture?ms:0;
        rate=1920000;frequency=0;duration_us=0;dropped=0;crc=0;
        puts(capture?"CELLIQ REPLAY READY":"CELLIQ REPLAY NO MEMORY");
    } else if(argc==4 && !strcmp(argv[1],"replay-data") && number(argv[2],&ms)) {
        size_t len=strlen(argv[3]);bool ok=replay && !complete && capture && ms==captured &&
            len>0 && !(len&1) && len<=512 && captured+len/2<=replay_expected;
        for(size_t i=0;ok && i<len;i++)if(!strchr("0123456789abcdefABCDEF",argv[3][i]))ok=false;
        if(ok)for(size_t i=0;i<len;i+=2){char pair[3]={argv[3][i],argv[3][i+1],0};capture[captured++]=(uint8_t)strtoul(pair,NULL,16);}
        printf("CELLIQ REPLAY %s %u\n",ok?"DATA":"REJECTED",(unsigned)captured);
    } else if(argc==3 && !strcmp(argv[1],"replay-end") && number(argv[2],&ms)) {
        crc=capture?esp_rom_crc32_le(0,capture,captured):0;
        complete=replay && replay_expected && captured==replay_expected && crc==ms;
        puts(complete?"CELLIQ REPLAY VERIFIED; diagnostic only":"CELLIQ REPLAY INVALID");status();
    } else if(argc==2 && !strcmp(argv[1],"status"))status();
    else if(argc==2 && !strcmp(argv[1],"sync")) {
        cell_iq_status_t result={0};analyze(&result);
        if(!result.lte_checked)puts("LTESYNC requires complete continuous IQ (1.92 or 2-20 MS/s), >=15 ms and free analysis memory");
    }
    else if(argc==2 && !strcmp(argv[1],"dump"))dump();
    else if(argc==2 && !strcmp(argv[1],"free")) {
        free(capture);capture=NULL;captured=0;complete=false;crc=0;replay_expected=0;status();
    } else if(argc==3 && !strcmp(argv[1],"quarantine")) {
        const char *name=argv[2];size_t n=strlen(name);
        if(n<9 || n>60 || strncmp(name,"hrf_",4) || strcmp(name+n-4,".cu8") || strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.")!=n || strstr(name,"..")) {
            puts("CELLIQ quarantine requires a capture basename hrf_*.cu8");return 0;
        }
        char old[128],renamed[140];snprintf(old,sizeof(old),"/sdcard/cell/%s",name);snprintf(renamed,sizeof(renamed),"%s.rejected",old);
        if(rename(old,renamed)!=0){puts("CELLIQ quarantine failed");return 0;}
        strncat(old,".json",sizeof(old)-strlen(old)-1);strncat(renamed,".json",sizeof(renamed)-strlen(renamed)-1);
        puts(rename(old,renamed)==0?"CELLIQ capture and metadata quarantined":"CELLIQ capture quarantined; metadata rename failed");
    } else if(argc==5 && !strcmp(argv[1],"capture") &&
        number(argv[2],&hz) && number(argv[3],&sps) && number(argv[4],&ms) &&
        hz>=700000000 && hz<=960000000 && sps>=225001 && sps<=2400000 && ms>=20 && ms<=1000)
        acquire(hz,sps,ms,false);
    else if(argc==5 && !strcmp(argv[1],"hackrf") &&
        number(argv[2],&hz) && number(argv[3],&sps) && number(argv[4],&ms) &&
        hz>=600000000 && hz<=2690000000u && sps>=2000000 && sps<=20000000 &&
        ms>=20 && ms<=1000 && (uint64_t)sps*2*ms/1000<=4000000)
        acquire(hz,sps,ms,true);
    else puts("usage: celliq capture <Hz 700M-960M> <rate 225001-2400000> <ms 20-1000> | hackrf <Hz 600M-2690M> <rate 2M-20M> <ms 20-1000, max 4MB> | status | dump | sync | free; receiver must be parked");
    return 0;
}
int cell_iq_command(int argc,char **argv)
{
    bool expected=false;
    if(!__atomic_compare_exchange_n(&ui_busy,&expected,true,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){puts("CELLIQ capture busy");return 0;}
    int result=cell_iq_run(argc,argv);
    __atomic_store_n(&ui_busy,false,__ATOMIC_RELEASE);
    return result;
}
