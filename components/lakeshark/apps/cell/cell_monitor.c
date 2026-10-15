#include "cell_monitor.h"
#include "cell_report.h"
#include "lte_sync.h"
#include "lte_mib.h"
#include "rtl-sdr.h"
#include "esp_attr.h"
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
static StaticTask_t worker_tcb;
static bool endpoint_subscribed;
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
static bool located, external, context_ok, quiet, baseline_saved;
static const char *context_reason;
static int64_t last_motion;
static int64_t last_imu_sample;
static cell_motion_t motion;
/* The last LTE search. Cells live in PSRAM; the lock guards them with state.
 * The clock error belongs to the dongle: a replug starts it over (ppm_stale),
 * since the tuner's correction is back at zero. */
static EXT_RAM_BSS_ATTR cell_lte_t cells[CELL_MAX_CELLS];
static unsigned cell_n;
static cell_ppm_t ppm_acc;
static int applied_ppm;
static bool ppm_verify;     /* a correction was applied and no cell has confirmed it yet */
static float ppm_before;    /* the estimate it was applied for */
static EXT_RAM_BSS_ATTR cell_status_t snapshot;   /* the worker's own copy, to keep its stack small */
static bool full_scan,no_mib;
static volatile bool ppm_stale;
static int64_t scan_t0;
#define LTE_CANDIDATES 48

/* A lost receiver invalidates live evidence even while the worker is blocked
 * in USB I/O. Do not touch worker-owned baseline/streak storage here. */
static void invalidate_locked(void)
{
    state.compared=false;state.changed=0;
    for(unsigned i=0;i<state.rows;i++)state.row[i].flagged=false;
}
static void ppm_publish_locked(void)
{
    cell_ppm_est_t e;
    state.ppm_known=cell_ppm_estimate(&ppm_acc,&e);
    state.ppm=state.ppm_known?e.ppm:0;state.ppm_spread=state.ppm_known?e.spread:0;
    state.ppm_cells=state.ppm_known?e.n:0;
    state.ppm_value=applied_ppm;state.ppm_applied=applied_ppm!=0;
}
static void receiver_event(const ls_radio_endpoint_event_t *event,void *arg)
{
    (void)arg;
    if(strcmp(event->endpoint_id,LS_RADIO_ENDPOINT_RTL_USB))return;
    portENTER_CRITICAL(&lock);
    if(event->kind==LS_RADIO_ENDPOINT_DETACHED) {
        receiver_epoch++;state.receiver_missing=true;state.needs_relearn=true;
        state.baseline=false;state.baseline_saved=false;invalidate_locked();
        ppm_stale=true;state.ppm_known=state.ppm_applied=false;state.ppm=state.ppm_spread=0;
        state.ppm_cells=0;state.ppm_value=0;
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
    portENTER_CRITICAL(&lock);
    *out=state;
    out->lte_count=cell_n;out->gated=!full_scan;out->mib_on=!no_mib;
    portEXIT_CRITICAL(&lock);
}
unsigned cell_monitor_cells(cell_lte_t *out,unsigned max)
{
    portENTER_CRITICAL(&lock);
    unsigned n=cell_n<max?cell_n:max;
    memcpy(out,cells,n*sizeof(cells[0]));
    portEXIT_CRITICAL(&lock);
    return n;
}
void cell_monitor_set_full_scan(bool full){portENTER_CRITICAL(&lock);full_scan=full;portEXIT_CRITICAL(&lock);}
void cell_monitor_set_mib(bool on){portENTER_CRITICAL(&lock);no_mib=!on;portEXIT_CRITICAL(&lock);}
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
    bool valid=cell_gps_valid(g.fix,g.last_fix_us,esp_timer_get_time(),g.lat_deg,g.lon_deg,g.hdop);
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
        if(!cell_motion_step(&motion,s.ax,s.ay,s.az,s.gx,s.gy,s.gz)) quiet=false;
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
            baseline=disk; baseline_saved=true; return true;
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
        state.baseline_saved=next.baseline && baseline_saved;
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
static FILE *lte_journal(void)
{
    if(!ls_sdcard_mounted())return NULL;
    mkdir("/sdcard/cell",0775);
    const char *path="/sdcard/cell/lte.jsonl";struct stat st;
    if(stat(path,&st)==0 && st.st_size>1024*1024) {
        if(remove("/sdcard/cell/lte.previous.jsonl")!=0 && errno!=ENOENT)return NULL;
        if(rename(path,"/sdcard/cell/lte.previous.jsonl")!=0)return NULL;
    }
    return fopen(path,"ab");
}
static void journal_line(const char *text)
{
    if(!ls_sdcard_mounted())return;
    FILE *f=lte_journal();if(!f){message("LTE found; SD journal unavailable");return;}
    bool ok=fputs(text,f)>=0;
    if(fflush(f) || fsync(fileno(f)))ok=false;
    if(fclose(f))ok=false;
    if(!ok)message("LTE found; SD journal write failed");
}
static unsigned long long journal_epoch(void)
{
    time_t epoch=time(NULL);
    return (unsigned long long)(epoch>1735689600?epoch:0);
}
static void log_lte(unsigned band,const lte_sync_result_t *r,const cell_lte_t *c)
{
    if(!ls_sdcard_mounted())return;
    ls_gps_state_t g;ls_gps_get(&g);
    bool fix=cell_gps_valid(g.fix,g.last_fix_us,esp_timer_get_time(),g.lat_deg,g.lon_deg,g.hdop);
    char gps[96]="null",power[16]="null",ppm[16]="null",text[360];
    if(fix)snprintf(gps,sizeof(gps),"[%.6f,%.6f]",g.lat_deg,g.lon_deg);
    if(c->rsrp10!=CELL_NO_POWER)snprintf(power,sizeof(power),"%.1f",c->rsrp10/10.);
    portENTER_CRITICAL(&lock);bool known=state.ppm_known;float value=state.ppm;portEXIT_CRITICAL(&lock);
    if(known)snprintf(ppm,sizeof(ppm),"%.2f",(double)value);
    snprintf(text,sizeof(text),"{\"kind\":\"LTE_SYNC\",\"epoch\":%llu,\"hz\":%lu,\"pci\":%d,\"hits\":%d,\"pairs\":%d,\"cfo_hz\":%d,\"pss\":%.3f,\"sss\":%.3f,\"band\":%u,\"earfcn\":%u,\"rsrp_dbfs\":%s,\"ppm\":%s,\"gps\":%s}\n",
        journal_epoch(),(unsigned long)c->hz,r->pci,r->hits,r->pairs,r->cfo_hz,(double)r->pss_score,(double)r->sss_score,
        cell_bands[band].band,c->earfcn,power,ppm,gps);
    journal_line(text);
}
static unsigned mark_col(unsigned band,uint32_t hz)
{
    uint32_t low=cell_bands[band].low,span=cell_bands[band].high-low;
    if(hz<low)return 0;
    unsigned col=(unsigned)((uint64_t)(hz-low)*96/span);
    return col>95?95:col;
}
static void mark_hz(unsigned band,uint32_t hz,uint8_t bit)
{
    portENTER_CRITICAL(&lock);state.mark[mark_col(band,hz)]|=bit;portEXIT_CRITICAL(&lock);
}
static void store_cell(const cell_lte_t *c)
{
    portENTER_CRITICAL(&lock);
    unsigned i=0;
    for(;i<cell_n;i++) if(cells[i].hz==c->hz && cells[i].pci==c->pci)break;
    if(i<CELL_MAX_CELLS) {
        if(i==cell_n)cell_n++;
        cells[i]=*c;
        state.last.hz=c->hz;state.last.pci=c->pci;
    }
    portEXIT_CRITICAL(&lock);
}
static void scan_progress(unsigned phase,unsigned tune,unsigned tunes,uint32_t hz)
{
    portENTER_CRITICAL(&lock);
    state.phase=phase;state.tune=tune;state.tunes=tunes;
    if(hz)state.frequency=hz;
    state.scan_ms=(uint32_t)((esp_timer_get_time()-scan_t0)/1000);state.elapsed_ms=state.scan_ms;
    portEXIT_CRITICAL(&lock);
}
static void occupancy_progress(uint32_t tune,uint32_t tunes,void *arg)
{
    const ls_sweep_plan_t *p=arg;
    scan_progress(CELL_PH_OCCUPANCY,tune,tunes,(uint32_t)ls_sweep_tune_center(p,tune-1));
}
/* The cheap pass: 1.92 MS/s energy sweep over the band, then which raster
 * points sit on a carrier wide enough to be LTE. */
static ls_radio_err_t lte_occupancy(unsigned band,cell_plateau_t *pl,unsigned *np,uint32_t *cand,unsigned *nc)
{
    ls_sweep_plan_t op;
    const uint32_t low=cell_bands[band].low,high=cell_bands[band].high;
    if(!ls_sweep_plan(low,high,CELL_BIN,1920000,&op) || op.n_bins>CELL_MAX_BINS)return LS_RADIO_ERR_INVALID;
    scan_progress(CELL_PH_OCCUPANCY,0,op.n_tunes,0);
    message("LTE: measuring which carriers are on air");
    ls_radio_err_t result=LS_RADIO_ERR_BUSY;
    for(int retry=0;retry<10 && result==LS_RADIO_ERR_BUSY && !cancel(NULL);retry++) {
        result=ls_sweep_run_endpoint(&op,CELL_GAIN,8,false,measured,occupancy_progress,cancel,&op,LS_RADIO_ENDPOINT_RTL_USB);
        if(result==LS_RADIO_ERR_BUSY)vTaskDelay(pdMS_TO_TICKS(300));
    }
    if(cancel(NULL))return LS_RADIO_ERR_STOPPED;
    if(result!=LS_RADIO_OK)return result;
    if(ls_sweep_gaps(&op,measured))return LS_RADIO_ERR_IO;
    int floor_db;
    *np=cell_plateaus(measured,op.n_bins,low,CELL_BIN,pl,CELL_PLATEAU_MAX,&floor_db);
    *nc=cell_candidates(pl,*np,low+700000,high-700000,cand,LTE_CANDIDATES);
    int8_t spectrum[96];uint8_t mark[96];
    memset(spectrum,CELL_MISSING,sizeof(spectrum));memset(mark,0,sizeof(mark));
    for(unsigned i=0;i<op.n_bins;i++) {
        unsigned col=i*96/op.n_bins;
        if(measured[i]!=CELL_MISSING && measured[i]>spectrum[col])spectrum[col]=measured[i];
    }
    for(unsigned p=0;p<*np;p++)
        for(unsigned col=mark_col(band,pl[p].lo);col<=mark_col(band,pl[p].hi-1);col++)mark[col]|=1;
    portENTER_CRITICAL(&lock);
    state.spectrum_count=96;memcpy(state.spectrum,spectrum,sizeof(spectrum));
    memset(state.reference,CELL_MISSING,sizeof(state.reference));memcpy(state.mark,mark,sizeof(mark));
    state.plateaus=*np;state.candidates=*nc;
    portEXIT_CRITICAL(&lock);
    return LS_RADIO_OK;
}
/* One continuous capture at the tuner's centre, rejecting any that dropped
 * samples or arrived slowly. */
static bool lte_capture(ls_radio_session_t *session,uint8_t *iq,size_t bytes,ls_radio_err_t *error)
{
    ls_radio_err_t e=ls_radio_iq_start(session);
    size_t discard=0,n=0,total=0;int64_t start=esp_timer_get_time();uint64_t before=0;
    if(e==LS_RADIO_OK) {
        while(discard<65536 && !cancel(NULL)) {
            e=ls_radio_iq_read(session,iq,4096,100,&n);
            if(e!=LS_RADIO_OK)break;
            discard+=n;
        }
        start=esp_timer_get_time();before=rtlsdr_stream_dropped();
        while(e==LS_RADIO_OK && total<bytes && !cancel(NULL)) {
            size_t need=bytes-total;if(need>16384)need=16384;
            n=0;
            e=ls_radio_iq_read(session,iq+total,need,100,&n);total+=n;
            if(esp_timer_get_time()-start>1000000){e=LS_RADIO_ERR_TIMEOUT;break;}
        }
    }
    bool usable=e==LS_RADIO_OK && total==bytes && rtlsdr_stream_dropped()==before &&
        esp_timer_get_time()-start<(int64_t)bytes*25/48;   /* twice the sample time */
    ls_radio_iq_stop(session);
    *error=e;
    return usable;
}
/* Narrow the oscillator search to what the cells already heard say. While a
 * fresh correction is still unproven the whole range is searched, so a
 * correction that moved the carriers the wrong way cannot hide them. */
static void ppm_window(uint32_t hz,int *centre,int *span)
{
    cell_ppm_est_t e;
    bool known=!ppm_verify && cell_ppm_estimate(&ppm_acc,&e);
    cell_cfo_window(known,known?e.ppm:0.f,(float)applied_ppm,hz,centre,span);
}
/* Two cells that agree on the dongle's error make it worth correcting. The
 * first capture after a correction must then land where the estimate says,
 * or the correction is taken back: its sign is not assumed, it is checked.
 * `newest` is the absolute error that capture's cell implies. */
static void ppm_update(ls_radio_session_t *session,bool after_correction,float newest)
{
    cell_ppm_est_t e;
    if(after_correction && ppm_verify) {
        if(fabsf(newest-ppm_before)>3.f) {
            ls_radio_iq_set_ppm(session,0);
            applied_ppm=0;cell_ppm_reset(&ppm_acc);
            message("Clock correction did not hold: taken back");
        }
        ppm_verify=false;
    }
    if(cell_ppm_estimate(&ppm_acc,&e) && e.agree>=2 && fabsf(e.ppm-(float)applied_ppm)>=1.f) {
        int want=(int)lroundf(e.ppm);
        if(ls_radio_iq_set_ppm(session,want)==LS_RADIO_OK){applied_ppm=want;ppm_before=e.ppm;ppm_verify=true;}
    }
    portENTER_CRITICAL(&lock);ppm_publish_locked();portEXIT_CRITICAL(&lock);
}
static void lte_found(unsigned band,uint32_t hz,const uint8_t *iq,size_t samples,void *mws,
                      const lte_sync_set_t *set,ls_radio_session_t *session)
{
    cell_lte_t row[LTE_SYNC_MAX_CELLS];
    /* The capture was taken under the correction in force now, whatever the
     * observations below lead to. */
    const float applied=(float)applied_ppm;
    const bool after_correction=ppm_verify;
    float newest=0;bool seen=false;
    for(int i=0;i<set->count;i++) {
        const lte_sync_result_t *r=&set->cell[i].r;
        row[i]=(cell_lte_t){.hz=hz,.pci=(uint16_t)r->pci,.cfo=(int16_t)r->cfo_hz,
            .hits=(uint8_t)(r->hits>255?255:r->hits),.rsrp10=CELL_NO_POWER,.pss=r->pss_score,.sss=r->sss_score};
        unsigned earfcn;
        if(cell_hz_to_earfcn(band,hz,&earfcn))row[i].earfcn=(uint16_t)earfcn;
        float db;
        if(lte_mib_crs_power(iq,samples,r->pci,set->cell[i].frame_start,r->cfo_hz,mws,&db))
            row[i].rsrp10=(int16_t)lroundf(db*10.f);
        if(cell_ppm_add(&ppm_acc,hz,r->pci,r->cfo_hz,applied)){newest=applied-(float)r->cfo_hz*1e6f/(float)hz;seen=true;}
    }
    if(seen)ppm_update(session,after_correction,newest);
    for(int i=0;i<set->count;i++) {
        const lte_sync_result_t *r=&set->cell[i].r;
        store_cell(&row[i]);
        mark_hz(band,hz,4);
        log_lte(band,r,&row[i]);
        portENTER_CRITICAL(&lock);float ppm=state.ppm;portEXIT_CRITICAL(&lock);
        printf("CELL_LTE hz=%lu earfcn=%u pci=%d hits=%d cfo=%d ppm=%.2f rsrp=%.1f pss=%.3f sss=%.3f\n",
            (unsigned long)hz,row[i].earfcn,r->pci,r->hits,r->cfo_hz,(double)ppm,
            row[i].rsrp10==CELL_NO_POWER?-999.:row[i].rsrp10/10.,(double)r->pss_score,(double)r->sss_score);
        cell_monitor_get(&snapshot);cell_report_observe(&snapshot);
    }
}
/* One 80 ms capture per carrier that gave a cell: the broadcast channel of
 * each cell on it, and a steadier power over eight frames. */
static ls_radio_err_t lte_mib_pass(ls_radio_session_t *session,uint8_t *iq,size_t bytes,
                                   void *ws,void *mws,unsigned *attempted)
{
    static EXT_RAM_BSS_ATTR cell_lte_t snap[CELL_MAX_CELLS];
    uint32_t freqs[CELL_MAX_CELLS];unsigned nf=0,n;
    portENTER_CRITICAL(&lock);
    n=cell_n;memcpy(snap,cells,n*sizeof(snap[0]));
    portEXIT_CRITICAL(&lock);
    for(unsigned i=0;i<n;i++) {
        unsigned k=0;
        for(;k<nf;k++) if(freqs[k]==snap[i].hz)break;
        if(k==nf)freqs[nf++]=snap[i].hz;
    }
    ls_radio_err_t error=LS_RADIO_OK;
    for(unsigned f=0;f<nf && !cancel(NULL) && error==LS_RADIO_OK;f++) {
        scan_progress(CELL_PH_MIB,f+1,nf,freqs[f]);
        message("LTE: reading the broadcast channel (MIB)");
        uint64_t tuned;
        error=ls_radio_iq_retune(session,freqs[f],false,&tuned);
        if(error!=LS_RADIO_OK)break;
        bool usable=lte_capture(session,iq,bytes,&error);
        if(cancel(NULL) || !usable)continue;
        int centre,span;ppm_window(freqs[f],&centre,&span);
        lte_sync_set_t set;int64_t yielded=esp_timer_get_time();
        if(!lte_sync_find_cells(iq,bytes/2,ws,&set,lte_yield,&yielded,centre,span))continue;
        for(int c=0;c<set.count && !cancel(NULL);c++) {
            const lte_sync_result_t *r=&set.cell[c].r;
            unsigned i=0;
            for(;i<n;i++) if(snap[i].hz==freqs[f] && snap[i].pci==r->pci)break;
            if(i==n)continue;
            cell_lte_t row=snap[i];
            float db;
            if(lte_mib_crs_power(iq,bytes/2,r->pci,set.cell[c].frame_start,r->cfo_hz,mws,&db))
                row.rsrp10=(int16_t)lroundf(db*10.f);
            lte_mib_result_t m;
            int64_t began=esp_timer_get_time();
            bool ok=lte_mib_confirm(iq,bytes/2,r->pci,set.cell[c].frame_start,r->cfo_hz,mws,&m,lte_yield,&yielded);
            if(cancel(NULL))break;
            ++*attempted;
            row.mib=ok?CELL_MIB_OK:CELL_MIB_FAILED;
            if(ok){row.n_rb=(uint8_t)m.n_rb;row.ports=(uint8_t)m.antenna_ports;row.sfn=(int16_t)m.sfn;}
            snap[i]=row;
            store_cell(&row);
            portENTER_CRITICAL(&lock);state.mib_tried=*attempted;if(ok)state.mib_done++;portEXIT_CRITICAL(&lock);
            char text[200];
            snprintf(text,sizeof(text),"{\"kind\":\"LTE_MIB\",\"epoch\":%llu,\"hz\":%lu,\"pci\":%d,\"found\":%s,\"n_rb\":%d,\"ports\":%d,\"sfn\":%d,\"phich_duration\":%d,\"phich_resource\":%d,\"frames\":%d,\"ms\":%lu}\n",
                journal_epoch(),(unsigned long)freqs[f],r->pci,ok?"true":"false",ok?m.n_rb:0,ok?m.antenna_ports:0,ok?m.sfn:0,
                ok?m.phich_duration:0,ok?m.phich_resource:0,ok?m.frames:0,(unsigned long)((esp_timer_get_time()-began)/1000));
            journal_line(text);
            printf("CELL_MIB hz=%lu pci=%d found=%d n_rb=%d ports=%d sfn=%d ms=%lu\n",(unsigned long)freqs[f],r->pci,ok,
                ok?m.n_rb:0,ok?m.antenna_ports:0,ok?m.sfn:0,(unsigned long)((esp_timer_get_time()-began)/1000));
        }
    }
    return error;
}
static void ppm_zero(void)
{
    ls_radio_requirements_t req={.required_caps=LS_RADIO_RX_IQ_U8,
        .min_hz=cell_bands[0].low,.max_hz=cell_bands[0].high,
        .sample_rate_hz=1920000,.iq_format=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id=LS_RADIO_ENDPOINT_RTL_USB};
    ls_radio_session_t *session=NULL;
    ls_radio_err_t error=LS_RADIO_ERR_BUSY;
    for(int retry=0;retry<10 && error==LS_RADIO_ERR_BUSY && !cancel(NULL);retry++) {
        error=ls_radio_acquire("cell-ppm",&req,&session);
        if(error==LS_RADIO_ERR_BUSY)vTaskDelay(pdMS_TO_TICKS(300));
    }
    if(error==LS_RADIO_OK) {
        error=applied_ppm?ls_radio_iq_set_ppm(session,0):LS_RADIO_OK;
        ls_radio_release(session);
    }
    if(error==LS_RADIO_OK) {
        cell_ppm_reset(&ppm_acc);applied_ppm=0;ppm_verify=false;
        portENTER_CRITICAL(&lock);ppm_publish_locked();portEXIT_CRITICAL(&lock);
        message("Dongle clock correction cleared");
    } else {
        char text[80];snprintf(text,sizeof(text),"Clock correction not cleared: %s",ls_radio_err_name(error));message(text);
    }
}
static bool lte_scan(unsigned band)
{
    const size_t sync_bytes=115200;     /* 30 ms, six possible synchronization occasions */
    const size_t mib_bytes=307200;      /* 80 ms, eight frames */
    uint8_t *iq=heap_caps_malloc(mib_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    void *ws=heap_caps_malloc(lte_sync_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    void *mws=heap_caps_malloc(lte_mib_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ls_radio_session_t *session=NULL;
    ls_radio_err_t error=LS_RADIO_ERR_NO_MEMORY,mib_error=LS_RADIO_OK;
    unsigned tunes=0,attempted=0,usable_count=0,discarded_count=0,plateau_n=0,mib_attempted=0;
    uint32_t cand[LTE_CANDIDATES];
    cell_plateau_t plateau[CELL_PLATEAU_MAX];
    const uint32_t low=cell_bands[band].low,high=cell_bands[band].high;
    const uint32_t first=low+700000,last=high-700000;
    bool gated,mib_wanted,empty=false;
    uint32_t occupancy_ms=0,sync_ms=0,mib_ms=0;
    int64_t phase_start;
    scan_t0=esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    gated=!full_scan;mib_wanted=!no_mib;cell_n=0;
    memset(state.mark,0,sizeof(state.mark));
    state.phase=CELL_PH_IDLE;state.mib_tried=state.mib_done=0;
    portEXIT_CRITICAL(&lock);
    if(!iq || !ws || !mws){message("LTE allocation failed");goto done;}
    if(ppm_stale){cell_ppm_reset(&ppm_acc);applied_ppm=0;ppm_verify=false;ppm_stale=false;}
    portENTER_CRITICAL(&lock);ppm_publish_locked();state.gated=gated;portEXIT_CRITICAL(&lock);
    if(gated) {
        phase_start=esp_timer_get_time();
        error=lte_occupancy(band,plateau,&plateau_n,cand,&tunes);
        occupancy_ms=(uint32_t)((esp_timer_get_time()-phase_start)/1000);
        if(error!=LS_RADIO_OK)goto done;
        empty=tunes==0;
        if(empty){message("LTE: no carrier above floor; try LTE SCAN: FULL");goto done;}
    } else tunes=(last-first)/CELL_RASTER_HZ+1;
    {
        ls_radio_requirements_t req={.required_caps=LS_RADIO_RX_IQ_U8,
            .min_hz=low,.max_hz=high,
            .sample_rate_hz=1920000,.iq_format=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
            .preferred_endpoint_id=LS_RADIO_ENDPOINT_RTL_USB};
        for(int retry=0;retry<10 && !cancel(NULL);retry++) {
            error=ls_radio_acquire("cell-lte",&req,&session);
            if(error!=LS_RADIO_ERR_BUSY)break;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if(error!=LS_RADIO_OK)goto done;
    phase_start=esp_timer_get_time();
    for(unsigned i=0;i<tunes && !cancel(NULL);i++) {
        uint32_t hz=gated?cand[i]:first+CELL_RASTER_HZ*i;
        portENTER_CRITICAL(&lock);state.lte_attempted=++attempted;portEXIT_CRITICAL(&lock);
        scan_progress(CELL_PH_SYNC,i+1,tunes,hz);
        cell_monitor_get(&snapshot);cell_report_activity(&snapshot,'P');
        message("LTE: acquiring 30 ms of continuous IQ");
        if(i==0) {
            ls_radio_iq_config_t cfg={.center_hz=hz,.sample_rate_hz=1920000,.bandwidth_hz=1920000,
                .gain_mode=LS_RADIO_GAIN_MANUAL,.gain_tenths_db=CELL_GAIN},actual;
            error=ls_radio_iq_configure(session,&cfg,&actual);
            if(error!=LS_RADIO_OK)break;
            if(actual.sample_rate_hz!=1920000){error=LS_RADIO_ERR_UNSUPPORTED;break;}
        } else {
            uint64_t tuned;
            error=ls_radio_iq_retune(session,hz,false,&tuned);
            if(error!=LS_RADIO_OK)break;
        }
        bool usable=lte_capture(session,iq,sync_bytes,&error);
        if(cancel(NULL))break;
        mark_hz(band,hz,2);
        if(!usable) {
            portENTER_CRITICAL(&lock);state.lte_discarded=++discarded_count;portEXIT_CRITICAL(&lock);
            if(error!=LS_RADIO_OK)break;
            message("LTE IQ gap / slow capture: discarded");continue;
        }
        portENTER_CRITICAL(&lock);state.lte_usable=++usable_count;portEXIT_CRITICAL(&lock);
        message("LTE: matching repeated PSS and SSS");
        int centre,span;ppm_window(hz,&centre,&span);
        lte_sync_set_t set;int64_t yielded=esp_timer_get_time();
        bool found=lte_sync_find_cells(iq,sync_bytes/2,ws,&set,lte_yield,&yielded,centre,span);
        if(cancel(NULL))break;
        if(found)lte_found(band,hz,iq,sync_bytes/2,mws,&set,session);
    }
    sync_ms=(uint32_t)((esp_timer_get_time()-phase_start)/1000);
    if(!cancel(NULL) && error==LS_RADIO_OK && mib_wanted) {
        portENTER_CRITICAL(&lock);bool any=cell_n!=0;portEXIT_CRITICAL(&lock);
        if(any) {
            phase_start=esp_timer_get_time();
            mib_error=lte_mib_pass(session,iq,mib_bytes,ws,mws,&mib_attempted);
            mib_ms=(uint32_t)((esp_timer_get_time()-phase_start)/1000);
        }
    }
    portENTER_CRITICAL(&lock);state.phase=CELL_PH_IDLE;state.tune=tunes;state.tunes=tunes;portEXIT_CRITICAL(&lock);
    if(cancel(NULL))message("LTE search stopped; cells found so far are kept");
    else if(error==LS_RADIO_OK && mib_error!=LS_RADIO_OK){
        char text[80];snprintf(text,sizeof(text),"LTE cells found; MIB read stopped: %s",ls_radio_err_name(mib_error));message(text);
    } else if(error==LS_RADIO_OK)message(cell_coverage_complete(tunes,attempted,usable_count,discarded_count)?
        (gated?"LTE pass (occupied carriers) done; no verdict":"LTE pass complete; no simulator verdict"):
        usable_count?"LTE partial coverage; windows discarded":"LTE failed; no usable windows");
done:
    if(session){ls_radio_iq_stop(session);ls_radio_release(session);}
    free(iq);free(ws);free(mws);
    portENTER_CRITICAL(&lock);state.phase=CELL_PH_IDLE;portEXIT_CRITICAL(&lock);
    if(cancel(NULL))message("LTE search stopped; cells found so far are kept");
    else if(error!=LS_RADIO_OK){char text[80];snprintf(text,sizeof(text),"LTE receiver: %s",ls_radio_err_name(error));message(text);}
    bool complete=!cancel(NULL) && error==LS_RADIO_OK &&
        (empty || cell_coverage_complete(tunes,attempted,usable_count,discarded_count));
    {
        cell_monitor_get(&snapshot);
        printf("CELL_SCAN band=%u mode=%s occupancy_ms=%lu plateaus=%u candidates=%u sync_ms=%lu mib_ms=%lu mib=%u/%u cells=%u ppm=%.2f applied=%d total_ms=%lu complete=%d\n",
            cell_bands[band].band,gated?"gated":"full",(unsigned long)occupancy_ms,plateau_n,gated?tunes:0,
            (unsigned long)sync_ms,(unsigned long)mib_ms,snapshot.mib_done,mib_attempted,snapshot.lte_count,
            snapshot.ppm_known?(double)snapshot.ppm:0.,snapshot.ppm_value,(unsigned long)((esp_timer_get_time()-scan_t0)/1000),complete);
    }
    if(ls_sdcard_mounted()) {
        char text[240];
        snprintf(text,sizeof(text),"{\"kind\":\"LTE_COVERAGE\",\"band\":%u,\"planned\":%u,\"attempted\":%u,\"usable\":%u,\"discarded\":%u,\"gated\":%s,\"plateaus\":%u,\"occupancy_ms\":%lu,\"sync_ms\":%lu,\"mib_ms\":%lu,\"complete\":%s}\n",
            band,tunes,attempted,usable_count,discarded_count,gated?"true":"false",plateau_n,
            (unsigned long)occupancy_ms,(unsigned long)sync_ms,(unsigned long)mib_ms,complete?"true":"false");
        journal_line(text);
    }
    return complete;
}
static void task(void *arg)
{
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        cell_status_t initial;cell_monitor_get(&initial);
        unsigned band=initial.band; int job=command;bool complete_lte=false;
        if(initial.needs_relearn) {memset(&baseline,0,sizeof(baseline));baseline_saved=false;}
        located=position(&tile_lat,&tile_lon);external=ls_board_hw_antenna_is_external();
        portENTER_CRITICAL(&lock);
        state.located=located;state.external=external;state.sd=ls_sdcard_mounted();
        portEXIT_CRITICAL(&lock);
        if(job==CELL_LTE){complete_lte=lte_scan(band);goto finished;}
        if(job==CELL_PPM_ZERO){ppm_zero();goto finished;}
        if(!manual && !located) {invalidate();message("GPS NO FIX: wait or select SITE: LOCAL");goto finished;}
        memset(streak,0,sizeof(streak));
        if(!cell_baseline_valid(&baseline,band,tile_lat,tile_lon,located,external)) {memset(&baseline,0,sizeof(baseline));baseline_saved=false;}
        if(job==CELL_LOAD && initial.needs_relearn) {
            message("Receiver changed: LEARN a fresh antenna baseline");goto finished;
        }
        if(job==CELL_LOAD || (located && job==CELL_WATCH && !initial.needs_relearn && baseline.passes!=3)) {
            bool loaded=read_baseline(band);
            if(job==CELL_LOAD) {
                message(loaded?(located?"GPS baseline loaded from SD":"Local baseline loaded: verify same site"):
                        "No valid SD baseline for this site / band");
                goto finished;
            }
        }
        if(job==CELL_LEARN) {
            memset(&baseline,0,sizeof(baseline));
            baseline_saved=false;
            baseline.magic=0x43454c4c;baseline.version=CELL_BASELINE_VERSION;baseline.band=band;
            baseline.receiver=CELL_RECEIVER_RTL;baseline.calibration=CELL_CALIBRATION;
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
        /* The settling time also learns what the gyro reads at rest. */
        cell_motion_reset(&motion);
        for(int i=0;i<12 && !cancel(NULL);i++) {
            ls_imu_sample_t sample;
            if(ls_imu_read(&sample)) {
                last_imu_sample=esp_timer_get_time();
                cell_motion_step(&motion,sample.ax,sample.ay,sample.az,sample.gx,sample.gy,sample.gz);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        while(!cancel(NULL)) {
            context_ok=true;context_reason="Site changed";quiet=true;last_motion=0;check_context();
            int64_t started=esp_timer_get_time();
            message(job==CELL_LEARN?"Learning: keep antenna and device still":"Sweeping cellular downlink spectrum");
            portENTER_CRITICAL(&lock);state.tune=0;state.tunes=plan.n_tunes;portEXIT_CRITICAL(&lock);
            ls_radio_err_t result=ls_sweep_run_endpoint(&plan,CELL_GAIN,8,false,measured,progress,cancel,NULL,LS_RADIO_ENDPOINT_RTL_USB);
            if(result==LS_RADIO_ERR_BUSY && passes==0 && esp_timer_get_time()-started<1000000) {
                message("Waiting for receiver to park");vTaskDelay(pdMS_TO_TICKS(300));
                /* Bounded retry; a different owner may legitimately hold it. */
                for(int retry=0;retry<10 && result==LS_RADIO_ERR_BUSY && !cancel(NULL);retry++) {
                    result=ls_sweep_run_endpoint(&plan,CELL_GAIN,8,false,measured,progress,cancel,NULL,LS_RADIO_ENDPOINT_RTL_USB);
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
                check_context();
                if(!context_ok) {baseline.passes=0;invalidate();message(context_reason);break;}
                baseline.checksum=cell_checksum(&baseline);
                portENTER_CRITICAL(&lock);
                if(receiver_epoch==job_epoch)state.needs_relearn=false;
                portEXIT_CRITICAL(&lock);
                baseline_saved=save_baseline(band);
                message(baseline_saved?"Baseline saved to SD; START to compare":"Baseline ready in RAM; SD save failed");break;
            }
            if(!context_ok) {message(context_reason);break;}
            message(!quiet?"Motion detected: comparison suppressed":!valid_base?"No baseline: LEARN captures three passes":changed?"Persistent RF change; identity unknown":"No persistent rise above threshold");
            for(int i=0;i<20 && !cancel(NULL);i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
finished:
        if(cancel(NULL))invalidate();
        portENTER_CRITICAL(&lock);
        state.baseline=(manual || located) && !state.needs_relearn && cell_baseline_valid(&baseline,band,tile_lat,tile_lon,located,external);
        state.baseline_saved=state.baseline && baseline_saved;
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
    if(!endpoint_subscribed) {
        endpoint_subscribed=ls_radio_endpoint_subscribe(receiver_event,NULL)>=0;
        if(!endpoint_subscribed) {message("Receiver subscription failed");return;}
    }
    message("Select band, then LEARN or START");
    /* Permanent worker: PSRAM stack avoids exhausting the UI's internal heap.
       ESP-IDF task stack sizes are bytes, including the static creation API. */
    StackType_t *stack=heap_caps_malloc(8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!stack) {message("Worker allocation failed");return;}
    worker=xTaskCreateStaticPinnedToCore(task,"cell_watch",8192,NULL,2,
                                          stack,&worker_tcb,tskNO_AFFINITY);
    if(!worker) {
        heap_caps_free(stack);
        message("Worker allocation failed");
    }
}
bool cell_monitor_request(int job,unsigned band,bool manual_site)
{
    if(!worker || band>=cell_band_count || job<CELL_WATCH || job>CELL_PPM_ZERO) return false;
    portENTER_CRITICAL(&lock);
    if(state.busy) {portEXIT_CRITICAL(&lock);return false;}
    bool missing=state.receiver_missing,relearn=state.needs_relearn;
    if(job==CELL_PPM_ZERO)state.busy=true;   /* a short chore: the screen keeps what it shows */
    else {
        memset(&state,0,sizeof(state));state.busy=true;state.learning=job==CELL_LEARN;state.band=band;
        state.lte=job==CELL_LTE;ppm_publish_locked();
        state.receiver_missing=missing;state.needs_relearn=relearn;
        state.manual_site=manual_site;
    }
    job_epoch=receiver_epoch;
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
bool cell_monitor_clear_ppm(void)
{
    cell_status_t s;cell_monitor_get(&s);
    return cell_monitor_request(CELL_PPM_ZERO,s.band,s.manual_site);
}
