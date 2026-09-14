#include "cell_monitor.h"
#include "cell_report.h"
#include "lte_sync.h"
#include "rtl-sdr.h"
#include "esp_heap_caps.h"
#include "ls_sweep.h"
#include "ls_gps.h"
#include "ls_imu.h"
#include "ls_sdcard.h"
#include "ls_board_hw.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t worker;
static cell_status_t state;
static int command;
static bool manual;
static bool stopped;
static uint32_t receiver_epoch, job_epoch;
static cell_baseline_t baseline, disk;
static int8_t measured[CELL_MAX_BINS];
static uint8_t streak[CELL_MAX_BINS];
static ls_sweep_plan_t plan;
static int tile_lat,tile_lon;
static bool located, external, context_ok, quiet;
static const char *context_reason;
static int64_t last_motion;
static int64_t last_imu_sample;

/* A lost receiver invalidates live evidence even while the worker is blocked
 * in USB I/O. Do not touch worker-owned baseline/streak storage here. */
static void invalidate_locked(void)
{
    state.compared=false;state.changed=0;
    for(unsigned i=0;i<state.rows;i++)state.row[i].flagged=false;
}
static void receiver_event(const ls_radio_endpoint_event_t *event,void *arg)
{
    (void)arg;
    if(!(event->capabilities&LS_RADIO_RX_IQ_U8))return;
    portENTER_CRITICAL(&lock);
    if(event->kind==LS_RADIO_ENDPOINT_DETACHED) {
        receiver_epoch++;state.receiver_missing=true;state.needs_relearn=true;
        state.baseline=false;invalidate_locked();
        snprintf(state.message,sizeof(state.message),"Receiver removed; comparison invalidated");
    } else if(state.receiver_missing) {
        state.receiver_missing=false;
        snprintf(state.message,sizeof(state.message),"Receiver restored; LEARN after antenna changes");
    }
    portEXIT_CRITICAL(&lock);
}
static void invalidate(void)
{
    memset(streak,0,sizeof(streak));
    portENTER_CRITICAL(&lock);invalidate_locked();portEXIT_CRITICAL(&lock);
}

static void message(const char *text)
{
    portENTER_CRITICAL(&lock);
    snprintf(state.message,sizeof(state.message),"%s",text);
    portEXIT_CRITICAL(&lock);
}
void cell_monitor_get(cell_status_t *out)
{
    portENTER_CRITICAL(&lock); *out=state; portEXIT_CRITICAL(&lock);
}
static bool cancel(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&lock); bool stop=stopped || receiver_epoch!=job_epoch; portEXIT_CRITICAL(&lock);
    return stop;
}
static bool position(int *lat,int *lon)
{
    if(manual) {*lat=0;*lon=0;return false;}
    ls_gps_state_t g; ls_gps_get(&g);
    int64_t age=esp_timer_get_time()-g.last_fix_us;
    bool valid=g.fix && g.last_fix_us>0 && age>=0 && age<5000000 &&
        isfinite(g.lat_deg) && isfinite(g.lon_deg) && g.hdop>0 && g.hdop<=5;
    *lat=valid?(int)floor(g.lat_deg*1000):0;
    *lon=valid?(int)floor(g.lon_deg*1000):0;
    return valid;
}
static void check_context(void)
{
    int lat,lon; bool fix=position(&lat,&lon);
    if(fix!=located) {context_ok=false;context_reason="GPS fix changed: retry or use SITE: LOCAL";}
    if(lat!=tile_lat || lon!=tile_lon) {context_ok=false;context_reason="GPS area changed: retry or use SITE: LOCAL";}
    if(ls_board_hw_antenna_is_external()!=external) {context_ok=false;context_reason="Antenna route changed: start a new baseline";}
    ls_imu_sample_t s;
    int64_t now=esp_timer_get_time();
    if(ls_imu_read(&s)) {
        last_imu_sample=now;
        if(!cell_motion_quiet(s.ax,s.ay,s.az,s.gx,s.gy,s.gz)) quiet=false;
    } else if(now-last_imu_sample>1000000) quiet=false;
}
static void progress(uint32_t tune,uint32_t tunes,void *arg)
{
    (void)arg;
    int64_t now=esp_timer_get_time();
    if(now-last_motion>=250000) {check_context();last_motion=now;}
    portENTER_CRITICAL(&lock);
    state.tune=tune; state.tunes=tunes;
    state.frequency=(uint32_t)ls_sweep_tune_center(&plan,tune-1);
    state.quiet=quiet;
    portEXIT_CRITICAL(&lock);
}
static void path(char *out,size_t n,const char *suffix,unsigned band)
{
    snprintf(out,n,"/sdcard/cell/%s_%d_%d_b%u_a%u.%s",located?"gps":"local",
             tile_lat,tile_lon,band,external,suffix);
}
static bool read_baseline(unsigned band)
{
    if(!ls_sdcard_mounted()) return false;
    const char *ext[]={"bin","bak"}; char file[128];
    for(unsigned i=0;i<2;i++) {
        path(file,sizeof(file),ext[i],band);
        FILE *f=fopen(file,"rb"); if(!f) continue;
        bool ok=fread(&disk,1,sizeof(disk),f)==sizeof(disk) && fgetc(f)==EOF;
        fclose(f);
        if(ok && cell_baseline_valid(&disk,band,tile_lat,tile_lon,located,external)) {
            baseline=disk; return true;
        }
    }
    return false;
}
static bool save_baseline(unsigned band)
{
    if(!ls_sdcard_mounted()) return false;
    if(mkdir("/sdcard/cell",0775)!=0 && errno!=EEXIST) return false;
    char file[128],tmp[128],bak[128];
    path(file,sizeof(file),"bin",band); path(tmp,sizeof(tmp),"new",band); path(bak,sizeof(bak),"bak",band);
    FILE *f=fopen(tmp,"wb"); if(!f) return false;
    bool ok=fwrite(&baseline,1,sizeof(baseline),f)==sizeof(baseline);
    if(fflush(f)!=0 || fsync(fileno(f))!=0) ok=false;
    if(fclose(f)!=0) ok=false;
    if(!ok) {remove(tmp);return false;}
    struct stat st;
    bool had=stat(file,&st)==0;
    if(had) {
        if(remove(bak)!=0 && errno!=ENOENT) {remove(tmp);return false;}
        if(rename(file,bak)!=0) {remove(tmp);return false;}
    }
    if(rename(tmp,file)==0) return true;
    if(had) rename(bak,file);
    return false;
}
static void publish_rows(bool comparable,unsigned changed,uint32_t elapsed)
{
    cell_status_t next; cell_monitor_get(&next);
    next.rows=0; next.changed=changed;next.elapsed_ms=elapsed;next.compared=comparable;
    next.baseline=baseline.passes==3;next.quiet=quiet;
    next.spectrum_count=96;
    memset(next.spectrum,CELL_MISSING,sizeof(next.spectrum));
    memset(next.reference,CELL_MISSING,sizeof(next.reference));
    for(unsigned i=0;i<plan.n_bins;i++) {
        if(measured[i]==CELL_MISSING) continue;
        unsigned column=i*96/plan.n_bins;
        if(measured[i]>next.spectrum[column])next.spectrum[column]=measured[i];
        if(comparable && baseline.power[i]>next.reference[column])next.reference[column]=baseline.power[i];
        int delta=comparable?(int)measured[i]-baseline.power[i]:0;
        int score=comparable?delta:measured[i]; unsigned j=0;
        for(;j<next.rows;j++) {
            int other=comparable?next.row[j].delta:next.row[j].power;
            if(score>other) break;
        }
        if(j>=8) continue;
        if(next.rows<8) next.rows++;
        for(unsigned k=next.rows-1;k>j;k--) next.row[k]=next.row[k-1];
        next.row[j].hz=(uint32_t)ls_sweep_out_hz(&plan,i);
        next.row[j].power=measured[i];next.row[j].delta=delta;
        next.row[j].flagged=streak[i]>=3;
    }
    portENTER_CRITICAL(&lock);
    if(receiver_epoch==job_epoch) {
        state.rows=next.rows;state.changed=next.changed;state.elapsed_ms=next.elapsed_ms;
        state.compared=next.compared;state.baseline=next.baseline;state.quiet=next.quiet;
        memcpy(state.row,next.row,sizeof(state.row));
        state.spectrum_count=next.spectrum_count;
        memcpy(state.spectrum,next.spectrum,sizeof(state.spectrum));
        memcpy(state.reference,next.reference,sizeof(state.reference));
    }
    portEXIT_CRITICAL(&lock);
}
static bool lte_yield(void *arg)
{
    int64_t *last=arg,now=esp_timer_get_time();
    if(now-*last>20000){vTaskDelay(1);*last=now;}
    return cancel(NULL);
}
static void log_lte(uint32_t hz,const lte_sync_result_t *r)
{
    if(!ls_sdcard_mounted())return;
    mkdir("/sdcard/cell",0775);
    const char *path="/sdcard/cell/lte.jsonl";struct stat st;
    if(stat(path,&st)==0 && st.st_size>1024*1024) {
        if(remove("/sdcard/cell/lte.previous.jsonl")!=0 && errno!=ENOENT)return;
        if(rename(path,"/sdcard/cell/lte.previous.jsonl")!=0)return;
    }
    FILE *f=fopen(path,"ab");if(!f){message("LTE found; SD journal unavailable");return;}
    ls_gps_state_t g;ls_gps_get(&g);int64_t age=esp_timer_get_time()-g.last_fix_us;
    bool fix=g.fix && g.last_fix_us>0 && age>=0 && age<5000000 && g.hdop>0 && g.hdop<=5 &&
        isfinite(g.lat_deg) && isfinite(g.lon_deg);
    time_t epoch=time(NULL);char gps[96]="null";
    if(fix)snprintf(gps,sizeof(gps),"[%.6f,%.6f]",g.lat_deg,g.lon_deg);
    int written=fprintf(f,"{\"kind\":\"LTE_SYNC\",\"epoch\":%llu,\"hz\":%lu,\"pci\":%d,\"hits\":%d,\"pairs\":%d,\"cfo_hz\":%d,\"pss\":%.3f,\"sss\":%.3f,\"gps\":%s}\n",
        (unsigned long long)(epoch>1735689600?epoch:0),(unsigned long)hz,r->pci,r->hits,r->pairs,r->cfo_hz,(double)r->pss_score,(double)r->sss_score,gps);
    bool ok=written>0;if(fflush(f) || fsync(fileno(f)))ok=false;if(fclose(f))ok=false;
    if(!ok)message("LTE found; SD journal write failed");
}
static bool lte_scan(unsigned band)
{
    const size_t bytes=115200; /* 30 ms, six possible synchronization occasions */
    uint8_t *iq=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    void *ws=heap_caps_malloc(lte_sync_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ls_radio_session_t *session=NULL;
    ls_radio_err_t error=LS_RADIO_ERR_NO_MEMORY;
    if(!iq || !ws){message("LTE allocation failed");goto done;}
    ls_radio_requirements_t req={.required_caps=LS_RADIO_RX_IQ_U8,
        .min_hz=cell_bands[band].low,.max_hz=cell_bands[band].high,
        .sample_rate_hz=1920000,.iq_format=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id=LS_RADIO_ENDPOINT_RTL_USB};
    for(int retry=0;retry<10 && !cancel(NULL);retry++) {
        error=ls_radio_acquire("cell-lte",&req,&session);
        if(error!=LS_RADIO_ERR_BUSY)break;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if(error!=LS_RADIO_OK)goto done;
    uint32_t first=cell_bands[band].low+700000,last=cell_bands[band].high-700000;
    unsigned tunes=(last-first)/100000+1;
    for(uint32_t hz=first;hz<=last && !cancel(NULL);hz+=100000) {
        portENTER_CRITICAL(&lock);
        state.frequency=hz;state.tune=(hz-first)/100000+1;state.tunes=tunes;
        portEXIT_CRITICAL(&lock);
        cell_status_t activity;cell_monitor_get(&activity);cell_report_activity(&activity,'P');
        message("LTE: acquiring 30 ms of continuous IQ");
        ls_radio_iq_config_t cfg={.center_hz=hz,.sample_rate_hz=1920000,.bandwidth_hz=1920000,
            .gain_mode=LS_RADIO_GAIN_MANUAL,.gain_tenths_db=CELL_GAIN},actual;
        error=ls_radio_iq_configure(session,&cfg,&actual);
        if(error!=LS_RADIO_OK)break;
        if(actual.sample_rate_hz!=1920000){error=LS_RADIO_ERR_UNSUPPORTED;break;}
        error=ls_radio_iq_start(session);if(error!=LS_RADIO_OK)break;
        size_t discard=0,n=0;
        while(discard<65536 && !cancel(NULL)) {
            error=ls_radio_iq_read(session,iq,4096,100,&n);
            if(error!=LS_RADIO_OK)break;
            discard+=n;
        }
        size_t total=0;int64_t start=esp_timer_get_time();
        uint64_t before=rtlsdr_stream_dropped();
        while(error==LS_RADIO_OK && total<bytes && !cancel(NULL)) {
            size_t need=bytes-total;if(need>16384)need=16384;
            n=0;
            error=ls_radio_iq_read(session,iq+total,need,100,&n);total+=n;
            if(esp_timer_get_time()-start>1000000){error=LS_RADIO_ERR_TIMEOUT;break;}
        }
        bool usable=error==LS_RADIO_OK && total==bytes && rtlsdr_stream_dropped()==before &&
            esp_timer_get_time()-start<60000;
        ls_radio_iq_stop(session);
        if(cancel(NULL))break;
        if(error!=LS_RADIO_OK)break;
        if(!usable){message("LTE IQ gap / slow capture: discarded");continue;}
        message("LTE: matching repeated PSS and SSS");
        lte_sync_result_t result;int64_t yielded=esp_timer_get_time();
        bool found=lte_sync_find(iq,total/2,ws,&result,lte_yield,&yielded);
        if(cancel(NULL))break;
        portENTER_CRITICAL(&lock);
        state.elapsed_ms=(uint32_t)((esp_timer_get_time()-start)/1000);
        if(found) {
            if(state.lte_count==8){memmove(state.cell,state.cell+1,7*sizeof(state.cell[0]));state.lte_count=7;}
            unsigned i=state.lte_count++;
            state.cell[i].hz=hz;state.cell[i].pci=result.pci;state.cell[i].hits=result.hits;
            state.cell[i].cfo=result.cfo_hz;state.cell[i].pss=result.pss_score;state.cell[i].sss=result.sss_score;
        }
        portEXIT_CRITICAL(&lock);
        if(found) {
            log_lte(hz,&result);
            printf("CELL_LTE hz=%lu pci=%d hits=%d cfo=%d pss=%.3f sss=%.3f\n",(unsigned long)hz,result.pci,result.hits,result.cfo_hz,(double)result.pss_score,(double)result.sss_score);
            cell_status_t report;cell_monitor_get(&report);cell_report_observe(&report);
        }
    }
    if(cancel(NULL))message("LTE search stopped; previous cell rows are history");
    else if(error==LS_RADIO_OK)message("LTE pass complete; no simulator verdict");
done:
    if(session){ls_radio_iq_stop(session);ls_radio_release(session);}
    free(iq);free(ws);
    if(cancel(NULL))message("LTE search stopped; previous cell rows are history");
    else if(error!=LS_RADIO_OK){char text[80];snprintf(text,sizeof(text),"LTE receiver: %s",ls_radio_err_name(error));message(text);}
    return !cancel(NULL) && error==LS_RADIO_OK;
}
static void task(void *arg)
{
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        cell_status_t initial;cell_monitor_get(&initial);
        unsigned band=initial.band; int job=command;bool complete_lte=false;
        if(initial.needs_relearn) memset(&baseline,0,sizeof(baseline));
        located=position(&tile_lat,&tile_lon);external=ls_board_hw_antenna_is_external();
        portENTER_CRITICAL(&lock);
        state.located=located;state.external=external;state.sd=ls_sdcard_mounted();
        portEXIT_CRITICAL(&lock);
        if(job==CELL_LTE){complete_lte=lte_scan(band);goto finished;}
        memset(streak,0,sizeof(streak));
        if(!cell_baseline_valid(&baseline,band,tile_lat,tile_lon,located,external)) memset(&baseline,0,sizeof(baseline));
        if(job==CELL_LOAD && initial.needs_relearn) {
            message("Receiver changed: LEARN a fresh antenna baseline");goto finished;
        }
        if(job==CELL_LOAD || (located && job==CELL_WATCH && !initial.needs_relearn)) {
            bool loaded=read_baseline(band);
            if(job==CELL_LOAD) {
                message(loaded?(located?"GPS baseline loaded from SD":"Local baseline loaded: verify same site"):
                        "No valid SD baseline for this site / band");
                goto finished;
            }
        }
        if(job==CELL_LEARN) {
            memset(&baseline,0,sizeof(baseline));
            baseline.magic=0x43454c4c;baseline.version=1;baseline.band=band;
            baseline.rate=CELL_RATE;baseline.gain=CELL_GAIN;
            baseline.lat_tile=tile_lat;baseline.lon_tile=tile_lon;baseline.located=located;
            baseline.antenna=external;baseline.created=(uint32_t)time(NULL);
            memset(baseline.power,CELL_MISSING,sizeof(baseline.power));
        }
        if(!ls_sweep_plan(cell_bands[band].low,cell_bands[band].high,CELL_BIN,CELL_RATE,&plan) || plan.n_bins>CELL_MAX_BINS) {
            message("Invalid sweep plan");goto finished;
        }
        unsigned passes=0;
        /* Give a touch on LEARN time to settle before measuring movement. */
        message("Settling for 3 seconds; keep device still");
        last_imu_sample=0;
        for(int i=0;i<12 && !cancel(NULL);i++) {
            ls_imu_sample_t sample;
            if(ls_imu_read(&sample)) last_imu_sample=esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        while(!cancel(NULL)) {
            context_ok=true;context_reason="Site changed";quiet=true;last_motion=0;check_context();
            int64_t started=esp_timer_get_time();
            message(job==CELL_LEARN?"Learning: keep antenna and device still":"Sweeping cellular downlink spectrum");
            portENTER_CRITICAL(&lock);state.tune=0;state.tunes=plan.n_tunes;portEXIT_CRITICAL(&lock);
            ls_radio_err_t result=ls_sweep_run_cancelable(&plan,CELL_GAIN,8,false,measured,progress,cancel,NULL);
            if(result==LS_RADIO_ERR_BUSY && passes==0 && esp_timer_get_time()-started<1000000) {
                message("Waiting for receiver to park");vTaskDelay(pdMS_TO_TICKS(300));
                /* Bounded retry; a different owner may legitimately hold it. */
                for(int retry=0;retry<10 && result==LS_RADIO_ERR_BUSY && !cancel(NULL);retry++) {
                    result=ls_sweep_run_cancelable(&plan,CELL_GAIN,8,false,measured,progress,cancel,NULL);
                    if(result==LS_RADIO_ERR_BUSY) vTaskDelay(pdMS_TO_TICKS(300));
                }
            }
            check_context();
            if(cancel(NULL) || result==LS_RADIO_ERR_STOPPED) {
                invalidate();cell_status_t current;cell_monitor_get(&current);
                message(current.receiver_missing?"Receiver removed; reconnect then LEARN":
                        current.needs_relearn?"Receiver changed; LEARN after antenna swap":
                        "Stopped; incomplete sweep discarded");break;
            }
            if(result!=LS_RADIO_OK || ls_sweep_gaps(&plan,measured)) {
                invalidate();
                char text[80];snprintf(text,sizeof(text),"Sweep failed: %s; no comparison",result==LS_RADIO_OK?"missing IQ":ls_radio_err_name(result));
                message(text);break;
            }
            passes++;
            bool valid_base=cell_baseline_valid(&baseline,band,tile_lat,tile_lon,located,external);
            bool comparable=valid_base && context_ok && quiet;
            unsigned changed=cell_compare(baseline.power,measured,streak,plan.n_bins,comparable);
            if(job==CELL_LEARN) {
                if(!context_ok || !quiet) {invalidate();message(!context_ok?context_reason:"Motion / stale IMU: rest device and retry");break;}
                for(unsigned i=0;i<plan.n_bins;i++) if(measured[i]>baseline.power[i]) baseline.power[i]=measured[i];
                baseline.passes++;baseline.count=plan.n_bins;
            }
            portENTER_CRITICAL(&lock);state.passes=passes;portEXIT_CRITICAL(&lock);
            publish_rows(comparable,changed,(uint32_t)((esp_timer_get_time()-started)/1000));
            if(job!=CELL_LEARN) {cell_status_t report;cell_monitor_get(&report);cell_report_observe(&report);}
            if(job==CELL_LEARN && baseline.passes==3) {
                baseline.checksum=cell_checksum(&baseline);
                portENTER_CRITICAL(&lock);
                if(receiver_epoch==job_epoch)state.needs_relearn=false;
                portEXIT_CRITICAL(&lock);
                message(save_baseline(band)?"Baseline saved to SD; START to compare":"Baseline ready in RAM; SD save failed");break;
            }
            if(!context_ok) {message(context_reason);break;}
            message(!quiet?"Motion detected: comparison suppressed":!valid_base?"No baseline: LEARN captures three passes":changed?"Persistent RF change; identity unknown":"No persistent rise above threshold");
            for(int i=0;i<20 && !cancel(NULL);i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
finished:
        if(cancel(NULL))invalidate();
        portENTER_CRITICAL(&lock);
        state.baseline=!state.needs_relearn && cell_baseline_valid(&baseline,band,tile_lat,tile_lon,located,external);
        state.busy=false;state.learning=false;
        portEXIT_CRITICAL(&lock);
        cell_status_t report;cell_monitor_get(&report);
        if(report.receiver_missing)cell_report_observe(&report);
        else if(cancel(NULL))cell_report_activity(&report,'S');
        else if(job==CELL_LTE)cell_report_activity(&report,complete_lte?'C':'F');
    }
}
void cell_monitor_init(void)
{
    if(worker) return;
    ls_radio_endpoint_subscribe(receiver_event,NULL);
    message("Select band, then LEARN or START");
    if(xTaskCreate(task,"cell_watch",8192,NULL,2,&worker)!=pdPASS) message("Worker allocation failed");
}
bool cell_monitor_request(int job,unsigned band,bool manual_site)
{
    if(!worker || band>=cell_band_count || job<CELL_WATCH || job>CELL_LTE) return false;
    portENTER_CRITICAL(&lock);
    if(state.busy) {portEXIT_CRITICAL(&lock);return false;}
    bool missing=state.receiver_missing,relearn=state.needs_relearn;
    memset(&state,0,sizeof(state));state.busy=true;state.learning=job==CELL_LEARN;state.band=band;
    state.lte=job==CELL_LTE;
    state.receiver_missing=missing;state.needs_relearn=relearn;job_epoch=receiver_epoch;
    state.manual_site=manual_site;
    stopped=false;command=job;manual=manual_site;
    portEXIT_CRITICAL(&lock);
    xTaskNotifyGive(worker);return true;
}
void cell_monitor_stop(void)
{
    portENTER_CRITICAL(&lock);stopped=true;portEXIT_CRITICAL(&lock);
}
void cell_monitor_wait_stopped(void)
{
    cell_monitor_stop();
    cell_status_t s;
    do {cell_monitor_get(&s);if(s.busy)vTaskDelay(pdMS_TO_TICKS(10));} while(s.busy);
}
