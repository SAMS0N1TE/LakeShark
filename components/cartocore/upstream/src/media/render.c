#include "cartocore/arch.h"
/* Fresh integer area scaler and two-colour cell fitter. */
#include "cartocore/media.h"
#include <string.h>
#include <math.h>
#include "tables.h"
static const uint32_t ansi16[16]={0x000000,0x800000,0x008000,0x808000,0x000080,0x800080,0x008080,0xc0c0c0,0x808080,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff};
static int sq(int x) { return x*x; }
cc_media_layout cc_media_geometry(const cc_media *m,unsigned sw,unsigned sh,unsigned aw,unsigned ah) {
    unsigned w=(unsigned)m->width,h=(unsigned)m->height;
    cc_media_layout r={0,0,sw,sh,0,0,w,h};
    double cell=m->options.cell_aspect?m->options.cell_aspect:0.5;
    /* Dot width/height = cell aspect * vertical dots / horizontal dots. */
    double dot=cell*((double)h/m->options.rows)/((double)w/m->options.cols);
    double target=(double)w*dot/h,source=(double)aw/ah;
    if(m->options.sizing==CC_MEDIA_FIT) {
        if(source<target) r.dw=(unsigned)((double)h*source/dot+0.5);
        else r.dh=(unsigned)((double)w*dot/source+0.5);
        if(!r.dw) r.dw=1;
        if(!r.dh) r.dh=1;
        r.dx=(w-r.dw)/2; r.dy=(h-r.dh)/2;
    } else if(m->options.sizing==CC_MEDIA_FILL) {
        if(source<target) r.sh=(unsigned)(sh*source/target+0.5);
        else r.sw=(unsigned)(sw*target/source+0.5);
        if(!r.sw) r.sw=1;
        if(!r.sh) r.sh=1;
        r.sx=(sw-r.sw)/2; r.sy=(sh-r.sh)/2;
    }
    return r;
}
static uint32_t quantize(uint32_t c,int colors) {
    if(colors==1) return c;
    int r=c>>16,g=(c>>8)&255,b=c&255;
    if(colors==16) {
        unsigned best=~0u,k=0;
        for(unsigned j=0;j<16;j++) {
            uint32_t p=ansi16[j]; unsigned e=sq(r-(int)(p>>16))+sq(g-(int)((p>>8)&255))+sq(b-(int)(p&255));
            if(e<best) { best=e; k=j; }
        }
        return ansi16[k]|((0x80u|k)<<24);
    }
    static const int level[6]={0,95,135,175,215,255};
    int v[3]={r,g,b},q[3],err=0;
    for(int i=0;i<3;i++) {
        int best=100000; q[i]=0;
        for(int j=0;j<6;j++) if(sq(v[i]-level[j])<best) { best=sq(v[i]-level[j]); q[i]=j; }
        err+=best;
    }
    int gray=(r+g+b)/3; gray=gray<8?0:(gray-8+5)/10; if(gray>23) gray=23;
    int z=8+10*gray;
    if(sq(r-z)+sq(g-z)+sq(b-z)<err) return (uint32_t)z*0x010101u;
    return (uint32_t)(level[q[0]]<<16|level[q[1]]<<8|level[q[2]]);
}
int cc_media_init(cc_media *m,cc_arena *a,cc_media_options o) {
    if(!m || !a || o.cols<1 || o.rows<1 || o.cols>4096 || o.rows>4096 || (unsigned)o.mode>CC_ASCII ||
       (o.colors!=1 && o.colors!=0 && o.colors!=16) || (unsigned)o.dither>CC_DITHER_DIFFUSION || o.stability>1000000 ||
       (unsigned)o.sizing>CC_MEDIA_STRETCH || !isfinite(o.cell_aspect) || o.cell_aspect<0) return 0;
    size_t mark=a->used; cc_arch_clear(m,sizeof(*m)); m->options=o;
    m->width=o.cols*(o.mode==CC_HALF?1:2); m->height=o.rows*(o.mode==CC_QUADRANT || o.mode==CC_HALF?2:4);
    m->scaled=cc_arena_array(a,(size_t)m->width*m->height,3,1);
    m->masks=cc_arena_array(a,(size_t)o.cols*o.rows,1,1);
    m->xb=cc_arena_array(a,m->width+1,sizeof(uint32_t),4);
    m->yb=cc_arena_array(a,m->height+1,sizeof(uint32_t),4);
    m->diffusion=cc_arena_array(a,2*(m->width+2),sizeof(int32_t),4);
    if(!m->scaled || !m->masks || !m->xb || !m->yb || !m->diffusion) { a->used=mark; return 0; }
    cc_encoder_init(&m->encoder); return 1;
}
static unsigned luminance(const uint8_t *p) {
    return (54u*cc_linear[p[0]]+183u*cc_linear[p[1]]+19u*cc_linear[p[2]]+128)>>8;
}
static unsigned fit(const uint8_t rgb[8][3],int n,unsigned mask,int colors,cc_cell *c,int measure) {
    static const unsigned reciprocal[9]={0,65536,32768,21846,16384,13108,10923,9363,8192};
    unsigned sums[2][3]={{0}},counts[2]={0}; uint32_t col[2]; unsigned error=0;
    for(int i=0;i<n;i++) { unsigned k=(mask>>i)&1; counts[k]++; for(int j=0;j<3;j++) sums[k][j]+=rgb[i][j]; }
    for(int k=0;k<2;k++) {
        int other=counts[k]?k:1-k; unsigned count=counts[other];
        unsigned r=((sums[other][0]+count/2)*reciprocal[count])>>16;
        unsigned g=((sums[other][1]+count/2)*reciprocal[count])>>16;
        unsigned b=((sums[other][2]+count/2)*reciprocal[count])>>16;
        col[k]=quantize((r<<16)|(g<<8)|b,colors);
    }
    c->bg=col[0]; c->fg=col[1];
    if(!measure) return 0;
    for(int i=0;i<n;i++) { uint32_t p=col[(mask>>i)&1]; error+=sq((int)rgb[i][0]-(int)((p>>16)&255))+sq((int)rgb[i][1]-(int)((p>>8)&255))+sq((int)rgb[i][2]-(int)(p&255)); }
    return error;
}
int cc_media_render(cc_media *m,cc_rgb_frame f,const cc_cell *previous,cc_cell *cells) {
    if(!m || !cells || !f.pixels || !f.width || !f.height || f.width>32768 || f.height>32768 || f.stride<(size_t)f.width*3 || f.stride>SIZE_MAX/f.height) return 0;
    int w=m->width,h=m->height; cc_media_options o=m->options;
    if(o.cols<1 || o.rows<1 || o.cols>4096 || o.rows>4096 || (unsigned)o.mode>CC_ASCII ||
       (o.colors!=0 && o.colors!=1 && o.colors!=16) || (unsigned)o.dither>CC_DITHER_DIFFUSION ||
       w!=o.cols*(o.mode==CC_HALF?1:2) || h!=o.rows*(o.mode==CC_HALF || o.mode==CC_QUADRANT?2:4) ||
       !m->scaled || !m->masks || !m->xb || !m->yb || !m->diffusion || o.stability>1000000 ||
       (unsigned)o.sizing>CC_MEDIA_STRETCH || !isfinite(o.cell_aspect) || o.cell_aspect<0) return 0;
    if(f.pixels!=m->scaled || f.width!=(unsigned)w || f.height!=(unsigned)h || f.stride!=(size_t)w*3) {
    cc_media_layout rect=cc_media_geometry(m,f.width,f.height,f.width,f.height);
    cc_arch_clear(m->scaled,(size_t)w*h*3);
    for(unsigned x=0;x<=rect.dw;x++) m->xb[x]=rect.sx+(uint32_t)((uint64_t)x*rect.sw/rect.dw);
    for(unsigned y=0;y<=rect.dh;y++) m->yb[y]=rect.sy+(uint32_t)((uint64_t)y*rect.sh/rect.dh);
    /* Integer source bins partition the image: every source pixel contributes
       once when shrinking. Enlarging uses nearest replication for empty bins. */
    for(unsigned y=0;y<rect.dh;y++) {
        unsigned y0=m->yb[y],y1=m->yb[y+1]; if(y1==y0) y1=y0+1;
        for(unsigned x=0;x<rect.dw;x++) {
            unsigned x0=m->xb[x],x1=m->xb[x+1]; if(x1==x0) x1=x0+1;
            uint64_t r=0,g=0,b=0; unsigned count=(x1-x0)*(y1-y0);
            if(count<=65535) {
                unsigned sr=0,sg=0,sb=0;
                for(unsigned sy=y0;sy<y1;sy++) {
                    const uint8_t *p=f.pixels+(size_t)sy*f.stride+3*x0;
                    if(x1-x0==4) {
                        sr+=p[0]+p[3]+p[6]+p[9]; sg+=p[1]+p[4]+p[7]+p[10]; sb+=p[2]+p[5]+p[8]+p[11];
                    } else for(unsigned sx=x0;sx<x1;sx++,p+=3) { sr+=p[0]; sg+=p[1]; sb+=p[2]; }
                }
                r=sr; g=sg; b=sb;
            } else for(unsigned sy=y0;sy<y1;sy++) {
                const uint8_t *p=f.pixels+(size_t)sy*f.stride+3*x0;
                for(unsigned sx=x0;sx<x1;sx++,p+=3) { r+=p[0]; g+=p[1]; b+=p[2]; }
            }
            uint8_t *p=m->scaled+3*((size_t)(y+rect.dy)*w+x+rect.dx);
            if(count<=65535) {
                p[0]=(uint8_t)(((unsigned)r+count/2)/count); p[1]=(uint8_t)(((unsigned)g+count/2)/count); p[2]=(uint8_t)(((unsigned)b+count/2)/count);
            } else {
                p[0]=(uint8_t)((r+count/2)/count); p[1]=(uint8_t)((g+count/2)/count); p[2]=(uint8_t)((b+count/2)/count);
            }
        }
    }
    }
    cc_dither d=o.dither;
    if(d==CC_DITHER_AUTO) d=o.mode==CC_BRAILLE || o.mode==CC_ASCII?CC_DITHER_BLUE:CC_DITHER_BAYER;
    if(o.video && d==CC_DITHER_DIFFUSION) d=CC_DITHER_BLUE;
    cc_arch_clear(m->diffusion,(size_t)2*(w+2)*sizeof(int32_t));
    int cw=o.mode==CC_HALF?1:2,ch=o.mode==CC_HALF || o.mode==CC_QUADRANT?2:4,n=cw*ch;
    if(d==CC_DITHER_DIFFUSION) {
        cc_arch_clear(m->masks,(size_t)o.cols*o.rows);
        for(int gy=0;gy<h;gy++) {
            int32_t *row=m->diffusion+(gy&1)*(w+2),*next=m->diffusion+((gy+1)&1)*(w+2);
            cc_arch_clear(next,(size_t)(w+2)*sizeof(*next));
            for(int gx=0;gx<w;gx++) {
                int cx=gx/cw,cy=gy/ch; unsigned lo=4095,hi=0;
                for(int y=0;y<ch;y++) for(int x=0;x<cw;x++) {
                    unsigned l=luminance(m->scaled+3*((size_t)(cy*ch+y)*w+cx*cw+x));
                    if(l<lo) lo=l;
                    if(l>hi) hi=l;
                }
                int value=(int)luminance(m->scaled+3*((size_t)gy*w+gx))+row[gx+1]/16;
                int on=value>(int)(lo+hi)/2,e=value-(on?(int)hi:(int)lo);
                if(on) m->masks[(size_t)cy*o.cols+cx]|=(uint8_t)(1u<<((gy%ch)*cw+gx%cw));
                row[gx+2]+=e*7; next[gx]+=e*3; next[gx+1]+=e*5; next[gx+2]+=e;
            }
        }
    }
    for(int cy=0;cy<o.rows;cy++) for(int cx=0;cx<o.cols;cx++) {
        uint8_t rgb[8][3]; unsigned lum[8],lo=4095,hi=0,mask=0;
        for(int y=0;y<ch;y++) for(int x=0;x<cw;x++) {
            int i=y*cw+x; const uint8_t *p=m->scaled+3*((size_t)(cy*ch+y)*w+cx*cw+x);
            memcpy(rgb[i],p,3); lum[i]=luminance(p); if(lum[i]<lo) lo=lum[i]; if(lum[i]>hi) hi=lum[i];
        }
        int mid=(int)(lo+hi)/2,spread=(int)(hi-lo);
        for(int y=0;spread && d!=CC_DITHER_DIFFUSION && y<ch;y++) for(int x=0;x<cw;x++) {
            int i=y*cw+x,gx=cx*cw+x,gy=cy*ch+y,adjust=0;
            if(d==CC_DITHER_BAYER) adjust=((int)cc_bayer[(gy&7)*8+(gx&7)]*2-63)*spread/512;
            if(d==CC_DITHER_BLUE) adjust=((int)cc_blue[(gy&7)*8+(gx&7)]*2-63)*spread/512;
            int value=(int)lum[i];
            if(value>mid+adjust) mask|=1u<<i;
        }
        if(d==CC_DITHER_DIFFUSION) mask=m->masks[(size_t)cy*o.cols+cx];
        /* Share the map encoder glyph vocabulary, including ASCII silhouettes. */
        unsigned bits=cc_media_bits(o.mode,mask); uint32_t cp=cc_encoder_glyph(&m->encoder,o.mode,bits);
        if(o.mode==CC_ASCII) mask=cc_encoder_mask(o.mode,cp);
        size_t at=(size_t)cy*o.cols+cx;
        cc_cell c={cp,0,0}; unsigned error=fit(rgb,n,mask,o.colors,&c,o.video && previous && previous[at].codepoint!=cp);
        if(o.video && previous && previous[at].codepoint!=cp) {
            unsigned oldmask=cc_encoder_mask(o.mode,previous[at].codepoint);
            cc_cell old={previous[at].codepoint,0,0}; unsigned olderror=fit(rgb,n,oldmask,o.colors,&old,1);
            if(olderror<=error || olderror-error<=o.stability*(unsigned)n) c=old;
        }
        cells[at]=c;
    }
    return 1;
}
