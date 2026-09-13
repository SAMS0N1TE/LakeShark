#include "rec_watch.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
#include "esp_timer.h"
#include <string.h>
static rec_watch_status_t state;
static uint32_t frequency=433920000;
static ls_mixrf_status_t mix;
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
    return true;
}
bool rec_watch_enable(bool on){state.enabled=on;return true;}
bool rec_watch_enabled(void){return state.enabled;}
bool rec_watch_request_export(uint32_t id)
{if(!id)return false;strcpy(state.export_status,"SIMULATED export; no SD write");return true;}
void rec_watch_snapshot(rec_watch_status_t *out){if(out)*out=state;}
bool rec_watch_request_pin(uint32_t id,bool pin)
{for(int i=0;i<state.count;i++)if(state.event[i].id==id){state.event[i].pinned=pin;return true;}return false;}
bool rec_watch_alert_target(const char *peer)
{if(!peer)return false;strncpy(state.peer,peer,16);state.peer[16]=0;state.alerts=peer[0]!=0;return true;}

bool ls_mixrf_card_scan(bool on){mix.card_requested=mix.card_scanning=on;mix.card_polls=12;mix.card_tx=12;return true;}
