/* Real MAP planes/pictures: compare each reuse render with a fresh full render. */
#include "cartocore/render.h"
#include "cartocore/out.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static double ticks(void) { LARGE_INTEGER n,f;QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return (double)n.QuadPart/f.QuadPart; }
#else
#include <time.h>
static double ticks(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
#endif
static unsigned char memory[2][32000000], cache[2][4000000];
static cc_cell cells[2][400*120];
static uint16_t pixels[2][800*480],previous[800*480];
int main(int argc,char **argv) {
    assert(argc>=2); FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);
    void *data=malloc(n);assert(data && fread(data,1,n,f)==n);fclose(f);
    cc_ctile map;assert(cc_ctile_open((cc_str){data,n},&map));
    cc_renderer r[2];cc_arena a[2];cc_decoded_cache dc[2];
    cc_pixel_style style={0};for(int i=0;i<CC_INK_COUNT;i++)style.rgb[i]=cc_palette(i);
    style.river=0x4080a0;style.relief=60;
    cc_pixel_style reference_style=style;
#ifndef MAPFAST_BASELINE
    static uint16_t shades[CC_INK_COUNT][256];
    cc_pixels_rgb565_prepare(&style,shades);style.shades=(const uint16_t (*)[256])shades;
#endif
    const int moves[][2]={{0,0},{90,0},{-90,0},{0,32},{0,-32},{18,12},{-24,-16},{1,3},{2,4},{4,8},{800,480}};
    for(int terrain=0;terrain<=60;terrain+=60) for(unsigned z=13;z<=15;z++) {
        for(int k=0;k<2;k++) {
            cc_arena_init(&a[k],memory[k],sizeof(memory[k]));
            assert(cc_renderer_init(&r[k],&a[k],345,60,cells[k]));
            assert(cc_decoded_cache_init(&dc[k],cache[k],sizeof(cache[k])));
            r[k].decoded_cache=&dc[k];r[k].terrain_strength=terrain;r[k].edges_mode=CC_EDGES_SMOOTH;
        }
        int64_t left=(INT64_C(1262080)<<(z-13))/2,top=(INT64_C(1533952)<<(z-13))/2;
        double reuse=0,full=0;
        for(unsigned j=0;j<sizeof(moves)/sizeof(*moves);j++) {
            left+=moves[j][0];top+=moves[j][1];
            if(j>=8)for(int k=0;k<2;k++)r[k].edges_mode=CC_EDGES_CRISP;
            for(int k=0;k<2;k++) {
                if(k)cc_renderer_invalidate(&r[k]);
                double start=ticks();
                assert(cc_render(&r[k],&map,z,left,top,CC_QUADRANT,0,NULL));
#ifndef MAPFAST_BASELINE
                if(!k && r[k].pixel_reuse)cc_pixels_rgb565_pan(&r[k].planes,&style,pixels[k],previous,r[k].saved,r[k].pixel_dx,r[k].pixel_dy);
                else
#endif
                cc_pixels_rgb565(&r[k].planes,k?&reference_style:&style,pixels[k]);

                double ms=1000.0*(ticks()-start);
                if(!k)memcpy(previous,pixels[k],690*240*2);
                if(j>0 && j<7) {if(k)full+=ms;else reuse+=ms;}
            }
            if(z==14 && terrain==60 && j==1 && getenv("MAPFAST_RGB565")) {FILE *out=fopen(getenv("MAPFAST_RGB565"),"wb");assert(out);fwrite(pixels[0],2,690*240,out);fclose(out);}
            if(memcmp(pixels[0],pixels[1],690*240*2)) {
                size_t bad=0;while(pixels[0][bad]==pixels[1][bad])bad++;
                fprintf(stderr,"mismatch terrain=%d z%u move=%u at %zu,%zu\n",terrain,z,j,bad%690,bad/690);return 1;
            }
        }
        reuse=full=0;double fresh=0;
        left=(INT64_C(1262080)<<(z-13))/2;top=(INT64_C(1533952)<<(z-13))/2;
        for(int sample=0;sample<40;sample++) {
            r[0].edges_mode=CC_EDGES_SMOOTH;
            cc_renderer_invalidate(&r[0]);
            double base_start=ticks();
            assert(cc_render(&r[0],&map,z,left,top,CC_QUADRANT,0,NULL));
            cc_pixels_rgb565(&r[0].planes,&style,previous);fresh+=1000*(ticks()-base_start);
            for(int k=0;k<2;k++) {
                r[k].edges_mode=CC_EDGES_SMOOTH;
                if(k)cc_renderer_invalidate(&r[k]);
                double start=ticks();
                assert(cc_render(&r[k],&map,z,left+90,top,CC_QUADRANT,0,NULL));
#ifndef MAPFAST_BASELINE
                if(!k && r[k].pixel_reuse)cc_pixels_rgb565_pan(&r[k].planes,&style,pixels[k],previous,r[k].saved,r[k].pixel_dx,r[k].pixel_dy);
                else
#endif
                cc_pixels_rgb565(&r[k].planes,k?&reference_style:&style,pixels[k]);
                double ms=1000*(ticks()-start);if(k)full+=ms;else reuse+=ms;
            }
            assert(!memcmp(pixels[0],pixels[1],690*240*2));
        }
        printf("z%u terrain=%d pan mean %.3f ms full %.3f ms fresh-cached %.3f ms identical\n",z,terrain,reuse/40,full/40,fresh/40);
    }

    free(data);return 0;
}
