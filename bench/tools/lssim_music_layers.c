/* Host-only view fixture: decode the named file to memory, never to a sink. */
#include "ls_music_backend.h"
#include "ls_decode.h"
#include <stdlib.h>
#include <stdio.h>
static ls_audio_analysis *analysis;
static ls_audio_overview *overview;
static ls_audio_tools *tools;
static bool attempted;
ls_audio_analysis *ls_music_analysis(void) {
    if(attempted)return analysis;
    attempted=true;const char *path=getenv("LSSIM_MUSIC_PCM");if(!path)return NULL;
    FILE *file=fopen(path,"rb");if(!file)return NULL;
    ls_decode *d=ls_decode_open(file);if(!d){fclose(file);return NULL;}
    analysis=ls_audio_analysis_new();tools=ls_audio_tools_new();
    int16_t pcm[4096];size_t got;
    while(analysis&&(got=ls_decode_read(d,pcm,2048))){
        ls_audio_analysis_append(analysis,pcm,got,ls_decode_channels(d),ls_decode_rate(d));
        ls_audio_analysis_process(analysis,4);
        if(tools)ls_audio_tools_feed(tools,pcm,got,ls_decode_channels(d),ls_decode_rate(d));
    }
    ls_decode_close(d);fclose(file);
    overview=ls_audio_overview_new("music-view-cache");
    if(overview&&ls_audio_overview_request(overview,path,false))
        for(unsigned i=0;i<10000&&!ls_audio_overview_ready(overview);i++)ls_audio_overview_step(overview,2048);
    return analysis;
}
ls_audio_overview *ls_music_overview(void){ls_music_analysis();if(overview)ls_audio_overview_step(overview,1);return overview;}
ls_audio_tools *ls_music_tools(void){ls_music_analysis();return tools;}
