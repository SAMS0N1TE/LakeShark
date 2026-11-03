/* A tiny app-owned OOK threshold protocol, on a decimated worker channel.
 * The sample message is [0xA5 sync][8 payload bits], one bit per 16 samples.
 * The firmware knows neither sync nor framing. No SDK calls in process(). */
#include "lakeshark_app.h"
#include <stdio.h>
#include <string.h>
static struct { uint32_t epoch,clock,ones,bits,word; } decoder;
static int session,channel,worker;
static unsigned messages;
static unsigned last_state;
static char why[96]="Waiting for radio";
static int process(const int16_t *iq,uint32_t pairs,const lap_iq_meta2_t *meta,void *state,void *output,uint32_t cap) {
    (void)state;
    if(decoder.epoch!=meta->epoch || (meta->flags&LAP_IQ_GAP)) { memset(&decoder,0,sizeof(decoder)); decoder.epoch=meta->epoch; }
    int result=0;
    for(uint32_t i=0;i<pairs;i++) {
        int32_t re=iq[i*2],im=iq[i*2+1];
        if((int64_t)re*re+(int64_t)im*im>4000000) decoder.ones++;
        if(++decoder.clock==16) {
            decoder.word=(decoder.word<<1)|(decoder.ones>=8); decoder.bits++;
            decoder.clock=decoder.ones=0;
            if(decoder.bits>=16 && ((decoder.word>>8)&255)==0xa5 && cap) { ((uint8_t *)output)[0]=decoder.word&255; result=1; }
        }
    }
    return result;
}
static void enter(void) {
    memset(&decoder,0,sizeof(decoder));
    lap_caps_t caps; if(lap_caps(&caps,sizeof(caps)) || !caps.radio_present) { strcpy(why,"No radio fitted: attach an SDR to decode."); return; }
    for(unsigned i=0;i<caps.radio_count;i++) if(caps.radio_present&(1u<<i)) {
        lap_radio_info2_t info;
        if(lap_radio_info2((int)i,&info,sizeof(info)) || !info.present || info.busy) continue;
        uint32_t rate=256000; bool supported=false;
        for(unsigned j=0;j<info.rate_count;j++) if(rate>=info.rates[j].min_hz&&rate<=info.rates[j].max_hz) supported=true;
        if(!supported) continue;
        lap_rx_config_t cfg={433920000,rate,LAP_GAIN_AUTO,LAP_RX_SPECTRUM_ONLY};
        session=lap_rx_open((int)i,&cfg); if(session>0) break;
    }
    if(session<=0) strcpy(why,"Radio busy or unsupported rate; close and retry.");
}
static void draw(void) {
    if(session>0&&lap_rx_state(session)==LAP_RX_RUNNING&&!channel) {
        lap_channel_config_t c={sizeof(c),1,0,16000,4000}; channel=lap_rx_channel_open(session,&c);
        if(channel>0) {
            lap_dsp_config_t cfg={sizeof(cfg),1,4000,process,&decoder,sizeof(decoder)};
            worker=lap_rx_dsp_start(channel,&cfg);
        }
    }
    if(worker>0) {
        lap_dsp_status_t st; lap_rx_dsp_status(worker,&st);
        if(st.state!=last_state) { last_state=st.state; lap_log("custom worker state=%lu overruns=%lu",(unsigned long)st.state,(unsigned long)st.overruns); }
        lap_dsp_result_t meta; uint8_t payload[LAP_DSP_OUTPUT_MAX];
        for(int i=0;i<4&&lap_rx_dsp_next(worker,&meta,payload,sizeof(payload))==1;i++) {
            messages++; lap_log("custom decoded %u flags=%lu",payload[0],(unsigned long)meta.iq.flags);
        }
        snprintf(why,sizeof(why),"worker %lu blocks %lu drops %lu messages %u",(unsigned long)st.state,(unsigned long)st.blocks,(unsigned long)st.dropped,messages);
        /* Worker has an independent tap; no need to drain the display queue. */
    }
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK)); lap_put_str(1,2,"CUSTOM PROTOCOL DSP",LAP_ATTR(LAP_CYAN,LAP_BLACK));
    lap_put_str(1,4,why,LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
static void leave(void) {
    if(worker>0) lap_rx_dsp_close(worker);
    if(channel>0) lap_rx_channel_close(channel);
    if(session>0) lap_rx_close(session);
    worker=channel=session=0;
}
LAP_APP(.id="custom_decoder",.name="CUSTOM DSP",.sub="app-owned worker protocol",.version="1.0",.author="LakeShark",
    .icon=LAP_ICON_WAVE,.color=LAP_CYAN,.flags=LAP_CAP_RADIO_RX,.enter=enter,.draw=draw,.leave=leave);
