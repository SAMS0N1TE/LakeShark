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

#endif
