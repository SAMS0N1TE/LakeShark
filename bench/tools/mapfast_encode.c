/* Host analogue of the P4 ROM tdefl API. No hardware. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "miniz.c"
static size_t output_bytes;
static mz_bool count_output(const void *p,int n,void *ctx) {(void)p;(void)ctx;output_bytes+=n;return 1;}
static double now(void) {LARGE_INTEGER n,f;QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return 1e6*(double)n.QuadPart/f.QuadPart;}
static void u16(uint8_t **p,unsigned n) {*(*p)++=(uint8_t)n;*(*p)++=(uint8_t)(n>>8);}
static uint16_t lookup[65536],palette[256];
static uint32_t slots[512];
static unsigned slot(uint16_t colour){unsigned i=((unsigned)colour*40503u)&511;while(slots[i] && (uint16_t)slots[i]!=colour)i=(i+1)&511;return i;}
static size_t encode(const uint16_t *px,int w,int h,int form,uint8_t *out) {
    uint8_t *p=out;
    if(!form) {memcpy(out,px,(size_t)w*h*2);return (size_t)w*h*2;}
    if(form==1) {
        for(int y=0;y<h;y++) {
            uint8_t *count=p;u16(&p,0);unsigned runs=0;
            for(int x=0;x<w;) {int end=x+1;while(end<w && px[y*w+end]==px[y*w+x])end++;
                u16(&p,end-x);u16(&p,px[y*w+x]);runs++;x=end;}
            count[0]=(uint8_t)runs;count[1]=(uint8_t)(runs>>8);
        }
    } else {
        memset(lookup,255,sizeof(lookup));memset(slots,0,sizeof(slots));unsigned colours=0;
        for(int i=0;i<w*h;i++)if(form==4?!slots[slot(px[i])]:lookup[px[i]]==65535) {
            if(colours==(form==3?16u:256u))return 0;
            if(form==4)slots[slot(px[i])]=0x80000000u|(colours<<16)|px[i];
            lookup[px[i]]=(uint16_t)colours;palette[colours++]=px[i];
        }
        u16(&p,colours);for(unsigned i=0;i<colours;i++)u16(&p,palette[i]);
        if(form==2 || form==4)for(int i=0;i<w*h;i++)*p++=form==4?(uint8_t)(slots[slot(px[i])]>>16):(uint8_t)lookup[px[i]];
        else for(int i=0;i<w*h;i+=2)*p++=(uint8_t)(lookup[px[i]]|(i+1<w*h?lookup[px[i+1]]<<4:0));
    }
    return (size_t)(p-out);
}
int main(int argc,char **argv) {
    assert(argc==2);const int w=690,h=240;size_t n=(size_t)w*h;
    FILE *f=fopen(argv[1],"rb");assert(f);uint16_t *px=malloc(n*2);assert(fread(px,2,n,f)==n);fclose(f);
    uint8_t *out=malloc(n*4+h*2+1024);tdefl_compressor *z=malloc(sizeof(*z));assert(out && z);
    const char *names[]={"RGB565","row-RLE","index8","index4","index8-hash"};int probes[]={1,4,8,32};
    for(int strip=0;strip<2;strip++)for(int form=0;form<5;form++)for(int level=0;level<4;level++) {
        const int width=strip?90:w;
        uint16_t *in=px;
        if(strip) {in=malloc((size_t)width*h*2);for(int y=0;y<h;y++)memcpy(in+y*width,px+y*w+w-width,(size_t)width*2);}
        double prep=0,deflate=0;size_t bytes=0,compressed=0;
        for(int j=0;j<30;j++) {
            double a=now();bytes=encode(in,width,h,form,out);double b=now();if(!bytes)break;
            output_bytes=0;tdefl_init(z,count_output,NULL,probes[level]|TDEFL_GREEDY_PARSING_FLAG);
            assert(tdefl_compress_buffer(z,out,bytes,TDEFL_FINISH)==TDEFL_STATUS_DONE);
            double c=now();prep+=b-a;deflate+=c-b;compressed=output_bytes;
        }
        if(bytes)printf("{\"case\":\"%s\",\"format\":\"%s\",\"probes\":%d,\"raw\":%zu,\"z\":%zu,\"prep_us\":%.2f,\"deflate_us\":%.2f}\n",strip?"90px-strip":"full",names[form],probes[level],bytes,compressed,prep/30,deflate/30);
        if(strip)free(in);
    }
    free(out);free(px);free(z);return 0;
}
