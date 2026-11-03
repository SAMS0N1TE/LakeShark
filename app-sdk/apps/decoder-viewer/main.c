/* Explicit managed firmware decoder + decimated stream. No internal headers.
 * LAPSIM_SCRIPT='12:decode=page,12345,HELLO;20:key=n' drives typed fixtures.
 * N / left/right chooses a decoder; R retries after USB hotplug. */
#include "lakeshark_app.h"
#include <stdio.h>
#include <string.h>

static const struct { const char *name; unsigned kind,rate; uint64_t center; } modes[]={
    {"POCSAG",LAP_DEC_POCSAG,256000,152600000}, {"APRS",LAP_DEC_APRS,256000,144390000},
    {"ACARS",LAP_DEC_ACARS,256000,131550000}, {"AIS",LAP_DEC_AIS,256000,162000000},
    {"ADS-B",LAP_DEC_ADSB,2000000,1090000000}, {"SAME",LAP_DEC_SAME,256000,162550000},
    {"P25 C4FM CONTROL",LAP_DEC_P25,240000,851000000}, {"FLEX",LAP_DEC_FLEX,256000,929000000},
    {"FM AUDIO",LAP_DEC_FM,256000,154785000}, {"AM AUDIO",LAP_DEC_AM,256000,10000000},
    {"USB AUDIO",LAP_DEC_USB,256000,14074000}, {"LSB AUDIO",LAP_DEC_LSB,256000,7100000},
    {"CW AUDIO",LAP_DEC_CW,256000,14060000}, {"WFM AUDIO",LAP_DEC_WFM,256000,91000000}
};
static struct {
    int mode,h,channel,decoder,radio;
    unsigned rate;
    bool attempted;
    char why[96], rows[10][100]; unsigned row;
    lap_caps_t caps;
    lap_decoder_catalog_t catalog;
} s;
static void stop(void) {
    if (s.decoder>0) lap_rx_decoder_detach(s.decoder);
    if (s.channel>0) lap_rx_channel_close(s.channel);
    if (s.h>0) lap_rx_close(s.h);
    s.decoder=s.channel=s.h=0; s.attempted=false;
}
static bool contains(const lap_radio_range_t *ranges,unsigned n,uint64_t value) {
    for (unsigned i=0;i<n;i++) if (value>=ranges[i].min_hz && value<=ranges[i].max_hz) return true;
    return false;
}
static void open_radio(void) {
    lap_caps(&s.caps,sizeof(s.caps));
    if (!s.caps.radio_present) { snprintf(s.why,sizeof(s.why),"No radio: attach an SDR and press R."); return; }
    if (!(s.catalog.supported&(1u<<modes[s.mode].kind))) { snprintf(s.why,sizeof(s.why),"This firmware has no session adapter for this decoder."); return; }
    for (unsigned i=0;i<s.caps.radio_count;i++) {
        lap_radio_info2_t info;
        if (lap_radio_info2((int)i,&info,sizeof(info)) || !info.present || info.busy ||
            !contains(info.frequencies,info.frequency_count,modes[s.mode].center)) continue;
        unsigned rate=modes[s.mode].rate;
        if (!contains(info.rates,info.rate_count,rate)) {
            if ((s.catalog.lr125_supported&(1u<<modes[s.mode].kind)) && contains(info.rates,info.rate_count,125000)) rate=125000;
            else continue;
        }
        s.rate=rate;
        lap_rx_config_t cfg={modes[s.mode].center,rate,LAP_GAIN_AUTO,LAP_RX_SPECTRUM_ONLY};
        int h=lap_rx_open((int)i,&cfg);
        if (h>0) { s.h=h; s.radio=(int)i; snprintf(s.why,sizeof(s.why),"Opening %s",info.id); return; }
    }
    snprintf(s.why,sizeof(s.why),"No available radio supports this frequency and sample rate.");
}
static void event_row(const lap_event_header_t *h,const void *payload) {
    char *text=s.rows[s.row++%10];
    const char *origin=(h->flags&LAP_EVENT_SYNTHETIC) ? "SIM" : "LIVE";
    switch (h->decoder) {
    case LAP_DEC_POCSAG: case LAP_DEC_FLEX: {
        const lap_page_event_t *e=payload;
        snprintf(text,100,"%s RIC %lu func %u baud %u %.48s",origin,(unsigned long)e->address,e->function,e->baud,e->text); break;
    }
    case LAP_DEC_APRS: { const lap_aprs_event_t *e=payload; snprintf(text,100,"%s %.11s kind %lu latE7 %ld %.35s",origin,e->call,(unsigned long)e->kind,(long)e->lat_e7,e->text); break; }
    case LAP_DEC_ACARS: { const lap_acars_event_t *e=payload; snprintf(text,100,"%s %.7s label %.2s %.50s",origin,e->registration,e->label,e->text); break; }
    case LAP_DEC_AIS: { const lap_ais_event_t *e=payload; snprintf(text,100,"%s MMSI %lu type %u %.20s latE7 %ld",origin,(unsigned long)e->mmsi,e->message,e->name,(long)e->lat_e7); break; }
    case LAP_DEC_ADSB: { const lap_adsb_event_t *e=payload; snprintf(text,100,"%s ADDR %06lx DF %u %.8s alt %ld ft",origin,(unsigned long)e->address,e->df,e->callsign,(long)e->altitude_ft); break; }
    case LAP_DEC_P25: { const lap_p25_event_t *e=payload; snprintf(text,100,"%s NAC %03x DUID %u TG %lu SRC %lu",origin,e->nac,e->duid,(unsigned long)e->talkgroup,(unsigned long)e->source); break; }
    case LAP_DEC_SAME: { const lap_same_event_t *e=payload; snprintf(text,100,"%s %.3s %.3s %.8s %u areas %u minutes",origin,e->originator,e->event,e->sender,e->location_count,e->valid_minutes); break; }
    default: snprintf(text,100,"%s decoder %lu payload %lu",origin,(unsigned long)h->decoder,(unsigned long)h->payload_size); break;
    }
}
static void draw(void) {
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,1,"DECODER VIEWER  [N] next  [R] retry",LAP_ATTR(LAP_CYAN,LAP_BLACK));
    lap_put_str(1,3,modes[s.mode].name,LAP_ATTR(LAP_YELLOW,LAP_BLACK));
    if (!s.h && !s.attempted) { open_radio(); s.attempted=true; }
    if (s.h>0) {
        lap_rx_status_t st;
        if (!lap_rx_status(s.h,&st) && st.state==LAP_RX_RUNNING && !s.decoder) {
            lap_decoder_config_t cfg={sizeof(cfg),1,modes[s.mode].kind,0};
            s.decoder=lap_rx_decoder_attach(s.h,&cfg);
            lap_channel_config_t channel={sizeof(channel),1,0,s.rate==2000000 ? 62500 : s.rate==125000 ? 25000 : 16000,8000};
            s.channel=lap_rx_channel_open(s.h,&channel);
            lap_audio_listen(true);
        } else if (st.state<0) snprintf(s.why,sizeof(s.why),"Radio disconnected/error %d. Press R.",st.state);
    }
    if (s.decoder>0) {
        lap_decoder_status_t st;
        if (!lap_rx_decoder_status(s.decoder,&st,sizeof(st)))
            snprintf(s.why,sizeof(s.why),"state %d events %lu lost %lu input loss %lu",st.state,(unsigned long)st.events,(unsigned long)st.dropped,(unsigned long)st.input_dropped_pairs);
        union { lap_page_event_t page; lap_aprs_event_t aprs; lap_acars_event_t acars; lap_ais_event_t ais; lap_adsb_event_t adsb; lap_p25_event_t p25; lap_same_event_t same; } payload;
        for (int i=0;i<4;i++) { lap_event_header_t h; if (lap_rx_decoder_next(s.decoder,&h,sizeof(h),&payload,sizeof(payload))!=1) break; event_row(&h,&payload); }
        int16_t pcm[160]; lap_audio_read(pcm,160); /* optional monitor tap, no speaker */
    }
    lap_put_str(1,5,s.why,LAP_ATTR(LAP_WHITE,LAP_BLACK));
    if (s.channel>0) {
        lap_channel_status_t st;
        if (!lap_rx_channel_status(s.channel,&st,sizeof(st))) {
            char line[100]; snprintf(line,sizeof(line),"Channel %lu S/s queued %lu dropped %lu",(unsigned long)st.output_rate_hz,(unsigned long)st.queued_pairs,(unsigned long)st.dropped_pairs);
            lap_put_str(1,6,line,LAP_ATTR(LAP_GREEN,LAP_BLACK));
        }
        for (int i=0;i<4;i++) { int16_t iq[1024]; lap_iq_meta2_t meta; if (lap_rx_channel_read(s.channel,iq,512,&meta,sizeof(meta))<=0) break; }
    }
    for (unsigned i=0;i<10 && i<s.row;i++) lap_put_str(1,8+(int)i,s.rows[(s.row-1-i)%10],LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,19,"P25: Phase 1 C4FM control; fixed frequency",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
}
static bool key(lap_key_t k,char ch) {
    if (k==LAP_KEY_LEFT || k==LAP_KEY_RIGHT || (k==LAP_KEY_CHAR && ch=='n')) {
        stop(); s.mode=(s.mode+(k==LAP_KEY_LEFT ? (int)(sizeof(modes)/sizeof(modes[0]))-1 : 1))%(int)(sizeof(modes)/sizeof(modes[0])); return true;
    }
    if (k==LAP_KEY_CHAR && ch=='r') { stop(); return true; }
    return false;
}
static void enter(void) { memset(&s,0,sizeof(s)); lap_caps(&s.caps,sizeof(s.caps)); lap_rx_decoder_catalog(&s.catalog,sizeof(s.catalog)); }
static void leave(void) { stop(); lap_audio_listen(false); }
LAP_APP(.id="decoder-viewer",.name="DECODER VIEWER",.sub="typed firmware decoder events",.version="1.0",.author="LakeShark",
        .icon=LAP_ICON_WAVE,.color=LAP_CYAN,.flags=LAP_CAP_RADIO_RX,.enter=enter,.leave=leave,.draw=draw,.key=key);
