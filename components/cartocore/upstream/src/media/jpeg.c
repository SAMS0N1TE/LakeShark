/* Fresh baseline JPEG decoder: canonical Huffman, fixed point separable IDCT.
   No external source or decoder library used. */
#include "cartocore/media.h"
#include <string.h>
#include "idct_table.h"
#include "stream.h"
static const uint8_t zig[64]={0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
typedef struct { unsigned first[17],count[17],offset[17]; uint8_t value[256]; unsigned valid; } jhuff;
typedef struct { const uint8_t *p,*end; unsigned bits,n; int error; } jbits;
typedef struct { unsigned id,h,v,qt,dc,ac; int predictor; uint8_t pixels[256]; } jcomponent;
static unsigned u16(const uint8_t *p) { return (unsigned)p[0]*256+p[1]; }
static unsigned take(jbits *b,unsigned n) {
    while(b->n<n) {
        if(b->p==b->end) { b->error=1; return 0; }
        unsigned c=*b->p++;
        if(c==255) { if(b->p==b->end || *b->p++!=0) { b->error=1; return 0; } }
        b->bits=(b->bits<<8)|c; b->n+=8;
    }
    b->n-=n; return (b->bits>>b->n)&((1u<<n)-1);
}
static int symbol(jbits *b,const jhuff *t) {
    unsigned code=0;
    for(unsigned n=1;n<=16;n++) {
        code=(code<<1)|take(b,1); if(b->error) return -1;
        if(code>=t->first[n] && code-t->first[n]<t->count[n]) return t->value[t->offset[n]+code-t->first[n]];
    }
    b->error=1; return -1;
}
static int extend(jbits *b,unsigned n) {
    unsigned v=take(b,n); return n && v<(1u<<(n-1))?(int)v-(int)((1u<<n)-1):(int)v;
}
static uint8_t clamp(int x) { return x<0?0:x>255?255:(uint8_t)x; }
static int block(jbits *b,jcomponent *c,const jhuff *dc,const jhuff *ac,const uint16_t *qt,unsigned bx,unsigned by,unsigned scale) {
    unsigned side=8/scale;
    int32_t coef[64]={0},tmp[64]; int n=symbol(b,dc); if(n<0 || n>11) return 0;
    c->predictor+=extend(b,(unsigned)n); if(c->predictor<-2048 || c->predictor>2047) return 0;
    coef[0]=c->predictor*qt[0]; unsigned k=1;
    while(k<64) {
        int rs=symbol(b,ac); if(rs<0) return 0;
        if(!rs) break;
        unsigned run=(unsigned)rs>>4,bits=(unsigned)rs&15;
        if(!bits) { if(run!=15 || k+16>64) return 0; k+=16; continue; }
        if(bits>10 || k+run>=64) return 0;
        k+=run; unsigned at=zig[k++]; coef[at]=extend(b,bits)*qt[at];
    }
    if(b->error) return 0;
    if(k==1 || side==1) {
        uint8_t v=clamp(((coef[0]+4)>>3)+128);
        for(unsigned y=0;y<side;y++) memset(c->pixels+(by*side+y)*(c->h*side)+bx*side,v,side);
        return 1;
    }
    /* Reduced IDCT evaluates only low-frequency coefficients, with the same
       1/4 normalization as the 8x8 transform. Entropy still consumes all ACs. */
    for(unsigned y=0;y<side;y++) for(unsigned x=0;x<side;x++) {
        int64_t sum=0; for(unsigned u=0;u<side;u++) {
            int basis=side==8?cc_idct[x][u]:side==4?cc_idct4[x][u]:cc_idct2[x][u];
            sum+=(int64_t)coef[y*8+u]*basis;
        }
        tmp[y*8+x]=(int32_t)((sum+512)>>10);
    }
    for(unsigned y=0;y<side;y++) for(unsigned x=0;x<side;x++) {
        int64_t sum=0; for(unsigned v=0;v<side;v++) {
            int basis=side==8?cc_idct[y][v]:side==4?cc_idct4[y][v]:cc_idct2[y][v];
            sum+=(int64_t)tmp[v*8+x]*basis;
        }
        c->pixels[(by*side+y)*(c->h*side)+bx*side+x]=clamp((int)((sum+524288)>>20)+128);
    }
    return 1;
}
static unsigned ex16(const uint8_t *p,int le) { return le?(unsigned)p[1]*256+p[0]:u16(p); }
static uint32_t ex32(const uint8_t *p,int le) {
    return le?(uint32_t)ex16(p,1)|((uint32_t)ex16(p+2,1)<<16):((uint32_t)u16(p)<<16)|u16(p+2);
}
static unsigned exif_orientation(const uint8_t *p,size_t n) {
    if(n<14 || memcmp(p,"Exif\0\0",6)) return 1;
    p+=6; n-=6; int le=p[0]=='I' && p[1]=='I';
    if(!le && !(p[0]=='M' && p[1]=='M')) return 1;
    if(ex16(p+2,le)!=42) return 1;
    uint32_t off=ex32(p+4,le); if(off>n-2) return 1;
    unsigned count=ex16(p+off,le); size_t at=(size_t)off+2;
    for(unsigned i=0;i<count && at<=n && n-at>=12;i++,at+=12) {
        const uint8_t *e=p+at;
        if(ex16(e,le)==274 && ex16(e+2,le)==3 && ex32(e+4,le)==1) {
            unsigned v=ex16(e+8,le); return v>=1 && v<=8?v:1;
        }
    }
    return 1;
}
static int jpeg_run(const uint8_t *data,size_t size,cc_arena *arena,cc_rgb_frame *out,cc_grid_sink *sink,unsigned scale) {
    if(!data || !arena || !out || size<4 || data[0]!=255 || data[1]!=216) return 0;
    if(scale!=1 && scale!=2 && scale!=4 && scale!=8) return 0;
    size_t mark=arena->used,pos=2; unsigned w=0,h=0,nc=0,mh=0,mv=0,restart=0,qvalid=0; int sof=0; unsigned orientation=1;
    if(sink && (size<2 || data[size-2]!=255 || data[size-1]!=217)) sink->error=CC_MEDIA_TRUNCATED;
    uint16_t quant[4][64]={{0}}; jhuff dc[4],ac[4]; jcomponent c[3]={{0}};
    memset(dc,0,sizeof(dc)); memset(ac,0,sizeof(ac));
    while(pos<size) {
        if(data[pos++]!=255) goto bad;
        while(pos<size && data[pos]==255) pos++;
        if(pos==size) goto bad;
        unsigned marker=data[pos++]; if(marker==0xd9 || marker==0xd8 || marker==0 || (marker>=0xd0 && marker<=0xd7)) goto bad;
        if(size-pos<2) { if(sink) sink->error=CC_MEDIA_TRUNCATED; goto bad; }
        unsigned len=u16(data+pos); if(len<2) goto bad; if(len>size-pos) { if(sink) sink->error=CC_MEDIA_TRUNCATED; goto bad; }
        const uint8_t *p=data+pos+2,*end=data+pos+len; pos+=len;
        if(marker==0xdb) {
            while(p<end) {
                unsigned spec=*p++,id=spec&15; if(spec>>4 || id>3 || end-p<64) goto bad;
                for(unsigned i=0;i<64;i++) { if(!p[i]) goto bad; quant[id][zig[i]]=p[i]; }
                p+=64; qvalid|=1u<<id;
            }
        } else if(marker==0xc4) {
            while(p<end) {
                unsigned spec=*p++,id=spec&15,kind=spec>>4; if(id>3 || kind>1 || end-p<16) goto bad;
                jhuff *t=kind?ac+id:dc+id; memset(t,0,sizeof(*t)); unsigned code=0,total=0;
                for(unsigned i=1;i<=16;i++) {
                    t->first[i]=code; t->offset[i]=total; t->count[i]=*p++; total+=t->count[i];
                    code+=t->count[i]; if(code>=(1u<<i) || total>256) goto bad; code<<=1;
                }
                if(!total || (size_t)(end-p)<total) goto bad;
                memcpy(t->value,p,total); p+=total; t->valid=1;
            }
        } else if(marker==0xc2) { if(sink) sink->error=CC_MEDIA_PROGRESSIVE; goto bad;
        } else if(marker==0xe1) {
            if(end-p>=6 && !memcmp(p,"Exif\0\0",6)) orientation=exif_orientation(p,(size_t)(end-p));
        } else if(marker==0xc0) {
            if(sof || end-p<6 || *p++!=8) goto bad;
            h=u16(p); w=u16(p+2); nc=p[4]; p+=5;
            if(!w || !h || w>32768 || h>32768 || (nc!=1 && nc!=3) || end-p!=(int)(3*nc)) goto bad;
            for(unsigned i=0;i<nc;i++) {
                c[i].id=*p++; unsigned hv=*p++; c[i].h=hv>>4; c[i].v=hv&15; c[i].qt=*p++;
                if(c[i].qt>3 || !c[i].h || !c[i].v || c[i].h>2 || c[i].v>2) goto bad;
                for(unsigned j=0;j<i;j++) if(c[j].id==c[i].id) goto bad;
                if(c[i].h>mh) mh=c[i].h;
                if(c[i].v>mv) mv=c[i].v;
            }
            if(nc==1) { if(mh!=1 || mv!=1) goto bad; }
            else if(c[1].h!=1 || c[1].v!=1 || c[2].h!=1 || c[2].v!=1 || c[0].h!=mh || c[0].v!=mv || (mh==1 && mv!=1)) goto bad;
            sof=1;
        } else if(marker==0xdd) { if(end-p!=2) goto bad; restart=u16(p); }
        else if(marker==0xda) {
            if(!sof || end-p!=(int)(1+2*nc+3) || *p++!=nc) goto bad;
            unsigned order[3];
            for(unsigned i=0;i<nc;i++) {
                unsigned id=*p++,sel=*p++,j=0; while(j<nc && c[j].id!=id) j++;
                if(j==nc) goto bad;
                for(unsigned k=0;k<i;k++) if(order[k]==j) goto bad;
                order[i]=j; c[j].dc=sel>>4; c[j].ac=sel&15;
                if(c[j].dc>3 || c[j].ac>3 || !dc[c[j].dc].valid || !ac[c[j].ac].valid || !(qvalid&(1u<<c[j].qt))) goto bad;
            }
            if(p[0]!=0 || p[1]!=63 || p[2]!=0) goto bad;
            unsigned sw=(w+scale-1)/scale,sh=(h+scale-1)/scale;
            uint8_t *rgb=NULL;
            if(sink) {
                unsigned ow=orientation>=5?h:w,oh=orientation>=5?w:h;
                while(scale<8 && (ow+scale*2-1)/(scale*2)>=(unsigned)sink->media->width*2 &&
                      (oh+scale*2-1)/(scale*2)>=(unsigned)sink->media->height*2) scale*=2;
                sw=(w+scale-1)/scale; sh=(h+scale-1)/scale;
                sink->stats.width=w; sink->stats.height=h; sink->stats.orientation=orientation; sink->stats.scale=scale;
                if(!cc_grid_begin(sink,sw,sh,orientation,scale)) goto bad;
            } else { rgb=cc_arena_array(arena,(size_t)sw*sh,3,1); if(!rgb) goto bad; }
            unsigned side=8/scale;
            jbits b={data+pos,data+size,0,0,0}; unsigned mcu=0,rst=0;
            for(unsigned my=0;my<h;my+=mv*8) for(unsigned mx=0;mx<w;mx+=mh*8) {
                if(restart && mcu && mcu%restart==0) {
                    /* Entropy padding must consist of one bits. */
                    if(b.n && (b.bits&((1u<<b.n)-1))!=((1u<<b.n)-1)) goto bad;
                    b.n=0; b.bits=0;
                    if(b.end-b.p<2 || *b.p++!=255) goto bad;
                    while(b.p<b.end && *b.p==255) b.p++;
                    if(b.p==b.end || *b.p++!=0xd0+(rst++&7)) goto bad;
                    for(unsigned i=0;i<nc;i++) c[i].predictor=0;
                }
                for(unsigned i=0;i<nc;i++) { jcomponent *v=c+order[i];
                    for(unsigned by=0;by<v->v;by++) for(unsigned bx=0;bx<v->h;bx++)
                        if(!block(&b,v,dc+v->dc,ac+v->ac,quant[v->qt],bx,by,scale)) goto bad;
                }
                for(unsigned y=0;y<mv*side && my/scale+y<sh;y++) for(unsigned x=0;x<mh*side && mx/scale+x<sw;x++) {
                    unsigned yy=c[0].pixels[(y*c[0].v/mv)*(c[0].h*side)+x*c[0].h/mh];
                    uint8_t pixel[3]; uint8_t *dst=sink?pixel:rgb+3*((size_t)(my/scale+y)*sw+mx/scale+x);
                    if(nc==1) dst[0]=dst[1]=dst[2]=(uint8_t)yy;
                    else {
                        int cb=(int)c[1].pixels[(y/mv)*side+x/mh]-128,cr=(int)c[2].pixels[(y/mv)*side+x/mh]-128;
                        dst[0]=clamp((int)yy+((91881*cr+32768)>>16));
                        dst[1]=clamp((int)yy-((22554*cb+46802*cr+32768)>>16));
                        dst[2]=clamp((int)yy+((116130*cb+32768)>>16));
                    }
                    if(sink) cc_grid_pixel(sink,mx/scale+x,my/scale+y,pixel);
                }
                mcu++;
            }
            if(b.error || (b.n && (b.bits&((1u<<b.n)-1))!=((1u<<b.n)-1))) goto bad;
            if(b.end-b.p<2 || *b.p++!=255) goto bad;
            while(b.p<b.end && *b.p==255) b.p++;
            if(b.p==b.end || *b.p++!=0xd9) goto bad;
            *out=(cc_rgb_frame){rgb,sw,sh,(size_t)sw*3}; return 1;
        } else if(marker==0xfe || (marker>=0xe0 && marker<=0xef)) {
            /* Adobe transform 0 and CMYK are outside this YCbCr baseline. */
            if(marker==0xee && end-p>=12 && !memcmp(p,"Adobe",5) && p[11]!=1) goto bad;
        } else goto bad;
    }
bad: if(sink) sink->stats.arena_bytes=arena->used-mark; arena->used=mark; return 0;
}

int cc_jpeg_run(const uint8_t *d,size_t n,cc_arena *a,cc_rgb_frame *f,cc_grid_sink *s) { return jpeg_run(d,n,a,f,s,1); }
int cc_jpeg_decode_scaled(const uint8_t *d,size_t n,cc_arena *a,cc_rgb_frame *f,unsigned scale) { return jpeg_run(d,n,a,f,NULL,scale); }
int cc_jpeg_decode(const uint8_t *d,size_t n,cc_arena *a,cc_rgb_frame *f) { return cc_jpeg_decode_scaled(d,n,a,f,1); }
