/* Radio RX API 2.4. Included in the session owner: lifetime and validation
 * are identical on board and simulator. Capture only copies to SPSC slots;
 * all FIR/FFT/protocol work runs on lap_io, never the screen/capture task. */
#include <math.h>
#include <limits.h>
#include "esp_timer.h"
#include "apps/fm/fm_dsp.h"
#include "apps/fm/fm_lr_dsp.h"
#include "apps/fm/pocsag.h"
#include "apps/fm/aprs.h"
#include "apps/fm/ais.h"
#include "apps/fm/same.h"
#include "apps/acars/acars.h"
#include "apps/acars/acars_resample.h"
#include "apps/adsb/mode-s.h"
#include "dsp/hf_demod.h"
#include "apps/fm/flex.h"
#include "apps/p25/dsp_pipeline.h"
#include "apps/p25/p25_tsbk.h"
#include "apps/p25/p25p1_check_nid.h"

#define RX24_IN 32u
#define RX24_OUT 8u
#define RX24_EVENTS 32u
#define RX24_FFT 2048u
#define RX24_TAPS 127u
#define RX24_PAYLOAD 256u
#define RX24_PI 3.14159265358979323846f
#define RX24_LR ((1u<<LAP_DEC_FM)|(1u<<LAP_DEC_AM)|(1u<<LAP_DEC_POCSAG)|(1u<<LAP_DEC_FLEX)|(1u<<LAP_DEC_APRS)|(1u<<LAP_DEC_ACARS)|(1u<<LAP_DEC_SAME))
#define RX24_DECODERS ((1u<<LAP_DEC_FM)|(1u<<LAP_DEC_AM)|(1u<<LAP_DEC_USB)|(1u<<LAP_DEC_LSB)|(1u<<LAP_DEC_CW)|(1u<<LAP_DEC_POCSAG)|(1u<<LAP_DEC_APRS)|(1u<<LAP_DEC_ACARS)|(1u<<LAP_DEC_AIS)|(1u<<LAP_DEC_ADSB)|(1u<<LAP_DEC_SAME)|(1u<<LAP_DEC_P25)|(1u<<LAP_DEC_FLEX)|(1u<<LAP_DEC_WFM))

typedef struct {
    uint64_t first, center, at;
    uint32_t rate, epoch, flags;
    int32_t gain;
    unsigned pairs;
} rx24_meta_t;
typedef struct { lap_event_header_t h; uint8_t payload[RX24_PAYLOAD]; } rx24_event_t;
typedef struct {
    uint64_t sync;
    unsigned fill;
    uint8_t dibits[33+P25_TSDU_DIBIT_COUNT];
    float history[24], sum, threshold;
    unsigned history_at;
    uint16_t nac;
    uint8_t duid, corrected;
} rx24_p25_phase_t;
struct rx24 {
    atomic_bool wanted;
    atomic_uint head, tail, input_drops, epoch;
    atomic_int gain;
    uint32_t pending_flags;
    rx24_meta_t in_meta[RX24_IN];
    int16_t in[RX24_IN][SLOT_PAIRS*2];
    /* Mailboxes and published outputs under the session lock. */
    int channel, channel_state, decoder, decoder_state;
    bool channel_change, decoder_change, spec_change;
    lap_channel_config_t channel_cfg;
    lap_decoder_config_t decoder_cfg;
    lap_spectrum_config_t spec_cfg;
    uint32_t out_head, out_tail, out_offset, out_drops;
    rx24_meta_t out_meta[RX24_OUT];
    int16_t out[RX24_OUT][SLOT_PAIRS*2];
    unsigned out_fill;
    bool out_pending_gap;
    uint32_t event_head, event_tail, event_count, event_drops;
    rx24_event_t events[RX24_EVENTS];
    lap_spectrum_meta_t line_meta;
    float line[RX24_FFT];
    lap_rx_command_t commands[8];
    int pending_command;
    /* Worker only. */
    lap_channel_config_t channel_run;
    int channel_run_id;
    int16_t dsp_iq[4096];
    unsigned dsp_fill;
    lap_iq_meta2_t dsp_meta;
    lap_spectrum_config_t spec_run;
    uint32_t decoder_run, seen_epoch, seen_drops, seen_rate;
    rx24_meta_t current;
    unsigned decimation, dec_phase, fir_at;
    float taps[RX24_TAPS], fi[RX24_TAPS], fq[RX24_TAPS];
    float osc_i, osc_q, step_i, step_q;
    uint64_t output_sample;
    unsigned spec_fill, spec_frames, spec_skip;
    float re[RX24_FFT], im[RX24_FFT], acc[RX24_FFT], win[RX24_FFT];
    fm_dsp_t fm;
    fm_lr_dsp_t lr_fm;
    hf_demod_t hf;
    pocsag_ctx_t *pocsag[3];
    flex_ctx_t *flex[4];
    dsp_state_t p25_dsp;
    dsd_state p25_state;
    rx24_p25_phase_t p25_phase[20];
    uint64_t p25_sample, p25_last_event;
    uint8_t p25_corrected;

    aprs_ctx_t *aprs;
    ais_ctx_t *ais;
    same_ctx_t *same;
    acars_ctx_t *acars;
    acars_state_t acars_state;
    acars_rs_t acars_rs;
    mode_s_t modes;
    uint16_t mag[SLOT_PAIRS+241];
    unsigned mag_carry, mag_start;
    uint8_t u8[SLOT_PAIRS*2];
    float disc[1100], resampled[1100];
    int16_t pcm[1100];
};
static LS_LAP_POOL lap_radio_info2_t rx24_radios[LAP_RADIOS_MAX];
static int rx24_serial; /* process lifetime: handles never reused after unload */
static int rx24_next(void) { return rx24_serial < INT_MAX ? ++rx24_serial : LAP_RX_ENOMEM; }
static bool rx24_output(void *out, size_t cap, size_t need) {
    return cap >= need && ls_lap_api_readable(out,need);
}
static rx24_t *rx24_session(int h) {
    if (check(h) != ST_RUNNING) return NULL;
    queue_t *b = atomic_load(&p4_q); return b ? b->v24 : NULL;
}
static rx24_t *rx24_handle(int id, bool decoder) {
    if (!rx_allowed() || atomic_load(&p4_state) != ST_RUNNING || id <= 0) return NULL;
    queue_t *b = atomic_load(&p4_q);
    rx24_t *x = b ? b->v24 : NULL;
    return x && (decoder ? x->decoder : x->channel) == id ? x : NULL;
}
static void rx24_refresh(const ls_lap_rx_backend_t *backend, int n) {
    for (int i=0;i<LAP_RADIOS_MAX;i++) {
        lap_radio_info2_t a = {.size=sizeof(a),.version=1};
        if (i<n && backend && backend->info2) backend->info2(i,&a);
        lk(); rx24_radios[i]=a; ulk();
    }
}
int lap_radio_info2(int index, lap_radio_info2_t *out, size_t size) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!rx_allowed()) return LAP_RX_EACCES;
    if (!rx24_output(out,size,sizeof(*out))) return LAP_RX_EINVAL;
    lk();
    if (index<0 || index>=atomic_load(&radio_count)) { ulk(); return LAP_RX_ENODEV; }
    *out=rx24_radios[index]; ulk(); return 0;
}
static bool rx24_range(const lap_radio_range_t *r, unsigned n, uint64_t value) {
    for (unsigned i=0;i<n;i++) if (value>=r[i].min_hz && value<=r[i].max_hz) return true;
    return false;
}
int lap_rx_control(int h, int ctl, int64_t value) {
    int st=check(h); if (st<0) return st;
    rx24_t *x=rx24_session(h); if (!x) return LAP_RX_EAGAIN;
    if (ctl<0 || ctl>=LAP_RX_CTLS) return LAP_RX_EINVAL;
    lk(); if(p4_mail.radio<0) { ulk(); return LAP_RX_EUNSUPPORTED; }
    lap_radio_info2_t info=rx24_radios[p4_mail.radio];
    if (!(info.controls&(1u<<ctl))) { ulk(); return LAP_RX_EUNSUPPORTED; }
    if (x->pending_command || (atomic_load(&p4_requests)&(R_TUNE|R_GAIN|R_RATE))) { ulk(); return LAP_RX_EBUSY; }
    if ((ctl==LAP_RX_CTL_TUNE && (value<0 || !rx24_range(info.frequencies,info.frequency_count,(uint64_t)value))) ||
        (ctl==LAP_RX_CTL_RATE && (value<=0 || value>UINT32_MAX || !rx24_range(info.rates,info.rate_count,(uint64_t)value))) ||
        (ctl!=LAP_RX_CTL_TUNE && ctl!=LAP_RX_CTL_RATE && (value<INT32_MIN || value>INT32_MAX))) { ulk(); return LAP_RX_ERANGE; }
    int ticket=rx24_next(); if (ticket<0) { ulk(); return ticket; }
    x->commands[ticket%8]=(lap_rx_command_t){sizeof(lap_rx_command_t),1,(uint32_t)ticket,LAP_RX_COMMAND_QUEUED,ctl,0,value,0};
    x->pending_command=ticket; ulk(); wake(); return ticket;
}
int lap_rx_command_poll(int ticket, lap_rx_command_t *out, size_t size) {
    if (!rx_allowed() || !rx24_output(out,size,sizeof(*out))) return LAP_RX_EINVAL;
    queue_t *b=atomic_load(&p4_q); rx24_t *x=b ? b->v24 : NULL;
    if (atomic_load(&p4_state)==ST_IDLE || atomic_load(&p4_state)==ST_CLOSING || !x || ticket<=0) return LAP_RX_EINVAL;
    lk(); lap_rx_command_t c=x->commands[ticket%8]; ulk();
    if (c.id!=(uint32_t)ticket) return LAP_RX_EINVAL;
    *out=c; return c.state==LAP_RX_COMMAND_QUEUED ? LAP_RX_EAGAIN : 0;
}
int lap_rx_control_get(int h, int ctl, int64_t *value) {
    int st=check(h); if (st<0) return st;
    if (!rx24_output(value,sizeof(*value),sizeof(*value)) || ctl<0 || ctl>=LAP_RX_CTLS) return LAP_RX_EINVAL;
    lk(); if(p4_mail.radio<0) { ulk(); return LAP_RX_EUNSUPPORTED; }
    lap_radio_info2_t info=rx24_radios[p4_mail.radio]; ls_lap_rx_actual_t a=p4_mail.actual; ulk();
    if (!(info.controls&(1u<<ctl))) return LAP_RX_EUNSUPPORTED;
    *value=ctl==LAP_RX_CTL_TUNE ? (int64_t)a.center_hz : ctl==LAP_RX_CTL_GAIN ? (int64_t)a.gain_tenths_db : ctl==LAP_RX_CTL_RATE ? (int64_t)a.rate_hz : (int64_t)info.values[ctl];
    return 0;
}
static rx24_t *rx24_create(void) {
    rx24_t *x=BIG_ALLOC(sizeof(*x));
    if (x) { atomic_init(&x->epoch,1); atomic_init(&x->gain,LAP_GAIN_AUTO); }
    return x;
}
static void rx24_decoder_drop(rx24_t *x) {
    for (int i=0;i<3;i++) { pocsag_destroy(x->pocsag[i]); x->pocsag[i]=NULL; }
    for (int i=0;i<4;i++) { flex_destroy(x->flex[i]); x->flex[i]=NULL; }
    aprs_destroy(x->aprs); x->aprs=NULL; ais_destroy(x->ais); x->ais=NULL;
    same_destroy(x->same); x->same=NULL; acars_destroy(x->acars); x->acars=NULL;
    x->decoder_run=0;
}
static void rx24_fail(rx24_t *x, int error) {
    if (!x) return;
    lk();
    int ticket=x->pending_command;
    if (ticket) {
        x->commands[ticket%8].state=LAP_RX_COMMAND_FAILED;
        x->commands[ticket%8].result=error;
        x->pending_command=0;
    }
    x->channel_state=x->decoder_state=error;
    ulk();
}
static void rx24_destroy(rx24_t *x) { if (x) { rx24_decoder_drop(x); BIG_FREE(x); } }
static void rx24_gain(rx24_t *x, int gain) { if (x) atomic_store(&x->gain,gain); }
static void rx24_mark(rx24_t *x, uint32_t flags) {
    if (!x) return;
    atomic_fetch_add(&x->epoch,1); x->pending_flags|=flags;
}
static void rx24_push(rx24_t *x, const int16_t *iq, size_t pairs, uint64_t first, uint64_t center, uint32_t rate) {
    if (!x || !atomic_load_explicit(&x->wanted,memory_order_acquire)) return;
    while (pairs) {
        unsigned n=pairs>SLOT_PAIRS ? SLOT_PAIRS : (unsigned)pairs;
        unsigned h=atomic_load_explicit(&x->head,memory_order_relaxed);
        if (h-atomic_load_explicit(&x->tail,memory_order_acquire)>=RX24_IN) {
            atomic_fetch_add(&x->input_drops,(unsigned)pairs); x->pending_flags|=LAP_IQ_GAP; return;
        }
        x->in_meta[h%RX24_IN]=(rx24_meta_t){first,center,(uint64_t)esp_timer_get_time(),rate,atomic_load(&x->epoch),x->pending_flags,atomic_load(&x->gain),n};
        memcpy(x->in[h%RX24_IN],iq,n*4u); x->pending_flags=0;
        atomic_store_explicit(&x->head,h+1,memory_order_release);
        iq+=n*2; pairs-=n; first+=n;
    }
}
int lap_rx_channel_open(int h, const lap_channel_config_t *cfg) {
    int st=check(h); if (st<0) return st;
    lap_channel_config_t c;
    if (!ls_lap_copy_input(&c,cfg,sizeof(c)) || c.size!=sizeof(c) || c.version!=1) return LAP_RX_EINVAL;
    rx24_t *x=rx24_session(h); if (!x) return LAP_RX_EAGAIN;
    lk(); uint32_t rate=p4_mail.actual.rate_hz;
    if (c.output_rate_hz<8000 || c.output_rate_hz>rate || rate%c.output_rate_hz || rate/c.output_rate_hz>64 ||
        !c.bandwidth_hz || c.bandwidth_hz>c.output_rate_hz/2 || llabs((long long)c.offset_hz)*2+c.bandwidth_hz>=rate) { ulk(); return LAP_RX_ERANGE; }
    if (x->channel || x->channel_change) { ulk(); return LAP_RX_EBUSY; }
    int id=rx24_next(); if (id<0) { ulk(); return id; }
    x->channel=id; x->channel_cfg=c; x->channel_change=true; x->channel_state=LAP_RX_OPENING;
    atomic_store(&x->wanted,true); ulk(); wake(); return id;
}
void lap_rx_channel_close(int id) {
    rx24_t *x=rx24_handle(id,false); if (!x) return;
    ls_lap_dsp_channel_close(id);
    lk(); x->channel=0; x->channel_change=true; ulk(); wake();
}
int lap_rx_dsp_start(int channel,const lap_dsp_config_t *config) {
    if (!LAP_WORKER_DSP) return LAP_RX_EUNSUPPORTED;
    if(!rx24_handle(channel,false)) return LAP_RX_EINVAL;
    return ls_lap_dsp_attach(channel,config);
}
int lap_rx_channel_status(int id, lap_channel_status_t *out, size_t size) {
    rx24_t *x=rx24_handle(id,false);
    if (!x || !rx24_output(out,size,sizeof(*out))) return LAP_RX_EINVAL;
    lk(); unsigned queued=0;
    for (unsigned t=x->out_tail;t<x->out_head;t++) queued+=x->out_meta[t%RX24_OUT].pairs;
    queued-=x->out_offset;
    *out=(lap_channel_status_t){sizeof(*out),1,x->channel_state,x->channel_cfg.output_rate_hz,queued,x->out_drops,atomic_load(&x->input_drops)};
    ulk(); return 0;
}
int lap_rx_channel_read(int id, int16_t *iq, int max_pairs, lap_iq_meta2_t *meta, size_t size) {
    rx24_t *x=rx24_handle(id,false);
    if (!x || max_pairs<=0 || max_pairs>16384 || !ls_lap_api_readable(iq,(size_t)max_pairs*4) || !rx24_output(meta,size,sizeof(*meta))) return LAP_RX_EINVAL;
    lk(); if (x->channel_state!=LAP_RX_RUNNING) { int st=x->channel_state; ulk(); return st<0 ? st : 0; }
    if (x->out_tail==x->out_head) { ulk(); return 0; }
    unsigned t=x->out_tail%RX24_OUT, off=x->out_offset;
    rx24_meta_t m=x->out_meta[t]; unsigned n=m.pairs-off;
    if (n>(unsigned)max_pairs) n=(unsigned)max_pairs;
    memcpy(iq,x->out[t]+off*2,n*4u);
    *meta=(lap_iq_meta2_t){sizeof(*meta),1,m.epoch,off ? 0 : m.flags,m.first+off,m.center,m.at,m.rate,x->out_drops};
    x->out_offset+=n;
    if (x->out_offset==m.pairs) { x->out_offset=0; x->out_tail++; }
    ulk(); return (int)n;
}
int lap_rx_spectrum_config(int h, const lap_spectrum_config_t *cfg) {
    int st=check(h); if (st<0) return st;
    lap_spectrum_config_t c;
    if (!ls_lap_copy_input(&c,cfg,sizeof(c)) || c.size!=sizeof(c) || c.version!=1) return LAP_RX_EINVAL;
    if (c.fft_size<64 || c.fft_size>RX24_FFT || (c.fft_size&(c.fft_size-1)) || c.window>LAP_SPEC_BLACKMAN || !c.averages || c.averages>64 || !c.lines_per_second || c.lines_per_second>50) return LAP_RX_ERANGE;
    rx24_t *x=rx24_session(h); if (!x) return LAP_RX_EAGAIN;
    lk();
    if ((uint64_t)c.fft_size*c.averages*c.lines_per_second>p4_mail.actual.rate_hz) { ulk(); return LAP_RX_ERANGE; }
    x->spec_cfg=c; x->spec_change=true; atomic_store(&x->wanted,true); ulk(); wake(); return 0;
}
int lap_rx_spectrum_read(int h, float *db, int max_bins, lap_spectrum_meta_t *meta, size_t size) {
    int st=check(h); if (st<0) return st;
    rx24_t *x=rx24_session(h); if (!x) return 0;
    if (max_bins<0 || max_bins>(int)RX24_FFT || !rx24_output(meta,size,sizeof(*meta)) || !ls_lap_api_readable(db,(size_t)max_bins*sizeof(float))) return LAP_RX_EINVAL;
    lk(); unsigned n=x->line_meta.fft_size;
    if (max_bins<(int)n) { ulk(); return LAP_RX_ERANGE; }
    if (!x->line_meta.sequence) { ulk(); return 0; }
    memcpy(db,x->line,n*sizeof(float)); *meta=x->line_meta; ulk(); return (int)n;
}
int lap_rx_decoder_catalog(lap_decoder_catalog_t *out, size_t size) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!rx_allowed()) return LAP_RX_EACCES;
    if (!rx24_output(out,size,sizeof(*out))) return LAP_RX_EINVAL;
    *out=(lap_decoder_catalog_t){.size=sizeof(*out),.version=1,.supported=RX24_DECODERS,.lr125_supported=RX24_LR};
    for (unsigned i=1;i<=LAP_DEC_WFM;i++) out->rate_hz[i]=i==LAP_DEC_ADSB ? 2000000 : i==LAP_DEC_P25 ? 240000 : 256000;
    return 0;
}
int lap_rx_decoder_attach(int h, const lap_decoder_config_t *cfg) {
    int st=check(h); if (st<0) return st;
    lap_decoder_config_t c;
    if (!ls_lap_copy_input(&c,cfg,sizeof(c)) || c.size!=sizeof(c) || c.version!=1 || c.flags) return LAP_RX_EINVAL;
    if (c.decoder>31 || !(RX24_DECODERS&(1u<<c.decoder))) return LAP_RX_EUNSUPPORTED;
    rx24_t *x=rx24_session(h); if (!x) return LAP_RX_EAGAIN;
    lk();
    if (p4_mail.actual.rate_hz!=(c.decoder==LAP_DEC_ADSB ? 2000000u : c.decoder==LAP_DEC_P25 ? 240000u : 256000u) && !(p4_mail.actual.rate_hz==125000 && (RX24_LR&(1u<<c.decoder)))) { ulk(); return LAP_RX_ERANGE; }
    if (x->decoder || x->decoder_change) { ulk(); return LAP_RX_EBUSY; }
    int id=rx24_next(); if (id<0) { ulk(); return id; }
    x->decoder=id; x->decoder_cfg=c; x->decoder_change=true; x->decoder_state=LAP_RX_OPENING;
    atomic_store(&x->wanted,true); ulk(); wake(); return id;
}
void lap_rx_decoder_detach(int id) {
    rx24_t *x=rx24_handle(id,true); if (!x) return;
    lk(); x->decoder=0; x->decoder_change=true; x->event_head=x->event_tail=0; ulk(); wake();
}
int lap_rx_decoder_status(int id, lap_decoder_status_t *out, size_t size) {
    rx24_t *x=rx24_handle(id,true);
    if (!x || !rx24_output(out,size,sizeof(*out))) return LAP_RX_EINVAL;
    lk(); *out=(lap_decoder_status_t){sizeof(*out),1,x->decoder_state,x->decoder_cfg.decoder,x->event_count,x->event_drops,atomic_load(&x->input_drops)}; ulk(); return 0;
}
int lap_rx_decoder_next(int id, lap_event_header_t *header, size_t size, void *payload, size_t cap) {
    rx24_t *x=rx24_handle(id,true);
    if (!x || !rx24_output(header,size,sizeof(*header))) return LAP_RX_EINVAL;
    lk(); if (x->event_tail==x->event_head) { ulk(); return 0; }
    rx24_event_t *e=&x->events[x->event_tail%RX24_EVENTS];
    if (cap<e->h.payload_size) { ulk(); return LAP_RX_ERANGE; }
    if (!ls_lap_api_readable(payload,e->h.payload_size)) { ulk(); return LAP_RX_EINVAL; }
    *header=e->h; header->dropped=x->event_drops;
    memcpy(payload,e->payload,e->h.payload_size); x->event_tail++; ulk(); return 1;
}
static void rx24_emit(rx24_t *x, const void *payload, unsigned size, unsigned flags) {
    if (size>RX24_PAYLOAD) return;
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
    flags=(flags&~LAP_EVENT_LIVE)|LAP_EVENT_SYNTHETIC;
#endif
    lk();
    if (!x->decoder || x->decoder_change) { ulk(); return; }
    x->event_count++;
    if (x->event_head-x->event_tail==RX24_EVENTS) { x->event_drops++; ulk(); return; }
    rx24_event_t *e=&x->events[x->event_head++%RX24_EVENTS];
    e->h=(lap_event_header_t){sizeof(e->h),1,x->decoder_run,x->event_count,x->current.epoch,flags,p4_worker_radio,x->current.center,x->current.at,size,x->event_drops};
    memcpy(e->payload,payload,size); ulk();
}
static void rx24_page(void *user, const fm_page_t *p) {
    rx24_t *x=user; lap_page_event_t e={.address=p->address,.baud=p->baud,.function=p->function,.protocol=(uint8_t)p->protocol};
    snprintf(e.text,sizeof(e.text),"%s",p->text); rx24_emit(x,&e,sizeof(e),LAP_EVENT_LIVE|LAP_EVENT_CRC_OK);
}
static void rx24_aprs(const aprs_packet_t *p, void *user) {
    lap_aprs_event_t e={.lat_e7=(int32_t)(p->lat*1e7),.lon_e7=(int32_t)(p->lon*1e7),.kind=p->kind,.fields=p->position ? LAP_EVENT_POSITION_VALID : 0};
    memcpy(e.call,p->call,sizeof(e.call)); memcpy(e.recipient,p->recipient,sizeof(e.recipient)); memcpy(e.text,p->text,sizeof(e.text));
    rx24_emit(user,&e,sizeof(e),LAP_EVENT_LIVE|LAP_EVENT_CRC_OK|e.fields);
}
static void rx24_ais(const ais_vessel_t *p, void *user) {
    lap_ais_event_t e={.mmsi=p->mmsi,.lat_e7=(int32_t)(p->lat*1e7),.lon_e7=(int32_t)(p->lon*1e7),.sog=p->sog,.cog=p->cog,.heading=p->heading,.message=p->message,.fields=p->fields,.channel=p->channel,.ship_type=p->ship_type};
    memcpy(e.name,p->name,sizeof(e.name)); memcpy(e.callsign,p->callsign,sizeof(e.callsign));
    rx24_emit(user,&e,sizeof(e),LAP_EVENT_LIVE|LAP_EVENT_CRC_OK|(p->position ? LAP_EVENT_POSITION_VALID : 0));
}
static void rx24_same(const same_alert_t *p, void *user) {
    lap_same_event_t e={.location_count=p->location_count,.valid_minutes=p->valid_minutes,.day=p->day,.hour=p->hour,.minute=p->minute,.ended=p->ended,.test=p->test};
    memcpy(e.originator,p->originator,sizeof(e.originator)); memcpy(e.event,p->event,sizeof(e.event)); memcpy(e.sender,p->sender,sizeof(e.sender)); memcpy(e.locations,p->locations,sizeof(e.locations));
    rx24_emit(user,&e,sizeof(e),LAP_EVENT_LIVE);
}
/* mode_s callback has no user pointer. Only the I/O worker enters it. */
static rx24_t *rx24_adsb_owner;
static void rx24_adsb(mode_s_t *s, struct mode_s_msg *p) {
    (void)s; if (!p->crcok || !rx24_adsb_owner) return;
    lap_adsb_event_t e={.address=((uint32_t)p->aa1<<16)|((uint32_t)p->aa2<<8)|(uint32_t)p->aa3,.altitude_ft=p->altitude_is_valid ? (p->unit==MODE_S_UNIT_METERS ? (int32_t)(p->altitude*3.280839895) : p->altitude) : INT32_MIN,.speed=p->velocity_is_valid ? p->velocity : INT32_MIN,.heading=p->heading_is_valid ? p->heading : INT32_MIN,.df=(uint8_t)p->msgtype,.crc_ok=1,.corrected=p->errorbit>=0,.non_icao=(uint8_t)p->addr_nonicao};
    memcpy(e.callsign,p->flight,sizeof(e.callsign)); memcpy(e.frame,p->msg,sizeof(e.frame)); rx24_emit(rx24_adsb_owner,&e,sizeof(e),LAP_EVENT_LIVE|LAP_EVENT_CRC_OK);
}
static bool rx24_decoder_make(rx24_t *x, unsigned kind) {
    rx24_decoder_drop(x); x->decoder_run=kind; fm_dsp_init(&x->fm); fm_lr_init(&x->lr_fm,25000); hf_demod_init(&x->hf);
    x->mag_carry=x->mag_start=0;
    if (kind>=LAP_DEC_AM && kind<=LAP_DEC_CW) {
        hf_demod_cfg_t cfg=x->hf.cfg;
        cfg.mode=kind==LAP_DEC_AM ? HF_DEMOD_AM : kind==LAP_DEC_USB ? HF_DEMOD_USB : kind==LAP_DEC_LSB ? HF_DEMOD_LSB : HF_DEMOD_CW;
        cfg.filter_hz=kind==LAP_DEC_AM ? 6000 : kind==LAP_DEC_CW ? 500 : 2400;
        hf_demod_set(&x->hf,&cfg);
    }
    if (kind==LAP_DEC_P25) {
        dsp_init(&x->p25_dsp); dsp_set_mode(&x->p25_dsp,DEMOD_FSK4_TRACKING);
        memset(&x->p25_state,0,sizeof(x->p25_state)); memset(x->p25_phase,0,sizeof(x->p25_phase));
        x->p25_sample=x->p25_last_event=0;
    }
    if (kind==LAP_DEC_FLEX) {
        for (int i=0;i<4;i++) { x->flex[i]=flex_create_sink(rx24_page,x,(ls_flex_mode_t)i); if (!x->flex[i]) return false; }
    }
    if (kind==LAP_DEC_POCSAG) {
        static const int baud[]={512,1200,2400};
        for (int i=0;i<3;i++) { x->pocsag[i]=pocsag_create_sink(rx24_page,x,baud[i]); if (!x->pocsag[i]) return false; pocsag_quiet(x->pocsag[i],true); }
    } else if (kind==LAP_DEC_APRS) return (x->aprs=aprs_create(rx24_aprs,x))!=NULL;
    else if (kind==LAP_DEC_AIS) return (x->ais=ais_create(rx24_ais,x))!=NULL;
    else if (kind==LAP_DEC_SAME) return (x->same=same_create(rx24_same,x))!=NULL;
    else if (kind==LAP_DEC_ACARS) { memset(&x->acars_state,0,sizeof(x->acars_state)); acars_rs_init(&x->acars_rs); return (x->acars=acars_create(&x->acars_state))!=NULL; }
    else if (kind==LAP_DEC_ADSB) mode_s_init(&x->modes);
    return true;
}

static void rx24_decoder_reset(rx24_t *x) {
    fm_dsp_init(&x->fm); fm_lr_reset(&x->lr_fm); hf_demod_reset(&x->hf);
    for (int i=0;i<3;i++) if (x->pocsag[i]) pocsag_seam(x->pocsag[i]);
    for (int i=0;i<4;i++) if (x->flex[i]) flex_reset(x->flex[i]);
    if (x->aprs) aprs_reset(x->aprs);
    if (x->ais) ais_reset(x->ais);
    if (x->same) same_reset(x->same);
    if (x->acars) { acars_reset(x->acars); acars_rs_init(&x->acars_rs); }
    if (x->decoder_run==LAP_DEC_ADSB) { mode_s_init(&x->modes); x->mag_carry=x->mag_start=0; }
    if (x->decoder_run==LAP_DEC_P25) {
        dsp_init(&x->p25_dsp); dsp_set_mode(&x->p25_dsp,DEMOD_FSK4_TRACKING);
        memset(&x->p25_state,0,sizeof(x->p25_state)); memset(x->p25_phase,0,sizeof(x->p25_phase));
        x->p25_sample=x->p25_last_event=0;
    }
}
static void rx24_p25_block(const uint8_t block[12], const dsd_state *state, unsigned grants, void *user) {
    rx24_t *x=user;
    for (unsigned i=0;i<(grants ? grants : 1);i++) {
        lap_p25_event_t e={.nac=(uint16_t)state->nac,.duid=7,.corrected_bits=x->p25_corrected,.opcode=block[0]&63,.mfid=block[1]};
        memcpy(e.block,block,sizeof(e.block));
        if (grants) {
            const p25_call_info_t *g=&state->p25_grants[state->p25_grant_count-grants+i];
            e.talkgroup=g->talkgroup; e.source=g->source; e.carrier_hz=g->carrier_hz;
            e.channel=g->channel; e.slot=g->slot; e.slots_per_carrier=g->slots_per_carrier; e.support=(uint8_t)g->support; e.system_valid=g->system_valid; e.wacn=g->wacn; e.sysid=g->sysid;
        }
        rx24_emit(x,&e,sizeof(e),LAP_EVENT_LIVE|LAP_EVENT_CRC_OK);
    }
}
/* Incremental framing over the firmware C4FM front end, BCH and TSBK decoder.
   Ten phases and both polarities are tried; only validated results publish.
   This adapter never enters DSD's blocking audio reader or global receiver. */
static void rx24_p25(rx24_t *x, const int16_t *pcm, unsigned n) {
    uint64_t sync=0;
    for (const char *p=P25P1_SYNC;*p;p++) sync=(sync<<2)|(unsigned)(*p-'0');
    for (unsigned i=0;i<n;i++,x->p25_sample++) {
        float v;
        for (unsigned polarity=0;polarity<2;polarity++) {
            v=polarity ? -(float)pcm[i] : (float)pcm[i];
            rx24_p25_phase_t *p=&x->p25_phase[x->p25_sample%10+polarity*10];
            unsigned dibit=v>=0 ? (v>p->threshold ? 1 : 0) : (v< -p->threshold ? 3 : 2);
            if (!p->fill) {
                p->sum+=fabsf(v)-p->history[p->history_at]; p->history[p->history_at]=fabsf(v); p->history_at=(p->history_at+1)%24;
                p->sync=((p->sync<<2)|(v>=0 ? 1u : 3u))&0xffffffffffffull;
                if (p->sync==sync) { p->threshold=(p->sum/24)*(2.0f/3); p->fill=1; p->sync=0; }
                continue;
            }
            p->dibits[p->fill-1]=(uint8_t)dibit; p->fill++;
            if (p->fill==34) {
                char bits[63],duid[3]={0}; unsigned at=0;
                for (unsigned j=0;j<33;j++) {
                    if (j==11) continue;
                    bits[at++]=(char)((p->dibits[j]>>1)&1);
                    if (at<63) bits[at++]=(char)(p->dibits[j]&1);
                }
                int nac=0, corrected=0;
                if (!check_NID_ec(bits,&nac,duid,p->dibits[32]&1,&corrected)) { p->fill=0; continue; }
                p->nac=(uint16_t)nac; p->duid=(uint8_t)(((duid[0]-'0')<<2)|(duid[1]-'0')); p->corrected=(uint8_t)corrected;
                if (p->duid!=7) {
                    if (!x->p25_last_event || x->p25_sample-x->p25_last_event>20) {
                        lap_p25_event_t e={.nac=p->nac,.duid=p->duid,.corrected_bits=p->corrected,.opcode=255};
                        rx24_emit(x,&e,sizeof(e),LAP_EVENT_LIVE); x->p25_last_event=x->p25_sample;
                    }
                    p->fill=0;
                }
            }
            if (p->fill==34+P25_TSDU_DIBIT_COUNT) {
                x->p25_state.nac=p->nac; x->p25_corrected=p->corrected;
                if (!x->p25_last_event || x->p25_sample-x->p25_last_event>20) {
                    if (p25_tsbk_process_tsdu_sink(&x->p25_state,p->dibits+33,P25_TSDU_DIBIT_COUNT,rx24_p25_block,x))
                        x->p25_last_event=x->p25_sample;
                }
                p->fill=0;
            }
        }
    }
}
static void rx24_decode(rx24_t *x, const int16_t *iq, unsigned n) {
    unsigned d=x->decoder_run;
    if (!d) return;
    if (x->current.rate!=(d==LAP_DEC_ADSB ? 2000000u : d==LAP_DEC_P25 ? 240000u : 256000u) && !(x->current.rate==125000 && (RX24_LR&(1u<<d)))) {
        lk(); x->decoder_state=LAP_RX_ERANGE; ulk(); return;
    }
    for (unsigned i=0;i<n*2;i++) x->u8[i]=(uint8_t)(((int)iq[i]+32768)>>8);
    if (d==LAP_DEC_P25) {
        int count=dsp_process_iq(&x->p25_dsp,x->u8,(int)n*2,x->pcm,1100);
        rx24_p25(x,x->pcm,(unsigned)count);
    } else if (d==LAP_DEC_WFM) {
        int count=fm_demod_wide(&x->fm,x->u8,(int)n*2,x->pcm,1100); ls_lap_audio_tap(x->pcm,count);
    } else if (d==LAP_DEC_ADSB) {
        unsigned total=x->mag_carry+n;
        mode_s_compute_magnitude_vector(x->u8,x->mag+x->mag_carry,n*2);
        rx24_adsb_owner=x;
        unsigned next=mode_s_detect_from(&x->modes,x->mag,total,x->mag_start,rx24_adsb);
        rx24_adsb_owner=NULL;
        unsigned keep=total<241 ? total : 241;
        unsigned discarded=total-keep;
        memmove(x->mag,x->mag+discarded,keep*sizeof(uint16_t));
        x->mag_start=next>discarded ? next-discarded : 0; x->mag_carry=keep;
    } else if (d==LAP_DEC_AIS) ais_process(x->ais,x->u8,n*2);
    else if (d==LAP_DEC_AM && x->current.rate==125000) {
        int count=fm_lr_demod_am(&x->lr_fm,&x->fm,(const uint8_t *)iq,n*4,x->pcm,1100);
        if (count) ls_lap_audio_tap(x->pcm,count);
    }
    else if (d>=LAP_DEC_AM && d<=LAP_DEC_CW) {
        int count=hf_demod_u8(&x->hf,x->u8,n*2,x->pcm,1100); if (count) ls_lap_audio_tap(x->pcm,count);
    } else if (d==LAP_DEC_ACARS) {
        int count=x->current.rate==125000 ? fm_lr_demod_env(&x->lr_fm,&x->fm,(const uint8_t *)iq,n*4,x->disc,1100) : fm_demod_env(&x->fm,x->u8,n*2,x->disc,1100);
        count=acars_rs_5to3(&x->acars_rs,x->disc,count,x->resampled,1100);
        unsigned before=x->acars_state.n_delivered; acars_process(x->acars,x->resampled,count);
        unsigned added=x->acars_state.n_delivered-before;
        if (added>ACARS_MSG_LOG_MAX) added=ACARS_MSG_LOG_MAX;
        for (unsigned k=added;k>0;k--) {
            unsigned i=(x->acars_state.msg_head+ACARS_MSG_LOG_MAX-k)%ACARS_MSG_LOG_MAX;
            acars_msg_out_t *p=&x->acars_state.msgs[i];
            lap_acars_event_t e={.bcs=p->bcs,.mode=(uint8_t)p->mode,.block_id=(uint8_t)p->block_id};
            memcpy(e.registration,p->reg,sizeof(e.registration)); memcpy(e.label,p->label,sizeof(e.label)); memcpy(e.text,p->text,sizeof(e.text));
            rx24_emit(x,&e,sizeof(e),LAP_EVENT_LIVE|LAP_EVENT_CRC_OK);
        }
    } else {
        int count=x->current.rate==125000 ? fm_lr_demod(&x->lr_fm,&x->fm,(const uint8_t *)iq,n*4,x->disc,1100) : fm_demod_iq(&x->fm,x->u8,n*2,x->disc,1100);
        if (d==LAP_DEC_FLEX) { for (int i=0;i<4;i++) flex_process(x->flex[i],x->disc,count); }
        else if (d==LAP_DEC_POCSAG) { for (int i=0;i<3;i++) pocsag_process(x->pocsag[i],x->disc,count); }
        else {
            count=fm_demod_to_audio(&x->fm,x->disc,count,x->pcm,1100);
            if (d==LAP_DEC_FM) ls_lap_audio_tap(x->pcm,count);
            else if (d==LAP_DEC_APRS) aprs_process(x->aprs,x->pcm,count);
            else if (d==LAP_DEC_SAME) same_process(x->same,x->pcm,count);
        }
    }
}
static void rx24_fir_reset(rx24_t *x, uint32_t rate) {
    memset(x->fi,0,sizeof(x->fi)); memset(x->fq,0,sizeof(x->fq));
    x->dec_phase=x->fir_at=0; x->osc_i=1; x->osc_q=0;
    float w=-2*RX24_PI*x->channel_run.offset_hz/rate;
    x->step_i=cosf(w); x->step_q=sinf(w);
    x->decimation=rate/x->channel_run.output_rate_hz;
    float cutoff=0.5f*x->channel_run.bandwidth_hz/rate, sum=0;
    for (unsigned i=0;i<RX24_TAPS;i++) {
        int m=(int)i-(int)(RX24_TAPS/2);
        float v=m ? sinf(2*RX24_PI*cutoff*m)/(RX24_PI*m) : 2*cutoff;
        x->taps[i]=v*(0.5f-0.5f*cosf(2*RX24_PI*i/(RX24_TAPS-1))); sum+=x->taps[i];
    }
    for (unsigned i=0;i<RX24_TAPS;i++) x->taps[i]/=sum;
}
static void rx24_out_publish(rx24_t *x) {
    if (!x->out_fill) return;
    lk(); x->out_meta[x->out_head%RX24_OUT].pairs=x->out_fill; x->out_head++; x->out_fill=0; ulk();
}
static void rx24_channel(rx24_t *x, const int16_t *iq, unsigned n) {
    if (!x->channel_run.output_rate_hz) return;
    uint32_t rate=x->current.rate;
    if (rate%x->channel_run.output_rate_hz || rate/x->channel_run.output_rate_hz>64 || rate<x->channel_run.output_rate_hz || llabs((long long)x->channel_run.offset_hz)*2+x->channel_run.bandwidth_hz>=rate) {
        lk(); x->channel_state=LAP_RX_ERANGE; ulk(); return;
    }
    for (unsigned i=0;i<n;i++) {
        float a=iq[i*2]*x->osc_i-iq[i*2+1]*x->osc_q, b=iq[i*2]*x->osc_q+iq[i*2+1]*x->osc_i;
        float oi=x->osc_i*x->step_i-x->osc_q*x->step_q;
        x->osc_q=x->osc_i*x->step_q+x->osc_q*x->step_i; x->osc_i=oi;
        x->fi[x->fir_at]=a; x->fq[x->fir_at]=b; x->fir_at=(x->fir_at+1)%RX24_TAPS;
        if (++x->dec_phase<x->decimation) continue;
        x->dec_phase=0;
        float vi=0,vq=0;
        for (unsigned k=0;k<RX24_TAPS;k++) { unsigned at=(x->fir_at+RX24_TAPS-1-k)%RX24_TAPS; vi+=x->taps[k]*x->fi[at]; vq+=x->taps[k]*x->fq[at]; }
        if(!x->dsp_fill) x->dsp_meta=(lap_iq_meta2_t){sizeof(x->dsp_meta),1,x->current.epoch,x->current.flags,x->output_sample,
            (uint64_t)((int64_t)x->current.center+x->channel_run.offset_hz),x->current.at,x->channel_run.output_rate_hz,atomic_load(&x->input_drops)};
        x->dsp_iq[x->dsp_fill*2]=(int16_t)fmaxf(-32768,fminf(32767,vi));
        x->dsp_iq[x->dsp_fill*2+1]=(int16_t)fmaxf(-32768,fminf(32767,vq));
        if(++x->dsp_fill==2048) { ls_lap_dsp_submit(x->channel_run_id,x->dsp_iq,2048,&x->dsp_meta); x->dsp_fill=0; }
        lk(); bool full=x->out_head-x->out_tail==RX24_OUT; if (full) { x->out_drops++; x->out_pending_gap=true; } ulk();
        if (full) { x->output_sample++; continue; }
        unsigned h=x->out_head%RX24_OUT;
        if (!x->out_fill) {
            x->out_meta[h]=x->current; x->out_meta[h].first=x->output_sample;
            x->out_meta[h].center=(uint64_t)((int64_t)x->current.center+x->channel_run.offset_hz);
            x->out_meta[h].rate=x->channel_run.output_rate_hz;
            if (x->out_pending_gap) { x->out_meta[h].flags|=LAP_IQ_GAP; x->out_pending_gap=false; }
        }
        x->out[h][x->out_fill*2]=(int16_t)fmaxf(-32768,fminf(32767,vi));
        x->out[h][x->out_fill*2+1]=(int16_t)fmaxf(-32768,fminf(32767,vq));
        x->out_fill++; x->output_sample++;
        if (x->out_fill==SLOT_PAIRS) rx24_out_publish(x);
    }
    float norm=1.0f/sqrtf(x->osc_i*x->osc_i+x->osc_q*x->osc_q);
    x->osc_i*=norm; x->osc_q*=norm;
}
static void rx24_fft(rx24_t *x) {
    unsigned n=x->spec_run.fft_size;
    for (unsigned i=1,j=0;i<n;i++) { unsigned bit=n>>1; for (;j&bit;bit>>=1) j^=bit; j^=bit; if (i<j) { float t=x->re[i]; x->re[i]=x->re[j]; x->re[j]=t; t=x->im[i]; x->im[i]=x->im[j]; x->im[j]=t; } }
    for (unsigned len=2;len<=n;len<<=1) {
        float angle=-2*RX24_PI/len, wr=cosf(angle),wi=sinf(angle);
        for (unsigned i=0;i<n;i+=len) { float cr=1,ci=0;
            for (unsigned k=0;k<len/2;k++) { unsigned a=i+k,b=a+len/2;
                float br=x->re[b]*cr-x->im[b]*ci,bi=x->re[b]*ci+x->im[b]*cr;
                x->re[b]=x->re[a]-br; x->im[b]=x->im[a]-bi; x->re[a]+=br; x->im[a]+=bi;
                float t=cr*wr-ci*wi; ci=cr*wi+ci*wr; cr=t;
            }
        }
    }
    for (unsigned i=0;i<n;i++) x->acc[i]+=x->re[i]*x->re[i]+x->im[i]*x->im[i];
    if (++x->spec_frames<x->spec_run.averages) return;
    float gain=0; for (unsigned i=0;i<n;i++) gain+=x->win[i];
    float scale=1.0f/(gain*gain*x->spec_frames);
    lk(); unsigned sequence=x->line_meta.sequence+1;
    for (unsigned i=0;i<n;i++) x->line[i]=10*log10f(fmaxf(x->acc[(i+n/2)%n]*scale,1e-20f));
    x->line_meta=(lap_spectrum_meta_t){sizeof(lap_spectrum_meta_t),1,sequence,x->current.epoch,x->current.center,x->current.at,x->current.rate,(float)x->current.rate/n,x->current.gain,(uint16_t)n,x->spec_run.window,atomic_load(&x->input_drops)};
    ulk(); memset(x->acc,0,sizeof(x->acc)); x->spec_frames=0;
}
static void rx24_spectrum(rx24_t *x, const int16_t *iq, unsigned n) {
    unsigned fft=x->spec_run.fft_size; if (!fft) return;
    for (unsigned i=0;i<n;i++) {
        if (x->spec_skip) { x->spec_skip--; continue; }
        x->re[x->spec_fill]=iq[i*2]*(1.0f/32768)*x->win[x->spec_fill]; x->im[x->spec_fill]=iq[i*2+1]*(1.0f/32768)*x->win[x->spec_fill];
        if (++x->spec_fill==fft) {
            rx24_fft(x); x->spec_fill=0;
            unsigned every=x->current.rate/(x->spec_run.lines_per_second*x->spec_run.averages);
            x->spec_skip=every>fft ? every-fft : 0;
        }
    }
}
static void rx24_work(rx24_t *x, ls_lap_rx_actual_t *actual) {
    if (!x) return;
    lk(); int ticket=x->pending_command; lap_rx_command_t c=ticket ? x->commands[ticket%8] : (lap_rx_command_t){0}; ulk();
    if (ticket) {
        int rc=be()->control ? be()->control(c.control,c.requested,actual) : LAP_RX_EUNSUPPORTED;
        if (rc!=LAP_RX_EAGAIN) {
            lk(); lap_rx_command_t *result=&x->commands[ticket%8];
            result->state=rc<0 ? LAP_RX_COMMAND_FAILED : LAP_RX_COMMAND_APPLIED; result->result=rc;
            result->applied=c.control==LAP_RX_CTL_TUNE ? (int64_t)actual->center_hz : c.control==LAP_RX_CTL_GAIN ? (int64_t)actual->gain_tenths_db : c.control==LAP_RX_CTL_RATE ? (int64_t)actual->rate_hz : c.requested;
            if (!rc && c.control<LAP_RX_CTL_ANTENNA) rx24_radios[p4_mail.radio].values[c.control]=(int32_t)c.requested;
            x->pending_command=0; p4_mail.actual=*actual; ulk();
        }
    }
    lk(); bool ch=x->channel_change, dec=x->decoder_change, spec=x->spec_change;
    lap_channel_config_t cc=x->channel_cfg; lap_decoder_config_t dc=x->decoder_cfg; lap_spectrum_config_t sc=x->spec_cfg;
    int channel=x->channel, decoder=x->decoder;
    x->channel_change=x->decoder_change=x->spec_change=false; ulk();
    if (ch) {
        x->channel_run_id=channel; x->dsp_fill=0;
        x->channel_run=channel ? cc : (lap_channel_config_t){0};
        lk(); x->out_head=x->out_tail=x->out_offset=x->out_fill=x->out_drops=0; x->output_sample=0; x->channel_state=LAP_RX_RUNNING; ulk();
        if (channel) rx24_fir_reset(x,actual->rate_hz);
    }
    if (dec) {
        lk(); x->event_head=x->event_tail=x->event_count=x->event_drops=0; ulk();
        bool ok=true;
        if (decoder) ok=rx24_decoder_make(x,dc.decoder); else rx24_decoder_drop(x);
        if (!ok) rx24_decoder_drop(x);
        lk(); x->decoder_state=ok ? LAP_RX_RUNNING : LAP_RX_ENOMEM; ulk();
    }
    if (spec) {
        x->spec_run=sc; x->spec_fill=x->spec_frames=x->spec_skip=0;
        memset(x->acc,0,sizeof(x->acc));
        lk(); x->line_meta.sequence=0; ulk();
        for (unsigned i=0;i<sc.fft_size;i++) {
            float a=2*RX24_PI*i/(sc.fft_size-1);
            x->win[i]=sc.window==LAP_SPEC_RECT ? 1 : sc.window==LAP_SPEC_HANN ? 0.5f-0.5f*cosf(a) : 0.42f-0.5f*cosf(a)+0.08f*cosf(2*a);
        }
    }
    atomic_store(&x->wanted,channel || decoder || x->spec_run.fft_size);
    /* At most 32K pairs per I/O pass. Never execute unbounded app work. */
    for (unsigned budget=0;budget<32;budget++) {
        unsigned t=atomic_load_explicit(&x->tail,memory_order_relaxed);
        if (t==atomic_load_explicit(&x->head,memory_order_acquire)) break;
        x->current=x->in_meta[t%RX24_IN]; uint32_t dropped=atomic_load(&x->input_drops);
        if (x->seen_epoch!=x->current.epoch || x->seen_rate!=x->current.rate || (x->current.flags&LAP_IQ_GAP)) {
            if(x->dsp_fill) { ls_lap_dsp_gap(x->channel_run_id); x->dsp_fill=0; }
            x->current.flags|=LAP_IQ_GAP;
            if (x->channel_run.output_rate_hz) {
                rx24_out_publish(x); rx24_fir_reset(x,x->current.rate);
                if (x->decimation) x->output_sample=x->current.first/x->decimation;
            }
            if (x->decoder_run) rx24_decoder_reset(x);
            x->spec_fill=x->spec_frames=x->spec_skip=0; memset(x->acc,0,sizeof(x->acc));
            x->seen_epoch=x->current.epoch; x->seen_rate=x->current.rate; x->seen_drops=dropped;
        }
        const int16_t *iq=x->in[t%RX24_IN];
        rx24_channel(x,iq,x->current.pairs); rx24_spectrum(x,iq,x->current.pairs); rx24_decode(x,iq,x->current.pairs);
        atomic_store_explicit(&x->tail,t+1,memory_order_release);
    }
}

#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
bool ls_lap_rx24_sim_step(const char *command) {
    extern int ls_lap_rx_sim_delay, ls_lap_rx_sim_result;
    int value;
    if (sscanf(command,"rxdelay=%d",&value)==1) { ls_lap_rx_sim_delay=value<0 ? 0 : value>1000 ? 1000 : value; return true; }
    if (sscanf(command,"rxresult=%d",&value)==1) { ls_lap_rx_sim_result=value<0 ? value : 0; return true; }
    if (strncmp(command,"decode=",7)) return false;
    queue_t *b=atomic_load(&p4_q); rx24_t *x=b ? b->v24 : NULL;
    if (!x || !x->decoder_run || !x->decoder || atomic_load(&p4_state)!=ST_RUNNING) return true;
    /* Fixture fields are values, never scraped from the legacy text timeline. */
    const char *args=strchr(command+7,','); if (!args) return true; args++;
    if ((!strncmp(command+7,"page,",5) && x->decoder_run==LAP_DEC_POCSAG) ||
        (!strncmp(command+7,"flex,",5) && x->decoder_run==LAP_DEC_FLEX)) {
        fm_page_t e={.baud=1200,.function=1,.protocol=x->decoder_run==LAP_DEC_FLEX ? FM_PAGE_PROTOCOL_FLEX : FM_PAGE_PROTOCOL_POCSAG};
        unsigned address;
        if (sscanf(args,"%u,%79[^;]",&address,e.text)==2) { e.address=address; rx24_page(x,&e); }
    } else if (!strncmp(command+7,"aprs,",5) && x->decoder_run==LAP_DEC_APRS) {
        aprs_packet_t e={.position=true,.kind=APRS_POSITION};
        if (sscanf(args,"%11[^,],%lf,%lf,%159[^;]",e.call,&e.lat,&e.lon,e.text)==4 && e.lat>=-90 && e.lat<=90 && e.lon>=-180 && e.lon<=180) rx24_aprs(&e,x);
    } else if (!strncmp(command+7,"ais,",4) && x->decoder_run==LAP_DEC_AIS) {
        ais_vessel_t e={.position=true,.fields=AIS_POSITION,.message=1,.sog=1023,.cog=3600,.heading=511};
        unsigned mmsi;
        if (sscanf(args,"%u,%lf,%lf,%20[^;]",&mmsi,&e.lat,&e.lon,e.name)==4 && e.lat>=-90 && e.lat<=90 && e.lon>=-180 && e.lon<=180) { e.mmsi=mmsi; rx24_ais(&e,x); }
    } else if (!strncmp(command+7,"acars,",6) && x->decoder_run==LAP_DEC_ACARS) {
        lap_acars_event_t e={0};
        if (sscanf(args,"%7[^,],%2[^,],%220[^;]",e.registration,e.label,e.text)==3) rx24_emit(x,&e,sizeof(e),LAP_EVENT_CRC_OK);
    } else if (!strncmp(command+7,"adsb,",5) && x->decoder_run==LAP_DEC_ADSB) {
        lap_adsb_event_t e={.altitude_ft=INT32_MIN,.speed=INT32_MIN,.heading=INT32_MIN}; unsigned address;
        if (sscanf(args,"%x,%d,%8[^;]",&address,&e.altitude_ft,e.callsign)==3) { e.address=address; e.crc_ok=1; e.df=17; rx24_emit(x,&e,sizeof(e),LAP_EVENT_CRC_OK); }
    } else if (!strncmp(command+7,"p25,",4) && x->decoder_run==LAP_DEC_P25) {
        lap_p25_event_t e={.duid=7}; unsigned nac,tg,source;
        if (sscanf(args,"%x,%u,%u",&nac,&tg,&source)==3) { e.nac=nac; e.talkgroup=tg; e.source=source; rx24_emit(x,&e,sizeof(e),LAP_EVENT_CRC_OK); }
    } else if (!strncmp(command+7,"same,",5) && x->decoder_run==LAP_DEC_SAME) {
        same_alert_t e={0}; if (same_parse(args,&e)) rx24_same(&e,x);
    }
    return true;
}
#endif
