#include "ls_audio_tools.h"
#include "aprs.h"
#include "fm_tone.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ls_audio_tools *tools;
static double phase;
static void tone(float a,float b,unsigned ms,float amplitude) {
    int16_t pcm[160];unsigned frames=ms*16;
    for(unsigned at=0;at<frames;at+=160) {
        unsigned n=frames-at<160?frames-at:160;
        for(unsigned i=0;i<n;i++,phase++)pcm[i]=(int16_t)(amplitude*32767*(sin(6.283185307179586*a*phase/16000)+(b?sin(6.283185307179586*b*phase/16000):0)));
        ls_audio_tools_feed(tools,pcm,n,1,16000);
    }
}
static bool heard(ls_tool_kind kind,const char *text) {
    ls_tool_event e[64];ls_tool_status status;unsigned count=0;
    assert(ls_audio_tools_snapshot(tools,&status,e,64,&count));
    for(unsigned i=0;i<count;i++)if(e[i].kind==kind && strstr(e[i].text,text))return true;
    return false;
}
static void morse(const char *code) {
    for(const char *p=code;*p;p++) {
        if(*p==' '){tone(0,0,160,0);continue;}
        tone(700,0,*p=='.'?80:240,.4f);tone(0,0,80,0);
    }
    tone(0,0,800,0);
}
static unsigned char bits[6000];static unsigned nbits;
static void flag(void){for(int i=0;i<8;i++)bits[nbits++]=(0x7e>>i)&1;}
static void afsk(void) {
    unsigned char frame[512];const char *calls[]={"APRS  ","N0CALL"};
    for(int a=0;a<2;a++){for(int i=0;i<6;i++)frame[a*7+i]=(unsigned char)(calls[a][i]<<1);frame[a*7+6]=a?0x61:0x60;}
    frame[14]=3;frame[15]=0xf0;const char *info=">MUSIC packet test";size_t n=strlen(info);memcpy(frame+16,info,n);n+=16;
    uint16_t crc=aprs_crc(frame,n)^0xffff;frame[n++]=(unsigned char)crc;frame[n++]=(unsigned char)(crc>>8);
    nbits=0;for(int i=0;i<32;i++)flag();unsigned ones=0;
    for(size_t i=0;i<n;i++)for(int j=0;j<8;j++){unsigned b=(frame[i]>>j)&1;bits[nbits++]=b;ones=b?ones+1:0;if(ones==5){bits[nbits++]=0;ones=0;}}
    for(int i=0;i<4;i++)flag();
    int16_t pcm[37];unsigned used=0,last=0;int mark=1;double angle=0;
    unsigned total=(unsigned)ceil(nbits*16000.0/1200);
    for(unsigned i=0;i<total;i++) {
        unsigned bit=(unsigned)floor(i*1200.0/16000);
        if(i==0 || bit!=last){if(!bits[bit])mark^=1;last=bit;}
        angle+=6.283185307179586*(mark?1200:2200)/16000;pcm[used++]=(int16_t)(14000*sin(angle));
        if(used==37){ls_audio_tools_feed(tools,pcm,used,1,16000);used=0;}
    }
    if(used)ls_audio_tools_feed(tools,pcm,used,1,16000);
}
int main(void) {
    tools=ls_audio_tools_new();assert(tools);
    tone(697,1209,160,.2f);tone(0,0,120,0);tone(770,1336,160,.2f);tone(0,0,120,0);
    assert(heard(LS_TOOL_DTMF,"1") && heard(LS_TOOL_DTMF,"5"));
    ls_audio_tools_reset(tools);tone(697,0,300,.4f);assert(!heard(LS_TOOL_DTMF,"1"));
    ls_audio_tools_reset(tools);tone(100,0,1200,.4f);assert(heard(LS_TOOL_CTCSS,"100.0"));
    ls_audio_tools_reset(tools);uint32_t dcs=fm_dcs_word(0023);int16_t code_pcm[160];
    for(unsigned at=0;at<32000;at+=160){for(unsigned i=0;i<160;i++){unsigned bit=(unsigned)((at+i)*134.4/16000)%23;code_pcm[i]=(dcs>>bit)&1?12000:-12000;}ls_audio_tools_feed(tools,code_pcm,160,1,16000);}
    assert(heard(LS_TOOL_DCS,"D023"));
    ls_audio_tools_reset(tools);int16_t stereo_pcm[441*2];
    for(unsigned at=0;at<8820;at+=441){for(unsigned i=0;i<441;i++){double t=(at+i)/44100.0;stereo_pcm[2*i]=stereo_pcm[2*i+1]=(int16_t)(6500*(sin(6.283185307179586*852*t)+sin(6.283185307179586*1477*t)));}ls_audio_tools_feed(tools,stereo_pcm,441,2,44100);}
    assert(heard(LS_TOOL_DTMF,"9"));
    ls_audio_tools_reset(tools);morse("... --- ...");assert(heard(LS_TOOL_CW,"SOS"));
    ls_tool_status status;ls_tool_event e[64];unsigned count;assert(ls_audio_tools_snapshot(tools,&status,e,64,&count));assert(status.wpm>12 && status.wpm<18);
    ls_audio_tools_reset(tools);afsk();assert(heard(LS_TOOL_APRS,"MUSIC packet test"));
    ls_audio_tools_reset(tools);tone(0,0,1000,0);assert(!heard(LS_TOOL_DTMF,"") && !heard(LS_TOOL_CW,"") && !heard(LS_TOOL_APRS,""));
    ls_audio_analysis *source=ls_audio_analysis_new();assert(source);
    ls_audio_analysis_reset_at(source,30000);
    int16_t pair[160*2];
    for(unsigned at=0;at<3200;at+=160){
        for(unsigned i=0;i<160;i++)pair[2*i]=pair[2*i+1]=(int16_t)(6500*(sin(6.28318530718*697*(at+i)/16000)+sin(6.28318530718*1209*(at+i)/16000)));
        ls_audio_analysis_append(source,pair,160,2,16000);ls_audio_tools_pull(tools,source,160);
    }
    assert(ls_audio_tools_snapshot(tools,&status,e,64,&count));assert(count&&e[0].time_ms>=30000&&e[0].time_ms<30200);
    ls_audio_analysis_free(source);
    ls_audio_tools_free(tools);
    ls_audio_generator *g=ls_audio_generator_new();assert(g);int16_t *pcm=malloc(16000*2*sizeof(*pcm));assert(pcm);
    ls_audio_generator_render(g,pcm,16000,2,16000);int peak=0,crossings=0;
    for(int i=0;i<16000;i++){assert(pcm[2*i]==pcm[2*i+1]);int v=abs(pcm[2*i]);if(v>peak)peak=v;if(i && pcm[2*i]>=0 && pcm[2*i-2]<0)crossings++;}
    assert(peak>=2060 && peak<=2070 && abs(crossings-1000)<2);
    assert(!ls_audio_generator_set(g,LS_GEN_SINE,1000,1000,1000,-3));
    assert(ls_audio_generator_set(g,LS_GEN_SWEEP,500,2000,1000,-24));ls_audio_generator_render(g,pcm,16000,1,16000);
    int first=0,last_count=0;for(int i=1;i<4000;i++)if(pcm[i]>=0 && pcm[i-1]<0)first++;
    for(int i=12001;i<16000;i++)if(pcm[i]>=0 && pcm[i-1]<0)last_count++;
    assert(last_count>first*2);free(pcm);ls_audio_generator_free(g);
    puts("PASS PCM DTMF, CTCSS, CW/WPM, Bell202/APRS, silence and modest sine/sweep generator");return 0;
}
