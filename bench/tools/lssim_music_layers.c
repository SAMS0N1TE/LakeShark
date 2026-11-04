/* Host-only view fixture: decode the named file to memory, never to a sink.
   LSSIM_MUSIC_LIVE=1 feeds it 40 ms per frame instead and builds no
   overview, the way the board shows a track before its overview is ready. */
#include "ls_music_backend.h"
#include "ls_decode.h"
#include <stdlib.h>
#include <stdio.h>
static ls_audio_analysis *analysis;
static ls_audio_overview *overview;
static ls_audio_tools *tools;
static bool attempted, live;
static ls_decode *stream;
static FILE *stream_file;
static void feed(size_t frames) {
    int16_t pcm[4096];size_t got;
    while(analysis&&frames&&(got=ls_decode_read(stream,pcm,frames>2048?2048:frames))){
        ls_audio_analysis_append(analysis,pcm,got,ls_decode_channels(stream),ls_decode_rate(stream));
        ls_audio_analysis_process(analysis,4);
        if(tools)ls_audio_tools_feed(tools,pcm,got,ls_decode_channels(stream),ls_decode_rate(stream));
        frames-=got;
    }
}
ls_audio_analysis *ls_music_analysis(void) {
    if(attempted)return analysis;
    attempted=true;const char *path=getenv("LSSIM_MUSIC_PCM");if(!path)return NULL;
    live=getenv("LSSIM_MUSIC_LIVE")!=NULL;
    stream_file=fopen(path,"rb");if(!stream_file)return NULL;
    stream=ls_decode_open(stream_file);if(!stream){fclose(stream_file);return NULL;}
    analysis=ls_audio_analysis_new();tools=ls_audio_tools_new();
    if(live)return analysis;
    feed((size_t)-1);
    ls_decode_close(stream);fclose(stream_file);stream=NULL;
    overview=ls_audio_overview_new("music-view-cache");
    if(overview&&ls_audio_overview_request(overview,path,false))
        for(unsigned i=0;i<10000&&!ls_audio_overview_ready(overview);i++)ls_audio_overview_step(overview,2048);
    return analysis;
}
ls_audio_overview *ls_music_overview(void){
    ls_music_analysis();
    if(live&&stream)feed(ls_decode_rate(stream)*40/1000);
    if(overview)ls_audio_overview_step(overview,1);
    return overview;
}
ls_audio_tools *ls_music_tools(void){ls_music_analysis();return tools;}
