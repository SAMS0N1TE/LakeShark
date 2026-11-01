/* Shared 2x4 history columns, extracted from SWEEP's RSSI strips. */
#ifndef LS_BRAILLE_H
#define LS_BRAILLE_H
#include <stdint.h>
#include "tuilib/tui_core.h"
/* Dot-matrix text and picture glyphs are pictures, not line observations. */
static inline void ls_braille_glyph_put(tui_surface *sf,tui_rect clip,int x,int y,
                                      int16_t ch,uint8_t at)
{
    clip=tui_rect_intersect(clip,tui_surface_rect(sf));
    if(tui_rect_contains(clip,x,y)) sf->back[y*sf->w+x]=(tui_cell){ch,at};
}
/* tui_put_char is byte-sized; preserve all eight braille bits in the grid. */
static inline void ls_braille_put(tui_surface *sf,tui_rect clip,int x,int y,
                                  int16_t ch,uint8_t at)
{
    clip=tui_rect_intersect(clip,tui_surface_rect(sf));
    if(tui_rect_contains(clip,x,y)) {
        sf->back[y*sf->w+x]=(tui_cell){ch,at};
        /* Observe the actual column strokes, without interpolating samples
           into a waveform the board never drew. */
        tui_stroke_hook_fn hook=__atomic_load_n(&tui_stroke_hook,__ATOMIC_ACQUIRE);
        if(hook && ch>=0x2800 && ch<=0x28ff) {
            static const uint8_t bit[4][2]={{0,3},{1,4},{2,5},{6,7}};
            for(int half=0;half<2;half++) for(int k=0;k<4;) {
                if(!(ch&(1u<<bit[k][half]))) {k++;continue;}
                int a=k;while(k+1<4 && (ch&(1u<<bit[k+1][half]))) k++;
                if(hook(1,sf,tui_surface_rect(sf),2,4,x*2+half,y*4+a,x*2+half,y*4+k,at))
                    for(int j=a;j<=k;j++) hook(2,sf,tui_surface_rect(sf),2,4,
                        x*2+half,y*4+j,1u<<bit[j][half],0x2800,at);
                k++;
            }
        }
    }
}
static inline int ls_braille_pixel(uint16_t cp,int x,int y,int w,int h) {
    static const uint8_t bit[4][2]={{0,3},{1,4},{2,5},{6,7}};
    int sx=x*2/w,sy=y*4/h;
    int xa=sx*w/2,xb=(sx+1)*w/2,ya=sy*h/4,yb=(sy+1)*h/4;
    /* A small dot centred in each of the eight slots, with air around it. */
    int dw=(xb-xa+1)/2,dh=(yb-ya+1)/2;
    int dx=x-(xa+(xb-xa-dw)/2),dy=y-(ya+(yb-ya-dh)/2);
    return (cp & (1u<<bit[sy][sx])) && dx>=0 && dx<dw && dy>=0 && dy<dh;
}
/* Bottom-up dot index; callers bound level to 0..4. No lookup storage. */
static inline uint8_t ls_braille_dot(unsigned level, int right)
{
    unsigned bit = level == 0 ? (right ? 7 : 6) :
                   (right ? 6 : 3) - level;
    return (uint8_t)(1u << bit);
}
static inline uint8_t ls_braille_column(unsigned height, int right)
{
    uint8_t bits = 0;
    for (unsigned k = 0; k < height && k < 4; ++k)
        bits |= ls_braille_dot(k, right);
    return bits;
}
#endif
