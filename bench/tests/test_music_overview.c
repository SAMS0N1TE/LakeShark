#include "ls_audio_overview.h"
#include <assert.h>
#include <stdio.h>
static void finish(ls_audio_overview *o) {
    for(unsigned i=0;i<1000 && !ls_audio_overview_ready(o);i++)ls_audio_overview_step(o,2048);
    assert(ls_audio_overview_ready(o) && ls_audio_overview_progress(o)==100);
}
int main(int argc,char **argv) {
    assert(argc==2);char root[512],source[512];
    snprintf(root,sizeof(root),"%s/overview",argv[1]);snprintf(source,sizeof(source),"%s/tone.wav",argv[1]);
    ls_audio_overview *o=ls_audio_overview_new(root);assert(o);
    assert(ls_audio_overview_request(o,source,true));finish(o);assert(!ls_audio_overview_cached(o));
    assert(ls_audio_overview_rate(o)==44100 && ls_audio_overview_duration_ms(o)==2000);
    int16_t lo,hi;assert(ls_audio_overview_wave(o,500,&lo,&hi) && lo<-7000 && hi>7000);
    unsigned brightest=0;for(unsigned bin=0;bin<LS_OV_BINS;bin++)if(ls_audio_overview_spectro(o,250,bin)>brightest)brightest=ls_audio_overview_spectro(o,250,bin);
    assert(brightest>100);assert(ls_audio_overview_free(o));
    o=ls_audio_overview_new(root);assert(o && ls_audio_overview_request(o,source,false));
    ls_audio_overview_step(o,1);assert(ls_audio_overview_ready(o) && ls_audio_overview_cached(o));
    assert(ls_audio_overview_slice_request(o,500,4096,0));ls_audio_overview_step(o,1);
    uint32_t at;unsigned bins;assert(ls_audio_overview_slice_ready(o,&at,&bins) && at==500 && bins==2049);
    assert(ls_audio_overview_slice_db(o,93)>-20 && ls_audio_overview_slice_db(o,929)<-70);
    FILE *f=fopen(source,"ab");assert(f);fputc(0,f);fclose(f);
    assert(ls_audio_overview_request(o,source,false));ls_audio_overview_step(o,1);assert(!ls_audio_overview_cached(o));finish(o);
    assert(ls_audio_overview_request(o,"missing-audio-file",false));ls_audio_overview_step(o,32);
    assert(!ls_audio_overview_ready(o) && *ls_audio_overview_error(o));
    assert(ls_audio_overview_free(o));puts("PASS streamed overview, waveform/spectrogram, one-slice cache reopen, identity invalidation, missing file");return 0;
}
