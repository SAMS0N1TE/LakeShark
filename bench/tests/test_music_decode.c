#include "ls_decode.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
static float amplitude(const int16_t *p,size_t n,int ch,float hz,unsigned rate) {
    double re=0,im=0;
    for(size_t i=0;i<n;i++){double a=6.283185307179586*hz*i/rate;re+=p[2*i+ch]*cos(a);im+=p[2*i+ch]*sin(a);}
    return (float)(2*sqrt(re*re+im*im)/n);
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc==2);
    static const char *ext[]={"wav","mp3","flac","ogg"};
    int16_t *pcm=malloc(8192*2*sizeof(*pcm));assert(pcm);
    for(int i=0;i<4;i++) {
        char path[512];snprintf(path,sizeof(path),"%s/tone.%s",argv[1],ext[i]);
        FILE *f=fopen(path,"rb");assert(f);
        printf("Testing %s\n",ext[i]);ls_decode *d=ls_decode_open(f);assert(d);
        assert(ls_decode_rate(d)==44100 && ls_decode_channels(d)==2);
        assert(ls_decode_frames(d)>=88000 && ls_decode_frames(d)<92000);
        assert(ls_decode_seek(d,22050));assert(ls_decode_position(d)==22050);
        size_t got=ls_decode_read(d,pcm,8192);assert(got==8192);
        assert(fabsf(amplitude(pcm,got,0,1000,44100)-12000)<650);
        assert(fabsf(amplitude(pcm,got,1,2000,44100)-6000)<500);
        assert(amplitude(pcm,got,0,2000,44100)<500);
        assert(ls_decode_seek(d,0));assert(ls_decode_read(d,pcm,127)==127);
        assert(ls_decode_seek(d,UINT64_MAX));assert(ls_decode_read(d,pcm,127)==0);
        assert(!ls_decode_failed(d));ls_decode_close(d);assert(fclose(f)==0);
        printf("PASS %s: stream, channels, duration, tone, exact seek, EOF\n",ext[i]);
    }
    FILE *bad=tmpfile();assert(bad);fputs("not audio",bad);rewind(bad);assert(!ls_decode_open(bad));fclose(bad);
    free(pcm);return 0;
}
