#ifndef LS_BRAILLE_H
#define LS_BRAILLE_H
#include <stdint.h>
static inline int ls_braille_pixel(uint16_t cp,int x,int y,int w,int h) {
    static const uint8_t bit[4][2]={{0,3},{1,4},{2,5},{6,7}};
    int sx=x*2/w,sy=y*4/h;
    int xa=sx*w/2,xb=(sx+1)*w/2,ya=sy*h/4,yb=(sy+1)*h/4;
    /* A small dot centred in each of the eight slots, with air around it. */
    int dw=(xb-xa+1)/2,dh=(yb-ya+1)/2;
    int dx=x-(xa+(xb-xa-dw)/2),dy=y-(ya+(yb-ya-dh)/2);
    return (cp & (1u<<bit[sy][sx])) && dx>=0 && dx<dw && dy>=0 && dy<dh;
}
#endif
