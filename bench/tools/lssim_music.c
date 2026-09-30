/* Simulator fixtures only. Production screen and renderer are unchanged. */
#include "ls_music_backend.h"
#include "esp_timer.h"
#include <math.h>
static ls_music_state_t state;
static bool warm;
static int vol=43,index;
static uint32_t position;
static int64_t since;
static const char*names[]={"forge-test.wav","Gojira - The art of dying.wav","Night Dive - Below the surface.wav","LakeShark - Midnight radio.wav"};
bool ls_music_open(void){state=LS_MUSIC_STOPPED;return true;}
void ls_music_close(void){state=LS_MUSIC_STOPPED;}
bool ls_music_scan(int source){(void)source;return true;}
int ls_music_count(void){return 4;}
const char*ls_music_name(int i){return i>=0&&i<4?names[i]:"";}
const char*ls_music_error(void){return "";}
bool ls_music_play(int i){if(i<0||i>=4)return false;index=i;position=72000;since=esp_timer_get_time();state=LS_MUSIC_PLAYING;return true;}
uint32_t ls_music_position_ms(void){return position+(state==LS_MUSIC_PLAYING?(uint32_t)((esp_timer_get_time()-since)/1000):0);}
uint32_t ls_music_duration_ms(void){return index==0?8000:594000;}
bool ls_music_toggle(void){if(state==LS_MUSIC_PLAYING){position=ls_music_position_ms();state=LS_MUSIC_PAUSED;}else{since=esp_timer_get_time();state=LS_MUSIC_PLAYING;}return true;}
void ls_music_stop(void){state=LS_MUSIC_STOPPED;position=0;}
ls_music_state_t ls_music_state(void){return state;}
int ls_music_volume(void){return vol;}
void ls_music_volume_step(int d){vol+=d;if(vol<0)vol=0;if(vol>100)vol=100;}
const char*ls_music_output(void){return "SIMULATED AUDIO";}
bool ls_music_warm(void){return warm;}
void ls_music_set_warm(bool b){warm=b;}
unsigned ls_music_peak(int ch){double t=esp_timer_get_time()/1000000.0;return ls_music_live()?(unsigned)((.12+.8*fabs(sin(t*7+ch*.9))*fabs(sin(t*1.3)))*32768):0;}
/* A falling tilt with each band breathing at its own rate. */
int ls_music_spectrum(float lo,float hi,int n,float*db){(void)lo;(void)hi;double t=esp_timer_get_time()/1000000.0;for(int i=0;i<n;i++)db[i]=ls_music_live()?(float)(-10-2.2*i+9*sin(t*(1.7+.37*i)+i)):-INFINITY;return ls_music_live()?n:0;}
bool ls_music_format(ls_music_format_t*f){f->rate_hz=44100;f->bits=16;f->channels=2;f->mono_out=true;return true;}
float ls_music_volume_db(void){return (vol-100)*0.5f;}
static ls_music_mic_t mic;
bool ls_music_mic_start(bool record){state=LS_MUSIC_STOPPED;mic=record?LS_MIC_RECORDING:LS_MIC_LIVE;since=esp_timer_get_time();return true;}
void ls_music_mic_stop(void){mic=LS_MIC_OFF;}
ls_music_mic_t ls_music_mic_state(void){return mic;}
const char*ls_music_mic_file(void){return "/sdcard/music/REC-001.wav";}
uint32_t ls_music_mic_ms(void){return mic==LS_MIC_RECORDING?(uint32_t)((esp_timer_get_time()-since)/1000):0;}
bool ls_music_mic_step(void){return true;}
bool ls_music_live(void){return state==LS_MUSIC_PLAYING||mic!=LS_MIC_OFF;}
/* A slow sweep with two harmonics over a noise floor, one column per 46 ms. */
bool ls_music_spectrogram(ls_music_cursor_t*c,int hops,int rows,float*db){if(!ls_music_live())return false;uint32_t due=(uint32_t)(esp_timer_get_time()/(46440*hops));if(!c->started){c->started=true;c->next=due>120?due-120:0;}if(c->next>=due)return false;double t=c->next++*0.04644*hops;double f=0.12+0.1*sin(t*0.9);for(int i=0;i<rows;i++){double x=(double)i/rows,d=-104+8*sin(i*1.7+t*3)-18*x;for(int h=1;h<=3;h++){double e=fabs(x-f*h)*rows;if(e<1.5)d=fmax(d,-25-8*h-6*e);}db[i]=(float)d;}return true;}
