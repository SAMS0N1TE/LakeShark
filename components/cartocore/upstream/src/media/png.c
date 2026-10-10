/* PNG baseline decoder, written here from the chunk/filter definitions. */
#include "cartocore/media.h"
#include "../ingest/ingest.h"
#include <string.h>
#include "stream.h"
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=~0u; while(n--) { c^=*p++; for(int k=0;k<8;k++) c=(c>>1)^(0xedb88320u & (0u-(c&1))); } return ~c;
}
static int paeth(int a,int b,int c) {
    int p=a+b-c,da=p-a,db=p-b,dc=p-c; if(da<0) da=-da; if(db<0) db=-db; if(dc<0) dc=-dc;
    return da<=db && da<=dc?a:db<=dc?b:c;
}
typedef struct { const uint8_t *data; size_t chunk,left; const uint8_t *p; } png_input;
static int png_read(void *v) {
    png_input *in=v;
    while(!in->left) {
        in->chunk+=12+be32(in->data+in->chunk);
        in->left=be32(in->data+in->chunk); in->p=in->data+in->chunk+8;
    }
    in->left--; return *in->p++;
}
typedef struct {
    uint8_t *line,*above,*rgb; size_t row,at; unsigned y,filter,w,h,bpp,type,depth,channels,npal;
    int hastrans; const uint8_t (*palette)[3]; const uint8_t *alpha; const unsigned *transparent;
    cc_grid_sink *sink;
} png_output;
static int png_emit(void *v,unsigned char value) {
    png_output *ctx=v;
    if(ctx->y>=ctx->h) return 0;
    if(!ctx->at) { if(value>4) return 0; ctx->filter=value; ctx->at=1; return 1; }
    size_t at=ctx->at-1; unsigned bpp=ctx->bpp;
    uint8_t *line=ctx->line,*above=ctx->above;
    int left=at>=bpp?line[at-bpp]:0,up=above[at],ul=at>=bpp?above[at-bpp]:0;
    unsigned filter=ctx->filter;
    line[at]=(uint8_t)(value+(filter==1?left:filter==2?up:filter==3?(left+up)/2:filter==4?paeth(left,up,ul):0));
    if(++ctx->at<=ctx->row) return 1;
    unsigned w=ctx->w,y=ctx->y,channels=ctx->channels,depth=ctx->depth,type=ctx->type,npal=ctx->npal;
    int hastrans=ctx->hastrans; const unsigned *transparent=ctx->transparent;
    const uint8_t (*palette)[3]=ctx->palette; const uint8_t *alpha=ctx->alpha;
        for(unsigned x=0;x<w;x++) {
            unsigned samples[4]={0};
            for(unsigned k=0;k<channels;k++) {
                size_t bit=((size_t)x*channels+k)*depth;
                samples[k]=depth==16?(unsigned)line[bit/8]*256+line[bit/8+1]:depth==8?line[bit/8]:(line[bit/8]>>(8-depth-bit%8))&((1u<<depth)-1);
            }
            uint8_t pixel[3]; uint8_t *q=ctx->sink?pixel:ctx->rgb+3*((size_t)y*w+x); unsigned al=255,max=(1u<<depth)-1;
            if(type==3) { if(samples[0]>=npal) return 0; memcpy(q,palette[samples[0]],3); al=alpha[samples[0]]; }
            else if(type==0 || type==4) {
                q[0]=q[1]=q[2]=(uint8_t)((samples[0]*255+max/2)/max);
                if(type==4) al=(samples[1]*255+max/2)/max;
                else if(hastrans && samples[0]==transparent[0]) al=0;
            } else {
                for(unsigned k=0;k<3;k++) q[k]=(uint8_t)((samples[k]*255+max/2)/max);
                if(type==6) al=(samples[3]*255+max/2)/max;
                else if(hastrans && samples[0]==transparent[0] && samples[1]==transparent[1] && samples[2]==transparent[2]) al=0;
            }
            if(al!=255) for(int k=0;k<3;k++) q[k]=(uint8_t)((q[k]*al+127)/255);
            if(ctx->sink) cc_grid_pixel(ctx->sink,x,y,pixel);
        }
    ctx->above=line; ctx->line=above; ctx->at=0; ctx->y++; return 1;
}
int cc_png_run(const uint8_t *data,size_t size,cc_arena *a,cc_rgb_frame *f,cc_grid_sink *sink) {
    static const uint8_t sig[8]={137,80,78,71,13,10,26,10};
    if(!data || !a || !f) return 0;
    if(size<33) { if(sink) sink->error=CC_MEDIA_TRUNCATED; return 0; }
    if(memcmp(data,sig,8)) { if(sink) sink->error=CC_MEDIA_UNSUPPORTED; return 0; }
    size_t mark=a->used,pos=8,zsize=0; unsigned w=0,h=0,type=0,bpp=0,npal=0,depth=0,channels=0; size_t first_idat=0; int header=0,ended=0,idat=0,idat_end=0,hastrans=0;
    uint8_t palette[256][3],alpha[256]; memset(alpha,255,sizeof(alpha)); unsigned transparent[3]={0};

    while(pos<size) {
        if(size-pos<12) { if(sink) sink->error=CC_MEDIA_TRUNCATED; goto bad; }
        uint32_t n=be32(data+pos); const uint8_t *tag=data+pos+4,*p=tag+4;
        if(n>size-pos-12) { if(sink) sink->error=CC_MEDIA_TRUNCATED; goto bad; }
        if(crc(tag,(size_t)n+4)!=be32(p+n)) goto bad;
        if(!header && memcmp(tag,"IHDR",4)) goto bad;
        for(unsigned k=0;k<4;k++) if((tag[k]<'A' || tag[k]>'Z') && (tag[k]<'a' || tag[k]>'z')) goto bad;
        if(tag[2]&32) goto bad;
        if(!memcmp(tag,"IHDR",4)) {
            if(header || n!=13) goto bad;
            w=be32(p); h=be32(p+4); type=p[9];
            depth=p[8];
            if(!w || !h || w>32768 || h>32768 || p[10] || p[11] ) goto bad;
            if(p[12]) { if(sink) sink->error=CC_MEDIA_UNSUPPORTED; goto bad; }
            channels=type==0?1:type==2?3:type==3?1:type==4?2:type==6?4:0;
            if(!channels || (depth!=8 && depth!=16 && depth!=1 && depth!=2 && depth!=4) ||
               (type==3 && depth==16) || (type!=0 && type!=3 && depth<8)) goto bad;
            bpp=(channels*depth+7)/8;
            header=1;
        } else if(!memcmp(tag,"PLTE",4)) {
            if(idat || npal || !n || n%3 || n>768 || type==0 || type==4 || (type==3 && n/3>(1u<<depth))) goto bad;
            npal=n/3; memcpy(palette,p,n);
        } else if(!memcmp(tag,"tRNS",4)) {
            if(idat || hastrans) goto bad;
            hastrans=1;
            if(type==3) { if(!npal || n>npal || !n) goto bad; memcpy(alpha,p,n); }
            else if(type==0 && n==2) { transparent[0]=(unsigned)p[0]*256+p[1]; }
            else if(type==2 && n==6) for(int k=0;k<3;k++) transparent[k]=(unsigned)p[k*2]*256+p[k*2+1];
            else goto bad;
            if(type==0 && transparent[0]>((1u<<depth)-1)) goto bad;
            if(type==2) for(unsigned k=0;k<3;k++) if(transparent[k]>((1u<<depth)-1)) goto bad;
        } else if(!memcmp(tag,"IDAT",4)) {
            if(idat_end || (type==3 && !npal)) goto bad;
            if(!idat) first_idat=pos;
            zsize+=n; idat=1;
        } else if(!memcmp(tag,"IEND",4)) {
            if(n || !idat) goto bad;
            ended=1; pos+=12; break;
        } else {
            if(!(tag[0]&32)) goto bad;
            if(idat) idat_end=1;
        }
        pos+=(size_t)n+12;
    }
    if(!ended) { if(sink) sink->error=CC_MEDIA_TRUNCATED; goto bad; }
    if(pos!=size) goto bad;
    size_t row=((size_t)w*channels*depth+7)/8,written=0;
    if(row+1>SIZE_MAX/h) goto bad;
    uint8_t *lines=cc_arena_array(a,row,2,1),*history=cc_arena_alloc(a,32768,1),*rgb=NULL;
    if(!sink) rgb=cc_arena_array(a,(size_t)w*h,3,1);
    if(!lines || !history || (!sink && !rgb)) { if(sink) sink->error=CC_MEDIA_OOM; goto bad; }
    memset(lines,0,row*2);
    if(sink) {
        if(!cc_grid_begin(sink,w,h,1,1)) goto bad;
        sink->stats.width=w; sink->stats.height=h; sink->stats.orientation=1; sink->stats.scale=1;
    }
    png_input in={data,first_idat,be32(data+first_idat),data+first_idat+8};
    png_output ctx={lines,lines+row,rgb,row,0,0,0,w,h,bpp,type,depth,channels,npal,
        hastrans,palette,alpha,transparent,sink};
    if(!cc_inflate_stream(png_read,&in,zsize,png_emit,&ctx,history,(row+1)*h,&written) ||
       written!=(row+1)*h || ctx.y!=h || ctx.at) goto bad;
    *f=(cc_rgb_frame){rgb,w,h,(size_t)w*3}; return 1;
bad: a->used=mark; return 0;
}
int cc_media_decode(const uint8_t *data,size_t size,cc_arena *a,cc_rgb_frame *f) {
    if(!data || !a || !f) return 0;
    if(size>=2 && data[0]==255 && data[1]==216) return cc_jpeg_decode(data,size,a,f);
    return cc_png_decode(data,size,a,f);
}

int cc_png_decode(const uint8_t *d,size_t n,cc_arena *a,cc_rgb_frame *f) { return cc_png_run(d,n,a,f,NULL); }
