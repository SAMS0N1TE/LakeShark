#ifndef LS_CALLS_HOST_AUDIO

/* Screens without a media fixture have no decoded audio. */
#include "../../components/apps/tui/ls_music_backend.h"
#include <string.h>
unsigned ls_music_peak(int ch){(void)ch;return 0;}
int ls_music_spectrum(float lo,float hi,int n,float *db){(void)lo;(void)hi;(void)n;(void)db;return 0;}
bool ls_music_format(ls_music_format_t *out){memset(out,0,sizeof(*out));return false;}
bool ls_music_spectrogram_line(ls_music_cursor_t *c,unsigned hop,float *p){(void)c;(void)hop;(void)p;return false;}
int ls_music_volume(void){return 0;}
float ls_music_volume_db(void){return -50.0f;}
bool ls_music_muted(void){return false;}
uint32_t ls_music_position_ms(void){return 0;}
bool ls_music_seek_ms(uint32_t ms){(void)ms;return false;}
void ls_music_mic_stop(void){}

#endif

/* Views without a PCM fixture are explicitly empty. */
#include "../../components/apps/tui/ls_music_backend.h"
#ifndef LS_CALLS_HOST_AUDIO
ls_audio_analysis *ls_music_analysis(void){return NULL;}
ls_audio_overview *ls_music_overview(void){return NULL;}
ls_audio_tools *ls_music_tools(void){return NULL;}
#endif
bool ls_music_speed_set(unsigned v){(void)v;return false;}
unsigned ls_music_speed_get(void){return 1000;}
bool ls_music_loop_set(uint32_t a,uint32_t b,bool v){(void)a;(void)b;(void)v;return false;}
bool ls_music_loop_get(uint32_t *a,uint32_t *b){*a=*b=0;return false;}
bool ls_music_marker_add(void){return false;}
void ls_music_markers_clear(void){}
unsigned ls_music_marker_count(void){return 0;}
bool ls_music_marker_get(unsigned i,uint32_t *v){(void)i;(void)v;return false;}
bool ls_music_tone_start(ls_gen_kind k,unsigned a,unsigned b,unsigned t,int db){(void)k;(void)a;(void)b;(void)t;(void)db;return false;}
bool ls_music_tone_active(void){return false;}
