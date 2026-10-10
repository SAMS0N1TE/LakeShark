#include "cartocore/core.h"
#include <string.h>
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
/* Legacy rows remain borrowed. SH1 grids are expanded with scene descriptors;
   two short horizontal rows serve every vertically interpolated output row. */
void cc_terrain_blit(cc_planes *p,const cc_scene *s) {
    p->terrain_active=0;
    memset(p->shade,16,(size_t)p->cols*p->rows);
    if(!p->terrain_strength) return;
    if(!s->terrain) return;
    p->terrain_phase_x=(int)((-s->terrain->ox/2)&3);
    p->terrain_phase_y=(int)((-s->terrain->oy/4)&3);
    for(const cc_terrain *t=s->terrain;t;t=t->next) {
        if(t->zoom_shift) {
            int factor=1<<t->zoom_shift;
            for(int y=0;y<p->rows;y++) for(int x=0;x<p->cols;x++) {
                if(t->clipped && (x*2+1<t->clip[0] || y*4+2<t->clip[1] ||
                       x*2+1>=t->clip[2] || y*4+2>=t->clip[3])) continue;
                int64_t dx=(int64_t)x*2+1-t->ox,dy=(int64_t)y*4+2-t->oy;
                if(dx<0 || dy<0 || dx>=256*factor || dy>=256*factor) continue;
                unsigned q;
                if(t->sx) {
                    int sx=(1<<t->sx)*factor,sy=(1<<t->sy)*factor,w=(256>>t->sx)+1;
                    int ix=(int)(dx/sx),iy=(int)(dy/sy),fx=(int)(dx%sx),fy=(int)(dy%sy);
                    const uint8_t *r=t->runs+iy*w+ix;
                    int64_t a=(int64_t)r[0]*(sx-fx)+(int64_t)r[1]*fx,b=(int64_t)r[w]*(sx-fx)+(int64_t)r[w+1]*fx;
                    q=(unsigned)((a*(sy-fy)+b*fy+(int64_t)sx*sy/2)/((int64_t)sx*sy));
                } else {
                    unsigned ix=(unsigned)(dx/factor)/2,iy=(unsigned)(dy/factor)/4;
                    uint32_t tag=u32(t->runs+iy*4); const uint8_t *r=t->runs+(tag&0x7fffffff);
                    if(tag&0x80000000u) q=(r[ix/2]>>((ix&1)*4))&15;
                    else { const uint8_t *end=t->runs+(u32(t->runs+(iy+1)*4)&0x7fffffff); q=8; for(;r<end;r+=2) if(ix<r[0]) { q=r[1]; break; } }
                }
                p->shade[(size_t)y*p->cols+x]=(uint8_t)q;
            }
            continue;
        }
        int y0=t->oy>2?(t->oy+1)/4:0,y1=(t->oy+253)/4+1;
        int a=t->ox,b=t->ox+256;
        int x0=a/2-(a<0 && a%2),x1=b/2-(b<0 && b%2);
        if(x0<0) x0=0;
        if(x1>p->cols) x1=p->cols;
        if(x0>=x1) continue;
        if(y1>p->rows) y1=p->rows;
        if(t->sx) {
            unsigned w=(256u>>t->sx)+1,maskx=(1u<<t->sx)-1,masky=(1u<<t->sy)-1;
            unsigned bits=t->sx+t->sy,round=1u<<(bits-1);
            int16_t line[2][129]; int last=-2,upper=0;
            for(int y=y0;y<y1;y++) {
                int sy=y*4+2-t->oy; if(sy<0 || sy>=256) continue;
                int iy=sy>>t->sy; unsigned fy=(unsigned)sy&masky;
                if(iy!=last) {
                    int reuse=iy==last+1; if(reuse) upper^=1;
                    for(int row=reuse?1:0;row<2;row++) {
                        const uint8_t *src=t->runs+(iy+row)*w;
                        int16_t *dst=line[upper^row];
                        for(int x=x0;x<x1;x++) {
                            int sx=x*2+1-t->ox; unsigned ix=(unsigned)sx>>t->sx,fx=(unsigned)sx&maskx;
                            dst[x-x0]=(int16_t)((src[ix]<<t->sx)+((int)src[ix+1]-src[ix])*(int)fx);
                        }
                    }
                    last=iy;
                }
                uint8_t *dst=p->shade+(size_t)y*p->cols;
                const int16_t *a=line[upper],*b=line[upper^1];
                for(int x=x0;x<x1;x++) {
                    int k=x-x0;
                    dst[x]=(uint8_t)(((a[k]<<t->sy)+(b[k]-a[k])*(int)fy+(int)round)>>bits);
                }
            }
            continue;
        }
        for(int y=y0;y<y1;y++) {
            int sy=y*4+2-t->oy; if(sy<0 || sy>=256) continue;
            uint32_t tag=u32(t->runs+(sy/4)*4);
            const uint8_t *r=t->runs+(tag&0x7fffffffu);
            uint8_t *dst=p->shade+(size_t)y*p->cols;
            if(tag&0x80000000u) {
                int sx=(x0*2+1-t->ox)/2;
                for(int x=x0;x<x1;x++,sx++) dst[x]=(uint8_t)((r[sx/2]>>((sx&1)*4))&15);
                continue;
            }
            const uint8_t *end=t->runs+(u32(t->runs+(sy/4+1)*4)&0x7fffffffu); int start=0;
            for(;r<end;r+=2) {
                int stop=r[0]*2;
                int ra=t->ox+start,rb=t->ox+stop;
                int lo=ra/2-(ra<0 && ra%2),hi=rb/2-(rb<0 && rb%2);
                if(lo<x0) lo=x0;
                if(hi>x1) hi=x1;
                if(hi>lo) memset(dst+lo,r[1],(size_t)(hi-lo));
                start=stop;
            }
        }
    }
    p->terrain_active=1;
}
/* Tiny offline-like colour lookup built when the style changes. Water,
   buildings and line inks are identical at every shade level. */
void cc_terrain_palette(cc_planes *p) {
    if(p->terrain_palette_strength==p->terrain_strength && p->terrain_palette16==p->colors16 && p->terrain_palette_source==p->palette) return;
    p->terrain_palette_strength=p->terrain_strength; p->terrain_palette16=p->colors16; p->terrain_palette_source=p->palette;
    for(unsigned ink=0;ink<CC_INK_COUNT;ink++) for(int q=0;q<16;q++) {
        uint32_t rgb=p->palette?p->palette[ink]:cc_palette((uint8_t)ink),out=0;
        int land=ink==CC_LAND || ink==CC_WOOD || ink==CC_PARK || ink==CC_FARM || ink==CC_RESIDENTIAL || ink==CC_INDUSTRIAL;
        for(unsigned k=0;k<3;k++) {
            int v=(rgb>>(8*k))&255;
            if(land && !p->colors16) { int delta=(q-8)*p->terrain_strength; v+=(v+44)*delta/800; }
            if(v<0) v=0;
            if(v>255) v=255;
            out|=(uint32_t)v<<(8*k);
        }
        if(p->colors16) {
            unsigned idx=cc_palette16((uint8_t)ink);
            out|=(uint32_t)(0x80|idx)<<24;
        }
        p->terrain_colours[ink][q]=out;
    }
}
