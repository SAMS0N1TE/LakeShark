/* Shared 2x4 history columns, extracted from SWEEP's RSSI strips. */
#ifndef LS_BRAILLE_H
#define LS_BRAILLE_H
#include <stdint.h>
#include "tuilib/tui_core.h"
/* tui_put_char is byte-sized; preserve all eight braille bits in the grid. */
static inline void ls_braille_put(tui_surface *sf,tui_rect clip,int x,int y,
                                  int16_t ch,uint8_t at)
{
    clip=tui_rect_intersect(clip,tui_surface_rect(sf));
    if(tui_rect_contains(clip,x,y)) sf->back[y*sf->w+x]=(tui_cell){ch,at};
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
