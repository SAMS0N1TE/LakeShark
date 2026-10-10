/* Written here; integer bins exactly match the full-frame area scaler. */
#include "stream.h"
#include <string.h>
int cc_grid_begin(cc_grid_sink *s,unsigned w,unsigned h,unsigned orientation,unsigned scale) {
    s->sw=w; s->sh=h; s->orientation=orientation; s->scale=scale;
    s->ow=orientation>=5?h:w; s->oh=orientation>=5?w:h;
    unsigned aw=s->stats.width?s->stats.width:w,ah=s->stats.height?s->stats.height:h;
    s->layout=cc_media_geometry(s->media,s->ow,s->oh,orientation>=5?ah:aw,orientation>=5?aw:ah);
    size_t n=(size_t)s->media->width*s->media->height;
    s->sum=cc_arena_array(s->arena,n,3*sizeof(uint64_t),8);
    if(!s->sum) { s->error=CC_MEDIA_OOM; return 0; }
    memset(s->sum,0,n*3*sizeof(uint64_t)); return 1;
}
static void range(unsigned x,unsigned src,unsigned dst,unsigned *lo,unsigned *hi) {
    if(dst>=src) {
        *lo=(unsigned)((x*dst+src-1)/src);
        *hi=(unsigned)(((x+1)*dst+src-1)/src);
    } else { *lo=(unsigned)(((x+1)*dst-1)/src); *hi=*lo+1; }
}
void cc_grid_pixel(cc_grid_sink *s,unsigned x,unsigned y,const uint8_t rgb[3]) {
    unsigned u=x,v=y;
    switch(s->orientation) {
    case 2:u=s->sw-1-x;break;
    case 3:u=s->sw-1-x;v=s->sh-1-y;break;
    case 4:v=s->sh-1-y;break;
    case 5:u=y;v=x;break;
    case 6:u=s->sh-1-y;v=x;break;
    case 7:u=s->sh-1-y;v=s->sw-1-x;break;
    case 8:u=y;v=s->sw-1-x;break;
    }
    unsigned x0,x1,y0,y1,w=(unsigned)s->media->width;
    cc_media_layout r=s->layout;
    if(u<r.sx || u>=r.sx+r.sw || v<r.sy || v>=r.sy+r.sh) return;
    range(u-r.sx,r.sw,r.dw,&x0,&x1); range(v-r.sy,r.sh,r.dh,&y0,&y1);
    x0+=r.dx; x1+=r.dx; y0+=r.dy; y1+=r.dy;
    for(unsigned ty=y0;ty<y1;ty++) for(unsigned tx=x0;tx<x1;tx++) {
        uint64_t *p=s->sum+3*((size_t)ty*w+tx);
        for(unsigned k=0;k<3;k++) p[k]+=rgb[k];
    }
}
void cc_grid_finish(cc_grid_sink *s) {
    unsigned w=(unsigned)s->media->width,h=(unsigned)s->media->height;
    cc_media_layout r=s->layout;
    memset(s->media->scaled,0,(size_t)w*h*3);
    for(unsigned y=0;y<r.dh;y++) for(unsigned x=0;x<r.dw;x++) {
        unsigned dx=(unsigned)((uint64_t)(x+1)*r.sw/r.dw)-(unsigned)((uint64_t)x*r.sw/r.dw);
        unsigned dy=(unsigned)((uint64_t)(y+1)*r.sh/r.dh)-(unsigned)((uint64_t)y*r.sh/r.dh);
        uint64_t n=(uint64_t)(dx?dx:1)*(dy?dy:1); size_t at=3*((size_t)(y+r.dy)*w+x+r.dx);
        for(unsigned k=0;k<3;k++) s->media->scaled[at+k]=(uint8_t)((s->sum[at+k]+n/2)/n);
    }
}
cc_media_error cc_media_decode_grid(const uint8_t *data,size_t size,cc_arena *a,cc_media *m,cc_media_stats *stats) {
    if(!data || !a || !m || !m->scaled || m->width<1 || m->width>8192 || m->height<1 || m->height>16384) return CC_MEDIA_MALFORMED;
    size_t mark=a->used; cc_grid_sink s; memset(&s,0,sizeof(s));
    s.arena=a; s.media=m; s.error=CC_MEDIA_MALFORMED; cc_rgb_frame f;
    int ok=size>=2 && data[0]==255 && data[1]==216?
        cc_jpeg_run(data,size,a,&f,&s):cc_png_run(data,size,a,&f,&s);
    if(ok) { cc_grid_finish(&s); s.error=CC_MEDIA_OK; }
    s.stats.arena_bytes=a->used-mark;
    if(stats) *stats=s.stats;
    a->used=mark; return s.error;
}
const char *cc_media_error_string(cc_media_error e) {
    switch(e) {
    case CC_MEDIA_OK:return "OK";
    case CC_MEDIA_TRUNCATED:return "Truncated image";
    case CC_MEDIA_OOM:return "Image decode out of memory";
    case CC_MEDIA_PROGRESSIVE:return "Unsupported progressive JPEG (SOF2); use baseline JPEG";
    case CC_MEDIA_UNSUPPORTED:return "Unsupported image encoding";
    default:return "Malformed image";
    }
}
