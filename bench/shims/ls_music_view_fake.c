#ifndef LS_CALLS_HOST_AUDIO

/* Screens without a media fixture have no decoded audio. */
#include "../../components/apps/tui/ls_music_backend.h"
#include <string.h>
unsigned ls_music_peak(int ch){(void)ch;return 0;}
int ls_music_spectrum(float lo,float hi,int n,float *db){(void)lo;(void)hi;(void)n;(void)db;return 0;}
bool ls_music_format(ls_music_format_t *out){memset(out,0,sizeof(*out));return false;}
bool ls_music_spectrogram(ls_music_cursor_t *c,int h,int n,float *db){(void)c;(void)h;(void)n;(void)db;return false;}

#endif
