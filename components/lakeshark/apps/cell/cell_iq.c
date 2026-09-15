/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded IQ acquisition for validating cellular broadcast reception.
 * Captures are ADC samples, never decoded subscriber traffic. */
#include "cell_iq.h"
#include "cell_performance.h"
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
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static uint8_t *capture;
static size_t captured;
static uint32_t frequency, rate, duration_us, crc;
static uint64_t dropped;
static bool complete,replay;
static bool timing_rejected, device_loss;
static const char *capture_source="NONE";
static size_t replay_expected;
static bool ui_busy, ui_auto, ui_stop;
static cell_session_t session;
static char session_path[112];
static portMUX_TYPE status_lock=portMUX_INITIALIZER_UNLOCKED;
static const uint32_t auto_centers[]={739000000,751000000,881500000,1981250000,1992500000,2150000000};
static int ui_phase;
static cell_iq_status_t ui_status;
static uint32_t ui_hz,ui_rate,ui_ms;
static ls_radio_iq_health_t radio_health;
static ls_radio_err_t radio_health_result, stop_result;
static ls_radio_iq_config_t capture_config;
static bool capture_config_valid;
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
    memset(&radio_health,0,sizeof(radio_health));
    memset(&capture_config,0,sizeof(capture_config));capture_config_valid=false;
    radio_health_result=LS_RADIO_ERR_UNSUPPORTED;
    stop_result=LS_RADIO_ERR_INVALID;
    captured=0;complete=false;crc=0;duration_us=0;dropped=0;timing_rejected=false;device_loss=false;
    frequency=hz;rate=requested_rate;replay=false;replay_expected=0;
    capture_source=hackrf?"HACKRF":"RTL";
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
    /* P4's configured cache line is 128 bytes. Preserve DMA alignment all
     * the way to FatFs, avoiding SDMMC's per-sector bounce-buffer fallback. */
    capture=heap_caps_aligned_alloc(128,wanted,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!capture) {err=LS_RADIO_ERR_NO_MEMORY;goto done;}
    ls_radio_iq_config_t cfg={.center_hz=hz,.sample_rate_hz=rate,
        .bandwidth_hz=rate,.gain_mode=LS_RADIO_GAIN_MANUAL,.gain_tenths_db=hackrf?640:297},actual;
    err=ls_radio_iq_configure(session,&cfg,&actual);
    if(err!=LS_RADIO_OK) goto done;
    capture_config=actual;capture_config_valid=true;
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
    stop_result=ls_radio_iq_stop(session);
    /* Query only after RX stops: no extra control transfers in the RF window.
     * HackRF preserves counts on IDLE; they cover startup through stop, not
     * exactly our retained window. Keep them as evidence until the attached
     * firmware's reset/rollback behavior has been validated. */
    if(hackrf && stream_started && stop_result==LS_RADIO_OK)
        radio_health_result=ls_radio_iq_get_health(session,&radio_health);
    if(radio_health_result==LS_RADIO_OK &&
       (radio_health.num_shortfalls || radio_health.error || radio_health.active_mode ||
        radio_health.request_flag || radio_health.m0_count<captured || radio_health.m4_count<captured)) {
        device_loss=true;complete=false;err=LS_RADIO_ERR_IO;
    }
    ls_radio_release(session);
    if(stop_result!=LS_RADIO_OK){complete=false;err=stop_result;}
    if(hackrf)printf("HRFHEALTH result=%s api=%04x phase=post_stop m0=%lu m4=%lu shortfalls=%lu longest=%lu mode=%lu error=%lu stop=%s\n",
        ls_radio_err_name(radio_health_result),radio_health.usb_api,
        (unsigned long)radio_health.m0_count,(unsigned long)radio_health.m4_count,
        (unsigned long)radio_health.num_shortfalls,(unsigned long)radio_health.longest_shortfall,
        (unsigned long)radio_health.active_mode,(unsigned long)radio_health.error,ls_radio_err_name(stop_result));
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
        result->resample_ms=(uint32_t)((esp_timer_get_time()-start)/1000);
        free(filter);input=converted;
    }
    if(samples<28800 || samples>153600){free(converted);return;}
    __atomic_store_n(&ui_phase,CELL_IQ_SYNCHRONIZING,__ATOMIC_RELEASE);
    void *ws=heap_caps_malloc(lte_sync_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!ws){free(converted);return;}
    result->lte_checked=true;
    int64_t sync_start=esp_timer_get_time();
    result->lte_found=lte_sync_find(input,samples,ws,&result->lte,sync_yield,&last);
    result->sync_ms=(uint32_t)((esp_timer_get_time()-sync_start)/1000);
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
            result->mib_found=lte_mib_confirm(input,samples,result->lte.pci,frame,result->lte.cfo_hz,ws,&result->mib,sync_yield,&last);
            result->mib_ms=(uint32_t)((esp_timer_get_time()-mib_start)/1000);
            printf("LTEMIB execution=P4 source=%s found=%d n_rb=%d ports=%d sfn=%d frames=%d phich_duration=%d phich_resource=%d payload=%06lx elapsed_ms=%lu workspace=%u\n",
                replay?"REPLAY":capture_source,result->mib_found,result->mib.n_rb,result->mib.antenna_ports,
                result->mib.sfn,result->mib.frames,result->mib.phich_duration,result->mib.phich_resource,
                (unsigned long)result->mib.payload,(unsigned long)result->mib_ms,(unsigned)lte_mib_workspace_size());
            free(ws);
        }
    }
    result->analysis_ms=(uint32_t)((esp_timer_get_time()-start)/1000);
    printf("CELLPROFILE resample_ms=%lu sync_ms=%lu mib_ms=%lu analysis_ms=%lu mib_mode=confirm_two_crc\n",
        (unsigned long)result->resample_ms,(unsigned long)result->sync_ms,
        (unsigned long)result->mib_ms,(unsigned long)result->analysis_ms);
    free(converted);
}
static bool write_metadata(FILE *f,const cell_iq_status_t *result)
{
    const ls_radio_iq_health_t *h=&result->radio_health;
    if(fprintf(f,"{\"profile\":{\"acquire_ms\":%lu,\"resample_ms\":%lu,\"sync_ms\":%lu,\"raw_save_ms\":%lu},"
        "\"receiver_config\":{\"valid\":%s,\"bandwidth_hz\":%lu,\"gain_tenths_db\":%d},"
        "\"raw_stats\":{\"valid\":%s,\"dc_i_codes\":%.4f,\"dc_q_codes\":%.4f,\"ac_power_codes2\":%.4f,\"rail_fraction\":%.8f},"
        "\"mib_mode\":\"confirm_two_crc\",\"raw_write_method\":\"aligned_posix\",",
        (unsigned long)result->acquire_ms,(unsigned long)result->resample_ms,
        (unsigned long)result->sync_ms,(unsigned long)result->raw_save_ms,
        result->config_valid?"true":"false",(unsigned long)result->config.bandwidth_hz,result->config.gain_tenths_db,
        result->raw_stats_valid?"true":"false",result->dc_i_codes,result->dc_q_codes,
        result->ac_power_codes2,result->rail_fraction)<0)return false;
    if(fprintf(f,"\"radio_health\":{\"result\":\"%s\",\"usb_api\":%u,\"phase\":\"post_stop\",\"window\":\"whole_rx_session\",\"stop_result\":\"%s\",\"requested_mode\":%u,\"request_flag\":%u,\"active_mode\":%lu,\"m0_bytes\":%lu,\"m4_bytes\":%lu,\"shortfalls\":%lu,\"longest_shortfall_bytes\":%lu,\"shortfall_limit\":%lu,\"threshold\":%lu,\"next_mode\":%lu,\"error\":%lu},",
        ls_radio_err_name(result->radio_health_result),h->usb_api,ls_radio_err_name(result->stop_result),
        h->requested_mode,h->request_flag,(unsigned long)h->active_mode,
        (unsigned long)h->m0_count,(unsigned long)h->m4_count,(unsigned long)h->num_shortfalls,
        (unsigned long)h->longest_shortfall,(unsigned long)h->shortfall_limit,
        (unsigned long)h->threshold,(unsigned long)h->next_mode,(unsigned long)h->error)<0)return false;
                bool ok=fprintf(f,"\"schema\":2,\"decoder_version\":\"p4-mib-3\",\"raw_file\":\"%s\",\"unix_time\":%lld,\"started_us\":%llu,\"sequence\":%lu,\"automatic\":%s,\"raw_saved\":%s,\"complete\":%s,\"speed_kts\":%.3f,\"hdop\":%.2f,\"satellites\":%u,\"end_imu_valid\":%s,\"end_accel_g\":[%.5f,%.5f,%.5f],\"end_gyro_dps\":[%.5f,%.5f,%.5f],\"end_gps_valid\":%s,\"end_latitude\":%.7f,\"end_longitude\":%.7f,\"source\":\"HackRF-P4\",\"format\":\"cu8\",\"frequency_hz\":%lu,\"rate\":%lu,\"bytes\":%u,\"crc32\":\"%08lx\",\"dropped\":%llu,\"imu_valid\":%s,\"accel_g\":[%.5f,%.5f,%.5f],\"gyro_dps\":[%.5f,%.5f,%.5f],\"mag_valid\":%s,\"mag_uT\":[%.5f,%.5f,%.5f],\"heading_deg\":%.3f,\"gps_valid\":%s,\"latitude\":%.7f,\"longitude\":%.7f,\"capture_elapsed_us\":%lu,\"lte\":{\"execution\":\"P4\",\"checked\":%s,\"found\":%s,\"pci\":%d,\"hits\":%d,\"pairs\":%d,\"cfo_hz\":%d,\"pss\":%.5f,\"sss\":%.5f,\"analysis_ms\":%lu,\"identity_decoded\":false,\"mib\":{\"checked\":%s,\"found\":%s,\"n_rb\":%d,\"antenna_ports\":%d,\"sfn\":%d,\"crc_frames\":%d,\"first_frame\":%d,\"phich_duration\":%d,\"phich_resource\":%d,\"payload\":%lu,\"elapsed_ms\":%lu}}}\n",
                    result->raw_file,(long long)result->unix_time,(unsigned long long)result->started_us,(unsigned long)result->sequence,
                    result->automatic?"true":"false",result->raw_saved?"true":"false",result->complete?"true":"false",
                    result->speed_kts,result->hdop,result->satellites,result->end_imu_valid?"true":"false",
                    result->end_imu.ax,result->end_imu.ay,result->end_imu.az,
                    result->end_imu.gx,result->end_imu.gy,result->end_imu.gz,
                    result->end_gps_valid?"true":"false",result->end_latitude,result->end_longitude,
                    (unsigned long)result->hz,(unsigned long)result->rate,(unsigned)result->bytes,(unsigned long)crc,(unsigned long long)dropped,
                    result->imu_valid?"true":"false",result->imu.ax,result->imu.ay,result->imu.az,result->imu.gx,result->imu.gy,result->imu.gz,
                    result->imu.mag_valid?"true":"false",result->imu.mx,result->imu.my,result->imu.mz,result->heading,
                    result->gps_valid?"true":"false",result->latitude,result->longitude,(unsigned long)result->elapsed_us,
                    result->lte_checked?"true":"false",result->lte_found?"true":"false",result->lte.pci,result->lte.hits,result->lte.pairs,result->lte.cfo_hz,
                    (double)result->lte.pss_score,(double)result->lte.sss_score,(unsigned long)result->analysis_ms,
                    result->mib_checked?"true":"false",result->mib_found?"true":"false",result->mib.n_rb,result->mib.antenna_ports,
                    result->mib.sfn,result->mib.frames,result->mib.first_frame,result->mib.phich_duration,result->mib.phich_resource,
                    (unsigned long)result->mib.payload,(unsigned long)result->mib_ms)>0;
    return ok;
}
static void publish_result(const cell_iq_status_t *result)
{
    portENTER_CRITICAL(&status_lock);ui_status=*result;portEXIT_CRITICAL(&status_lock);
}
static void capture_once(void)
{
    cell_iq_status_t result={0};
    result.automatic=ui_auto;result.multi=session.multi;
    result.attempts=ui_auto?session.attempts:0;result.decoded=ui_auto?session.decoded:0;
    result.raw_bytes=ui_auto?session.raw_bytes:0;
    result.sequence=ui_auto?session.attempts+1:1;
    result.started_us=esp_timer_get_time();result.unix_time=time(NULL);
    result.lte.pci=-1;
    result.hz=ui_hz;result.rate=ui_rate;
    result.imu_valid=ls_imu_read(&result.imu);
    result.heading=result.imu_valid && result.imu.mag_valid?ls_imu_heading():0;
    ls_gps_state_t gps;ls_gps_get(&gps);
    result.gps_valid=gps.fix && gps.last_fix_us>0 && esp_timer_get_time()-gps.last_fix_us<5000000;
    result.latitude=gps.lat_deg;result.longitude=gps.lon_deg;
    result.speed_kts=gps.speed_kts;result.hdop=gps.hdop;result.satellites=gps.sats_used;
    if(!ui_auto || !session.attempts)publish_result(&result);
    /* An unavailable endpoint must not leave an older capture marked current. */
    complete=false;captured=0;dropped=0;duration_us=0;timing_rejected=false;crc=0;
    frequency=ui_hz;rate=ui_rate;capture_source="HACKRF";
    int64_t acquire_start=esp_timer_get_time();
    acquire(ui_hz,ui_rate,ui_ms,true);
    result.acquire_ms=(uint32_t)((esp_timer_get_time()-acquire_start)/1000);
    result.config=capture_config;result.config_valid=capture_config_valid;
    result.radio_health=radio_health;result.radio_health_result=radio_health_result;result.stop_result=stop_result;
    result.end_imu_valid=ls_imu_read(&result.end_imu);
    ls_gps_get(&gps);
    result.end_gps_valid=gps.fix && gps.last_fix_us>0 && esp_timer_get_time()-gps.last_fix_us<5000000;
    result.end_latitude=gps.lat_deg;result.end_longitude=gps.lon_deg;
    result.complete=complete;result.bytes=captured;result.elapsed_us=duration_us;result.dropped=dropped;
    snprintf(result.message,sizeof(result.message),complete?"Capture complete / held in RAM":"Capture failed or incomplete / not saved");
    if(timing_rejected)snprintf(result.message,sizeof(result.message),"USB capture too slow / select a lower rate");
    int64_t dc_sum[2]={0,0};uint64_t squares=0,rails=0;
    for(unsigned bin=0;bin<64 && complete;bin++) {
        size_t begin=bin*captured/64,end=(bin+1)*captured/64;
        unsigned peak=0;
        for(size_t i=begin;i<end;i++){
            int v=(int)capture[i]-128;unsigned a=abs(v);if(a>peak)peak=a;
            dc_sum[i&1]+=v;squares+=(unsigned)(v*v);
            rails+=capture[i]==0 || capture[i]==255;
        }
        result.envelope[bin]=(uint8_t)peak;
    }
    if(complete && captured>=2) {
        double n=captured/2,mi=dc_sum[0]/n,mq=dc_sum[1]/n;
        double power=squares/n-mi*mi-mq*mq;
        result.raw_stats_valid=true;result.dc_i_codes=(float)mi;result.dc_q_codes=(float)mq;
        result.ac_power_codes2=(float)(power>0?power:0);
        result.rail_fraction=(float)((double)rails/captured);
    }
    vTaskPrioritySet(NULL,2); /* Analysis and file writes yield to interaction. */
    analyze(&result);
    __atomic_store_n(&ui_phase,CELL_IQ_SAVING,__ATOMIC_RELEASE);
    bool storage_ok=ls_sdcard_mounted();
    char path[112]={0};
    if(storage_ok) {
        mkdir("/sdcard/cell",0775);
        snprintf(path,sizeof(path),"/sdcard/cell/hrf_%08lx_%012llx.cu8",
                 (unsigned long)result.unix_time,(unsigned long long)result.started_us);
        snprintf(result.raw_file,sizeof(result.raw_file),"%s",path+13);
        /* Retain bounded failed captures for replay, with complete=false.
         * The decoder never consumes these as a continuous RF observation. */
        if(captured && (!ui_auto || cell_session_keep_raw(&session,captured))) {
            int64_t save_start=esp_timer_get_time();
            /* Bypass stdio's small, potentially unaligned intermediate
             * buffer. The VFS passes this aligned block directly to FatFs.
             * close still flushes file metadata; short writes remain errors. */
            int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0666);
            if(fd>=0){result.raw_saved=write(fd,capture,captured)==(ssize_t)captured;if(close(fd)!=0)result.raw_saved=false;}
            result.raw_save_ms=(uint32_t)((esp_timer_get_time()-save_start)/1000);
            if(!result.raw_saved)storage_ok=false;
        }
        if(result.raw_saved || !ui_auto) {
            char meta_path[120];snprintf(meta_path,sizeof(meta_path),"%s.json",path);
            FILE *f=fopen(meta_path,"w");
            if(f){result.metadata_saved=write_metadata(f,&result);if(fclose(f)!=0)result.metadata_saved=false;}
            if(!result.metadata_saved)storage_ok=false;
        }
        if(ui_auto) {
            /* Never delete old evidence. Raw IQ has a per-session budget;
             * small observation records continue after that budget is used. */
            struct stat st;
            if(stat(session_path,&st)==0 && st.st_size>=32*1024*1024)storage_ok=false;
            FILE *f=storage_ok?fopen(session_path,"a"):NULL;
            bool logged=false;
            if(f){logged=write_metadata(f,&result);if(fclose(f)!=0)logged=false;}
            result.metadata_saved=logged;
            if(!logged)storage_ok=false;
        }
    }
    if(ui_auto) {
        cell_session_finish(&session,complete,result.mib_found,result.raw_saved?captured:0);
        result.attempts=session.attempts;result.decoded=session.decoded;
        result.raw_bytes=session.raw_bytes;result.wait_ms=session.wait_ms;
        if(!storage_ok)__atomic_store_n(&ui_stop,true,__ATOMIC_RELEASE);
    }
    snprintf(result.message,sizeof(result.message),!storage_ok?"SD unavailable / session stopped":
        device_loss?"Radio sample loss / decode rejected":!complete?"Capture failed / attempt logged":result.raw_saved?"IQ + evidence saved on SD":"IQ budget used / evidence saved");
    publish_result(&result);
}
static void capture_worker(void *unused)
{
    (void)unused;
    do {
        if(ui_auto && session.multi)__atomic_store_n(&ui_hz,auto_centers[session.channel],__ATOMIC_RELEASE);
        __atomic_store_n(&ui_phase,CELL_IQ_RECEIVING,__ATOMIC_RELEASE);
        vTaskPrioritySet(NULL,8);
        capture_once();
        if(!ui_auto || __atomic_load_n(&ui_stop,__ATOMIC_ACQUIRE))break;
        __atomic_store_n(&ui_phase,CELL_IQ_WAITING,__ATOMIC_RELEASE);
        for(uint32_t ms=0;ms<session.wait_ms && !__atomic_load_n(&ui_stop,__ATOMIC_ACQUIRE);ms+=100)
            vTaskDelay(pdMS_TO_TICKS(100));
    } while(!__atomic_load_n(&ui_stop,__ATOMIC_ACQUIRE));
    __atomic_store_n(&ui_busy,false,__ATOMIC_RELEASE);
    vTaskDelete(NULL);
}
static bool begin(uint32_t hz,uint32_t sps,uint32_t ms,bool automatic,bool multi)
{
    if(hz<600000000 || hz>2690000000u || sps<2000000 || sps>20000000 || ms<20 || ms>1000 || (uint64_t)sps*2*ms/1000>4000000)return false;
    bool expected=false;
    if(!__atomic_compare_exchange_n(&ui_busy,&expected,true,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    ui_hz=hz;ui_rate=sps;ui_ms=ms;ui_auto=automatic;
    __atomic_store_n(&ui_stop,false,__ATOMIC_RELEASE);
    cell_session_begin(&session,multi);
    snprintf(session_path,sizeof(session_path),"/sdcard/cell/session_%08lx_%012llx.jsonl",
             (unsigned long)time(NULL),(unsigned long long)esp_timer_get_time());
    __atomic_store_n(&ui_phase,CELL_IQ_RECEIVING,__ATOMIC_RELEASE);
    if(xTaskCreatePinnedToCore(capture_worker,"cell_hrf",6144,NULL,8,NULL,0)!=pdPASS) {
        __atomic_store_n(&ui_busy,false,__ATOMIC_RELEASE);return false;
    }
    return true;
}
bool cell_iq_hackrf_begin(uint32_t hz,uint32_t sps,uint32_t ms)
{return begin(hz,sps,ms,false,false);}
bool cell_iq_auto_begin(uint32_t hz,bool multi)
{
    if(!cell_performance_active() || !ls_sdcard_mounted())return false;
    return begin(hz,8000000,80,true,multi);
}
void cell_iq_stop(void) {__atomic_store_n(&ui_stop,true,__ATOMIC_RELEASE);}
void cell_iq_get_status(cell_iq_status_t *out)
{
    if(!out)return;
    portENTER_CRITICAL(&status_lock);*out=ui_status;portEXIT_CRITICAL(&status_lock);
    out->busy=__atomic_load_n(&ui_busy,__ATOMIC_ACQUIRE);
    out->stopping=out->busy && __atomic_load_n(&ui_stop,__ATOMIC_ACQUIRE);
    out->phase=out->busy?__atomic_load_n(&ui_phase,__ATOMIC_ACQUIRE):CELL_IQ_IDLE;
    out->active_hz=__atomic_load_n(&ui_hz,__ATOMIC_ACQUIRE);
}
static void read_sd(const char *name,bool emit_data)
{
    size_t n=strlen(name);
    if(!n || n>80 || strstr(name,"..") || strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=n) {
        puts("CFILE ERROR invalid basename");return;
    }
    char path[112];snprintf(path,sizeof(path),"/sdcard/cell/%s",name);
    struct stat st;
    if(stat(path,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size>32*1024*1024){puts("CFILE ERROR unavailable");return;}
    FILE *f=fopen(path,"rb");if(!f){puts("CFILE ERROR open");return;}
    printf("CFILE BEGIN %lu\n",(unsigned long)st.st_size);
    uint8_t buf[48];size_t got,off=0;uint32_t sum=0;
    while((got=fread(buf,1,sizeof(buf),f))>0) {
        sum=esp_rom_crc32_le(sum,buf,got);
        if(emit_data) {
            static const char hex[]="0123456789abcdef";
            char line[97];
            for(size_t i=0;i<got;i++){line[2*i]=hex[buf[i]>>4];line[2*i+1]=hex[buf[i]&15];}
            line[2*got]=0;
            printf("CF %u %s\n",(unsigned)off,line);
        }
        off+=got;
        if(off%3072==0)vTaskDelay(1);
    }
    bool ok=!ferror(f);if(fclose(f)!=0)ok=false;
    if(ok)printf("CFILE END %u %08lx\n",(unsigned)off,(unsigned long)sum);
    else puts("CFILE ERROR read");
}
static int cell_iq_run(int argc,char **argv)
{
    uint32_t hz,sps,ms;
    if(argc==3 && !strcmp(argv[1],"read")){read_sd(argv[2],true);return 0;}
    if(argc==3 && !strcmp(argv[1],"check")){read_sd(argv[2],false);return 0;}
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
    if(argc>=2 && !strcmp(argv[1],"once")) {
        uint32_t hz,sps,ms;
        if(argc!=5 || !number(argv[2],&hz) || !number(argv[3],&sps) || !number(argv[4],&ms))
            puts("usage: celliq once <Hz> <rate> <ms>");
        else puts(cell_iq_hackrf_begin(hz,sps,ms)?"CELLIQ ONCE started":"CELLIQ ONCE unavailable / invalid parameters or busy");
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"stop")){cell_iq_stop();puts("CELLIQ stopping after current save");return 0;}
    if(argc>=2 && !strcmp(argv[1],"auto")) {
        uint32_t hz=739000000;
        bool multi=argc==3 && !strcmp(argv[2],"list");
        if(argc!=3 || (!multi && !number(argv[2],&hz)))puts("usage: celliq auto <Hz|list>");
        else puts(cell_iq_auto_begin(hz,multi)?"CELLIQ AUTO started":"CELLIQ AUTO unavailable: need performance mode, SD and idle receiver");
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"status")) {
        cell_iq_status_t out;cell_iq_get_status(&out);
        printf("CELLIQ busy=%d auto=%d phase=%d attempts=%lu decoded=%lu raw_bytes=%llu message=%s\n",
               out.busy,out.automatic,out.phase,(unsigned long)out.attempts,(unsigned long)out.decoded,(unsigned long long)out.raw_bytes,out.message);
        if(!out.busy) {
            status();
            printf("CELLIQ saved=%s health=%s api=%04x m0=%lu m4=%lu shortfalls=%lu longest=%lu device_error=%lu\n",
                out.raw_file[0]?out.raw_file:"none",ls_radio_err_name(out.radio_health_result),out.radio_health.usb_api,
                (unsigned long)out.radio_health.m0_count,(unsigned long)out.radio_health.m4_count,
                (unsigned long)out.radio_health.num_shortfalls,(unsigned long)out.radio_health.longest_shortfall,
                (unsigned long)out.radio_health.error);
        }
        return 0;
    }
    bool expected=false;
    if(!__atomic_compare_exchange_n(&ui_busy,&expected,true,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){puts("CELLIQ capture busy");return 0;}
    int result=cell_iq_run(argc,argv);
    __atomic_store_n(&ui_busy,false,__ATOMIC_RELEASE);
    return result;
}
