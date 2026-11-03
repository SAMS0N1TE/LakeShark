/* API 2.6: bounded HackRF burst generator and recording replayer. */
#include "lakeshark_app.h"
#include <stdio.h>
static int radio=-1, job, error;
static uint64_t frequency=100000000;
static lap_hackrf_tx_config_t gain={.size=sizeof(gain),.version=1};
static lap_packet_config_t config;
static int8_t iq[8192];
static void discover(void) {
    lap_caps_t caps; radio=-1;
    if(lap_caps(&caps,sizeof(caps))<0) return;
    lap_radio_info_t radios[8]; int count=lap_radio_list(radios,8);
    for(int i=0;i<count;i++) {
        lap_radio_modes_t modes;
        if(radios[i].present&&radios[i].kind==LAP_RADIO_KIND_HACKRF&&
           !lap_radio_modes(i,&modes)&&(modes.modes&LAP_MODE_TX_IQ)) { radio=i; break; }
    }
}
static void enter(void) {
    job=error=0; gain.vga_db=0; gain.amp=0; discover();
    /* A four-sample complex tone, 500 kHz from centre at 2 MS/s. */
    for(int i=0;i<4096;i++) { const int8_t c[4]={32,0,-32,0},s[4]={0,32,0,-32}; iq[2*i]=c[i%4]; iq[2*i+1]=s[i%4]; }
}
static void leave(void) { if(job>0) lap_tx_cancel(job); }
static bool key(lap_key_t k,char ch) {
    (void)k;
    if(ch=='c'||ch=='C') { if(job>0) lap_tx_cancel(job); return true; }
    if(job>0) return false;
    if(ch=='+'&&gain.vga_db<47) { gain.vga_db++; return true; }
    if(ch=='-'&&gain.vga_db) { gain.vga_db--; return true; }
    if(ch=='a'||ch=='A') { gain.amp=!gain.amp; return true; }
    if(ch=='f'||ch=='F') { frequency+=1000000; return true; }
    if(ch!='t'&&ch!='T'&&ch!='r'&&ch!='R') return false;
    discover(); if(radio<0) return true;
    error=lap_tx_hackrf_config(radio,&gain); if(error<0) return true;
    config=(lap_packet_config_t){.size=sizeof(config),.version=1,.frequency_hz=frequency,
        .modulation=LAP_MOD_LORA,.sf=7,.coding_rate=5,.bandwidth_hz=2000000,.power_dbm=0};
    job=ch=='r'||ch=='R'?lap_tx_replay(radio,&config,"signal.lsr"):lap_tx_iq(radio,&config,iq,4096,2000000);
    if(job<0) { error=job; job=0; } return true;
}
static void draw(void) {
    if(job>0) { lap_io_status_t s; if(!lap_tx_status(job,&s)&&s.state>=LAP_IO_DONE) { error=s.error; job=0; } }
    if(radio<0&&!(lap_frame()%25)) discover();
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    char line[120];
    lap_put_str(1,1,"HACKRF SIGNAL",LAP_ATTR(LAP_CYAN|LAP_BRIGHT,LAP_BLACK));
    snprintf(line,sizeof(line),"%lu MHz  VGA %u dB  amp %s",(unsigned long)(frequency/1000000),gain.vga_db,gain.amp?"on":"off");
    lap_put_str(1,3,line,LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,5,radio<0?"Attach a HackRF to enable TX":"T burst  R replay signal.lsr  C cancel",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
    lap_put_str(1,7,"+/- VGA   A amplifier   F centre +1 MHz",LAP_ATTR(LAP_WHITE,LAP_BLACK));
    snprintf(line,sizeof(line),"Job %d   result %d",job,error); lap_put_str(1,9,line,LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
LAP_APP(.id="hackrf_signal",.name="HACKRF SIGNAL",.sub="Bounded I/Q TX",.version="1.0",.author="LakeShark",
    .icon=LAP_ICON_GEAR,.color=LAP_CYAN,.flags=LAP_CAP_RADIO_RX|LAP_CAP_RADIO_TX,.enter=enter,.leave=leave,.draw=draw,.key=key);
