#include "rec_watch.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
static rec_watch_status_t state;
static ls_mixrf_status_t mix;
static rec_source_t source;
rec_source_t rec_watch_source(void){return source;}
bool rec_watch_select_source(rec_source_t next){if(state.enabled)return false;source=next;return true;}
bool ls_mixrf_capture(bool on,uint32_t hz){mix.capturing=on;mix.frequency=hz;return true;}
static uint32_t frequency=433920000;
bool ls_mixrf_start(void){mix.ready=mix.keyboard=mix.power=mix.cc=mix.nfc=mix.nrf=true;mix.cc_version=0x14;mix.nfc_identity=0x2a;strcpy(mix.status,"SIMULATED keyboard radios");return true;}
void ls_mixrf_snapshot(ls_mixrf_status_t *out){if(mix.receiving)mix.samples=(uint32_t)(esp_timer_get_time()/100000);if(out)*out=mix;}
bool ls_mixrf_receive(bool on,uint32_t hz){mix.receiving=mix.receive_requested=on;mix.frequency=hz;mix.rssi=-78;return true;}
void ls_mixrf_diagnostics(void){}
bool ls_mixrf_scan(bool on){mix.scanning=mix.scan_requested=on;mix.sweeps=12;mix.energy_hits=82;for(int i=0;i<84;i++)mix.occupancy[i]=(i>=10&&i<22)?160:(i>=55&&i<62)?90:0;return true;}
bool ls_mixrf_nfc_watch(bool on){mix.nfc_watching=mix.nfc_requested=on;return true;}
static int threshold=0,gap=30,minimum_edges=6;
static uint32_t min_pulse=40,max_span=8000000;
int rec_get_thresh(void){return threshold;}
void rec_set_thresh(int n){threshold=n;}
int rec_get_gap_ms(void){return gap;}
void rec_set_gap_ms(int n){gap=n;}
uint32_t rec_get_min_pulse(void){return min_pulse;}
void rec_set_min_pulse(uint32_t n){min_pulse=n;}
uint32_t rec_get_max_span(void){return max_span;}
void rec_set_max_span(uint32_t n){max_span=n;}
int rec_get_min_edges(void){return minimum_edges;}
void rec_set_min_edges(int n){minimum_edges=n;}
uint32_t rec_get_freq(void){return frequency;}
void rec_set_freq(uint32_t hz){frequency=hz;}
const char *rec_end_reason_name(int reason){return reason==1?"gap":"limit";}
bool rec_watch_start(void)
{
    state.ready=true;
    strcpy(state.storage,"SIMULATED archive; 2 files, < 520 KiB");
    state.count=3;
    for(int i=0;i<3;i++)state.event[i]=(rec_watch_event_t){.id=i+1,.frequency=433920000,.count=20+i,.edges=24,.span_us=8400};
    for(int i=0;i<3;i++)for(int j=0;j<24;j++)state.preview[i][j]=(j%2?-1:1)*(j%3?300:600);
    int32_t pulse[100];
    for(int frame=0;frame<2;frame++) {
        int base=frame*50;pulse[base]=300;pulse[base+1]=-9300;
        for(int bit=0;bit<24;bit++) {
            bool one=(0xA53C19>>(23-bit))&1;
            pulse[base+2+bit*2]=one?900:300;pulse[base+3+bit*2]=one?-300:-900;
        }
    }
    state.event[0].edges=100;state.event[0].span_us=76800;
    /* One of the three from the SX1262, with the modulation it was heard
       with. The archive really does hold a mix - the source is per capture,
       not per session - and without one here nothing that depends on a
       capture being replayable or exportable as FSK was reachable at all. */
    state.event[2].source=REC_SOURCE_SX1262;
    state.event[2].bitrate=2400;
    state.event[2].deviation_hz=18500;
    memcpy(state.preview[0],pulse,sizeof(state.preview[0]));
    rec_decode_ook24(pulse,100,&state.decoded[0]);
    return true;
}
bool rec_watch_enable(bool on){state.enabled=on;return true;}
bool rec_watch_enabled(void){return state.enabled;}
/* The bench has no radio to refuse a session, so nothing ever went wrong
   and the reason is always empty. */
const char *rec_watch_error(void){return "";}
/* No radio on the bench, so a replay goes nowhere - but it refuses under a
   running watch exactly as the real one does, which is the only outcome the
   screen has to say anything about. The power is kept so a test can check
   that the screen sent what the operator picked rather than a constant. */
static int sim_replay_dbm = -128;
int  rec_watch_sim_replay_dbm(void){return sim_replay_dbm;}
bool rec_watch_request_replay(uint32_t id,int dbm)
{(void)id;if(state.enabled)return false;sim_replay_dbm=dbm;return true;}
/* The FSK settings are plain state, so the bench keeps them for real - the
   screen reads them back to draw the setup list. */
static rec_fsk_mod_t sim_fsk={4800,25000,0x2DD42DD4u,32,59};
void rec_watch_fsk_get(rec_fsk_mod_t *out){if(out)*out=sim_fsk;}
/* No radio to sweep, but the sweep is a state machine as far as the screen
   is concerned - busy or not, with or without findings - and every control
   that acts on a running sweep was unreachable from the bench while this
   returned a flag that never changed. So it keeps the state instead: a
   request starts it, a stop ends it, and it leaves two findings behind for
   whatever wants to tune to one.

   The frequencies are inside the range asked for, because that is the one
   property a caller can check without knowing what the bench made up. */
static bool sim_sweeping;
static int  sim_found;
static rec_scan_bin_t sim_bin[2];
bool rec_watch_request_scan(uint32_t lo,uint32_t hi,uint32_t secs,int bins)
{
    (void)secs;(void)bins;
    if(state.enabled)return false;   /* the radio is busy watching */
    if(hi<=lo)return false;
    sim_sweeping=true;
    sim_found=2;
    sim_bin[0].hz=lo+(hi-lo)/4;   sim_bin[0].dbm=-72.0f;
    sim_bin[1].hz=lo+(hi-lo)*3/4; sim_bin[1].dbm=-88.0f;
    return true;
}
bool rec_watch_scan_busy(void){return sim_sweeping;}
int rec_watch_scan_progress(void){return sim_sweeping?50:0;}
const char *rec_watch_scan_stage(void){return sim_sweeping?"Sweeping":"";}
int rec_watch_scan_live(rec_scan_bin_t *o,int m){(void)o;(void)m;return 0;}
float rec_watch_scan_live_floor(void){return -120.0f;}
int rec_watch_scan_hits(void){return sim_found;}
void rec_watch_scan_stop(void){sim_sweeping=false;}
static rec_scan_on_hit_t sim_on_hit=REC_SCAN_ON_HIT_BUZZ;
void rec_watch_scan_on_hit(rec_scan_on_hit_t m){sim_on_hit=m;}
rec_scan_on_hit_t rec_watch_scan_on_hit_get(void){return sim_on_hit;}
uint32_t rec_watch_scan_last_hit(void){return 0;}
static float sim_gate=REC_SCAN_DETECT_DB;
float rec_watch_scan_threshold(void){return sim_gate;}
void rec_watch_scan_set_threshold(float db){sim_gate=db;}
void rec_watch_scan_preview_threshold(float db){sim_gate=db;}
int rec_watch_scan_events(void){return sim_found;}
bool rec_watch_scan_catch(uint32_t hz){(void)hz;if(!sim_sweeping)return false;sim_sweeping=false;return true;}
/* -1 for "no sweep has run", which is what the screen tests to decide
   whether to offer the findings at all - distinct from a sweep that ran and
   found nothing. */
int rec_watch_scan_result(rec_scan_bin_t *out,int max)
{
    if(!sim_found)return -1;
    int n=sim_found<max?sim_found:max;
    if(out)for(int i=0;i<n;i++)out[i]=sim_bin[i];
    return out?n:sim_found;
}
float rec_watch_scan_floor(void){return -120.0f;}
/* Nothing on the air to learn from, so the bench answers with a fixed
   reading once it has been asked - the screen's job is to lay it out and
   say how sure it is, and neither can be checked against a function that
   never returns anything. */
static bool sim_learned;
bool rec_watch_request_learn(uint32_t secs)
{(void)secs;if(state.enabled)return false;sim_learned=true;return true;}
bool rec_watch_learn_busy(void){return false;}
bool rec_watch_learn_result(rec_learn_t *out)
{
    if(!sim_learned)return false;
    if(out) {
        memset(out,0,sizeof(*out));
        out->bitrate=2400;
        out->deviation_hz=18500;
        out->sync_word=0xD391D391u;
        out->confidence=76;
        snprintf(out->note,sizeof(out->note),"valley between two tones");
    }
    return true;
}
bool rec_watch_learn_apply(void)
{
    if(!sim_learned || state.enabled)return false;
    sim_fsk.bitrate=2400;sim_fsk.deviation_hz=18500;sim_fsk.sync_word=0xD391D391u;
    return true;
}
bool rec_watch_fsk_set(const rec_fsk_mod_t *in){if(!in)return false;sim_fsk=*in;return true;}
bool rec_watch_request_export(uint32_t id)
{if(!id)return false;strcpy(state.export_status,"SIMULATED export; no SD write");return true;}
void rec_watch_snapshot(rec_watch_status_t *out){if(out)*out=state;}
bool rec_watch_request_pin(uint32_t id,bool pin)
{for(int i=0;i<state.count;i++)if(state.event[i].id==id){state.event[i].pinned=pin;return true;}return false;}
bool rec_watch_alert_target(const char *peer)
{if(!peer)return false;strncpy(state.peer,peer,16);state.peer[16]=0;state.alerts=peer[0]!=0;return true;}

bool ls_mixrf_card_scan(bool on){mix.card_requested=mix.card_scanning=on;mix.card_polls=12;mix.card_tx=12;return true;}

/* No finger on the bench: nothing is ever being dragged. */
bool ls_tui_touch_held(int *sc,int *sr,int *c,int *r){(void)sc;(void)sr;(void)c;(void)r;return false;}

static bool file_replay_busy;
static char file_replay_result[112];
static int file_replay_count, file_replay_dbm;
bool rec_watch_request_replay_file(const char *path,int dbm)
{
    if(!path || !path[0] || file_replay_busy || state.enabled)return false;
    file_replay_busy=true;file_replay_count++;file_replay_dbm=dbm;
    snprintf(file_replay_result,sizeof(file_replay_result),"Replay queued");return true;
}
bool rec_watch_replay_status(char *out,size_t len)
{if(out && len)snprintf(out,len,"%s",file_replay_result);return file_replay_busy;}
void rec_watch_sim_file_done(const char *result)
{file_replay_busy=false;snprintf(file_replay_result,sizeof(file_replay_result),"%s",result);}
int rec_watch_sim_file_count(void){return file_replay_count;}
int rec_watch_sim_file_dbm(void){return file_replay_dbm;}
