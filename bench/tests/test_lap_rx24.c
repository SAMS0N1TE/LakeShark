/* Radio RX API 2.4 contracts, against real runtime/DSP and scriptable SIM. */
#include "ls_lap_api.h"
#include "ls_lap_radio.h"
#include "ls_lap_sys.h"
#include "ls_value.h"
#include "pocsag_gen.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern bool ls_lap_radio_sim_absent;
static void pump(unsigned n) {
    ls_lap_api_callback(false); while (n--) ls_lap_radio_service(); ls_lap_api_callback(true);
}
static void begin(unsigned caps) {
    static lap_app_t app;
    memset(&app,0,sizeof(app)); strcpy(app.id,"rx24-test"); app.api_major=2; app.api_minor=4; app.flags=caps;
    ls_lap_api_begin(app.id,1,0); ls_lap_sys_begin(); ls_lap_radio_begin(&app); ls_lap_api_callback(true); pump(1);
}
static void end(void) { ls_lap_sys_end(); pump(2); ls_lap_api_callback(false); ls_lap_api_cleanup(); }
static int open_sim(uint64_t center,unsigned rate) {
    lap_rx_config_t cfg={center,rate,LAP_GAIN_AUTO,LAP_RX_SPECTRUM_ONLY};
    int h=lap_rx_open(0,&cfg); assert(h>0); pump(1); assert(lap_rx_state(h)==LAP_RX_RUNNING); return h;
}
static void discovery_commands(void) {
    begin(0); lap_radio_info2_t info;
    assert(lap_radio_info2(0,&info,sizeof(info))==LAP_RX_EACCES); end();
    begin(LAP_CAP_RADIO_RX);
    assert(!lap_radio_info2(0,&info,sizeof(info)));
    assert(info.version==1 && info.frequency_count==2 && info.frequencies[1].max_hz==6000000000ull && info.gain_count==7);
    assert(lap_radio_info2(0,(void *)1,sizeof(info))==LAP_RX_EINVAL);
    int h=open_sim(5800000000ull,256000);
    assert(lap_rx_control(h,LAP_RX_CTL_TUNE,2000000000)==LAP_RX_ERANGE);
    assert(lap_rx_control(h,LAP_RX_CTL_DIRECT,1)==LAP_RX_EUNSUPPORTED);
    assert(ls_lap_rx24_sim_step("rxdelay=3"));
    int ticket=lap_rx_control(h,LAP_RX_CTL_TUNE,5700000123ull); assert(ticket>0);
    lap_rx_command_t c;
    assert(lap_rx_command_poll(ticket,&c,sizeof(c))==LAP_RX_EAGAIN && c.state==LAP_RX_COMMAND_QUEUED);
    assert(lap_rx_control(h,LAP_RX_CTL_GAIN,0)==LAP_RX_EBUSY);
    pump(2); assert(lap_rx_command_poll(ticket,&c,sizeof(c))==LAP_RX_EAGAIN);
    pump(3); assert(!lap_rx_command_poll(ticket,&c,sizeof(c)) && c.state==LAP_RX_COMMAND_APPLIED && c.applied==5700000123ll);
    ticket=lap_rx_control(h,LAP_RX_CTL_GAIN,LAP_GAIN_AUTO); assert(ticket>0); pump(5);
    assert(!lap_rx_command_poll(ticket,&c,sizeof(c)) && c.applied==LAP_GAIN_AUTO);
    int64_t value=0; assert(!lap_rx_control_get(h,LAP_RX_CTL_GAIN,&value) && value==LAP_GAIN_AUTO);
    ticket=lap_rx_control(h,LAP_RX_CTL_PPM,-12); pump(5);
    assert(!lap_rx_command_poll(ticket,&c,sizeof(c)) && c.applied==-12);
    assert(!lap_rx_control_get(h,LAP_RX_CTL_PPM,&value) && value==-12);
    ticket=lap_rx_control(h,LAP_RX_CTL_PPM,900); pump(5);
    assert(!lap_rx_command_poll(ticket,&c,sizeof(c)) && c.state==LAP_RX_COMMAND_FAILED && c.result==LAP_RX_ERANGE);
    assert(lap_rx_state(h)==LAP_RX_RUNNING);
    assert(ls_lap_rx24_sim_step("rxresult=-9"));
    ticket=lap_rx_control(h,LAP_RX_CTL_AGC,1); pump(5); assert(!lap_rx_command_poll(ticket,&c,sizeof(c)) && c.state==LAP_RX_COMMAND_FAILED);
    lap_rx_close(h); pump(1); assert(lap_rx_command_poll(ticket,&c,sizeof(c))==LAP_RX_EINVAL); end();
    ls_lap_rx24_sim_step("rxdelay=2");
}
static void channels_spectrum(void) {
    begin(LAP_CAP_RADIO_RX); int h=open_sim(5000000000ull,256000);
    lap_channel_config_t cfg={sizeof(cfg),1,20000,16000,4000};
    int channel=lap_rx_channel_open(h,&cfg); assert(channel>0);
    cfg.output_rate_hz=12345;
    assert(lap_rx_channel_open(h,&cfg)==LAP_RX_ERANGE);
    lap_spectrum_config_t spectrum={sizeof(spectrum),1,256,LAP_SPEC_RECT,1,25};
    assert(!lap_rx_spectrum_config(h,&spectrum));
    float *db=ls_lap_malloc(2048*sizeof(float)); int16_t *iq=ls_lap_malloc(16384*4);
    pump(35);
    lap_channel_status_t status; assert(!lap_rx_channel_status(channel,&status,sizeof(status)) && status.state==LAP_RX_RUNNING && status.output_rate_hz==16000 && status.dropped_pairs>0);
    lap_iq_meta2_t m;
    int n=lap_rx_channel_read(channel,iq,16384,&m,sizeof(m)); assert(n>0 && m.center_hz==5000020000ull && m.rate_hz==16000);
    lap_spectrum_meta_t meta;
    assert(lap_rx_spectrum_read(h,db,128,&meta,sizeof(meta))==LAP_RX_ERANGE);
    assert(lap_rx_spectrum_read(h,db,2048,&meta,sizeof(meta))==256 && meta.center_hz==5000000000ull && meta.span_hz==256000 && meta.bin_hz==1000);
    assert(meta.window==LAP_SPEC_RECT && meta.gain_tenths_db==LAP_GAIN_AUTO);
    unsigned epoch=meta.epoch;
    int ticket=lap_rx_control(h,LAP_RX_CTL_TUNE,5100000000ull); assert(ticket>0); pump(8);
    assert(lap_rx_spectrum_read(h,db,2048,&meta,sizeof(meta))==256 && meta.epoch!=epoch && meta.center_hz==5100000000ull);
    for (unsigned fft=64;fft<=2048;fft*=2) {
        spectrum.fft_size=fft; spectrum.window=LAP_SPEC_BLACKMAN; spectrum.averages=2;
        assert(!lap_rx_spectrum_config(h,&spectrum)); pump(8);
        assert(lap_rx_spectrum_read(h,db,2048,&meta,sizeof(meta))==(int)fft);
        for (unsigned i=0;i<fft;i++) assert(isfinite(db[i]));
    }
    lap_rx_channel_close(channel); assert(lap_rx_channel_status(channel,&status,sizeof(status))==LAP_RX_EINVAL);
    lap_rx_close(h); pump(1); end();
}
static void decoder_events(void) {
    begin(LAP_CAP_RADIO_RX); int h=open_sim(152600000,256000);
    lap_decoder_catalog_t catalog; assert(!lap_rx_decoder_catalog(&catalog,sizeof(catalog)) && (catalog.supported&(1u<<LAP_DEC_P25)));
    assert(ls_lap_rx24_sim_step("decode=page,12345,before attach"));
    lap_decoder_config_t cfg={sizeof(cfg),1,LAP_DEC_POCSAG,0};
    int d=lap_rx_decoder_attach(h,&cfg); assert(d>0); pump(2);
    lap_decoder_status_t status; assert(!lap_rx_decoder_status(d,&status,sizeof(status)) && status.state==LAP_RX_RUNNING);
    lap_page_event_t page; lap_event_header_t header;
    assert(!lap_rx_decoder_next(d,&header,sizeof(header),&page,sizeof(page)));
    ls_lap_rx24_sim_step("decode=aprs,WRONG,40,-70,not requested");
    assert(!lap_rx_decoder_next(d,&header,sizeof(header),&page,sizeof(page)));
    ls_lap_rx24_sim_step("decode=page,12345,hello structured");
    assert(lap_rx_decoder_next(d,&header,sizeof(header),&page,1)==LAP_RX_ERANGE);
    assert(lap_rx_decoder_next(d,&header,sizeof(header),&page,sizeof(page))==1);
    assert(header.decoder==LAP_DEC_POCSAG && header.radio==0 && header.center_hz==152600000 && (header.flags&LAP_EVENT_SYNTHETIC) && !(header.flags&LAP_EVENT_LIVE));
    assert(page.address==12345 && !strcmp(page.text,"hello structured"));
    for (int i=0;i<40;i++) ls_lap_rx24_sim_step("decode=page,12345,overflow");
    assert(!lap_rx_decoder_status(d,&status,sizeof(status)) && status.dropped==8);
    lap_rx_decoder_detach(d); assert(lap_rx_decoder_next(d,&header,sizeof(header),&page,sizeof(page))==LAP_RX_EINVAL); pump(1);
    /* Every adapter allocates, resets and frees through the actual firmware API. */
    for (unsigned kind=1;kind<=LAP_DEC_WFM;kind++) {
        if (!(catalog.supported&(1u<<kind))) continue;
        if (kind==LAP_DEC_ADSB || kind==LAP_DEC_P25) continue;
        cfg.decoder=kind; d=lap_rx_decoder_attach(h,&cfg); assert(d>0); pump(3);
        assert(!lap_rx_decoder_status(d,&status,sizeof(status)) && status.state==LAP_RX_RUNNING);
        lap_rx_decoder_detach(d); pump(1);
    }
    lap_rx_close(h); pump(1); end();
}
/* A backend with no synthetic samples: feed a real POCSAG FSK waveform through
   the shared worker and firmware FM discriminator, then assert typed output. */
static int quiet_poll(ls_lap_rx_actual_t *a) { return 0; }
static ls_lap_rx_backend_t quiet;
static void real_pager(unsigned rate) {
    quiet=*ls_lap_rx_default_backend(); quiet.poll=quiet_poll; ls_lap_radio_set_backend(&quiet);
    begin(LAP_CAP_RADIO_RX); int h=open_sim(152600000,rate);
    lap_decoder_config_t cfg={sizeof(cfg),1,LAP_DEC_POCSAG,0};
    int d=lap_rx_decoder_attach(h,&cfg); assert(d>0); pump(1);
    uint8_t bits[BITS_GEN_MAX]; size_t nb=pocsag_build_bits(1234568,3,"RX24 REAL PAGE",576,bits,sizeof(bits));
    size_t pairs=(nb+8)*rate/1200; double phase=0; int16_t iq[2048*2];
    for (size_t start=0;start<pairs;) {
        unsigned n=(unsigned)(pairs-start); if (n>2048) n=2048;
        for (unsigned i=0;i<n;i++) {
            size_t bit=(start+i)*1200/rate; if (bit>=nb) bit=nb-1;
            phase+=6.283185307179586*(bits[bit] ? 4500 : -4500)/rate;
            iq[i*2]=(int16_t)(12000*cos(phase)); iq[i*2+1]=(int16_t)(12000*sin(phase));
        }
        ls_lap_rx_push(iq,n); pump(1); start+=n;
    }
    lap_event_header_t header; lap_page_event_t page; int found=0;
    while (lap_rx_decoder_next(d,&header,sizeof(header),&page,sizeof(page))==1)
        if (page.address==1234568 && strstr(page.text,"RX24 REAL PAGE")) found++;
    assert(found>0); end(); ls_lap_radio_set_backend(NULL);
}

/* Reuse the firmware's BCH encoder as the existing P25 fixtures do, with
   test-local exported names; the runtime links the independent real decoder. */
#define check_NID_ec rx24_fixture_check_NID_ec
#define check_NID rx24_fixture_check_NID
#define nid_distance rx24_fixture_nid_distance
#include "p25p1_check_nid.c"
#undef check_NID_ec
#undef check_NID
#undef nid_distance
#include "p25_tsbk.h"
#define LS_EQ_UINT(a,b) assert((a)==(b))
#include "../fixtures/p25_tsbk_fixture.h"
static void real_p25(void) {
    quiet=*ls_lap_rx_default_backend(); quiet.poll=quiet_poll; ls_lap_radio_set_backend(&quiet);
    begin(LAP_CAP_RADIO_RX); int h=open_sim(851000000,240000);
    lap_decoder_config_t cfg={sizeof(cfg),1,LAP_DEC_P25,0};
    int d=lap_rx_decoder_attach(h,&cfg); assert(d>0); pump(1);
    uint8_t symbols[120+24+33+303]; unsigned count=0;
    for (int i=0;i<120;i++) symbols[count++]=(i&1) ? 1 : 3;
    for (const char *p=P25P1_SYNC;*p;p++) symbols[count++]=*p-'0';
    int data[BCH_KK], par[BCH_RR]; char cw[BCH_NN];
    for (int i=0;i<12;i++) data[i]=(0x293>>(11-i))&1;
    for (int i=0;i<4;i++) data[12+i]=(7>>(3-i))&1;
    bch_encode(data,par);
    for (int i=0;i<BCH_KK;i++) cw[i]=data[i];
    for (int i=0;i<BCH_RR;i++) cw[BCH_KK+i]=par[i];
    for (int i=0;i<11;i++) symbols[count++]=(cw[i*2]<<1)|cw[i*2+1];
    symbols[count++]=0;
    for (int i=11;i<31;i++) symbols[count++]=(cw[i*2]<<1)|cw[i*2+1];
    symbols[count++]=cw[62]<<1;
    uint8_t iden[12],grant[12],encoded[196];
    make_tdma_iden(iden,2); iden[0]&=~0x80; add_crc(iden);
    make_group_grant(grant,(2u<<12)|4,1234);
    encode_block(iden,encoded); encode_block(grant,encoded+98);
    unsigned at=0;
    for (unsigned i=0;i<303;i++) symbols[count++]=(i>=14 && (i-14)%36==0) ? 0 : at<196 ? encoded[at++] : 0;
    double phase=0; int16_t iq[4096]; unsigned fill=0;
    static const int deviation[]={600,1800,-600,-1800};
    for (int repeat=0;repeat<5;repeat++) {
        for (unsigned sym=0;sym<count;sym++) for (int j=0;j<50;j++) {
            phase+=6.283185307179586*deviation[symbols[sym]]/240000;
            iq[fill*2]=(int16_t)(22000*cos(phase)); iq[fill*2+1]=(int16_t)(22000*sin(phase));
            if (++fill==2048) { ls_lap_rx_push(iq,fill); pump(1); fill=0; }
        }
    }
    if (fill) { ls_lap_rx_push(iq,fill); pump(1); }
    lap_event_header_t header; lap_p25_event_t event; int found=0;
    while (lap_rx_decoder_next(d,&header,sizeof(header),&event,sizeof(event))==1) {
        assert(event.nac==0x293 && event.duid==7);
        if (event.talkgroup==1234 && event.source==0x123456 && event.carrier_hz) {
            assert(event.opcode==0 && event.carrier_hz==851037500ull && (header.flags&LAP_EVENT_CRC_OK)); found++;
        }
    }
    assert(found>0); end(); ls_lap_radio_set_backend(NULL);
}
static void unplug(void) {
    begin(LAP_CAP_RADIO_RX); int h=open_sim(152600000,256000);
    ls_lap_radio_sim_absent=true; pump(1); assert(lap_rx_state(h)==LAP_RX_ENODEV);
    lap_rx_close(h); pump(1); end();
    begin(LAP_CAP_RADIO_RX); lap_caps_t caps; assert(!lap_caps(&caps,sizeof(caps)) && !caps.radio_present); end();
    ls_lap_radio_sim_absent=false;
}
int main(void) {
    discovery_commands(); channels_spectrum(); decoder_events(); real_pager(256000); real_pager(125000); real_p25(); unplug();
    puts("test_lap_rx24: discovery/completions/64-bit/channel/FFT/decoder isolation/real POCSAG+P25/hotplug passed"); return 0;
}
